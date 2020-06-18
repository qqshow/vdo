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
 * $Id: //eng/uds-releases/krusty/src/uds/searchList.c#4 $
 */

#include "searchList.h"

#include "errors.h"
#include "logger.h"
#include "memoryAlloc.h"

/**********************************************************************/
int makeSearchList(unsigned int         capacity,
                   struct search_list **listPtr)
{
  if (capacity == 0) {
    return logErrorWithStringError(UDS_INVALID_ARGUMENT,
                                   "search list must have entries");
  }
  if (capacity > UINT8_MAX) {
    return logErrorWithStringError(UDS_INVALID_ARGUMENT,
                                  "search list capacity must fit in 8 bits");
  }

  // We need three temporary entry arrays for purgeSearchList(). Allocate them
  // contiguously with the main array.
  unsigned int bytes
    = (sizeof(struct search_list) + (4 * capacity * sizeof(uint8_t)));
  struct search_list *list;
  int result = allocateCacheAligned(bytes, "search list", &list);
  if (result != UDS_SUCCESS) {
    return result;
  }

  list->capacity       = capacity;
  list->firstDeadEntry = 0;

  // Fill in the indexes of the chapter index cache entries. These will be
  // only ever be permuted as the search list is used.
  uint8_t i;
  for (i = 0; i < capacity; i++) {
    list->entries[i] = i;
  }

  *listPtr = list;
  return UDS_SUCCESS;
}

/**********************************************************************/
void freeSearchList(struct search_list **listPtr)
{
  FREE(*listPtr);
  *listPtr = NULL;
}

/**********************************************************************/
void purgeSearchList(struct search_list                *searchList,
                     const struct cached_chapter_index  chapters[],
                     uint64_t                           oldestVirtualChapter)
{
  if (searchList->firstDeadEntry == 0) {
    // There are no live entries in the list to purge.
    return;
  }

  /*
   * Partition the previously-alive entries in the list into three temporary
   * lists, keeping the current LRU search order within each list. The element
   * array was allocated with enough space for all four lists.
   */
  uint8_t *entries = &searchList->entries[0];
  uint8_t *alive   = &entries[searchList->capacity];
  uint8_t *skipped = &alive[searchList->capacity];
  uint8_t *dead    = &skipped[searchList->capacity];
  unsigned int nextAlive   = 0;
  unsigned int nextSkipped = 0;
  unsigned int nextDead    = 0;

  int i;
  for (i = 0; i < searchList->firstDeadEntry; i++) {
    uint8_t entry = entries[i];
    const struct cached_chapter_index *chapter = &chapters[entry];
    if ((chapter->virtual_chapter < oldestVirtualChapter)
        || (chapter->virtual_chapter == UINT64_MAX)) {
      dead[nextDead++] = entry;
    } else if (chapter->skip_search) {
      skipped[nextSkipped++] = entry;
    } else {
      alive[nextAlive++] = entry;
    }
  }

  // Copy the temporary lists back to the search list so we wind up with
  // [ alive, alive, skippable, new-dead, new-dead, old-dead, old-dead ]
  memcpy(entries, alive, nextAlive);
  entries += nextAlive;

  memcpy(entries, skipped, nextSkipped);
  entries += nextSkipped;

  memcpy(entries, dead, nextDead);
  // The first dead entry is now the start of the copied dead list.
  searchList->firstDeadEntry = (nextAlive + nextSkipped);
}
