/*
 * Copyright (c) 2020 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. 
 *
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/user/vdoAudit.c#13 $
 */

#include <err.h>
#include <getopt.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fileUtils.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "syscalls.h"

#include "blockMapInternals.h"
#include "numUtils.h"
#include "recoveryJournal.h"
#include "refCounts.h"
#include "referenceBlock.h"
#include "slabDepotInternals.h"
#include "slabJournalInternals.h"
#include "slabSummaryInternals.h"
#include "types.h"
#include "vdo.h"
#include "vdoInternal.h"
#include "vdoLoad.h"
#include "vdoState.h"

#include "blockMapUtils.h"
#include "vdoVolumeUtils.h"

// Reference counts are one byte, so the error delta range of possible
// (stored - audited) values is [ 0 - 255 .. 255 - 0 ].
enum {
  MIN_ERROR_DELTA = -255,
  MAX_ERROR_DELTA = 255,
};

/**
 * A record to hold the audit information for each slab.
 **/
typedef struct {
  SlabCount            slabNumber;
  PhysicalBlockNumber  slabOrigin;
  /** Reference counts audited from the block map for each slab data block */
  uint8_t             *refCounts;
  /** Number of reference count inconsistencies found in the slab */
  uint32_t             badRefCounts;
  /**
   * Histogram of the reference count differences in the slab, indexed by
   * 255 + (storedReferences - auditedReferences).
   **/
  uint32_t             deltaCounts[MAX_ERROR_DELTA - MIN_ERROR_DELTA + 1];
  /** Offset in the slab of the first block with an error */
  SlabBlockNumber      firstError;
  /** Offset in the slab of the last block with an error */
  SlabBlockNumber      lastError;
} SlabAudit;

static const char usageString[]
  = "[--help] [ [--summary] | [--verbose] ] [--version] filename";

static const char helpString[] =
  "vdoAudit - confirm the reference counts of a VDO device\n"
  "\n"
  "SYNOPSIS\n"
  "  vdoAudit [ [--summary] | [--verbose] ] <filename>\n"
  "\n"
  "DESCRIPTION\n"
  "  vdoAudit adds up the logical block references to all physical\n"
  "  blocks of a VDO device found in <filename>, then compares that sum\n"
  "  to the stored number of logical blocks.  It also confirms all of\n"
  "  the actual reference counts on all physical blocks against the\n"
  "  stored reference counts. Finally, it validates that the slab summary\n"
  "  approximation of the free blocks in each slab is correct.\n"
  "\n"
  "  If --verbose is specified, a line item will be reported for each\n"
  "  inconsistency; otherwise a summary of the problems will be displayed.\n"
  "\n";

static struct option options[] = {
  { "help",    no_argument, NULL, 'h' },
  { "summary", no_argument, NULL, 's' },
  { "verbose", no_argument, NULL, 'v' },
  { "version", no_argument, NULL, 'V' },
  { NULL,      0,           NULL,  0  },
};
static char optionString[] = "hsvV";

// Command-line options
static const char  *filename;
static bool         verbose          = false;

// Values loaded from the volume
static VDO         *vdo              = NULL;
static SlabSummary *summary          = NULL;
static BlockCount   slabDataBlocks   = 0;

/** Total number of mapped entries found in block map leaf pages */
static BlockCount   lbnCount         = 0;

/** Reference counts and audit counters for each slab */
static SlabAudit    slabs[MAX_SLABS] = { { 0, }, };

// Total number of errors of each type found
static uint64_t     badBlockMappings = 0;
static uint64_t     badRefCounts     = 0;
static SlabCount    badSlabs         = 0;
static SlabCount    badSummaryHints  = 0;

/**
 * Explain how this command-line function is used.
 *
 * @param progname           Name of this program
 * @param usageOptionString  Multi-line explanation
 **/
static void usage(const char *progname, const char *usageOptionsString)
{
  fprintf(stderr, "Usage: %s %s\n", progname, usageOptionsString);
  exit(1);
}

/**
 * Display an error count and a description of the count, appending
 * 's' as a plural unless the error count is equal to one.
 **/
static void printErrorCount(uint64_t errorCount, const char *errorName)
{
  printf("%" PRIu64" %s%s\n",
         errorCount, errorName, ((errorCount == 1) ? "" : "s"));
}

/**
 * Display a histogram of the reference count error deltas found in the audit
 * of a slab.
 *
 * @param audit  The audit to display
 **/
static void printSlabErrorHistogram(const SlabAudit *audit)
{
  if (audit->badRefCounts == 0) {
    return;
  }

  // 50 histogram bar dots, so each dot represents 2% of the errors in a slab.
  static const char *HISTOGRAM_BAR
    = "**************************************************";
  unsigned int scale = strlen(HISTOGRAM_BAR);

  printf("  error     delta   histogram\n");
  printf("  delta     count   (%u%% of errors in slab per dot)\n",
         100 / scale);

  for (int delta = MIN_ERROR_DELTA; delta <= MAX_ERROR_DELTA; delta++) {
    uint32_t count = audit->deltaCounts[delta - MIN_ERROR_DELTA];
    if (count == 0) {
      continue;
    }
    // Round up any fraction of a dot to a full dot.
    int width = computeBucketCount(scale * (uint64_t) count,
                                   audit->badRefCounts);
    printf("  %5d  %8u   %.*s\n", delta, count, width, HISTOGRAM_BAR);
  }

  printf("\n");
}

/**
 * Display a summary of the problems found in the audit of a slab.
 *
 * @param audit  The audit to display
 **/
static void printSlabErrorSummary(const SlabAudit *audit)
{
  if (audit->badRefCounts == 0) {
    return;
  }

  printf("slab %u at PBN %" PRIu64 " had ",
         audit->slabNumber, audit->slabOrigin);


  if (audit->badRefCounts == 1) {
    printf("1 reference count error in SBN %u", audit->lastError);
  } else {
    printf("%u reference count errors in SBN range [%u .. %u]",
           audit->badRefCounts, audit->firstError, audit->lastError);
  }
  printf("\n");
}

/**
 * Display a summary of all the problems found in the audit.
 **/
static void printErrorSummary(void)
{
  printf("audit summary for VDO volume '%s':\n", filename);
  printErrorCount(badBlockMappings, "block mapping error");
  printErrorCount(badSummaryHints, "free space hint error");
  printErrorCount(badRefCounts, "reference count error");
  printErrorCount(badSlabs, "error-containing slab");

  for (SlabCount i = 0; i < vdo->depot->slabCount; i++) {
    printSlabErrorSummary(&slabs[i]);
    printSlabErrorHistogram(&slabs[i]);
  }
}

/**
 * Release any and all allocated memory.
 **/
static void freeAuditAllocations(void)
{
  freeSlabSummary(&summary);
  for (SlabCount i = 0; i < vdo->depot->slabCount; i++) {
    FREE(slabs[i].refCounts);
  }
  freeVDOFromFile(&vdo);
}

/**
 * Get the filename and any option settings from the input arguments and place
 * them in the corresponding global variables. Print command usage if
 * arguments are wrong.
 *
 * @param argc  Number of input arguments
 * @param argv  Array of input arguments
 *
 * @return VDO_SUCCESS or some error.
 **/
static int processAuditArgs(int argc, char *argv[])
{
  int   c;
  while ((c = getopt_long(argc, argv, optionString, options, NULL)) != -1) {
    switch (c) {
    case 'h':
      printf("%s", helpString);
      exit(0);
      break;

    case 's':
      verbose = false;
      break;

    case 'v':
      verbose = true;
      break;

    case 'V':
      printf("%s version is: %s\n", argv[0], CURRENT_VERSION);
      exit(0);
      break;

    default:
      usage(argv[0], usageString);
      break;

    }
  }

  // Explain usage and exit
  if (optind != (argc - 1)) {
    usage(argv[0], usageString);
  }

  filename = argv[optind];

  return VDO_SUCCESS;
}

/**
 * Read from the layer.
 *
 * @param startBlock  The block number at which to start reading
 * @param blockCount  The number of blocks to read
 * @param buffer      The buffer to read into
 *
 * @return VDO_SUCCESS or an error
 **/
static int readFromLayer(PhysicalBlockNumber  startBlock,
                         BlockCount           blockCount,
                         char                *buffer)
{
  return vdo->layer->reader(vdo->layer, startBlock, blockCount, buffer, NULL);
}

/**
 * Get the offset in the slab for a given PBN.
 *
 * @param [in]  pbn                 The pbn in question.
 * @param [out] slabBlockNumberPtr  A poiter to the offset.
 *
 * @return VDO_SUCCESS, or VDO_OUT_OF_RANGE if this is a slab metadata block
 **/
static int getSlabBlockNumberForPBN(PhysicalBlockNumber  pbn,
                                    SlabBlockNumber     *slabBlockNumberPtr)
{
  SlabDepot *depot = vdo->depot;
  uint64_t slabOffsetMask = (1ULL << depot->slabSizeShift) - 1;
  uint64_t slabBlockNumber = ((pbn - depot->firstBlock) & slabOffsetMask);
  if (slabBlockNumber >= slabDataBlocks) {
    return VDO_OUT_OF_RANGE;
  }

  *slabBlockNumberPtr = slabBlockNumber;
  return VDO_SUCCESS;
}

/**
 * Report a problem with a block map entry.
 **/
static void reportBlockMapEntry(const char          *message,
                                BlockMapSlot         slot,
                                Height               height,
                                PhysicalBlockNumber  pbn,
                                BlockMappingState    state)
{
  badBlockMappings++;
  if (!verbose) {
    return;
  }

  if (isCompressed(state)) {
    warnx("Mapping at (page %" PRIu64 ", slot %u) (height %u)"
          " %s (PBN %" PRIu64 ", state %u)\n",
          slot.pbn, slot.slot, height, message, pbn, state);
  } else {
    warnx("Mapping at (page %" PRIu64 ", slot %u) (height %u)"
          " %s (PBN %" PRIu64 ")\n",
          slot.pbn, slot.slot, height, message, pbn);
  }
}

/**
 * Record the given reference in a block map page.
 *
 * Implements MappingExaminer.
 **/
static int examineBlockMapEntry(BlockMapSlot        slot,
                                Height              height,
                                PhysicalBlockNumber pbn,
                                BlockMappingState   state)
{
  if (state == MAPPING_STATE_UNMAPPED) {
    if (pbn != ZERO_BLOCK) {
      reportBlockMapEntry("is unmapped but has a physical block",
                          slot, height, pbn, state);
      return VDO_BAD_MAPPING;
    }
    return VDO_SUCCESS;
  }

  if (isCompressed(state) && (pbn == ZERO_BLOCK)) {
    reportBlockMapEntry("is compressed but has no physical block",
                        slot, height, pbn, state);
    return VDO_BAD_MAPPING;
  }

  if (height == 0) {
    lbnCount++;
    if (pbn == ZERO_BLOCK) {
      return VDO_SUCCESS;
    }
  }

  SlabCount slabNumber = 0;
  int result = getSlabNumber(vdo->depot, pbn, &slabNumber);
  if (result != VDO_SUCCESS) {
    reportBlockMapEntry("refers to out-of-range physical block",
                        slot, height, pbn, state);
    return result;
  }

  SlabBlockNumber offset = 0;
  result = getSlabBlockNumberForPBN(pbn, &offset);
  if (result != VDO_SUCCESS) {
    reportBlockMapEntry("refers to slab metadata block",
                        slot, height, pbn, state);
    return result;
  }

  SlabAudit *audit = &slabs[slabNumber];
  if (height > 0) {
    // If this interior tree block has already been referenced, warn.
    if ((audit->refCounts[offset]) != 0) {
      reportBlockMapEntry("refers to previously referenced tree page",
                          slot, height, pbn, state);
    }

    // If this interior tree block appears to be compressed, warn.
    if (isCompressed(state)) {
      reportBlockMapEntry("refers to compressed fragment",
                          slot, height, pbn, state);
    }

    audit->refCounts[offset] = PROVISIONAL_REFERENCE_COUNT;
  } else {
    // If incrementing the reference count goes above the maximum, warn.
    if ((audit->refCounts[offset] == PROVISIONAL_REFERENCE_COUNT)
        || (++audit->refCounts[offset] > MAXIMUM_REFERENCE_COUNT) ) {
      reportBlockMapEntry("overflows reference count",
                          slot, height, pbn, state);
    }
  }

  return VDO_SUCCESS;
}

/**
 * Report a problem with the reference count of a block in a slab.
 *
 * @param audit              The audit record for the slab
 * @param sbn                The offset of the block within the slab
 * @param treePage           <code>true</code> if the block appears to be a
 *                           block map tree page
 * @param pristine           <code>true</code> if the slab has never been used
 * @param auditedReferences  The number of references to the block found
 *                           by examining the block map tree
 * @param storedReferences   The reference count recorded in the slab (or
 *                           zero for a pristine slab)
 **/
static void reportRefCount(SlabAudit       *audit,
                           SlabBlockNumber  sbn,
                           bool             treePage,
                           bool             pristine,
                           ReferenceCount   auditedReferences,
                           ReferenceCount   storedReferences)
{
  int errorDelta = storedReferences - (int) auditedReferences;
  badRefCounts++;
  if (audit->badRefCounts == 0) {
    badSlabs++;
  }

  audit->badRefCounts++;
  audit->deltaCounts[errorDelta - MIN_ERROR_DELTA]++;
  audit->firstError = minBlock(audit->firstError, sbn);
  audit->lastError  = maxBlock(audit->lastError, sbn);

  if (!verbose) {
    return;
  }

  warnx("Reference mismatch for%s pbn %" PRIu64 "\n"
        "Block map had %u but%s slab %u had %u\n",
        (treePage ? " tree page" : ""),
        audit->slabOrigin + sbn,
        auditedReferences,
        (pristine ? " (uninitialized)" : ""),
        audit->slabNumber,
        storedReferences);
}

/**
 * Verify all reference count entries in a given PackedReferenceSector against
 * observed reference counts. Any mismatches will generate a warning message.
 *
 * @param audit           The audit record for the slab
 * @param sector          PackedReferenceSector to check
 * @param entries         Number of counts in this sector
 * @param startingOffset  The starting offset within the slab
 *
 * @return The allocated count for this sector
 **/
static BlockCount verifyRefCountSector(SlabAudit             *audit,
                                       PackedReferenceSector *sector,
                                       BlockCount             entries,
                                       SlabBlockNumber        startingOffset)
{
  BlockCount allocatedCount = 0;
  for (BlockCount i = 0; i < entries; i++) {
    SlabBlockNumber sbn                = startingOffset + i;
    ReferenceCount  observedReferences = audit->refCounts[sbn];
    ReferenceCount  storedReferences   = sector->counts[i];

    // If the observed reference is provisional, it is a block map tree page,
    // and there are two valid reference count values.
    if (observedReferences == PROVISIONAL_REFERENCE_COUNT) {
      if ((storedReferences == 1)
          || (storedReferences == MAXIMUM_REFERENCE_COUNT)) {
        allocatedCount++;
        continue;
      }

      reportRefCount(audit, sbn, true, false,
                     observedReferences, storedReferences);
      continue;
    }

    if (observedReferences != storedReferences) {
      // Mismatch, but maybe the refcount is provisional and the proper
      // count is 0.
      if ((observedReferences == EMPTY_REFERENCE_COUNT)
          && (storedReferences == PROVISIONAL_REFERENCE_COUNT)) {
        continue;
      }

      reportRefCount(audit, sbn, false, false,
                     observedReferences, storedReferences);
    }
    if (storedReferences > 0) {
      allocatedCount++;
    }
  }

  return allocatedCount;
}

/**
 * Verify all reference count entries in a given PackedReferenceBlock
 * against observed reference counts.
 * Any mismatches will generate a warning message.
 *
 * @param audit           The audit record for the slab
 * @param block           PackedReferenceBlock to check
 * @param blockEntries    Number of counts in this block
 * @param startingOffset  The starting offset within the slab
 *
 * @return The allocated count for this block
 **/
static BlockCount verifyRefCountBlock(SlabAudit            *audit,
                                      PackedReferenceBlock *block,
                                      BlockCount            blockEntries,
                                      SlabBlockNumber       startingOffset)
{
  BlockCount allocatedCount = 0;
  BlockCount entries        = blockEntries;
  for (SectorCount i = 0; (i < SECTORS_PER_BLOCK) && (entries > 0); i++) {
    BlockCount sectorEntries = minBlock(entries, COUNTS_PER_SECTOR);
    allocatedCount += verifyRefCountSector(audit, &block->sectors[i],
                                           sectorEntries, startingOffset);
    startingOffset += sectorEntries;
    entries        -= sectorEntries;
  }

  return allocatedCount;
}

/**
 * Verify that the number of free blocks in the slab matches the summary's
 * approximate value.
 *
 * @param slabNumber  The number of the slab to check
 * @param freeBlocks  The actual number of free blocks in the slab
 **/
static void verifySummaryHint(SlabCount slabNumber, BlockCount freeBlocks)
{
  BlockCount freeBlockHint
    = getSummarizedFreeBlockCount(getSummaryForZone(summary, 0), slabNumber);
  BlockCount hintError = (1ULL << summary->hintShift);
  if ((freeBlocks < maxBlock(freeBlockHint, hintError) - hintError)
      || (freeBlocks >= (freeBlockHint + hintError))) {
    badSummaryHints++;
    if (verbose) {
      warnx("Slab summary reports roughly %" PRIu64 " free blocks in\n"
            "slab %u, instead of %" PRIu64 " blocks",
            freeBlockHint, slabNumber, freeBlocks);
    }
  }
}

/**
 * Verify that the reference counts for a given slab are consistent with the
 * block map.
 *
 * @param slabNumber  The number of the slab to verify
 * @param buffer      A buffer to hold the reference counts for the slab
 **/
static int verifySlab(SlabCount slabNumber, char *buffer)
{
  SlabAudit *audit = &slabs[slabNumber];

  if (!mustLoadRefCounts(summary->zones[0], slabNumber)) {
    // Confirm that all reference counts for this pristine slab are 0.
    for (SlabBlockNumber sbn = 0; sbn < slabDataBlocks; sbn++) {
      if (audit->refCounts[sbn] != 0) {
        reportRefCount(audit, sbn, false, true, audit->refCounts[sbn], 0);
      }
    }

    // Verify that the slab summary contains the expected free block count.
    verifySummaryHint(slabNumber, slabDataBlocks);
    return VDO_SUCCESS;
  }

  // Get the refCounts stored on this used slab.
  int result = readFromLayer(audit->slabOrigin + slabDataBlocks,
                             getSlabConfig(vdo->depot)->referenceCountBlocks,
                             buffer);
  if (result != VDO_SUCCESS) {
    warnx("Could not read reference count buffer for slab number %u\n",
          slabNumber);
    return result;
  }

  char            *currentBlockStart = buffer;
  BlockCount       freeBlocks        = 0;
  SlabBlockNumber  currentOffset     = 0;
  BlockCount       remainingEntries  = slabDataBlocks;
  while (remainingEntries > 0) {
    PackedReferenceBlock *block = (PackedReferenceBlock *) currentBlockStart;
    BlockCount blockEntries = minBlock(COUNTS_PER_BLOCK, remainingEntries);
    BlockCount allocatedCount
      = verifyRefCountBlock(audit, block, blockEntries, currentOffset);
    freeBlocks        += (blockEntries - allocatedCount);
    remainingEntries  -= blockEntries;
    currentBlockStart += VDO_BLOCK_SIZE;
    currentOffset     += blockEntries;
  }

  // Verify that the slab summary contains the expected free block count.
  verifySummaryHint(slabNumber, freeBlocks);
  return VDO_SUCCESS;
}

/**
 * Check that the reference counts are consistent with the block map. Warn for
 * any physical block whose reference counts are inconsistent.
 *
 * @return VDO_SUCCESS or some error.
 **/
static int verifyPBNRefCounts(void)
{
  const SlabDepot  *depot          = vdo->depot;
  const SlabConfig *slabConfig     = getSlabConfig(depot);
  BlockCount        refCountBlocks = slabConfig->referenceCountBlocks;
  size_t            refCountBytes  = refCountBlocks * VDO_BLOCK_SIZE;

  char *buffer;
  int result = vdo->layer->allocateIOBuffer(vdo->layer, refCountBytes,
                                            "slab reference counts", &buffer);
  if (result != VDO_SUCCESS) {
    warnx("Could not allocate %zu bytes for slab reference counts",
          refCountBytes);
    return result;
  }

  for (SlabCount slabNumber = 0; slabNumber < depot->slabCount; slabNumber++) {
    result = verifySlab(slabNumber, buffer);
    if (result != VDO_SUCCESS) {
      break;
    }
  }

  FREE(buffer);
  return result;
}

/**
 * Audit a VDO by checking that its block map and reference counts are
 * consistent.
 *
 * @return <code>true</code> if the volume was fully consistent
 **/
static bool auditVDO(void)
{
  if (vdo->loadState == VDO_NEW) {
    warnx("The VDO volume is newly formatted and has no auditable state");
    return false;
  }

  if (vdo->loadState != VDO_CLEAN) {
    warnx("WARNING: The VDO was not cleanly shut down (it has state '%s')",
          getVDOStateName(vdo->loadState));
  }

  // Get logical block count and populate observed slab reference counts.
  int result = examineBlockMapEntries(vdo, examineBlockMapEntry);
  if (result != VDO_SUCCESS) {
    return false;
  }

  // Load the slab summary data.
  result = loadSlabSummarySync(vdo, &summary);
  if (result != VDO_SUCCESS) {
    return false;
  }

  // Audit stored versus counted mapped logical blocks.
  BlockCount savedLBNCount = getJournalLogicalBlocksUsed(vdo->recoveryJournal);
  if (lbnCount == savedLBNCount) {
    warnx("Logical block count matched at %" PRIu64, savedLBNCount);
  } else {
    warnx("Logical block count mismatch! Expected %" PRIu64 ", got %" PRIu64,
          savedLBNCount, lbnCount);
  }

  // Now confirm the stored references of all physical blocks.
  result = verifyPBNRefCounts();
  if (result != VDO_SUCCESS) {
    return false;
  }

  return ((lbnCount == savedLBNCount)
          && (badRefCounts == 0)
          && (badSummaryHints == 0));
}

/**********************************************************************/
int main(int argc, char *argv[])
{
  static char errBuf[ERRBUF_SIZE];

  int result = registerStatusCodes();
  if (result != VDO_SUCCESS) {
    errx(1, "Could not register status codes: %s",
         stringError(result, errBuf, ERRBUF_SIZE));
  }

  result = processAuditArgs(argc, argv);
  if (result != VDO_SUCCESS) {
    exit(1);
  }

  openLogger();

  result = makeVDOFromFile(filename, true, &vdo);
  if (result != VDO_SUCCESS) {
    errx(1, "Could not load VDO from '%s': %s",
         filename, stringError(result, errBuf, ERRBUF_SIZE));
  }

  SlabDepot *depot = vdo->depot;
  PhysicalBlockNumber slabOrigin = depot->firstBlock;
  const SlabConfig *slabConfig = getSlabConfig(depot);
  slabDataBlocks = slabConfig->dataBlocks;
  depot->slabCount = calculateSlabCount(depot);

  for (SlabCount i = 0; i < depot->slabCount; i++) {
    SlabAudit *audit = &slabs[i];
    audit->slabNumber = i;
    audit->slabOrigin = slabOrigin;
    slabOrigin       += slabConfig->slabBlocks;
    // So firstError = min(firstError, x) will always do the right thing.
    audit->firstError = (SlabBlockNumber) -1;

    result = ALLOCATE(slabDataBlocks, uint8_t, __func__, &audit->refCounts);
    if (result != VDO_SUCCESS) {
      freeAuditAllocations();
      errx(1, "Could not allocate %" PRIu64 " reference counts: %s",
           slabDataBlocks, stringError(result, errBuf, ERRBUF_SIZE));
    }
  }

  bool passed = auditVDO();
  if (passed) {
    warnx("All pbn references matched.\n");
  } else if (!verbose) {
    printErrorSummary();
  }

  freeAuditAllocations();
  exit(passed ? 0 : 1);
}
