/*! @file cache.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌​‌​‌​‍‌​⁠⁠‌
    @brief Block cache for a storage device.
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#ifdef CACHE_TRACE
#define TRACE
#endif

#ifdef CACHE_DEBUG
#define DEBUG
#endif

#include "cache.h"

#include "conf.h"
#include "console.h"
#include "device.h"
#include "devimpl.h"
#include "error.h"
#include "heap.h"
#include "memory.h"
#include "misc.h"
#include "string.h"
#include "thread.h"

// INTERNAL TYPE DEFINITIONS
//

struct cache{
    struct storage * backing;
    

    struct cache_entry * head;
    struct cache_entry * tail;

    struct cache_entry entries[64];
};

struct cache_entry{
    int dirty;
    unsigned long long pos;
    void* block;

    struct cache_entry *prev;
    struct cache_entry *next;

};

/**
 * @brief Creates/initializes a cache with the passed backing storage device (disk) and makes it
 * available through cptr.
 * @param disk Pointer to the backing storage device.
 * @param cptr Pointer to the cache to create.
 * @return 0 on success, negative error code if error
 */
int create_cache(struct storage* disk, struct cache** cptr) {
    // FIXME
    struct cache *c = kmalloc( sizeof(struct cache));

    c->backing = disk;

    //initiate cache data blocks.
    for(int i = 0; i < 64;i++){
        c->entries[i].block = kmalloc(CACHE_BLKSZ);
    }
    
    return 0;
}

/**
 * @brief Reads a CACHE_BLKSZ sized block from the backing interface into the cache.
 * @param cache Pointer to the cache.
 * @param pos Position in the backing storage device. Must be aligned to a multiple of the block
 * size of the backing interface.
 * @param pptr Pointer to the block pointer read from the cache. Assume that CACHE_BLKSZ will always
 * be equal to the block size of the storage disk. Any replacement policy is permitted, as long as
 * your design meets the above specifications.
 * @return 0 on success, negative error code if error
 */
int cache_get_block(struct cache* cache, unsigned long long pos, void** pptr) {
    // FIXME
    struct cache_entry* hit = find_cache(cache, pos);
    if(hit == NULL){
        
        storage_fetch(cache->backing, pos, cache->tail->block, CACHE_BLKSZ);
        insert_head(cache, cache->tail);
        remove_tail(cache);

        *pptr = cache->tail->block;
    } else {
        *pptr = hit->block;
    }

    return 0;
}

/**
 * @brief Releases a block previously obtained from cache_get_block().
 * @param cache Pointer to the cache.
 * @param pblk Pointer to a block that was made available in cache_get_block() (which means that
 * pblk == *pptr for some pptr).
 * @param dirty Indicates whether the block has been modified (1) or not (0). If dirty == 1, the
 * block has been written to. If dirty == 0, the block has not been written to.
 * @return 0 on success, negative error code if error
 */
void cache_release_block(struct cache* cache, void* pblk, int dirty) {
    // FIXME
    int i;

    for(i = 0; i<64; i++){
        if(cache->entries[i].block == pblk){
            break;
        }
        return -1;
    }

    cache->entries[i].dirty = dirty;

    return 0;
}

/**
 * @brief Flushes the cache to the backing device
 * @param cache Pointer to the cache to flush
 * @return 0 on success, error code if error
 */
int cache_flush(struct cache* cache) {
    // FIXME
    for(int i = 0; i<64; i++){

        if(cache->entries[i].dirty == 1){
            storage_store(cache->backing, cache->entries[i].pos, cache->entries[i].block, CACHE_BLKSZ);
        }
       
    }

    return 0;
}

struct cache_entry* find_cache(struct cache* cache, unsigned long long pos){


    return NULL;
}


void remove_tail(struct cache* cache){


}

void insert_head(struct cache* cache, struct cache_entry* entry){

}