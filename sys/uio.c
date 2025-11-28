/*! @file uio.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌​‌​‌​‍‌​⁠⁠‌
    @brief Uniform I/O interface
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#ifdef UIO_DEBUG
#define DEBUG
#endif

#ifdef UIO_TRACE
#define TRACE
#endif

#include "uio.h"

#include <stddef.h>  // for NULL and offsetof

#include "error.h"
#include "heap.h"
#include "memory.h"
#include "misc.h"
#include "string.h"
#include "thread.h"
#include "uioimpl.h"
#include "intr.h"

static void nulluio_close(struct uio* uio);

static long nulluio_read(struct uio* uio, void* buf, unsigned long bufsz);

static long nulluio_write(struct uio* uio, const void* buf, unsigned long buflen);

// INTERNAL GLOBAL VARIABLES AND CONSTANTS
//

void uio_close(struct uio* uio) {
    debug("uio_close: refcnt=%d, has_close=%d", uio->refcnt, (uio->intf->close != NULL));

    // Decrement reference count if it's greater than 0
    if (uio->refcnt > 0) {
        uio->refcnt--;
        debug("uio_close: decremented refcnt to %d", uio->refcnt);
    }

    // Only call the actual close method when refcnt reaches 0
    if (uio->refcnt == 0 && uio->intf->close != NULL) {
        debug("uio_close: calling close method");
        uio->intf->close(uio);
    } else if (uio->refcnt > 0) {
        debug("uio_close: NOT calling close (refcnt=%d still has references)", uio->refcnt);
    }
}

long uio_read(struct uio* uio, void* buf, unsigned long bufsz) {
    if (uio->intf->read != NULL) {
        if (0 <= (long)bufsz)
            return uio->intf->read(uio, buf, bufsz);
        else
            return -EINVAL;
    } else
        return -ENOTSUP;
}

long uio_write(struct uio* uio, const void* buf, unsigned long buflen) {
    if (uio->intf->write != NULL) {
        if (0 <= (long)buflen)
            return uio->intf->write(uio, buf, buflen);
        else
            return -EINVAL;
    } else
        return -ENOTSUP;
}

int uio_cntl(struct uio* uio, int op, void* arg) {
    if (uio->intf->cntl != NULL)
        return uio->intf->cntl(uio, op, arg);
    else
        return -ENOTSUP;
}

unsigned long uio_refcnt(const struct uio* uio) {
    assert(uio != NULL);
    return uio->refcnt;
}

int uio_addref(struct uio* uio) { return ++uio->refcnt; }

struct uio* create_null_uio(void) {
    static const struct uio_intf nulluio_intf = {
        .close = &nulluio_close, .read = &nulluio_read, .write = &nulluio_write};

    static struct uio nulluio = {.intf = &nulluio_intf, .refcnt = 0};

    return &nulluio;
}

static void nulluio_close(struct uio* uio) {
    // ...
}

static long nulluio_read(struct uio* uio, void* buf, unsigned long bufsz) {
    // ...
    return -ENOTSUP;
}

static long nulluio_write(struct uio* uio, const void* buf, unsigned long buflen) {
    // ...
    return -ENOTSUP;
}

struct pipe {
    struct uio wio; // writer uio
    struct uio rio; // reader uio

    unsigned int hpos; // head position
    unsigned int tpos; // tail position
    char* rbuf; // ring buffer of PAGE_SIZE

    struct lock lock;
    struct condition read_not_empty; // signalled when ring buffer is no longer empty, so it can be read
    struct condition write_not_full; // signalled when ring buffer is no longer full, so it can be written to

    int closing; // use to indicate that one of pipe_write_close or pipe_read_close is already freeing the pipe
};

static void pipe_write_close(struct uio *wio);
static void pipe_read_close(struct uio *rio);
static long pipe_read(struct uio *rio, void *buf, unsigned long bufsz);
static long pipe_write(struct uio *wio, const void *buf, unsigned long buflen);

static const struct uio_intf pipe_write_intf = {
    .close = &pipe_write_close,
    .read = NULL,
    .write = &pipe_write,
    .cntl = NULL
};

static const struct uio_intf pipe_read_intf = {
    .close = &pipe_read_close,
    .read = &pipe_read,
    .write = NULL,
    .cntl = NULL
};

static int rbuf_full(const struct pipe * pipe);
static void rbuf_putc(struct pipe * pipe, char c);
int rbuf_empty(const struct pipe * pipe);
char rbuf_getc(struct pipe * pipe);

void create_pipe(struct uio **wptr, struct uio **rptr) {
    struct pipe * pipe = kmalloc(sizeof(struct pipe));
    if (pipe == NULL) {
        *wptr = NULL;
        *rptr = NULL;
        return;
    }

    // allocate ring buffer
    pipe->rbuf = (char *)alloc_phys_page();
    if (pipe->rbuf == NULL) {
        kfree(pipe);
        *wptr = NULL;
        *rptr = NULL;
        return;
    }
    // initialize ring buffer
    pipe->hpos = 0;
    pipe->tpos = 0;

    // initialize lock and condition waits
    condition_init(&pipe->read_not_empty, "pipe.read_not_empty");
    condition_init(&pipe->write_not_full, "pipe.write_not_full");
    lock_init(&pipe->lock);
    pipe->closing = 0;

    // initialize and return uio
    uio_init1(&pipe->wio, &pipe_write_intf);
    uio_init1(&pipe->rio, &pipe_read_intf);

    *wptr = &pipe->wio;
    *rptr = &pipe->rio;
}

static void pipe_write_close(struct uio* wio) {
    struct pipe * pipe = (struct pipe *)((char*)wio - offsetof(struct pipe, wio));

    lock_acquire(&pipe->lock);

    condition_broadcast(&pipe->read_not_empty);
    int should_free = pipe->wio.refcnt == 0 && pipe->rio.refcnt == 0 && pipe->closing == 0;
    if (should_free) pipe->closing = 1;

    lock_release(&pipe->lock);

    if (should_free) {
        free_phys_page(pipe->rbuf);
        kfree(pipe);
    }
};

static void pipe_read_close(struct uio* rio) {
    struct pipe * pipe = (struct pipe *)((char*)rio - offsetof(struct pipe, rio));

    lock_acquire(&pipe->lock);
    condition_broadcast(&pipe->write_not_full);

    condition_broadcast(&pipe->read_not_empty);
    int should_free = pipe->wio.refcnt == 0 && pipe->rio.refcnt == 0 && pipe->closing == 0;
    if (should_free) pipe->closing = 1;

    lock_release(&pipe->lock);

    if (should_free) {
        free_phys_page(pipe->rbuf);
        kfree(pipe);
    }
};

static long pipe_read(struct uio* rio, void* buf, unsigned long bufsz) {
    struct pipe * pipe = (struct pipe *)((char*)rio - offsetof(struct pipe, rio));

    lock_acquire(&pipe->lock);

    // Wait for the first character to arrive (and there's writers that can send it)
    int pie = disable_interrupts();
    while (rbuf_empty(pipe) && pipe->wio.refcnt > 0) {
        lock_release(&pipe->lock);
        condition_wait(&pipe->read_not_empty);
        lock_acquire(&pipe->lock);
    }
    restore_interrupts(pie);

    // empty and no writers to fill it
    if (rbuf_empty(pipe) && pipe->wio.refcnt == 0) {
        lock_release(&pipe->lock);
        return 0;
    }

    unsigned int i = 0;

    while (i < bufsz) {
        if (rbuf_empty(pipe)) {
            lock_release(&pipe->lock);
            return i;
        }

        // keep wreading until done or empty
        while (i < bufsz && !rbuf_empty(pipe)) {
            char c = rbuf_getc(pipe);
            ((char *)buf)[i] = c;
            i++;
        }

        // Tell writers that the buffer is no longer full
        condition_broadcast(&pipe->write_not_full);
    }

    lock_release(&pipe->lock);
    
    return i;
};

static long pipe_write(struct uio* wio, const void* buf, unsigned long buflen) {
    struct pipe * pipe = (struct pipe *)((char*)wio - offsetof(struct pipe, wio));

    lock_acquire(&pipe->lock);

    unsigned int i = 0;

    while (i < buflen) {
        // wait while it's full (and there's readers that can read it)
        int pie = disable_interrupts();
        while (rbuf_full(pipe) && pipe->rio.refcnt > 0) {
            lock_release(&pipe->lock);
            condition_wait(&pipe->write_not_full);
            lock_acquire(&pipe->lock);
        }
        restore_interrupts(pie);

        // no readers, return -EPIPE
        if (pipe->rio.refcnt == 0) {
            lock_release(&pipe->lock);
            return -EPIPE;
        }

        // keep writing until done or full
        while (i < buflen && !rbuf_full(pipe)) {
            char c = ((const char *)buf)[i];
            rbuf_putc(pipe, c);
            i++;
        }

        // Tell readers that the buffer is no longer empty
        condition_broadcast(&pipe->read_not_empty);
    }

    lock_release(&pipe->lock);

    return i;
};

int rbuf_empty(const struct pipe * pipe) {
    return (pipe->hpos == pipe->tpos);
}

static int rbuf_full(const struct pipe * pipe) {
    return (pipe->tpos - pipe->hpos == PAGE_SIZE);
}

char rbuf_getc(struct pipe * pipe) {
    uint_fast16_t hpos;
    char c;

    hpos = pipe->hpos;
    c = pipe->rbuf[hpos % PAGE_SIZE];
    asm volatile ("" ::: "memory");
    pipe->hpos = hpos + 1;
    return c;
}

static void rbuf_putc(struct pipe * pipe, char c) {
    uint_fast16_t tpos = pipe->tpos;
    pipe->rbuf[tpos % PAGE_SIZE] = c;
    asm volatile ("" ::: "memory");
    pipe->tpos = tpos + 1;
}