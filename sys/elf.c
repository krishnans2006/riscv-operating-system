/*! @file elf.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌​‌​‌​‍‌​⁠⁠‌
    @brief ELF file loader
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA

*/

#ifdef ELF_TRACE
#define TRACE
#endif

#ifdef ELF_DEBUG
#define DEBUG
#endif

#include "elf.h"

#include <stdint.h>

#include "conf.h"
#include "error.h"
#include "memory.h"
#include "misc.h"
#include "string.h"
#include "uio.h"

// Offsets into e_ident

#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6
#define EI_OSABI 7
#define EI_ABIVERSION 8
#define EI_PAD 9

// ELF header e_ident[EI_CLASS] values

#define ELFCLASSNONE 0
#define ELFCLASS32 1
#define ELFCLASS64 2

// ELF header e_ident[EI_DATA] values

#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

// ELF header e_ident[EI_VERSION] values

#define EV_NONE 0
#define EV_CURRENT 1

// ELF header e_type values

enum elf_et { ET_NONE = 0, ET_REL, ET_EXEC, ET_DYN, ET_CORE };

/*! @struct elf64_ehdr
    @brief ELF header struct
*/
struct elf64_ehdr {
    unsigned char e_ident[16]; // EI_MAG0, EI_MAG1, EI_MAG2, EI_MAG3, EI_CLASS(4), EI_DATA(5), EI_VERSION(6), EI_OSABI(7), EI_ABIVERSION(8), EI_PAD(9-15)
    uint16_t e_type;      // object file type
    uint16_t e_machine;   // required architecture, like EM_RISCV
    uint32_t e_version;   // file version: EV_NONE (invalid version) or  EV_CURRENT (current version)
    uint64_t e_entry;     // ✨ virtual address to which the system first transfers control, thus starting the process
    uint64_t e_phoff;     // ⭐️ program header table's file offset in bytes
    uint64_t e_shoff;     // section header table's file offset in bytes
    uint32_t e_flags;
    uint16_t e_ehsize;    // ELF header's size in bytes
    uint16_t e_phentsize; // ⭐️ size in bytes of one entry in the file's program header table
    uint16_t e_phnum;     // ⭐️ number of entries in the program header table
    uint16_t e_shentsize; // size in bytes of one entry in the file's section header table
    uint16_t e_shnum;     // number of entries in the section header table
    uint16_t e_shstrndx;  // section header table index of the entry associated with the section name string table
};

/*! @enum elf_pt
    @brief Program header p_type values
*/
enum elf_pt { PT_NULL = 0, PT_LOAD, PT_DYNAMIC, PT_INTERP, PT_NOTE, PT_SHLIB, PT_PHDR, PT_TLS };

// Program header p_flags bits

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

/*! @struct elf64_phdr
    @brief Program header struct
*/
struct elf64_phdr {
    uint32_t p_type; // PT_LOAD means loadable
    uint32_t p_flags;
    uint64_t p_offset; // offset from the beginning of the file at which the first byte of the segment resides
    uint64_t p_vaddr;  // virtual address at which the first byte of the segment resides in memory
    uint64_t p_paddr;
    uint64_t p_filesz; // number of bytes in the file image of the segment
    uint64_t p_memsz;  // number of bytes in the memory image of the segment
    uint64_t p_align;
};

// ELF header e_machine values (short list)

#define EM_RISCV 243
/**
 * \brief Validates and loads an ELF file into memory.
 *
 * This function validates an ELF file, then loads its contents into memory,
 * returning the start of the entry point through \p eptr.
 *
 * The loader processes only program header entries of type `PT_LOAD`. The layouts
 * of structures and magic values can be found in the Linux ELF header file
 * `<uapi/linux/elf.h>`
 * The implementation should ensure that all loaded sections of the program are
 * mapped within the memory range `0x80100000` to `0x81000000`.
 *
 * Let's do some reading! The following documentation will be very helpful!
 * [Helpful doc](https://linux.die.net/man/5/elf)
 * Good luck!
 * [Educational video](https://www.youtube.com/watch?v=dQw4w9WgXcQ)
 *
 * \param[in]  uio  Pointer to an user I/O corresponding to the ELF file.
 * \param[out] eptr   Double pointer used to return the ELF file's entry point.
 *
 * \return 0 on success, or a negative error code on failure.
 */
int elf_load(struct uio* uio, void (**eptr)(void)) {
    // FIXME
    struct elf64_ehdr ehdr;
    unsigned long long pos = 0;
    if (uio_cntl(uio, FCNTL_SETPOS, &pos) < 0) return -EIO;
    if (uio_read(uio, &ehdr, sizeof(ehdr)) != sizeof(ehdr)) return -EIO;

    if (ehdr.e_ident[0] != 0x7f || ehdr.e_ident[1] != 'E' || ehdr.e_ident[2] != 'L' || ehdr.e_ident[3] != 'F' 
        || ehdr.e_ident[EI_CLASS] != ELFCLASS64
        || ehdr.e_ident[EI_DATA] != ELFDATA2LSB
        || ehdr.e_type != ET_EXEC
        || ehdr.e_machine != EM_RISCV
        || ehdr.e_phoff == 0) {
        return -EBADFMT;
    }

    struct elf64_phdr phdr;
    for (size_t i = 0; i < ehdr.e_phnum; i++) {
        // get program header
        uint64_t ph_location = ehdr.e_phoff + i * ehdr.e_phentsize;
        if (uio_cntl(uio, FCNTL_SETPOS, &ph_location) < 0) return -EIO;
        if (uio_read(uio, &phdr, sizeof(phdr)) != sizeof(phdr)) return -EIO;
        if (phdr.p_type != PT_LOAD) continue;

        // the file size may not be larger than the memory size
        if (phdr.p_filesz > phdr.p_memsz) return -ENOMEM;
        // ensure loaded segments are mapped within the memory range `0x80100000` to `0x81000000`
        if (!(phdr.p_vaddr >= 0x80100000 && phdr.p_vaddr < 0x81000000 && (phdr.p_vaddr + phdr.p_memsz) >= 0x80100000 && (phdr.p_vaddr + phdr.p_memsz) < 0x81000000)) return -EINVAL;

        // load file segment into memory
        uint64_t segment_location = phdr.p_offset;
        if (uio_cntl(uio, FCNTL_SETPOS, &segment_location) < 0) return -EIO;
        if (uio_read(uio, (void *)phdr.p_vaddr, phdr.p_filesz) != phdr.p_filesz) return -EIO;
        if (phdr.p_filesz < phdr.p_memsz) {
            void* zero_start = (void *)(phdr.p_vaddr + phdr.p_filesz);
            size_t zero_size = phdr.p_memsz - phdr.p_filesz;
            memset(zero_start, 0, zero_size);
        }
    }
    // return the start of the entry point
    *eptr = (void (*)(void))ehdr.e_entry;

    return 0;
}