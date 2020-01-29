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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vioPool.c#9 $
 */

#include "vioPool.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "constants.h"
#include "vio.h"
#include "types.h"

/**
 * An vio_pool is a collection of preallocated vios.
 **/
struct vio_pool {
  /** The number of objects managed by the pool */
  size_t                 size;
  /** The list of objects which are available */
  RingNode               available;
  /** The queue of requestors waiting for objects from the pool */
  struct wait_queue      waiting;
  /** The number of objects currently in use */
  size_t                 busyCount;
  /** The list of objects which are in use */
  RingNode               busy;
  /** The number of requests when no object was available */
  uint64_t               outageCount;
  /** The ID of the thread on which this pool may be used */
  ThreadID               threadID;
  /** The buffer backing the pool's vios */
  char                  *buffer;
  /** The pool entries */
  struct vio_pool_entry  entries[];
};

/**********************************************************************/
int makeVIOPool(PhysicalLayer    *layer,
                size_t            poolSize,
                ThreadID          threadID,
                VIOConstructor   *vioConstructor,
                void             *context,
                struct vio_pool **poolPtr)
{
  struct vio_pool *pool;
  int result = ALLOCATE_EXTENDED(struct vio_pool, poolSize,
                                 struct vio_pool_entry, __func__, &pool);
  if (result != VDO_SUCCESS) {
    return result;
  }

  pool->threadID = threadID;
  initializeRing(&pool->available);
  initializeRing(&pool->busy);

  result = ALLOCATE(poolSize * VDO_BLOCK_SIZE, char, "VIO pool buffer",
                    &pool->buffer);
  if (result != VDO_SUCCESS) {
    freeVIOPool(&pool);
    return result;
  }

  char *ptr = pool->buffer;
  size_t i;
  for (i = 0; i < poolSize; i++) {
    struct vio_pool_entry *entry = &pool->entries[i];
    entry->buffer                = ptr;
    entry->context               = context;
    result = vioConstructor(layer, entry, ptr, &entry->vio);
    if (result != VDO_SUCCESS) {
      freeVIOPool(&pool);
      return result;
    }

    ptr += VDO_BLOCK_SIZE;
    initializeRing(&entry->node);
    pushRingNode(&pool->available, &entry->node);
    pool->size++;
  }

  *poolPtr = pool;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeVIOPool(struct vio_pool **poolPtr)
{
  if (*poolPtr == NULL) {
    return;
  }

  // Remove all available entries from the object pool.
  struct vio_pool *pool = *poolPtr;
  ASSERT_LOG_ONLY(!hasWaiters(&pool->waiting),
                  "VIO pool must not have any waiters when being freed");
  ASSERT_LOG_ONLY((pool->busyCount == 0),
                  "VIO pool must not have %zu busy entries when being freed",
                  pool->busyCount);
  ASSERT_LOG_ONLY(isRingEmpty(&pool->busy),
                  "VIO pool must not have busy entries when being freed");

  struct vio_pool_entry *entry;
  while ((entry = asVIOPoolEntry(chopRingNode(&pool->available))) != NULL) {
    freeVIO(&entry->vio);
  }

  // Make sure every vio_pool_entry has been removed.
  size_t i;
  for (i = 0; i < pool->size; i++) {
    struct vio_pool_entry *entry = &pool->entries[i];
    ASSERT_LOG_ONLY(isRingEmpty(&entry->node), "VIO Pool entry still in use:"
                    " VIO is in use for physical block %" PRIu64
                    " for operation %u",
                    entry->vio->physical,
                    entry->vio->operation);
  }

  FREE(pool->buffer);
  FREE(pool);
  *poolPtr = NULL;
}

/**********************************************************************/
bool isVIOPoolBusy(struct vio_pool *pool)
{
  return (pool->busyCount != 0);
}

/**********************************************************************/
int acquireVIOFromPool(struct vio_pool *pool, struct waiter *waiter)
{
  ASSERT_LOG_ONLY((pool->threadID == getCallbackThreadID()),
                  "acquire from active vio_pool called from correct thread");

  if (isRingEmpty(&pool->available)) {
    pool->outageCount++;
    return enqueueWaiter(&pool->waiting, waiter);
  }

  pool->busyCount++;
  RingNode *entry = chopRingNode(&pool->available);
  pushRingNode(&pool->busy, entry);
  (*waiter->callback)(waiter, entry);
  return VDO_SUCCESS;
}

/**********************************************************************/
void returnVIOToPool(struct vio_pool *pool, struct vio_pool_entry *entry)
{
  ASSERT_LOG_ONLY((pool->threadID == getCallbackThreadID()),
                  "vio pool entry returned on same thread as it was acquired");
  entry->vio->completion.errorHandler = NULL;
  if (hasWaiters(&pool->waiting)) {
    notifyNextWaiter(&pool->waiting, NULL, entry);
    return;
  }

  pushRingNode(&pool->available, &entry->node);
  --pool->busyCount;
}

/**********************************************************************/
uint64_t getVIOPoolOutageCount(struct vio_pool *pool)
{
  return pool->outageCount;
}
