// rtc.c - Goldfish RTC driver
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef RTC_TRACE
#define TRACE
#endif

#ifdef RTC_DEBUG
#define DEBUG
#endif

#include "rtc.h"
#include "conf.h"
#include "assert.h"
#include "devimpl.h"
#include "console.h"
#include "string.h"
#include "heap.h"

#include "error.h"

#include <stdint.h>

// INTERNAL TYPE DEFINITIONS
// 

struct rtc_regs {
    uint32_t time_low;  // read first, latches time_high
    uint32_t time_high; //
};

struct rtc_device {
    struct serial base; // must be first
    volatile struct rtc_regs * regs;
};

// INTERNAL FUNCTION DEFINITIONS
//

static int rtc_open(struct serial * ser);
static void rtc_close(struct serial * ser);
static int rtc_recv(struct serial * ser, void * buf, unsigned int bufsz);

static uint64_t read_real_time(volatile struct rtc_regs * regs);

// INTERNAL GLOBAL VARIABLES AND CONSTANTS
//

static const struct serial_intf rtc_serial_intf = {
    .blksz = 8,
    .open = &rtc_open,
    .close = &rtc_close,
    .recv = &rtc_recv
};

// EXPORTED FUNCTION DEFINITIONS
//

// void rtc_attach(void * mmio_base)
//
// Description:
//     Attaches the RTC device at the given MMIO address by initializing and registering it.
//
// Inputs:
//     void * mmio_base  - The base address of the RTC device's MMIO region.
//
// Outputs:
//     void
//
// Side Effects:
//     Allocates memory for the device struct
//     Registers the device with the OS device manager
//
void rtc_attach(void * mmio_base) {
    struct rtc_device * rtc = (struct rtc_device *) kcalloc(1, sizeof(struct rtc_device));

    serial_init(&rtc->base, &rtc_serial_intf);
    rtc->regs = (volatile struct rtc_regs *)mmio_base;

    register_device("rtc", DEV_SERIAL, rtc);
}

int rtc_open(struct serial * ser) {
    trace("%s()", __func__);
    return 0;
}

void rtc_close(struct serial * ser) {
    trace("%s()", __func__);
}

// int rtc_recv(struct serial * ser, void * buf, unsigned int bufsz)
//
// Description:
//     Receives the RTC time value and copies it into the provided buffer
//
// Inputs:
//     struct serial * ser  - Pointer to the serial struct representing the RTC device
//     void * buf           - Pointer to the buffer where the time value will be copied
//     unsigned int bufsz   - Size of the buffer in bytes
//
// Outputs:
//     int                  - Number of bytes copied into the buffer (either 0 or 8)
//
// Side Effects:
//     Reads from the time_low and time_high registers of the RTC device
//
int rtc_recv(struct serial * ser, void * buf, unsigned int bufsz) {
    struct rtc_device * const rtc = (void *)ser - offsetof(struct rtc_device, base);

    if (bufsz == 0) {
        return 0;
    }

    uint64_t time = read_real_time(rtc->regs);

    // We always copy 8 bytes
    memcpy(buf, &time, 8);
    return 8;
}

// uint64_t read_real_time(volatile struct rtc_regs * regs)
//
// Description:
//     Reads the current time from the RTC registers.
//
// Inputs:
//     volatile struct rtc_regs * regs  - Pointer to the RTC registers struct corresponding to the device
//
// Outputs:
//     uint64_t                         - The current time value read from the RTC device
//
// Side Effects:
//     Reads from the time_low and time_high registers of the RTC device
//
uint64_t read_real_time(volatile struct rtc_regs * regs) {
    uint32_t time_l = regs->time_low;
    uint32_t time_h = regs->time_high;
    return ((uint64_t) time_h << 32) | time_l;
}
