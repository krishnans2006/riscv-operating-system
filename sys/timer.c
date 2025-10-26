// timer.c - A timer system
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//


#ifdef TIMER_TRACE
#define TRACE
#endif

#ifdef TIMER_DEBUG
#define DEBUG
#endif

#include "timer.h"
#include "thread.h"
#include "riscv.h"
#include "misc.h"
#include "intr.h"
#include "conf.h"
#include "see.h" // for set_stcmp

// EXPORTED GLOBAL VARIABLE DEFINITIONS
// 

char timer_initialized = 0;

// INTERNVAL GLOBAL VARIABLE DEFINITIONS
//

static struct alarm * sleep_list;

// INTERNAL FUNCTION DECLARATIONS
//

// EXPORTED FUNCTION DEFINITIONS
//

void timer_init(void) {
    set_stcmp(UINT64_MAX);
    timer_initialized = 1;
}

// void alarm_init(struct alarm * al, const char * name)
//
// Description:
//     Initializes an alarm struct.
//
// Inputs:
//     struct alarm * al  - Pointer to the alarm struct to initialize
//     const char * name  - Optional name for the condition variable
//
// Outputs:
//     void
//
// Side Effects:
//     Sets the twake value of the alarm to the current time
//
void alarm_init(struct alarm * al, const char * name) {
    if (name == NULL) {
        condition_init(&al->cond, "alarm");
    } else {
        condition_init(&al->cond, name);
    }

    al->next = NULL;
    al->twake = rdtime();
}

// void alarm_sleep(struct alarm * al, unsigned long long tcnt)
//
// Description:
//     Puts the current thread to sleep until the specified number of ticks has elapsed.
//
// Inputs:
//     struct alarm * al        - Pointer to the alarm struct representing the sleep request
//     unsigned long long tcnt  - Number of ticks to sleep, relative to the last init, reset, or wake-up
//
// Outputs:
//     void
//
// Side Effects:
//     Can trigger a context switch
//     Sets the twake value of the alarm
//     Modifies the sleep_list linked list of pending alarms
//     Sets the stcmp register to schedule the next timer interrupt
//     Writes to the STIE bit of the SIE CSR
//
void alarm_sleep(struct alarm * al, unsigned long long tcnt) {
    unsigned long long now;
    struct alarm * prev;
    int pie;

    now = rdtime();

    // If the tcnt is so large it wraps around, set it to UINT64_MAX

    if (UINT64_MAX - al->twake < tcnt)
        al->twake = UINT64_MAX;
    else
        al->twake += tcnt;
    
    // If the wake-up time has already passed, return

    if (al->twake < now)
        return;
    
    // Add alarm to sleep_list

    // Since sleep_list is modified in the timer ISR, disable interrupts
    pie = disable_interrupts();

    struct alarm * curr = sleep_list;
    prev = NULL;

    if (curr == NULL) {
        // Insert as only element
        sleep_list = al;
        al->next = NULL;
    } else {
        if (al->twake < curr->twake) {
            // Insert at head of list
            sleep_list = al;
            al->next = curr;
        } else {
            // Find insertion point
            while (curr != NULL && al->twake > curr->twake) {
                prev = curr;
                curr = curr->next;
            }

            // Insert between prev and curr
            // This also handles curr = NULL (insert at end of list)
            prev->next = al;
            al->next = curr;
        }
    }

    // Update mtimecmp
    if (sleep_list == al) {
        set_stcmp(al->twake);
    }

    // Force enable timer interrupts
    csrs_sie(RISCV_SIE_STIE);

    // Put current thread to sleep (this will also re-enable interrupts during the context switch)
    condition_wait(&al->cond);

    // When we return from condition_wait, the condition has been broadcast by the ISR and we're done sleeping
    // Restore interrupts to previous state for the thread to continue
    restore_interrupts(pie);
}

// Resets the alarm so that the next sleep increment is relative to the time
// alarm_reset is called.
void alarm_reset(struct alarm * al) {
    al->twake = rdtime();
}

void alarm_sleep_sec(struct alarm * al, unsigned int sec) {
    alarm_sleep(al, sec * TIMER_FREQ);
}

void alarm_sleep_ms(struct alarm * al, unsigned long ms) {
    alarm_sleep(al, ms * (TIMER_FREQ / 1000));
}

void alarm_sleep_us(struct alarm * al, unsigned long us) {
    alarm_sleep(al, us * (TIMER_FREQ / 1000 / 1000));
}

void sleep_sec(unsigned int sec) {
    sleep_ms(1000UL * sec);
}

void sleep_ms(unsigned long ms) {
    sleep_us(1000UL * ms);
}

void sleep_us(unsigned long us) {
    struct alarm al;

    alarm_init(&al, "sleep");
    alarm_sleep_us(&al, us);
}

// void handle_timer_interrupt(void)
//
// Description:
//     Handles a timer interrupt by waking up any alarms whose wake-up time has arrived.
//
// Inputs:
//     void
//
// Outputs:
//     void
//
// Side Effects:
//     Broadcasts condition variables to wake sleeping threads
//     Modifies the sleep_list linked list of pending alarms
//     Sets the stcmp register to schedule the next timer interrupt
//     Writes to the STIE bit of the SIE CSR
//
void handle_timer_interrupt(void) {
    struct alarm * head = sleep_list;
    struct alarm * next;
    uint64_t now;

    now = rdtime();

    trace("[%lu] %s()", now, __func__);
    debug("[%lu] mtcmp = %lu", now, rdtime());

    // Find all alarms that need to be woken up
    while (head != NULL && head->twake <= now) {
        head->twake = now;
        condition_broadcast(&head->cond);

        next = head->next;
        head = next;
    }

    sleep_list = head;
    if (sleep_list != NULL) {
        set_stcmp(sleep_list->twake);
    } else {
        // Disable timer interrupts
        csrc_sie(RISCV_SIE_STIE);
    }
}
