/*! @file vioblk.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌​‌​‌​‍‌​⁠⁠‌
    @brief VirtIO block device
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#include "devimpl.h"
#ifdef VIOBLK_TRACE
#define TRACE
#endif

#ifdef VIOBLK_DEBUG
#define DEBUG
#endif

#include <limits.h>

#include "conf.h"
#include "console.h"
#include "device.h"
#include "error.h"
#include "heap.h"
#include "intr.h"
#include "misc.h"
#include "string.h"
#include "thread.h"
#include "uio.h"  // FCNTL
#include "virtio.h"

// COMPILE-TIME PARAMETERS
//

#ifndef VIOBLK_INTR_PRIO
#define VIOBLK_INTR_PRIO 1
#endif

#ifndef VIOBLK_NAME
#define VIOBLK_NAME "vioblk"
#endif



// INTERNAL CONSTANT DEFINITIONS
//
#define VIRTIO_IRQ_STATUS_USED (1 << 0)
#define VIRTQ_LEN 128

#define VIRTIO_BLK_T_IN           0 
#define VIRTIO_BLK_T_OUT          1 
#define VIRTIO_BLK_T_FLUSH        4 
#define VIRTIO_BLK_T_GET_ID       8 
#define VIRTIO_BLK_T_GET_LIFETIME 10 
#define VIRTIO_BLK_T_DISCARD      11 
#define VIRTIO_BLK_T_WRITE_ZEROES 13 
#define VIRTIO_BLK_T_SECURE_ERASE   14


// VirtIO block device feature bits (number, *not* mask)

#define VIRTIO_BLK_F_SIZE_MAX 1
#define VIRTIO_BLK_F_SEG_MAX 2
#define VIRTIO_BLK_F_GEOMETRY 4
#define VIRTIO_BLK_F_RO 5
#define VIRTIO_BLK_F_BLK_SIZE 6
#define VIRTIO_BLK_F_FLUSH 9
#define VIRTIO_BLK_F_TOPOLOGY 10
#define VIRTIO_BLK_F_CONFIG_WCE 11
#define VIRTIO_BLK_F_MQ 12
#define VIRTIO_BLK_F_DISCARD 13
#define VIRTIO_BLK_F_WRITE_ZEROES 14

// INTERNAL TYPE DEFINITIONS
//

struct vioblk_storage {
    struct storage base;
    volatile struct virtio_mmio_regs * regs;
    int irqno;
    char opened;

    struct condition used_ring_updated;

    struct virtq_desc descriptors[VIRTQ_LEN];  // Descriptor table
    struct virtq_avail * avail;  // Avail ring
    struct virtq_used * used;  // Used ring
};

// INTERNAL FUNCTION DECLARATIONS
//

/**
 * @brief Sets the virtq avail and virtq used queues such that they are available for use. (Hint,
 * read virtio.h) Enables the interupt line for the virtio device and sets necessary flags in vioblk
 * device.
 * @param sto Storage IO struct for the storage device
 * @return Return 0 on success or negative error code if error. If the given sto is already opened,
 * then return -EBUSY.
 */
static int vioblk_storage_open(struct storage* sto);

/**
 * @brief Resets the virtq avail and virtq used queues and sets necessary flags in vioblk device. If
 * the given sto is not opened, this function does nothing.
 * @param sto Storage IO struct for the storage device
 * @return None
 */
static void vioblk_storage_close(struct storage* sto);

/**
 * @brief Reads bytecnt number of bytes from the disk and writes them to buf. Achieves this by
 * repeatedly setting the appropriate registers to request a block from the disk, waiting until the
 * data has been populated in block buffer cache, and then writes that data out to buf. Thread
 * sleeps while waiting for the disk to service the request. Reads that exceed the end of the block
 * device should be truncated. Reads whose bytecnt is not a multiple of blksz should be rounded
 * down to the nearest blksz.
 * @param sto Storage IO struct for the storage device
 * @param pos The starting position for the read within the VirtIO device
 * @param buf A pointer to the buffer to fill with the read data
 * @param bytecnt The number of bytes to read from the VirtIO device into the buffer
 * @return The number of bytes read from the device, or negative error code if error
 */
static long vioblk_storage_fetch(struct storage* sto, unsigned long long pos, void* buf,
                                 unsigned long bytecnt);

/**
 * @brief Writes bytecnt number of bytes from the parameter buf to the disk. The size of the virtio
 * device should not change. You should only overwrite existing data. Write should also not create
 * any new files. Achieves this by filling up the block buffer cache and then setting the
 * appropriate registers to request the disk write the contents of the cache to the specified block
 * location. Thread sleeps while waiting for the disk to service the request. Writes that exceed
 * the end of the block device should be truncated. Writes whose bytecnt is not a multiple of
 * blksz should be rounded down to the nearest blksz.
 * @param sto Storage IO struct for the storage device
 * @param pos The starting position for the write within the VirtIO device
 * @param buf A pointer to the buffer with the data to write
 * @param bytecnt The number of bytes to write to the VirtIO device from the buffer
 * @return The number of bytes written to the device, or negative error code if error
 */
static long vioblk_storage_store(struct storage* sto, unsigned long long pos, const void* buf,
                                 unsigned long bytecnt);

/**
 * @brief Given a file io object, a specific command, and possibly some arguments, execute the
 * corresponding functions on the VirtIO block device.
 * @details Any commands such as FCNTL_GETEND should pass back through the arg variable. Do not
 * directly return the value.
 * @details FCNTL_GETEND should return the capacity of the VirtIO block device in bytes.
 * @param sto Storage IO struct for the storage device
 * @param op Operation to execute. vioblk should support FCNTL_GETEND.
 * @param arg Argument specific to the operation being performed
 * @return Status code on the operation performed
 */
static int vioblk_storage_cntl(struct storage* sto, int op, void* arg);

/**
 * @brief The interrupt handler for the VirtIO device. When an interrupt occurs, the system will
 * call this function.
 * @param irqno The interrupt request number for the VirtIO device
 * @param aux A generic pointer for auxiliary data.
 * @return None
 */
static void vioblk_isr(int irqno, void *aux);

// INTERNAL GLOBAL VARIABLES
//

static const struct storage_intf vioblk_storage_intf = {
    .blksz = 512,
    .open = &vioblk_storage_open,
    .close = &vioblk_storage_close,
    .fetch = &vioblk_storage_fetch,
    .store = &vioblk_storage_store,
    .cntl = &vioblk_storage_cntl};

struct virtio_blk_req_hdr {
  uint32_t type; // IN=0, OUT=1, FLUSH=4, ...
  uint32_t reserved;
  uint64_t sector; // LBA in 512B units
} __attribute__((packed));

// EXPORTED FUNCTION DEFINITIONS
//

// Attaches a VirtIO block device. Declared and called directly from virtio.c.
/**
 * @brief Initializes virtio block device with the necessary IO operation
 * functions and sets the required feature bits.
 * @param regs Memory mapped register of Virtio
 * @param irqno Interrupt request number of the device
 * @return None
 */
void vioblk_attach(volatile struct virtio_mmio_regs* regs, int irqno) {
    virtio_featset_t enabled_features, wanted_features, needed_features;
    struct vioblk_storage* vbd;
    unsigned int blksz;
    int result;

    trace("%s(regs=%p,irqno=%d)", __func__, regs, irqno);

    assert(regs->device_id == VIRTIO_ID_BLOCK);

    // Signal device that we found a driver

    regs->status |= VIRTIO_STAT_DRIVER;
    __sync_synchronize();  // fence o,io

    // Negotiate features. We need:
    //  - VIRTIO_F_RING_RESET and
    //  - VIRTIO_F_INDIRECT_DESC
    // We want:
    //  - VIRTIO_BLK_F_BLK_SIZE and
    //  - VIRTIO_BLK_F_TOPOLOGY.

    virtio_featset_init(needed_features);
    virtio_featset_add(needed_features, VIRTIO_F_RING_RESET);
    virtio_featset_add(needed_features, VIRTIO_F_INDIRECT_DESC);
    virtio_featset_init(wanted_features);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_BLK_SIZE);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_TOPOLOGY);
    result = virtio_negotiate_features(regs, enabled_features, wanted_features, needed_features);

    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    // If the device provides a block size, use it. Otherwise, use 512.

    if (virtio_featset_test(enabled_features, VIRTIO_BLK_F_BLK_SIZE))
        blksz = regs->config.blk.blk_size;
    else
        blksz = 512;

    // blksz must be a power of two
    assert(((blksz - 1) & blksz) == 0);

    // FIXME
    vbd = kcalloc(1, sizeof(struct vioblk_storage));
    vbd->regs = regs;
    vbd->irqno = irqno;
    vbd->opened = 0;

    vbd->avail = kcalloc(1, VIRTQ_AVAIL_SIZE(VIRTQ_LEN));
    vbd->used = kcalloc(1, VIRTQ_USED_SIZE(VIRTQ_LEN));

    condition_init(&vbd->used_ring_updated, "vbd.used_ring_updated");

    virtio_attach_virtq(regs, 0, VIRTQ_LEN, (uint64_t) vbd->descriptors, (uint64_t) vbd->used, (uint64_t) vbd->avail);

    storage_init(&vbd->base, &vioblk_storage_intf, vbd->regs->config.blk.capacity);

    regs->status |= VIRTIO_STAT_DRIVER_OK; //set the driver to OK
    // fence o,oi
    __sync_synchronize();

    register_device(VIOBLK_NAME, DEV_STORAGE, vbd);

}

static int vioblk_storage_open(struct storage* sto) {
    // FIXME

    struct vioblk_storage * const vblk = (void *)sto - offsetof(struct vioblk_storage, base);

    if (vblk->opened) {
        return -EBUSY;
    }

    virtio_enable_virtq(vblk->regs, 0);

    int srcno = vblk->irqno;
    enable_intr_source(srcno, VIOBLK_INTR_PRIO, &vioblk_isr, vblk);

    vblk->opened = 1;

    return 0;
}

static void vioblk_storage_close(struct storage* sto) {
    // FIXME
    struct vioblk_storage * const vblk = (void *)sto - offsetof(struct vioblk_storage, base);

    if (!vblk->opened) {
        return;
    }

    disable_intr_source(vblk->irqno);
    virtio_reset_virtq(vblk->regs, 0);

    vblk->opened = 0;
    return;
}

static long vioblk_storage_fetch(struct storage* sto, unsigned long long pos, void* buf,
                                 unsigned long bytecnt) {
    // FIXME
    struct vioblk_storage * const vblk = (void *)sto - offsetof(struct vioblk_storage, base);

    if (!vblk->opened) {
        return -EINVAL;
    }

    if(bytecnt == 0){
        return 0;
    }


    uint64_t size = bytecnt / 512;

    struct virtio_blk_req_hdr * request = kcalloc(1, sizeof(struct virtio_blk_req_hdr));

    uint8_t *status = kcalloc(1, sizeof(uint8_t));

    request->type = VIRTIO_BLK_T_IN;
    request->sector = pos;

    vblk->descriptors[0].addr  = (uint64_t)request;
    vblk->descriptors[0].len   = sizeof *request;
    vblk->descriptors[0].flags = VIRTQ_DESC_F_NEXT;
    vblk->descriptors[0].next  = 1;

    vblk->descriptors[1].addr  = (uint64_t)(char*)buf;
    vblk->descriptors[1].len   = size*512;
    vblk->descriptors[1].flags = VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE;
    vblk->descriptors[1].next  = 2;

    vblk->descriptors[2].addr  = (uint64_t)status;
    vblk->descriptors[2].len   = 1;
    vblk->descriptors[2].flags = VIRTQ_DESC_F_WRITE;

    
    uint16_t curr_used_idx = vblk->used->idx;
    
    vblk->avail->ring[vblk->avail->idx % VIRTQ_LEN] = 0;
    vblk->avail->idx++;

        // Notify device
    virtio_notify_avail(vblk->regs, 0);

    int pie = disable_interrupts();
    while (vblk->used->idx == curr_used_idx) {
        condition_wait(&vblk->used_ring_updated);
    }
    restore_interrupts(pie);

    if (*status != 0) return -EIO;

    // When we get here, the device has processed our request
    // and the ISR should have run, responding to the interrupt.
    return size*512;
}

static long vioblk_storage_store(struct storage* sto, unsigned long long pos, const void* buf,
                                 unsigned long bytecnt) {
    // FIXME

    struct vioblk_storage * const vblk = (void *)sto - offsetof(struct vioblk_storage, base);

    if (!vblk->opened) {
        return -EINVAL;
    }

    if(bytecnt == 0){
        return 0;
    }

    uint64_t size = bytecnt / 512;

    struct virtio_blk_req_hdr * request = kcalloc(1, sizeof(struct virtio_blk_req_hdr));

    uint8_t *status = kcalloc(1, sizeof(uint8_t));

    request->type = VIRTIO_BLK_T_OUT;
    request->sector = pos;

    vblk->descriptors[0].addr  = (uint64_t)request;
    vblk->descriptors[0].len   = sizeof *request;
    vblk->descriptors[0].flags = VIRTQ_DESC_F_NEXT;
    vblk->descriptors[0].next  = 1;

    vblk->descriptors[1].addr  = (uint64_t)(char*)buf;
    vblk->descriptors[1].len   = size*512;
    vblk->descriptors[1].flags = VIRTQ_DESC_F_NEXT;
    vblk->descriptors[1].next  = 2;

    vblk->descriptors[2].addr  = (uint64_t)status;
    vblk->descriptors[2].len   = 1;
    vblk->descriptors[2].flags = VIRTQ_DESC_F_WRITE;

    
    uint16_t curr_used_idx = vblk->used->idx;
    
    vblk->avail->ring[vblk->avail->idx % VIRTQ_LEN] = 0;
    vblk->avail->idx++;

        // Notify device
    virtio_notify_avail(vblk->regs, 0);

    int pie = disable_interrupts();
    while (vblk->used->idx == curr_used_idx) {
        condition_wait(&vblk->used_ring_updated);
    }
    restore_interrupts(pie);

    if (*status != 0) return -EIO;

    // When we get here, the device has processed our request
    // and the ISR should have run, responding to the interrupt.
    return size*512;
}

static int vioblk_storage_cntl(struct storage* sto, int op, void* arg) {
    // FIXME
    struct vioblk_storage * const vblk = (void *)sto - offsetof(struct vioblk_storage, base);

    if (!vblk->opened) {
        return -EINVAL;
    }

    if(op == FCNTL_GETEND){
       *(unsigned long long*)arg = vblk->base.capacity;
    } 

    return -ENOTSUP;
}

static void vioblk_isr(int irqno, void* aux) {
    // FIXME

    struct vioblk_storage * const vblk = aux; // aux is viorng_serial*

    // Acknowledge interrupt
    uint32_t status = vblk->regs->interrupt_status;

    if (status & VIRTIO_IRQ_STATUS_USED) {
        // idx is the next index to be written to, so the last used element is at idx-1
        // struct virtq_used_elem used = vrng->used->ring[(vrng->used->idx - 1) % VIRTQ_LEN];

        // We don't care which element was added to the used ring, since we're going to broadcast anyways
        condition_broadcast(&vblk->used_ring_updated);

        vblk->regs->interrupt_ack = VIRTIO_IRQ_STATUS_USED;
        return;
    }

    // We didn't handle the interrupt...
    vblk->regs->interrupt_ack = 0;

    return;
}
