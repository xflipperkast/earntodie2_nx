#pragma once
#include "runtime_base.h"
#ifdef __SWITCH__


/* Interface: module */


#include <elf.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <switch.h>


struct Etd2xLinker;

typedef enum Etd2xModuleState {
    ETD2X_MODULE_EMPTY = 0,
    ETD2X_MODULE_LOADED,
    ETD2X_MODULE_LAYOUT_READY,
    ETD2X_MODULE_RESERVED,
    ETD2X_MODULE_DYNAMIC_PARSED,
    ETD2X_MODULE_RELOCATING,
    ETD2X_MODULE_RELOCATED,
    ETD2X_MODULE_MAPPED,
    ETD2X_MODULE_CONSTRUCTED,
    ETD2X_MODULE_FINALIZED,
    ETD2X_MODULE_FAILED
} Etd2xModuleState;

typedef struct Etd2xRelocStats {
    size_t relative;
    size_t glob_dat;
    size_t jump_slot;
    size_t abs64;
    size_t relr;
    size_t tls_dtpmod;
    size_t tls_dtprel;
    size_t tls_tprel;
    size_t tlsdesc;
    size_t irelative;
    size_t ifunc;
    size_t weak_zero;
    size_t unresolved;
    size_t unsupported;
} Etd2xRelocStats;

typedef struct Etd2xTlsInfo {
    bool present;
    uint64_t vaddr;
    size_t file_size;
    size_t mem_size;
    size_t alignment;
    size_t module_id;
    ptrdiff_t static_offset;
    bool registered;
} Etd2xTlsInfo;

typedef struct Etd2xDynamicInfo {
    /*
     * Keep ELF virtual addresses as the canonical dynamic symbol/string-table
     * locations. Readers select the writable backing view before mapping and
     * the runtime alias after Horizon code-memory mapping.
     */
    uint64_t strtab_vaddr;
    size_t strsz;

    uint64_t symtab_vaddr;
    size_t syment;
    size_t symbol_count;

    Elf64_Rela *rela;
    size_t rela_count;

    /* Android packed RELA (APS2) is decoded at dynamic-parse time into owned
     * Elf64_Rela entries so the normal relocation engine remains canonical. */
    Elf64_Rela *android_rela;
    size_t android_rela_count;

    Elf64_Rela *jmprel;
    size_t jmprel_count;

    Elf64_Xword *relr;
    size_t relr_count;
    bool relr_is_android;

    uint64_t init_vaddr;
    uint64_t init_array_vaddr;
    size_t init_array_count;
    uint64_t fini_vaddr;
    uint64_t fini_array_vaddr;
    size_t fini_array_count;

    uint64_t hash_vaddr;
    uint64_t gnu_hash_vaddr;

    uint64_t versym_vaddr;
    uint64_t verneed_vaddr;
    size_t verneed_count;
    uint64_t verdef_vaddr;
    size_t verdef_count;

    uint32_t *needed_name_offsets;
    size_t needed_count;
    uint32_t soname_offset;

    /* Dynamic-linker policy bits. Android/Bionic uses DT_FLAGS_1/DF_1_GLOBAL
     * to build the process-wide global lookup group, and DT_SYMBOLIC or
     * DT_FLAGS/DF_SYMBOLIC to request local-first binding for definitions in
     * this DSO. Keep the raw flag words so the linker can reproduce that
     * policy without title-specific exceptions. */
    uint64_t flags;
    uint64_t flags_1;
    bool symbolic;
} Etd2xDynamicInfo;

typedef struct Etd2xModule {
    char path[0x200];

    void *file_image;
    size_t file_size;

    Elf64_Ehdr *ehdr;
    Elf64_Phdr *phdrs;
    Elf64_Shdr *shdrs;

    uint64_t min_vaddr;
    uint64_t max_vaddr;
    size_t image_size;

    void *backing;
    void *runtime_base;
    uintptr_t load_bias;
    struct Etd2xVmRange *reservation;
    bool mapped;
    bool post_map_relocations_applied;

    struct Etd2xLinker *linker;

    Etd2xDynamicInfo dynamic;
    Etd2xTlsInfo tls;
    Etd2xRelocStats reloc_stats;
    Etd2xModuleState state;
} Etd2xModule;

void etd2x_module_init(Etd2xModule *module);
void etd2x_module_unload(Etd2xModule *module);

void *etd2x_module_runtime_address(const Etd2xModule *module, uint64_t elf_vaddr);
void *etd2x_module_backing_address(const Etd2xModule *module, uint64_t elf_vaddr);
bool etd2x_module_backing_contains(const Etd2xModule *module, uint64_t elf_vaddr, size_t size);
bool etd2x_module_runtime_contains(const Etd2xModule *module, uintptr_t runtime_address, size_t size);


/* Interface: dynamic */


#include <stdbool.h>



Etd2xResult etd2x_dynamic_parse(Etd2xModule *module);
Etd2xResult etd2x_dynamic_run_constructors(Etd2xModule *module);
Etd2xResult etd2x_dynamic_run_destructors(Etd2xModule *module);

/* Enable verbose AArch64 constructor tracing around real-APK bring-up.
 * Kept off for the retained Stage 1-12 fixtures so diagnostics do not alter
 * their normal output or timing. */
void etd2x_dynamic_set_constructor_trace(bool enabled);

Etd2xResult etd2x_dynamic_get_needed(const Etd2xModule *module, size_t index, const char **out_name);
Etd2xResult etd2x_dynamic_get_soname(const Etd2xModule *module, const char **out_name);


/* Interface: symbols */


#include <elf.h>
#include <stdint.h>



typedef struct Etd2xSymbolVersion {
    const char *name;
    const char *provider; /* DT_VERNEED vn_file for imported versions. */
    uint32_t hash;
    bool hidden;
} Etd2xSymbolVersion;

typedef struct Etd2xResolvedSymbol {
    uintptr_t address;
    const char *name;
    Etd2xSymbolVersion version;
    const Etd2xModule *provider;
    unsigned type;
    bool weak_zero;
} Etd2xResolvedSymbol;

Etd2xResult etd2x_symbol_get(const Etd2xModule *module, size_t symbol_index,
                             const Elf64_Sym **out_symbol, const char **out_name);
Etd2xResult etd2x_symbol_resolve(Etd2xModule *module, size_t symbol_index,
                                 Etd2xResolvedSymbol *out);
/* Re-resolve a relocation symbol after mapping without recording a second
 * compatibility bind or relocation failure statistic. Used by the deferred
 * IFUNC pass, which revisits already-accounted relocations. */
Etd2xResult etd2x_symbol_resolve_post_map(Etd2xModule *module, size_t symbol_index,
                                          Etd2xResolvedSymbol *out);
Etd2xResult etd2x_symbol_probe(const Etd2xModule *module, size_t symbol_index,
                               const char **out_name, bool *out_weak_zero);

Etd2xResult etd2x_symbol_version(const Etd2xModule *module, size_t symbol_index,
                                   Etd2xSymbolVersion *out_version);
Etd2xResult etd2x_symbol_find_defined_versioned_ex(const Etd2xModule *module, const char *name,
                                                   const Etd2xSymbolVersion *required_version,
                                                   uintptr_t *out_address, unsigned *out_type);
Etd2xResult etd2x_symbol_find_defined_versioned(const Etd2xModule *module, const char *name,
                                                const Etd2xSymbolVersion *required_version,
                                                uintptr_t *out_address);
Etd2xResult etd2x_symbol_find_tls_defined_versioned(const Etd2xModule *module, const char *name,
                                                    const Etd2xSymbolVersion *required_version,
                                                    size_t *out_offset);
Etd2xResult etd2x_symbol_resolve_tls(const Etd2xModule *requester, size_t symbol_index,
                                     const Etd2xModule **out_provider, size_t *out_offset,
                                     Etd2xSymbolVersion *out_version);
Etd2xResult etd2x_symbol_find_defined(const Etd2xModule *module, const char *name,
                                      uintptr_t *out_address);


/* Interface: linker */


#include <stdbool.h>
#include <stddef.h>




typedef struct Etd2xLinkerNode Etd2xLinkerNode;

typedef struct Etd2xLinker {
    Etd2xLinkerNode **nodes;
    size_t node_count;
    size_t node_capacity;
    size_t compat_bind_count;
    char library_dir[0x200];
} Etd2xLinker;

void etd2x_linker_init(Etd2xLinker *linker, const char *library_dir);
void etd2x_linker_unload(Etd2xLinker *linker);

/* Load one root module plus its complete DT_NEEDED graph. */
Etd2xResult etd2x_linker_load(Etd2xLinker *linker, const char *path, Etd2xModule **out_root);

/* Stage 14 Java Activity bootstrap may know the complete native root set before
 * Java executes the corresponding System.loadLibrary() calls. Discover the
 * union graph and reserve guest addresses only. Relocation, mapping,
 * .init/.init_array and JNI_OnLoad remain tied to the actual load operation so
 * unrelated RTLD_LOCAL roots cannot leak into one another's symbol scope. */
Etd2xResult etd2x_linker_prepare_roots(Etd2xLinker *linker,
                                       const char *const *paths,
                                       size_t path_count,
                                       Etd2xModule **out_primary_root);

/* Run one already-mapped module's dependency constructors exactly once. Safe to
 * call repeatedly; dependencies are constructed before the requested module.
 * A constructor failure is latched: later calls return it without re-running. */
Etd2xResult etd2x_linker_construct_module(Etd2xLinker *linker, const Etd2xModule *module);

/* True once the module's DT_INIT/DT_INIT_ARRAY have completed, i.e. once the
 * module is "loaded" in the Android-observable dlopen() sense. A module that
 * prepare_roots() mapped with deferred constructors reports false. */
bool etd2x_linker_module_constructed(const Etd2xLinker *linker, const Etd2xModule *module);

/* Eager convenience path retained for NativeActivity and host tests: prepare the
 * complete graph, then construct every requested root in dependency order. */
Etd2xResult etd2x_linker_load_roots(Etd2xLinker *linker,
                                    const char *const *paths,
                                    size_t path_count,
                                    Etd2xModule **out_primary_root);

/* Android System.loadLibrary() may request another root after the initial Java
 * bootstrap graph is already live. Add one lib*.so root to the existing graph
 * without relocating/reconstructing modules that are already loaded. */
Etd2xResult etd2x_linker_load_library(Etd2xLinker *linker,
                                      const char *soname,
                                      Etd2xModule **out_module,
                                      bool *out_newly_loaded);

/* Resolve one undefined guest symbol against the loaded dependency scope. */
Etd2xResult etd2x_linker_resolve_import_versioned(const Etd2xModule *requester,
                                                  const char *name,
                                                  const Etd2xSymbolVersion *version,
                                                  uintptr_t *out_address,
                                                  const Etd2xModule **out_provider);
Etd2xResult etd2x_linker_resolve_import(const Etd2xModule *requester,
                                        const char *name,
                                        uintptr_t *out_address,
                                        const Etd2xModule **out_provider);

/* Side-effect-free import lookup used by relocation preflight. Unlike the
 * normal resolver it does not increment compatibility bind counters or emit
 * per-bind success logs. */
Etd2xResult etd2x_linker_resolve_tls_import_versioned(const Etd2xModule *requester,
                                                      const char *name,
                                                      const Etd2xSymbolVersion *version,
                                                      const Etd2xModule **out_provider,
                                                      size_t *out_offset);

Etd2xResult etd2x_linker_probe_import_versioned(const Etd2xModule *requester,
                                                const char *name,
                                                const Etd2xSymbolVersion *version,
                                                uintptr_t *out_address,
                                                const Etd2xModule **out_provider);
Etd2xResult etd2x_linker_probe_import(const Etd2xModule *requester,
                                      const char *name,
                                      uintptr_t *out_address,
                                      const Etd2xModule **out_provider);

/* dlsym(handle, name) searches the handle's local dependency group, not just
 * the root object. Version is NULL for ordinary dlsym and non-NULL for future
 * dlvsym-style lookups. */
Etd2xResult etd2x_linker_find_in_handle(const Etd2xLinker *linker,
                                        const Etd2xModule *module,
                                        const char *name,
                                        const Etd2xSymbolVersion *version,
                                        uintptr_t *out_address,
                                        const Etd2xModule **out_provider);
Etd2xResult etd2x_linker_find_global(const Etd2xLinker *linker,
                                     const char *name,
                                     const Etd2xSymbolVersion *version,
                                     uintptr_t *out_address,
                                     const Etd2xModule **out_provider);

/* Stable dlopen handle lifetime. Java/System.loadLibrary roots are pinned;
 * native dlopen references are counted and dlclose runs eligible destructors. */
Etd2xResult etd2x_linker_acquire_module(Etd2xLinker *linker, const Etd2xModule *module);
Etd2xResult etd2x_linker_release_module(Etd2xLinker *linker, const Etd2xModule *module);
Etd2xResult etd2x_linker_pin_module(Etd2xLinker *linker, const Etd2xModule *module);
bool etd2x_linker_module_handle_live(const Etd2xLinker *linker, const Etd2xModule *module);

/* Resolve only against Bionic's process-wide guest global group. Used when a
 * relocation references a default-visibility symbol defined by the requester:
 * such a definition is preemptible unless the DSO requested symbolic binding
 * or the symbol is STV_PROTECTED. */
Etd2xResult etd2x_linker_probe_global_versioned(const Etd2xModule *requester,
                                                const char *name,
                                                const Etd2xSymbolVersion *version,
                                                uintptr_t *out_address,
                                                const Etd2xModule **out_provider);
Etd2xResult etd2x_linker_probe_global(const Etd2xModule *requester,
                                      const char *name,
                                      uintptr_t *out_address,
                                      const Etd2xModule **out_provider);

/* dlopen(..., RTLD_GLOBAL) promotes an already-loaded local root so its symbols
 * are visible to later loads. DF_1_GLOBAL modules are promoted automatically. */
Etd2xResult etd2x_linker_promote_global(Etd2xLinker *linker,
                                        const Etd2xModule *module,
                                        const char *reason);

size_t etd2x_linker_module_count(const Etd2xLinker *linker);
size_t etd2x_linker_compat_bind_count(const Etd2xLinker *linker);
const Etd2xModule *etd2x_linker_module_at(const Etd2xLinker *linker, size_t index);


/* Interface: loader */



/* Generic single-module loader retained for Stage 1/2 regression tests. */
Etd2xResult etd2x_loader_load(const char *path, Etd2xModule *module);

/* Stage 1 regression path: succeeds only when the guest requires no relocations. */
Etd2xResult etd2x_loader_load_stage1(const char *path, Etd2xModule *module);

Etd2xResult etd2x_loader_find_export(const Etd2xModule *module, const char *name, void **out_address);

/* Stage 3 linker building blocks. These do not run constructors implicitly. */
Etd2xResult etd2x_loader_prepare(const char *path, Etd2xModule *module);
/* Header-only classification for speculative Java native-library candidates. */
Etd2xResult etd2x_loader_probe_shared_object(const char *path, bool *out_shared);
Etd2xResult etd2x_loader_relocate(Etd2xModule *module);
Etd2xResult etd2x_loader_patch_system_registers(Etd2xModule *module);
Etd2xResult etd2x_loader_map(Etd2xModule *module);
Etd2xResult etd2x_loader_resolve_ifuncs(Etd2xModule *module);
Etd2xResult etd2x_loader_construct(Etd2xModule *module);


/* Interface: relocations */



Etd2xResult etd2x_apply_relr(Etd2xModule *module);
Etd2xResult etd2x_preflight_relocations(Etd2xModule *module, size_t *out_unique_missing);
Etd2xResult etd2x_apply_relocations(Etd2xModule *module);
Etd2xResult etd2x_apply_post_map_relocations(Etd2xModule *module);


#endif /* __SWITCH__ */
