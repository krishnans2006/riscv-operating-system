// viorng.c - VirtIO rng device
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "virtio.h"
#include "intr.h"
#include "heap.h"
#include "error.h"
#include "string.h"
#include "thread.h"
#include "devimpl.h"
#include "misc.h"
#include "conf.h"
#include "intr.h"
#include "console.h"

// INTERNAL CONSTANT DEFINITIONS
//

#ifndef VIORNG_BUFSZ
#define VIORNG_BUFSZ 256
#endif

#ifndef VIORNG_NAME
#define VIORNG_NAME "viorng"
#endif

#ifndef VIORNG_IRQ_PRIO
#define VIORNG_IRQ_PRIO 1
#endif

#define VIRTQ_LEN 128

#define VIRTIO_IRQ_STATUS_USED (1 << 0)

// INTERNAL TYPE DEFINITIONS
//

struct viorng_serial {
    struct serial base;
    volatile struct virtio_mmio_regs * regs;
    int irqno;
    char opened;

    struct condition used_ring_updated; ///< signalled when ISR finds that the device has updated the used ring

    struct virtq_desc descriptors[VIRTQ_LEN];  // Descriptor table
    struct virtq_avail * avail;  // Avail ring
    struct virtq_used * used;  // Used ring
};

// INTERNAL FUNCTION DECLARATIONS
//

static int viorng_serial_open(struct serial * ser);

static void viorng_serial_close(struct serial * ser);
static int viorng_serial_recv(struct serial * ser, void * buf, unsigned int bufsz);

static void viorng_isr(int irqno, void * aux);

// INTERNAL GLOBAL VARIABLES
//

static const struct serial_intf viorng_serial_intf = {
    .blksz = 1,
    .open = &viorng_serial_open,
    .close = &viorng_serial_close,
    .recv = &viorng_serial_recv
};

// EXPORTED FUNCTION DEFINITIONS
//

// Attaches a VirtIO rng device. Declared and called directly from virtio.c.

// void viorng_attach(volatile struct virtio_mmio_regs * regs, int irqno)
//
// Description:
//     Attaches the VioRNG device at the given MMIO address by initializing and registering it.
//
// Inputs:
//     volatile struct virtio_mmio_regs * regs  - The MMIO registers of the VirtIO RNG device
//     int irqno                                - The source number of the device
//
// Outputs:
//     void
//
// Side Effects:
//     Allocates memory for the device struct
//     Registers the device with the OS device manager
//     Writes to the status register of the VirtIO device
//
void viorng_attach(volatile struct virtio_mmio_regs * regs, int irqno) {
    virtio_featset_t enabled_features, wanted_features, needed_features;
    struct viorng_serial * vrng;
    int result;
    
    assert (regs->device_id == VIRTIO_ID_RNG);

    // Signal device that we found a driver

    regs->status |= VIRTIO_STAT_DRIVER;
    // fence o,io
    __sync_synchronize();

    virtio_featset_init(needed_features);
    virtio_featset_init(wanted_features);
    result = virtio_negotiate_features(regs,
        enabled_features, wanted_features, needed_features);

    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    // Allocate and initialize device struct
    vrng = kcalloc(1, sizeof(struct viorng_serial));

    vrng->regs = regs;
    vrng->irqno = irqno;
    vrng->opened = 0;

    vrng->avail = kcalloc(1, VIRTQ_AVAIL_SIZE(VIRTQ_LEN));
    vrng->used = kcalloc(1, VIRTQ_USED_SIZE(VIRTQ_LEN));

    condition_init(&vrng->used_ring_updated, "viorng.used_ring_updated");

    virtio_attach_virtq(regs, 0, VIRTQ_LEN, (uint64_t) vrng->descriptors, (uint64_t) vrng->used, (uint64_t) vrng->avail);

    serial_init(&vrng->base, &viorng_serial_intf);

    regs->status |= VIRTIO_STAT_DRIVER_OK; //set the driver to OK
    // fence o,oi
    __sync_synchronize();

    register_device(VIORNG_NAME, DEV_SERIAL, vrng);
}

// int viorng_serial_open(struct serial * ser)
//
// Description:
//     Opens the VioRNG serial device for use.
//
// Inputs:
//     struct serial * ser  - Pointer to the serial struct representing the VioRNG device
//
// Outputs:
//     int                  - 0 on success, -EBUSY if device is already open
//
// Side Effects:
//     Registers an ISR for the VioRNG device
//     Enables the interrupt source on the PLIC for the VioRNG device
//
int viorng_serial_open(struct serial * ser) {
    struct viorng_serial * const vrng = (void *)ser - offsetof(struct viorng_serial, base);

    if (vrng->opened) {
        return -EBUSY;
    }

    virtio_enable_virtq(vrng->regs, 0);

    int srcno = vrng->irqno;
    enable_intr_source(srcno, VIORNG_IRQ_PRIO, &viorng_isr, vrng);

    vrng->opened = 1;

    return 0;
}

// void viorng_serial_close(struct serial * ser)
//
// Description:
//     Closes the VioRNG serial device.
//
// Inputs:
//     struct serial * ser  - Pointer to the serial struct representing the VioRNG device
//
// Outputs:
//     void
//
// Side Effects:
//     Disables the interrupt source on the PLIC for the VioRNG device
//
void viorng_serial_close(struct serial * ser) {
    struct viorng_serial * const vrng = (void *)ser - offsetof(struct viorng_serial, base);

    if (!vrng->opened) {
        return;
    }

    disable_intr_source(vrng->irqno);
    virtio_reset_virtq(vrng->regs, 0);

    vrng->opened = 0;
}

// int viorng_serial_recv(struct serial * ser, void * buf, unsigned int bufsz)
//
// Description:
//     Receives random data from the VioRNG device into the provided buffer.
//
// Inputs:
//     struct serial * ser      - Pointer to the serial struct representing the VioRNG device
//     void * buf               - Pointer to the buffer where received data will be stored
//     unsigned int bufsz       - Size of the buffer in bytes
//
// Outputs:
//     int                      - Number of bytes received and copied into the buffer (between 0 and bufsz)
//
// Side Effects:
//     Can trigger a context switch
//     Writes to the avail ring of the VirtIO device
//     Notifies the VirtIO device of new available descriptors
//     Reads from the used ring of the VirtIO device
//
int viorng_serial_recv(struct serial * ser, void * buf, unsigned int bufsz) {
    struct viorng_serial * const vrng = (void *)ser - offsetof(struct viorng_serial, base);

    if (!vrng->opened) {
        return -EINVAL;
    }

    if (bufsz == 0) {
        return 0;
    }

    uint16_t curr_used_idx = vrng->used->idx;

    // Set descriptor 0 to point to buf
    vrng->descriptors[0].addr = (uint64_t) buf;
    vrng->descriptors[0].len = bufsz;
    vrng->descriptors[0].flags = VIRTQ_DESC_F_WRITE;
    vrng->descriptors[0].next = 0;

    // Add descriptor 0 to avail ring
    vrng->avail->ring[vrng->avail->idx % VIRTQ_LEN] = 0;
    vrng->avail->idx++;

    // Notify device
    virtio_notify_avail(vrng->regs, 0);

    int pie = disable_interrupts();
    while (vrng->used->idx == curr_used_idx) {
        condition_wait(&vrng->used_ring_updated);
    }
    restore_interrupts(pie);

    // When we get here, the device has processed our request
    // and the ISR should have run, responding to the interrupt.
    return vrng->used->ring[(curr_used_idx) % VIRTQ_LEN].len;
}

// void viorng_isr(int irqno, void * aux)
//
// Description:
//     ISR for the VioRNG device, to handle used ring updates.
//
// Inputs:
//     int irqno            - The source number of the interrupt
//     void * aux           - Pointer to the viorng_serial struct representing the VioRNG device
//
// Outputs:
//     void
//
// Side Effects:
//     Reads from the interrupt_status register of the VirtIO device
//     Broadcasts to the used_ring_updated condition variable
//
void viorng_isr(int irqno, void * aux) {
    struct viorng_serial * const vrng = aux; // aux is viorng_serial*

    // Acknowledge interrupt
    uint32_t status = vrng->regs->interrupt_status;

    if (status & VIRTIO_IRQ_STATUS_USED) {
        // idx is the next index to be written to, so the last used element is at idx-1
        // struct virtq_used_elem used = vrng->used->ring[(vrng->used->idx - 1) % VIRTQ_LEN];

        // We don't care which element was added to the used ring, since we're going to broadcast anyways
        condition_broadcast(&vrng->used_ring_updated);

        vrng->regs->interrupt_ack = VIRTIO_IRQ_STATUS_USED;
        return;
    }

    // We didn't handle the interrupt...
    vrng->regs->interrupt_ack = 0;
}
