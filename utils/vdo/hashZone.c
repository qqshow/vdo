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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/hashZone.c#24 $
 */

#include "hashZone.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"
#include "permassert.h"

#include "constants.h"
#include "dataVIO.h"
#include "hashLock.h"
#include "hashLockInternals.h"
#include "pointerMap.h"
#include "ringNode.h"
#include "statistics.h"
#include "threadConfig.h"
#include "types.h"
#include "vdoInternal.h"

enum {
	LOCK_POOL_CAPACITY = MAXIMUM_USER_VIOS,
};

struct hash_zone {
	/** Which hash zone this is */
	zone_count_t zone_number;

	/** The thread ID for this zone */
	thread_id_t thread_id;

	/** Mapping from chunkName fields to HashLocks */
	struct pointer_map *hash_lock_map;

	/** Ring containing all unused HashLocks */
	RingNode lock_pool;

	/**
	 * Statistics shared by all hash locks in this zone. Only modified on
	 * the hash zone thread, but queried by other threads.
	 **/
	struct hash_lock_statistics statistics;

	/** Array of all HashLocks */
	struct hash_lock *lock_array;
};

/**
 * Implements PointerKeyComparator.
 **/
static bool compare_keys(const void *this_key, const void *that_key)
{
	// Null keys are not supported.
	return (memcmp(this_key, that_key, sizeof(struct uds_chunk_name)) == 0);
}

/**
 * Implements PointerKeyComparator.
 **/
static uint32_t hash_key(const void *key)
{
	const struct uds_chunk_name *name = key;
	/*
	 * Use a fragment of the chunk name as a hash code. It must not overlap
	 * with fragments used elsewhere to ensure uniform distributions.
	 */
	// XXX pick an offset in the chunk name that isn't used elsewhere
	return get_unaligned_le32(&name->name[4]);
}

/**********************************************************************/
static inline struct hash_lock *as_hash_lock(RingNode *pool_node)
{
	STATIC_ASSERT(offsetof(struct hash_lock, pool_node) == 0);
	return (struct hash_lock *) pool_node;
}

/**********************************************************************/
int make_hash_zone(struct vdo *vdo, zone_count_t zone_number,
		   struct hash_zone **zone_ptr)
{
	struct hash_zone *zone;
	int result = ALLOCATE(1, struct hash_zone, __func__, &zone);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = make_pointer_map(LOCK_MAP_CAPACITY, 0, compare_keys, hash_key,
				  &zone->hash_lock_map);
	if (result != VDO_SUCCESS) {
		free_hash_zone(&zone);
		return result;
	}

	zone->zone_number = zone_number;
	zone->thread_id = get_hash_zone_thread(get_thread_config(vdo),
					       zone_number);
	initializeRing(&zone->lock_pool);

	result = ALLOCATE(LOCK_POOL_CAPACITY, struct hash_lock,
			  "hash_lock array", &zone->lock_array);
	if (result != VDO_SUCCESS) {
		free_hash_zone(&zone);
		return result;
	}

	vio_count_t i;
	for (i = 0; i < LOCK_POOL_CAPACITY; i++) {
		struct hash_lock *lock = &zone->lock_array[i];
		initialize_hash_lock(lock);
		pushRingNode(&zone->lock_pool, &lock->pool_node);
	}

	*zone_ptr = zone;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_hash_zone(struct hash_zone **zone_ptr)
{
	if (*zone_ptr == NULL) {
		return;
	}

	struct hash_zone *zone = *zone_ptr;
	free_pointer_map(&zone->hash_lock_map);
	FREE(zone->lock_array);
	FREE(zone);
	*zone_ptr = NULL;
}

/**********************************************************************/
zone_count_t get_hash_zone_number(const struct hash_zone *zone)
{
	return zone->zone_number;
}

/**********************************************************************/
thread_id_t get_hash_zone_thread_id(const struct hash_zone *zone)
{
	return zone->thread_id;
}

/**********************************************************************/
struct hash_lock_statistics
get_hash_zone_statistics(const struct hash_zone *zone)
{
	const struct hash_lock_statistics *stats = &zone->statistics;
	return (struct hash_lock_statistics) {
		.dedupe_advice_valid =
			READ_ONCE(stats->dedupe_advice_valid),
		.dedupe_advice_stale =
			READ_ONCE(stats->dedupe_advice_stale),
		.concurrent_data_matches =
			READ_ONCE(stats->concurrent_data_matches),
		.concurrent_hash_collisions =
			READ_ONCE(stats->concurrent_hash_collisions),
	};
}

/**
 * Return a hash lock to the zone's pool and null out the reference to it.
 *
 * @param [in]     zone     The zone from which the lock was borrowed
 * @param [in,out] lock_ptr  The last reference to the lock being returned
 **/
static void return_hash_lock_to_pool(struct hash_zone *zone,
				     struct hash_lock **lock_ptr)
{
	struct hash_lock *lock = *lock_ptr;
	*lock_ptr = NULL;

	memset(lock, 0, sizeof(*lock));
	initialize_hash_lock(lock);
	pushRingNode(&zone->lock_pool, &lock->pool_node);
}

/**********************************************************************/
int acquire_hash_lock_from_zone(struct hash_zone *zone,
				const struct uds_chunk_name *hash,
				struct hash_lock *replace_lock,
				struct hash_lock **lock_ptr)
{
	// Borrow and prepare a lock from the pool so we don't have to do two
	// pointer_map accesses in the common case of no lock contention.
	struct hash_lock *new_lock = as_hash_lock(popRingNode(&zone->lock_pool));
	int result = ASSERT(new_lock != NULL,
			    "never need to wait for a free hash lock");
	if (result != VDO_SUCCESS) {
		return result;
	}

	// Fill in the hash of the new lock so we can map it, since we have to
	// use the hash as the map key.
	new_lock->hash = *hash;

	struct hash_lock *lock;
	result = pointer_map_put(zone->hash_lock_map, &new_lock->hash, new_lock,
				 (replace_lock != NULL), (void **) &lock);
	if (result != VDO_SUCCESS) {
		return_hash_lock_to_pool(zone, &new_lock);
		return result;
	}

	if (replace_lock != NULL) {
		// XXX on mismatch put the old lock back and return a severe
		// error
		ASSERT_LOG_ONLY(lock == replace_lock,
				"old lock must have been in the lock map");
		// XXX check earlier and bail out?
		ASSERT_LOG_ONLY(replace_lock->registered,
				"old lock must have been marked registered");
		replace_lock->registered = false;
	}

	if (lock == replace_lock) {
		lock = new_lock;
		lock->registered = true;
	} else {
		// There's already a lock for the hash, so we don't need the
		// borrowed lock.
		return_hash_lock_to_pool(zone, &new_lock);
	}

	*lock_ptr = lock;
	return VDO_SUCCESS;
}

/**********************************************************************/
void return_hash_lock_to_zone(struct hash_zone *zone,
			      struct hash_lock **lock_ptr)
{
	struct hash_lock *lock = *lock_ptr;
	*lock_ptr = NULL;

	if (lock->registered) {
		struct hash_lock *removed =
			pointer_map_remove(zone->hash_lock_map, &lock->hash);
		ASSERT_LOG_ONLY(lock == removed,
				"hash lock being released must have been mapped");
	} else {
		ASSERT_LOG_ONLY(lock != pointer_map_get(zone->hash_lock_map,
							&lock->hash),
				"unregistered hash lock must not be in the lock map");
	}

	ASSERT_LOG_ONLY(!has_waiters(&lock->waiters),
			"hash lock returned to zone must have no waiters");
	ASSERT_LOG_ONLY((lock->duplicate_lock == NULL),
			"hash lock returned to zone must not reference a PBN lock");
	ASSERT_LOG_ONLY((lock->state == HASH_LOCK_DESTROYING),
			"returned hash lock must not be in use with state %s",
			get_hash_lock_state_name(lock->state));
	ASSERT_LOG_ONLY(isRingEmpty(&lock->pool_node),
			"hash lock returned to zone must not be in a pool ring");
	ASSERT_LOG_ONLY(isRingEmpty(&lock->duplicate_ring),
			"hash lock returned to zone must not reference DataVIOs");

	return_hash_lock_to_pool(zone, &lock);
}

/**
 * Dump a compact description of hash_lock to the log if the lock is not on the
 * free list.
 *
 * @param lock  The hash lock to dump
 **/
static void dump_hash_lock(const struct hash_lock *lock)
{
	if (!isRingEmpty(&lock->pool_node)) {
		// This lock is on the free list.
		return;
	}

	// Necessarily cryptic since we can log a lot of these. First three
	// chars of state is unambiguous. 'U' indicates a lock not registered in
	// the map.
	const char *state = get_hash_lock_state_name(lock->state);
	logInfo("  hl %" PRIptr ": %3.3s %c%" PRIu64
		"/%u rc=%u wc=%zu agt=%" PRIptr,
		(const void *) lock, state, (lock->registered ? 'D' : 'U'),
		lock->duplicate.pbn, lock->duplicate.state,
		lock->reference_count, count_waiters(&lock->waiters),
		(void *) lock->agent);
}

/**
 * Increment a statistic counter in a non-atomic yet thread-safe manner.
 *
 * @param stat  The statistic field to increment
 **/
static void increment_stat(uint64_t *stat)
{
	// Must only be mutated on the hash zone thread. Prevents any compiler
	// shenanigans from affecting other threads reading stats.
	WRITE_ONCE(*stat, *stat + 1);
}

/**********************************************************************/
void bump_hash_zone_valid_advice_count(struct hash_zone *zone)
{
	increment_stat(&zone->statistics.dedupe_advice_valid);
}

/**********************************************************************/
void bump_hash_zone_stale_advice_count(struct hash_zone *zone)
{
	increment_stat(&zone->statistics.dedupe_advice_stale);
}

/**********************************************************************/
void bump_hash_zone_data_match_count(struct hash_zone *zone)
{
	increment_stat(&zone->statistics.concurrent_data_matches);
}

/**********************************************************************/
void bump_hash_zone_collision_count(struct hash_zone *zone)
{
	increment_stat(&zone->statistics.concurrent_hash_collisions);
}

/**********************************************************************/
void dump_hash_zone(const struct hash_zone *zone)
{
	if (zone->hash_lock_map == NULL) {
		logInfo("struct hash_zone %u: NULL map", zone->zone_number);
		return;
	}

	logInfo("struct hash_zone %u: mapSize=%zu", zone->zone_number,
		pointer_map_size(zone->hash_lock_map));
	vio_count_t i;
	for (i = 0; i < LOCK_POOL_CAPACITY; i++) {
		dump_hash_lock(&zone->lock_array[i]);
	}
}