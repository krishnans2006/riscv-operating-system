// uart.c -  NS8250-compatible serial port
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef UART_TRACE
#define TRACE
#endif

#ifdef UART_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "misc.h"
#include "uart.h"
#include "devimpl.h"
#include "intr.h"
#include "heap.h"
#include "thread.h"
#include "console.h"

#include "error.h"

#include <stdint.h>

// COMPILE-TIME CONSTANT DEFINITIONS
//

#ifndef UART_RBUFSZ
#define UART_RBUFSZ 64
#endif

#ifndef UART_INTR_PRIO
#define UART_INTR_PRIO 1
#endif

#ifndef UART_DEVNAME
#define UART_DEVNAME "uart"
#endif


// INTERNAL TYPE DEFINITIONS
// 

struct uart_regs {
    union {
        char rbr; // DLAB=0 read
        char thr; // DLAB=0 write
        uint8_t dll; // DLAB=1
    };
    
    union {
        uint8_t ier; // DLAB=0
        uint8_t dlm; // DLAB=1
    };
    
    union {
        uint8_t iir; // read
        uint8_t fcr; // write
    };

    uint8_t lcr;
    uint8_t mcr;
    uint8_t lsr;
    uint8_t msr;
    uint8_t scr;
};

#define LCR_DLAB (1 << 7)
#define LSR_OE (1 << 1)
#define LSR_DR (1 << 0)
#define LSR_THRE (1 << 5)
#define IER_DRIE (1 << 0)
#define IER_THREIE (1 << 1)

// Simple fixed-size ring buffer

struct ringbuf {
    unsigned int hpos; // head of queue (from where elements are removed)
    unsigned int tpos; // tail of queue (where elements are inserted)
    char data[UART_RBUFSZ];
};

// UART device structure

struct uart_serial {
    struct serial base;
    volatile struct uart_regs * regs;
    int irqno;
    char opened;

    unsigned long rxovrcnt; ///< number of times OE was set
    
    struct condition rxbnotempty; ///< signalled when rxbuf becomes not empty
    struct condition txbnotfull;  ///< signalled when txbuf becomes not full

    struct ringbuf rxbuf;
    struct ringbuf txbuf;

    struct lock lock;
};

// INTERNAL FUNCTION DEFINITIONS
//

static int uart_serial_open(struct serial * ser);
static void uart_serial_close(struct serial * ser);
static int uart_serial_recv(struct serial * ser, void * buf, unsigned int bufsz);
static int uart_serial_send(struct serial * ser, const void * buf, unsigned int bufsz);

static void uart_isr(int srcno, void * aux);

// Ring buffer (struct rbuf) functions

static void rbuf_init(struct ringbuf * rbuf);
static int rbuf_empty(const struct ringbuf * rbuf);
static int rbuf_full(const struct ringbuf * rbuf);
static void rbuf_putc(struct ringbuf * rbuf, char c);
static char rbuf_getc(struct ringbuf * rbuf);

// INTERNAL GLOBAL VARIABLES
//

static const struct serial_intf uart_serial_intf = {
    .blksz = 1,
    .open = &uart_serial_open,
    .close = &uart_serial_close,
    .recv = &uart_serial_recv,
    .send = &uart_serial_send
};

// EXPORTED FUNCTION DEFINITIONS
//

void attach_uart(void * mmio_base, int irqno) {
    struct uart_serial * uart;

    trace("%s(%p,%d)", __func__, mmio_base, irqno);
    
    // UART0 is used for the console and should not be attached as a normal
    // device. It should already be initialized by console_init(). We still
    // register the device (to reserve the name uart0), but pass a NULL device
    // pointer, so that find_serial("uart", 0) returns NULL.

    if (mmio_base == (void*)UART0_MMIO_BASE) {
        register_device(UART_DEVNAME, DEV_SERIAL, NULL);
        return;
    }
    
    uart = kcalloc(1, sizeof(struct uart_serial));

    uart->regs = mmio_base;
    uart->irqno = irqno;
    uart->opened = 0;

    // Initialize condition variables. The ISR is registered when our interrupt
    // source is enabled in uart_serial_open().

    condition_init(&uart->rxbnotempty, "uart.rxnotempty");
    condition_init(&uart->txbnotfull, "uart.txnotfull");

    // Initialize lock

    lock_init(&uart->lock);

    // Initialize hardware

    uart->regs->ier = 0;
    uart->regs->lcr = LCR_DLAB;
    // fence o,o ?
    uart->regs->dll = 0x01;
    uart->regs->dlm = 0x00;
    // fence o,o ?
    uart->regs->lcr = 0; // DLAB=0

    serial_init(&uart->base, &uart_serial_intf);
    register_device(UART_DEVNAME, DEV_SERIAL, uart);
}

// int uart_serial_open(struct serial * ser)
//
// Description:
//     Opens the UART serial device for use.
//
// Inputs:
//     struct serial * ser  - Pointer to the serial struct representing the UART device
//
// Outputs:
//     int                  - 0 on success, -EBUSY if device is already open
//
// Side Effects:
//     Reads from the RBR register of the UART device
//     Writes to the IER register of the UART device
//     Registers an ISR for the UART device
//     Enables the interrupt source on the PLIC for the UART device
//
int uart_serial_open(struct serial * ser) {
    struct uart_serial * const uart =
        (void*)ser - offsetof(struct uart_serial, base);

    trace("%s()", __func__);

    if (uart->opened)
        return -EBUSY;
    
    // Reset receive and transmit buffers
    
    rbuf_init(&uart->rxbuf);
    rbuf_init(&uart->txbuf);

    // Read receive buffer register to flush any stale data in hardware buffer

    uart->regs->rbr; // forces a read because uart->regs is volatile

    // Enable interrupts when data ready (DR) status asserted
    uart->regs->ier = IER_DRIE;

    // Register interrupt handler
    int srcno = uart->irqno;
    enable_intr_source(srcno, UART_INTR_PRIO, &uart_isr, uart);

    // Indicate that device is open
    uart->opened = 1;

    return 0;
}

// void uart_serial_close(struct serial * ser)
//
// Description:
//     Closes the UART serial device.
//
// Inputs:
//     struct serial * ser  - Pointer to the serial struct representing the UART device
//
// Outputs:
//     void
//
// Side Effects:
//     Writes to the IER register of the UART device
//     Disables the interrupt source on the PLIC for the UART device
//
void uart_serial_close(struct serial * ser) {
    struct uart_serial * const uart =
        (void*)ser - offsetof(struct uart_serial, base);

    trace("%s()", __func__);

    if (!uart->opened) {
        return;
    }

    // Disable interrupts
    uart->regs->ier = 0;
    disable_intr_source(uart->irqno);

    // Indicate that device is closed
    uart->opened = 0;
}

// int uart_serial_recv(struct serial * ser, void * buf, unsigned int bufsz)
//
// Description:
//     Receives data from the UART device into the provided buffer.
//
// Inputs:
//     struct serial * ser      - Pointer to the serial struct representing the UART device
//     void * buf               - Pointer to the buffer where received data will be stored
//     unsigned int bufsz       - Size of the buffer in bytes
//
// Outputs:
//     int                      - Number of bytes received and copied into the buffer (between 0 and bufsz)
//
// Side Effects:
//     Can trigger a context switch
//     Writes to the IER register of the UART device
//
int uart_serial_recv(struct serial * ser, void * buf, unsigned int bufsz) {
    struct uart_serial * const uart = (void *)ser - offsetof(struct uart_serial, base);

    if (uart->opened == 0) {
        return -EINVAL;
    }

    // Enable DR and THRE interrupts
    uart->regs->ier |= IER_DRIE | IER_THREIE;

    // Wait for the first character to arrive
    int pie = disable_interrupts();
    while (rbuf_empty(&uart->rxbuf)) {
        condition_wait(&uart->rxbnotempty);
    }
    restore_interrupts(pie);

    unsigned int num_recvd = 0;

    for (unsigned int i = 0; i < bufsz; i++) {
        if (rbuf_empty(&uart->rxbuf)) {
            return num_recvd;
        }

        char c = rbuf_getc(&uart->rxbuf);
        ((char *)buf)[i] = c;
        num_recvd++;

        // Enable DR interrupts, since we just pulled a character from rxbuf (it's no longer full)
        uart->regs->ier |= IER_DRIE;
    }

    return num_recvd;
}

// int uart_serial_send(struct serial * ser, const void * buf, unsigned int bufsz)
//
// Description:
//     Sends data from the provided buffer to the UART device.
//
// Inputs:
//     struct serial * ser          - Pointer to the serial struct representing the UART device
//     const void * buf             - Pointer to the buffer containing data to be sent
//     unsigned int bufsz           - Size of the buffer in bytes
//
// Outputs:
//     int                          - Number of bytes sent (should be equal to bufsz)
//
// Side Effects:
//     Can trigger a context switch
//     Writes to the IER register of the UART device
//
int uart_serial_send(struct serial * ser, const void * buf, unsigned int bufsz) {
    struct uart_serial * const uart = (void *)ser - offsetof(struct uart_serial, base);

    if (uart->opened == 0) {
        return -EINVAL;
    }

    // Enable THRE interrupts
    uart->regs->ier |= IER_THREIE;

    lock_acquire(&uart->lock);

    unsigned int i = 0;

    while (i < bufsz) {
        int pie = disable_interrupts();
        while (rbuf_full(&uart->txbuf)) {
            condition_wait(&uart->txbnotfull);
        }
        restore_interrupts(pie);

        while (i < bufsz && !rbuf_full(&uart->txbuf)) {
            char c = ((const char *)buf)[i];
            rbuf_putc(&uart->txbuf, c);
            i++;
        }

        // Enable THRE interrupts, since we just added character(s) to txbuf (it's no longer empty)
        uart->regs->ier |= IER_THREIE;
    }

    lock_release(&uart->lock);
    
    return bufsz;
}

// void uart_isr(int srcno, void * aux)
//
// Description:
//     ISR for the UART device, to handle transmit and receive interrupts.
//
// Inputs:
//     int srcno        - Interrupt source number
//     void * aux       - Pointer to the uart_serial struct representing the UART device
//
// Outputs:
//     void
//
// Side Effects:
//     Reads from the LSR and RBR registers of the UART device
//     Writes to the THR and IER registers of the UART device
//     Broadcasts condition variables to wake waiting threads
//
void uart_isr(int srcno, void * aux) {
    struct uart_serial * uart = aux;  // aux is uart_serial*

    uint8_t lsr = uart->regs->lsr;

    // DR
    if (lsr & LSR_DR) {
        if (rbuf_full(&uart->rxbuf)) {
            // Disable interrupts
            uart->regs->ier &= ~IER_DRIE;
        } else {
            char c = uart->regs->rbr;
            rbuf_putc(&uart->rxbuf, c);
            condition_broadcast(&uart->rxbnotempty);
        }
    }

    // THRE
    if (lsr & LSR_THRE) {
        if (rbuf_empty(&uart->txbuf)) {
            // Disable interrupts
            uart->regs->ier &= ~IER_THREIE;
        } else {
            char c = rbuf_getc(&uart->txbuf);
            uart->regs->thr = c;
            condition_broadcast(&uart->txbnotfull);
        }
    }
}


void rbuf_init(struct ringbuf * rbuf) {
    rbuf->hpos = 0;
    rbuf->tpos = 0;
}


int rbuf_empty(const struct ringbuf * rbuf) {
    return (rbuf->hpos == rbuf->tpos);
}


int rbuf_full(const struct ringbuf * rbuf) {
    return (rbuf->tpos - rbuf->hpos == UART_RBUFSZ);
}


void rbuf_putc(struct ringbuf * rbuf, char c) {
    uint_fast16_t tpos;

    tpos = rbuf->tpos;
    rbuf->data[tpos % UART_RBUFSZ] = c;
    asm volatile ("" ::: "memory");
    rbuf->tpos = tpos + 1;
}

char rbuf_getc(struct ringbuf * rbuf) {
    uint_fast16_t hpos;
    char c;

    hpos = rbuf->hpos;
    c = rbuf->data[hpos % UART_RBUFSZ];
    asm volatile ("" ::: "memory");
    rbuf->hpos = hpos + 1;
    return c;
}

// The functions below provide polled uart input and output for the console.

#define UART0 (*(volatile struct uart_regs*)UART0_MMIO_BASE)

void console_device_init(void) {
    UART0.ier = 0x00;

    // Configure UART0. We set the baud rate divisor to 1, the lowest value,
    // for the fastest baud rate. In a physical system, the actual baud rate
    // depends on the attached oscillator frequency. In a virtualized system,
    // it doesn't matter.
    
    UART0.lcr = LCR_DLAB;
    UART0.dll = 0x01;
    UART0.dlm = 0x00;

    // The com0_putc and com0_getc functions assume DLAB=0.

    UART0.lcr = 0;
}

void console_device_putc(char c) {
    // Spin until THR is empty
    while (!(UART0.lsr & LSR_THRE))
        continue;

    UART0.thr = c;
}

char console_device_getc(void) {
    // Spin until RBR contains a byte
    while (!(UART0.lsr & LSR_DR))
        continue;
    
    return UART0.rbr;
}
