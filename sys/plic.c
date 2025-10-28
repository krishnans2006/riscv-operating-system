// plic.c - RISC-V PLIC
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef PLIC_TRACE
#define TRACE
#endif

#ifdef PLIC_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "plic.h"
#include "misc.h"

#include <stdint.h>

// INTERNAL MACRO DEFINITIONS
//

// CTX(i,0) is hartid /i/ M-mode context
// CTX(i,1) is hartid /i/ S-mode context

#define CTX(i,s) (2*(i)+(s))

// INTERNAL TYPE DEFINITIONS
// 


struct plic_regs {
	union {
		uint32_t priority[PLIC_SRC_CNT]; /**< Interrupt Priorities registers */
		char _reserved_priority[0x1000];
	};

	union {
		uint32_t pending[PLIC_SRC_CNT / 32]; /**< Interrupt Pending Bits registers */
		char _reserved_pending[0x1000];
	};

	union {
		uint32_t enable[PLIC_CTX_CNT][32]; /**< Interrupt Enables registers */
		char _reserved_enable[0x200000-0x2000];
	};

	struct {
		union {
			struct {
				uint32_t threshold;	/**< Priority Thresholds registers */
				uint32_t claim;	/**< Interrupt Claim/Completion registers */
			};
			
			char _reserved_ctxctl[0x1000];
		};
	} ctx[PLIC_CTX_CNT];
};

#define PLIC (*(volatile struct plic_regs *)PLIC_MMIO_BASE)

// INTERNAL FUNCTION DECLARATIONS
//

static void plic_set_source_priority (
	uint_fast32_t srcno, uint_fast32_t level);

static int plic_source_pending(uint_fast32_t srcno);

static void plic_enable_source_for_context (
	uint_fast32_t ctxno, uint_fast32_t srcno);

static void plic_disable_source_for_context (
	uint_fast32_t ctxno, uint_fast32_t srcno);

static void plic_set_context_threshold (
	uint_fast32_t ctxno, uint_fast32_t level);

static uint_fast32_t plic_claim_context_interrupt (
	uint_fast32_t ctxno);

static void plic_complete_context_interrupt (
	uint_fast32_t ctxno, uint_fast32_t srcno);


static void plic_enable_all_sources_for_context(uint_fast32_t ctxno);

static void plic_disable_all_sources_for_context(uint_fast32_t ctxno);

// We currently only support single-hart operation, sending interrupts to S mode
// on hart 0 (context 1). The low-level PLIC functions already understand
// contexts, so we only need to modify the high-level functions (plit_init,
// plic_claim_request, plic_finish_request)to add support for multiple harts.

// EXPORTED FUNCTION DEFINITIONS
// 

void plic_init(void) {
	int i;

	// Disable all sources by setting priority to 0

	for (i = 0; i < PLIC_SRC_CNT; i++)
		plic_set_source_priority(i, 0);
	
	// Route all sources to S mode on hart 0 only

	for (int i = 0; i < PLIC_CTX_CNT; i++)
		plic_disable_all_sources_for_context(i);
	
	plic_enable_all_sources_for_context(CTX(0,1));
}

extern void plic_enable_source(int srcno, int prio) {
	trace("%s(srcno=%d,prio=%d)", __func__, srcno, prio);
	assert (0 < srcno && srcno <= PLIC_SRC_CNT);
	assert (prio > 0);

	plic_set_source_priority(srcno, prio);
}

extern void plic_disable_source(int irqno) {
	if (0 < irqno)
		plic_set_source_priority(irqno, 0);
	else
		debug("plic_disable_irq called with irqno = %d", irqno);
}

extern int plic_claim_interrupt(void) {
	trace("%s()", __func__);
	return plic_claim_context_interrupt(CTX(0,1));
}

extern void plic_finish_interrupt(int irqno) {
	trace("%s(irqno=%d)", __func__, irqno);
	plic_complete_context_interrupt(CTX(0,1), irqno);
}

// INTERNAL FUNCTION DEFINITIONS
//

// void plic_set_source_priority(uint_fast32_t srcno, uint_fast32_t level)
//
// Description:
//     Sets the priority level of the given interrupt source number.
//
// Inputs:
//     uint_fast32_t srcno - The source number of the interrupt to set the priority for.
//     uint_fast32_t level - The priority level to set for the interrupt source.
//
// Outputs:
//     void
//
// Side Effects:
//     Writes to a priority register on the PLIC
//
static inline void plic_set_source_priority(uint_fast32_t srcno, uint_fast32_t level) {
    if (srcno == 0 || srcno >= PLIC_SRC_CNT) {
        return;
    }
    if (level < PLIC_PRIO_MIN) {
        level = 0;
    } else if (level > PLIC_PRIO_MAX) {
        level = PLIC_PRIO_MAX;
    }
    
	PLIC.priority[srcno] = level;
}

// int plic_source_pending(uint_fast32_t srcno)
//
// Description:
//     Checks if the given interrupt source number is pending.
//
// Inputs:
//     uint_fast32_t srcno - The source number of the interrupt to check.
//
// Outputs:
//     int                 - 1 if the interrupt source is pending, else 0
//
// Side Effects:
//     Reads from a pending register on the PLIC
//
static inline int plic_source_pending(uint_fast32_t srcno) {
    if (srcno == 0 || srcno >= PLIC_SRC_CNT) {
        return 0;
    }
    
    int source_group = srcno / 32;
    int source_bit = srcno % 32;

    return (PLIC.pending[source_group] & (1 << source_bit)) > 0;
}

// void plic_enable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcno)
//
// Description:
//     Enables the given interrupt source for the specified context.
//
// Inputs:
//     uint_fast32_t ctxno - The context number to enable the interrupts for.
//     uint_fast32_t srcno - The source number of the device to enable interrupts for.
//
// Outputs:
//     void
//
// Side Effects:
//     Writes to an enable register on the PLIC
//
static inline void plic_enable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcno) {
    if (srcno == 0 || srcno >= PLIC_SRC_CNT) {
        return;
    }
    if (ctxno >= PLIC_CTX_CNT) {
        return;
    }

    int source_group = srcno / 32;
    int source_bit = srcno % 32;
    
    PLIC.enable[ctxno][source_group] |= (1 << source_bit);
}

// void plic_disable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcid)
//
// Description:
//     Disables the given interrupt source for the specified context.
//
// Inputs:
//     uint_fast32_t ctxno - The context number to disable the interrupts for.
//     uint_fast32_t srcid - The source number of the device to disable interrupts for.
//
// Outputs:
//     void
//
// Side Effects:
//     Writes to an enable register on the PLIC
//
static inline void plic_disable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcid) {
    if (ctxno >= PLIC_CTX_CNT) {
        return;
    }
    if (srcid == 0 || srcid >= PLIC_SRC_CNT) {
        return;
    }

    int source_group = srcid / 32;
    int source_bit = srcid % 32;
    
    PLIC.enable[ctxno][source_group] &= ~(1 << source_bit);
}

// void plic_set_context_threshold(uint_fast32_t ctxno, uint_fast32_t level)
//
// Description:
//     Sets the priority threshold for the specified context.
//
// Inputs:
//     uint_fast32_t ctxno - The context number to set the threshold for.
//     uint_fast32_t level - The priority threshold level to set.
//
// Outputs:
//     void
//
// Side Effects:
//     Writes to a ctx.threshold register on the PLIC
//
static inline void plic_set_context_threshold(uint_fast32_t ctxno, uint_fast32_t level) {
    if (ctxno >= PLIC_CTX_CNT) {
        return;
    }
    if (level < PLIC_PRIO_MIN) {
        level = 0;
    } else if (level > PLIC_PRIO_MAX) {
        level = PLIC_PRIO_MAX;
    }

    PLIC.ctx[ctxno].threshold = level;
}

// uint_fast32_t plic_claim_context_interrupt(uint_fast32_t ctxno)
//
// Description:
//     Claims an interrupt from the PLIC for the specified context.
//
// Inputs:
//     uint_fast32_t ctxno - The context number to claim the interrupt for.
//
// Outputs:
//     uint_fast32_t       - The source number of the claimed interrupt, or 0 if none are pending.
//
// Side Effects:
//     Reads from a ctx.claim register on the PLIC
//
static inline uint_fast32_t plic_claim_context_interrupt(uint_fast32_t ctxno) {
    if (ctxno >= PLIC_CTX_CNT) {
        return 0;
    }

    return (uint_fast32_t) PLIC.ctx[ctxno].claim;
}

// void plic_complete_context_interrupt(uint_fast32_t ctxno, uint_fast32_t srcno)
//
// Description:
//     Signals completion of an interrupt to the PLIC for the specified context.
//
// Inputs:
//     uint_fast32_t ctxno - The context number to complete the interrupt for.
//     uint_fast32_t srcno - The source number of the interrupt that was just handled.
//
// Outputs:
//     void
//
// Side Effects:
//     Writes to a ctx.claim register on the PLIC
//
static inline void plic_complete_context_interrupt(uint_fast32_t ctxno, uint_fast32_t srcno) {
    if (ctxno >= PLIC_CTX_CNT) {
        return;
    }
    if (srcno == 0 || srcno >= PLIC_SRC_CNT) {
        return;
    }

    PLIC.ctx[ctxno].claim = srcno;
}

// void plic_enable_all_sources_for_context(uint_fast32_t ctxno)
//
// Description:
//     Enables all interrupt sources for the specified context.
//
// Inputs:
//     uint_fast32_t ctxno - The context number to enable all interrupt sources for.
//
// Outputs:
//     void
//
// Side Effects:
//     Writes to some enable registers on the PLIC
//
static void plic_enable_all_sources_for_context(uint_fast32_t ctxno) {
    if (ctxno >= PLIC_CTX_CNT) {
        return;
    }

    for (int i = 1; i < PLIC_SRC_CNT; i++) {
        plic_enable_source_for_context(ctxno, i);
    }
}

// void plic_disable_all_sources_for_context(uint_fast32_t ctxno)
//
// Description:
//     Disables all interrupt sources for the specified context.
//
// Inputs:
//     uint_fast32_t ctxno - The context number to disable all interrupt sources for.
//
// Outputs:
//     void
//
// Side Effects:
//     Writes to some enable registers on the PLIC
//
static void plic_disable_all_sources_for_context(uint_fast32_t ctxno) {
    if (ctxno >= PLIC_CTX_CNT) {
        return;
    }

    for (int i = 1; i < PLIC_SRC_CNT; i++) {
        plic_disable_source_for_context(ctxno, i);
    }
}
