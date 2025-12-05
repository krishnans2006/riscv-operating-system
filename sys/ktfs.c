/*! @file ktfs.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌​‌​‌​‍‌​⁠⁠‌
    @brief KTFS Implementation.
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#ifdef KTFS_TRACE
#define TRACE
#endif

#ifdef KTFS_DEBUG
#define DEBUG
#endif

#include "ktfs.h"

#include "cache.h"
#include "console.h"
#include "device.h"
#include "devimpl.h"
#include "error.h"
#include "filesys.h"
#include "fsimpl.h"
#include "heap.h"
#include "misc.h"
#include "string.h"
#include "thread.h"
#include "uio.h"
#include "uioimpl.h"

// INTERNAL TYPE DEFINITIONS
//

struct opened_files {
    struct opened_file* head;
    struct opened_file* tail;
};

struct opened_file {
    struct ktfs_file* file;
    struct opened_file* next;
};

struct ktfs_fs {
    struct filesystem base;  // castable from struct filesystem*
    struct cache* cache;  // stores all other info (storage, cache, etc.)
    struct opened_files opened_files;  // list of opened files

    uint32_t K;
    uint32_t B;
    uint32_t N;
};

// Wrapper for ktfs_superblock to fill a full block
struct ktfs_superblock_block {
    union {
        struct ktfs_superblock fields;
        uint8_t raw[KTFS_BLKSZ];
    };
};

// Wrapper for ktfs_data_block (for void* casting)
struct ktfs_data_block_block {
    struct ktfs_data_block data;
};

// Wrapper for ktfs_bitmap (for void* casting)
struct ktfs_bitmap_block {
    struct ktfs_bitmap bitmap;
};

struct ktfs_inode_block {
    struct ktfs_inode inodes[KTFS_BLKSZ / KTFS_INOSZ];
};

struct ktfs_indirect_block {
    uint32_t pointers[KTFS_BLKSZ / sizeof(uint32_t)];
};

struct ktfs_directory {
    struct ktfs_dir_entry entries[KTFS_BLKSZ / KTFS_DENSZ];
};

/// @brief File struct for a file in the Keegan Teal Filesystem
struct ktfs_file {
    struct uio base;  // uio struct as base, to make this castable from struct uio*
    struct ktfs_dir_entry dir_entry;  // dentry
    unsigned long size;  // size of the file in bytes
    unsigned long pos;  // current position in file for read/write operations
    struct ktfs_fs* fs;  // pointer to the file system this file belongs to
};

// INTERNAL FUNCTION DECLARATIONS
//

int ktfs_open(struct filesystem* fs, const char* name, struct uio** uioptr);
void ktfs_close(struct uio* uio);
int ktfs_cntl(struct uio* uio, int cmd, void* arg);
long ktfs_fetch(struct uio* uio, void* buf, unsigned long len);
long ktfs_store(struct uio* uio, const void* buf, unsigned long len);
int ktfs_create(struct filesystem* fs, const char* name);
int ktfs_delete(struct filesystem* fs, const char* name);
void ktfs_flush(struct filesystem* fs);

void ktfs_listing_close(struct uio* uio);
long ktfs_listing_read(struct uio *uio, void *buf, unsigned long bufsz);

static int get_root_inode(struct ktfs_fs* ktfs, struct ktfs_inode** root_inode_ptr, int* root_inode_num_ptr, struct ktfs_inode_block** block_to_release);
static int get_inode_from_dentry(struct ktfs_fs* ktfs, struct ktfs_dir_entry* dentry, struct ktfs_inode* inode_copy);
static int ktfs_open_listing(struct ktfs_fs *ktfs, struct uio **uioptr);
static int ktfs_open_file(struct ktfs_fs* ktfs, const char* name, struct uio** uioptr);
static int ktfs_get_nth_dentry(struct ktfs_fs* ktfs, struct ktfs_inode* dir_inode, int n, struct ktfs_dir_entry** dentry_ptr, struct ktfs_directory** block_to_release);
static int ktfs_get_data_block(struct ktfs_fs* ktfs, struct ktfs_inode* inode, int block_index, struct ktfs_data_block_block** data_block_ptr);
static int ktfs_read_data(struct ktfs_fs* ktfs, struct ktfs_inode* inode, unsigned long start_byte, unsigned long end_byte, void* buf);
static int ktfs_write_data(struct ktfs_fs* ktfs, struct ktfs_inode* inode, unsigned long start_byte, unsigned long end_byte, const void* buf);
static int ktfs_add_dentry(struct ktfs_fs* ktfs, struct ktfs_inode* dir_inode, uint16_t inode_num, const char* name);
static int ktfs_claim_inode(struct ktfs_fs* ktfs, uint16_t* inode_num_ptr);
static int ktfs_release_inode(struct ktfs_fs* ktfs, uint16_t inode_num);
static int ktfs_claim_data_block(struct ktfs_fs* ktfs, uint32_t* block_num_ptr);
static int ktfs_release_data_block(struct ktfs_fs* ktfs, uint32_t block_num);
static int ktfs_expand_file(struct ktfs_fs* ktfs, struct ktfs_inode* inode, unsigned long new_size);
static int ktfs_shrink_file(struct ktfs_fs* ktfs, struct ktfs_inode* inode, unsigned long new_size);
static int ktfs_does_file_exist(struct ktfs_fs* ktfs, const char* name, int* exists_ptr);

static struct ktfs_file* get_opened_file(struct opened_files* ofiles, const char* name);
static int add_opened_file(struct opened_files* ofiles, struct ktfs_file* file);
static void remove_opened_file(struct opened_files* ofiles, struct ktfs_file* file);

static int cache_get_block_by_index(struct cache* cache, unsigned long index, void** pptr);

// INTERNAL GLOBAL VARIABLES
//

static const struct uio_intf ktfs_file_uio_intf = {
    .close = &ktfs_close,
    .read = &ktfs_fetch,
    .write = &ktfs_store,
    .cntl = &ktfs_cntl,
};

static const struct uio_intf ktfs_listing_uio_intf = {
    .close = &ktfs_listing_close,
    .read = &ktfs_listing_read,
    .write = NULL,
    .cntl = NULL,
};

// HELPER FUNCTIONS
//

static int get_root_inode(struct ktfs_fs* ktfs, struct ktfs_inode** root_inode_ptr, int* root_inode_num_ptr, struct ktfs_inode_block** block_to_release) {
    // Read superblock
    struct ktfs_superblock_block* superblock;
    int result = cache_get_block_by_index(ktfs->cache, 0, (void **)&superblock);
    if (result != 0) {
        return result;
    }

    // Find the root inode
    uint16_t root_inode_num = superblock->fields.root_directory_inode;

    uint16_t inodes_start_block = 1 + superblock->fields.inode_bitmap_block_count + superblock->fields.bitmap_block_count;
    uint16_t inode_block_offset = (root_inode_num * KTFS_INOSZ) / KTFS_BLKSZ;
    uint16_t inode_offset_within_block = root_inode_num % (KTFS_BLKSZ / KTFS_INOSZ);

    cache_release_block(ktfs->cache, (void*)superblock, 0);

    struct ktfs_inode_block* inode_block;
    result = cache_get_block_by_index(ktfs->cache, inodes_start_block + inode_block_offset, (void**)&inode_block);
    if (result != 0) {
        return result;
    }
    
    *root_inode_ptr = &inode_block->inodes[inode_offset_within_block];
    *root_inode_num_ptr = root_inode_num;
    *block_to_release = inode_block;

    return 0;
}

static int get_inode_from_dentry(struct ktfs_fs* ktfs, struct ktfs_dir_entry* dentry, struct ktfs_inode* inode_copy) {
    // Read superblock
    struct ktfs_superblock_block* superblock;
    int result = cache_get_block_by_index(ktfs->cache, 0, (void **)&superblock);
    if (result != 0) {
        return result;
    }

    uint16_t inode_num = dentry->inode;

    uint16_t inodes_start_block = 1 + superblock->fields.inode_bitmap_block_count + superblock->fields.bitmap_block_count;
    uint16_t inode_block_offset = (inode_num * KTFS_INOSZ) / KTFS_BLKSZ;
    uint16_t inode_offset_within_block = inode_num % (KTFS_BLKSZ / KTFS_INOSZ);

    cache_release_block(ktfs->cache, (void*)superblock, 0);

    struct ktfs_inode_block* inode_block;
    result = cache_get_block_by_index(ktfs->cache, inodes_start_block + inode_block_offset, (void**)&inode_block);
    if (result != 0) {
        return result;
    }
    
    *inode_copy = inode_block->inodes[inode_offset_within_block];

    cache_release_block(ktfs->cache, (void*)inode_block, 0);

    return 0;
}

static int ktfs_open_listing(struct ktfs_fs* ktfs, struct uio** uioptr) {
    // Get root inode
    struct ktfs_inode* root_inode;
    int root_inode_num;
    struct ktfs_inode_block* inode_block;
    int result = get_root_inode(ktfs, &root_inode, &root_inode_num, &inode_block);
    if (result != 0) {
        return result;
    }

    // Convert to listing uio
    struct ktfs_file* listing = kcalloc(1, sizeof(struct ktfs_file));
    if (listing == NULL) {
        cache_release_block(ktfs->cache, (void*)inode_block, 0);
        return -ENOMEM;
    }

    listing->base.intf = &ktfs_listing_uio_intf;
    listing->dir_entry.inode = root_inode_num;
    strncpy(listing->dir_entry.name, "\\", KTFS_MAX_FILENAME_LEN);
    listing->size = root_inode->size;
    listing->pos = 0;
    listing->fs = ktfs;

    cache_release_block(ktfs->cache, (void*)inode_block, 0);

    *uioptr = (struct uio*)listing;

    return 0;
}

static int ktfs_open_file(struct ktfs_fs* ktfs, const char* name, struct uio** uioptr) {
    // Is the file already opened?
    struct ktfs_file* opened_file = get_opened_file(&ktfs->opened_files, name);
    if (opened_file != NULL) {
        // File is busy
        return -EBUSY;
    }

    // Find the file

    // Get root inode
    struct ktfs_inode* root_inode_orig;
    int root_inode_num;
    struct ktfs_inode_block* inode_block;
    int result =
        get_root_inode(ktfs, &root_inode_orig, &root_inode_num, &inode_block);
    if (result != 0) {
        return result;
    }

    // Copy root inode so we can release the block
    struct ktfs_inode root_inode = *root_inode_orig;
    cache_release_block(ktfs->cache, (void*)inode_block, 0);

    // Go through the root directory to find the file
    uint32_t num_dentries = root_inode.size / KTFS_DENSZ;  // Must be exactly divisible

    struct ktfs_dir_entry* dentry;
    struct ktfs_directory* dir_block;
    
    for (uint32_t i = 0; i < num_dentries; i++) {
        // Get nth dentry
        result = ktfs_get_nth_dentry(ktfs, &root_inode, i, &dentry, &dir_block);
        if (result != 0) {
            return result;
        }
        // Check if this is the file we want
        if (strcmp(dentry->name, name) == 0) {
            // Found the file, get its inode (for file size, etc.)
            struct ktfs_inode file_inode;
            result = get_inode_from_dentry(ktfs, dentry, &file_inode);
            if (result != 0) {
                cache_release_block(ktfs->cache, (void*)dir_block, 0);
                return result;
            }

            // Convert to file uio, add to opened files list, and return
            struct ktfs_file* file = kcalloc(1, sizeof(struct ktfs_file));
            if (file == NULL) {
                cache_release_block(ktfs->cache, (void*)dir_block, 0);
                return -ENOMEM;
            }

            file->base.intf = &ktfs_file_uio_intf;
            file->dir_entry = *dentry;
            strncpy(file->dir_entry.name, name, KTFS_MAX_FILENAME_LEN);
            file->size = file_inode.size;
            file->pos = 0;
            file->fs = ktfs;

            cache_release_block(ktfs->cache, (void*)dir_block, 0);

            result = add_opened_file(&ktfs->opened_files, file);
            if (result != 0) {
                kfree(file);
                return result;
            }

            *uioptr = (struct uio*)file;

            return 0;
        }

        cache_release_block(ktfs->cache, (void*)dir_block, 0);
    }

    // File not found
    return -ENOENT;
}

static int ktfs_get_nth_dentry(struct ktfs_fs* ktfs, struct ktfs_inode* dir_inode, int n, struct ktfs_dir_entry** dentry_ptr, struct ktfs_directory** block_to_release) {
    // This function needs to deal with direct, indirect, and doubly-indirect blocks
    // This is difficult! So, we split it into three cases

    int num_dentries_per_block = KTFS_BLKSZ / KTFS_DENSZ;
    int num_indirections_per_indirect_block = KTFS_BLKSZ / sizeof(uint32_t);

    struct ktfs_directory* dir_block;

    if (n < KTFS_NUM_DIRECT_DATA_BLOCKS * num_dentries_per_block) {
        // Direct block case
        int block_index = n / num_dentries_per_block;
        int entry_index = n % num_dentries_per_block;

        int result = cache_get_block_by_index(ktfs->cache, dir_inode->block[block_index] + 1 + ktfs->K + ktfs->B + ktfs->N, (void**)&dir_block);
        if (result != 0) {
            return result;
        }

        *dentry_ptr = &dir_block->entries[entry_index];
        *block_to_release = dir_block;
    } else if (n < (KTFS_NUM_DIRECT_DATA_BLOCKS + KTFS_NUM_INDIRECT_BLOCKS * num_indirections_per_indirect_block) * num_dentries_per_block) {
        // Indirect block case
        // Note: We only have one indirect block, but we still use KTFS_NUM_INDIRECT_BLOCKS at times to make it more readable

        int new_n = n - KTFS_NUM_DIRECT_DATA_BLOCKS * num_dentries_per_block;  // Adjusted for direct blocks
        // int indirect_block_index = new_n / (num_indirections_per_indirect_block * num_dentries_per_block);
        int within_indirect_index = (new_n / num_dentries_per_block) % num_indirections_per_indirect_block;
        int entry_index = new_n % num_dentries_per_block;

        struct ktfs_indirect_block* indirect_block;
        int result = cache_get_block_by_index(ktfs->cache, dir_inode->indirect + 1 + ktfs->K + ktfs->B + ktfs->N, (void**)&indirect_block);
        if (result != 0) {
            return result;
        }
        uint32_t data_block_num = indirect_block->pointers[within_indirect_index];

        cache_release_block(ktfs->cache, (void*)indirect_block, 0);

        result = cache_get_block_by_index(ktfs->cache, data_block_num + 1 + ktfs->K + ktfs->B + ktfs->N, (void**)&dir_block);
        if (result != 0) {
            return result;
        }

        *dentry_ptr = &dir_block->entries[entry_index];
        *block_to_release = dir_block;
    } else {
        // Doubly-indirect block case
        int new_n = n - (KTFS_NUM_DIRECT_DATA_BLOCKS + KTFS_NUM_INDIRECT_BLOCKS * num_indirections_per_indirect_block) * num_dentries_per_block;
        int dindirect_block_index = new_n / (num_indirections_per_indirect_block * num_indirections_per_indirect_block * num_dentries_per_block);
        int within_dindirect_index = (new_n / (num_indirections_per_indirect_block * num_dentries_per_block)) % num_indirections_per_indirect_block;
        int within_indirect_index = (new_n / num_dentries_per_block) % num_indirections_per_indirect_block;
        int entry_index = new_n % num_dentries_per_block;

        struct ktfs_indirect_block* dindirect_block;
        int result = cache_get_block_by_index(ktfs->cache, dir_inode->dindirect[dindirect_block_index] + 1 + ktfs->K + ktfs->B + ktfs->N, (void**)&dindirect_block);
        if (result != 0) {
            return result;
        }
        uint32_t indirect_block_num = dindirect_block->pointers[within_dindirect_index];

        cache_release_block(ktfs->cache, (void*)dindirect_block, 0);

        struct ktfs_indirect_block* indirect_block;
        result = cache_get_block_by_index(ktfs->cache, indirect_block_num + 1 + ktfs->K + ktfs->B + ktfs->N, (void**)&indirect_block);
        if (result != 0) {
            return result;
        }
        uint32_t data_block_num = indirect_block->pointers[within_indirect_index];

        cache_release_block(ktfs->cache, (void*)indirect_block, 0);

        result = cache_get_block_by_index(ktfs->cache, data_block_num + 1 + ktfs->K + ktfs->B + ktfs->N, (void**)&dir_block);
        if (result != 0) {
            return result;
        }

        *dentry_ptr = &dir_block->entries[entry_index];
        *block_to_release = dir_block;
    }

    return 0;
}

static int ktfs_get_data_block(struct ktfs_fs* ktfs, struct ktfs_inode* inode, int block_index, struct ktfs_data_block_block** data_block_ptr) {
    // This function gets the data block at block_index (0-based) for the given inode
    // It handles direct, indirect, and doubly-indirect blocks
    // NOTE: The block must be released by the caller using cache_release_block
    
    struct ktfs_data_block_block* data_block;

    int num_indirections_per_indirect_block = KTFS_BLKSZ / sizeof(uint32_t);

    if (block_index < KTFS_NUM_DIRECT_DATA_BLOCKS) {
        // Direct block case
        uint32_t data_block_num = inode->block[block_index];
        int result = cache_get_block_by_index(ktfs->cache, data_block_num + 1 + ktfs->K + ktfs->B + ktfs->N, (void**)&data_block);
        if (result != 0) {
            return result;
        }
    } else if (block_index < KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_NUM_INDIRECT_BLOCKS * num_indirections_per_indirect_block)) {
        // Indirect block case
        int within_indirect_index = (block_index - KTFS_NUM_DIRECT_DATA_BLOCKS) % num_indirections_per_indirect_block;
        
        struct ktfs_indirect_block* indirect_block;
        int result = cache_get_block_by_index(ktfs->cache, inode->indirect + 1 + ktfs->K + ktfs->B + ktfs->N, (void**)&indirect_block);
        if (result != 0) {
            return result;
        }
        uint32_t data_block_num = indirect_block->pointers[within_indirect_index];

        cache_release_block(ktfs->cache, (void*)indirect_block, 0);

        result = cache_get_block_by_index(ktfs->cache, data_block_num + 1 + ktfs->K + ktfs->B + ktfs->N, (void**)&data_block);
        if (result != 0) {
            return result;
        }
    } else {
        // Doubly-indirect block case
        int new_i = block_index - (KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_NUM_INDIRECT_BLOCKS * num_indirections_per_indirect_block));
        int dindirect_block_index = new_i / (num_indirections_per_indirect_block * num_indirections_per_indirect_block);
        int within_dindirect_index = (new_i / num_indirections_per_indirect_block) % num_indirections_per_indirect_block;
        int within_indirect_index = new_i % num_indirections_per_indirect_block;

        struct ktfs_indirect_block* dindirect_block;
        int result = cache_get_block_by_index(ktfs->cache, inode->dindirect[dindirect_block_index] + 1 + ktfs->K + ktfs->B + ktfs->N, (void**)&dindirect_block);
        if (result != 0) {
            return result;
        }
        uint32_t indirect_block_num = dindirect_block->pointers[within_dindirect_index];

        cache_release_block(ktfs->cache, (void*)dindirect_block, 0);

        struct ktfs_indirect_block* indirect_block;
        result = cache_get_block_by_index(ktfs->cache, indirect_block_num + 1 + ktfs->K + ktfs->B + ktfs->N, (void**)&indirect_block);
        if (result != 0) {
            return result;
        }
        uint32_t data_block_num = indirect_block->pointers[within_indirect_index];

        cache_release_block(ktfs->cache, (void*)indirect_block, 0);

        result = cache_get_block_by_index(ktfs->cache, data_block_num + 1 + ktfs->K + ktfs->B + ktfs->N, (void**)&data_block);
        if (result != 0) {
            return result;
        }
    }

    *data_block_ptr = data_block;
    return 0;
}

static int ktfs_read_data(struct ktfs_fs* ktfs, struct ktfs_inode* inode, unsigned long start_byte, unsigned long end_byte, void* buf) {
    // This function needs to deal with direct, indirect, and doubly-indirect blocks
    // This is difficult! So, we split it into three cases (as with ktfs_get_nth_dentry)

    // Note: start_byte and end_byte are relative to the start of the file
    int start_block_index = start_byte / KTFS_BLKSZ;
    int end_block_index = end_byte / KTFS_BLKSZ;
    // Also, we need offsets within the blocks
    int start_block_offset_bytes = start_byte % KTFS_BLKSZ;
    int end_block_offset_bytes = end_byte % KTFS_BLKSZ;

    // Now lets find all the blocks between start_block_index and
    // end_block_index (inclusive) If the block is equal to either the start or
    // end block, we need to take care of offsets

    int num_indirections_per_indirect_block = KTFS_BLKSZ / sizeof(uint32_t);
    int num_total_blocks = KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_NUM_INDIRECT_BLOCKS * num_indirections_per_indirect_block) + (KTFS_NUM_DINDIRECT_BLOCKS * num_indirections_per_indirect_block * num_indirections_per_indirect_block);

    unsigned long bytes_copied = 0;

    for (int i = start_block_index; i <= end_block_index && i < num_total_blocks; i++) {
        // Get to the actual data block if it's indirect or doubly-indirect
        // All three if/else cases below will fill this variable
        struct ktfs_data_block_block* data_block;

        ktfs_get_data_block(ktfs, inode, i, &data_block);

        // Now, data_block has the data we want, so we copy the right parts of it to buf
        // Note: both offsets are inclusive
        if (i == start_block_index && i == end_block_index) {
            // Both start and end block
            memcpy(buf, &data_block->data.data[start_block_offset_bytes], end_block_offset_bytes - start_block_offset_bytes + 1);
            cache_release_block(ktfs->cache, (void*)data_block, 0);
            return 0;
        } else if (i == start_block_index) {
            // Start block only
            unsigned long bytes_to_copy = KTFS_BLKSZ - start_block_offset_bytes;
            memcpy(buf, &data_block->data.data[start_block_offset_bytes], bytes_to_copy);
            bytes_copied += bytes_to_copy;
        } else if (i == end_block_index) {
            // End block only
            memcpy(&((uint8_t*)buf)[bytes_copied], &data_block->data.data[0], end_block_offset_bytes + 1);
            cache_release_block(ktfs->cache, (void*)data_block, 0);
            return 0;
        } else {
            // Middle block
            memcpy(&((uint8_t*)buf)[bytes_copied], &data_block->data.data[0], KTFS_BLKSZ);
            bytes_copied += KTFS_BLKSZ;
        }

        cache_release_block(ktfs->cache, (void*)data_block, 0);
    }

    // If we haven't returned yet, something went wrong
    // We never reached the end block (?)
    return -EBADFD;
}

static int ktfs_write_data(struct ktfs_fs* ktfs, struct ktfs_inode* inode, unsigned long start_byte, unsigned long end_byte, const void* buf) {
    // As with ktfs_read_data, we need to deal with direct, indirect, and doubly-indirect blocks
    // This function also assumes that the file is already big enough to hold the data being
    // written (i.e., up to end_byte) using something like FCNTL_SETEND. This should be handled
    // in ktfs_create/ktfs_store.

    int start_block_index = start_byte / KTFS_BLKSZ;
    int end_block_index = end_byte / KTFS_BLKSZ;

    int start_block_offset_bytes = start_byte % KTFS_BLKSZ;
    int end_block_offset_bytes = end_byte % KTFS_BLKSZ;

    int num_indirections_per_indirect_block = KTFS_BLKSZ / sizeof(uint32_t);
    int num_total_blocks = KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_NUM_INDIRECT_BLOCKS * num_indirections_per_indirect_block) + (KTFS_NUM_DINDIRECT_BLOCKS * num_indirections_per_indirect_block * num_indirections_per_indirect_block);

    unsigned long bytes_to_write = end_byte - start_byte + 1;
    unsigned long bytes_written = 0;

    for (int i = start_block_index; i <= end_block_index && i < num_total_blocks; i++) {
        struct ktfs_data_block_block* data_block;

        ktfs_get_data_block(ktfs, inode, i, &data_block);

        // Now, data_block is the block we want to write to, but only to certain parts of it
        if (i == start_block_index && i == end_block_index) {
            // Both start and end block
            memcpy(&data_block->data.data[start_block_offset_bytes], buf, bytes_to_write);
            cache_release_block(ktfs->cache, (void*)data_block, 1);
            return 0;
        } else if (i == start_block_index) {
            // Start block only
            unsigned long bytes_in_block = KTFS_BLKSZ - start_block_offset_bytes;
            memcpy(&data_block->data.data[start_block_offset_bytes], buf, bytes_in_block);
            bytes_written += bytes_in_block;
        } else if (i == end_block_index) {
            // End block only
            unsigned long bytes_in_block = end_block_offset_bytes + 1;
            memcpy(&data_block->data.data[0], &((uint8_t*)buf)[bytes_written], bytes_in_block);
            cache_release_block(ktfs->cache, (void*)data_block, 1);
            return 0;
        } else {
            // Middle block
            memcpy(&data_block->data.data[0], &((uint8_t*)buf)[bytes_written], KTFS_BLKSZ);
            bytes_written += KTFS_BLKSZ;
        }

        cache_release_block(ktfs->cache, (void*)data_block, 1);
    }

    // If we haven't returned yet, something went wrong
    return -EBADFD;
}

static int ktfs_add_dentry(struct ktfs_fs* ktfs, struct ktfs_inode* dir_inode, uint16_t inode_num, const char* name) {
    // Find the position to write to
    unsigned long start_byte = dir_inode->size;
    unsigned long end_byte = start_byte + KTFS_DENSZ - 1;

    // Create the dentry
    struct ktfs_dir_entry new_dentry;
    new_dentry.inode = inode_num;
    strncpy(new_dentry.name, name, KTFS_MAX_FILENAME_LEN);

    // Expand the directory size
    int result = ktfs_expand_file(ktfs, dir_inode, dir_inode->size + KTFS_DENSZ);
    if (result != 0) {
        return result;
    }
    
    // Write the dentry
    result = ktfs_write_data(ktfs, dir_inode, start_byte, end_byte, &new_dentry);
    if (result != 0) {
        return result;
    }

    return 0;
}

static int ktfs_claim_inode(struct ktfs_fs* ktfs, uint16_t* inode_num_ptr) {
    // Read superblock
    struct ktfs_superblock_block* superblock;
    int result = cache_get_block_by_index(ktfs->cache, 0, (void **)&superblock);
    if (result != 0) {
        return result;
    }

    uint32_t inode_bitmap_block_count = superblock->fields.inode_bitmap_block_count;
    cache_release_block(ktfs->cache, (void*)superblock, 0);

    // Read inode bitmap
    // Note: each block tracks KTFS_BLKSZ * 8 inodes (1 bit per inode)

    unsigned long num_inodes = ktfs->K * (KTFS_BLKSZ / KTFS_INOSZ);
    unsigned long num_inodes_processed = 0;

    for (int i = 1; i < 1 + inode_bitmap_block_count; i++) {
        struct ktfs_bitmap_block* bitmap_block;
        result = cache_get_block_by_index(ktfs->cache, i, (void**)&bitmap_block);
        if (result != 0) {
            return result;
        }
        
        for (int byte_index = 0; byte_index < KTFS_BLKSZ; byte_index++) {
            for (int bit_index = 0; bit_index < 8; bit_index++) {

                // Are we out of inodes? (no more inode blocks left, even though bitmap is larger)
                if (num_inodes_processed >= num_inodes) {
                    cache_release_block(ktfs->cache, (void*)bitmap_block, 0);
                    return -ENOINODEBLKS;
                }
                num_inodes_processed++;
                
                uint8_t mask = 1 << bit_index;

                // Is the inode free?
                if ((bitmap_block->bitmap.bytes[byte_index] & mask) == 0) {
                    // Free inode found, claim it
                    bitmap_block->bitmap.bytes[byte_index] |= mask;

                    // Calculate inode number
                    uint16_t inode_num = ((i - 1) * KTFS_BLKSZ * 8) + (byte_index * 8) + bit_index;
                    *inode_num_ptr = inode_num;

                    cache_release_block(ktfs->cache, (void*)bitmap_block, 1);
                    return 0;
                }
            }
        }
    }

    // No free inode found
    return -ENOINODEBLKS;
}

static int ktfs_release_inode(struct ktfs_fs* ktfs, uint16_t inode_num) {
    // Read superblock
    struct ktfs_superblock_block* superblock;
    int result = cache_get_block_by_index(ktfs->cache, 0, (void **)&superblock);
    if (result != 0) {
        return result;
    }

    uint32_t inode_bitmap_block_count = superblock->fields.inode_bitmap_block_count;
    cache_release_block(ktfs->cache, (void*)superblock, 0);

    // Find the inode in the bitmap
    uint32_t inodes_per_block = KTFS_BLKSZ * 8;

    uint32_t block_index = inode_num / inodes_per_block;
    assert(block_index < inode_bitmap_block_count);

    uint32_t within_block_index = inode_num % inodes_per_block;
    uint32_t byte_index = within_block_index / 8;
    uint32_t bit_index = within_block_index % 8;

    struct ktfs_bitmap_block* bitmap_block;
    result = cache_get_block_by_index(ktfs->cache, 1 + block_index, (void**)&bitmap_block);
    if (result != 0) {
        return result;
    }

    uint8_t mask = 1 << bit_index;
    if ((bitmap_block->bitmap.bytes[byte_index] & mask) == 0) {
        // Inode is already free
        cache_release_block(ktfs->cache, (void*)bitmap_block, 0);
        return -EINVAL;
    }

    // Release the inode
    bitmap_block->bitmap.bytes[byte_index] &= ~mask;
    cache_release_block(ktfs->cache, (void*)bitmap_block, 1);
    return 0;
}

static int ktfs_claim_data_block(struct ktfs_fs* ktfs, uint32_t* block_num_ptr) {
    // Read superblock
    struct ktfs_superblock_block* superblock;
    int result = cache_get_block_by_index(ktfs->cache, 0, (void **)&superblock);
    if (result != 0) {
        return result;
    }

    uint32_t inode_bitmap_block_count = superblock->fields.inode_bitmap_block_count;
    uint32_t data_bitmap_block_count = superblock->fields.bitmap_block_count;
    cache_release_block(ktfs->cache, (void*)superblock, 0);

    // Read data bitmap
    for (int i = 1 + inode_bitmap_block_count; i < 1 + inode_bitmap_block_count + data_bitmap_block_count; i++) {
        struct ktfs_bitmap_block* bitmap_block;
        result = cache_get_block_by_index(ktfs->cache, i, (void**)&bitmap_block);
        if (result != 0) {
            return result;
        }
        
        for (int byte_index = 0; byte_index < KTFS_BLKSZ; byte_index++) {
            for (int bit_index = 0; bit_index < 8; bit_index++) {
                uint8_t mask = 1 << bit_index;

                // Is the data block free?
                if ((bitmap_block->bitmap.bytes[byte_index] & mask) == 0) {
                    // Free data block found, claim it
                    bitmap_block->bitmap.bytes[byte_index] |= mask;

                    // Calculate block number
                    uint32_t block_num = ((i - 1 - inode_bitmap_block_count) * KTFS_BLKSZ * 8) + (byte_index * 8) + bit_index;
                    *block_num_ptr = block_num;

                    cache_release_block(ktfs->cache, (void*)bitmap_block, 1);
                    return 0;
                }
            }
        }
    }

    // No free data block found
    return -ENODATABLKS;
}

static int ktfs_release_data_block(struct ktfs_fs* ktfs, uint32_t block_num) {
    // Read superblock
    struct ktfs_superblock_block* superblock;
    int result = cache_get_block_by_index(ktfs->cache, 0, (void **)&superblock);
    if (result != 0) {
        return result;
    }

    uint32_t inode_bitmap_block_count = superblock->fields.inode_bitmap_block_count;
    uint32_t data_bitmap_block_count = superblock->fields.bitmap_block_count;
    cache_release_block(ktfs->cache, (void*)superblock, 0);

    // Find the data block in the bitmap
    uint32_t data_blocks_per_block = KTFS_BLKSZ * 8;

    uint32_t block_index = block_num / data_blocks_per_block;
    assert(block_index < data_bitmap_block_count);
    uint32_t within_block_index = block_num % data_blocks_per_block;
    uint32_t byte_index = within_block_index / 8;
    uint32_t bit_index = within_block_index % 8;

    struct ktfs_bitmap_block* bitmap_block;
    result = cache_get_block_by_index(ktfs->cache, 1 + inode_bitmap_block_count + block_index, (void**)&bitmap_block);
    if (result != 0) {
        return result;
    }

    uint8_t mask = 1 << bit_index;
    if ((bitmap_block->bitmap.bytes[byte_index] & mask) == 0) {
        // Data block is already free
        cache_release_block(ktfs->cache, (void*)bitmap_block, 0);
        return -EINVAL;
    }

    // Release the data block
    bitmap_block->bitmap.bytes[byte_index] &= ~mask;
    cache_release_block(ktfs->cache, (void*)bitmap_block, 1);
    return 0;
}

static int ktfs_expand_file(struct ktfs_fs* ktfs, struct ktfs_inode* inode, unsigned long new_size) {
    // This function expands the file represented by inode to new_size bytes
    // It allocates new data blocks as necessary and updates the inode accordingly
    // It also handles indirect and doubly-indirect blocks (so if all the data blocks are used and
    // a new one is needed, it will allocate the indirect block as well)

    unsigned long current_size = inode->size;
    if (new_size <= current_size) {
        // No expansion needed
        return 0;
    }

    int current_num_blocks = (current_size + KTFS_BLKSZ - 1) / KTFS_BLKSZ;
    int new_num_blocks = (new_size + KTFS_BLKSZ - 1) / KTFS_BLKSZ;

    if (current_num_blocks == new_num_blocks) {
        // No new blocks needed
        inode->size = new_size;
        return 0;
    }

    int num_indirections_per_indirect_block = KTFS_BLKSZ / sizeof(uint32_t);

    // Allocate any necessary indirect or doubly-indirect blocks first
    if (current_num_blocks < KTFS_NUM_DIRECT_DATA_BLOCKS && new_num_blocks >= KTFS_NUM_DIRECT_DATA_BLOCKS) {
        // Need to allocate indirect block
        uint32_t indirect_block_num;
        int result = ktfs_claim_data_block(ktfs, &indirect_block_num);
        if (result != 0) {
            return result;
        }
        inode->indirect = indirect_block_num;
    }
    for (int i = 0; i < KTFS_NUM_DINDIRECT_BLOCKS; i++) {
        int dindirect_start_block = KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_NUM_INDIRECT_BLOCKS * num_indirections_per_indirect_block) + (i * num_indirections_per_indirect_block * num_indirections_per_indirect_block);
        if (current_num_blocks < dindirect_start_block && new_num_blocks >= dindirect_start_block) {
            // Need to allocate doubly-indirect block
            uint32_t dindirect_block_num;
            int result = ktfs_claim_data_block(ktfs, &dindirect_block_num);
            if (result != 0) {
                return result;
            }
            inode->dindirect[i] = dindirect_block_num;

            // Now, allocate all necessary indirect blocks within this doubly-indirect block
            for (int j = 0; j < num_indirections_per_indirect_block; j++) {
                int indirect_start_block = dindirect_start_block + (j * num_indirections_per_indirect_block);
                if (current_num_blocks < indirect_start_block && new_num_blocks >= indirect_start_block) {
                    // Need to allocate indirect block
                    uint32_t indirect_block_num;
                    result = ktfs_claim_data_block(ktfs, &indirect_block_num);
                    if (result != 0) {
                        return result;
                    }

                    // Update doubly-indirect block to point to this indirect block
                    struct ktfs_indirect_block* dindirect_block;
                    result = cache_get_block_by_index(ktfs->cache, inode->dindirect[i] + 1 + ktfs->K + ktfs->B + ktfs->N, (void**)&dindirect_block);
                    if (result != 0) {
                        return result;
                    }
                    dindirect_block->pointers[j] = indirect_block_num;
                    cache_release_block(ktfs->cache, (void*)dindirect_block, 1);
                } else {
                    // No more indirect blocks needed in this doubly-indirect block
                    break;
                }
            }
        } else {
            // No more doubly-indirect blocks needed
            break;
        }
    }

    // Now, allocate new data blocks
    for (int i = current_num_blocks; i < new_num_blocks; i++) {
        uint32_t data_block_num;
        int result = ktfs_claim_data_block(ktfs, &data_block_num);
        if (result != 0) {
            return result;
        }

        // Handle cases for direct, indirect, and doubly-indirect blocks
        if (i < KTFS_NUM_DIRECT_DATA_BLOCKS) {
            // Direct block
            inode->block[i] = data_block_num;
        } else if (i < KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_NUM_INDIRECT_BLOCKS * num_indirections_per_indirect_block)) {
            // Indirect block
            struct ktfs_indirect_block* indirect_block;
            result = cache_get_block_by_index(ktfs->cache, inode->indirect + 1 + ktfs->K + ktfs->B + ktfs->N, (void**)&indirect_block);
            if (result != 0) {
                return result;
            }

            uint32_t within_indirect_index = (i - KTFS_NUM_DIRECT_DATA_BLOCKS) % num_indirections_per_indirect_block;
            indirect_block->pointers[within_indirect_index] = data_block_num;
            cache_release_block(ktfs->cache, (void*)indirect_block, 1);
        } else {
            // Doubly-indirect block
            int new_i = i - (KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_NUM_INDIRECT_BLOCKS * num_indirections_per_indirect_block));
            int dindirect_block_index = new_i / (num_indirections_per_indirect_block * num_indirections_per_indirect_block);
            int within_dindirect_index = (new_i / num_indirections_per_indirect_block) % num_indirections_per_indirect_block;
            int within_indirect_index = new_i % num_indirections_per_indirect_block;
            
            struct ktfs_indirect_block* dindirect_block;
            result = cache_get_block_by_index(ktfs->cache, inode->dindirect[dindirect_block_index] + 1 + ktfs->K + ktfs->B + ktfs->N, (void**)&dindirect_block);
            if (result != 0) {
                return result;
            }
            uint32_t indirect_block_num = dindirect_block->pointers[within_dindirect_index];
            cache_release_block(ktfs->cache, (void*)dindirect_block, 0);

            struct ktfs_indirect_block* indirect_block;
            result = cache_get_block_by_index(ktfs->cache, indirect_block_num + 1 + ktfs->K + ktfs->B + ktfs->N, (void**)&indirect_block);
            if (result != 0) {
                return result;
            }
            indirect_block->pointers[within_indirect_index] = data_block_num;
            cache_release_block(ktfs->cache, (void*)indirect_block, 1);
        }
    }

    inode->size = new_size;
    return 0;
}

static int ktfs_shrink_file(struct ktfs_fs* ktfs, struct ktfs_inode* inode, unsigned long new_size) {
    // This function shrinks the file represented by inode to new_size bytes
    // It releases data blocks as necessary and updates the inode accordingly
    // It also handles indirect and doubly-indirect blocks (so if an indirect block is no longer
    // needed, it will be released as well)

    unsigned long current_size = inode->size;
    if (new_size >= current_size) {
        // No shrinking needed
        return 0;
    }

    int current_num_blocks = (current_size + KTFS_BLKSZ - 1) / KTFS_BLKSZ;
    int new_num_blocks = (new_size + KTFS_BLKSZ - 1) / KTFS_BLKSZ;

    if (current_num_blocks == new_num_blocks) {
        // No blocks to release
        inode->size = new_size;
        return 0;
    }

    int num_indirections_per_indirect_block = KTFS_BLKSZ / sizeof(uint32_t);

    // Release data blocks no longer needed
    for (int i = new_num_blocks; i < current_num_blocks; i++) {
        // Handle cases for direct, indirect, and doubly-indirect blocks
        if (i < KTFS_NUM_DIRECT_DATA_BLOCKS) {
            // Direct block
            ktfs_release_data_block(ktfs, inode->block[i]);
            inode->block[i] = 0;
        } else if (i < KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_NUM_INDIRECT_BLOCKS * num_indirections_per_indirect_block)) {
            // Indirect block
            struct ktfs_indirect_block* indirect_block;
            int result = cache_get_block_by_index(ktfs->cache, inode->indirect + 1 + ktfs->K + ktfs->B + ktfs->N, (void**)&indirect_block);
            if (result != 0) {
                return result;
            }

            uint32_t within_indirect_index = (i - KTFS_NUM_DIRECT_DATA_BLOCKS) % num_indirections_per_indirect_block;
            ktfs_release_data_block(ktfs, indirect_block->pointers[within_indirect_index]);
            indirect_block->pointers[within_indirect_index] = 0;
            cache_release_block(ktfs->cache, (void*)indirect_block, 1);
        } else {
            // Doubly-indirect block
            int new_i = i - (KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_NUM_INDIRECT_BLOCKS * num_indirections_per_indirect_block));
            int dindirect_block_index = new_i / (num_indirections_per_indirect_block * num_indirections_per_indirect_block);
            int within_dindirect_index = (new_i / num_indirections_per_indirect_block) % num_indirections_per_indirect_block;
            int within_indirect_index = new_i % num_indirections_per_indirect_block;

            struct ktfs_indirect_block* dindirect_block;
            int result = cache_get_block_by_index(ktfs->cache, inode->dindirect[dindirect_block_index] + 1 + ktfs->K + ktfs->B + ktfs->N, (void**)&dindirect_block);
            if (result != 0) {
                return result;
            }
            uint32_t indirect_block_num = dindirect_block->pointers[within_dindirect_index];
            cache_release_block(ktfs->cache, (void*)dindirect_block, 0);

            struct ktfs_indirect_block* indirect_block;
            result = cache_get_block_by_index(ktfs->cache, indirect_block_num + 1 + ktfs->K + ktfs->B + ktfs->N, (void**)&indirect_block);
            if (result != 0) {
                return result;
            }
            ktfs_release_data_block(ktfs, indirect_block->pointers[within_indirect_index]);
            indirect_block->pointers[within_indirect_index] = 0;
            cache_release_block(ktfs->cache, (void*)indirect_block, 1);
        }
    }

    // Now, check if we can release indirect or doubly-indirect blocks
    if (current_num_blocks > KTFS_NUM_DIRECT_DATA_BLOCKS && new_num_blocks <= KTFS_NUM_DIRECT_DATA_BLOCKS) {
        // Release indirect block
        ktfs_release_data_block(ktfs, inode->indirect);
        inode->indirect = 0;
    }
    for (int i = 0; i < KTFS_NUM_DINDIRECT_BLOCKS; i++) {
        int dindirect_start_block = KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_NUM_INDIRECT_BLOCKS * num_indirections_per_indirect_block) + (i * num_indirections_per_indirect_block * num_indirections_per_indirect_block);

        struct ktfs_indirect_block* dindirect_block;
        int result = cache_get_block_by_index(ktfs->cache, inode->dindirect[i] + 1 + ktfs->K + ktfs->B + ktfs->N, (void**)&dindirect_block);
        if (result != 0) {
            return result;
        }

        // First, release any indirect blocks within this doubly-indirect block if needed
        for (int j = 0; j < num_indirections_per_indirect_block; j++) {
            int indirect_start_block = dindirect_start_block + (j * num_indirections_per_indirect_block);

            if (current_num_blocks > indirect_start_block && new_num_blocks <= indirect_start_block) {
                // Release indirect block
                uint32_t indirect_block_num = dindirect_block->pointers[j];
                ktfs_release_data_block(ktfs, indirect_block_num);
                dindirect_block->pointers[j] = 0;
            }
        }

        // Can we release the whole doubly-indirect block too?
        if (current_num_blocks > dindirect_start_block && new_num_blocks <= dindirect_start_block) {
            // Release doubly-indirect block
            ktfs_release_data_block(ktfs, inode->dindirect[i]);
            inode->dindirect[i] = 0;
        }

        cache_release_block(ktfs->cache, (void*)dindirect_block, 1);
    }
    
    inode->size = new_size;
    return 0;
}

static int ktfs_does_file_exist(struct ktfs_fs* ktfs, const char* name, int* exists_ptr) {
    // Get root inode
    struct ktfs_inode* root_inode_orig;
    int root_inode_num;
    struct ktfs_inode_block* inode_block;
    int result = get_root_inode(ktfs, &root_inode_orig, &root_inode_num, &inode_block);
    if (result != 0) {
        return result;
    }

    // Copy root inode so we can release inode block
    struct ktfs_inode root_inode = *root_inode_orig;;
    cache_release_block(ktfs->cache, (void*)inode_block, 0);

    // Go through root directory to find the file
    uint32_t num_dentries = root_inode.size / KTFS_DENSZ;

    struct ktfs_dir_entry* dentry;
    struct ktfs_directory* dir_block;

    for (uint32_t i = 0; i < num_dentries; i++) {
        result = ktfs_get_nth_dentry(ktfs, &root_inode, i, &dentry, &dir_block);
        if (result != 0) {
            return result;
        }

        if (strcmp(dentry->name, name) == 0) {
            // File found
            *exists_ptr = 1;
            cache_release_block(ktfs->cache, (void*)dir_block, 0);
            return 0;
        }

        cache_release_block(ktfs->cache, (void*)dir_block, 0);
    }

    *exists_ptr = 0;
    return 0;
}

// OPENED FILES LIST HELPERS
//

static struct ktfs_file* get_opened_file(struct opened_files* ofiles, const char* name) {
    struct opened_file* cur = ofiles->head;
    while (cur != NULL) {
        if (strcmp(cur->file->dir_entry.name, name) == 0) {
            return cur->file;
        }
        cur = cur->next;
    }
    return NULL;
}

static int add_opened_file(struct opened_files* ofiles, struct ktfs_file* file) {
    struct opened_file* new_ofile = kcalloc(1, sizeof(struct opened_file));
    if (new_ofile == NULL) {
        return -ENOMEM;
    }
    new_ofile->file = file;
    new_ofile->next = NULL;

    if (ofiles->head == NULL) {
        ofiles->head = new_ofile;
        ofiles->tail = new_ofile;
    } else {
        ofiles->tail->next = new_ofile;
        ofiles->tail = new_ofile;
    }

    return 0;
}

static void remove_opened_file(struct opened_files* ofiles, struct ktfs_file* file) {
    struct opened_file* cur = ofiles->head;
    struct opened_file* prev = NULL;

    while (cur != NULL) {
        if (cur->file == file) {
            // Found it
            if (prev == NULL) {
                // The file is at the head
                ofiles->head = cur->next;
                // Is it the only file?
                if (ofiles->head == NULL) {
                    ofiles->tail = NULL;
                }
            } else {
                prev->next = cur->next;
                if (cur->next == NULL) {
                    ofiles->tail = prev;
                }
            }
            kfree(cur);
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

// CACHE WRAPPERS
//

static int cache_get_block_by_index(struct cache* cache, unsigned long index, void** pptr) {
    return cache_get_block(cache, index * KTFS_BLKSZ, pptr);
}

/**
 * @brief Mounts the file system with associated backing cache
 * @param cache Pointer to cache struct for the file system
 * @return 0 if mount successful, negative error code if error
 */
int mount_ktfs(const char* name, struct cache* cache) {
    struct ktfs_fs* fs = kcalloc(1, sizeof(struct ktfs_fs));

    if (fs == NULL) {
        return -ENOMEM;
    }

    fs->base.open = &ktfs_open;
    fs->base.create = &ktfs_create;
    fs->base.delete = &ktfs_delete;
    fs->base.flush = &ktfs_flush;

    fs->cache = cache;
    fs->opened_files.head = NULL;
    fs->opened_files.tail = NULL;

    // Read superblock
    struct ktfs_superblock_block* superblock;
    int result = cache_get_block_by_index(cache, 0, (void **)&superblock);
    if (result != 0) {
        return result;
    }

    fs->K = superblock->fields.inode_bitmap_block_count;
    fs->B = superblock->fields.bitmap_block_count;
    fs->N = superblock->fields.inode_block_count;

    cache_release_block(cache, (void*)superblock, 0);

    return attach_filesystem(name, (struct filesystem*)fs);
}

/**
 * @brief Opens a file or ls (listing) with the given name and returns a pointer to the uio through
 * the double pointer
 * @param name The name of the file to open or "\" for listing (CP3)
 * @param uioptr Will return a pointer to a file or ls (list) uio pointer through this double
 * pointer
 * @return 0 if open successful, negative error code if error
 */
int ktfs_open(struct filesystem* fs, const char* name, struct uio** uioptr) {
    struct ktfs_fs* ktfs = (struct ktfs_fs*)fs;

    if (name == NULL || uioptr == NULL) {
        return -EINVAL;
    }

    if (strcmp(name, "\\") == 0) {
        return ktfs_open_listing(ktfs, uioptr);
    } else {
        return ktfs_open_file(ktfs, name, uioptr);
    }
}

/**
 * @brief Closes the file that is represented by the uio struct
 * @param uio The file io to be closed
 * @return None
 */
void ktfs_close(struct uio* uio) {
    struct ktfs_file* file = (struct ktfs_file*)uio;
    struct ktfs_fs* ktfs = file->fs;

    remove_opened_file(&ktfs->opened_files, file);
    kfree(file);
}

/**
 * @brief Reads data from file attached to uio into provided argument buffer
 * @param uio uio of file to be read
 * @param buf Buffer to be filled
 * @param len Number of bytes to read
 * @return Number of bytes read if successful, negative error code if error
 */
long ktfs_fetch(struct uio* uio, void* buf, unsigned long len) {
    struct ktfs_file* file = (struct ktfs_file*)uio;

    // Only read up to file size
    if (len > file->size - file->pos) {
        len = file->size - file->pos;
    }

    unsigned long start_byte = file->pos;
    unsigned long end_byte = file->pos + len - 1;

    struct ktfs_inode inode_copy;
    int result = get_inode_from_dentry(file->fs, &file->dir_entry, &inode_copy);
    if (result != 0) {
        return result;
    }

    result = ktfs_read_data(file->fs, &inode_copy, start_byte, end_byte, buf);
    if (result != 0) {
        return result;
    }

    file->pos += len;
    return len;
}

/**
 * @brief Write data from the provided argument buffer into file attached to uio
 * @param uio The file to be written to
 * @param buf The buffer to be read from
 * @param len Number of bytes to write from the buffer to the file
 * @return Number of bytes written from the buffer to the file system if sucessful, negative error
 * code if error
 */
long ktfs_store(struct uio* uio, const void* buf, unsigned long len) {
    struct ktfs_file* file = (struct ktfs_file*)uio;
    struct ktfs_fs* ktfs = file->fs;

    // Read superblock
    struct ktfs_superblock_block* superblock;
    int result = cache_get_block_by_index(ktfs->cache, 0, (void**)&superblock);
    if (result != 0) {
        return result;
    }

    uint16_t inode_num = file->dir_entry.inode;

    uint16_t inodes_start_block = 1 + superblock->fields.inode_bitmap_block_count + superblock->fields.bitmap_block_count;
    uint16_t inode_block_offset = (inode_num * KTFS_INOSZ) / KTFS_BLKSZ;
    uint16_t inode_offset_within_block = inode_num % (KTFS_BLKSZ / KTFS_INOSZ);

    cache_release_block(ktfs->cache, (void*)superblock, 0);

    // Get inode
    struct ktfs_inode_block* inode_block;
    result = cache_get_block_by_index(ktfs->cache, inodes_start_block + inode_block_offset, (void**)&inode_block);
    if (result != 0) {
        return result;
    }
    struct ktfs_inode* inode = &inode_block->inodes[inode_offset_within_block];

    unsigned long start_byte = file->pos;
    
    if (start_byte + len > file->size) {
        // Expand the file first
        result = ktfs_expand_file(ktfs, inode, start_byte + len);
        if (result != 0) {
            cache_release_block(ktfs->cache, (void*)inode_block, 0);
            return result;
        }
        file->size = inode->size;
    }

    unsigned long end_byte = file->pos + len - 1;
    result = ktfs_write_data(ktfs, inode, start_byte, end_byte, buf);
    if (result != 0) {
        cache_release_block(ktfs->cache, (void*)inode_block, 0);
        return result;
    }

    cache_release_block(ktfs->cache, (void*)inode_block, 1);
    file->pos += len;
    return len;
}

/**
 * @brief Create a new file in the file system
 * @param fs The file system in which to create the file
 * @param name The name of the file
 * @return 0 if successful, negative error code if error
 */
int ktfs_create(struct filesystem* fs, const char* name) {
    struct ktfs_fs* ktfs = (struct ktfs_fs*)fs;

    // Does the file already exist?
    int exists;
    int result = ktfs_does_file_exist(ktfs, name, &exists);
    if (result != 0) {
        return result;
    }
    if (exists) {
        return -EEXIST;
    }

    // Claim an inode number
    uint16_t inode_num;
    result = ktfs_claim_inode(ktfs, &inode_num);
    if (result != 0) {
        return result;
    }

    // Create the inode
    struct ktfs_inode new_inode;
    new_inode.size = 0;
    memset(new_inode.block, 0, sizeof(new_inode.block));
    new_inode.indirect = 0;
    memset(new_inode.dindirect, 0, sizeof(new_inode.dindirect));

    // Write the inode to disk
    struct ktfs_superblock_block* superblock;
    result = cache_get_block_by_index(ktfs->cache, 0, (void **)&superblock);
    if (result != 0) {
        return result;
    }

    uint16_t inodes_start_block = 1 + superblock->fields.inode_bitmap_block_count + superblock->fields.bitmap_block_count;
    uint16_t inode_block_offset = (inode_num * KTFS_INOSZ) / KTFS_BLKSZ;
    uint16_t inode_offset_within_block = inode_num % (KTFS_BLKSZ / KTFS_INOSZ);

    cache_release_block(ktfs->cache, (void*)superblock, 0);

    struct ktfs_inode_block* inode_block;
    result = cache_get_block_by_index(ktfs->cache, inodes_start_block + inode_block_offset, (void**)&inode_block);
    if (result != 0) {
        return result;
    }

    inode_block->inodes[inode_offset_within_block] = new_inode;
    cache_release_block(ktfs->cache, (void*)inode_block, 1);

    // Create dentry
    struct ktfs_inode* root_inode;
    int root_inode_num;
    result = get_root_inode(ktfs, &root_inode, &root_inode_num, &inode_block);
    if (result != 0) {
        return result;
    }

    result = ktfs_add_dentry(ktfs, root_inode, inode_num, name);
    if (result != 0) {
        cache_release_block(ktfs->cache, (void*)inode_block, 0);
        return result;
    }

    cache_release_block(ktfs->cache, (void*)inode_block, 1);
    return 0;
}

/**
 * @brief Deletes a certain file from the file system with the given name
 * @param fs The file system to delete the file from
 * @param name The name of the file to be deleted
 * @return 0 if successful, negative error code if error
 */
int ktfs_delete(struct filesystem* fs, const char* name) {
    struct ktfs_fs* ktfs = (struct ktfs_fs*)fs;

    // Does the file exist?
    int exists;
    int result = ktfs_does_file_exist(ktfs, name, &exists);
    if (result != 0) {
        return result;
    }
    if (!exists) {
        return -ENOENT;
    }

    // Is the file open?
    struct ktfs_file* opened_file = get_opened_file(&ktfs->opened_files, name);
    if (opened_file != NULL) {
        return -EBUSY;
    }

    // Get root inode
    struct ktfs_inode* root_inode;
    int root_inode_num;
    struct ktfs_inode_block* inode_block;
    result = get_root_inode(ktfs, &root_inode, &root_inode_num, &inode_block);
    if (result != 0) {
        return result;
    }

    // Go through the root directory to find the dentry
    uint32_t num_dentries = root_inode->size / KTFS_DENSZ;  // Must be exactly divisible

    struct ktfs_dir_entry* dentry;
    struct ktfs_directory* dir_block;
    int dentry_index;
    int found = 0;
    
    for (uint32_t i = 0; i < num_dentries; i++) {
        // Get nth dentry
        result = ktfs_get_nth_dentry(ktfs, root_inode, i, &dentry, &dir_block);
        if (result != 0) {
            cache_release_block(ktfs->cache, (void*)inode_block, 0);
            return result;
        }
        // Check if this is the file we want
        if (strcmp(dentry->name, name) == 0) {
            dentry_index = i;
            found = 1;
            break;
        }
        cache_release_block(ktfs->cache, (void*)dir_block, 0);
    }

    if (!found) {
        // Dentry not found
        cache_release_block(ktfs->cache, (void*)inode_block, 0);
        return -ENOENT;
    }

    // We found the dentry, so save the inode number and release the dir block
    // All we need from now on is the inode number (to release it later) and the dentry index
    uint16_t inode_num = dentry->inode;
    cache_release_block(ktfs->cache, (void*)dir_block, 0);

    // To delete the dentry, we move the last dentry into its place and shrink the file
    // This is because we can't have gaps in the list of dentries
    if (dentry_index == num_dentries - 1) {
        // Last dentry, just shrink
        result = ktfs_shrink_file(ktfs, root_inode, root_inode->size - KTFS_DENSZ);
        if (result != 0) {
            cache_release_block(ktfs->cache, (void*)inode_block, 0);
            return result;
        }
    } else {
        // Get last dentry
        struct ktfs_dir_entry* last_dentry;
        struct ktfs_directory* dir_block;
        result = ktfs_get_nth_dentry(ktfs, root_inode, num_dentries - 1, &last_dentry, &dir_block);
        if (result != 0) {
            cache_release_block(ktfs->cache, (void*)inode_block, 0);
            return result;
        }
        // Write last dentry into dentry_index position
        result = ktfs_write_data(ktfs, root_inode, dentry_index * KTFS_DENSZ, (dentry_index + 1) * KTFS_DENSZ - 1, last_dentry);
        if (result != 0) {
            cache_release_block(ktfs->cache, (void*)dir_block, 0);
            cache_release_block(ktfs->cache, (void*)inode_block, 0);
            return result;
        }
        cache_release_block(ktfs->cache, (void*)dir_block, 0);
        // Shrink the file
        result = ktfs_shrink_file(ktfs, root_inode, root_inode->size - KTFS_DENSZ);
        if (result != 0) {
            cache_release_block(ktfs->cache, (void*)inode_block, 0);
            return result;
        }
    }

    cache_release_block(ktfs->cache, (void*)inode_block, 1);

    // Now, shrink the file to 0 (to release all data blocks, indirect blocks, etc.)
    
    // Read superblock
    struct ktfs_superblock_block* superblock;
    result = cache_get_block_by_index(ktfs->cache, 0, (void**)&superblock);
    if (result != 0) {
        return result;
    }

    uint16_t inodes_start_block = 1 + superblock->fields.inode_bitmap_block_count + superblock->fields.bitmap_block_count;
    uint16_t inode_block_offset = (inode_num * KTFS_INOSZ) / KTFS_BLKSZ;
    uint16_t inode_offset_within_block = inode_num % (KTFS_BLKSZ / KTFS_INOSZ);

    cache_release_block(ktfs->cache, (void*)superblock, 0);

    // Get inode
    struct ktfs_inode_block* file_inode_block;
    result = cache_get_block_by_index(ktfs->cache, inodes_start_block + inode_block_offset, (void**)&file_inode_block);
    if (result != 0) {
        return result;
    }
    struct ktfs_inode* file_inode = &file_inode_block->inodes[inode_offset_within_block];

    // Shrink to 0
    result = ktfs_shrink_file(ktfs, file_inode, 0);
    if (result != 0) {
        cache_release_block(ktfs->cache, (void*)file_inode_block, 0);
        return result;
    }
    cache_release_block(ktfs->cache, (void*)file_inode_block, 1);

    // Finally, release the inode
    result = ktfs_release_inode(ktfs, inode_num);
    if (result != 0) {
        return result;
    }

    return 0;
}

/**
 * @brief Given a file io object, a specific command, and possibly some arguments, execute the
 * corresponding functions
 * @details Any commands such as (FCNTL_GETEND, FCNTL_GETPOS, ...) should pass back through the arg
 * variable. Do not directly return the value.
 * @details FCNTL_GETEND should pass back the size of the file in bytes through the arg variable.
 * @details FCNTL_SETEND should set the size of the file to the value passed in through arg.
 * @details FCNTL_GETPOS should pass back the current position of the file pointer in bytes through
 * the arg variable.
 * @details FCNTL_SETPOS should set the current position of the file pointer to the value passed in
 * through arg.
 * @param uio the uio object of the file to perform the control function
 * @param cmd the operation to execute. KTFS should support FCNTL_GETEND, FCNTL_SETEND (CP2),
 * FCNTL_GETPOS, FCNTL_SETPOS.
 * @param arg the argument to pass in, may be different for different control functions
 * @return 0 if successful, negative error code if error
 */
int ktfs_cntl(struct uio* uio, int cmd, void* arg) {
    struct ktfs_file* file = (struct ktfs_file*)uio;

    if (cmd == FCNTL_GETEND) {
        *((unsigned long*)arg) = file->size;
        return 0;
    } else if (cmd == FCNTL_SETEND) {
        unsigned long new_size = *((unsigned long*)arg);
        struct ktfs_fs* ktfs = file->fs;

        // Read superblock
        struct ktfs_superblock_block* superblock;
        int result = cache_get_block_by_index(ktfs->cache, 0, (void**)&superblock);
        if (result != 0) {
            return result;
        }

        uint16_t inode_num = file->dir_entry.inode;

        uint16_t inodes_start_block = 1 + superblock->fields.inode_bitmap_block_count + superblock->fields.bitmap_block_count;
        uint16_t inode_block_offset = (inode_num * KTFS_INOSZ) / KTFS_BLKSZ;
        uint16_t inode_offset_within_block = inode_num % (KTFS_BLKSZ / KTFS_INOSZ);

        cache_release_block(ktfs->cache, (void*)superblock, 0);

        // Get inode
        struct ktfs_inode_block* inode_block;
        result = cache_get_block_by_index(ktfs->cache, inodes_start_block + inode_block_offset, (void**)&inode_block);
        if (result != 0) {
            return result;
        }
        struct ktfs_inode* inode = &inode_block->inodes[inode_offset_within_block];

        // Resize
        if (new_size > inode->size) {
            // Expand file
            result = ktfs_expand_file(ktfs, inode, new_size);
            if (result != 0) {
                cache_release_block(ktfs->cache, (void*)inode_block, 0);
                return result;
            }
        } else if (new_size < inode->size) {
            // Shrink file
            result = ktfs_shrink_file(ktfs, inode, new_size);
            if (result != 0) {
                cache_release_block(ktfs->cache, (void*)inode_block, 0);
                return result;
            }
        }

        // Update file size in ktfs_file struct
        file->size = new_size;

        cache_release_block(ktfs->cache, (void*)inode_block, 1);
    } else if (cmd == FCNTL_GETPOS) {
        *((unsigned long*)arg) = file->pos;
        return 0;
    } else if (cmd == FCNTL_SETPOS) {
        unsigned long new_pos = *((unsigned long*)arg);
        if (new_pos > file->size) {
            return -EINVAL;
        }
        file->pos = new_pos;
        return 0;
    }
    return -EINVAL;
}

/**
 * @brief Flushes the cache to the backing device
 * @return None
 */
void ktfs_flush(struct filesystem* fs) {
    struct ktfs_fs* ktfs = (struct ktfs_fs*)fs;

    cache_flush(ktfs->cache);

    // Although it seems like we might want to also close all opened files here, we're not supposed
    // to, so this function seems like a bit of a waste. I guess it helps for writable filesystems
    // though, since we would want to flush any unwritten data to disk.
}

/**
 * @brief Closes the listing device represented by the uio pointer
 * @param uio The uio pointer of ls
 * @return None
 */
void ktfs_listing_close(struct uio* uio) {
    struct ktfs_file* listing = (struct ktfs_file*)uio;
    
    remove_opened_file(&listing->fs->opened_files, listing);
    // kfree(listing);
}

/**
 * @brief Reads all of the files names in the file system using ls and copies them into the
 * providied buffer
 * @param uio The uio pointer of ls
 * @param buf The buffer to copy the file names to
 * @param bufsz The size of the buffer
 * @return The size written to the buffer
 */
long ktfs_listing_read(struct uio* uio, void* buf, unsigned long bufsz) {
    struct ktfs_file* listing = (struct ktfs_file*)uio;

    // listing is guaranteed to be the ktfs_file representing the root directory
    // Since it's returned in ktfs_open/ktfs_open_listing

    int num_dentries = listing->size / KTFS_DENSZ;  // Must be exactly divisible

    unsigned long bytes_copied = 0;

    for (int i = 0; i < num_dentries; i++) {
        // Get nth dentry
        struct ktfs_dir_entry* dentry;
        struct ktfs_directory* dir_block;
        int result = ktfs_get_nth_dentry(listing->fs, NULL, i, &dentry, &dir_block);
        if (result != 0) {
            return result;
        }

        int filename_len = strlen(dentry->name);

        if (bytes_copied + filename_len + 1 > bufsz) {
            // Not enough space to copy this filename and null terminator
            cache_release_block(listing->fs->cache, (void*)dir_block, 0);
            return bytes_copied;
        }

        memcpy(&((uint8_t*)buf)[bytes_copied], dentry->name, filename_len);  // Includes null terminator
        bytes_copied += filename_len;

        cache_release_block(listing->fs->cache, (void*)dir_block, 0);
    }

    return bytes_copied;
}
