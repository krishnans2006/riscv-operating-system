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

    struct lock lock;
};

struct cache_entry{
    int dirty;
    unsigned long long pos;
    void* block;

    struct cache_entry *prev;
    struct cache_entry *next;

    int open;
    struct condition cond;
};

// INTERNAL FUNCTION DECLARATIONS
//

struct cache_entry* find_cache(struct cache* cache, unsigned long long pos);
void remove_entry(struct cache* cache, struct cache_entry * entry);
void insert_head(struct cache* cache, struct cache_entry* entry);
void init_linkedlist(struct cache* cache);

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
    
    lock_init(&c->lock);

    init_linkedlist(c);
    
    *cptr = c;
    
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
    //I think one thread can only hold onto the block at a time. Otherwise you would have problems where one thread could be modifying the block while another block is reading
    if(pos % CACHE_BLKSZ){
        return -EINVAL;
    }

    if(cache == NULL){
        return -EINVAL;
    }

    lock_acquire(&cache->lock);

    struct cache_entry* hit = find_cache(cache, pos);

    int feedback;

    if(hit == NULL){
        //eviction write-back
        

        struct cache_entry* entry = cache->tail;

        while(entry->open == 1){

            if(entry == cache->head){
                lock_release(&cache->lock);
                return -EBUSY;
            }

            entry = entry->prev;
        }

        if(entry->dirty == 1){
            feedback = storage_store(cache->backing, entry->pos, entry->block, CACHE_BLKSZ);
            if (feedback < 0) {
                lock_release(&cache->lock);
                return feedback;
            }
        }

        feedback = storage_fetch(cache->backing, pos, entry->block, CACHE_BLKSZ);
        if (feedback < 0){
            lock_release(&cache->lock);
            return feedback;
        } 

        entry->dirty = 0;
        entry->pos = pos;
        entry->open = 1;

        remove_entry(cache, entry);
        insert_head(cache, entry);

        *pptr = cache->head->block;
    } else {

        while(hit->open == 1){
            condition_wait(&hit->cond);
        }
        *pptr = hit->block;

        //move hit to the front of the linked list
        remove_entry(cache, hit);

        insert_head(cache, hit);

        hit -> open = 1;
        
    }

    lock_release(&cache->lock);

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
    if(cache == NULL || pblk == NULL){
        return ;
    }
    
    struct cache_entry * cur = cache->head;
    while(cur){

        if(cur->block == pblk){
            
            cur->dirty = dirty;
            cur->open = 0;
            condition_broadcast(&cur->cond);
            return;
        }
       cur = cur->next;
       
    }


    //return 0;
}

/**
 * @brief Flushes the cache to the backing device
 * @param cache Pointer to the cache to flush
 * @return 0 on success, error code if error
 */
int cache_flush(struct cache* cache) {
    // FIXME
    if(cache == NULL){
        return -EINVAL;
    }

    lock_acquire(&cache->lock);

    struct cache_entry * cur = cache->head;
    struct cache_entry * prev;

    int feedback;

    while(cur){

        while(cur->open == 1){
            condition_wait(&cur->cond);
        }

        if(cur->dirty == 1){
            feedback = storage_store(cache->backing, cur->pos, cur->block, CACHE_BLKSZ);
            if (feedback < 0){
                lock_release(&cache->lock);
                return feedback;
            } 
        }
        prev = cur;
        cur = cur->next;

        kfree(prev->block);
        kfree(prev);
    }


    lock_release(&cache->lock);

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


void remove_entry(struct cache* cache, struct cache_entry * entry){


    if(entry == cache->head){
        cache->head = entry->next;
        entry->next->prev = NULL;
            
    } else if(entry == cache->tail){
        cache->tail = entry->prev;
        entry->prev->next = NULL;

        
    } else{
        entry->prev->next = entry->next;
        entry->next->prev = entry->prev;
    }
    
}

void insert_head(struct cache* cache, struct cache_entry* entry){


    entry->next = cache->head;
    cache->head = entry;

    entry->next->prev = entry;
    entry->prev = NULL;
}

void init_linkedlist(struct cache* cache){
    struct cache_entry * cur = (struct cache_entry*) kmalloc(sizeof(struct cache_entry));

    cache->head = cur;

    struct cache_entry * next;
    struct cache_entry * prev = NULL;

    for(int i = 1; i < CACHE_NUM_BLOCKS; i++){
        next = (struct cache_entry*) kmalloc(sizeof(struct cache_entry));

        cur->next = next;
        cur->prev = prev;
        cur->dirty = 0;
        cur->pos   = -1;
        cur->block = kmalloc(CACHE_BLKSZ);
        cur->open = 0;
        condition_init(&cur->cond, "cache_block_open");

        prev = cur;
        cur = next;

        
    }

    cur->next = NULL;
    cur->prev = prev;
    cur->dirty = 0;
    cur->pos   = -1;
    cur->block = kmalloc(CACHE_BLKSZ);
    cur->open = 0;
    condition_init(&cur->cond, "cache_block_open");

    cache->tail = cur;


}