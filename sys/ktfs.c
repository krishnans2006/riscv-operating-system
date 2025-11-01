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

static int get_root_inode(struct ktfs_fs* ktfs, struct ktfs_inode** root_inode_ptr, int* root_inode_num_ptr);
static int get_inode_from_dentry(struct ktfs_fs* ktfs, struct ktfs_dir_entry* dentry, struct ktfs_inode** inode_ptr);
static int ktfs_open_listing(struct ktfs_fs *ktfs, struct uio **uioptr);
static int ktfs_open_file(struct ktfs_fs* ktfs, const char* name, struct uio** uioptr);
static int ktfs_get_nth_dentry(struct ktfs_fs* ktfs, struct ktfs_inode* dir_inode, int n, struct ktfs_dir_entry** dentry_ptr);
static int ktfs_read_data(struct ktfs_fs* ktfs, struct ktfs_inode* inode, unsigned long start_byte, unsigned long end_byte, void* buf);

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

static int get_root_inode(struct ktfs_fs* ktfs, struct ktfs_inode** root_inode_ptr, int* root_inode_num_ptr) {
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

    cache_release_block(ktfs->cache, (void*)inode_block, 0);

    return 0;
}

static int get_inode_from_dentry(struct ktfs_fs* ktfs, struct ktfs_dir_entry* dentry, struct ktfs_inode** inode_ptr) {
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
    
    *inode_ptr = &inode_block->inodes[inode_offset_within_block];

    cache_release_block(ktfs->cache, (void*)inode_block, 0);

    return 0;
}

static int ktfs_open_listing(struct ktfs_fs* ktfs, struct uio** uioptr) {
    // Get root inode
    struct ktfs_inode* root_inode;
    int root_inode_num;
    int result = get_root_inode(ktfs, &root_inode, &root_inode_num);
    if (result != 0) {
        return result;
    }

    // Convert to listing uio
    struct ktfs_file* listing = kcalloc(1, sizeof(struct ktfs_file));
    if (listing == NULL) {
        return -ENOMEM;
    }

    listing->base.intf = &ktfs_listing_uio_intf;
    listing->dir_entry.inode = root_inode_num;
    strncpy(listing->dir_entry.name, "\\", KTFS_MAX_FILENAME_LEN);
    listing->size = root_inode->size;
    listing->pos = 0;
    listing->fs = ktfs;

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
    struct ktfs_inode* root_inode;
    int root_inode_num;
    int result = get_root_inode(ktfs, &root_inode, &root_inode_num);
    if (result != 0) {
        return result;
    }

    // Go through the root directory to find the file
    int num_dentries = root_inode->size / KTFS_DENSZ;  // Must be exactly divisible

    struct ktfs_dir_entry* dentry;
    
    for (int i = 0; i < num_dentries; i++) {
        // Get nth dentry
        result = ktfs_get_nth_dentry(ktfs, root_inode, i, &dentry);
        if (result != 0) {
            return result;
        }
        kprintf("Want: '%s', Found: '%s'\n", name, dentry->name);
        // Check if this is the file we want
        if (strcmp(dentry->name, name) == 0) {
            // Found the file, get its inode (for file size, etc.)
            struct ktfs_inode* file_inode;
            result = get_inode_from_dentry(ktfs, dentry, &file_inode);
            if (result != 0) {
                return result;
            }

            // Convert to file uio, add to opened files list, and return
            struct ktfs_file* file = kcalloc(1, sizeof(struct ktfs_file));
            if (file == NULL) {
                return -ENOMEM;
            }

            file->base.intf = &ktfs_file_uio_intf;
            file->dir_entry = *dentry;
            strncpy(file->dir_entry.name, name, KTFS_MAX_FILENAME_LEN);
            file->size = file_inode->size;
            file->pos = 0;
            file->fs = ktfs;

            result = add_opened_file(&ktfs->opened_files, file);
            if (result != 0) {
                kfree(file);
                return result;
            }

            *uioptr = (struct uio*)file;

            return 0;
        }
    }

    // File not found
    return -ENOENT;
}

static int ktfs_get_nth_dentry(struct ktfs_fs* ktfs, struct ktfs_inode* dir_inode, int n, struct ktfs_dir_entry** dentry_ptr) {
    // This function needs to deal with direct, indirect, and doubly-indirect blocks
    // This is difficult! So, we split it into three cases

    int num_dentries_per_block = KTFS_BLKSZ / KTFS_DENSZ;
    int num_indirections_per_indirect_block = KTFS_BLKSZ / sizeof(uint32_t);

    struct ktfs_directory* dir_block;

    if (n < KTFS_NUM_DIRECT_DATA_BLOCKS * num_dentries_per_block) {
        // Direct block case
        int block_index = n / num_dentries_per_block;
        int entry_index = n % num_dentries_per_block;

        int result = cache_get_block_by_index(ktfs->cache, dir_inode->block[block_index] + 4, (void**)&dir_block);
        if (result != 0) {
            return result;
        }

        *dentry_ptr = &dir_block->entries[entry_index];
    } else if (n < (KTFS_NUM_DIRECT_DATA_BLOCKS + KTFS_NUM_INDIRECT_BLOCKS * num_indirections_per_indirect_block) * num_dentries_per_block) {
        // Indirect block case
        // Note: We only have one indirect block, but we still use KTFS_NUM_INDIRECT_BLOCKS at times to make it more readable

        int new_n = n - KTFS_NUM_DIRECT_DATA_BLOCKS * num_dentries_per_block;  // Adjusted for direct blocks
        // int indirect_block_index = new_n / (num_indirections_per_indirect_block * num_dentries_per_block);
        int within_indirect_index = (new_n / num_dentries_per_block) % num_indirections_per_indirect_block;
        int entry_index = new_n % num_dentries_per_block;

        struct ktfs_indirect_block* indirect_block;
        int result = cache_get_block_by_index(ktfs->cache, dir_inode->indirect + 4, (void**)&indirect_block);
        if (result != 0) {
            return result;
        }
        uint32_t data_block_num = indirect_block->pointers[within_indirect_index];

        cache_release_block(ktfs->cache, (void*)indirect_block, 0);

        result = cache_get_block_by_index(ktfs->cache, data_block_num + 4, (void**)&dir_block);
        if (result != 0) {
            return result;
        }

        *dentry_ptr = &dir_block->entries[entry_index];
    } else {
        // Doubly-indirect block case
        int new_n = n - (KTFS_NUM_DIRECT_DATA_BLOCKS + KTFS_NUM_INDIRECT_BLOCKS * num_indirections_per_indirect_block) * num_dentries_per_block;
        int dindirect_block_index = new_n / (num_indirections_per_indirect_block * num_indirections_per_indirect_block * num_dentries_per_block);
        int within_dindirect_index = (new_n / (num_indirections_per_indirect_block * num_dentries_per_block)) % num_indirections_per_indirect_block;
        int within_indirect_index = (new_n / num_dentries_per_block) % num_indirections_per_indirect_block;
        int entry_index = new_n % num_dentries_per_block;

        struct ktfs_indirect_block* dindirect_block;
        int result = cache_get_block_by_index(ktfs->cache, dir_inode->dindirect[dindirect_block_index] + 4, (void**)&dindirect_block);
        if (result != 0) {
            return result;
        }
        uint32_t indirect_block_num = dindirect_block->pointers[within_dindirect_index];

        cache_release_block(ktfs->cache, (void*)dindirect_block, 0);

        struct ktfs_indirect_block* indirect_block;
        result = cache_get_block_by_index(ktfs->cache, indirect_block_num + 4, (void**)&indirect_block);
        if (result != 0) {
            return result;
        }
        uint32_t data_block_num = indirect_block->pointers[within_indirect_index];

        cache_release_block(ktfs->cache, (void*)indirect_block, 0);

        result = cache_get_block_by_index(ktfs->cache, data_block_num + 4, (void**)&dir_block);
        if (result != 0) {
            return result;
        }

        *dentry_ptr = &dir_block->entries[entry_index];
    }

    cache_release_block(ktfs->cache, (void*)dir_block, 0);

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

        if (i < KTFS_NUM_DIRECT_DATA_BLOCKS) {
            // Direct block case
            uint32_t data_block_num = inode->block[i];
            int result = cache_get_block_by_index(ktfs->cache, data_block_num + 4, (void**)&data_block);
            if (result != 0) {
                return result;
            }
        } else if (i < KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_NUM_INDIRECT_BLOCKS * num_indirections_per_indirect_block)) {
            // Indirect block case
            int within_indirect_index = (i - KTFS_NUM_DIRECT_DATA_BLOCKS) % num_indirections_per_indirect_block;
            
            struct ktfs_indirect_block* indirect_block;
            int result = cache_get_block_by_index(ktfs->cache, inode->indirect + 4, (void**)&indirect_block);
            if (result != 0) {
                return result;
            }
            uint32_t data_block_num = indirect_block->pointers[within_indirect_index];

            cache_release_block(ktfs->cache, (void*)indirect_block, 0);

            result = cache_get_block_by_index(ktfs->cache, data_block_num + 4, (void**)&data_block);
            if (result != 0) {
                return result;
            }
        } else {
            // Doubly-indirect block case
            int new_i = i - (KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_NUM_INDIRECT_BLOCKS * num_indirections_per_indirect_block));
            int dindirect_block_index = new_i / (num_indirections_per_indirect_block * num_indirections_per_indirect_block);
            int within_dindirect_index = (new_i / num_indirections_per_indirect_block) % num_indirections_per_indirect_block;
            int within_indirect_index = new_i % num_indirections_per_indirect_block;

            struct ktfs_indirect_block* dindirect_block;
            int result = cache_get_block_by_index(ktfs->cache, inode->dindirect[dindirect_block_index] + 4, (void**)&dindirect_block);
            if (result != 0) {
                return result;
            }
            uint32_t indirect_block_num = dindirect_block->pointers[within_dindirect_index];

            cache_release_block(ktfs->cache, (void*)dindirect_block, 0);

            struct ktfs_indirect_block* indirect_block;
            result = cache_get_block_by_index(ktfs->cache, indirect_block_num + 4, (void**)&indirect_block);
            if (result != 0) {
                return result;
            }
            uint32_t data_block_num = indirect_block->pointers[within_indirect_index];

            cache_release_block(ktfs->cache, (void*)indirect_block, 0);

            result = cache_get_block_by_index(ktfs->cache, data_block_num + 4, (void**)&data_block);
            if (result != 0) {
                return result;
            }
        }

        // Now, data_block has the data we want, so we copy the right parts of it to buf
        // Note: both offsets are inclusive
        if (i == start_block_index && i == end_block_index) {
            // Both start and end block
            memcpy(buf, &data_block->data.data[start_block_offset_bytes], end_block_offset_bytes - start_block_offset_bytes + 1);
            return 0;
        } else if (i == start_block_index) {
            // Start block only
            unsigned long bytes_to_copy = KTFS_BLKSZ - start_block_offset_bytes;
            memcpy(&((uint8_t*)buf)[bytes_copied], &data_block->data.data[start_block_offset_bytes], bytes_to_copy);
            bytes_copied += bytes_to_copy;
        } else if (i == end_block_index) {
            // End block only
            memcpy(&((uint8_t*)buf)[bytes_copied], &data_block->data.data[0], end_block_offset_bytes + 1);
            bytes_copied += end_block_offset_bytes + 1;
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
    // kfree(file);
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

    struct ktfs_inode* inode;
    int result = get_inode_from_dentry(file->fs, &file->dir_entry, &inode);
    if (result != 0) {
        return result;
    }

    result = ktfs_read_data(file->fs, inode, start_byte, end_byte, buf);
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
    // Not implemented yet (read-only file system for now)
    return -ENOTSUP;
}

/**
 * @brief Create a new file in the file system
 * @param fs The file system in which to create the file
 * @param name The name of the file
 * @return 0 if successful, negative error code if error
 */
int ktfs_create(struct filesystem* fs, const char* name) {
    // Not implemented yet (read-only file system for now)
    return -ENOTSUP;
}

/**
 * @brief Deletes a certain file from the file system with the given name
 * @param fs The file system to delete the file from
 * @param name The name of the file to be deleted
 * @return 0 if successful, negative error code if error
 */
int ktfs_delete(struct filesystem* fs, const char* name) {
    // Not implemented yet (read-only file system for now)
    return -ENOTSUP;
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
        // Not implemented yet (read-only file system for now)
        return -ENOTSUP;
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
        int result = ktfs_get_nth_dentry(listing->fs, NULL, i, &dentry);
        if (result != 0) {
            return result;
        }

        int filename_len = strlen(dentry->name);

        if (bytes_copied + filename_len + 1 > bufsz) {
            // Not enough space to copy this filename and null terminator
            return bytes_copied;
        }

        memcpy(&((uint8_t*)buf)[bytes_copied], dentry->name, filename_len);  // Includes null terminator
        bytes_copied += filename_len;
    }

    return bytes_copied;
}
