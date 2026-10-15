/* runtime loader: preserved standalone implementation. */
#define _GNU_SOURCE 1
#include "runtime.h"


/* ---- elf64 ---- */
#pragma push_macro("ETD2X_MRS_CNTPCT_EL0_BASE")
#pragma push_macro("ETD2X_MRS_CNTVCT_EL0_BASE")
#pragma push_macro("ETD2X_MRS_RT_MASK")
#pragma push_macro("ETD2X_MRS_SYSREG_MASK")
#pragma push_macro("ETD2X_PAGE_SIZE")









#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ETD2X_PAGE_SIZE 0x1000ULL

/* Android/Linux arm64 code may read CNTVCT_EL0 directly. Horizon userland
 * exposes the architectural physical counter used by libnx system ticks, but
 * a guest CNTVCT_EL0 access raises an exception before ETD2X can translate it.
 * Rewrite only that exact MRS sysreg encoding, preserving Rt, while the guest
 * image is still writable. This is platform-wide AArch64 compatibility, not a
 * package/function/offset patch. */
#define ETD2X_MRS_RT_MASK          0x0000001fu
#define ETD2X_MRS_SYSREG_MASK      0xffffffe0u
#define ETD2X_MRS_CNTVCT_EL0_BASE  0xd53be040u
#define ETD2X_MRS_CNTPCT_EL0_BASE  0xd53be020u

static uint64_t elf64_align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1ULL);
}

static uint64_t elf64_align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1ULL) & ~(alignment - 1ULL);
}

static bool elf64_range_valid(size_t file_size, uint64_t offset, uint64_t size) {
    return offset <= file_size && size <= file_size - offset;
}

/* Reads exactly size bytes at offset; large requests go straight to the
 * destination without a stdio bounce buffer. */
static bool elf64_read_exact_at(int fd, uint64_t offset, void *dst, size_t size) {
    if (lseek(fd, (off_t)offset, SEEK_SET) != (off_t)offset) return false;
    uint8_t *out = (uint8_t *)dst;
    while (size) {
        const ssize_t got = read(fd, out, size);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        out += got;
        size -= (size_t)got;
    }
    return true;
}

/* Only the ELF, program and section headers are kept from the file; PT_LOAD
 * contents are read directly into the load image (layout_load_image), so a
 * module never needs a second full-size copy of the file in memory. The
 * header block is stored as [Ehdr][Phdr table][Shdr table] in file_image. */
static Etd2xResult elf64_read_headers(int fd, Etd2xModule *module) {
    const off_t end = lseek(fd, 0, SEEK_END);
    if (end <= 0) return ETD2X_ERR_IO;
    module->file_size = (size_t)end;
    if (module->file_size < sizeof(Elf64_Ehdr)) return ETD2X_ERR_ELF_BOUNDS;

    Elf64_Ehdr eh;
    if (!elf64_read_exact_at(fd, 0, &eh, sizeof(eh))) return ETD2X_ERR_IO;
    const uint64_t ph_bytes = (uint64_t)eh.e_phnum * sizeof(Elf64_Phdr);
    const uint64_t sh_bytes = eh.e_shnum ? (uint64_t)eh.e_shnum * sizeof(Elf64_Shdr) : 0;
    /* Bounds are validated below; only read tables that lie inside the file. */
    const bool ph_ok = eh.e_phentsize == sizeof(Elf64_Phdr) && elf64_range_valid(module->file_size, eh.e_phoff, ph_bytes);
    const bool sh_ok = !eh.e_shnum ||
                       (eh.e_shentsize == sizeof(Elf64_Shdr) && elf64_range_valid(module->file_size, eh.e_shoff, sh_bytes));
    uint8_t *block = malloc(sizeof(eh) + (ph_ok ? ph_bytes : 0) + (sh_ok ? sh_bytes : 0));
    if (!block) return ETD2X_ERR_NO_MEMORY;
    memcpy(block, &eh, sizeof(eh));
    module->file_image = block;
    if ((ph_ok && ph_bytes && !elf64_read_exact_at(fd, eh.e_phoff, block + sizeof(eh), (size_t)ph_bytes)) ||
        (sh_ok && sh_bytes && !elf64_read_exact_at(fd, eh.e_shoff, block + sizeof(eh) + (ph_ok ? ph_bytes : 0), (size_t)sh_bytes)))
        return ETD2X_ERR_IO;
    return ETD2X_OK;
}

static Etd2xResult elf64_validate_headers(Etd2xModule *module) {
    if (module->file_size < sizeof(Elf64_Ehdr)) {
        return ETD2X_ERR_ELF_BOUNDS;
    }

    module->ehdr = (Elf64_Ehdr *)module->file_image;
    const Elf64_Ehdr *eh = module->ehdr;

    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) {
        return ETD2X_ERR_ELF_MAGIC;
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS64) {
        return ETD2X_ERR_ELF_CLASS;
    }
    if (eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        return ETD2X_ERR_ELF_ENDIAN;
    }
    if (eh->e_machine != EM_AARCH64) {
        return ETD2X_ERR_ELF_MACHINE;
    }
    if (eh->e_type != ET_DYN) {
        return ETD2X_ERR_ELF_TYPE;
    }
    if (eh->e_phentsize != sizeof(Elf64_Phdr) ||
        !elf64_range_valid(module->file_size, eh->e_phoff, (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr))) {
        return ETD2X_ERR_ELF_BOUNDS;
    }

    const size_t ph_bytes = (size_t)eh->e_phnum * sizeof(Elf64_Phdr);
    module->phdrs = (Elf64_Phdr *)((uint8_t *)module->file_image + sizeof(Elf64_Ehdr));

    if (eh->e_shnum != 0) {
        if (eh->e_shentsize != sizeof(Elf64_Shdr) ||
            !elf64_range_valid(module->file_size, eh->e_shoff, (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr))) {
            return ETD2X_ERR_ELF_BOUNDS;
        }
        module->shdrs = (Elf64_Shdr *)((uint8_t *)module->file_image + sizeof(Elf64_Ehdr) + ph_bytes);
    }

    module->state = ETD2X_MODULE_LOADED;
    return ETD2X_OK;
}

static Etd2xResult elf64_shared_object_kind(int fd, const Etd2xModule *module, bool *shared) {
    *shared = true;
    for (unsigned i = 0; i < module->ehdr->e_phnum; ++i) {
        const Elf64_Phdr *ph = &module->phdrs[i];
        if (ph->p_type != PT_DYNAMIC) continue;
        if (!elf64_range_valid(module->file_size, ph->p_offset, ph->p_filesz) ||
            ph->p_filesz % sizeof(Elf64_Dyn)) return ETD2X_ERR_ELF_BOUNDS;
        bool terminated = false;
        for (uint64_t off = 0; off < ph->p_filesz; off += sizeof(Elf64_Dyn)) {
            Elf64_Dyn dyn;
            if (!elf64_read_exact_at(fd, ph->p_offset + off, &dyn, sizeof(dyn))) return ETD2X_ERR_IO;
            if (dyn.d_tag == DT_NULL) { terminated = true; break; }
            /* DF_1_PIE is independent of the filename or ET_DYN header. */
            if (dyn.d_tag == DT_FLAGS_1 && (dyn.d_un.d_val & UINT64_C(0x08000000)))
                *shared = false;
        }
        if (!terminated) return ETD2X_ERR_ELF_BOUNDS;
    }
    return ETD2X_OK;
}

Etd2xResult etd2x_loader_probe_shared_object(const char *path, bool *out_shared) {
    if (!path || !out_shared) return ETD2X_ERR_ARGUMENT;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return ETD2X_ERR_IO;
    Etd2xModule module = {0};
    Etd2xResult rc = elf64_read_headers(fd, &module);
    if (rc == ETD2X_OK) rc = elf64_validate_headers(&module);
    bool shared = false;
    if (rc == ETD2X_OK) rc = elf64_shared_object_kind(fd, &module, &shared);
    else if (rc == ETD2X_ERR_ELF_TYPE && module.ehdr->e_type == ET_EXEC) rc = ETD2X_OK;
    free(module.file_image);
    close(fd);
    if (rc == ETD2X_OK) *out_shared = shared;
    return rc;
}

static Etd2xResult elf64_layout_load_image(Etd2xModule *module, int fd) {
    uint64_t min_vaddr = UINT64_MAX;
    uint64_t max_vaddr = 0;
    unsigned load_count = 0;

    for (uint16_t i = 0; i < module->ehdr->e_phnum; ++i) {
        const Elf64_Phdr *ph = &module->phdrs[i];
        if (ph->p_type != PT_LOAD) {
            continue;
        }

        ++load_count;
        if (ph->p_filesz > ph->p_memsz || !elf64_range_valid(module->file_size, ph->p_offset, ph->p_filesz)) {
            return ETD2X_ERR_ELF_BOUNDS;
        }
        if (ph->p_vaddr > UINT64_MAX - ph->p_memsz) {
            return ETD2X_ERR_ELF_BOUNDS;
        }

        const uint64_t begin = elf64_align_down(ph->p_vaddr, ETD2X_PAGE_SIZE);
        const uint64_t raw_end = ph->p_vaddr + ph->p_memsz;
        if (raw_end > UINT64_MAX - (ETD2X_PAGE_SIZE - 1ULL)) {
            return ETD2X_ERR_ELF_BOUNDS;
        }
        const uint64_t end = elf64_align_up(raw_end, ETD2X_PAGE_SIZE);
        if (begin < min_vaddr) min_vaddr = begin;
        if (end > max_vaddr) max_vaddr = end;
    }

    if (load_count == 0 || min_vaddr == UINT64_MAX || max_vaddr <= min_vaddr ||
        max_vaddr - min_vaddr > SIZE_MAX) {
        return ETD2X_ERR_ELF_NO_LOAD;
    }

    module->min_vaddr = min_vaddr;
    module->max_vaddr = max_vaddr;
    module->image_size = (size_t)(max_vaddr - min_vaddr);

    module->backing = memalign(ETD2X_PAGE_SIZE, module->image_size);
    if (!module->backing) {
        return ETD2X_ERR_NO_MEMORY;
    }

    /* Every byte not covered by a segment's file data is zero (BSS, padding
     * between segments). PT_LOAD entries are ascending by p_vaddr (ELF gABI),
     * so only the gaps are cleared; an out-of-order table clears everything
     * first. Later segments overwrite earlier ones where they overlap. */
    bool ascending = true;
    uint64_t previous_vaddr = 0;
    for (uint16_t i = 0; i < module->ehdr->e_phnum; ++i) {
        const Elf64_Phdr *ph = &module->phdrs[i];
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_vaddr < previous_vaddr) ascending = false;
        previous_vaddr = ph->p_vaddr;
    }
    if (!ascending) memset(module->backing, 0, module->image_size);

    uint8_t *const image = (uint8_t *)module->backing;
    size_t cleared_to = 0;
    for (uint16_t i = 0; i < module->ehdr->e_phnum; ++i) {
        const Elf64_Phdr *ph = &module->phdrs[i];
        if (ph->p_type != PT_LOAD) {
            continue;
        }

        const uint64_t offset = ph->p_vaddr - module->min_vaddr;
        if (offset > module->image_size || ph->p_memsz > module->image_size - offset) {
            return ETD2X_ERR_ELF_BOUNDS;
        }

        if (ascending && offset > cleared_to) memset(image + cleared_to, 0, (size_t)offset - cleared_to);
        if (ph->p_filesz && !elf64_read_exact_at(fd, ph->p_offset, image + offset, (size_t)ph->p_filesz)) {
            return ETD2X_ERR_IO;
        }
        const size_t data_end = (size_t)(offset + ph->p_filesz);
        if (data_end > cleared_to) cleared_to = data_end;
    }
    if (ascending && cleared_to < module->image_size)
        memset(image + cleared_to, 0, module->image_size - cleared_to);

    module->state = ETD2X_MODULE_LAYOUT_READY;
    ETD2X_LOGI("ELF layout: %u PT_LOAD segments, vaddr=%#llx..%#llx, image=%zu bytes",
               load_count,
               (unsigned long long)module->min_vaddr,
               (unsigned long long)module->max_vaddr,
               module->image_size);

    return ETD2X_OK;
}

static Etd2xResult elf64_reserve_runtime(Etd2xModule *module) {
    module->runtime_base = etd2x_vm_reserve(module->image_size, ETD2X_PAGE_SIZE, &module->reservation);

    if (!module->runtime_base || !module->reservation) {
        return ETD2X_ERR_VM_RESERVE;
    }
    if ((uintptr_t)module->runtime_base < module->min_vaddr) {
        return ETD2X_ERR_VM_RESERVE;
    }

    /* ELF load bias B: runtime address corresponding to guest virtual address 0. */
    module->load_bias = (uintptr_t)module->runtime_base - (uintptr_t)module->min_vaddr;
    module->state = ETD2X_MODULE_RESERVED;

    ETD2X_LOGI("Reserved guest runtime base=%p load_bias=%#llx",
               module->runtime_base, (unsigned long long)module->load_bias);
    return ETD2X_OK;
}

Etd2xResult etd2x_loader_patch_system_registers(Etd2xModule *module) {
    if (!module || !module->backing || !module->ehdr || !module->phdrs ||
        module->state < ETD2X_MODULE_LAYOUT_READY || module->mapped) {
        return ETD2X_ERR_ARGUMENT;
    }

    /* PF_X describes page permissions, not instruction boundaries: .rodata
     * often shares that segment. Scanning it corrupts embedded bytecode. */
    if (!module->shdrs || !module->ehdr->e_shnum) {
        ETD2X_LOGE("ELF instruction translation requires section boundaries: %s", module->path);
        return ETD2X_ERR_ELF_BOUNDS;
    }
    /* Validate every code range before rewriting any bytes. */
    for (uint16_t i = 0; i < module->ehdr->e_shnum; ++i) {
        const Elf64_Shdr *sh = &module->shdrs[i];
        if (!(sh->sh_flags & SHF_ALLOC) || !(sh->sh_flags & SHF_EXECINSTR) || !sh->sh_size) continue;
        if (sh->sh_type != SHT_PROGBITS || sh->sh_size > SIZE_MAX ||
            !elf64_range_valid(module->file_size, sh->sh_offset, sh->sh_size) ||
            !etd2x_module_backing_contains(module, sh->sh_addr, (size_t)sh->sh_size))
            return ETD2X_ERR_ELF_BOUNDS;
        bool executable_load = false;
        for (uint16_t j = 0; j < module->ehdr->e_phnum; ++j) {
            const Elf64_Phdr *ph = &module->phdrs[j];
            if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X) || sh->sh_addr < ph->p_vaddr) continue;
            const uint64_t offset = sh->sh_addr - ph->p_vaddr;
            if (offset <= ph->p_filesz && sh->sh_size <= ph->p_filesz - offset &&
                ph->p_offset <= UINT64_MAX - offset && sh->sh_offset == ph->p_offset + offset)
                executable_load = true;
        }
        if (!executable_load) return ETD2X_ERR_ELF_BOUNDS;
    }

    size_t patched_cntvct = 0, patched_svc = 0;
    for (uint16_t i = 0; i < module->ehdr->e_shnum; ++i) {
        const Elf64_Shdr *sh = &module->shdrs[i];
        if (!(sh->sh_flags & SHF_ALLOC) || !(sh->sh_flags & SHF_EXECINSTR) || sh->sh_size < sizeof(uint32_t)) continue;
        uint64_t first = sh->sh_addr;
        const uint64_t last = first + sh->sh_size;
        first = (first + 3u) & ~UINT64_C(3);
        if (last <= first || last - first < sizeof(uint32_t)) continue;

        /* Only complete instruction words inside allocated code sections. */
        const size_t word_count = (size_t)((last - first) / sizeof(uint32_t));
        if (!etd2x_module_backing_contains(module, first, word_count * sizeof(uint32_t))) {
            return ETD2X_ERR_ELF_BOUNDS;
        }
        /* backing is page aligned and first is 4-byte aligned. */
        uint32_t *words = (uint32_t *)etd2x_module_backing_address(module, first);
        if (!words) return ETD2X_ERR_ELF_BOUNDS;
        for (size_t k = 0; k < word_count; ++k) {
            const uint32_t insn = words[k];
            if (insn == ETD2X_LINUX_SVC_INSN) {
                words[k] = ETD2X_LINUX_SYSCALL_BRK;
                ++patched_svc;
                continue;
            }
            if ((insn & ETD2X_MRS_SYSREG_MASK) != ETD2X_MRS_CNTVCT_EL0_BASE) {
                continue;
            }

            words[k] = ETD2X_MRS_CNTPCT_EL0_BASE | (insn & ETD2X_MRS_RT_MASK);
            ++patched_cntvct;
            ETD2X_LOGCI("SYSREG",
                        "module=%s rewrite CNTVCT_EL0->CNTPCT_EL0 vaddr=0x%llx rt=x%u",
                        module->path, (unsigned long long)(first + (uint64_t)k * sizeof(uint32_t)),
                        insn & ETD2X_MRS_RT_MASK);
        }
    }

    if (patched_cntvct) {
        ETD2X_LOGCI("SYSREG", "module=%s Android timer-register rewrites=%zu",
                    module->path, patched_cntvct);
    }
    if (patched_svc)
        ETD2X_LOGCI("SYSCALL", "module=%s inline Linux syscall translations=%zu version=1",
                    module->path, patched_svc);
    return ETD2X_OK;
}

/* Instruction fetch is not coherent with data writes: executable segments
 * written through the backing alias must reach memory before the code alias
 * runs them. Data and read-only pages need no maintenance; the Cortex-A57
 * data cache is physically indexed, so both aliases see the same lines. */
static void elf64_sync_executable_segments(const Etd2xModule *module, bool code_alias) {
    for (uint16_t i = 0; i < module->ehdr->e_phnum; ++i) {
        const Elf64_Phdr *ph = &module->phdrs[i];
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X) || !ph->p_memsz) continue;
        const uint64_t first = elf64_align_down(ph->p_vaddr, ETD2X_PAGE_SIZE) - module->min_vaddr;
        const uint64_t last = elf64_align_up(ph->p_vaddr + ph->p_memsz, ETD2X_PAGE_SIZE) - module->min_vaddr;
        if (last > module->image_size || first >= last) continue;
        if (code_alias) armICacheInvalidate((uint8_t *)module->runtime_base + first, (size_t)(last - first));
        else armDCacheFlush((uint8_t *)module->backing + first, (size_t)(last - first));
    }
}

static Etd2xResult elf64_map_and_protect_image(Etd2xModule *module) {
    elf64_sync_executable_segments(module, false);

    Result rc = svcMapProcessCodeMemory(
        envGetOwnProcessHandle(),
        (u64)module->runtime_base,
        (u64)module->backing,
        module->image_size
    );
    if (R_FAILED(rc)) {
        ETD2X_LOGE("svcMapProcessCodeMemory failed: 0x%08X", rc);
        return ETD2X_ERR_VM_MAP;
    }
    module->mapped = true;

    const size_t page_count = module->image_size / ETD2X_PAGE_SIZE;
    uint8_t *exec_pages = calloc(page_count, 1);
    if (!exec_pages) {
        return ETD2X_ERR_NO_MEMORY;
    }

    for (uint16_t i = 0; i < module->ehdr->e_phnum; ++i) {
        const Elf64_Phdr *ph = &module->phdrs[i];
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X)) {
            continue;
        }

        const uint64_t first = elf64_align_down(ph->p_vaddr, ETD2X_PAGE_SIZE) - module->min_vaddr;
        const uint64_t last = elf64_align_up(ph->p_vaddr + ph->p_memsz, ETD2X_PAGE_SIZE) - module->min_vaddr;
        for (uint64_t off = first; off < last; off += ETD2X_PAGE_SIZE) {
            const size_t page = (size_t)(off / ETD2X_PAGE_SIZE);
            if (page < page_count) exec_pages[page] = 1;
        }
    }

    /*
     * Horizon disallows the unsafe W -> X transition for code memory. Mark
     * executable runs RX first, then mark every remaining page RW. Relocations
     * have already been applied to the writable backing alias before mapping.
     */
    for (int want_exec = 1; want_exec >= 0; --want_exec) {
        for (size_t page = 0; page < page_count;) {
            const bool executable = exec_pages[page] != 0;
            if ((int)executable != want_exec) {
                ++page;
                continue;
            }

            size_t end = page + 1;
            while (end < page_count && (exec_pages[end] != 0) == executable) {
                ++end;
            }

            void *address = (uint8_t *)module->runtime_base + page * ETD2X_PAGE_SIZE;
            const size_t size = (end - page) * ETD2X_PAGE_SIZE;
            const u32 perm = executable ? Perm_Rx : Perm_Rw;

            rc = svcSetProcessMemoryPermission(envGetOwnProcessHandle(), (u64)address, size, perm);
            if (R_FAILED(rc)) {
                ETD2X_LOGE("svcSetProcessMemoryPermission failed at %p: 0x%08X", address, rc);
                free(exec_pages);
                return ETD2X_ERR_VM_PERMISSIONS;
            }

            page = end;
        }
    }

    free(exec_pages);
    elf64_sync_executable_segments(module, true);

    module->state = ETD2X_MODULE_MAPPED;
    ETD2X_LOGI("Mapped guest image at %p", module->runtime_base);
    return ETD2X_OK;
}

static bool elf64_module_has_relocations(const Etd2xModule *module) {
    return module->dynamic.android_rela_count != 0 ||
           module->dynamic.rela_count != 0 ||
           module->dynamic.jmprel_count != 0 ||
           module->dynamic.relr_count != 0;
}

Etd2xResult etd2x_loader_prepare(const char *path, Etd2xModule *module) {
    if (!path || !module) {
        return ETD2X_ERR_ARGUMENT;
    }

    etd2x_module_init(module);
    snprintf(module->path, sizeof(module->path), "%s", path);

    Etd2xResult result;
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        ETD2X_LOGE("Could not open %s", path);
        result = ETD2X_ERR_IO;
        goto fail;
    }
    result = elf64_read_headers(fd, module);
    if (result == ETD2X_OK) {
        ETD2X_LOGI("Loaded %s (%zu bytes)", path, module->file_size);
        result = elf64_validate_headers(module);
    }
    if (result == ETD2X_OK) {
        bool shared = false;
        result = elf64_shared_object_kind(fd, module, &shared);
        if (result == ETD2X_OK && !shared) {
            ETD2X_LOGE("Cannot load ELF executable as a shared library: %s", path);
            result = ETD2X_ERR_ELF_TYPE;
        }
    }
    if (result == ETD2X_OK) result = elf64_layout_load_image(module, fd);
    close(fd);
    if (result != ETD2X_OK) goto fail;

    result = elf64_reserve_runtime(module);
    if (result != ETD2X_OK) goto fail;

    result = etd2x_dynamic_parse(module);
    if (result != ETD2X_OK) goto fail;

    return ETD2X_OK;

fail:
    module->state = ETD2X_MODULE_FAILED;
    ETD2X_LOGE("Loader prepare failed for %s: %s", path, etd2x_result_string(result));
    etd2x_module_unload(module);
    return result;
}

Etd2xResult etd2x_loader_relocate(Etd2xModule *module) {
    if (!module || module->state < ETD2X_MODULE_DYNAMIC_PARSED) {
        return ETD2X_ERR_ARGUMENT;
    }
    return etd2x_apply_relocations(module);
}

Etd2xResult etd2x_loader_map(Etd2xModule *module) {
    if (!module || module->state < ETD2X_MODULE_RELOCATED) {
        return ETD2X_ERR_ARGUMENT;
    }
    Etd2xResult result = etd2x_loader_patch_system_registers(module);
    if (result != ETD2X_OK) return result;
    /* Only generic architectural translations above alter guest instructions. */
    return elf64_map_and_protect_image(module);
}

Etd2xResult etd2x_loader_resolve_ifuncs(Etd2xModule *module) {
    if (!module || !module->mapped || module->state < ETD2X_MODULE_MAPPED) {
        return ETD2X_ERR_ARGUMENT;
    }
    return etd2x_apply_post_map_relocations(module);
}

Etd2xResult etd2x_loader_construct(Etd2xModule *module) {
    if (!module || module->state < ETD2X_MODULE_MAPPED) {
        return ETD2X_ERR_ARGUMENT;
    }
    Etd2xResult result = etd2x_dynamic_run_constructors(module);
    if (result == ETD2X_OK) {
        module->state = ETD2X_MODULE_FINALIZED;
    }
    return result;
}

static Etd2xResult elf64_loader_load_internal(const char *path, Etd2xModule *module, bool allow_relocations) {
    Etd2xResult result = etd2x_loader_prepare(path, module);
    if (result != ETD2X_OK) {
        return result;
    }

    if (!allow_relocations && elf64_module_has_relocations(module)) {
        ETD2X_LOGW("Stage 1 regression path rejects relocation-bearing guests");
        result = ETD2X_ERR_RELOCATIONS_REQUIRED;
        goto fail;
    }

    if (allow_relocations) {
        result = etd2x_loader_relocate(module);
        if (result != ETD2X_OK) goto fail;
    } else {
        module->state = ETD2X_MODULE_RELOCATED;
    }

    result = etd2x_loader_map(module);
    if (result != ETD2X_OK) goto fail;

    result = etd2x_loader_resolve_ifuncs(module);
    if (result != ETD2X_OK) goto fail;

    result = etd2x_loader_construct(module);
    if (result != ETD2X_OK) goto fail;

    return ETD2X_OK;

fail:
    module->state = ETD2X_MODULE_FAILED;
    ETD2X_LOGE("Loader failed for %s: %s", path, etd2x_result_string(result));
    etd2x_module_unload(module);
    return result;
}

Etd2xResult etd2x_loader_load(const char *path, Etd2xModule *module) {
    return elf64_loader_load_internal(path, module, true);
}

Etd2xResult etd2x_loader_load_stage1(const char *path, Etd2xModule *module) {
    return elf64_loader_load_internal(path, module, false);
}

Etd2xResult etd2x_loader_find_export(const Etd2xModule *module, const char *name, void **out_address) {
    if (!module || !name || !out_address || !module->dynamic.symtab_vaddr ||
        !module->dynamic.strtab_vaddr || !module->runtime_base || !module->mapped) {
        return ETD2X_ERR_ARGUMENT;
    }
    uintptr_t address = 0;
    Etd2xResult rc = etd2x_symbol_find_defined(module, name, &address);
    if (rc != ETD2X_OK) return rc;
    *out_address = (void *)address;
    ETD2X_LOGI("Resolved export %s -> %p", name, *out_address);
    return ETD2X_OK;
}

#pragma pop_macro("ETD2X_PAGE_SIZE")
#pragma pop_macro("ETD2X_MRS_SYSREG_MASK")
#pragma pop_macro("ETD2X_MRS_RT_MASK")
#pragma pop_macro("ETD2X_MRS_CNTVCT_EL0_BASE")
#pragma pop_macro("ETD2X_MRS_CNTPCT_EL0_BASE")


/* ---- dynamic ---- */
#pragma push_macro("DF_1_GLOBAL")
#pragma push_macro("DF_SYMBOLIC")
#pragma push_macro("DT_ANDROID_REL")
#pragma push_macro("DT_ANDROID_RELA")
#pragma push_macro("DT_ANDROID_RELASZ")
#pragma push_macro("DT_ANDROID_RELR")
#pragma push_macro("DT_ANDROID_RELRENT")
#pragma push_macro("DT_ANDROID_RELRSZ")
#pragma push_macro("DT_ANDROID_RELSZ")
#pragma push_macro("DT_FLAGS_1")
#pragma push_macro("DT_GNU_HASH")
#pragma push_macro("DT_RELR")
#pragma push_macro("DT_RELRENT")
#pragma push_macro("DT_RELRSZ")
#pragma push_macro("DT_VERDEF")
#pragma push_macro("DT_VERDEFNUM")
#pragma push_macro("DT_VERNEED")
#pragma push_macro("DT_VERNEEDNUM")
#pragma push_macro("DT_VERSYM")





#include <elf.h>
#include <limits.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#ifndef DT_RELR
#define DT_RELR 36
#endif
#ifndef DT_RELRSZ
#define DT_RELRSZ 35
#endif
#ifndef DT_RELRENT
#define DT_RELRENT 37
#endif
#ifndef DT_ANDROID_RELA
#define DT_ANDROID_RELA 0x60000011
#endif
#ifndef DT_ANDROID_RELASZ
#define DT_ANDROID_RELASZ 0x60000012
#endif
#ifndef DT_ANDROID_REL
#define DT_ANDROID_REL 0x6000000f
#endif
#ifndef DT_ANDROID_RELSZ
#define DT_ANDROID_RELSZ 0x60000010
#endif
#ifndef DT_ANDROID_RELR
#define DT_ANDROID_RELR 0x6fffe000
#endif
#ifndef DT_ANDROID_RELRSZ
#define DT_ANDROID_RELRSZ 0x6fffe001
#endif
#ifndef DT_ANDROID_RELRENT
#define DT_ANDROID_RELRENT 0x6fffe003
#endif
#ifndef DT_GNU_HASH
#define DT_GNU_HASH 0x6ffffef5
#endif
#ifndef DT_FLAGS_1
#define DT_FLAGS_1 0x6ffffffb
#endif

#ifndef DT_VERSYM
#define DT_VERSYM 0x6ffffff0
#endif
#ifndef DT_VERDEF
#define DT_VERDEF 0x6ffffffc
#endif
#ifndef DT_VERDEFNUM
#define DT_VERDEFNUM 0x6ffffffd
#endif
#ifndef DT_VERNEED
#define DT_VERNEED 0x6ffffffe
#endif
#ifndef DT_VERNEEDNUM
#define DT_VERNEEDNUM 0x6fffffff
#endif
#ifndef DF_SYMBOLIC
#define DF_SYMBOLIC 0x2
#endif
#ifndef DF_1_GLOBAL
#define DF_1_GLOBAL 0x2
#endif

typedef struct dynamic_Etd2xSlebReader {
    const uint8_t *data;
    size_t size;
    size_t offset;
} dynamic_Etd2xSlebReader;

static bool dynamic_read_sleb128(dynamic_Etd2xSlebReader *reader, int64_t *out) {
    if (!reader || !out) return false;
    uint64_t value = 0;
    unsigned shift = 0;
    uint8_t byte = 0;
    do {
        if (reader->offset >= reader->size || shift >= 64) return false;
        byte = reader->data[reader->offset++];
        value |= (uint64_t)(byte & 0x7fu) << shift;
        shift += 7u;
    } while ((byte & 0x80u) != 0);
    if (shift < 64u && (byte & 0x40u)) value |= (~UINT64_C(0)) << shift;
    *out = (int64_t)value;
    return true;
}

static Etd2xResult dynamic_decode_android_packed_rela(const uint8_t *blob, size_t blob_size,
                                               Elf64_Rela **out_rela, size_t *out_count) {
    enum {
        GROUPED_BY_INFO = 1,
        GROUPED_BY_OFFSET_DELTA = 2,
        GROUPED_BY_ADDEND = 4,
        GROUP_HAS_ADDEND = 8,
    };
    if (!blob || blob_size < 4 || !out_rela || !out_count || memcmp(blob, "APS2", 4) != 0)
        return ETD2X_ERR_RELOCATION_FORMAT;
    dynamic_Etd2xSlebReader reader = { blob + 4, blob_size - 4, 0 };
    int64_t count_i = 0, offset_i = 0;
    if (!dynamic_read_sleb128(&reader, &count_i) || !dynamic_read_sleb128(&reader, &offset_i) ||
        count_i < 0 || offset_i < 0 || (uint64_t)count_i > SIZE_MAX / sizeof(Elf64_Rela))
        return ETD2X_ERR_RELOCATION_FORMAT;
    const size_t count = (size_t)count_i;
    Elf64_Rela *rela = count ? calloc(count, sizeof(*rela)) : NULL;
    if (count && !rela) return ETD2X_ERR_NO_MEMORY;

    int64_t offset = offset_i;
    int64_t info = 0;
    int64_t addend = 0;
    size_t produced = 0;
    while (produced < count) {
        int64_t group_size_i = 0, flags_i = 0;
        if (!dynamic_read_sleb128(&reader, &group_size_i) || !dynamic_read_sleb128(&reader, &flags_i) ||
            group_size_i <= 0 || (uint64_t)group_size_i > count - produced || flags_i < 0) {
            free(rela); return ETD2X_ERR_RELOCATION_FORMAT;
        }
        const size_t group_size = (size_t)group_size_i;
        const uint64_t flags = (uint64_t)flags_i;
        if (flags & ~(uint64_t)(GROUPED_BY_INFO | GROUPED_BY_OFFSET_DELTA |
                                GROUPED_BY_ADDEND | GROUP_HAS_ADDEND)) {
            free(rela); return ETD2X_ERR_RELOCATION_FORMAT;
        }
        int64_t group_offset_delta = 0;
        if ((flags & GROUPED_BY_OFFSET_DELTA) && !dynamic_read_sleb128(&reader, &group_offset_delta)) {
            free(rela); return ETD2X_ERR_RELOCATION_FORMAT;
        }
        if ((flags & GROUPED_BY_INFO) && !dynamic_read_sleb128(&reader, &info)) {
            free(rela); return ETD2X_ERR_RELOCATION_FORMAT;
        }
        if ((flags & GROUP_HAS_ADDEND) && (flags & GROUPED_BY_ADDEND)) {
            int64_t delta = 0;
            if (!dynamic_read_sleb128(&reader, &delta) ||
                __builtin_add_overflow(addend, delta, &addend)) {
                free(rela); return ETD2X_ERR_RELOCATION_FORMAT;
            }
        } else if (!(flags & GROUP_HAS_ADDEND)) {
            addend = 0;
        }

        for (size_t i = 0; i < group_size; ++i) {
            int64_t delta = group_offset_delta;
            if (!(flags & GROUPED_BY_OFFSET_DELTA) && !dynamic_read_sleb128(&reader, &delta)) {
                free(rela); return ETD2X_ERR_RELOCATION_FORMAT;
            }
            if (__builtin_add_overflow(offset, delta, &offset) || offset < 0) {
                free(rela); return ETD2X_ERR_RELOCATION_FORMAT;
            }
            if (!(flags & GROUPED_BY_INFO) && !dynamic_read_sleb128(&reader, &info)) {
                free(rela); return ETD2X_ERR_RELOCATION_FORMAT;
            }
            if ((flags & GROUP_HAS_ADDEND) && !(flags & GROUPED_BY_ADDEND)) {
                int64_t addend_delta = 0;
                if (!dynamic_read_sleb128(&reader, &addend_delta) ||
                    __builtin_add_overflow(addend, addend_delta, &addend)) {
                    free(rela); return ETD2X_ERR_RELOCATION_FORMAT;
                }
            }
            rela[produced].r_offset = (Elf64_Addr)(uint64_t)offset;
            rela[produced].r_info = (Elf64_Xword)(uint64_t)info;
            rela[produced].r_addend = (Elf64_Sxword)addend;
            produced++;
        }
    }
    if (reader.offset > reader.size) { free(rela); return ETD2X_ERR_RELOCATION_FORMAT; }
    *out_rela = rela;
    *out_count = count;
    return ETD2X_OK;
}

static bool dynamic_read_u32(const Etd2xModule *module, uint64_t vaddr, uint32_t *out) {
    if (!out || !etd2x_module_backing_contains(module, vaddr, sizeof(uint32_t))) {
        return false;
    }
    memcpy(out, etd2x_module_backing_address(module, vaddr), sizeof(*out));
    return true;
}

static size_t dynamic_symbol_count_from_sysv_hash(const Etd2xModule *module, uint64_t hash_vaddr) {
    uint32_t nbucket = 0;
    uint32_t nchain = 0;
    if (!dynamic_read_u32(module, hash_vaddr, &nbucket) ||
        !dynamic_read_u32(module, hash_vaddr + sizeof(uint32_t), &nchain)) {
        return 0;
    }

    const uint64_t words = 2ULL + (uint64_t)nbucket + (uint64_t)nchain;
    if (words > SIZE_MAX / sizeof(uint32_t) ||
        !etd2x_module_backing_contains(module, hash_vaddr, (size_t)words * sizeof(uint32_t))) {
        return 0;
    }
    return (size_t)nchain;
}

static size_t dynamic_symbol_count_from_gnu_hash(const Etd2xModule *module, uint64_t gnu_hash_vaddr) {
    uint32_t nbuckets = 0;
    uint32_t symoffset = 0;
    uint32_t bloom_size = 0;
    uint32_t bloom_shift = 0;
    (void)bloom_shift;

    if (!dynamic_read_u32(module, gnu_hash_vaddr + 0, &nbuckets) ||
        !dynamic_read_u32(module, gnu_hash_vaddr + 4, &symoffset) ||
        !dynamic_read_u32(module, gnu_hash_vaddr + 8, &bloom_size) ||
        !dynamic_read_u32(module, gnu_hash_vaddr + 12, &bloom_shift)) {
        return 0;
    }

    const uint64_t bloom_bytes = (uint64_t)bloom_size * sizeof(Elf64_Xword);
    if (gnu_hash_vaddr > UINT64_MAX - 16ULL - bloom_bytes) {
        return 0;
    }
    const uint64_t buckets_vaddr = gnu_hash_vaddr + 16ULL + bloom_bytes;
    const uint64_t buckets_bytes = (uint64_t)nbuckets * sizeof(uint32_t);
    if (buckets_bytes > SIZE_MAX || buckets_vaddr > UINT64_MAX - buckets_bytes ||
        !etd2x_module_backing_contains(module, buckets_vaddr, (size_t)buckets_bytes)) {
        return 0;
    }

    uint32_t max_bucket = 0;
    for (uint32_t i = 0; i < nbuckets; ++i) {
        uint32_t bucket = 0;
        if (!dynamic_read_u32(module, buckets_vaddr + (uint64_t)i * sizeof(uint32_t), &bucket)) {
            return 0;
        }
        if (bucket > max_bucket) {
            max_bucket = bucket;
        }
    }

    if (max_bucket < symoffset) {
        return (size_t)symoffset;
    }

    const uint64_t chains_vaddr = buckets_vaddr + buckets_bytes;
    const uint64_t chain_index = (uint64_t)max_bucket - symoffset;
    if (chain_index > (UINT64_MAX - chains_vaddr) / sizeof(uint32_t)) {
        return 0;
    }
    uint64_t chain_vaddr = chains_vaddr + chain_index * sizeof(uint32_t);

    /* Bound the walk by the guest image instead of trusting malformed input. */
    size_t walked = 0;
    while (walked < 1U << 20) {
        uint32_t chain = 0;
        if (!dynamic_read_u32(module, chain_vaddr, &chain)) {
            return 0;
        }
        ++walked;
        if (chain & 1U) {
            const uint64_t count = (uint64_t)max_bucket + walked;
            return count <= SIZE_MAX ? (size_t)count : 0;
        }
        chain_vaddr += sizeof(uint32_t);
    }

    return 0;
}

static size_t dynamic_symbol_count_from_sections(const Etd2xModule *module) {
    if (!module->shdrs || !module->ehdr) {
        return 0;
    }
    for (uint16_t i = 0; i < module->ehdr->e_shnum; ++i) {
        const Elf64_Shdr *sh = &module->shdrs[i];
        if (sh->sh_type == SHT_DYNSYM && sh->sh_entsize == sizeof(Elf64_Sym)) {
            return (size_t)(sh->sh_size / sizeof(Elf64_Sym));
        }
    }
    return 0;
}

Etd2xResult etd2x_dynamic_parse(Etd2xModule *module) {
    if (!module || !module->ehdr || !module->backing) {
        return ETD2X_ERR_ARGUMENT;
    }

    const Elf64_Phdr *dynamic_ph = NULL;
    const Elf64_Phdr *tls_ph = NULL;
    for (uint16_t i = 0; i < module->ehdr->e_phnum; ++i) {
        if (module->phdrs[i].p_type == PT_DYNAMIC) dynamic_ph = &module->phdrs[i];
        else if (module->phdrs[i].p_type == PT_TLS) tls_ph = &module->phdrs[i];
    }
    if (!dynamic_ph) {
        return ETD2X_ERR_DYNAMIC_MISSING;
    }
    if (dynamic_ph->p_memsz < sizeof(Elf64_Dyn) ||
        !etd2x_module_backing_contains(module, dynamic_ph->p_vaddr, (size_t)dynamic_ph->p_memsz)) {
        return ETD2X_ERR_DYNAMIC_INVALID;
    }

    const Elf64_Dyn *entries = (const Elf64_Dyn *)etd2x_module_backing_address(module, dynamic_ph->p_vaddr);
    const size_t entry_count = (size_t)(dynamic_ph->p_memsz / sizeof(Elf64_Dyn));

    uint64_t strtab_vaddr = 0, strsz = 0;
    uint64_t symtab_vaddr = 0, syment = 0;
    uint64_t rela_vaddr = 0, relasz = 0, relaent = 0;
    uint64_t jmprel_vaddr = 0, pltrelsz = 0, pltrel = 0;
    uint64_t rel_vaddr = 0, relsz = 0;
    uint64_t relr_vaddr = 0, relrsz = 0, relrent = 0;
    uint64_t android_rela_vaddr = 0, android_relasz = 0;
    uint64_t android_rel_vaddr = 0, android_relsz = 0;
    uint64_t android_relr_vaddr = 0, android_relrsz = 0, android_relrent = 0;
    uint64_t hash_vaddr = 0, gnu_hash_vaddr = 0;
    uint64_t init_vaddr = 0, init_array_vaddr = 0, init_arraysz = 0;
    uint64_t fini_vaddr = 0, fini_array_vaddr = 0, fini_arraysz = 0;
    uint64_t versym_vaddr = 0, verneed_vaddr = 0, verneednum = 0;
    uint64_t verdef_vaddr = 0, verdefnum = 0;
    uint64_t dt_flags = 0, dt_flags_1 = 0;
    bool dt_symbolic = false;
    uint32_t soname_offset = 0;
    size_t needed_count = 0;

    for (size_t i = 0; i < entry_count; ++i) {
        const Elf64_Dyn *dyn = &entries[i];
        if (dyn->d_tag == DT_NULL) {
            break;
        }
        switch (dyn->d_tag) {
            case DT_STRTAB: strtab_vaddr = dyn->d_un.d_ptr; break;
            case DT_STRSZ: strsz = dyn->d_un.d_val; break;
            case DT_SYMTAB: symtab_vaddr = dyn->d_un.d_ptr; break;
            case DT_SYMENT: syment = dyn->d_un.d_val; break;
            case DT_RELA: rela_vaddr = dyn->d_un.d_ptr; break;
            case DT_RELASZ: relasz = dyn->d_un.d_val; break;
            case DT_RELAENT: relaent = dyn->d_un.d_val; break;
            case DT_REL: rel_vaddr = dyn->d_un.d_ptr; break;
            case DT_RELSZ: relsz = dyn->d_un.d_val; break;
            case DT_JMPREL: jmprel_vaddr = dyn->d_un.d_ptr; break;
            case DT_PLTRELSZ: pltrelsz = dyn->d_un.d_val; break;
            case DT_PLTREL: pltrel = dyn->d_un.d_val; break;
            case DT_HASH: hash_vaddr = dyn->d_un.d_ptr; break;
            case DT_GNU_HASH: gnu_hash_vaddr = dyn->d_un.d_ptr; break;
            case DT_INIT: init_vaddr = dyn->d_un.d_ptr; break;
            case DT_INIT_ARRAY: init_array_vaddr = dyn->d_un.d_ptr; break;
            case DT_INIT_ARRAYSZ: init_arraysz = dyn->d_un.d_val; break;
            case DT_FINI: fini_vaddr = dyn->d_un.d_ptr; break;
            case DT_FINI_ARRAY: fini_array_vaddr = dyn->d_un.d_ptr; break;
            case DT_FINI_ARRAYSZ: fini_arraysz = dyn->d_un.d_val; break;
            case DT_VERSYM: versym_vaddr = dyn->d_un.d_ptr; break;
            case DT_VERNEED: verneed_vaddr = dyn->d_un.d_ptr; break;
            case DT_VERNEEDNUM: verneednum = dyn->d_un.d_val; break;
            case DT_VERDEF: verdef_vaddr = dyn->d_un.d_ptr; break;
            case DT_VERDEFNUM: verdefnum = dyn->d_un.d_val; break;
            case DT_FLAGS: dt_flags = dyn->d_un.d_val; break;
            case DT_FLAGS_1: dt_flags_1 = dyn->d_un.d_val; break;
            case DT_SYMBOLIC: dt_symbolic = true; break;
            case DT_NEEDED: ++needed_count; break;
            case DT_SONAME:
                if (dyn->d_un.d_val > UINT32_MAX) return ETD2X_ERR_DYNAMIC_INVALID;
                soname_offset = (uint32_t)dyn->d_un.d_val;
                break;
            case DT_RELR: relr_vaddr = dyn->d_un.d_ptr; break;
            case DT_RELRSZ: relrsz = dyn->d_un.d_val; break;
            case DT_RELRENT: relrent = dyn->d_un.d_val; break;
            case DT_ANDROID_RELA: android_rela_vaddr = dyn->d_un.d_ptr; break;
            case DT_ANDROID_RELASZ: android_relasz = dyn->d_un.d_val; break;
            case DT_ANDROID_REL: android_rel_vaddr = dyn->d_un.d_ptr; break;
            case DT_ANDROID_RELSZ: android_relsz = dyn->d_un.d_val; break;
            case DT_ANDROID_RELR: android_relr_vaddr = dyn->d_un.d_ptr; break;
            case DT_ANDROID_RELRSZ: android_relrsz = dyn->d_un.d_val; break;
            case DT_ANDROID_RELRENT: android_relrent = dyn->d_un.d_val; break;
            default: break;
        }
    }

    if (rel_vaddr || relsz || android_rel_vaddr || android_relsz) {
        ETD2X_LOGCE("DYNAMIC", "REL relocation table present; AArch64 Stage 2 supports RELA only");
        return ETD2X_ERR_RELOCATION_FORMAT;
    }

    if (!strtab_vaddr || !strsz || !symtab_vaddr) {
        return ETD2X_ERR_SYMBOL_TABLE;
    }
    if (syment && syment != sizeof(Elf64_Sym)) {
        return ETD2X_ERR_DYNAMIC_INVALID;
    }
    if (strsz > SIZE_MAX || !etd2x_module_backing_contains(module, strtab_vaddr, (size_t)strsz) ||
        !etd2x_module_backing_contains(module, symtab_vaddr, sizeof(Elf64_Sym))) {
        return ETD2X_ERR_DYNAMIC_INVALID;
    }

    module->dynamic.strtab_vaddr = strtab_vaddr;
    module->dynamic.strsz = (size_t)strsz;
    module->dynamic.symtab_vaddr = symtab_vaddr;
    module->dynamic.syment = sizeof(Elf64_Sym);
    module->dynamic.hash_vaddr = hash_vaddr;
    module->dynamic.gnu_hash_vaddr = gnu_hash_vaddr;
    module->dynamic.versym_vaddr = versym_vaddr;
    module->dynamic.verneed_vaddr = verneed_vaddr;
    module->dynamic.verneed_count = (size_t)verneednum;
    module->dynamic.verdef_vaddr = verdef_vaddr;
    module->dynamic.verdef_count = (size_t)verdefnum;

    size_t symbol_count = 0;
    if (hash_vaddr) {
        symbol_count = dynamic_symbol_count_from_sysv_hash(module, hash_vaddr);
    }
    if (!symbol_count && gnu_hash_vaddr) {
        symbol_count = dynamic_symbol_count_from_gnu_hash(module, gnu_hash_vaddr);
    }
    if (!symbol_count) {
        symbol_count = dynamic_symbol_count_from_sections(module);
    }
    if (!symbol_count) {
        return ETD2X_ERR_SYMBOL_TABLE;
    }
    if (symbol_count > SIZE_MAX / sizeof(Elf64_Sym) ||
        !etd2x_module_backing_contains(module, symtab_vaddr, symbol_count * sizeof(Elf64_Sym))) {
        return ETD2X_ERR_DYNAMIC_INVALID;
    }
    module->dynamic.symbol_count = symbol_count;

    if (versym_vaddr) {
        if (symbol_count > SIZE_MAX / sizeof(Elf64_Half) ||
            !etd2x_module_backing_contains(module, versym_vaddr, symbol_count * sizeof(Elf64_Half))) {
            return ETD2X_ERR_DYNAMIC_INVALID;
        }
    }
    if ((verneed_vaddr == 0) != (verneednum == 0) || (verdef_vaddr == 0) != (verdefnum == 0)) {
        return ETD2X_ERR_DYNAMIC_INVALID;
    }

    if (soname_offset >= module->dynamic.strsz && soname_offset != 0) {
        return ETD2X_ERR_DYNAMIC_INVALID;
    }
    module->dynamic.soname_offset = soname_offset;
    module->dynamic.flags = dt_flags;
    module->dynamic.flags_1 = dt_flags_1;
    module->dynamic.symbolic = dt_symbolic || ((dt_flags & DF_SYMBOLIC) != 0);

    if (needed_count) {
        if (needed_count > SIZE_MAX / sizeof(uint32_t)) {
            return ETD2X_ERR_NO_MEMORY;
        }
        module->dynamic.needed_name_offsets = calloc(needed_count, sizeof(uint32_t));
        if (!module->dynamic.needed_name_offsets) {
            return ETD2X_ERR_NO_MEMORY;
        }
        module->dynamic.needed_count = needed_count;

        size_t needed_index = 0;
        for (size_t i = 0; i < entry_count; ++i) {
            const Elf64_Dyn *dyn = &entries[i];
            if (dyn->d_tag == DT_NULL) break;
            if (dyn->d_tag != DT_NEEDED) continue;
            if (dyn->d_un.d_val >= module->dynamic.strsz || dyn->d_un.d_val > UINT32_MAX ||
                needed_index >= needed_count) {
                return ETD2X_ERR_DYNAMIC_INVALID;
            }
            module->dynamic.needed_name_offsets[needed_index++] = (uint32_t)dyn->d_un.d_val;
        }
        if (needed_index != needed_count) {
            return ETD2X_ERR_DYNAMIC_INVALID;
        }
    }

    if (tls_ph) {
        size_t align = (size_t)(tls_ph->p_align ? tls_ph->p_align : 1u);
        if ((align & (align - 1u)) != 0 || tls_ph->p_filesz > tls_ph->p_memsz ||
            tls_ph->p_memsz > SIZE_MAX || tls_ph->p_filesz > SIZE_MAX ||
            !etd2x_module_backing_contains(module, tls_ph->p_vaddr, (size_t)tls_ph->p_filesz)) {
            return ETD2X_ERR_DYNAMIC_INVALID;
        }
        module->tls.present = true;
        module->tls.vaddr = tls_ph->p_vaddr;
        module->tls.file_size = (size_t)tls_ph->p_filesz;
        module->tls.mem_size = (size_t)tls_ph->p_memsz;
        module->tls.alignment = align;
        ETD2X_LOGCI("TLS", "module=%s PT_TLS vaddr=%#llx filesz=%zu memsz=%zu align=%zu",
                    module->path, (unsigned long long)module->tls.vaddr,
                    module->tls.file_size, module->tls.mem_size, module->tls.alignment);
    }

    if (android_relasz) {
        if (!android_rela_vaddr || android_relasz > SIZE_MAX ||
            !etd2x_module_backing_contains(module, android_rela_vaddr, (size_t)android_relasz)) {
            return ETD2X_ERR_RELOCATION_FORMAT;
        }
        const uint8_t *packed = (const uint8_t *)etd2x_module_backing_address(module, android_rela_vaddr);
        Etd2xResult packed_rc = dynamic_decode_android_packed_rela(
            packed, (size_t)android_relasz,
            &module->dynamic.android_rela, &module->dynamic.android_rela_count);
        if (packed_rc != ETD2X_OK) {
            ETD2X_LOGCE("DYNAMIC", "invalid DT_ANDROID_RELA/APS2 table module=%s", module->path);
            return packed_rc;
        }
        ETD2X_LOGCI("DYNAMIC", "decoded APS2 relocations module=%s count=%zu bytes=%llu",
                    module->path, module->dynamic.android_rela_count,
                    (unsigned long long)android_relasz);
    }

    if (relasz) {
        if (!rela_vaddr || (relaent && relaent != sizeof(Elf64_Rela)) || relasz % sizeof(Elf64_Rela) != 0 ||
            relasz > SIZE_MAX || !etd2x_module_backing_contains(module, rela_vaddr, (size_t)relasz)) {
            return ETD2X_ERR_RELOCATION_FORMAT;
        }
        module->dynamic.rela = (Elf64_Rela *)etd2x_module_backing_address(module, rela_vaddr);
        module->dynamic.rela_count = (size_t)(relasz / sizeof(Elf64_Rela));
    }

    if (pltrelsz) {
        if (!jmprel_vaddr || pltrel != DT_RELA || pltrelsz % sizeof(Elf64_Rela) != 0 ||
            pltrelsz > SIZE_MAX || !etd2x_module_backing_contains(module, jmprel_vaddr, (size_t)pltrelsz)) {
            return ETD2X_ERR_RELOCATION_FORMAT;
        }
        module->dynamic.jmprel = (Elf64_Rela *)etd2x_module_backing_address(module, jmprel_vaddr);
        module->dynamic.jmprel_count = (size_t)(pltrelsz / sizeof(Elf64_Rela));
    }

    if (relrsz) {
        if (!relr_vaddr || (relrent && relrent != sizeof(Elf64_Xword)) || relrsz % sizeof(Elf64_Xword) != 0 ||
            relrsz > SIZE_MAX || !etd2x_module_backing_contains(module, relr_vaddr, (size_t)relrsz)) {
            return ETD2X_ERR_RELOCATION_FORMAT;
        }
        module->dynamic.relr = (Elf64_Xword *)etd2x_module_backing_address(module, relr_vaddr);
        module->dynamic.relr_count = (size_t)(relrsz / sizeof(Elf64_Xword));
        module->dynamic.relr_is_android = false;
    } else if (android_relrsz) {
        if (!android_relr_vaddr || (android_relrent && android_relrent != sizeof(Elf64_Xword)) ||
            android_relrsz % sizeof(Elf64_Xword) != 0 || android_relrsz > SIZE_MAX ||
            !etd2x_module_backing_contains(module, android_relr_vaddr, (size_t)android_relrsz)) {
            return ETD2X_ERR_RELOCATION_FORMAT;
        }
        module->dynamic.relr = (Elf64_Xword *)etd2x_module_backing_address(module, android_relr_vaddr);
        module->dynamic.relr_count = (size_t)(android_relrsz / sizeof(Elf64_Xword));
        module->dynamic.relr_is_android = true;
    }

    if (init_array_vaddr || init_arraysz) {
        if (!init_array_vaddr || init_arraysz % sizeof(uintptr_t) != 0 || init_arraysz > SIZE_MAX ||
            !etd2x_module_backing_contains(module, init_array_vaddr, (size_t)init_arraysz)) {
            return ETD2X_ERR_CONSTRUCTOR;
        }
        module->dynamic.init_array_vaddr = init_array_vaddr;
        module->dynamic.init_array_count = (size_t)(init_arraysz / sizeof(uintptr_t));
    }
    if (init_vaddr) {
        if (!etd2x_module_backing_contains(module, init_vaddr, 1)) {
            return ETD2X_ERR_CONSTRUCTOR;
        }
        module->dynamic.init_vaddr = init_vaddr;
    }
    if (fini_array_vaddr || fini_arraysz) {
        if (!fini_array_vaddr || fini_arraysz % sizeof(uintptr_t) != 0 || fini_arraysz > SIZE_MAX ||
            !etd2x_module_backing_contains(module, fini_array_vaddr, (size_t)fini_arraysz)) {
            return ETD2X_ERR_CONSTRUCTOR;
        }
        module->dynamic.fini_array_vaddr = fini_array_vaddr;
        module->dynamic.fini_array_count = (size_t)(fini_arraysz / sizeof(uintptr_t));
    }
    if (fini_vaddr) {
        if (!etd2x_module_backing_contains(module, fini_vaddr, 1)) return ETD2X_ERR_CONSTRUCTOR;
        module->dynamic.fini_vaddr = fini_vaddr;
    }

    ETD2X_LOGCI("DYNAMIC",
        "symbols=%zu needed=%zu rela=%zu android_rela=%zu plt=%zu relr=%zu init_array=%zu fini_array=%zu versions=%u tls=%u flags=%#llx flags1=%#llx symbolic=%u global=%u",
        module->dynamic.symbol_count,
        module->dynamic.needed_count,
        module->dynamic.rela_count,
        module->dynamic.android_rela_count,
        module->dynamic.jmprel_count,
        module->dynamic.relr_count,
        module->dynamic.init_array_count,
        module->dynamic.fini_array_count,
        module->dynamic.versym_vaddr ? 1u : 0u,
        module->tls.present ? 1u : 0u,
        (unsigned long long)module->dynamic.flags,
        (unsigned long long)module->dynamic.flags_1,
        module->dynamic.symbolic ? 1u : 0u,
        (module->dynamic.flags_1 & DF_1_GLOBAL) ? 1u : 0u);

    module->state = ETD2X_MODULE_DYNAMIC_PARSED;
    return ETD2X_OK;
}


static bool dynamic_g_constructor_trace_enabled = false;

void etd2x_dynamic_set_constructor_trace(bool enabled) {
    dynamic_g_constructor_trace_enabled = enabled;
}

static int64_t dynamic_sign_extend_26(uint32_t value) {
    int64_t v = (int64_t)(value & 0x03ffffffu);
    if (v & (1LL << 25)) v -= (1LL << 26);
    return v;
}

static int64_t dynamic_sign_extend_21(uint32_t value) {
    int64_t v = (int64_t)(value & 0x001fffffu);
    if (v & (1LL << 20)) v -= (1LL << 21);
    return v;
}

typedef struct dynamic_Etd2xCtorPltInfo {
    bool valid;
    uint64_t got_vaddr;
    size_t reloc_index;
    const char *symbol_name;
    uintptr_t resolved_address;
    const Etd2xModule *provider;
} dynamic_Etd2xCtorPltInfo;

/* Constructor tracing runs only after guest code memory has been mapped.  Do
 * not treat the module's broad reserved range, or stale pre-map backing
 * pointers, as safe diagnostic memory.  Every tracer read must go through a
 * mapped guest runtime alias first. */
static bool dynamic_trace_mapped_guest_contains(const Etd2xModule *module,
                                        uintptr_t address, size_t size) {
    return module && module->mapped && module->state >= ETD2X_MODULE_MAPPED &&
           etd2x_module_runtime_contains(module, address, size);
}

/* DT_JMPREL is parsed before mapping, so Etd2xDynamicInfo::jmprel intentionally
 * points at the writable backing image.  Once mapped, that backing pointer is
 * not a valid source for constructor diagnostics on Horizon.  Convert the
 * validated backing offset to the mapped runtime alias before reading it. */
static const Elf64_Rela *dynamic_trace_mapped_jmprel_entry(const Etd2xModule *module,
                                                   size_t index) {
    if (!module || !module->backing || !module->dynamic.jmprel ||
        index >= module->dynamic.jmprel_count || !module->mapped ||
        module->state < ETD2X_MODULE_MAPPED) {
        return NULL;
    }

    const uintptr_t backing_begin = (uintptr_t)module->backing;
    const uintptr_t table_backing = (uintptr_t)module->dynamic.jmprel;
    if (table_backing < backing_begin) return NULL;

    const uintptr_t table_offset_u = table_backing - backing_begin;
    if (table_offset_u > module->image_size) return NULL;
    const size_t table_offset = (size_t)table_offset_u;

    if (index > SIZE_MAX / sizeof(Elf64_Rela)) return NULL;
    const size_t entry_delta = index * sizeof(Elf64_Rela);
    if (entry_delta > module->image_size - table_offset) return NULL;
    const size_t entry_offset = table_offset + entry_delta;
    if (sizeof(Elf64_Rela) > module->image_size - entry_offset) return NULL;

    const uintptr_t runtime = (uintptr_t)module->runtime_base + entry_offset;
    if (!dynamic_trace_mapped_guest_contains(module, runtime, sizeof(Elf64_Rela))) return NULL;
    return (const Elf64_Rela *)runtime;
}

/* Decode the canonical AArch64 ELF PLT entry emitted by lld/ld:
 *   adrp x16, ...
 *   ldr  x17, [x16, #imm]
 *   add  x16, x16, #imm
 *   br   x17
 * and map its GOT slot back to the DT_JMPREL symbol.  This is diagnostic only;
 * it never alters the GOT or guest code. */
static bool dynamic_trace_decode_plt(const Etd2xModule *module, uintptr_t target,
                             dynamic_Etd2xCtorPltInfo *out) {
    if (!module || !out || !dynamic_trace_mapped_guest_contains(module, target, 16)) return false;
    memset(out, 0, sizeof(*out));

    uint32_t w[4] = {0, 0, 0, 0};
    memcpy(w, (const void *)target, sizeof(w));
    if ((w[0] & 0x9f00001fu) != 0x90000010u || /* ADRP x16 */
        (w[1] & 0xffc003ffu) != 0xf9400211u || /* LDR x17,[x16,#imm] */
        w[3] != 0xd61f0220u) {                 /* BR x17 */
        return false;
    }

    const uint32_t immlo = (w[0] >> 29) & 0x3u;
    const uint32_t immhi = (w[0] >> 5) & 0x7ffffu;
    const int64_t page_delta = dynamic_sign_extend_21((immhi << 2) | immlo) << 12;
    const uintptr_t page = target & ~(uintptr_t)0xfffu;
    uintptr_t got_page = page;
    if (page_delta < 0) {
        const uint64_t magnitude = (uint64_t)(-page_delta);
        if ((uint64_t)got_page < magnitude) return false;
        got_page -= (uintptr_t)magnitude;
    } else {
        const uint64_t delta = (uint64_t)page_delta;
        if (delta > UINTPTR_MAX - got_page) return false;
        got_page += (uintptr_t)delta;
    }
    const uintptr_t ldr_offset = (uintptr_t)(((w[1] >> 10) & 0xfffu) << 3);
    if (ldr_offset > UINTPTR_MAX - got_page) return false;
    const uintptr_t got_runtime = got_page + ldr_offset;
    if (!dynamic_trace_mapped_guest_contains(module, got_runtime, sizeof(uintptr_t)) ||
        got_runtime < module->load_bias) {
        return false;
    }
    const uint64_t got_vaddr = (uint64_t)(got_runtime - module->load_bias);

    for (size_t i = 0; i < module->dynamic.jmprel_count; ++i) {
        const Elf64_Rela *rela = dynamic_trace_mapped_jmprel_entry(module, i);
        if (!rela) return false;
        if (rela->r_offset != got_vaddr || ELF64_R_TYPE(rela->r_info) != R_AARCH64_JUMP_SLOT) {
            continue;
        }
        const size_t sym_index = (size_t)ELF64_R_SYM(rela->r_info);
        const Elf64_Sym *sym = NULL;
        const char *name = NULL;
        if (etd2x_symbol_get(module, sym_index, &sym, &name) != ETD2X_OK || !name || !name[0]) {
            return false;
        }

        uintptr_t resolved = 0;
        const Etd2xModule *provider = NULL;
        if (module->linker && sym->st_shndx == SHN_UNDEF) {
            (void)etd2x_linker_probe_import(module, name, &resolved, &provider);
        } else if (sym->st_shndx != SHN_UNDEF) {
            resolved = module->load_bias + (uintptr_t)sym->st_value;
            provider = module;
        }
        if (provider && resolved && !dynamic_trace_mapped_guest_contains(provider, resolved, 1)) {
            /* A guest provider must resolve back into one of its mapped guest
             * ranges.  Reject stale/corrupt diagnostic metadata rather than
             * following or printing it.  Compat exports have provider==NULL. */
            resolved = 0;
            provider = NULL;
        }
        if (!resolved) {
            /* GOT reads are permitted only after the same mapped-guest check
             * above; never dereference a computed slot merely because it lies
             * numerically near the module. */
            memcpy(&resolved, (const void *)got_runtime, sizeof(resolved));
        }

        out->valid = true;
        out->got_vaddr = got_vaddr;
        out->reloc_index = i;
        out->symbol_name = name;
        out->resolved_address = resolved;
        out->provider = provider;
        return true;
    }
    return false;
}

static void dynamic_trace_nearest_defined_symbol(const Etd2xModule *module, uint64_t target_off,
                                         const char **out_name, uint64_t *out_value) {
    const char *nearest_name = NULL;
    uint64_t nearest_value = 0;
    for (size_t si = 0; si < module->dynamic.symbol_count; ++si) {
        const Elf64_Sym *sym = NULL;
        const char *name = NULL;
        if (etd2x_symbol_get(module, si, &sym, &name) != ETD2X_OK ||
            sym->st_shndx == SHN_UNDEF || !name || !name[0] ||
            sym->st_value > target_off || sym->st_value < nearest_value) {
            continue;
        }
        nearest_value = sym->st_value;
        nearest_name = name;
    }
    if (out_name) *out_name = nearest_name;
    if (out_value) *out_value = nearest_value;
}

/* Constructor tracing is intentionally observational.  Scan far enough to
 * capture late direct calls too: real global-init functions are not always
 * tiny compiler wrappers.  PLT targets are decoded back to their dynamic
 * symbol so a hardware crash identifies the Android/runtime boundary instead
 * of only an opaque trampoline address. */
static void dynamic_trace_constructor_calls(const Etd2xModule *module, size_t index,
                                    uintptr_t ctor) {
    if (!dynamic_g_constructor_trace_enabled || !module) return;

    const uint64_t ctor_off = (uint64_t)(ctor - module->load_bias);
    ETD2X_LOGCI("CTORTRACE", "module=%s index=%zu ctor_offset=0x%llx",
                module->path, index, (unsigned long long)ctor_off);

    const size_t scan_bytes = 0x240;
    const size_t max_targets = 24;
    size_t targets = 0;

    for (size_t off = 0; off + sizeof(uint32_t) <= scan_bytes; off += 4) {
        const uintptr_t pc = ctor + off;
        if (!dynamic_trace_mapped_guest_contains(module, pc, sizeof(uint32_t))) break;
        uint32_t insn = 0;
        memcpy(&insn, (const void *)pc, sizeof(insn));
        if ((insn & 0xfc000000u) != 0x94000000u) continue; /* BL imm26 */

        const int64_t disp = dynamic_sign_extend_26(insn) << 2;
        const uintptr_t target = (uintptr_t)((int64_t)pc + disp);
        if (!dynamic_trace_mapped_guest_contains(module, target, 0x40)) continue;

        const uint64_t target_off = (uint64_t)(target - module->load_bias);
        dynamic_Etd2xCtorPltInfo plt = {0};
        if (dynamic_trace_decode_plt(module, target, &plt)) {
            ETD2X_LOGCI("CTORTRACE",
                        "module=%s index=%zu bl_at=+0x%zx target_offset=0x%llx plt=%s jmprel=%zu got=0x%llx resolved=%p provider=%s",
                        module->path, index, off, (unsigned long long)target_off,
                        plt.symbol_name ? plt.symbol_name : "<unknown>", plt.reloc_index,
                        (unsigned long long)plt.got_vaddr, (void *)plt.resolved_address,
                        plt.provider ? plt.provider->path : "<compat/unknown>");
        } else {
            const char *nearest_name = NULL;
            uint64_t nearest_value = 0;
            dynamic_trace_nearest_defined_symbol(module, target_off, &nearest_name, &nearest_value);
            if (nearest_name) {
                ETD2X_LOGCI("CTORTRACE",
                            "module=%s index=%zu bl_at=+0x%zx target_offset=0x%llx nearest=%s+0x%llx",
                            module->path, index, off, (unsigned long long)target_off,
                            nearest_name, (unsigned long long)(target_off - nearest_value));
            } else {
                ETD2X_LOGCI("CTORTRACE",
                            "module=%s index=%zu bl_at=+0x%zx target_offset=0x%llx",
                            module->path, index, off, (unsigned long long)target_off);
            }
        }

        /* Show the instructions that prepare x0.. arguments and the BL itself.
         * This is especially useful for NULL-receiver crashes after a late
         * constructor call. */
        const size_t before = off >= 0x20 ? 0x20 : off;
        for (size_t row_off = off - before; row_off <= off && row_off + 0x10 <= scan_bytes; row_off += 0x10) {
            const uintptr_t row_pc = ctor + row_off;
            if (!dynamic_trace_mapped_guest_contains(module, row_pc, 0x10)) break;
            uint32_t cw[4] = {0, 0, 0, 0};
            memcpy(cw, (const void *)row_pc, sizeof(cw));
            ETD2X_LOGCI("CTORTRACE",
                        "callsite+0x%zx: %08x %08x %08x %08x",
                        row_off, cw[0], cw[1], cw[2], cw[3]);
        }

        for (size_t row = 0; row < 0x40; row += 0x10) {
            uint32_t w[4] = {0, 0, 0, 0};
            memcpy(w, (const void *)(target + row), sizeof(w));
            ETD2X_LOGCI("CTORTRACE",
                        "code+0x%llx: %08x %08x %08x %08x",
                        (unsigned long long)(target_off + row),
                        w[0], w[1], w[2], w[3]);
        }
        if (++targets >= max_targets) {
            ETD2X_LOGCW("CTORTRACE", "module=%s index=%zu target cap reached=%zu",
                        module->path, index, max_targets);
            break;
        }
    }

    ETD2X_LOGCI("CTORTRACE", "module=%s index=%zu direct_calls=%zu scan=0x%zx",
                module->path, index, targets, scan_bytes);

    /* Persist the trace before entering guest code so a hardware exception
     * cannot erase the exact late call target and arguments that led to it. */
    etd2x_diag_flush();
}

Etd2xResult etd2x_dynamic_run_constructors(Etd2xModule *module) {
    if (!module || !module->mapped || module->state < ETD2X_MODULE_MAPPED) {
        return ETD2X_ERR_ARGUMENT;
    }

    typedef void (*dynamic_CtorFn)(void);
    size_t called = 0;

    if (module->dynamic.init_vaddr) {
        void *address = etd2x_module_runtime_address(module, module->dynamic.init_vaddr);
        if (!address) {
            return ETD2X_ERR_CONSTRUCTOR;
        }
        ETD2X_LOGCI("CTOR", "DT_INIT -> %p", address);
        ((dynamic_CtorFn)address)();
        ++called;
    }

    if (module->dynamic.init_array_count) {
        uintptr_t *array = (uintptr_t *)etd2x_module_runtime_address(module, module->dynamic.init_array_vaddr);
        if (!array) {
            return ETD2X_ERR_CONSTRUCTOR;
        }
        for (size_t i = 0; i < module->dynamic.init_array_count; ++i) {
            const uintptr_t target = array[i];
            if (target == 0 || target == UINTPTR_MAX) {
                continue;
            }
            if (!etd2x_module_runtime_contains(module, target, 1)) {
                ETD2X_LOGCE("CTOR", "init_array[%zu]=%p is outside the module", i, (void *)target);
                return ETD2X_ERR_CONSTRUCTOR;
            }
            const uint64_t target_off = (uint64_t)(target - module->load_bias);
            ETD2X_LOGCI("CTOR", "module=%s init_array[%zu] -> %p offset=0x%llx",
                        module->path, i, (void *)target,
                        (unsigned long long)target_off);
            dynamic_trace_constructor_calls(module, i, target);
            ((dynamic_CtorFn)target)();
            ETD2X_LOGCI("CTOR", "module=%s init_array[%zu] returned", module->path, i);
            ++called;
        }
    }

    ETD2X_LOGCI("CTOR", "module=%s constructors called=%zu", module->path, called);
    module->state = ETD2X_MODULE_CONSTRUCTED;
    return ETD2X_OK;
}


Etd2xResult etd2x_dynamic_run_destructors(Etd2xModule *module) {
    if (!module || !module->mapped || module->state < ETD2X_MODULE_CONSTRUCTED) {
        return ETD2X_ERR_ARGUMENT;
    }

    typedef void (*dynamic_DtorFn)(void);
    size_t called = 0;

    /* Bionic runs DT_FINI_ARRAY in reverse order, then DT_FINI. */
    if (module->dynamic.fini_array_count) {
        uintptr_t *array = (uintptr_t *)etd2x_module_runtime_address(
            module, module->dynamic.fini_array_vaddr);
        if (!array) return ETD2X_ERR_CONSTRUCTOR;
        for (size_t i = module->dynamic.fini_array_count; i > 0; --i) {
            const uintptr_t target = array[i - 1u];
            if (target == 0 || target == UINTPTR_MAX) continue;
            if (!etd2x_module_runtime_contains(module, target, 1)) {
                ETD2X_LOGCE("DTOR", "fini_array[%zu]=%p is outside the module", i - 1u, (void *)target);
                return ETD2X_ERR_CONSTRUCTOR;
            }
            ETD2X_LOGCI("DTOR", "module=%s fini_array[%zu] -> %p", module->path, i - 1u, (void *)target);
            ((dynamic_DtorFn)target)();
            ++called;
        }
    }

    if (module->dynamic.fini_vaddr) {
        void *address = etd2x_module_runtime_address(module, module->dynamic.fini_vaddr);
        if (!address) return ETD2X_ERR_CONSTRUCTOR;
        ETD2X_LOGCI("DTOR", "module=%s DT_FINI -> %p", module->path, address);
        ((dynamic_DtorFn)address)();
        ++called;
    }

    ETD2X_LOGCI("DTOR", "module=%s destructors called=%zu", module->path, called);
    return ETD2X_OK;
}


static const char *dynamic_dynamic_string_view(const Etd2xModule *module, uint32_t offset) {
    if (!module || !module->dynamic.strtab_vaddr || offset >= module->dynamic.strsz) {
        return NULL;
    }
    const uint64_t vaddr = module->dynamic.strtab_vaddr + (uint64_t)offset;
    const size_t remaining = module->dynamic.strsz - (size_t)offset;
    const char *name = NULL;
    if (module->mapped) {
        name = (const char *)etd2x_module_runtime_address(module, vaddr);
        if (!name || !etd2x_module_runtime_contains(module, (uintptr_t)name, remaining)) {
            return NULL;
        }
    } else {
        if (!etd2x_module_backing_contains(module, vaddr, remaining)) {
            return NULL;
        }
        name = (const char *)etd2x_module_backing_address(module, vaddr);
    }
    return memchr(name, '\0', remaining) ? name : NULL;
}

Etd2xResult etd2x_dynamic_get_needed(const Etd2xModule *module, size_t index, const char **out_name) {
    if (!module || !out_name || index >= module->dynamic.needed_count ||
        !module->dynamic.needed_name_offsets) {
        return ETD2X_ERR_ARGUMENT;
    }
    const char *name = dynamic_dynamic_string_view(module, module->dynamic.needed_name_offsets[index]);
    if (!name || !name[0]) {
        return ETD2X_ERR_DYNAMIC_INVALID;
    }
    *out_name = name;
    return ETD2X_OK;
}

Etd2xResult etd2x_dynamic_get_soname(const Etd2xModule *module, const char **out_name) {
    if (!module || !out_name) {
        return ETD2X_ERR_ARGUMENT;
    }
    if (!module->dynamic.soname_offset) {
        return ETD2X_ERR_SYMBOL_NOT_FOUND;
    }
    const char *name = dynamic_dynamic_string_view(module, module->dynamic.soname_offset);
    if (!name || !name[0]) {
        return ETD2X_ERR_DYNAMIC_INVALID;
    }
    *out_name = name;
    return ETD2X_OK;
}

#pragma pop_macro("DT_VERSYM")
#pragma pop_macro("DT_VERNEEDNUM")
#pragma pop_macro("DT_VERNEED")
#pragma pop_macro("DT_VERDEFNUM")
#pragma pop_macro("DT_VERDEF")
#pragma pop_macro("DT_RELRSZ")
#pragma pop_macro("DT_RELRENT")
#pragma pop_macro("DT_RELR")
#pragma pop_macro("DT_GNU_HASH")
#pragma pop_macro("DT_FLAGS_1")
#pragma pop_macro("DT_ANDROID_RELSZ")
#pragma pop_macro("DT_ANDROID_RELRSZ")
#pragma pop_macro("DT_ANDROID_RELRENT")
#pragma pop_macro("DT_ANDROID_RELR")
#pragma pop_macro("DT_ANDROID_RELASZ")
#pragma pop_macro("DT_ANDROID_RELA")
#pragma pop_macro("DT_ANDROID_REL")
#pragma pop_macro("DF_SYMBOLIC")
#pragma pop_macro("DF_1_GLOBAL")


/* ---- module ---- */



#include <stdlib.h>
#include <string.h>

void etd2x_module_init(Etd2xModule *module) {
    if (module) {
        memset(module, 0, sizeof(*module));
        module->state = ETD2X_MODULE_EMPTY;
    }
}

bool etd2x_module_backing_contains(const Etd2xModule *module, uint64_t elf_vaddr, size_t size) {
    if (!module || !module->backing || elf_vaddr < module->min_vaddr || elf_vaddr > module->max_vaddr) {
        return false;
    }

    const uint64_t offset = elf_vaddr - module->min_vaddr;
    if (offset > module->image_size) {
        return false;
    }
    return size <= module->image_size - (size_t)offset;
}

void *etd2x_module_backing_address(const Etd2xModule *module, uint64_t elf_vaddr) {
    if (!etd2x_module_backing_contains(module, elf_vaddr, 1)) {
        return NULL;
    }
    return (void *)((uintptr_t)module->backing + (uintptr_t)(elf_vaddr - module->min_vaddr));
}

void *etd2x_module_runtime_address(const Etd2xModule *module, uint64_t elf_vaddr) {
    if (!module || !module->runtime_base || elf_vaddr < module->min_vaddr || elf_vaddr >= module->max_vaddr) {
        return NULL;
    }

    return (void *)((uintptr_t)module->runtime_base + (uintptr_t)(elf_vaddr - module->min_vaddr));
}

bool etd2x_module_runtime_contains(const Etd2xModule *module, uintptr_t runtime_address, size_t size) {
    if (!module || !module->runtime_base) {
        return false;
    }
    const uintptr_t begin = (uintptr_t)module->runtime_base;
    const uintptr_t end = begin + module->image_size;
    if (runtime_address < begin || runtime_address > end) {
        return false;
    }
    return size <= (size_t)(end - runtime_address);
}

void etd2x_module_unload(Etd2xModule *module) {
    if (!module) {
        return;
    }

    if (module->mapped && module->runtime_base && module->backing && module->image_size) {
        svcUnmapProcessCodeMemory(
            envGetOwnProcessHandle(),
            (u64)module->runtime_base,
            (u64)module->backing,
            module->image_size
        );
    }

    etd2x_vm_release(module->reservation);
    module->reservation = NULL;

    free(module->dynamic.needed_name_offsets);
    free(module->dynamic.android_rela);
    free(module->backing);
    free(module->file_image);
    etd2x_module_init(module);
}



/* ---- linker ---- */
#pragma push_macro("DF_1_GLOBAL")
#pragma push_macro("DF_1_NODELETE")







#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#ifndef DF_1_GLOBAL
#define DF_1_GLOBAL 0x2
#endif
#ifndef DF_1_NODELETE
#define DF_1_NODELETE 0x8
#endif

struct Etd2xLinkerNode {
    Etd2xModule module;
    bool discovering;
    bool discovered;
    bool relocated;
    bool constructing;
    bool constructed;
    bool global_visible;
    bool rtld_global;
    bool pinned;
    bool destructed;
    size_t direct_refcount;
    Etd2xResult construct_failure;
    Etd2xResult load_failure;

    /* Android/Bionic relocates a dlopen root against a local group containing
     * that root plus its DT_NEEDED closure (breadth-first).  A dependency that
     * was already loaded keeps the scope from its first load.  Java package
     * probing may discover several future System.loadLibrary roots up front,
     * so discovery alone must not make those unrelated roots visible. */
    struct Etd2xLinkerNode *relocation_scope_root;
    struct Etd2xLinkerNode **local_group;
    size_t local_group_count;
};

/* Bionic serializes dlopen()/System.loadLibrary() and the DT_INIT/
 * DT_INIT_ARRAY calls they trigger under one recursive loader mutex. A second
 * thread opening a module that is still being constructed therefore blocks
 * until construction completes, while a constructor that re-enters dlopen on
 * the same thread proceeds. */
static pthread_once_t linker_g_linker_lock_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t linker_g_linker_lock;

static void linker_linker_lock_init(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&linker_g_linker_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

static void linker_linker_lock(void) {
    pthread_once(&linker_g_linker_lock_once, linker_linker_lock_init);
    pthread_mutex_lock(&linker_g_linker_lock);
}

static void linker_linker_unlock(void) {
    pthread_mutex_unlock(&linker_g_linker_lock);
}

static uint64_t linker_linker_monotonic_ms(void) {
    struct timespec ts = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000) + (uint64_t)ts.tv_nsec / UINT64_C(1000000);
}

static const char *linker_path_basename(const char *path) {
    if (!path) return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool linker_name_matches_module(const Etd2xModule *module, const char *name) {
    if (!module || !name) return false;
    if (strcmp(linker_path_basename(module->path), name) == 0) return true;

    const char *soname = NULL;
    return etd2x_dynamic_get_soname(module, &soname) == ETD2X_OK && strcmp(soname, name) == 0;
}

static bool linker_version_allows_provider(const Etd2xModule *module, const Etd2xSymbolVersion *version) {
    return !version || !version->provider || !version->provider[0] ||
           linker_name_matches_module(module, version->provider);
}

static Etd2xLinkerNode *linker_find_node_by_needed(const Etd2xLinker *linker, const char *name) {
    if (!linker || !name) return NULL;
    for (size_t i = 0; i < linker->node_count; ++i) {
        Etd2xLinkerNode *node = linker->nodes[i];
        if (node && linker_name_matches_module(&node->module, name)) return node;
    }
    return NULL;
}

static Etd2xLinkerNode *linker_find_node_by_path(const Etd2xLinker *linker, const char *path) {
    if (!linker || !path) return NULL;
    for (size_t i = 0; i < linker->node_count; ++i) {
        Etd2xLinkerNode *node = linker->nodes[i];
        if (node && strcmp(node->module.path, path) == 0) return node;
    }
    return NULL;
}

static Etd2xResult linker_append_node(Etd2xLinker *linker, Etd2xLinkerNode *node) {
    if (linker->node_count == linker->node_capacity) {
        size_t next = linker->node_capacity ? linker->node_capacity * 2 : 8;
        if (next < linker->node_capacity || next > SIZE_MAX / sizeof(*linker->nodes)) {
            return ETD2X_ERR_NO_MEMORY;
        }
        Etd2xLinkerNode **grown = realloc(linker->nodes, next * sizeof(*linker->nodes));
        if (!grown) return ETD2X_ERR_NO_MEMORY;
        linker->nodes = grown;
        linker->node_capacity = next;
    }
    linker->nodes[linker->node_count++] = node;
    return ETD2X_OK;
}

static void linker_free_node(Etd2xLinkerNode *node) {
    if (!node) return;
    free(node->local_group);
    node->local_group = NULL;
    node->local_group_count = 0;
    etd2x_module_unload(&node->module);
    free(node);
}

static bool linker_has_path_separator(const char *name) {
    return name && (strchr(name, '/') != NULL || strchr(name, '\\') != NULL);
}

static Etd2xResult linker_dependency_path(const Etd2xLinker *linker, const char *needed,
                                   char *out, size_t out_size) {
    if (!linker || !needed || !needed[0] || !out || out_size == 0 || linker_has_path_separator(needed)) {
        return ETD2X_ERR_DEPENDENCY_INVALID;
    }
    const int n = snprintf(out, out_size, "%s%s%s",
                           linker->library_dir,
                           linker->library_dir[0] ? "/" : "",
                           needed);
    if (n < 0 || (size_t)n >= out_size) return ETD2X_ERR_DEPENDENCY_INVALID;
    return ETD2X_OK;
}

static Etd2xResult linker_discover_module(Etd2xLinker *linker, const char *path,
                                   Etd2xLinkerNode **out_node) {
    Etd2xLinkerNode *existing = linker_find_node_by_path(linker, path);
    if (existing) {
        if (out_node) *out_node = existing;
        return ETD2X_OK;
    }

    Etd2xLinkerNode *node = calloc(1, sizeof(*node));
    if (!node) return ETD2X_ERR_NO_MEMORY;
    node->discovering = true;

    const uint64_t prepare_begin_ms = linker_linker_monotonic_ms();
    Etd2xResult rc = etd2x_loader_prepare(path, &node->module);
    const uint64_t prepare_elapsed_ms = linker_linker_monotonic_ms() - prepare_begin_ms;
    ETD2X_LOGCI("PERF", "linker prepare module=%s elapsedMs=%llu",
                linker_path_basename(path), (unsigned long long)prepare_elapsed_ms);
    if (rc != ETD2X_OK) {
        free(node);
        return rc;
    }
    node->module.linker = linker;

    rc = linker_append_node(linker, node);
    if (rc != ETD2X_OK) {
        etd2x_module_unload(&node->module);
        free(node);
        return rc;
    }

    const char *soname = NULL;
    if (etd2x_dynamic_get_soname(&node->module, &soname) != ETD2X_OK) {
        soname = linker_path_basename(path);
    }
    ETD2X_LOGCI("LINKER", "discovered module=%s soname=%s needed=%zu",
                path, soname, node->module.dynamic.needed_count);

    for (size_t i = 0; i < node->module.dynamic.needed_count; ++i) {
        const char *needed = NULL;
        rc = etd2x_dynamic_get_needed(&node->module, i, &needed);
        if (rc != ETD2X_OK) return rc;

        if (etd2x_compat_is_virtual_library(needed)) {
            ETD2X_LOGCI("COMPAT", "virtual DT_NEEDED module=%s dependency=%s",
                        linker_path_basename(path), needed);
            continue;
        }

        Etd2xLinkerNode *dep = linker_find_node_by_needed(linker, needed);
        if (dep) {
            ETD2X_LOGCI("LINKER", "DT_NEEDED module=%s dependency=%s (already discovered)",
                        linker_path_basename(path), needed);
            continue;
        }

        char dep_path[sizeof(node->module.path)];
        rc = linker_dependency_path(linker, needed, dep_path, sizeof(dep_path));
        if (rc != ETD2X_OK) return rc;

        ETD2X_LOGCI("LINKER", "DT_NEEDED module=%s dependency=%s path=%s",
                    linker_path_basename(path), needed, dep_path);
        rc = linker_discover_module(linker, dep_path, &dep);
        if (rc != ETD2X_OK) {
            ETD2X_LOGCE("LINKER", "dependency=%s required_by=%s failed: %s",
                        needed, path, etd2x_result_string(rc));
            return rc == ETD2X_ERR_IO ? ETD2X_ERR_DEPENDENCY_NOT_FOUND : rc;
        }
    }

    node->discovering = false;
    node->discovered = true;
    if (out_node) *out_node = node;
    return ETD2X_OK;
}

static Etd2xResult linker_derive_library_dir(Etd2xLinker *linker, const char *root_path) {
    if (linker->library_dir[0]) return ETD2X_OK;
    const char *slash = strrchr(root_path, '/');
    if (!slash) return ETD2X_OK;
    const size_t len = (size_t)(slash - root_path);
    if (len >= sizeof(linker->library_dir)) return ETD2X_ERR_DEPENDENCY_INVALID;
    memcpy(linker->library_dir, root_path, len);
    linker->library_dir[len] = '\0';
    return ETD2X_OK;
}


static Etd2xResult linker_construct_node(Etd2xLinker *linker, Etd2xLinkerNode *node) {
    if (!linker || !node) return ETD2X_ERR_ARGUMENT;
    if (node->constructed) return ETD2X_OK;
    /* Constructors have observable side effects and may have partially run.
       Never re-run them after a failure, and never report the module as
       initialized either. */
    if (node->construct_failure != ETD2X_OK) return node->construct_failure;
    if (node->constructing) {
        /* A dependency cycle is allowed once the complete graph is relocated
           and mapped. Avoid recursive re-entry; the outer frame will finish. */
        return ETD2X_OK;
    }

    node->constructing = true;
    Etd2xResult rc = ETD2X_OK;
    for (size_t i = 0; i < node->module.dynamic.needed_count; ++i) {
        const char *needed = NULL;
        rc = etd2x_dynamic_get_needed(&node->module, i, &needed);
        if (rc != ETD2X_OK) goto fail;
        if (etd2x_compat_is_virtual_library(needed)) continue;
        Etd2xLinkerNode *dep = linker_find_node_by_needed(linker, needed);
        if (!dep) {
            rc = ETD2X_ERR_DEPENDENCY_NOT_FOUND;
            goto fail;
        }
        rc = linker_construct_node(linker, dep);
        if (rc != ETD2X_OK) goto fail;
    }

    const uint64_t construct_begin_ms = linker_linker_monotonic_ms();
    rc = etd2x_loader_construct(&node->module);
    const uint64_t construct_elapsed_ms = linker_linker_monotonic_ms() - construct_begin_ms;
    ETD2X_LOGCI("PERF", "linker constructors module=%s elapsedMs=%llu",
                linker_path_basename(node->module.path), (unsigned long long)construct_elapsed_ms);
    if (rc != ETD2X_OK) goto fail;
    node->constructing = false;
    node->constructed = true;
    node->destructed = false;
    return ETD2X_OK;

fail:
    node->constructing = false;
    node->construct_failure = rc;
    return rc;
}

void etd2x_linker_init(Etd2xLinker *linker, const char *library_dir) {
    if (!linker) return;
    memset(linker, 0, sizeof(*linker));
    if (library_dir) snprintf(linker->library_dir, sizeof(linker->library_dir), "%s", library_dir);
}

void etd2x_linker_unload(Etd2xLinker *linker) {
    if (!linker) return;
    etd2x_compat_set_active_linker(NULL);
    for (size_t i = linker->node_count; i > 0; --i) {
        Etd2xLinkerNode *node = linker->nodes[i - 1];
        if (!node) continue;
        linker_free_node(node);
    }
    free(linker->nodes);
    memset(linker, 0, sizeof(*linker));
}

static bool linker_group_contains(Etd2xLinkerNode *const *group, size_t count,
                           const Etd2xLinkerNode *node) {
    for (size_t i = 0; i < count; ++i)
        if (group[i] == node) return true;
    return false;
}

/* Build the Bionic-style local lookup group for one dlopen/System.loadLibrary
 * root.  The ordering is breadth-first: root, direct DT_NEEDED entries, then
 * their dependencies.  Virtual Android system libraries remain provided by
 * ETD2X's compat namespace and therefore are not linker nodes here. */
static Etd2xResult linker_build_local_group(Etd2xLinker *linker, Etd2xLinkerNode *root) {
    if (!linker || !root) return ETD2X_ERR_ARGUMENT;
    if (root->local_group) return ETD2X_OK;
    if (linker->node_count == 0) return ETD2X_ERR_DEPENDENCY_NOT_FOUND;

    Etd2xLinkerNode **group = calloc(linker->node_count, sizeof(*group));
    if (!group) return ETD2X_ERR_NO_MEMORY;
    size_t count = 1;
    group[0] = root;

    for (size_t cursor = 0; cursor < count; ++cursor) {
        Etd2xLinkerNode *node = group[cursor];
        for (size_t i = 0; i < node->module.dynamic.needed_count; ++i) {
            const char *needed = NULL;
            Etd2xResult rc = etd2x_dynamic_get_needed(&node->module, i, &needed);
            if (rc != ETD2X_OK) {
                free(group);
                return rc;
            }
            if (etd2x_compat_is_virtual_library(needed)) continue;

            Etd2xLinkerNode *dep = linker_find_node_by_needed(linker, needed);
            if (!dep) {
                free(group);
                return ETD2X_ERR_DEPENDENCY_NOT_FOUND;
            }
            if (!linker_group_contains(group, count, dep)) group[count++] = dep;
        }
    }

    root->local_group = group;
    root->local_group_count = count;
    ETD2X_LOGCI("LINKER", "local group root=%s members=%zu policy=bionic-bfs",
                linker_path_basename(root->module.path), count);
    return ETD2X_OK;
}

/* Discovery reserves addresses for every package candidate, but Android does
 * not relocate/map a System.loadLibrary root until that load actually occurs.
 * Finalizing lazily preserves real load order and prevents an unrelated
 * pre-discovered root from becoming a relocation provider. */
static Etd2xResult linker_finalize_local_group(Etd2xLinker *linker, Etd2xLinkerNode *root) {
    if (!linker || !root) return ETD2X_ERR_ARGUMENT;
    if (root->load_failure != ETD2X_OK) return root->load_failure;

    Etd2xResult rc = linker_build_local_group(linker, root);
    if (rc != ETD2X_OK) goto fail;

    for (size_t i = 0; i < root->local_group_count; ++i) {
        Etd2xLinkerNode *node = root->local_group[i];
        if (node->relocated) continue;
        if (node->load_failure != ETD2X_OK) {
            rc = node->load_failure;
            goto fail;
        }
        node->relocation_scope_root = root;
        rc = etd2x_bionic_tls_register_module(&node->module);
        if (rc != ETD2X_OK) {
            node->load_failure = rc;
            goto fail;
        }
    }

    for (size_t i = 0; i < root->local_group_count; ++i) {
        Etd2xLinkerNode *node = root->local_group[i];
        if (node->relocated) continue;
        const uint64_t module_begin_ms = linker_linker_monotonic_ms();
        rc = etd2x_loader_relocate(&node->module);
        ETD2X_LOGCI("PERF", "linker local relocate root=%s module=%s elapsedMs=%llu",
                    linker_path_basename(root->module.path), linker_path_basename(node->module.path),
                    (unsigned long long)(linker_linker_monotonic_ms() - module_begin_ms));
        if (rc != ETD2X_OK) {
            node->load_failure = rc;
            goto fail;
        }
        node->relocated = true;
    }

    /* Relocations may patch words inside the PT_TLS initialization image.
     * Refresh this loader thread's arena only after every template is final. */
    etd2x_bionic_tls_activate_current();

    for (size_t i = 0; i < root->local_group_count; ++i) {
        Etd2xLinkerNode *node = root->local_group[i];
        if (node->module.mapped) continue;
        const uint64_t module_begin_ms = linker_linker_monotonic_ms();
        rc = etd2x_loader_map(&node->module);
        ETD2X_LOGCI("PERF", "linker local map root=%s module=%s elapsedMs=%llu",
                    linker_path_basename(root->module.path), linker_path_basename(node->module.path),
                    (unsigned long long)(linker_linker_monotonic_ms() - module_begin_ms));
        if (rc != ETD2X_OK) {
            node->load_failure = rc;
            goto fail;
        }
    }

    /* Android/Bionic resolves GNU IFUNC/IRELATIVE only after ordinary
     * relocations and executable mapping. All members of this local group are
     * mapped before any resolver runs, so resolvers can safely inspect/call
     * dependency code just like the platform linker. */
    for (size_t i = 0; i < root->local_group_count; ++i) {
        Etd2xLinkerNode *node = root->local_group[i];
        const uint64_t module_begin_ms = linker_linker_monotonic_ms();
        rc = etd2x_loader_resolve_ifuncs(&node->module);
        ETD2X_LOGCI("PERF", "linker IFUNC resolve root=%s module=%s elapsedMs=%llu",
                    linker_path_basename(root->module.path), linker_path_basename(node->module.path),
                    (unsigned long long)(linker_linker_monotonic_ms() - module_begin_ms));
        if (rc != ETD2X_OK) {
            node->load_failure = rc;
            goto fail;
        }
    }

    /* DF_1_NODELETE is an ELF object-lifetime contract, not a caller policy.
     * Once such a DSO is actually loaded, keep its constructors/state alive
     * across dlclose just as Android's linker does for NODELETE objects. */
    for (size_t i = 0; i < root->local_group_count; ++i) {
        Etd2xLinkerNode *node = root->local_group[i];
        if (!node || !node->module.mapped || node->pinned) continue;
        if ((node->module.dynamic.flags_1 & DF_1_NODELETE) == 0) continue;
        node->pinned = true;
        ETD2X_LOGCI("LINKER", "pin module=%s reason=DF_1_NODELETE",
                    linker_path_basename(node->module.path));
    }

    /* Android API 23+ maintains a process-wide global group in addition to
     * each dlopen root's BFS local group. A DSO linked with -z global carries
     * DF_1_GLOBAL and becomes visible to later relocations once loaded. The
     * Java bootstrap may have pre-discovered unrelated roots, so visibility is
     * deliberately tied to actual mapping rather than discovery. */
    for (size_t i = 0; i < root->local_group_count; ++i) {
        Etd2xLinkerNode *node = root->local_group[i];
        if (!node->module.mapped || node->global_visible) continue;
        if ((node->module.dynamic.flags_1 & DF_1_GLOBAL) == 0 && !node->rtld_global) continue;
        node->global_visible = true;
        ETD2X_LOGCI("LINKER", "global group add module=%s reason=%s",
                    linker_path_basename(node->module.path),
                    node->rtld_global ? "RTLD_GLOBAL" : "DF_1_GLOBAL");
    }
    return ETD2X_OK;

fail:
    root->load_failure = rc;
    ETD2X_LOGCE("LINKER", "local group finalize failed root=%s: %s",
                linker_path_basename(root->module.path), etd2x_result_string(rc));
    return rc;
}

size_t etd2x_linker_module_count(const Etd2xLinker *linker) {
    return linker ? linker->node_count : 0;
}

size_t etd2x_linker_compat_bind_count(const Etd2xLinker *linker) {
    return linker ? linker->compat_bind_count : 0;
}

const Etd2xModule *etd2x_linker_module_at(const Etd2xLinker *linker, size_t index) {
    if (!linker || index >= linker->node_count || !linker->nodes[index]) return NULL;
    return &linker->nodes[index]->module;
}

static Etd2xLinkerNode *linker_find_node_by_module(const Etd2xLinker *linker, const Etd2xModule *module) {
    if (!linker || !module) return NULL;
    for (size_t i = 0; i < linker->node_count; ++i) {
        Etd2xLinkerNode *node = linker->nodes[i];
        if (node && &node->module == module) return node;
    }
    return NULL;
}

Etd2xResult etd2x_linker_construct_module(Etd2xLinker *linker, const Etd2xModule *module) {
    if (!linker || !module) return ETD2X_ERR_ARGUMENT;
    linker_linker_lock();
    Etd2xLinkerNode *node = linker_find_node_by_module(linker, module);
    Etd2xResult rc = ETD2X_ERR_ARGUMENT;
    if (node) {
        etd2x_compat_set_active_linker(linker);
        rc = linker_finalize_local_group(linker, node);
        if (rc == ETD2X_OK) rc = linker_construct_node(linker, node);
    }
    linker_linker_unlock();
    return rc;
}

bool etd2x_linker_module_constructed(const Etd2xLinker *linker, const Etd2xModule *module) {
    linker_linker_lock();
    const Etd2xLinkerNode *node = linker_find_node_by_module(linker, module);
    const bool constructed = node && node->constructed;
    linker_linker_unlock();
    return constructed;
}

static Etd2xResult linker_resolve_import_impl(const Etd2xModule *requester,
                                       const char *name,
                                       const Etd2xSymbolVersion *version,
                                       uintptr_t *out_address,
                                       const Etd2xModule **out_provider,
                                       bool account_compat_bind) {
    if (!requester || !requester->linker || !name || !out_address) {
        return ETD2X_ERR_ARGUMENT;
    }
    Etd2xLinker *linker = requester->linker;
    Etd2xLinkerNode *requester_node = linker_find_node_by_module(linker, requester);
    if (!requester_node || !requester_node->relocation_scope_root ||
        !requester_node->relocation_scope_root->local_group) {
        return ETD2X_ERR_SYMBOL_NOT_FOUND;
    }

    Etd2xLinkerNode *scope_root = requester_node->relocation_scope_root;

    /* Bionic API 23+: global group first, then this root's BFS local group.
     * Only actually loaded DF_1_GLOBAL/RTLD_GLOBAL modules participate; roots
     * merely pre-discovered by the Java bootstrap are intentionally excluded. */
    for (size_t i = 0; i < linker->node_count; ++i) {
        Etd2xLinkerNode *node = linker->nodes[i];
        if (!node || !node->global_visible) continue;

        if (!linker_version_allows_provider(&node->module, version)) continue;
        uintptr_t address = 0;
        Etd2xResult rc = etd2x_symbol_find_defined_versioned(&node->module, name, version, &address);
        if (rc == ETD2X_OK) {
            *out_address = address;
            if (out_provider) *out_provider = &node->module;
            return ETD2X_OK;
        }
        if (rc != ETD2X_ERR_SYMBOL_NOT_FOUND) return rc;
    }

    for (size_t i = 0; i < scope_root->local_group_count; ++i) {
        Etd2xLinkerNode *node = scope_root->local_group[i];
        if (!node) continue;

        if (!linker_version_allows_provider(&node->module, version)) continue;
        uintptr_t address = 0;
        Etd2xResult rc = etd2x_symbol_find_defined_versioned(&node->module, name, version, &address);
        if (rc == ETD2X_OK) {
            *out_address = address;
            if (out_provider) *out_provider = &node->module;
            return ETD2X_OK;
        }
        if (rc != ETD2X_ERR_SYMBOL_NOT_FOUND) return rc;
    }

    uintptr_t compat_address = 0;
    const char *compat_provider = NULL;
    Etd2xResult compat_rc = etd2x_compat_resolve(name, &compat_address, &compat_provider);
    if (compat_rc == ETD2X_OK) {
        if (version && version->provider && version->provider[0] &&
            (!compat_provider || strcmp(version->provider, compat_provider) != 0)) {
            return ETD2X_ERR_SYMBOL_NOT_FOUND;
        }
        *out_address = compat_address;
        if (out_provider) *out_provider = NULL;
        if (account_compat_bind) {
            linker->compat_bind_count++;
            ETD2X_LOGCI("COMPAT", "symbol=%s requester=%s provider=%s address=%p",
                        name, requester->path, compat_provider ? compat_provider : "<compat>",
                        (void *)compat_address);
        }
        return ETD2X_OK;
    }
    if (compat_rc != ETD2X_ERR_SYMBOL_NOT_FOUND) return compat_rc;
    return ETD2X_ERR_SYMBOL_NOT_FOUND;
}

Etd2xResult etd2x_linker_resolve_tls_import_versioned(const Etd2xModule *requester,
                                                      const char *name,
                                                      const Etd2xSymbolVersion *version,
                                                      const Etd2xModule **out_provider,
                                                      size_t *out_offset) {
    if (!requester || !requester->linker || !name || !name[0] ||
        !out_provider || !out_offset) return ETD2X_ERR_ARGUMENT;
    Etd2xLinker *linker = requester->linker;
    Etd2xLinkerNode *requester_node = linker_find_node_by_module(linker, requester);
    if (!requester_node || !requester_node->relocation_scope_root ||
        !requester_node->relocation_scope_root->local_group) {
        return ETD2X_ERR_SYMBOL_NOT_FOUND;
    }

    for (size_t i = 0; i < linker->node_count; ++i) {
        Etd2xLinkerNode *node = linker->nodes[i];
        if (!node || !node->global_visible) continue;
        if (!linker_version_allows_provider(&node->module, version)) continue;
        size_t offset = 0;
        Etd2xResult rc = etd2x_symbol_find_tls_defined_versioned(
            &node->module, name, version, &offset);
        if (rc == ETD2X_OK) {
            *out_provider = &node->module;
            *out_offset = offset;
            return ETD2X_OK;
        }
        if (rc != ETD2X_ERR_SYMBOL_NOT_FOUND && rc != ETD2X_ERR_ARGUMENT) return rc;
    }

    Etd2xLinkerNode *scope_root = requester_node->relocation_scope_root;
    for (size_t i = 0; i < scope_root->local_group_count; ++i) {
        Etd2xLinkerNode *node = scope_root->local_group[i];
        if (!node) continue;
        if (!linker_version_allows_provider(&node->module, version)) continue;
        size_t offset = 0;
        Etd2xResult rc = etd2x_symbol_find_tls_defined_versioned(
            &node->module, name, version, &offset);
        if (rc == ETD2X_OK) {
            *out_provider = &node->module;
            *out_offset = offset;
            return ETD2X_OK;
        }
        if (rc != ETD2X_ERR_SYMBOL_NOT_FOUND && rc != ETD2X_ERR_ARGUMENT) return rc;
    }
    return ETD2X_ERR_SYMBOL_NOT_FOUND;
}

Etd2xResult etd2x_linker_probe_global_versioned(const Etd2xModule *requester,
                                                const char *name,
                                                const Etd2xSymbolVersion *version,
                                                uintptr_t *out_address,
                                                const Etd2xModule **out_provider) {
    if (!requester || !requester->linker || !name || !name[0] || !out_address) {
        return ETD2X_ERR_ARGUMENT;
    }

    Etd2xLinker *linker = requester->linker;
    for (size_t i = 0; i < linker->node_count; ++i) {
        Etd2xLinkerNode *node = linker->nodes[i];
        if (!node || !node->global_visible || !node->constructed) continue;

        if (!linker_version_allows_provider(&node->module, version)) continue;
        uintptr_t address = 0;
        Etd2xResult rc = etd2x_symbol_find_defined_versioned(&node->module, name, version, &address);
        if (rc == ETD2X_OK) {
            *out_address = address;
            if (out_provider) *out_provider = &node->module;
            return ETD2X_OK;
        }
        if (rc != ETD2X_ERR_SYMBOL_NOT_FOUND) return rc;
    }
    return ETD2X_ERR_SYMBOL_NOT_FOUND;
}

Etd2xResult etd2x_linker_probe_global(const Etd2xModule *requester,
                                      const char *name,
                                      uintptr_t *out_address,
                                      const Etd2xModule **out_provider) {
    return etd2x_linker_probe_global_versioned(requester, name, NULL, out_address, out_provider);
}

Etd2xResult etd2x_linker_promote_global(Etd2xLinker *linker,
                                        const Etd2xModule *module,
                                        const char *reason) {
    if (!linker || !module) return ETD2X_ERR_ARGUMENT;
    Etd2xLinkerNode *node = linker_find_node_by_module(linker, module);
    if (!node) return ETD2X_ERR_ARGUMENT;

    node->rtld_global = true;
    if (module->mapped && !node->global_visible) {
        node->global_visible = true;
        ETD2X_LOGCI("LINKER", "global group add module=%s reason=%s",
                    linker_path_basename(module->path), reason && reason[0] ? reason : "RTLD_GLOBAL");
    }
    return ETD2X_OK;
}

Etd2xResult etd2x_linker_resolve_import_versioned(const Etd2xModule *requester,
                                                  const char *name,
                                                  const Etd2xSymbolVersion *version,
                                                  uintptr_t *out_address,
                                                  const Etd2xModule **out_provider) {
    return linker_resolve_import_impl(requester, name, version, out_address, out_provider, true);
}

Etd2xResult etd2x_linker_resolve_import(const Etd2xModule *requester,
                                        const char *name,
                                        uintptr_t *out_address,
                                        const Etd2xModule **out_provider) {
    return etd2x_linker_resolve_import_versioned(requester, name, NULL, out_address, out_provider);
}

Etd2xResult etd2x_linker_probe_import_versioned(const Etd2xModule *requester,
                                                const char *name,
                                                const Etd2xSymbolVersion *version,
                                                uintptr_t *out_address,
                                                const Etd2xModule **out_provider) {
    return linker_resolve_import_impl(requester, name, version, out_address, out_provider, false);
}

Etd2xResult etd2x_linker_probe_import(const Etd2xModule *requester,
                                      const char *name,
                                      uintptr_t *out_address,
                                      const Etd2xModule **out_provider) {
    return etd2x_linker_probe_import_versioned(requester, name, NULL, out_address, out_provider);
}

Etd2xResult etd2x_linker_find_global(const Etd2xLinker *linker,
                                     const char *name,
                                     const Etd2xSymbolVersion *version,
                                     uintptr_t *out_address,
                                     const Etd2xModule **out_provider) {
    if (!linker || !name || !name[0] || !out_address) return ETD2X_ERR_ARGUMENT;
    linker_linker_lock();
    Etd2xResult rc = ETD2X_ERR_SYMBOL_NOT_FOUND;
    for (size_t i = 0; i < linker->node_count; ++i) {
        Etd2xLinkerNode *node = linker->nodes[i];
        if (!node || !node->global_visible || !node->constructed) continue;
        if (!linker_version_allows_provider(&node->module, version)) continue;
        uintptr_t address = 0;
        Etd2xResult find_rc = etd2x_symbol_find_defined_versioned(&node->module, name, version, &address);
        if (find_rc == ETD2X_OK) {
            *out_address = address;
            if (out_provider) *out_provider = &node->module;
            rc = ETD2X_OK;
            break;
        }
        if (find_rc != ETD2X_ERR_SYMBOL_NOT_FOUND) { rc = find_rc; break; }
    }
    linker_linker_unlock();
    return rc;
}

Etd2xResult etd2x_linker_find_in_handle(const Etd2xLinker *linker,
                                        const Etd2xModule *module,
                                        const char *name,
                                        const Etd2xSymbolVersion *version,
                                        uintptr_t *out_address,
                                        const Etd2xModule **out_provider) {
    if (!linker || !module || !name || !name[0] || !out_address) return ETD2X_ERR_ARGUMENT;
    linker_linker_lock();
    Etd2xLinkerNode *root = linker_find_node_by_module(linker, module);
    if (!root || (!root->pinned && root->direct_refcount == 0) || !root->constructed) {
        linker_linker_unlock();
        return ETD2X_ERR_SYMBOL_NOT_FOUND;
    }
    Etd2xResult rc = linker_build_local_group((Etd2xLinker *)(uintptr_t)linker, root);
    if (rc == ETD2X_OK) {
        rc = ETD2X_ERR_SYMBOL_NOT_FOUND;
        for (size_t i = 0; i < root->local_group_count; ++i) {
            Etd2xLinkerNode *node = root->local_group[i];
            if (!node || !node->constructed) continue;
            if (!linker_version_allows_provider(&node->module, version)) continue;
            uintptr_t address = 0;
            Etd2xResult find_rc = etd2x_symbol_find_defined_versioned(&node->module, name, version, &address);
            if (find_rc == ETD2X_OK) {
                *out_address = address;
                if (out_provider) *out_provider = &node->module;
                rc = ETD2X_OK;
                break;
            }
            if (find_rc != ETD2X_ERR_SYMBOL_NOT_FOUND) { rc = find_rc; break; }
        }
    }
    linker_linker_unlock();
    return rc;
}

static bool linker_node_live_as_root(const Etd2xLinkerNode *node) {
    return node && node->constructed && (node->pinned || node->direct_refcount != 0);
}

static bool linker_node_needed_by_other_live_root(const Etd2xLinker *linker,
                                           const Etd2xLinkerNode *candidate,
                                           const Etd2xLinkerNode *ignore_root) {
    for (size_t i = 0; linker && i < linker->node_count; ++i) {
        Etd2xLinkerNode *root = linker->nodes[i];
        if (!root || root == ignore_root || !linker_node_live_as_root(root) || !root->local_group) continue;
        if (linker_group_contains(root->local_group, root->local_group_count, candidate)) return true;
    }
    return false;
}

Etd2xResult etd2x_linker_acquire_module(Etd2xLinker *linker, const Etd2xModule *module) {
    if (!linker || !module) return ETD2X_ERR_ARGUMENT;
    linker_linker_lock();
    Etd2xLinkerNode *node = linker_find_node_by_module(linker, module);
    Etd2xResult rc = ETD2X_ERR_ARGUMENT;
    if (node) {
        rc = linker_finalize_local_group(linker, node);
        if (rc == ETD2X_OK) rc = linker_construct_node(linker, node);
        if (rc == ETD2X_OK) {
            ++node->direct_refcount;
            ETD2X_LOGCI("LINKER", "acquire module=%s refcount=%zu pinned=%u",
                        linker_path_basename(module->path), node->direct_refcount, node->pinned ? 1u : 0u);
        }
    }
    linker_linker_unlock();
    return rc;
}

Etd2xResult etd2x_linker_pin_module(Etd2xLinker *linker, const Etd2xModule *module) {
    if (!linker || !module) return ETD2X_ERR_ARGUMENT;
    linker_linker_lock();
    Etd2xLinkerNode *node = linker_find_node_by_module(linker, module);
    Etd2xResult rc = ETD2X_ERR_ARGUMENT;
    if (node) {
        node->pinned = true;
        rc = ETD2X_OK;
        ETD2X_LOGCI("LINKER", "pin module=%s", linker_path_basename(module->path));
    }
    linker_linker_unlock();
    return rc;
}

bool etd2x_linker_module_handle_live(const Etd2xLinker *linker, const Etd2xModule *module) {
    if (!linker || !module) return false;
    linker_linker_lock();
    const Etd2xLinkerNode *node = linker_find_node_by_module(linker, module);
    const bool live = node && node->constructed && (node->pinned || node->direct_refcount != 0);
    linker_linker_unlock();
    return live;
}

Etd2xResult etd2x_linker_release_module(Etd2xLinker *linker, const Etd2xModule *module) {
    if (!linker || !module) return ETD2X_ERR_ARGUMENT;
    linker_linker_lock();
    Etd2xLinkerNode *root = linker_find_node_by_module(linker, module);
    if (!root || root->direct_refcount == 0) {
        linker_linker_unlock();
        return ETD2X_ERR_ARGUMENT;
    }
    --root->direct_refcount;
    ETD2X_LOGCI("LINKER", "release module=%s refcount=%zu pinned=%u",
                linker_path_basename(module->path), root->direct_refcount, root->pinned ? 1u : 0u);
    if (root->direct_refcount != 0 || root->pinned) {
        linker_linker_unlock();
        return ETD2X_OK;
    }

    Etd2xResult rc = linker_build_local_group(linker, root);
    if (rc != ETD2X_OK) { linker_linker_unlock(); return rc; }
    /* Bionic tears down the dlopen root before dependencies.  local_group is
     * BFS ordered root -> direct dependencies -> deeper dependencies, so walk
     * forward here (constructor order is handled separately by construct_node). */
    for (size_t i = 0; i < root->local_group_count; ++i) {
        Etd2xLinkerNode *node = root->local_group[i];
        if (!node || !node->constructed || node->pinned || node->direct_refcount != 0 ||
            linker_node_needed_by_other_live_root(linker, node, root)) {
            continue;
        }
        rc = etd2x_dynamic_run_destructors(&node->module);
        if (rc != ETD2X_OK) break;
        node->constructed = false;
        node->destructed = true;
        node->module.state = ETD2X_MODULE_MAPPED;
        if ((node->module.dynamic.flags_1 & DF_1_GLOBAL) == 0) {
            node->global_visible = false;
        }
        node->rtld_global = false;
        ETD2X_LOGCI("LINKER", "logical unload module=%s mapping=retained",
                    linker_path_basename(node->module.path));
    }
    linker_linker_unlock();
    return rc;
}

static void linker_rollback_nodes_from(Etd2xLinker *linker, size_t first_new) {
    if (!linker || first_new > linker->node_count) return;
    for (size_t i = linker->node_count; i > first_new; --i) {
        Etd2xLinkerNode *node = linker->nodes[i - 1];
        if (!node) continue;
        linker_free_node(node);
        linker->nodes[i - 1] = NULL;
    }
    linker->node_count = first_new;
}

static Etd2xResult linker_linker_load_library_locked(Etd2xLinker *linker,
                                              const char *soname,
                                              Etd2xModule **out_module,
                                              bool *out_newly_loaded);

Etd2xResult etd2x_linker_load_library(Etd2xLinker *linker,
                                      const char *soname,
                                      Etd2xModule **out_module,
                                      bool *out_newly_loaded) {
    if (!linker || !soname || !soname[0] || !out_module || linker_has_path_separator(soname)) {
        return ETD2X_ERR_ARGUMENT;
    }
    linker_linker_lock();
    const Etd2xResult rc = linker_linker_load_library_locked(linker, soname, out_module, out_newly_loaded);
    linker_linker_unlock();
    return rc;
}

static Etd2xResult linker_linker_load_library_locked(Etd2xLinker *linker,
                                              const char *soname,
                                              Etd2xModule **out_module,
                                              bool *out_newly_loaded) {
    *out_module = NULL;
    if (out_newly_loaded) *out_newly_loaded = false;

    Etd2xLinkerNode *existing = linker_find_node_by_needed(linker, soname);
    if (existing) {
        const bool was_mapped = existing->module.mapped;
        Etd2xResult rc = linker_finalize_local_group(linker, existing);
        if (rc != ETD2X_OK) return rc;
        *out_module = &existing->module;
        if (out_newly_loaded) *out_newly_loaded = !was_mapped;
        return ETD2X_OK;
    }

    if (!linker->library_dir[0]) return ETD2X_ERR_DEPENDENCY_INVALID;

    char path[sizeof(((Etd2xModule *)0)->path)];
    Etd2xResult rc = linker_dependency_path(linker, soname, path, sizeof(path));
    if (rc != ETD2X_OK) return rc;

    /* A first root can still use the normal all-at-once path. */
    if (linker->node_count == 0) {
        rc = etd2x_linker_load(linker, path, out_module);
        if (rc == ETD2X_OK && out_newly_loaded) *out_newly_loaded = true;
        return rc;
    }

    const size_t first_new = linker->node_count;
    const uint64_t load_begin_ms = linker_linker_monotonic_ms();
    Etd2xLinkerNode *root = NULL;
    rc = linker_discover_module(linker, path, &root);
    if (rc != ETD2X_OK) goto fail;

    rc = linker_finalize_local_group(linker, root);
    if (rc != ETD2X_OK) goto fail;

    etd2x_compat_set_active_linker(linker);
    rc = linker_construct_node(linker, root);
    if (rc != ETD2X_OK) goto fail;

    *out_module = &root->module;
    if (out_newly_loaded) *out_newly_loaded = true;
    ETD2X_LOGCI("LINKER", "incremental root finalized module=%s added=%zu total=%zu elapsedMs=%llu",
                soname, linker->node_count - first_new, linker->node_count,
                (unsigned long long)(linker_linker_monotonic_ms() - load_begin_ms));
    return ETD2X_OK;

fail:
    ETD2X_LOGCE("LINKER", "incremental load failed module=%s: %s",
                soname, etd2x_result_string(rc));
    linker_rollback_nodes_from(linker, first_new);
    return rc;
}

Etd2xResult etd2x_linker_prepare_roots(Etd2xLinker *linker,
                                       const char *const *paths,
                                       size_t path_count,
                                       Etd2xModule **out_primary_root) {
    if (!linker || !paths || path_count == 0 || !out_primary_root || linker->node_count != 0) {
        return ETD2X_ERR_ARGUMENT;
    }
    *out_primary_root = NULL;
    for (size_t i = 0; i < path_count; ++i) {
        if (!paths[i] || !paths[i][0]) return ETD2X_ERR_ARGUMENT;
    }

    const uint64_t load_begin_ms = linker_linker_monotonic_ms();
    uint64_t phase_begin_ms = load_begin_ms;
    Etd2xResult rc = linker_derive_library_dir(linker, paths[0]);
    if (rc != ETD2X_OK) return rc;

    Etd2xLinkerNode **roots = calloc(path_count, sizeof(*roots));
    if (!roots) return ETD2X_ERR_NO_MEMORY;
    for (size_t i = 0; i < path_count; ++i) {
        rc = linker_discover_module(linker, paths[i], &roots[i]);
        if (rc != ETD2X_OK) goto fail;
    }

    ETD2X_LOGCI("PERF", "linker phase=discover elapsedMs=%llu",
                (unsigned long long)(linker_linker_monotonic_ms() - phase_begin_ms));
    ETD2X_LOGCI("LINKER", "dependency graph complete modules=%zu roots=%zu",
                linker->node_count, path_count);

    /* etd2x_loader_prepare() already reserves each module's guest address.
       Keep relocation + mapping deferred until the corresponding dlopen/
       System.loadLibrary root actually runs. This preserves Android load order
       and, critically, prevents unrelated package roots from participating in
       one another's RTLD_LOCAL symbol lookup scope. */
    etd2x_compat_set_active_linker(linker);
    *out_primary_root = &roots[0]->module;
    ETD2X_LOGCI("LINKER", "roots prepared primary=%s roots=%zu modules=%zu relocation=deferred map=deferred constructors=deferred",
                paths[0], path_count, linker->node_count);
    ETD2X_LOGCI("PERF", "linker prepare-roots total elapsedMs=%llu",
                (unsigned long long)(linker_linker_monotonic_ms() - load_begin_ms));
    free(roots);
    return ETD2X_OK;

fail:
    ETD2X_LOGCE("LINKER", "prepare graph failed primary=%s roots=%zu: %s",
                paths[0], path_count, etd2x_result_string(rc));
    free(roots);
    etd2x_linker_unload(linker);
    return rc;
}

Etd2xResult etd2x_linker_load_roots(Etd2xLinker *linker,
                                    const char *const *paths,
                                    size_t path_count,
                                    Etd2xModule **out_primary_root) {
    Etd2xResult rc = etd2x_linker_prepare_roots(linker, paths, path_count, out_primary_root);
    if (rc != ETD2X_OK) return rc;

    const uint64_t phase_begin_ms = linker_linker_monotonic_ms();
    for (size_t i = 0; i < path_count; ++i) {
        Etd2xLinkerNode *root = linker_find_node_by_path(linker, paths[i]);
        if (!root) {
            rc = ETD2X_ERR_DEPENDENCY_NOT_FOUND;
            goto fail;
        }
        rc = linker_finalize_local_group(linker, root);
        if (rc != ETD2X_OK) goto fail;
        rc = linker_construct_node(linker, root);
        if (rc != ETD2X_OK) goto fail;
    }
    ETD2X_LOGCI("PERF", "linker phase=construct elapsedMs=%llu",
                (unsigned long long)(linker_linker_monotonic_ms() - phase_begin_ms));
    ETD2X_LOGCI("LINKER", "roots finalized primary=%s roots=%zu modules=%zu",
                paths[0], path_count, linker->node_count);
    return ETD2X_OK;

fail:
    ETD2X_LOGCE("LINKER", "construct graph failed primary=%s roots=%zu: %s",
                paths[0], path_count, etd2x_result_string(rc));
    etd2x_linker_unload(linker);
    *out_primary_root = NULL;
    return rc;
}

Etd2xResult etd2x_linker_load(Etd2xLinker *linker, const char *path, Etd2xModule **out_root) {
    const char *paths[1] = { path };
    return etd2x_linker_load_roots(linker, paths, 1, out_root);
}

#pragma pop_macro("DF_1_NODELETE")
#pragma pop_macro("DF_1_GLOBAL")


/* ---- symbols ---- */
#pragma push_macro("STT_GNU_IFUNC")





#include <elf.h>
#include <string.h>

#ifndef STT_GNU_IFUNC
#define STT_GNU_IFUNC 10
#endif

static const void *symbols_dynamic_view(const Etd2xModule *module, uint64_t elf_vaddr, size_t size) {
    if (!module || size == 0) {
        return NULL;
    }

    if (module->mapped) {
        void *address = etd2x_module_runtime_address(module, elf_vaddr);
        if (!address || !etd2x_module_runtime_contains(module, (uintptr_t)address, size)) {
            return NULL;
        }
        return address;
    }

    if (!etd2x_module_backing_contains(module, elf_vaddr, size)) {
        return NULL;
    }
    return etd2x_module_backing_address(module, elf_vaddr);
}

Etd2xResult etd2x_symbol_get(const Etd2xModule *module, size_t symbol_index,
                             const Elf64_Sym **out_symbol, const char **out_name) {
    if (!module || !out_symbol || !out_name ||
        !module->dynamic.symtab_vaddr || !module->dynamic.strtab_vaddr) {
        return ETD2X_ERR_ARGUMENT;
    }
    if (symbol_index >= module->dynamic.symbol_count) {
        return ETD2X_ERR_SYMBOL_INDEX;
    }
    if (symbol_index > UINT64_MAX / sizeof(Elf64_Sym)) {
        return ETD2X_ERR_DYNAMIC_INVALID;
    }
    const uint64_t symbol_offset = (uint64_t)symbol_index * sizeof(Elf64_Sym);
    if (module->dynamic.symtab_vaddr > UINT64_MAX - symbol_offset) {
        return ETD2X_ERR_DYNAMIC_INVALID;
    }

    const uint64_t symbol_vaddr = module->dynamic.symtab_vaddr + symbol_offset;
    const Elf64_Sym *symbol = (const Elf64_Sym *)symbols_dynamic_view(
        module, symbol_vaddr, sizeof(Elf64_Sym));
    if (!symbol || symbol->st_name >= module->dynamic.strsz) {
        return ETD2X_ERR_DYNAMIC_INVALID;
    }

    if (module->dynamic.strtab_vaddr > UINT64_MAX - symbol->st_name) {
        return ETD2X_ERR_DYNAMIC_INVALID;
    }
    const uint64_t name_vaddr = module->dynamic.strtab_vaddr + symbol->st_name;
    const size_t remaining = module->dynamic.strsz - symbol->st_name;
    const char *name = (const char *)symbols_dynamic_view(module, name_vaddr, remaining);
    if (!name || !memchr(name, '\0', remaining)) {
        return ETD2X_ERR_DYNAMIC_INVALID;
    }

    *out_symbol = symbol;
    *out_name = name;
    return ETD2X_OK;
}

static const char *symbols_dynamic_string_at(const Etd2xModule *module, uint32_t offset) {
    if (!module || !module->dynamic.strtab_vaddr || offset >= module->dynamic.strsz) return NULL;
    const size_t remaining = module->dynamic.strsz - (size_t)offset;
    const char *s = (const char *)symbols_dynamic_view(module, module->dynamic.strtab_vaddr + offset, remaining);
    return s && memchr(s, '\0', remaining) ? s : NULL;
}

Etd2xResult etd2x_symbol_version(const Etd2xModule *module, size_t symbol_index,
                                 Etd2xSymbolVersion *out_version) {
    if (!module || !out_version || symbol_index >= module->dynamic.symbol_count) {
        return ETD2X_ERR_ARGUMENT;
    }
    memset(out_version, 0, sizeof(*out_version));
    if (!module->dynamic.versym_vaddr) return ETD2X_OK;

    const uint64_t off = (uint64_t)symbol_index * sizeof(Elf64_Half);
    const Elf64_Half *versym = (const Elf64_Half *)symbols_dynamic_view(
        module, module->dynamic.versym_vaddr + off, sizeof(*versym));
    if (!versym) return ETD2X_ERR_DYNAMIC_INVALID;

    const Elf64_Half raw = *versym;
    const Elf64_Half index = raw & 0x7fffu;
    out_version->hidden = (raw & 0x8000u) != 0;
    if (index == VER_NDX_LOCAL || index == VER_NDX_GLOBAL) return ETD2X_OK;

    uint64_t cursor = module->dynamic.verneed_vaddr;
    for (size_t i = 0; cursor && i < module->dynamic.verneed_count; ++i) {
        const Elf64_Verneed *need = (const Elf64_Verneed *)symbols_dynamic_view(module, cursor, sizeof(*need));
        if (!need) return ETD2X_ERR_DYNAMIC_INVALID;
        if (need->vn_version != VER_NEED_CURRENT || need->vn_aux == 0) return ETD2X_ERR_DYNAMIC_INVALID;

        uint64_t aux_cursor = cursor + need->vn_aux;
        for (size_t j = 0; j < need->vn_cnt; ++j) {
            const Elf64_Vernaux *aux = (const Elf64_Vernaux *)symbols_dynamic_view(module, aux_cursor, sizeof(*aux));
            if (!aux) return ETD2X_ERR_DYNAMIC_INVALID;
            if ((aux->vna_other & 0x7fffu) == index) {
                const char *name = symbols_dynamic_string_at(module, aux->vna_name);
                if (!name || !name[0]) return ETD2X_ERR_DYNAMIC_INVALID;
                const char *provider = symbols_dynamic_string_at(module, need->vn_file);
                if (!provider || !provider[0]) return ETD2X_ERR_DYNAMIC_INVALID;
                out_version->name = name;
                out_version->provider = provider;
                out_version->hash = aux->vna_hash;
                return ETD2X_OK;
            }
            if (j + 1u < need->vn_cnt) {
                if (!aux->vna_next) return ETD2X_ERR_DYNAMIC_INVALID;
                aux_cursor += aux->vna_next;
            }
        }
        if (i + 1u < module->dynamic.verneed_count) {
            if (!need->vn_next) return ETD2X_ERR_DYNAMIC_INVALID;
            cursor += need->vn_next;
        }
    }

    cursor = module->dynamic.verdef_vaddr;
    for (size_t i = 0; cursor && i < module->dynamic.verdef_count; ++i) {
        const Elf64_Verdef *def = (const Elf64_Verdef *)symbols_dynamic_view(module, cursor, sizeof(*def));
        if (!def) return ETD2X_ERR_DYNAMIC_INVALID;
        if (def->vd_version != VER_DEF_CURRENT || def->vd_aux == 0) return ETD2X_ERR_DYNAMIC_INVALID;
        if ((def->vd_ndx & 0x7fffu) == index) {
            const uint64_t aux_vaddr = cursor + def->vd_aux;
            const Elf64_Verdaux *aux = (const Elf64_Verdaux *)symbols_dynamic_view(module, aux_vaddr, sizeof(*aux));
            if (!aux) return ETD2X_ERR_DYNAMIC_INVALID;
            const char *name = symbols_dynamic_string_at(module, aux->vda_name);
            if (!name || !name[0]) return ETD2X_ERR_DYNAMIC_INVALID;
            out_version->name = name;
            out_version->hash = def->vd_hash;
            return ETD2X_OK;
        }
        if (i + 1u < module->dynamic.verdef_count) {
            if (!def->vd_next) return ETD2X_ERR_DYNAMIC_INVALID;
            cursor += def->vd_next;
        }
    }

    ETD2X_LOGCE("SYMBOL", "version index=%u missing verneed/verdef module=%s symbol=%zu",
                (unsigned)index, module->path, symbol_index);
    return ETD2X_ERR_SYMBOL_NOT_FOUND;
}

static bool symbols_version_matches(const Etd2xSymbolVersion *candidate,
                            const Etd2xSymbolVersion *required) {
    if (!required || !required->name || !required->name[0]) {
        return !candidate || !candidate->hidden;
    }
    if (!candidate || !candidate->name) return false;
    if (required->hash && candidate->hash && required->hash != candidate->hash) return false;
    return strcmp(required->name, candidate->name) == 0;
}

static Etd2xResult symbols_defined_symbol_type(const Etd2xModule *module, const char *name,
                                             const Etd2xSymbolVersion *required_version,
                                             unsigned *out_type) {
    if (!module || !name || !out_type) return ETD2X_ERR_ARGUMENT;
    for (size_t index = 0; index < module->dynamic.symbol_count; ++index) {
        const Elf64_Sym *symbol = NULL;
        const char *symbol_name = NULL;
        Etd2xResult rc = etd2x_symbol_get(module, index, &symbol, &symbol_name);
        if (rc != ETD2X_OK) return rc;
        const unsigned bind = ELF64_ST_BIND(symbol->st_info);
        const unsigned visibility = ELF64_ST_VISIBILITY(symbol->st_other);
        if (symbol->st_shndx == SHN_UNDEF || bind == STB_LOCAL ||
            (visibility != STV_DEFAULT && visibility != STV_PROTECTED) ||
            strcmp(symbol_name, name) != 0) continue;
        Etd2xSymbolVersion candidate = {0};
        rc = etd2x_symbol_version(module, index, &candidate);
        if (rc != ETD2X_OK) return rc;
        if (!symbols_version_matches(&candidate, required_version)) continue;
        *out_type = ELF64_ST_TYPE(symbol->st_info);
        return ETD2X_OK;
    }
    return ETD2X_ERR_SYMBOL_NOT_FOUND;
}

Etd2xResult etd2x_symbol_find_defined_versioned_ex(const Etd2xModule *module, const char *name,
                                                   const Etd2xSymbolVersion *required_version,
                                                   uintptr_t *out_address, unsigned *out_type) {
    if (!module || !name || !out_address) return ETD2X_ERR_ARGUMENT;

    for (size_t index = 0; index < module->dynamic.symbol_count; ++index) {
        const Elf64_Sym *symbol = NULL;
        const char *symbol_name = NULL;
        Etd2xResult rc = etd2x_symbol_get(module, index, &symbol, &symbol_name);
        if (rc != ETD2X_OK) return rc;

        const unsigned bind = ELF64_ST_BIND(symbol->st_info);
        const unsigned visibility = ELF64_ST_VISIBILITY(symbol->st_other);
        const unsigned type = ELF64_ST_TYPE(symbol->st_info);
        if (symbol->st_shndx == SHN_UNDEF || bind == STB_LOCAL ||
            (visibility != STV_DEFAULT && visibility != STV_PROTECTED) ||
            strcmp(symbol_name, name) != 0) {
            continue;
        }

        Etd2xSymbolVersion candidate = {0};
        rc = etd2x_symbol_version(module, index, &candidate);
        if (rc != ETD2X_OK) return rc;
        if (!symbols_version_matches(&candidate, required_version)) continue;
        if (type == STT_TLS) return ETD2X_ERR_SYMBOL_NOT_FOUND;

        uintptr_t address = 0;
        if (symbol->st_shndx == SHN_ABS) {
            address = (uintptr_t)symbol->st_value;
        } else {
            if (symbol->st_value < module->min_vaddr || symbol->st_value >= module->max_vaddr) {
                return ETD2X_ERR_ELF_BOUNDS;
            }
            address = module->load_bias + (uintptr_t)symbol->st_value;
        }

        /* STT_GNU_IFUNC's symbol value is a resolver function, not the final
         * implementation. Before mapping we expose the resolver address so the
         * normal non-IFUNC relocation pass can complete without executing code.
         * Once mapped RX, lookups (including dlsym and the post-map relocation
         * pass) execute the resolver exactly at the Android linker boundary. */
        if (type == STT_GNU_IFUNC && module->mapped) {
            address = etd2x_call_android_ifunc_resolver(address);
            ETD2X_LOGCI("IFUNC", "resolved symbol=%s module=%s implementation=%p",
                        name, module->path, (void *)address);
        }
        *out_address = address;
        if (out_type) *out_type = type;
        return ETD2X_OK;
    }
    return ETD2X_ERR_SYMBOL_NOT_FOUND;
}

Etd2xResult etd2x_symbol_find_defined_versioned(const Etd2xModule *module, const char *name,
                                                const Etd2xSymbolVersion *required_version,
                                                uintptr_t *out_address) {
    return etd2x_symbol_find_defined_versioned_ex(module, name, required_version,
                                                  out_address, NULL);
}

Etd2xResult etd2x_symbol_find_tls_defined_versioned(const Etd2xModule *module, const char *name,
                                                    const Etd2xSymbolVersion *required_version,
                                                    size_t *out_offset) {
    if (!module || !name || !out_offset || !module->tls.present) return ETD2X_ERR_ARGUMENT;

    for (size_t index = 0; index < module->dynamic.symbol_count; ++index) {
        const Elf64_Sym *symbol = NULL;
        const char *symbol_name = NULL;
        Etd2xResult rc = etd2x_symbol_get(module, index, &symbol, &symbol_name);
        if (rc != ETD2X_OK) return rc;
        const unsigned bind = ELF64_ST_BIND(symbol->st_info);
        const unsigned visibility = ELF64_ST_VISIBILITY(symbol->st_other);
        if (symbol->st_shndx == SHN_UNDEF || ELF64_ST_TYPE(symbol->st_info) != STT_TLS ||
            bind == STB_LOCAL || (visibility != STV_DEFAULT && visibility != STV_PROTECTED) ||
            strcmp(symbol_name, name) != 0) continue;

        Etd2xSymbolVersion candidate = {0};
        rc = etd2x_symbol_version(module, index, &candidate);
        if (rc != ETD2X_OK) return rc;
        if (!symbols_version_matches(&candidate, required_version)) continue;
        if (symbol->st_value > module->tls.mem_size) return ETD2X_ERR_ELF_BOUNDS;
        *out_offset = (size_t)symbol->st_value;
        return ETD2X_OK;
    }
    return ETD2X_ERR_SYMBOL_NOT_FOUND;
}

Etd2xResult etd2x_symbol_resolve_tls(const Etd2xModule *requester, size_t symbol_index,
                                     const Etd2xModule **out_provider, size_t *out_offset,
                                     Etd2xSymbolVersion *out_version) {
    if (!requester || !out_provider || !out_offset) return ETD2X_ERR_ARGUMENT;
    const Elf64_Sym *symbol = NULL;
    const char *name = NULL;
    Etd2xResult rc = etd2x_symbol_get(requester, symbol_index, &symbol, &name);
    if (rc != ETD2X_OK) return rc;
    if (ELF64_ST_TYPE(symbol->st_info) != STT_TLS && symbol_index != 0) {
        return ETD2X_ERR_RELOCATION_FORMAT;
    }

    Etd2xSymbolVersion version = {0};
    if (symbol_index != 0) {
        rc = etd2x_symbol_version(requester, symbol_index, &version);
        if (rc != ETD2X_OK) return rc;
    }
    if (out_version) *out_version = version;

    if (symbol_index == 0) {
        if (!requester->tls.present) return ETD2X_ERR_SYMBOL_NOT_FOUND;
        *out_provider = requester;
        *out_offset = 0;
        return ETD2X_OK;
    }

    if (symbol->st_shndx != SHN_UNDEF) {
        if (!requester->tls.present || symbol->st_value > requester->tls.mem_size) {
            return ETD2X_ERR_ELF_BOUNDS;
        }
        *out_provider = requester;
        *out_offset = (size_t)symbol->st_value;
        return ETD2X_OK;
    }

    if (requester->linker && name[0]) {
        rc = etd2x_linker_resolve_tls_import_versioned(
            requester, name, &version, out_provider, out_offset);
        if (rc == ETD2X_OK) return ETD2X_OK;
        if (rc != ETD2X_ERR_SYMBOL_NOT_FOUND) return rc;
    }
    if (ELF64_ST_BIND(symbol->st_info) == STB_WEAK) {
        *out_provider = NULL;
        *out_offset = 0;
        return ETD2X_OK;
    }
    return ETD2X_ERR_SYMBOL_UNRESOLVED;
}

Etd2xResult etd2x_symbol_find_defined(const Etd2xModule *module, const char *name,
                                      uintptr_t *out_address) {
    return etd2x_symbol_find_defined_versioned(module, name, NULL, out_address);
}



Etd2xResult etd2x_symbol_probe(const Etd2xModule *module, size_t symbol_index,
                               const char **out_name, bool *out_weak_zero) {
    if (!module) return ETD2X_ERR_ARGUMENT;

    const Elf64_Sym *symbol = NULL;
    const char *name = NULL;
    Etd2xResult rc = etd2x_symbol_get(module, symbol_index, &symbol, &name);
    if (rc != ETD2X_OK) return rc;

    if (out_name) *out_name = name;
    if (out_weak_zero) *out_weak_zero = false;

    if (symbol->st_shndx != SHN_UNDEF) return ETD2X_OK;

    if (module->linker && name[0]) {
        Etd2xSymbolVersion version = {0};
        rc = etd2x_symbol_version(module, symbol_index, &version);
        if (rc != ETD2X_OK) return rc;
        uintptr_t address = 0;
        const Etd2xModule *provider = NULL;
        rc = etd2x_linker_probe_import_versioned(module, name, &version, &address, &provider);
        if (rc == ETD2X_OK) return ETD2X_OK;
        if (rc != ETD2X_ERR_SYMBOL_NOT_FOUND) return rc;
    }

    if (ELF64_ST_BIND(symbol->st_info) == STB_WEAK) {
        if (out_weak_zero) *out_weak_zero = true;
        return ETD2X_OK;
    }
    return ETD2X_ERR_SYMBOL_UNRESOLVED;
}

static Etd2xResult symbols_symbol_resolve_impl(Etd2xModule *module, size_t symbol_index,
                                         Etd2xResolvedSymbol *out,
                                         bool account_compat_bind,
                                         bool account_failure_stats) {
    if (!module || !out) {
        return ETD2X_ERR_ARGUMENT;
    }

    const Elf64_Sym *symbol = NULL;
    const char *name = NULL;
    Etd2xResult rc = etd2x_symbol_get(module, symbol_index, &symbol, &name);
    if (rc != ETD2X_OK) {
        return rc;
    }

    out->address = 0;
    out->name = name;
    memset(&out->version, 0, sizeof(out->version));
    out->provider = NULL;
    out->type = ELF64_ST_TYPE(symbol->st_info);
    out->weak_zero = false;
    rc = etd2x_symbol_version(module, symbol_index, &out->version);
    if (rc != ETD2X_OK) return rc;

    if (symbol->st_shndx == SHN_ABS) {
        out->provider = module;
        out->address = (uintptr_t)symbol->st_value;
        if (out->type == STT_GNU_IFUNC && module->mapped) {
            out->address = etd2x_call_android_ifunc_resolver(out->address);
        }
        return ETD2X_OK;
    }

    if (symbol->st_shndx != SHN_UNDEF) {
        /* ELF default-visibility global/weak definitions are preemptible. On
         * Android API 23+ the process-wide global group is searched before the
         * requester's local group, even for relocations that name a definition
         * in the requester. STV_PROTECTED and DT_SYMBOLIC/DF_SYMBOLIC retain
         * local binding. This is important for process-wide ABI runtimes (C++
         * exception/RTTI support, sanitizers, allocators) intentionally placed
         * in a DF_1_GLOBAL DSO. */
        const unsigned bind = ELF64_ST_BIND(symbol->st_info);
        const unsigned visibility = ELF64_ST_VISIBILITY(symbol->st_other);
        if (module->linker && name[0] && !module->dynamic.symbolic &&
            visibility == STV_DEFAULT && (bind == STB_GLOBAL || bind == STB_WEAK)) {
            uintptr_t preempt_address = 0;
            const Etd2xModule *provider = NULL;
            Etd2xResult preempt_rc = etd2x_linker_probe_global_versioned(
                module, name, &out->version, &preempt_address, &provider);
            if (preempt_rc == ETD2X_OK && provider && provider != module) {
                unsigned provider_type = out->type;
                Etd2xResult meta_rc = symbols_defined_symbol_type(
                    provider, name, &out->version, &provider_type);
                if (meta_rc != ETD2X_OK) return meta_rc;
                out->provider = provider;
                out->type = provider_type;
                out->address = preempt_address;
                ETD2X_LOGCI("LINKER", "preempt symbol=%s requester=%s provider=%s address=%p",
                            name, module->path, provider->path, (void *)preempt_address);
                return ETD2X_OK;
            }
            if (preempt_rc != ETD2X_OK && preempt_rc != ETD2X_ERR_SYMBOL_NOT_FOUND) {
                return preempt_rc;
            }
        }

        if (symbol->st_value < module->min_vaddr || symbol->st_value >= module->max_vaddr) {
            ETD2X_LOGCE("SYMBOL", "defined symbol %s has out-of-range value %#llx",
                        name, (unsigned long long)symbol->st_value);
            return ETD2X_ERR_ELF_BOUNDS;
        }
        out->provider = module;
        out->address = module->load_bias + (uintptr_t)symbol->st_value;
        if (out->type == STT_GNU_IFUNC && module->mapped) {
            uintptr_t resolved_address = 0;
            unsigned provider_type = out->type;
            rc = etd2x_symbol_find_defined_versioned_ex(
                module, name, &out->version, &resolved_address, &provider_type);
            if (rc != ETD2X_OK) return rc;
            out->address = resolved_address;
            out->type = provider_type;
        }
        return ETD2X_OK;
    }

    if (module->linker && name[0]) {
        const Etd2xModule *provider = NULL;
        uintptr_t address = 0;
        rc = account_compat_bind
                 ? etd2x_linker_resolve_import_versioned(module, name, &out->version,
                                                         &address, &provider)
                 : etd2x_linker_probe_import_versioned(module, name, &out->version,
                                                       &address, &provider);
        if (rc == ETD2X_OK) {
            out->provider = provider;
            out->address = address;
            if (provider) {
                unsigned provider_type = out->type;
                Etd2xResult meta_rc = symbols_defined_symbol_type(
                    provider, name, &out->version, &provider_type);
                if (meta_rc != ETD2X_OK) return meta_rc;
                out->type = provider_type;
                ETD2X_LOGCI("LINKER", "symbol=%s requester=%s provider=%s address=%p",
                            name, module->path, provider->path, (void *)address);
            }
            return ETD2X_OK;
        }
        if (rc != ETD2X_ERR_SYMBOL_NOT_FOUND) {
            return rc;
        }
    }

    if (ELF64_ST_BIND(symbol->st_info) == STB_WEAK) {
        if (account_failure_stats) module->reloc_stats.weak_zero++;
        out->weak_zero = true;
        ETD2X_LOGCI("SYMBOL", "optional weak symbol absent %s -> 0", name[0] ? name : "<anonymous>");
        return ETD2X_OK;
    }

    if (account_failure_stats) module->reloc_stats.unresolved++;
    ETD2X_LOGCE("SYMBOL", "unresolved strong symbol name=%s index=%zu",
                name[0] ? name : "<anonymous>", symbol_index);
    return ETD2X_ERR_SYMBOL_UNRESOLVED;
}

Etd2xResult etd2x_symbol_resolve(Etd2xModule *module, size_t symbol_index,
                                 Etd2xResolvedSymbol *out) {
    return symbols_symbol_resolve_impl(module, symbol_index, out, true, true);
}

Etd2xResult etd2x_symbol_resolve_post_map(Etd2xModule *module, size_t symbol_index,
                                          Etd2xResolvedSymbol *out) {
    return symbols_symbol_resolve_impl(module, symbol_index, out, false, false);
}

#pragma pop_macro("STT_GNU_IFUNC")


/* ---- reloc aarch64 ---- */
#pragma push_macro("R_AARCH64_ABS64")
#pragma push_macro("R_AARCH64_GLOB_DAT")
#pragma push_macro("R_AARCH64_IRELATIVE")
#pragma push_macro("R_AARCH64_JUMP_SLOT")
#pragma push_macro("R_AARCH64_RELATIVE")
#pragma push_macro("R_AARCH64_TLSDESC")
#pragma push_macro("R_AARCH64_TLS_DTPMOD")
#pragma push_macro("R_AARCH64_TLS_DTPREL")
#pragma push_macro("R_AARCH64_TLS_TPREL")
#pragma push_macro("STT_GNU_IFUNC")






#include <elf.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef R_AARCH64_ABS64
#define R_AARCH64_ABS64 257
#endif
#ifndef R_AARCH64_GLOB_DAT
#define R_AARCH64_GLOB_DAT 1025
#endif
#ifndef R_AARCH64_JUMP_SLOT
#define R_AARCH64_JUMP_SLOT 1026
#endif
#ifndef R_AARCH64_RELATIVE
#define R_AARCH64_RELATIVE 1027
#endif
#ifndef R_AARCH64_TLS_DTPMOD
#define R_AARCH64_TLS_DTPMOD 1028
#endif
#ifndef R_AARCH64_TLS_DTPREL
#define R_AARCH64_TLS_DTPREL 1029
#endif
#ifndef R_AARCH64_TLS_TPREL
#define R_AARCH64_TLS_TPREL 1030
#endif
#ifndef R_AARCH64_TLSDESC
#define R_AARCH64_TLSDESC 1031
#endif
#ifndef R_AARCH64_IRELATIVE
#define R_AARCH64_IRELATIVE 1032
#endif
#ifndef STT_GNU_IFUNC
#define STT_GNU_IFUNC 10
#endif

#if defined(__aarch64__)
extern ptrdiff_t etd2x_tlsdesc_static_resolver(void *descriptor);
#else
static ptrdiff_t etd2x_tlsdesc_static_resolver(void *descriptor) {
    const ptrdiff_t *words = (const ptrdiff_t *)descriptor;
    return words ? words[1] : 0;
}
#endif

static const char *reloc_aarch64_reloc_name(unsigned type) {
    switch (type) {
        case R_AARCH64_NONE: return "R_AARCH64_NONE";
        case R_AARCH64_ABS64: return "R_AARCH64_ABS64";
        case R_AARCH64_GLOB_DAT: return "R_AARCH64_GLOB_DAT";
        case R_AARCH64_JUMP_SLOT: return "R_AARCH64_JUMP_SLOT";
        case R_AARCH64_RELATIVE: return "R_AARCH64_RELATIVE";
        case R_AARCH64_TLS_DTPMOD: return "R_AARCH64_TLS_DTPMOD";
        case R_AARCH64_TLS_DTPREL: return "R_AARCH64_TLS_DTPREL";
        case R_AARCH64_TLS_TPREL: return "R_AARCH64_TLS_TPREL";
        case R_AARCH64_TLSDESC: return "R_AARCH64_TLSDESC";
        case R_AARCH64_IRELATIVE: return "R_AARCH64_IRELATIVE";
        default: return "UNKNOWN";
    }
}

static Etd2xResult reloc_aarch64_resolve_for_relocation(Etd2xModule *module, const Elf64_Rela *rela,
                                           Etd2xResolvedSymbol *out) {
    const size_t symbol_index = (size_t)ELF64_R_SYM(rela->r_info);
    Etd2xResult rc = etd2x_symbol_resolve(module, symbol_index, out);
    if (rc != ETD2X_OK) {
        const Elf64_Sym *symbol = NULL;
        const char *name = "<invalid>";
        if (etd2x_symbol_get(module, symbol_index, &symbol, &name) != ETD2X_OK) {
            name = "<invalid>";
        }
        ETD2X_LOGCE("SYMBOL", "reloc=%s offset=%#llx index=%zu name=%s: %s",
                    reloc_aarch64_reloc_name((unsigned)ELF64_R_TYPE(rela->r_info)),
                    (unsigned long long)rela->r_offset,
                    symbol_index,
                    name,
                    etd2x_result_string(rc));
    }
    return rc;
}

typedef struct reloc_aarch64_Etd2xMissingImportList {
    const char **names;
    size_t count;
    size_t capacity;
    size_t reloc_count;
} reloc_aarch64_Etd2xMissingImportList;

static bool reloc_aarch64_missing_import_seen(const reloc_aarch64_Etd2xMissingImportList *list, const char *name) {
    for (size_t i = 0; i < list->count; ++i) {
        if (strcmp(list->names[i], name) == 0) return true;
    }
    return false;
}

static Etd2xResult reloc_aarch64_missing_import_add(reloc_aarch64_Etd2xMissingImportList *list, const char *name) {
    ++list->reloc_count;
    if (reloc_aarch64_missing_import_seen(list, name)) return ETD2X_OK;
    if (list->count == list->capacity) {
        size_t next = list->capacity ? list->capacity * 2u : 16u;
        if (next < list->capacity || next > SIZE_MAX / sizeof(*list->names)) {
            return ETD2X_ERR_NO_MEMORY;
        }
        const char **grown = (const char **)realloc(list->names, next * sizeof(*grown));
        if (!grown) return ETD2X_ERR_NO_MEMORY;
        list->names = grown;
        list->capacity = next;
    }
    list->names[list->count++] = name;
    return ETD2X_OK;
}

static Etd2xResult reloc_aarch64_preflight_rela_table(const Etd2xModule *module,
                                        const Elf64_Rela *table, size_t count,
                                        const char *label,
                                        reloc_aarch64_Etd2xMissingImportList *missing) {
    if (!count) return ETD2X_OK;
    if (!table) return ETD2X_ERR_RELOCATION_FORMAT;

    for (size_t i = 0; i < count; ++i) {
        const unsigned type = (unsigned)ELF64_R_TYPE(table[i].r_info);
        const bool normal_symbol = type == R_AARCH64_ABS64 || type == R_AARCH64_GLOB_DAT ||
                                   type == R_AARCH64_JUMP_SLOT;
        const bool tls_symbol = type == R_AARCH64_TLS_DTPMOD || type == R_AARCH64_TLS_DTPREL ||
                                type == R_AARCH64_TLS_TPREL || type == R_AARCH64_TLSDESC;
        if (!normal_symbol && !tls_symbol) continue;

        const size_t symbol_index = (size_t)ELF64_R_SYM(table[i].r_info);
        const char *name = "<invalid>";
        bool weak_zero = false;
        Etd2xResult rc = ETD2X_OK;
        if (tls_symbol) {
            const Elf64_Sym *symbol = NULL;
            if (etd2x_symbol_get(module, symbol_index, &symbol, &name) != ETD2X_OK)
                return ETD2X_ERR_SYMBOL_INDEX;
            const Etd2xModule *provider = NULL;
            size_t tls_offset = 0;
            rc = etd2x_symbol_resolve_tls(module, symbol_index, &provider, &tls_offset, NULL);
            weak_zero = rc == ETD2X_OK && provider == NULL;
        } else {
            rc = etd2x_symbol_probe(module, symbol_index, &name, &weak_zero);
        }
        if (rc == ETD2X_OK) continue;
        if (rc != ETD2X_ERR_SYMBOL_UNRESOLVED && rc != ETD2X_ERR_SYMBOL_NOT_FOUND) return rc;

        rc = reloc_aarch64_missing_import_add(missing, name && name[0] ? name : "<anonymous>");
        if (rc != ETD2X_OK) return rc;
        ETD2X_LOGCE("PREFLIGHT",
                    "unresolved strong import symbol=%s table=%s entry=%zu reloc=%s offset=%#llx",
                    name && name[0] ? name : "<anonymous>", label, i, reloc_aarch64_reloc_name(type),
                    (unsigned long long)table[i].r_offset);
    }
    return ETD2X_OK;
}

Etd2xResult etd2x_preflight_relocations(Etd2xModule *module, size_t *out_unique_missing) {
    if (!module || module->state < ETD2X_MODULE_DYNAMIC_PARSED) return ETD2X_ERR_ARGUMENT;

    reloc_aarch64_Etd2xMissingImportList missing = {0};
    Etd2xResult rc = reloc_aarch64_preflight_rela_table(module, module->dynamic.android_rela,
                                          module->dynamic.android_rela_count,
                                          "DT_ANDROID_RELA", &missing);
    if (rc == ETD2X_OK) {
        rc = reloc_aarch64_preflight_rela_table(module, module->dynamic.rela,
                                  module->dynamic.rela_count, "DT_RELA", &missing);
    }
    if (rc == ETD2X_OK) {
        rc = reloc_aarch64_preflight_rela_table(module, module->dynamic.jmprel,
                                  module->dynamic.jmprel_count, "DT_JMPREL", &missing);
    }

    if (out_unique_missing) *out_unique_missing = missing.count;
    if (rc == ETD2X_OK && missing.count != 0) {
        module->reloc_stats.unresolved = missing.reloc_count;
        ETD2X_LOGCE("PREFLIGHT",
                    "module=%s unresolved_unique=%zu unresolved_relocations=%zu",
                    module->path, missing.count, missing.reloc_count);
        rc = ETD2X_ERR_SYMBOL_UNRESOLVED;
    } else if (rc == ETD2X_OK) {
        ETD2X_LOGCI("PREFLIGHT", "module=%s imports resolved", module->path);
    }

    free(missing.names);
    return rc;
}

static Etd2xResult reloc_aarch64_apply_one_rela(Etd2xModule *module, const Elf64_Rela *rela) {
    const unsigned type = (unsigned)ELF64_R_TYPE(rela->r_info);
    const size_t symbol_index = (size_t)ELF64_R_SYM(rela->r_info);

    if (type == R_AARCH64_NONE) {
        return ETD2X_OK;
    }

    const size_t target_size = type == R_AARCH64_TLSDESC ? 2u * sizeof(uint64_t) : sizeof(uint64_t);
    if (!etd2x_module_backing_contains(module, rela->r_offset, target_size)) {
        ETD2X_LOGCE("RELOC", "type=%s offset=%#llx target outside module",
                    reloc_aarch64_reloc_name(type), (unsigned long long)rela->r_offset);
        return ETD2X_ERR_RELOCATION_TARGET;
    }

    uint64_t *target = (uint64_t *)etd2x_module_backing_address(module, rela->r_offset);

    switch (type) {
        case R_AARCH64_RELATIVE:
            if (symbol_index != 0) {
                ETD2X_LOGCW("RELOC", "RELATIVE relocation unexpectedly references symbol index=%zu", symbol_index);
            }
            *target = (uint64_t)module->load_bias + (uint64_t)rela->r_addend;
            module->reloc_stats.relative++;
            return ETD2X_OK;

        case R_AARCH64_IRELATIVE:
            /* The addend names executable resolver code. Android defers IFUNC/
             * IRELATIVE resolver execution until ordinary relocations are done
             * and code is mapped executable. The post-map pass below owns it. */
            return ETD2X_OK;

        case R_AARCH64_ABS64:
        case R_AARCH64_GLOB_DAT:
        case R_AARCH64_JUMP_SLOT: {
            Etd2xResolvedSymbol resolved;
            Etd2xResult rc = reloc_aarch64_resolve_for_relocation(module, rela, &resolved);
            if (rc != ETD2X_OK) {
                return rc;
            }
            *target = (uint64_t)resolved.address + (uint64_t)rela->r_addend;

            if (type == R_AARCH64_ABS64) module->reloc_stats.abs64++;
            else if (type == R_AARCH64_GLOB_DAT) module->reloc_stats.glob_dat++;
            else module->reloc_stats.jump_slot++;
            return ETD2X_OK;
        }

        case R_AARCH64_TLS_DTPMOD:
        case R_AARCH64_TLS_DTPREL:
        case R_AARCH64_TLS_TPREL:
        case R_AARCH64_TLSDESC: {
            const Etd2xModule *provider = NULL;
            size_t symbol_offset = 0;
            Etd2xResult rc = etd2x_symbol_resolve_tls(
                module, symbol_index, &provider, &symbol_offset, NULL);
            if (rc != ETD2X_OK) return rc;
            if (!provider) {
                if (type == R_AARCH64_TLSDESC) {
                    target[0] = (uint64_t)(uintptr_t)&etd2x_tlsdesc_static_resolver;
                    target[1] = 0;
                } else {
                    *target = 0;
                }
                module->reloc_stats.weak_zero++;
                return ETD2X_OK;
            }
            if (!provider->tls.present || !provider->tls.registered || provider->tls.module_id == 0) {
                ETD2X_LOGCE("TLS", "unregistered TLS provider=%s reloc=%s",
                            provider->path, reloc_aarch64_reloc_name(type));
                return ETD2X_ERR_RELOCATION_FORMAT;
            }

            const __int128 dtprel = (__int128)symbol_offset + (__int128)rela->r_addend;
            const __int128 tprel = (__int128)provider->tls.static_offset + dtprel;
            if (dtprel < INT64_MIN || dtprel > INT64_MAX || tprel < INT64_MIN || tprel > INT64_MAX) {
                return ETD2X_ERR_RELOCATION_TARGET;
            }

            if (type == R_AARCH64_TLS_DTPMOD) {
                *target = (uint64_t)provider->tls.module_id;
                module->reloc_stats.tls_dtpmod++;
            } else if (type == R_AARCH64_TLS_DTPREL) {
                *target = (uint64_t)(int64_t)dtprel;
                module->reloc_stats.tls_dtprel++;
            } else if (type == R_AARCH64_TLS_TPREL) {
                *target = (uint64_t)(int64_t)tprel;
                module->reloc_stats.tls_tprel++;
            } else {
                target[0] = (uint64_t)(uintptr_t)&etd2x_tlsdesc_static_resolver;
                target[1] = (uint64_t)(int64_t)tprel;
                module->reloc_stats.tlsdesc++;
            }
            ETD2X_LOGCI("TLS", "reloc=%s requester=%s provider=%s moduleId=%zu symOff=%zu tprel=%lld",
                        reloc_aarch64_reloc_name(type), module->path, provider->path, provider->tls.module_id,
                        symbol_offset, (long long)(int64_t)tprel);
            return ETD2X_OK;
        }

        default:
            module->reloc_stats.unsupported++;
            ETD2X_LOGCE("RELOC",
                        "unsupported type=%u name=%s offset=%#llx symbol_index=%zu",
                        type, reloc_aarch64_reloc_name(type),
                        (unsigned long long)rela->r_offset, symbol_index);
            return ETD2X_ERR_RELOCATION_UNSUPPORTED;
    }
}

static Etd2xResult reloc_aarch64_apply_rela_table(Etd2xModule *module, const Elf64_Rela *table,
                                     size_t count, const char *label) {
    if (!count) {
        return ETD2X_OK;
    }
    if (!table) {
        return ETD2X_ERR_RELOCATION_FORMAT;
    }

    ETD2X_LOGCI("RELOC", "%s entries=%zu", label, count);
    for (size_t i = 0; i < count; ++i) {
        Etd2xResult rc = reloc_aarch64_apply_one_rela(module, &table[i]);
        if (rc != ETD2X_OK) {
            ETD2X_LOGCE("RELOC", "%s entry=%zu failed: %s",
                        label, i, etd2x_result_string(rc));
            return rc;
        }
    }
    return ETD2X_OK;
}

/* Post-map relocation metadata parsed from DT_RELA/DT_JMPREL points into
 * module->backing.  Once Horizon has mapped that storage as process code
 * memory, the backing alias must no longer be dereferenced.  Android packed
 * RELA is different: it is decoded into an owned heap array and therefore
 * remains a valid host pointer. */
static const Elf64_Rela *reloc_aarch64_post_map_rela_entry(const Etd2xModule *module,
                                              const Elf64_Rela *table,
                                              size_t index) {
    if (!module || !table || !module->mapped || !module->runtime_base) return NULL;
    if (index > SIZE_MAX / sizeof(Elf64_Rela)) return NULL;

    const uintptr_t table_addr = (uintptr_t)table;
    const uintptr_t backing_begin = (uintptr_t)module->backing;
    const uintptr_t backing_end = backing_begin + module->image_size;
    const size_t delta = index * sizeof(Elf64_Rela);

    if (table_addr >= backing_begin && table_addr < backing_end) {
        const size_t table_offset = (size_t)(table_addr - backing_begin);
        if (delta > module->image_size - table_offset) return NULL;
        const size_t entry_offset = table_offset + delta;
        if (sizeof(Elf64_Rela) > module->image_size - entry_offset) return NULL;

        const uintptr_t runtime = (uintptr_t)module->runtime_base + entry_offset;
        if (!etd2x_module_runtime_contains(module, runtime, sizeof(Elf64_Rela))) return NULL;
        return (const Elf64_Rela *)runtime;
    }

    /* APS2/DT_ANDROID_RELA is decoded into separately owned host memory. */
    return &table[index];
}

static uint64_t *reloc_aarch64_post_map_target(Etd2xModule *module, uint64_t elf_vaddr, size_t size) {
    if (!module || !module->mapped || size == 0) return NULL;
    void *address = etd2x_module_runtime_address(module, elf_vaddr);
    if (!address || !etd2x_module_runtime_contains(module, (uintptr_t)address, size)) return NULL;
    return (uint64_t *)address;
}

static Etd2xResult reloc_aarch64_apply_post_map_rela(Etd2xModule *module, const Elf64_Rela *rela) {
    const unsigned type = (unsigned)ELF64_R_TYPE(rela->r_info);
    const size_t symbol_index = (size_t)ELF64_R_SYM(rela->r_info);

    if (type == R_AARCH64_IRELATIVE) {
        uint64_t *target = reloc_aarch64_post_map_target(module, rela->r_offset, sizeof(uint64_t));
        if (!target) return ETD2X_ERR_RELOCATION_TARGET;
        const uintptr_t resolver_address = module->load_bias + (uintptr_t)rela->r_addend;
        if (!etd2x_module_runtime_contains(module, resolver_address, sizeof(uint32_t))) {
            ETD2X_LOGCE("IFUNC", "IRELATIVE resolver outside module=%s resolver=%p",
                        module->path, (void *)resolver_address);
            return ETD2X_ERR_RELOCATION_TARGET;
        }
        const uintptr_t implementation = etd2x_call_android_ifunc_resolver(resolver_address);
        *target = (uint64_t)implementation;
        armDCacheFlush(target, sizeof(*target));
        module->reloc_stats.irelative++;
        ETD2X_LOGCI("IFUNC", "IRELATIVE module=%s offset=%#llx resolver=%p implementation=%p",
                    module->path, (unsigned long long)rela->r_offset,
                    (void *)resolver_address, (void *)implementation);
        return ETD2X_OK;
    }

    if (type != R_AARCH64_ABS64 && type != R_AARCH64_GLOB_DAT &&
        type != R_AARCH64_JUMP_SLOT) return ETD2X_OK;

    Etd2xResolvedSymbol resolved = {0};
    /* Ordinary relocation already performed and accounted this lookup before
     * mapping. The deferred pass only needs to discover/execute GNU IFUNC; do
     * not count compatibility bindings (or weak/unresolved stats) twice. */
    Etd2xResult rc = etd2x_symbol_resolve_post_map(module, symbol_index, &resolved);
    if (rc != ETD2X_OK) return rc;
    if (resolved.type != STT_GNU_IFUNC) return ETD2X_OK;
    uint64_t *target = reloc_aarch64_post_map_target(module, rela->r_offset, sizeof(uint64_t));
    if (!target) return ETD2X_ERR_RELOCATION_TARGET;
    *target = (uint64_t)resolved.address + (uint64_t)rela->r_addend;
    armDCacheFlush(target, sizeof(*target));
    module->reloc_stats.ifunc++;
    ETD2X_LOGCI("IFUNC", "symbol relocation module=%s symbol=%s offset=%#llx implementation=%p",
                module->path, resolved.name ? resolved.name : "<anonymous>",
                (unsigned long long)rela->r_offset, (void *)resolved.address);
    return ETD2X_OK;
}

static Etd2xResult reloc_aarch64_apply_post_map_table(Etd2xModule *module, const Elf64_Rela *table,
                                        size_t count, const char *label) {
    if (!count) return ETD2X_OK;
    if (!table) return ETD2X_ERR_RELOCATION_FORMAT;
    for (size_t i = 0; i < count; ++i) {
        const Elf64_Rela *rela = reloc_aarch64_post_map_rela_entry(module, table, i);
        if (!rela) {
            ETD2X_LOGCE("IFUNC", "%s entry=%zu unavailable in mapped runtime view", label, i);
            return ETD2X_ERR_RELOCATION_FORMAT;
        }
        Etd2xResult rc = reloc_aarch64_apply_post_map_rela(module, rela);
        if (rc != ETD2X_OK) {
            ETD2X_LOGCE("IFUNC", "%s entry=%zu failed: %s",
                        label, i, etd2x_result_string(rc));
            return rc;
        }
    }
    return ETD2X_OK;
}

Etd2xResult etd2x_apply_post_map_relocations(Etd2xModule *module) {
    if (!module || !module->mapped || module->state < ETD2X_MODULE_MAPPED)
        return ETD2X_ERR_ARGUMENT;
    if (module->post_map_relocations_applied) return ETD2X_OK;

    Etd2xResult rc = reloc_aarch64_apply_post_map_table(module, module->dynamic.android_rela,
                                          module->dynamic.android_rela_count,
                                          "DT_ANDROID_RELA");
    if (rc == ETD2X_OK) {
        rc = reloc_aarch64_apply_post_map_table(module, module->dynamic.rela,
                                  module->dynamic.rela_count, "DT_RELA");
    }
    if (rc == ETD2X_OK) {
        rc = reloc_aarch64_apply_post_map_table(module, module->dynamic.jmprel,
                                  module->dynamic.jmprel_count, "DT_JMPREL");
    }
    if (rc != ETD2X_OK) return rc;
    module->post_map_relocations_applied = true;
    ETD2X_LOGCI("IFUNC", "post-map complete module=%s IRELATIVE=%zu IFUNC=%zu",
                module->path, module->reloc_stats.irelative, module->reloc_stats.ifunc);
    return ETD2X_OK;
}

Etd2xResult etd2x_apply_relocations(Etd2xModule *module) {
    if (!module || module->state < ETD2X_MODULE_DYNAMIC_PARSED) {
        return ETD2X_ERR_ARGUMENT;
    }

    module->state = ETD2X_MODULE_RELOCATING;

    /* Resolve the complete import surface before mutating any relocation
     * target. This turns hardware runs into one-shot compatibility reports
     * instead of failing at the first missing PLT entry. */
    size_t unique_missing = 0;
    Etd2xResult rc = etd2x_preflight_relocations(module, &unique_missing);
    if (rc != ETD2X_OK) {
        module->state = ETD2X_MODULE_FAILED;
        return rc;
    }

    /* Bionic processes Android packed relocations before RELR and plain RELA. */
    rc = reloc_aarch64_apply_rela_table(module, module->dynamic.android_rela,
                          module->dynamic.android_rela_count, "DT_ANDROID_RELA");
    if (rc != ETD2X_OK) {
        module->state = ETD2X_MODULE_FAILED;
        return rc;
    }

    rc = etd2x_apply_relr(module);
    if (rc != ETD2X_OK) {
        module->state = ETD2X_MODULE_FAILED;
        return rc;
    }

    rc = reloc_aarch64_apply_rela_table(module, module->dynamic.rela, module->dynamic.rela_count, "DT_RELA");
    if (rc != ETD2X_OK) {
        module->state = ETD2X_MODULE_FAILED;
        return rc;
    }

    rc = reloc_aarch64_apply_rela_table(module, module->dynamic.jmprel, module->dynamic.jmprel_count, "DT_JMPREL");
    if (rc != ETD2X_OK) {
        module->state = ETD2X_MODULE_FAILED;
        return rc;
    }

    const size_t total = module->reloc_stats.relative + module->reloc_stats.glob_dat +
                         module->reloc_stats.jump_slot + module->reloc_stats.abs64 +
                         module->reloc_stats.relr + module->reloc_stats.tls_dtpmod +
                         module->reloc_stats.tls_dtprel + module->reloc_stats.tls_tprel +
                         module->reloc_stats.tlsdesc + module->reloc_stats.irelative +
                         module->reloc_stats.ifunc;

    ETD2X_LOGCI("RELOC",
                "summary RELATIVE=%zu GLOB_DAT=%zu JUMP_SLOT=%zu ABS64=%zu RELR=%zu TLS_DTPMOD=%zu TLS_DTPREL=%zu TLS_TPREL=%zu TLSDESC=%zu IRELATIVE=%zu IFUNC=%zu total=%zu",
                module->reloc_stats.relative,
                module->reloc_stats.glob_dat,
                module->reloc_stats.jump_slot,
                module->reloc_stats.abs64,
                module->reloc_stats.relr,
                module->reloc_stats.tls_dtpmod,
                module->reloc_stats.tls_dtprel,
                module->reloc_stats.tls_tprel,
                module->reloc_stats.tlsdesc,
                module->reloc_stats.irelative,
                module->reloc_stats.ifunc,
                total);

    module->state = ETD2X_MODULE_RELOCATED;
    return ETD2X_OK;
}

#pragma pop_macro("STT_GNU_IFUNC")
#pragma pop_macro("R_AARCH64_TLS_TPREL")
#pragma pop_macro("R_AARCH64_TLS_DTPREL")
#pragma pop_macro("R_AARCH64_TLS_DTPMOD")
#pragma pop_macro("R_AARCH64_TLSDESC")
#pragma pop_macro("R_AARCH64_RELATIVE")
#pragma pop_macro("R_AARCH64_JUMP_SLOT")
#pragma pop_macro("R_AARCH64_IRELATIVE")
#pragma pop_macro("R_AARCH64_GLOB_DAT")
#pragma pop_macro("R_AARCH64_ABS64")


/* ---- relr ---- */



#include <stdint.h>

static Etd2xResult relr_apply_one_relr(Etd2xModule *module, uint64_t guest_vaddr) {
    if (!etd2x_module_backing_contains(module, guest_vaddr, sizeof(uint64_t))) {
        ETD2X_LOGCE("RELR", "target=%#llx outside guest image", (unsigned long long)guest_vaddr);
        return ETD2X_ERR_RELOCATION_TARGET;
    }

    uint64_t *target = (uint64_t *)etd2x_module_backing_address(module, guest_vaddr);
    if (UINT64_MAX - *target < module->load_bias) {
        return ETD2X_ERR_RELR_INVALID;
    }
    *target += (uint64_t)module->load_bias;
    module->reloc_stats.relr++;
    return ETD2X_OK;
}

Etd2xResult etd2x_apply_relr(Etd2xModule *module) {
    if (!module) {
        return ETD2X_ERR_ARGUMENT;
    }
    if (!module->dynamic.relr_count) {
        return ETD2X_OK;
    }

    ETD2X_LOGCI("RELR", "table=%s entries=%zu",
                module->dynamic.relr_is_android ? "DT_ANDROID_RELR" : "DT_RELR",
                module->dynamic.relr_count);

    uint64_t where = 0;
    bool have_base = false;

    for (size_t i = 0; i < module->dynamic.relr_count; ++i) {
        const Elf64_Xword entry = module->dynamic.relr[i];

        if ((entry & 1ULL) == 0) {
            where = (uint64_t)entry;
            Etd2xResult rc = relr_apply_one_relr(module, where);
            if (rc != ETD2X_OK) {
                ETD2X_LOGCE("RELR", "entry=%zu direct=%#llx failed", i, (unsigned long long)entry);
                return rc;
            }
            if (where > UINT64_MAX - sizeof(uint64_t)) {
                return ETD2X_ERR_RELR_INVALID;
            }
            where += sizeof(uint64_t);
            have_base = true;
            continue;
        }

        if (!have_base) {
            ETD2X_LOGCE("RELR", "bitmap entry=%zu appeared before a direct address", i);
            return ETD2X_ERR_RELR_INVALID;
        }

        for (unsigned bit = 1; bit < 64; ++bit) {
            if (entry & (1ULL << bit)) {
                const uint64_t delta = (uint64_t)(bit - 1U) * sizeof(uint64_t);
                if (where > UINT64_MAX - delta) {
                    return ETD2X_ERR_RELR_INVALID;
                }
                Etd2xResult rc = relr_apply_one_relr(module, where + delta);
                if (rc != ETD2X_OK) {
                    ETD2X_LOGCE("RELR", "entry=%zu bitmap=%#llx bit=%u failed",
                                i, (unsigned long long)entry, bit);
                    return rc;
                }
            }
        }

        const uint64_t advance = 63ULL * sizeof(uint64_t);
        if (where > UINT64_MAX - advance) {
            return ETD2X_ERR_RELR_INVALID;
        }
        where += advance;
    }

    ETD2X_LOGCI("RELR", "applied=%zu", module->reloc_stats.relr);
    return ETD2X_OK;
}



/* ---- ifunc ---- */
#pragma push_macro("ETD2X_AT_HWCAP")
#pragma push_macro("ETD2X_AT_HWCAP2")
#pragma push_macro("ETD2X_IFUNC_ARG_HWCAP")




#include <stddef.h>
#include <stdint.h>

/* Linux arm64 auxv tags used by Android/Bionic. */
#define ETD2X_AT_HWCAP  16ul
#define ETD2X_AT_HWCAP2 26ul
#define ETD2X_IFUNC_ARG_HWCAP (UINT64_C(1) << 62)

typedef struct ifunc_Etd2xAndroidIfuncArg {
    unsigned long size;
    unsigned long hwcap;
    unsigned long hwcap2;
} ifunc_Etd2xAndroidIfuncArg;

uintptr_t etd2x_call_android_ifunc_resolver(uintptr_t resolver_address) {
    if (!resolver_address) return 0;

#if defined(__aarch64__)
    /* Android's arm64 IFUNC calling convention changed at API 30. Before API
     * 30 the resolver receives no arguments. Starting at API 30 Bionic passes
     * (AT_HWCAP | _IFUNC_ARG_HWCAP, __ifunc_arg_t*). ETD2X exposes API 30 by
     * default, so calling a resolver as resolver(void) is an ABI violation and
     * can make the resolver dereference garbage in x1. */
    if (etd2x_android_compat_platform_at_least(30)) {
        ifunc_Etd2xAndroidIfuncArg arg = {
            .size = sizeof(ifunc_Etd2xAndroidIfuncArg),
            .hwcap = etd2x_bionic_getauxval(ETD2X_AT_HWCAP),
            .hwcap2 = etd2x_bionic_getauxval(ETD2X_AT_HWCAP2),
        };
        typedef uintptr_t (*ifunc_Etd2xIfuncResolverApi30)(uint64_t, ifunc_Etd2xAndroidIfuncArg *);
        ifunc_Etd2xIfuncResolverApi30 resolver = (ifunc_Etd2xIfuncResolverApi30)resolver_address;
        return resolver((uint64_t)arg.hwcap | ETD2X_IFUNC_ARG_HWCAP, &arg);
    }
#endif

    typedef uintptr_t (*ifunc_Etd2xIfuncResolverLegacy)(void);
    return ((ifunc_Etd2xIfuncResolverLegacy)resolver_address)();
}

#pragma pop_macro("ETD2X_IFUNC_ARG_HWCAP")
#pragma pop_macro("ETD2X_AT_HWCAP2")
#pragma pop_macro("ETD2X_AT_HWCAP")


/* ---- vm space ---- */
#pragma push_macro("VM_LARGE_RANGE")


#ifdef __SWITCH__
#include <stdint.h>
#include <stdlib.h>
#include <switch.h>


/* Ranges at least this large are placed top-down; smaller ones bottom-up. */
#define VM_LARGE_RANGE ((uint64_t)1 << 30)

struct Etd2xVmRange {
    uintptr_t base;
    size_t len;
    VirtmemReservation *reservation;
};

/* Every live ETD2X reservation; guarded by the libnx virtmem mutex, which
 * also serializes placement against libnx's own address-space users. */
static Etd2xVmRange **vm_space_g_ranges;
static size_t vm_space_g_range_count, vm_space_g_range_cap;

static bool vm_space_vm_registry_add(Etd2xVmRange *range) {
    if (vm_space_g_range_count == vm_space_g_range_cap) {
        const size_t cap = vm_space_g_range_cap ? vm_space_g_range_cap * 2u : 64u;
        Etd2xVmRange **grown = realloc(vm_space_g_ranges, cap * sizeof(*grown));
        if (!grown) return false;
        vm_space_g_ranges = grown;
        vm_space_g_range_cap = cap;
    }
    vm_space_g_ranges[vm_space_g_range_count++] = range;
    return true;
}

static void vm_space_vm_registry_remove(Etd2xVmRange *range) {
    for (size_t i = 0; i < vm_space_g_range_count; ++i) {
        if (vm_space_g_ranges[i] != range) continue;
        vm_space_g_ranges[i] = vm_space_g_ranges[--vm_space_g_range_count];
        return;
    }
}

typedef struct vm_space_VmLayout {
    uintptr_t aslr_lo, aslr_hi;
    uintptr_t fixed[3][2]; /* alias, heap and stack regions */
} vm_space_VmLayout;

static bool vm_space_vm_layout(vm_space_VmLayout *out) {
    u64 v[8];
    static const InfoType kinds[8] = {
        InfoType_AslrRegionAddress, InfoType_AslrRegionSize,
        InfoType_AliasRegionAddress, InfoType_AliasRegionSize,
        InfoType_HeapRegionAddress, InfoType_HeapRegionSize,
        InfoType_StackRegionAddress, InfoType_StackRegionSize,
    };
    for (size_t i = 0; i < 8; ++i)
        if (R_FAILED(svcGetInfo(&v[i], kinds[i], CUR_PROCESS_HANDLE, 0))) return false;
    out->aslr_lo = (uintptr_t)v[0];
    out->aslr_hi = (uintptr_t)(v[0] + v[1]);
    for (size_t r = 0; r < 3; ++r) {
        out->fixed[r][0] = (uintptr_t)v[2 + 2 * r];
        out->fixed[r][1] = (uintptr_t)(v[2 + 2 * r] + v[3 + 2 * r]);
    }
    return true;
}

/* Within the unmapped run [lo, run_hi), the end of the free span starting at
 * lo, or lo itself when an obstacle covers lo (*skip_to then names where the
 * covering obstacle ends). *next_lo is where the following span starts. */
static uintptr_t vm_space_vm_free_span_end(const vm_space_VmLayout *layout, uintptr_t lo, uintptr_t run_hi,
                                  uintptr_t *skip_to, uintptr_t *next_lo) {
    uintptr_t hi = run_hi;
    *skip_to = 0;
    *next_lo = run_hi;
    const size_t count = 3u + vm_space_g_range_count;
    for (size_t o = 0; o < count; ++o) {
        const uintptr_t o0 = o < 3u ? layout->fixed[o][0] : vm_space_g_ranges[o - 3u]->base;
        const uintptr_t o1 = o < 3u ? layout->fixed[o][1] : vm_space_g_ranges[o - 3u]->base + vm_space_g_ranges[o - 3u]->len;
        if (o0 >= o1) continue;
        if (o0 <= lo && o1 > lo) {
            if (o1 > *skip_to) *skip_to = o1;
        } else if (o0 > lo && o0 < hi) {
            hi = o0;
            *next_lo = o1;
        }
    }
    return *skip_to ? lo : hi;
}

static uintptr_t vm_space_vm_place_locked(const vm_space_VmLayout *layout, size_t len, size_t align) {
    const bool top_down = (uint64_t)len >= VM_LARGE_RANGE;
    uintptr_t best = 0;
    for (uintptr_t cur = layout->aslr_lo; cur < layout->aslr_hi;) {
        MemoryInfo info;
        u32 page_info = 0;
        if (R_FAILED(svcQueryMemory(&info, &page_info, cur)) || info.size == 0) break;
        const uintptr_t run_lo = cur;
        uintptr_t run_hi = (uintptr_t)(info.addr + info.size);
        if (run_hi > layout->aslr_hi || run_hi <= run_lo) run_hi = layout->aslr_hi;
        cur = run_hi;
        if (info.type != MemType_Unmapped || run_hi - run_lo < len) continue;
        for (uintptr_t lo = run_lo; lo < run_hi && run_hi - lo >= len;) {
            uintptr_t skip_to = 0, next_lo = run_hi;
            const uintptr_t hi = vm_space_vm_free_span_end(layout, lo, run_hi, &skip_to, &next_lo);
            if (skip_to) { lo = skip_to; continue; }
            if (hi > lo && hi - lo >= len) {
                if (top_down) {
                    const uintptr_t base = (hi - len) & ~(uintptr_t)(align - 1u);
                    if (base >= lo && base > best) best = base;
                } else {
                    const uintptr_t base = (lo + align - 1u) & ~(uintptr_t)(align - 1u);
                    if (base >= lo && base <= hi - len) return base;
                }
            }
            if (next_lo <= lo) break;
            lo = next_lo;
        }
    }
    return best;
}

void *etd2x_vm_reserve(size_t len, size_t align, Etd2xVmRange **out_range) {
    if (out_range) *out_range = NULL;
    if (!out_range || !len || (len & 0xFFFu) || align < 0x1000u || (align & (align - 1u))) return NULL;
    Etd2xVmRange *range = calloc(1, sizeof(*range));
    if (!range) return NULL;
    vm_space_VmLayout layout;
    uintptr_t base = 0;
    virtmemLock();
    if (vm_space_vm_layout(&layout)) base = vm_space_vm_place_locked(&layout, len, align);
    if (base) range->reservation = virtmemAddReservation((void *)base, len);
    if (base && range->reservation) {
        range->base = base;
        range->len = len;
        if (!vm_space_vm_registry_add(range)) {
            virtmemRemoveReservation(range->reservation);
            range->reservation = NULL;
        }
    }
    virtmemUnlock();
    if (!range->reservation) {
        free(range);
        if ((uint64_t)len >= VM_LARGE_RANGE)
            ETD2X_LOGCW("VM", "address-space reservation failed len=%zu align=%zu", len, align);
        return NULL;
    }
    if ((uint64_t)len >= VM_LARGE_RANGE)
        ETD2X_LOGCI("VM", "address-space reservation base=%p len=%zu align=%zu",
                           (void *)base, len, align);
    *out_range = range;
    return (void *)base;
}

bool etd2x_vm_shrink(Etd2xVmRange *range, void *base, size_t len) {
    const uintptr_t b = (uintptr_t)base;
    if (!range || !len || b < range->base || b + len > range->base + range->len || b + len < b) return false;
    if (b == range->base && len == range->len) return true;
    bool ok = false;
    virtmemLock();
    VirtmemReservation *replacement = virtmemAddReservation(base, len);
    if (replacement) {
        virtmemRemoveReservation(range->reservation);
        range->reservation = replacement;
        range->base = b;
        range->len = len;
        ok = true;
    }
    virtmemUnlock();
    return ok;
}

void etd2x_vm_release(Etd2xVmRange *range) {
    if (!range) return;
    virtmemLock();
    vm_space_vm_registry_remove(range);
    if (range->reservation) virtmemRemoveReservation(range->reservation);
    virtmemUnlock();
    free(range);
}
#endif

#pragma pop_macro("VM_LARGE_RANGE")
