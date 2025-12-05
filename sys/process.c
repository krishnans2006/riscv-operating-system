/*! @file process.c
    @brief user process
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA

*/

/*!
 * @brief Enables trace messages for process.c
 */
#ifdef PROCESS_TRACE
#define TRACE
#endif

/*!
 * @brief Enables debug messages for process.c
 */
#ifdef PROCESS_DEBUG
#define DEBUG
#endif

#include "process.h"

#include "conf.h"
#include "elf.h"
#include "error.h"
#include "filesys.h"
#include "heap.h"
#include "memory.h"
#include "misc.h"
#include "riscv.h"
#include "string.h"
#include "thread.h"
#include "trap.h"
#include "uio.h"

// COMPILE-TIME PARAMETERS
//

/*!
 * @brief Maximum number of processes
 */
#ifndef NPROC
#define NPROC 16
#endif

// INTERNAL FUNCTION DECLARATIONS
//

static int build_stack(void* stack, int argc, char** argv);

static void fork_func(struct condition* forked, struct trap_frame* tfr);

// INTERNAL GLOBAL VARIABLES
//

/*!
 * @brief The main user process struct
 */
static struct process main_proc;

static struct process* proctab[NPROC] = {&main_proc};

// EXPORTED GLOBAL VARIABLES
//

char procmgr_initialized = 0;

// EXPORTED FUNCTION DEFINITIONS
//

void procmgr_init(void) {
    assert(memory_initialized && heap_initialized);
    assert(!procmgr_initialized);

    main_proc.tid = running_thread();
    main_proc.mtag = active_mspace();
    thread_set_process(main_proc.tid, &main_proc);
    procmgr_initialized = 1;
}

int process_exec(struct uio* exefile, int argc, char** argv) {
    // FIXME

    void *stack_page = NULL;
    int stksz;

    sfence_vma();

    int rc;

    stack_page = alloc_phys_page();

    stksz = build_stack(stack_page, argc, argv);

    reset_active_mspace();

    void (*entry)(void);   

    rc = elf_load(exefile, &entry);
    if (rc < 0) {
        uio_close(exefile);
        free_phys_page(stack_page);   
        return rc;
    }

    uio_close(exefile);

    uintptr_t user_stack_vma = UMEM_END_VMA - PAGE_SIZE;

    map_page(user_stack_vma, stack_page, PTE_R | PTE_W | PTE_U);

    uintptr_t usp = UMEM_END_VMA - (uintptr_t)stksz;

    struct trap_frame tfr;
    memset(&tfr, 0, sizeof(tfr));

    tfr.a0 = argc;                    
    tfr.a1 = (long)usp;         
    tfr.sp = (void *)usp;             
    tfr.ra = NULL;  

    tfr.sepc = (void *)entry;

    uintptr_t sstatus = csrr_sstatus();
    sstatus &= ~RISCV_SSTATUS_SPP;    // SPP = 0 
    sstatus &= ~RISCV_SSTATUS_SIE;    // SIE = 0 
    sstatus |= RISCV_SSTATUS_SPIE;    // SPIE = 1 
    tfr.sstatus = (long)sstatus;


    void *sscratch_value = (char *)running_thread_stack_base() - sizeof(struct trap_frame);

    trap_frame_jump(&tfr, sscratch_value);

}

int process_fork(const struct trap_frame* tfr) {
    // FIXME
    int tid = running_thread();

    int i;
    for(i = 0; i < NPROC; i++){
        if(proctab[i] == NULL){
            break;
        } else if (i == NPROC - 1){
            return -EMPROC;
        }
    }

    int pid = i;
    struct process * proc = kcalloc(1, sizeof(struct process));


    proc->mtag = clone_active_mspace();
    for(i = 0; i < PROCESS_UIOMAX; i ++){
        if(proctab[tid]->uiotab[i] != NULL){

            proc->uiotab[i] = proctab[tid]->uiotab[i];

            uio_addref(proc->uiotab[i]);

        }
    }
    proctab[pid] = proc;

    struct trap_frame * child_tfr = kcalloc(1, sizeof(struct trap_frame));

    *child_tfr = *tfr;

    child_tfr->a0 = 0;
    
    struct condition * cond = kcalloc(1, sizeof(struct condition));

    condition_init(cond, "fork process init");

    int tid_child = spawn_thread("fork child", (void(*)(void))&fork_func, cond, child_tfr);

    proc->tid = tid_child;

    thread_set_process(tid_child, proc);
    
    condition_wait(cond);

    return tid_child;
}

/** \brief
 *
 *
 *  Discard memory space, close your associated uio, free the memory you're supposed to free.
 *
 *
 */
void process_exit(void) {
    // FIXME
    struct process *proc = running_thread_process();

    for (int i = 0; i < PROCESS_UIOMAX; i++) {
        if (proc->uiotab[i] != NULL) {
            uio_close(proc->uiotab[i]);     
            proc->uiotab[i] = NULL;
        }
    }

    discard_active_mspace(); 

    proctab[proc->tid] = NULL;

    if (proc != &main_proc) {
        kfree(proc);                       
    }

    running_thread_exit();  

}

// INTERNAL FUNCTION DEFINITIONS
//

/**
 * \brief Builds the initial user stack for a new process.
 *
 * Builds the stack for a new process, including the argument vector (\p argv)
 * and the strings it points to. Note that \p argv must contain \p argc + 1
 * elements (the last one is a NULL pointer).
 *
 * Remember to round the final stack size up to a multiple of 16 bytes
 * (RISC-V ABI requirement).
 *
 * \param[in,out] stack  Pointer to the stack page (destination buffer).
 * \param[in]     argc   Number of arguments in \p argv.
 * \param[in]     argv   Array of argument pointers; length is \p argc+1 and
 *                       \p argv[argc] must be NULL.
 *
 * \return Size of the stack page on success; negative error code on failure.
 */
int build_stack(void* stack, int argc, char** argv) {
    size_t stksz, argsz;
    uintptr_t* newargv;
    char* p;
    int i;

    // We need to be able to fit argv[] on the initial stack page, so _argc_
    // cannot be too large. Note that argv[] contains argc+1 elements (last one
    // is a NULL pointer).

    if (PAGE_SIZE / sizeof(char*) - 1 < argc) return -ENOMEM;

    stksz = (argc + 1) * sizeof(char*);

    // Add the sizes of the null-terminated strings that argv[] points to.

    for (i = 0; i < argc; i++) {
        argsz = strlen(argv[i]) + 1;
        if (PAGE_SIZE - stksz < argsz) return -ENOMEM;
        stksz += argsz;
    }

    // Round up stksz to a multiple of 16 (RISC-V ABI requirement).

    stksz = ROUND_UP(stksz, 16);
    assert(stksz <= PAGE_SIZE);

    // Set _newargv_ to point to the location of the argument vector on the new
    // stack and set _p_ to point to the stack space after it to which we will
    // copy the strings. Note that the string pointers we write to the new
    // argument vector must point to where the user process will see the stack.
    // The user stack will be at the highest page in user memory, the address of
    // which is `(UMEM_END_VMA - PAGE_SIZE)`. The offset of the _p_ within the
    // stack is given by `p - newargv'.

    newargv = stack + PAGE_SIZE - stksz;
    p = (char*)(newargv + argc + 1);

    for (i = 0; i < argc; i++) {
        newargv[i] = (UMEM_END_VMA - PAGE_SIZE) + ((void*)p - (void*)stack);
        argsz = strlen(argv[i]) + 1;
        memcpy(p, argv[i], argsz);
        p += argsz;
    }

    newargv[argc] = 0;
    return stksz;
}

/**
 * \brief Function to be executed by the child process after fork.
 * This is a very beautiful function.
 * Tell the parent process that it is done with the trap frame, then jumps to user space (hint:
 * which function should we use?)
 *
 * \param[in] done  Pointer to a condition variable to signal parent
 * \param[in] tfr   Pointer to a trap frame
 *
 * \return NONE (very important, this is a hint)
 */
void fork_func(struct condition* done, struct trap_frame* tfr) {
    // FIXME
    condition_broadcast(done);

    void *sscratch_value = (char *)running_thread_stack_base() - sizeof(struct trap_frame);

    trap_frame_jump(tfr, sscratch_value);
    
}