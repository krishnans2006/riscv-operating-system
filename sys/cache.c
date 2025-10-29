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

    init_linkedlist(c);
    
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
    
    if(pos % CACHE_BLKSZ){
        return -EINVAL;
    }

    struct cache_entry* hit = find_cache(cache, pos);

    
    if(hit == NULL){
        
        storage_fetch(cache->backing, pos, cache->tail->block, CACHE_BLKSZ);
        cache->tail->dirty = 0;
        cache->tail->pos = pos;
        insert_head(cache, cache->tail);
        remove_tail(cache);

        *pptr = cache->tail->block;
    } else {
        *pptr = hit->block;

        //move hit to the front of the linked list
        
        hit->prev->next = hit->next;
        hit->next->prev = hit->prev;
        hit->next = cache->head;

        cache->head->prev = hit;
        cache->head = hit;
        
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
    
    struct cache_entry * cur = cache->head;
    while(cur){

        if(cur->block == pblk){
            
            cur->dirty = dirty;
            return 0;
        }
       cur = cur->next;
    }


    return -EEXIST;
}

/**
 * @brief Flushes the cache to the backing device
 * @param cache Pointer to the cache to flush
 * @return 0 on success, error code if error
 */
int cache_flush(struct cache* cache) {
    // FIXME
    struct cache_entry * cur = cache->head;
    struct cache_entry * prev;
    while(cur){

        if(cur->dirty == 1){
            storage_store(cache->backing, cur->pos, cur->block, CACHE_BLKSZ);
        }
        prev = cur;
        cur = cur->next;

        kfree(prev->block);
        kfree(prev);
    }

    kfree(cache);

    return 0;
}

struct cache_entry* find_cache(struct cache* cache, unsigned long long pos){

    struct cache_entry * cur = cache->head;
    while(cur){

        if(cur->pos == pos){
            return cur;
        }
       cur = cur->next;
    }

    return NULL;
}


void remove_tail(struct cache* cache){

    cache->tail = cache->tail->prev;
    cache->tail->next = NULL;

}

void insert_head(struct cache* cache, struct cache_entry* entry){

    entry->next = cache->head;
    cache->head = entry;

    entry->next->prev = entry;

}

void init_linkedlist(struct cache* cache){
    struct cache_entry * cur = (struct cache_entry*) kmalloc(sizeof(struct cache_entry));

    cache->head = cur;

    struct cache_entry * next;
    struct cache_entry * prev = NULL;

    for(int i = 1; i < CACHE_BLKSZ; i++){
        next = (struct cache_entry*) kmalloc(sizeof(struct cache_entry));

        cur->next = next;
        cur->prev = prev;
        cur->dirty = 0;
        cur->pos   = -1;
        cur->block = kmalloc(CACHE_BLKSZ);


        prev = cur;
        cur = next;
    }

    cur->next = NULL;
    cur->prev = prev;
    cur->dirty = 0;
    cur->pos   = -1;
    cur->block = kmalloc(CACHE_BLKSZ);

    cache->tail = cur;


}