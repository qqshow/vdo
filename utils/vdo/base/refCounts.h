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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/refCounts.h#17 $
 */

#ifndef REF_COUNTS_H
#define REF_COUNTS_H

#include "completion.h"
#include "journalPoint.h"
#include "slab.h"
#include "types.h"

/**
 * Create a reference counting object.
 *
 * <p>A reference counting object can keep a reference count for every physical
 * block in the VDO configuration. Since we expect the vast majority of the
 * blocks to have 0 or 1 reference counts, the structure is optimized for that
 * situation.
 *
 * @param [in]  block_count         The number of physical blocks that can be
 *                                  referenced
 * @param [in]  slab                The slab of the ref counts object
 * @param [in]  origin              The layer PBN at which to save ref_counts
 * @param [in]  read_only_notifier  The context for tracking read-only mode
 * @param [out] ref_counts_ptr      The pointer to hold the new ref counts object
 *
 * @return a success or error code
 **/
int make_ref_counts(block_count_t block_count,
		    struct vdo_slab *slab,
		    physical_block_number_t origin,
		    struct read_only_notifier *read_only_notifier,
		    struct ref_counts **ref_counts_ptr)
	__attribute__((warn_unused_result));

/**
 * Free a reference counting object and null out the reference to it.
 *
 * @param ref_counts_ptr  The reference to the reference counting object to free
 **/
void free_ref_counts(struct ref_counts **ref_counts_ptr);

/**
 * Check whether a ref_counts is active.
 *
 * @param ref_counts  The ref_counts to check
 **/
bool are_ref_counts_active(struct ref_counts *ref_counts)
	__attribute__((warn_unused_result));

/**
 * Get the stored count of the number of blocks that are currently free.
 *
 * @param  ref_counts  The ref_counts object
 *
 * @return the number of blocks with a reference count of zero
 **/
block_count_t get_unreferenced_block_count(struct ref_counts *ref_counts)
	__attribute__((warn_unused_result));

/**
 * Determine how many times a reference count can be incremented without
 * overflowing.
 *
 * @param  ref_counts  The ref_counts object
 * @param  pbn         The physical block number
 *
 * @return the number of increments that can be performed
 **/
uint8_t get_available_references(struct ref_counts *ref_counts,
				 physical_block_number_t pbn)
	__attribute__((warn_unused_result));

/**
 * Adjust the reference count of a block.
 *
 * @param [in]  ref_counts           The refcounts object
 * @param [in]  operation            The operation to perform
 * @param [in]  slab_journal_point   The slab journal entry for this adjustment
 * @param [out] free_status_changed  A pointer which will be set to true if the
 *                                   free status of the block changed
 *
 *
 * @return A success or error code, specifically:
 *           VDO_REF_COUNT_INVALID   if a decrement would result in a negative
 *                                   reference count, or an increment in a
 *                                   count greater than MAXIMUM_REFS
 *
 **/
int adjust_reference_count(struct ref_counts *ref_counts,
			   struct reference_operation operation,
			   const struct journal_point *slab_journal_point,
			   bool *free_status_changed)
	__attribute__((warn_unused_result));

/**
 * Adjust the reference count of a block during rebuild.
 *
 * @param ref_counts  The refcounts object
 * @param pbn         The number of the block to adjust
 * @param operation   The operation to perform on the count
 *
 * @return VDO_SUCCESS or an error
 **/
int adjust_reference_count_for_rebuild(struct ref_counts *ref_counts,
				       physical_block_number_t pbn,
				       journal_operation operation)
	__attribute__((warn_unused_result));

/**
 * Replay the reference count adjustment from a slab journal entry into the
 * reference count for a block. The adjustment will be ignored if it was already
 * recorded in the reference count.
 *
 * @param ref_counts   The refcounts object
 * @param entry_point  The slab journal point for the entry
 * @param entry        The slab journal entry being replayed
 *
 * @return VDO_SUCCESS or an error code
 **/
int replay_reference_count_change(struct ref_counts *ref_counts,
				  const struct journal_point *entry_point,
				  struct slab_journal_entry entry)
	__attribute__((warn_unused_result));

/**
 * Check whether two reference counters are equivalent. This method is
 * used for unit testing.
 *
 * @param counter_a The first counter to compare
 * @param counter_b The second counter to compare
 *
 * @return <code>true</code> if the two counters are equivalent
 **/
bool are_equivalent_reference_counters(struct ref_counts *counter_a,
				       struct ref_counts *counter_b)
	__attribute__((warn_unused_result));

/**
 * Find a block with a reference count of zero in the range of physical block
 * numbers tracked by the reference counter. If a free block is found, that
 * block is allocated by marking it as provisionally referenced, and the
 * allocated block number is returned.
 *
 * @param [in]  ref_counts     The reference counters to scan
 * @param [out] allocated_ptr  A pointer to hold the physical block number of
 *                             the block that was found and allocated
 *
 * @return VDO_SUCCESS if a free block was found and allocated;
 *         VDO_NO_SPACE if there are no unreferenced blocks;
 *         otherwise an error code
 **/
int allocate_unreferenced_block(struct ref_counts *ref_counts,
				physical_block_number_t *allocated_ptr)
	__attribute__((warn_unused_result));

/**
 * Provisionally reference a block if it is unreferenced.
 *
 * @param ref_counts  The reference counters
 * @param pbn         The PBN to reference
 * @param lock        The pbn_lock on the block (may be NULL)
 *
 * @return VDO_SUCCESS or an error
 **/
int provisionally_reference_block(struct ref_counts *ref_counts,
				  physical_block_number_t pbn,
				  struct pbn_lock *lock)
	__attribute__((warn_unused_result));

/**
 * Count all unreferenced blocks in a range [start_block, end_block) of
 * physical block numbers.
 *
 * @param ref_counts  The reference counters to scan
 * @param start_pbn   The physical block number at which to start
 *                    scanning (included in the scan)
 * @param end_pbn     The physical block number at which to stop
 *                    scanning (excluded from the scan)
 *
 * @return The number of unreferenced blocks
 **/
block_count_t count_unreferenced_blocks(struct ref_counts *ref_counts,
					physical_block_number_t start_pbn,
					physical_block_number_t end_pbn)
	__attribute__((warn_unused_result));

/**
 * Get the number of blocks required to save a reference counts state covering
 * the specified number of data blocks.
 *
 * @param block_count  The number of physical data blocks that can be referenced
 *
 * @return The number of blocks required to save reference counts with the
 *         given block count
 **/
block_count_t get_saved_reference_count_size(block_count_t block_count)
	__attribute__((warn_unused_result));

/**
 * Request a ref_counts object save several dirty blocks asynchronously. This
 * function currently writes 1 / flush_divisor of the dirty blocks.
 *
 * @param ref_counts       The ref_counts object to notify
 * @param flush_divisor    The inverse fraction of the dirty blocks to write
 **/
void save_several_reference_blocks(struct ref_counts *ref_counts,
				   size_t flush_divisor);

/**
 * Ask a ref_counts object to save all its dirty blocks asynchronously.
 *
 * @param ref_counts     The ref_counts object to notify
 **/
void save_dirty_reference_blocks(struct ref_counts *ref_counts);

/**
 * Mark all reference count blocks as dirty.
 *
 * @param ref_counts  The ref_counts of the reference blocks
 **/
void dirty_all_reference_blocks(struct ref_counts *ref_counts);

/**
 * Drain all reference count I/O. Depending upon the type of drain being
 * performed (as recorded in the ref_count's vdo_slab), the reference blocks
 * may be loaded from disk or dirty reference blocks may be written out.
 *
 * @param ref_counts  The reference counts to drain
 **/
void drain_ref_counts(struct ref_counts *ref_counts);

/**
 * Mark all reference count blocks dirty and cause them to hold locks on slab
 * journal block 1.
 *
 * @param ref_counts  The ref_counts of the reference blocks
 **/
void acquire_dirty_block_locks(struct ref_counts *ref_counts);

/**
 * Dump information about this ref_counts structure.
 *
 * @param ref_counts     The ref_counts to dump
 **/
void dump_ref_counts(const struct ref_counts *ref_counts);

#endif // REF_COUNTS_H
