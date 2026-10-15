/* main: preserved standalone implementation. */
#define _GNU_SOURCE 1
#include "runtime.h"


/* ---- main ---- */
#pragma push_macro("MAIN_LOOP_STALL_MS")
#pragma push_macro("MAIN_LOOP_STALL_REPEAT_MS")

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <ctype.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <switch.h>
























typedef int32_t (*main_JniOnLoadFn)(void *, void *);

static Etd2xResult main_package_jni_class_resolver_ex(const char *class_name,
                                                   char *out_super_name,
                                                   size_t out_super_name_cap,
                                                   char *out_interfaces,
                                                   size_t out_interfaces_cap,
                                                   bool *out_found,
                                                   void *user_data) {
    const char *source_path = (const char *)user_data;
    if (!source_path) return ETD2X_ERR_ARGUMENT;
    return etd2x_package_find_dex_class_ex(source_path, class_name, out_super_name,
                                           out_super_name_cap, out_interfaces,
                                           out_interfaces_cap, out_found);
}

static Etd2xResult main_package_jni_class_initializer(void *env, const char *class_name,
                                                   void *user_data) {
    const char *source_path = (const char *)user_data;
    if (!env || !class_name || !source_path) return ETD2X_ERR_ARGUMENT;

    /* The JNI model also contains framework classes. Only package DEX classes
     * have bytecode <clinit> bodies for this callback to execute. */
    char super_name[512] = {0};
    char interfaces[2048] = {0};
    bool found = false;
    Etd2xResult rc = etd2x_package_find_dex_class_ex(source_path, class_name, super_name,
                                                      sizeof(super_name), interfaces,
                                                      sizeof(interfaces), &found);
    if (rc != ETD2X_OK || !found) return rc;

    Etd2xDexExecReport report = {0};
    return etd2x_dex_initialize_class(source_path, class_name, env, &report);
}

static uint64_t main_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000) + (uint64_t)ts.tv_nsec / UINT64_C(1000000);
}

static uint64_t main_fast_tick_us(void) {
    static uint64_t freq = 0;
    if (!freq) freq = armGetSystemTickFreq();
    const uint64_t tick = armGetSystemTick();
    if (!freq) return 0;
    return (tick / freq) * UINT64_C(1000000) +
           ((tick % freq) * UINT64_C(1000000)) / freq;
}

static void main_log_memory_snapshot(const char *label) {
    u64 total = 0;
    u64 used = 0;
    Result rt = svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    Result ru = svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (R_SUCCEEDED(rt) && R_SUCCEEDED(ru)) {
        ETD2X_LOGCI("MEMORY", "%s total=%" PRIu64 " used=%" PRIu64 " free=%" PRIu64,
                    label ? label : "snapshot", total, used, total > used ? total - used : 0);
    } else {
        ETD2X_LOGCW("MEMORY", "%s svcGetInfo failed total_rc=0x%08x used_rc=0x%08x",
                    label ? label : "snapshot", rt, ru);
    }
}


static bool resolve(const Etd2xModule *module, const char *name, void **out) {
    Etd2xResult rc = etd2x_loader_find_export(module, name, out);
    if (rc == ETD2X_OK) return true;
    ETD2X_LOGCE("BOOT", "Missing entry %s: %s", name, etd2x_result_string(rc));
    return false;
}

/* runtime loading */
static bool main_etd2x_path_join(char *dst, size_t cap, const char *a, const char *b) {
    if (!dst || cap == 0 || !a || !b) return false;
    const size_t an = strlen(a), bn = strlen(b);
    const bool slash = an > 0 && a[an - 1] != '/';
    if (an + (slash ? 1u : 0u) + bn + 1u > cap) { dst[0] = 0; return false; }
    memcpy(dst, a, an);
    size_t pos = an;
    if (slash) dst[pos++] = '/';
    memcpy(dst + pos, b, bn);
    dst[pos + bn] = 0;
    return true;
}

static const char *main_path_basename(const char *path) {
    if (!path) return NULL;
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void main_sanitize_package_component(const char *package_name, char *out, size_t cap) {
    if (!out || cap == 0) return;
    size_t w = 0;
    for (const char *p = package_name ? package_name : "unknown"; *p && w + 1 < cap; ++p) {
        unsigned char c = (unsigned char)*p;
        out[w++] = (isalnum(c) || c == '.' || c == '_' || c == '-') ? (char)c : '_';
    }
    if (w == 0 && cap > 1) { out[0] = 'u'; out[1] = 0; return; }
    out[w] = 0;
}

static void main_clear_library_directory(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return;
    struct dirent *ent;
    char full[ETD2X_APK_PATH_CAP];
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!main_etd2x_path_join(full, sizeof(full), path, ent->d_name)) continue;
        struct stat st;
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) (void)unlink(full);
    }
    closedir(dir);
}

static void main_clear_directory_tree_contents(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return;
    struct dirent *ent;
    char full[ETD2X_APK_PATH_CAP];
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (!main_etd2x_path_join(full, sizeof(full), path, ent->d_name)) continue;
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            main_clear_directory_tree_contents(full);
            (void)rmdir(full);
        } else if (S_ISREG(st.st_mode)) {
            (void)unlink(full);
        }
    }
    closedir(dir);
}

static bool main_supported_jni_onload_version(int32_t version) {
    return version == ETD2X_JNI_VERSION_1_1 || version == ETD2X_JNI_VERSION_1_2 ||
           version == ETD2X_JNI_VERSION_1_4 || version == ETD2X_JNI_VERSION_1_6;
}

enum { ETD2X_JAVA_LOADED_LIBRARY_MAX = 128 };

typedef enum main_JavaLibraryLoadState {
    JAVA_LIBRARY_LOADING = 1,
    JAVA_LIBRARY_LOADED = 2,
    JAVA_LIBRARY_FAILED = 3,
} main_JavaLibraryLoadState;

typedef struct main_JavaLibraryLoadEntry {
    char soname[ETD2X_APK_LIBNAME_CAP + 7];
    main_JavaLibraryLoadState state;
    Etd2xResult failure;
} main_JavaLibraryLoadEntry;

typedef struct main_JavaLoadLibraryContext {
    Etd2xLinker *linker;
    main_JavaLibraryLoadEntry entries[ETD2X_JAVA_LOADED_LIBRARY_MAX];
    size_t entry_count;
} main_JavaLoadLibraryContext;

static main_JavaLibraryLoadEntry *main_java_library_find(main_JavaLoadLibraryContext *ctx, const char *soname) {
    if (!ctx || !soname) return NULL;
    for (size_t i = 0; i < ctx->entry_count; ++i)
        if (strcmp(ctx->entries[i].soname, soname) == 0) return &ctx->entries[i];
    return NULL;
}

static main_JavaLibraryLoadEntry *main_java_library_get_or_add(main_JavaLoadLibraryContext *ctx, const char *soname) {
    if (!ctx || !soname || !soname[0]) return NULL;
    main_JavaLibraryLoadEntry *entry = main_java_library_find(ctx, soname);
    if (entry) return entry;
    if (ctx->entry_count >= ETD2X_JAVA_LOADED_LIBRARY_MAX) return NULL;
    entry = &ctx->entries[ctx->entry_count];
    memset(entry, 0, sizeof(*entry));
    const int n = snprintf(entry->soname, sizeof(entry->soname), "%s", soname);
    if (n <= 0 || (size_t)n >= sizeof(entry->soname)) return NULL;
    ctx->entry_count++;
    return entry;
}

static Etd2xResult main_java_system_load_library(const char *library_name, void *user_data) {
    main_JavaLoadLibraryContext *ctx = (main_JavaLoadLibraryContext *)user_data;
    if (!ctx || !ctx->linker || !library_name || !library_name[0]) return ETD2X_ERR_ARGUMENT;
    if (strchr(library_name, '/') || strchr(library_name, '\\')) return ETD2X_ERR_DEPENDENCY_INVALID;

    /* java.lang.System.loadLibrary takes an unmapped logical name. Android's
       Runtime maps it to lib<name>.so before consulting the native loader. */
    char soname[ETD2X_APK_LIBNAME_CAP + 7];
    const int n = snprintf(soname, sizeof(soname), "lib%s.so", library_name);
    if (n <= 0 || (size_t)n >= sizeof(soname)) return ETD2X_ERR_DEPENDENCY_INVALID;

    main_JavaLibraryLoadEntry *entry = main_java_library_get_or_add(ctx, soname);
    if (!entry) return ETD2X_ERR_NO_MEMORY;
    if (entry->state == JAVA_LIBRARY_LOADED) {
        ETD2X_LOGCI("JAVA", "System.loadLibrary library=%s mapped=%s result=already-java-loaded",
                    library_name, soname);
        return ETD2X_OK;
    }
    if (entry->state == JAVA_LIBRARY_LOADING) {
        /* JNI_OnLoad may re-enter Java and request the same library. Android's
           native loader treats the in-progress load as the same operation; do
           not recurse into constructors/JNI_OnLoad a second time. */
        ETD2X_LOGCI("JAVA", "System.loadLibrary library=%s mapped=%s result=already-loading",
                    library_name, soname);
        return ETD2X_OK;
    }
    if (entry->state == JAVA_LIBRARY_FAILED) {
        ETD2X_LOGCW("JAVA", "System.loadLibrary library=%s mapped=%s result=previous-failure rc=%d",
                    library_name, soname, (int)entry->failure);
        return entry->failure != ETD2X_OK ? entry->failure : ETD2X_ERR_APK_UNSUPPORTED;
    }
    entry->state = JAVA_LIBRARY_LOADING;
    entry->failure = ETD2X_OK;

    Etd2xModule *module = NULL;
    bool newly_loaded = false;
    Etd2xResult rc = etd2x_linker_load_library(ctx->linker, soname, &module, &newly_loaded);
    if (rc != ETD2X_OK || !module) {
        entry->state = JAVA_LIBRARY_FAILED;
        entry->failure = rc != ETD2X_OK ? rc : ETD2X_ERR_DEPENDENCY_NOT_FOUND;
        return entry->failure;
    }

    if (!newly_loaded) {
        ETD2X_LOGCI("JAVA", "System.loadLibrary library=%s mapped=%s module=already-mapped constructors=pending",
                    library_name, soname);
    }

    /* Android's native loader performs relocation/mapping and executes
     * DT_INIT/DT_INIT_ARRAY as part of the actual dlopen/System.loadLibrary
     * operation. Java-route package probing only pre-discovers/reserves the
     * candidate graph; unrelated RTLD_LOCAL roots stay outside this root's
     * symbol lookup scope. */
    etd2x_dynamic_set_constructor_trace(etd2x_diag_full_trace_enabled());
    rc = etd2x_linker_construct_module(ctx->linker, module);
    etd2x_dynamic_set_constructor_trace(false);
    if (rc != ETD2X_OK) {
        entry->state = JAVA_LIBRARY_FAILED;
        entry->failure = rc;
        ETD2X_LOGCE("JAVA", "System.loadLibrary constructors failed library=%s rc=%d",
                    soname, (int)rc);
        return rc;
    }
    rc = etd2x_linker_pin_module(ctx->linker, module);
    if (rc != ETD2X_OK) {
        entry->state = JAVA_LIBRARY_FAILED;
        entry->failure = rc;
        return rc;
    }

    void *onload_addr = NULL;
    rc = etd2x_loader_find_export(module, "JNI_OnLoad", &onload_addr);
    if (rc == ETD2X_ERR_SYMBOL_NOT_FOUND || !onload_addr) {
        entry->state = JAVA_LIBRARY_LOADED;
        ETD2X_LOGCI("JAVA", "System.loadLibrary library=%s mapped=%s result=loaded JNI_OnLoad=absent",
                    library_name, soname);
        return ETD2X_OK;
    }
    if (rc != ETD2X_OK) {
        entry->state = JAVA_LIBRARY_FAILED;
        entry->failure = rc;
        return rc;
    }

    ETD2X_LOGCI("JAVA", "System.loadLibrary calling JNI_OnLoad library=%s entry=%p",
                soname, onload_addr);
    void *onload_env = etd2x_jni_current_env();
    if (!onload_env ||
        etd2x_jni_push_local_frame_host(onload_env, 32) != ETD2X_OK) {
        entry->state = JAVA_LIBRARY_FAILED;
        entry->failure = ETD2X_ERR_APK_UNSUPPORTED;
        return entry->failure;
    }
    if (etd2x_jni_push_application_class_loader_host(onload_env) != ETD2X_OK) {
        (void)etd2x_jni_pop_local_frame_host(onload_env, NULL);
        entry->state = JAVA_LIBRARY_FAILED;
        entry->failure = ETD2X_ERR_APK_UNSUPPORTED;
        return entry->failure;
    }
    const int32_t version = ((main_JniOnLoadFn)onload_addr)(etd2x_jni_vm(), NULL);
    etd2x_jni_pop_application_class_loader_host(onload_env);
    (void)etd2x_jni_pop_local_frame_host(onload_env, NULL);
    ETD2X_LOGCI("JAVA", "System.loadLibrary JNI_OnLoad library=%s returned=0x%08" PRIx32,
                soname, (uint32_t)version);
    if (!main_supported_jni_onload_version(version)) {
        entry->state = JAVA_LIBRARY_FAILED;
        entry->failure = ETD2X_ERR_APK_UNSUPPORTED;
        return entry->failure;
    }
    entry->state = JAVA_LIBRARY_LOADED;
    entry->failure = ETD2X_OK;
    return ETD2X_OK;
}

static uint64_t main_runtime_source_fingerprint(const char *path, const struct stat *st) {
    uint64_t h = UINT64_C(1469598103934665603);
    const unsigned char *p = (const unsigned char *)(path ? path : "");
    while (*p) { h ^= *p++; h *= UINT64_C(1099511628211); }
    if (st) {
        const unsigned char *q = (const unsigned char *)&st->st_size;
        for (size_t i = 0; i < sizeof(st->st_size); ++i) { h ^= q[i]; h *= UINT64_C(1099511628211); }
        q = (const unsigned char *)&st->st_mtime;
        for (size_t i = 0; i < sizeof(st->st_mtime); ++i) { h ^= q[i]; h *= UINT64_C(1099511628211); }
    }
    return h;
}

static size_t main_count_cached_shared_objects(const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) return 0;
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        size_t n = strlen(name);
        if (n < 4 || strcmp(name + n - 3, ".so") != 0) continue;
        char full[ETD2X_APK_PATH_CAP];
        if (!main_etd2x_path_join(full, sizeof(full), dir_path, name)) continue;
        struct stat st;
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) count++;
    }
    closedir(dir);
    return count;
}

static bool main_runtime_cache_matches(const char *runtime_root, const char *source_path,
                                  const struct stat *source_st, size_t *split_assets,
                                  size_t *obb_files) {
    char stamp_path[ETD2X_APK_PATH_CAP];
    if (!main_etd2x_path_join(stamp_path, sizeof(stamp_path), runtime_root, "package-cache.key")) return false;
    FILE *f = fopen(stamp_path, "rb");
    if (!f) return false;
    unsigned long long stored = 0;
    size_t assets = 0, obbs = 0;
    int matched = fscanf(f, "ETD2X-runtime-v2 %llx %zu %zu", &stored, &assets, &obbs);
    fclose(f);
    if (matched != 3 || (uint64_t)stored != main_runtime_source_fingerprint(source_path, source_st)) return false;
    *split_assets = assets;
    *obb_files = obbs;
    return true;
}

static bool main_runtime_library_cache_matches(const char *runtime_root, const char *lib_dir,
                                          const char *source_path, const struct stat *source_st,
                                          size_t expected_libs, size_t *split_assets, size_t *obb_files) {
    const bool stamp_match = main_runtime_cache_matches(runtime_root, source_path, source_st, split_assets, obb_files);
    const size_t cached_libs = main_count_cached_shared_objects(lib_dir);
    const bool hit = stamp_match && cached_libs >= expected_libs;
    ETD2X_LOGCI("PERF", "runtime lib cache stamp=%s cachedLibs=%zu expectedLibs=%zu result=%s",
                stamp_match ? "match" : "miss", cached_libs, expected_libs, hit ? "hit" : "fill");
    return hit;
}

static void main_runtime_cache_commit(const char *runtime_root, const char *source_path, const struct stat *source_st,
                                  size_t split_assets, size_t obb_files) {
    char stamp_path[ETD2X_APK_PATH_CAP];
    if (!main_etd2x_path_join(stamp_path, sizeof(stamp_path), runtime_root, "package-cache.key")) return;
    FILE *f = fopen(stamp_path, "wb");
    if (!f) return;
    fprintf(f, "ETD2X-runtime-v2 %016" PRIx64 " %zu %zu\n",
            main_runtime_source_fingerprint(source_path, source_st), split_assets, obb_files);
    fclose(f);
}

static Etd2xResult main_resolve_dex_jni_export(const char *class_name, const char *method_name,
                                          const char *signature, void **out_fn,
                                          bool *out_found, void *user_data) {
    if (!class_name || !method_name || !signature || !out_fn || !out_found || !user_data)
        return ETD2X_ERR_ARGUMENT;
    *out_fn = NULL;
    *out_found = false;
    const Etd2xLinker *linker = (const Etd2xLinker *)user_data;
    char short_name[1024], long_name[1536];
    if (!etd2x_jni_build_export_name(class_name, method_name, signature, false,
                               short_name, sizeof(short_name)) ||
        !etd2x_jni_build_export_name(class_name, method_name, signature, true,
                               long_name, sizeof(long_name))) return ETD2X_ERR_ARGUMENT;
    for (size_t m = 0; m < etd2x_linker_module_count(linker); ++m) {
        const Etd2xModule *module = etd2x_linker_module_at(linker, m);
        if (!module) continue;
        /* Java-route roots are deliberately discovered before they are all mapped.
         * etd2x_loader_find_export() requires a mapped runtime image; treating an
         * intentionally deferred module as a resolver error aborts JNI lookup before
         * later mapped modules (for example a library loaded by System.loadLibrary)
         * are searched.  An unmapped module simply cannot currently provide an
         * executable JNI entry point, so skip it and keep walking the namespace. */
        if (!module->mapped || !module->runtime_base ||
            !module->dynamic.symtab_vaddr || !module->dynamic.strtab_vaddr) {
            continue;
        }
        void *address = NULL;
        Etd2xResult rc = etd2x_loader_find_export(module, short_name, &address);
        if (rc == ETD2X_OK) {
            *out_fn = address; *out_found = true;
            ETD2X_LOGCI("JAVA", "DEX native resolved class=%s method=%s sig=%s symbol=%s module=%s",
                        class_name, method_name, signature, short_name, main_path_basename(module->path));
            return ETD2X_OK;
        }
        if (rc != ETD2X_ERR_SYMBOL_NOT_FOUND) return rc;
        address = NULL;
        rc = etd2x_loader_find_export(module, long_name, &address);
        if (rc == ETD2X_OK) {
            *out_fn = address; *out_found = true;
            ETD2X_LOGCI("JAVA", "DEX native resolved class=%s method=%s sig=%s symbol=%s module=%s",
                        class_name, method_name, signature, long_name, main_path_basename(module->path));
            return ETD2X_OK;
        }
        if (rc != ETD2X_ERR_SYMBOL_NOT_FOUND) return rc;
    }
    return ETD2X_OK;
}

enum {
    ETD2X_EGL_NONE = 0x3038,
    ETD2X_EGL_RED_SIZE = 0x3024,
    ETD2X_EGL_GREEN_SIZE = 0x3023,
    ETD2X_EGL_BLUE_SIZE = 0x3022,
    ETD2X_EGL_ALPHA_SIZE = 0x3021,
    ETD2X_EGL_DEPTH_SIZE = 0x3025,
    ETD2X_EGL_STENCIL_SIZE = 0x3026,
    ETD2X_EGL_SURFACE_TYPE = 0x3033,
    ETD2X_EGL_RENDERABLE_TYPE = 0x3040,
    ETD2X_EGL_WINDOW_BIT = 0x0004,
    ETD2X_EGL_OPENGL_ES2_BIT = 0x0004,
    ETD2X_EGL_OPENGL_ES3_BIT = 0x0040,
    ETD2X_EGL_CONTEXT_CLIENT_VERSION = 0x3098,
    ETD2X_EGL_OPENGL_ES_API = 0x30A0,
    ETD2X_EGL_WIDTH = 0x3057,
    ETD2X_EGL_HEIGHT = 0x3056,
};

typedef struct main_Etd2xJavaEglState {
    void *display;
    void *config;
    void *surface;
    void *context;
    int32_t width;
    int32_t height;
    bool initialized;
    bool current;
} main_Etd2xJavaEglState;



static bool main_drain_glsurfaceview_events(const char *source_path, void *view_ref);

/* java dispatch */
static void main_java_egl_shutdown(main_Etd2xJavaEglState *egl) {
    if (!egl || !egl->initialized) return;
    if (egl->current)
        (void)etd2x_eglMakeCurrent(egl->display, NULL, NULL, NULL);
    if (egl->context) (void)etd2x_eglDestroyContext(egl->display, egl->context);
    if (egl->surface) (void)etd2x_eglDestroySurface(egl->display, egl->surface);
    if (egl->display) (void)etd2x_eglTerminate(egl->display);
    memset(egl, 0, sizeof(*egl));
}

static bool main_java_egl_start(const Etd2xAndroidGLSurfaceViewInfo *info,
                           int32_t requested_width, int32_t requested_height,
                           main_Etd2xJavaEglState *out) {
    if (!info || !out || requested_width <= 0 || requested_height <= 0) return false;
    memset(out, 0, sizeof(*out));
    out->display = etd2x_eglGetDisplay(NULL);
    if (!out->display) {
        ETD2X_LOGCE("JAVA-GL", "GLSurfaceView eglGetDisplay failed error=0x%04x",
                    (unsigned)etd2x_eglGetError());
        return false;
    }
    int32_t major = 0, minor = 0;
    if (!etd2x_eglInitialize(out->display, &major, &minor)) {
        ETD2X_LOGCE("JAVA-GL", "GLSurfaceView eglInitialize failed error=0x%04x",
                    (unsigned)etd2x_eglGetError());
        return false;
    }
    out->initialized = true;
    if (!etd2x_eglBindAPI(ETD2X_EGL_OPENGL_ES_API)) {
        ETD2X_LOGCE("JAVA-GL", "GLSurfaceView eglBindAPI failed error=0x%04x",
                    (unsigned)etd2x_eglGetError());
        main_java_egl_shutdown(out);
        return false;
    }

    const int32_t version = info->egl_client_version > 0 ? info->egl_client_version : 1;
    const int32_t renderable = version >= 3 ? ETD2X_EGL_OPENGL_ES3_BIT : ETD2X_EGL_OPENGL_ES2_BIT;
    const int32_t red = info->egl_config_explicit ? info->egl_red_size : 8;
    const int32_t green = info->egl_config_explicit ? info->egl_green_size : 8;
    const int32_t blue = info->egl_config_explicit ? info->egl_blue_size : 8;
    const int32_t alpha = info->egl_config_explicit ? info->egl_alpha_size : 0;
    const int32_t depth = info->egl_config_explicit ? info->egl_depth_size : 24;
    const int32_t stencil = info->egl_config_explicit ? info->egl_stencil_size : 8;
    int32_t config_attrs[] = {
        ETD2X_EGL_RED_SIZE, red,
        ETD2X_EGL_GREEN_SIZE, green,
        ETD2X_EGL_BLUE_SIZE, blue,
        ETD2X_EGL_ALPHA_SIZE, alpha,
        ETD2X_EGL_DEPTH_SIZE, depth,
        ETD2X_EGL_STENCIL_SIZE, stencil,
        ETD2X_EGL_SURFACE_TYPE, ETD2X_EGL_WINDOW_BIT,
        ETD2X_EGL_RENDERABLE_TYPE, renderable,
        ETD2X_EGL_NONE
    };
    int32_t config_count = 0;
    bool config_ok = etd2x_eglChooseConfig(out->display, config_attrs, &out->config, 1, &config_count) &&
                     config_count >= 1 && out->config;
    if (!config_ok && !info->egl_config_explicit) {
        /* Cocos2d-x commonly asks for a depth/stencil-capable context and uses
         * stencil-backed ClippingNode UI. Prefer 24/8 for generic Java GLES
         * apps, but retain the old 16/0 path as a compatibility fallback when
         * the platform cannot satisfy the richer config. */
        config_attrs[9] = 16;
        config_attrs[11] = 0;
        out->config = NULL;
        config_count = 0;
        config_ok = etd2x_eglChooseConfig(out->display, config_attrs, &out->config, 1, &config_count) &&
                    config_count >= 1 && out->config;
        if (config_ok)
            ETD2X_LOGCW("JAVA-GL", "GLSurfaceView preferred depth/stencil unavailable; fallback depth=16 stencil=0");
    }
    if (!config_ok) {
        ETD2X_LOGCE("JAVA-GL", "GLSurfaceView eglChooseConfig failed version=%d rgba=%d/%d/%d/%d depth=%d stencil=%d error=0x%04x",
                    version, red, green, blue, alpha, depth, stencil,
                    (unsigned)etd2x_eglGetError());
        main_java_egl_shutdown(out);
        return false;
    }
    /* GLSurfaceView renders into its SurfaceHolder's Surface, which is the
     * process window. Use the same ANativeWindow that ANativeWindow_fromSurface
     * hands to native code: EGL rejects a null native window
     * (EGL_BAD_NATIVE_WINDOW), and the token carries the logical size. */
    Etd2xANativeWindow *native_window = etd2x_native_window();
    if (!native_window) {
        etd2x_native_window_prepare(requested_width, requested_height, 1 /* WINDOW_FORMAT_RGBA_8888 */);
        native_window = etd2x_native_window();
    }
    out->surface = etd2x_eglCreateWindowSurface(out->display, out->config, native_window, NULL);
    if (!out->surface) {
        ETD2X_LOGCE("JAVA-GL", "GLSurfaceView eglCreateWindowSurface failed error=0x%04x",
                    (unsigned)etd2x_eglGetError());
        main_java_egl_shutdown(out);
        return false;
    }
    const int32_t context_attrs[] = {
        ETD2X_EGL_CONTEXT_CLIENT_VERSION, version,
        ETD2X_EGL_NONE
    };
    out->context = etd2x_eglCreateContext(out->display, out->config, NULL, context_attrs);
    if (!out->context) {
        ETD2X_LOGCE("JAVA-GL", "GLSurfaceView eglCreateContext failed version=%d error=0x%04x",
                    version, (unsigned)etd2x_eglGetError());
        main_java_egl_shutdown(out);
        return false;
    }
    if (!etd2x_eglMakeCurrent(out->display, out->surface, out->surface, out->context)) {
        ETD2X_LOGCE("JAVA-GL", "GLSurfaceView eglMakeCurrent failed error=0x%04x",
                    (unsigned)etd2x_eglGetError());
        main_java_egl_shutdown(out);
        return false;
    }
    out->current = true;
    out->width = requested_width;
    out->height = requested_height;
    int32_t queried = 0;
    if (etd2x_eglQuerySurface(out->display, out->surface, ETD2X_EGL_WIDTH, &queried) && queried > 0)
        out->width = queried;
    if (etd2x_eglQuerySurface(out->display, out->surface, ETD2X_EGL_HEIGHT, &queried) && queried > 0)
        out->height = queried;
    (void)etd2x_eglSwapInterval(out->display, 1);
    ETD2X_LOGCI("JAVA-GL", "GLSurfaceView EGL ready version=%d egl=%d.%d size=%dx%d renderMode=%d queued=%zu",
                version, major, minor, out->width, out->height,
                info->render_mode, info->queued_event_count);
    return true;
}

/* Host dispatcher = the thread's Looper/uncaught-exception boundary. A Java
 * throwable that escaped a top-level callback has been reported by the caller;
 * it is then delivered (logged) and cleared exactly like
 * Thread.dispatchUncaughtException, so it can never leak into the next,
 * unrelated callback dispatched on the same JNIEnv. */
static void main_java_dispatch_uncaught(const char *checkpoint) {
    void *env = etd2x_jni_current_env();
    if (!env || !etd2x_jni_exception_pending_host(env)) return;
    char ex_class[256] = {0}, ex_msg[256] = {0};
    void *ex = etd2x_jni_exception_occurred_host(env);
    if (ex) {
        (void)etd2x_jni_object_class_name_host(ex, ex_class, sizeof(ex_class));
        (void)etd2x_jni_exception_message_host(ex, ex_msg, sizeof(ex_msg));
    }
    etd2x_jni_exception_clear_host(env);
    if (ex) etd2x_jni_delete_local_ref_host(env, ex);
    ETD2X_LOGCW("JAVA", "uncaught exception delivered checkpoint=%s class=%s message=%s",
                checkpoint ? checkpoint : "-", ex_class[0] ? ex_class : "<unknown>", ex_msg);
}

static bool main_dex_instance_callback(const char *source_path, const char *class_name,
                                  const char *method_name, const char *signature,
                                  void *receiver_ref, const Etd2xJniValue *args,
                                  size_t arg_count, const char *checkpoint) {
    Etd2xDexExecReport report = {0};
    Etd2xJniValue result = {0};
    Etd2xResult rc = etd2x_dex_execute_instance_method(source_path, class_name, method_name,
                                                        signature, etd2x_jni_current_env(),
                                                        receiver_ref, args, arg_count,
                                                        &result, &report);
    if (rc != ETD2X_OK || !report.completed) {
        ETD2X_LOGT("%s: FAIL class=%s method=%s sig=%s pc=%#x opcode=0x%02x detail=%s instructions=%zu calls=%zu",
                   checkpoint, report.class_name[0] ? report.class_name : class_name,
                   report.method_name[0] ? report.method_name : method_name,
                   report.signature[0] ? report.signature : signature,
                   report.pc, report.opcode,
                   report.detail[0] ? report.detail : etd2x_result_string(rc),
                   report.instructions, report.calls);
        main_java_dispatch_uncaught(checkpoint);
        return false;
    }
    /* Queued callbacks can run every frame. Their successful execution is a
     * full-trace event, not an urgent SD durability checkpoint each time. */
    if (etd2x_diag_full_trace_enabled() ||
        (strcmp(checkpoint, "stage14.java-ui-event") != 0 &&
         strcmp(checkpoint, "stage14.java-gl-queue-event") != 0))
        ETD2X_LOGT("%s: PASS class=%s method=%s instructions=%zu calls=%zu",
                   checkpoint, class_name, method_name, report.instructions, report.calls);
    return true;
}

/* Activity lifecycle callback bracketed by the API 29 ActivityLifecycleCallbacks
 * Pre/Post events, as Android's Activity.performStart/performResume/... raise
 * them around onStart/onResume/... (AndroidX Lifecycle observes these). */
static bool main_activity_lifecycle_callback(const char *source_path, const char *class_name,
                                        const char *method_name, void *activity_ref,
                                        const char *checkpoint) {
    const char *phase = strcmp(method_name, "onStart") == 0 ? "Started"
                      : strcmp(method_name, "onResume") == 0 ? "Resumed"
                      : strcmp(method_name, "onPause") == 0 ? "Paused"
                      : strcmp(method_name, "onStop") == 0 ? "Stopped"
                      : strcmp(method_name, "onDestroy") == 0 ? "Destroyed" : NULL;
    char event[32];
    if (phase) {
        snprintf(event, sizeof(event), "Pre%s", phase);
        (void)etd2x_android_dispatch_activity_lifecycle_event(etd2x_jni_current_env(), event);
    }
    if (!main_dex_instance_callback(source_path, class_name, method_name, "()V", activity_ref, NULL, 0,
                               checkpoint))
        return false;
    if (phase) {
        snprintf(event, sizeof(event), "Post%s", phase);
        (void)etd2x_android_dispatch_activity_lifecycle_event(etd2x_jni_current_env(), event);
    }
    return true;
}

static bool main_drain_glsurfaceview_events_direct(const char *source_path, void *view_ref) {
    void *env = etd2x_jni_current_env();
    size_t drained = 0;
    for (;;) {
        void *runnable = NULL;
        bool found = false;
        Etd2xResult rc = etd2x_android_glsurfaceview_dequeue_event(env, view_ref,
                                                                   &runnable, &found);
        if (rc != ETD2X_OK) return false;
        if (!found) break;
        char class_name[512];
        bool named = etd2x_jni_object_class_name_host(runnable, class_name, sizeof(class_name));
        bool ok = named && main_dex_instance_callback(source_path, class_name, "run", "()V",
                                                  runnable, NULL, 0,
                                                  "stage14.java-gl-queue-event");
        etd2x_jni_delete_local_ref_host(env, runnable);
        if (!ok) return false;
        drained++;
    }
    if (etd2x_diag_full_trace_enabled())
        ETD2X_LOGCI("JAVA-GL", "GLSurfaceView queue drained events=%zu", drained);
    return true;
}

static bool main_drain_activity_ui_events_budget(const char *source_path, uint64_t budget_us) {
    void *env = etd2x_jni_current_env();
    size_t drained = 0;
    const uint64_t begin_us = main_fast_tick_us();
    static uint64_t last_slow_report_us;
    for (;;) {
        void *runnable = NULL;
        bool found = false;
        const uint64_t used_us = main_fast_tick_us() - begin_us;
        if (budget_us && used_us >= budget_us) break;
        Etd2xResult rc = etd2x_android_activity_dequeue_ui_event_budget(env, &runnable, &found,
                            budget_us ? (budget_us - used_us) * UINT64_C(1000) : 0);
        if (rc != ETD2X_OK) return false;
        if (!found) break;
        char class_name[512];
        bool named = etd2x_jni_object_class_name_host(runnable, class_name, sizeof(class_name));
        const uint64_t callback_begin_us = main_fast_tick_us();
        bool ok = named && main_dex_instance_callback(source_path, class_name, "run", "()V",
                                                  runnable, NULL, 0,
                                                  "stage14.java-ui-event");
        etd2x_jni_delete_local_ref_host(env, runnable);
        if (!ok) return false;
        drained++;
        const uint64_t end_us = main_fast_tick_us();
        if (end_us - callback_begin_us >= UINT64_C(16667) &&
            (!last_slow_report_us || end_us - last_slow_report_us >= UINT64_C(1000000))) {
            last_slow_report_us = end_us;
            ETD2X_LOGCI("PERF", "main UI callback class=%s elapsedUs=%" PRIu64 " batchEvents=%zu batchUs=%" PRIu64,
                        class_name, end_us - callback_begin_us, drained, end_us - begin_us);
        }
        /* Poll host input between callback batches. Reposting callbacks must
         * not keep the main thread in this drain indefinitely. */
        if (budget_us && (end_us - begin_us >= budget_us || drained >= 64u)) break;
    }
    if (etd2x_diag_full_trace_enabled())
        ETD2X_LOGCI("JAVA", "Activity UI queue drained events=%zu", drained);
    return true;
}

static bool main_drain_activity_ui_events(const char *source_path) {
    return main_drain_activity_ui_events_budget(source_path, 0);
}

/* Window focus is not an Activity lifecycle callback. Android delivers it from
 * the window/main Looper after onResume() has returned.  Give already-due main
 * Looper work one turn before synthesizing focus so work posted by onResume()
 * cannot be reordered behind the later window event.  This is intentionally
 * title/engine independent and also provides a hard BEGIN marker for hardware
 * logs if guest onWindowFocusChanged() itself blocks. */
static bool main_java_activity_deliver_window_focus(const char *source_path,
                                               const char *activity_class,
                                               void *activity_ref,
                                               bool has_focus,
                                               const char *checkpoint) {
    if (!source_path || !activity_class || !activity_ref || !checkpoint) return false;
    ETD2X_LOGCI("JAVA", "Activity post-resume main-loop turn begin class=%s", activity_class);
    if (!main_drain_activity_ui_events(source_path)) {
        ETD2X_LOGT("%s-prequeue: FAIL class=%s", checkpoint, activity_class);
        return false;
    }
    /* ActivityThread adds the decor to WindowManager after onResume returns.
     * Surface callbacks belong to that window traversal, after already-posted
     * main-thread work, not to setContentView during onCreate. */
    if (has_focus) etd2x_android_set_activity_window_attached(true);
    if (etd2x_jni_exception_pending_host(etd2x_jni_current_env()) ||
        etd2x_jni_process_exit_requested(NULL, 0)) return false;
    if (!etd2x_android_do_frame()) return false;
    etd2x_choreographer_pump();
    /* Android has returned to its event loop at this boundary. Yield once so
     * app-owned workers awakened by onResume() can observe their queued state
     * before the synthetic WindowManager focus event is delivered. */
    svcSleepThread(0);
    ETD2X_LOGCI("JAVA", "Activity window-focus dispatch begin class=%s hasFocus=%d",
                activity_class, has_focus ? 1 : 0);
    /* WindowManager updates focus state before Activity.onWindowFocusChanged(),
     * so hasWindowFocus() queried from inside the callback observes the new state. */
    etd2x_android_set_window_focus(has_focus);
    Etd2xJniValue focus_args[1] = {{0}};
    focus_args[0].z = has_focus ? 1 : 0;
    const bool ok = main_dex_instance_callback(source_path, activity_class,
                                          "onWindowFocusChanged", "(Z)V",
                                          activity_ref, focus_args, 1,
                                          checkpoint);
    ETD2X_LOGCI("JAVA", "Activity window-focus dispatch end class=%s hasFocus=%d ok=%d",
                activity_class, has_focus ? 1 : 0, ok ? 1 : 0);
    return ok;
}

static bool main_java_require_activity_state(Etd2xAndroidActivityState expected,
                                        const char *checkpoint) {
    const Etd2xAndroidActivityState actual = etd2x_android_activity_state();
    if (actual == expected) return true;
    ETD2X_LOGCE("JAVA", "%s: FAIL reason=super-not-called expected=%s actual=%s",
                checkpoint ? checkpoint : "activity-lifecycle",
                etd2x_android_activity_state_name(expected),
                etd2x_android_activity_state_name(actual));
    void *env = etd2x_jni_current_env();
    if (env && !etd2x_jni_exception_pending_host(env)) {
        char msg[192];
        snprintf(msg, sizeof(msg), "Activity %s did not call through to super.%s",
                 checkpoint ? checkpoint : "callback", checkpoint ? checkpoint : "callback");
        (void)etd2x_jni_throw_new_by_name(env, "android/app/SuperNotCalledException", msg);
    }
    main_java_dispatch_uncaught(checkpoint);
    return false;
}

typedef struct main_JavaRendererInputCaps {
    bool touch_down;
    bool touch_up;
    bool touch_move;
    bool touch_cancel;
    bool key_down;
} main_JavaRendererInputCaps;

enum { ETD2X_JAVA_TOUCH_MAX = ETD2X_INPUT_MAX_TOUCHES + 1 };

typedef struct main_JavaTouchSet {
    int count;
    int32_t ids[ETD2X_JAVA_TOUCH_MAX];
    float xs[ETD2X_JAVA_TOUCH_MAX];
    float ys[ETD2X_JAVA_TOUCH_MAX];
} main_JavaTouchSet;

typedef struct main_JavaInputPerf {
    uint64_t down_calls;
    uint64_t down_total_ms;
    uint64_t down_max_ms;
    uint64_t down_frame_max_ms;
    uint64_t direct_native_calls;
    uint64_t dex_fallback_calls;
} main_JavaInputPerf;

static bool main_dex_exact_method_exists(const char *source_path, const char *class_name,
                                    const char *method_name, const char *signature) {
    Etd2xDexMethodImage method = {0};
    bool class_found = false, method_found = false;
    Etd2xResult rc = etd2x_package_load_dex_method(source_path, class_name, method_name,
                                                    signature, &method,
                                                    &class_found, &method_found);
    etd2x_package_release_dex_method(&method);
    return rc == ETD2X_OK && class_found && method_found;
}

static void main_log_cocos_writable_path(const char *source_path) {
    static const char *cls = "org/cocos2dx/lib/Cocos2dxHelper";
    static const char *name = "getCocos2dxWritablePath";
    static const char *sig = "()Ljava/lang/String;";
    if (!main_dex_exact_method_exists(source_path, cls, name, sig)) return;
    Etd2xDexExecReport report = {0};
    Etd2xJniValue result = {0};
    Etd2xResult rc = etd2x_dex_execute_static_method(source_path, cls, name, sig,
                                                       etd2x_jni_current_env(), NULL, 0,
                                                       &result, &report);
    if (rc == ETD2X_OK && report.completed && result.l) {
        char path[1024];
        if (etd2x_jni_string_utf_copy(result.l, path, sizeof(path)))
            ETD2X_LOGCI("STORAGE", "Cocos writable path=%s", path);
        etd2x_jni_delete_local_ref_host(etd2x_jni_current_env(), result.l);
    } else {
        if (etd2x_diag_full_trace_enabled())
            ETD2X_LOGCW("STORAGE", "Cocos writable path probe failed rc=%s detail=%s",
                        etd2x_result_string(rc), report.detail);
        main_java_dispatch_uncaught("cocos-writable-path");
    }
}


typedef struct main_JavaAppletLifecycleState {
    bool focus_pending;
    AppletFocusState focus_state;
    bool exit_requested;
    uint64_t focus_events;
} main_JavaAppletLifecycleState;

static void main_java_applet_hook(AppletHookType hook, void *param) {
    main_JavaAppletLifecycleState *state = (main_JavaAppletLifecycleState *)param;
    if (!state) return;
    if (hook == AppletHookType_OnFocusState || hook == AppletHookType_OnResume) {
        state->focus_state = appletGetFocusState();
        state->focus_pending = true;
        state->focus_events++;
    } else if (hook == AppletHookType_OnExitRequest) {
        state->exit_requested = true;
    }
}

static void main_java_storage_commit(const char *reason) {
    /* Android lifecycle callbacks are allowed to hand persistence to worker
     * threads. Give those workers a short bounded grace period before forcing
     * stdio + SD filesystem commit at the platform boundary. */
    svcSleepThread(UINT64_C(200000000));
    (void)fflush(NULL);
    Result rc = fsdevCommitDevice("sdmc");
    Etd2xCompatStorageStats stats = {0};
    etd2x_compat_get_storage_stats(&stats);
    ETD2X_LOGCI("STORAGE",
                "commit reason=%s rc=0x%08x graceMs=200 mappedPaths=%llu writeOpens=%llu mutations=%llu",
                reason ? reason : "unknown", rc,
                (unsigned long long)stats.mapped_paths,
                (unsigned long long)stats.write_opens,
                (unsigned long long)stats.mutations);
    etd2x_diag_flush();
}

static bool main_java_activity_enter_background(const char *source_path,
                                           const char *activity_class,
                                           void *activity_ref, void *view_ref,
                                           const char *reason) {
    ETD2X_LOGCI("JAVA", "Activity background begin class=%s reason=%s",
                activity_class, reason ? reason : "unknown");
    bool ok = true;
    etd2x_android_set_window_focus(false);
    Etd2xJniValue focus_args[1] = {{0}};
    focus_args[0].z = 0;
    if (!main_dex_instance_callback(source_path, activity_class, "onWindowFocusChanged", "(Z)V",
                               activity_ref, focus_args, 1,
                               "stage14.java-window-focus-loss")) {
        ETD2X_LOGCW("JAVA", "Activity focus-loss callback incomplete class=%s", activity_class);
        ok = false;
    }
    if (etd2x_android_activity_state() == ETD2X_ANDROID_ACTIVITY_RESUMED) {
        if (!main_activity_lifecycle_callback(source_path, activity_class, "onPause",
                                   activity_ref, "stage14.java-onPause")) {
            ETD2X_LOGCW("JAVA", "Activity onPause callback incomplete class=%s", activity_class);
            ok = false;
        } else if (!main_java_require_activity_state(ETD2X_ANDROID_ACTIVITY_PAUSED, "onPause")) {
            ok = false;
        }
    }

    /* Cocos GLSurfaceView.onPause queues renderer.handleOnPause() onto the GL
     * thread. Drain before onStop and while the EGL context/native application
     * are still alive so applicationDidEnterBackground/save hooks can execute. */
    if (view_ref && !main_drain_glsurfaceview_events(source_path, view_ref)) {
        ETD2X_LOGCW("JAVA", "GLSurfaceView pause queue drain incomplete");
        ok = false;
    }
    if (!main_drain_activity_ui_events(source_path)) {
        ETD2X_LOGCW("JAVA", "Activity UI pause queue drain incomplete");
        ok = false;
    }

    const Etd2xAndroidActivityState state = etd2x_android_activity_state();
    if (state == ETD2X_ANDROID_ACTIVITY_STARTED || state == ETD2X_ANDROID_ACTIVITY_PAUSED) {
        if (!main_activity_lifecycle_callback(source_path, activity_class, "onStop",
                                   activity_ref, "stage14.java-onStop")) {
            ETD2X_LOGCW("JAVA", "Activity onStop callback incomplete class=%s", activity_class);
            ok = false;
        } else if (!main_java_require_activity_state(ETD2X_ANDROID_ACTIVITY_STOPPED, "onStop")) {
            ok = false;
        }
    }
    main_java_storage_commit(reason ? reason : "background");
    ETD2X_LOGCI("JAVA", "Activity background complete class=%s reason=%s ok=%d",
                activity_class, reason ? reason : "unknown", ok ? 1 : 0);
    return ok;
}

static bool main_java_activity_resume_from_background(const char *source_path,
                                                 const char *activity_class,
                                                 void *activity_ref, void *view_ref) {
    ETD2X_LOGCI("JAVA", "Activity foreground resume begin class=%s", activity_class);
    bool ok = true;
    if (etd2x_android_activity_state() == ETD2X_ANDROID_ACTIVITY_STOPPED) {
        if (!main_dex_instance_callback(source_path, activity_class, "onRestart", "()V",
                                   activity_ref, NULL, 0, "stage14.java-bg-onRestart"))
            ok = false;
    }
    if (!main_activity_lifecycle_callback(source_path, activity_class, "onStart",
                               activity_ref, "stage14.java-bg-onStart"))
        ok = false;
    else if (!main_java_require_activity_state(ETD2X_ANDROID_ACTIVITY_STARTED, "bg-onStart"))
        ok = false;
    if (!main_activity_lifecycle_callback(source_path, activity_class, "onResume",
                               activity_ref, "stage14.java-bg-onResume"))
        ok = false;
    else if (!main_java_require_activity_state(ETD2X_ANDROID_ACTIVITY_RESUMED, "bg-onResume"))
        ok = false;
    if (!main_java_activity_deliver_window_focus(source_path, activity_class,
                                            activity_ref, true,
                                            "stage14.java-bg-window-focus"))
        ok = false;
    if (view_ref && !main_drain_glsurfaceview_events(source_path, view_ref))
        ok = false;
    if (!main_drain_activity_ui_events(source_path))
        ok = false;
    ETD2X_LOGCI("JAVA", "Activity foreground resume complete class=%s ok=%d",
                activity_class, ok ? 1 : 0);
    return ok;
}

/* Main-loop watchdog. The loop emits the periodic stall report itself, so a
 * blocked main thread would also hide every other thread's state. A host
 * thread reports instead once the loop stays in one phase too long. */
#define MAIN_LOOP_STALL_MS UINT64_C(5000)
#define MAIN_LOOP_STALL_REPEAT_MS UINT64_C(15000)
static uint64_t main_g_main_loop_mark_ms;
static const char *main_g_main_loop_phase;
static bool main_g_main_loop_watchdog_stop;
static Thread main_g_main_loop_watchdog;
static bool main_g_main_loop_watchdog_running;
static pthread_t main_g_main_loop_host;

static void main_main_loop_mark(const char *phase) {
    __atomic_store_n(&main_g_main_loop_phase, phase, __ATOMIC_RELEASE);
    __atomic_store_n(&main_g_main_loop_mark_ms, main_monotonic_ms(), __ATOMIC_RELEASE);
}

static void main_main_loop_watchdog_main(void *arg) {
    (void)arg;
    uint64_t last_report_ms = 0;
    uint64_t sampled_mark = 0;
    unsigned samples = 0;
    while (!__atomic_load_n(&main_g_main_loop_watchdog_stop, __ATOMIC_ACQUIRE)) {
        svcSleepThread(500000000LL);
        const uint64_t mark = __atomic_load_n(&main_g_main_loop_mark_ms, __ATOMIC_ACQUIRE);
        const uint64_t now = main_monotonic_ms();
        if (mark != sampled_mark) { sampled_mark = mark; samples = 0; }
        if (mark && now >= mark && samples < 8 && now - mark >= (UINT64_C(250) << samples) &&
            !__atomic_load_n(&main_g_main_loop_watchdog_stop, __ATOMIC_ACQUIRE)) {
            ++samples;
            Etd2xNativeContext context = {0};
            const int rc = etd2x_bionic_thread_sample_context_probe(main_g_main_loop_host, &context);
            char pc_name[192] = "unmapped", lr_name[192] = "unmapped";
            if (!rc) {
                (void)etd2x_compat_describe_guest_address(context.pc, pc_name, sizeof(pc_name));
                (void)etd2x_compat_describe_guest_address(context.lr, lr_name, sizeof(lr_name));
            }
            ETD2X_LOGCI("PERF", "main-loop PC sample=%u phase=%s elapsedMs=%" PRIu64
                        " rc=%d pc=%p (%s) lr=%p (%s) sp=%p",
                        samples, __atomic_load_n(&main_g_main_loop_phase, __ATOMIC_ACQUIRE), now - mark,
                        rc, (void *)context.pc, pc_name, (void *)context.lr, lr_name, (void *)context.sp);
            if (!rc) {
                char registers[896] = {0};
                size_t used = 0;
                for (unsigned i = 0; i < 29; ++i) {
                    const int n = snprintf(registers + used, sizeof(registers) - used,
                                           " x%u=%#" PRIxPTR, i, context.x[i]);
                    if (n < 0 || (size_t)n >= sizeof(registers) - used) break;
                    used += (size_t)n;
                }
                ETD2X_LOGCI("PERF", "main-loop registers sample=%u fp=%p%s",
                            samples, (void *)context.fp, registers);
                if (context.probe_address) {
                    char words[640] = {0};
                    used = 0;
                    for (unsigned i = 0; i < 24; ++i) {
                        const int n = snprintf(words + used, sizeof(words) - used,
                                               " +%u=%#" PRIx64, 960u + i * 8u, context.probe_words[i]);
                        if (n < 0 || (size_t)n >= sizeof(words) - used) break;
                        used += (size_t)n;
                    }
                    ETD2X_LOGCI("PERF", "main-loop memory sample=%u base=x19 addr=%p%s",
                                samples, (void *)context.probe_address, words);
                }
            }
        }
        if (!mark || now < mark || now - mark < MAIN_LOOP_STALL_MS) {
            last_report_ms = 0;
            continue;
        }
        if (last_report_ms && now - last_report_ms < MAIN_LOOP_STALL_REPEAT_MS) continue;
        last_report_ms = now;
        const char *phase = __atomic_load_n(&main_g_main_loop_phase, __ATOMIC_ACQUIRE);
        ETD2X_LOGCW("JAVA", "main loop stalled phase=%s elapsedMs=%" PRIu64, phase ? phase : "-", now - mark);
        (void)etd2x_bionic_pthread_log_wait_states("main-loop-stalled");
        etd2x_diag_flush();
        /* Last: takes the main Looper's queue lock, which a stalled main
         * thread may hold. */
        etd2x_android_log_looper_states("main-loop-stalled");
        etd2x_diag_flush();
    }
}

static void main_main_loop_watchdog_start(void) {
    extern char _start[];
    ETD2X_LOGCI("PERF", "main-loop sample hostBase=%p", (void *)_start);
    main_g_main_loop_host = pthread_self();
    __atomic_store_n(&main_g_main_loop_watchdog_stop, false, __ATOMIC_RELEASE);
    main_main_loop_mark("start");
    /* The 64-entry snapshot with 24 frames per thread needs 48 KiB itself. */
    if (R_SUCCEEDED(threadCreate(&main_g_main_loop_watchdog, main_main_loop_watchdog_main, NULL, NULL,
                                 0x18000, 0x3B, -2))) {
        if (R_SUCCEEDED(threadStart(&main_g_main_loop_watchdog))) {
            main_g_main_loop_watchdog_running = true;
            return;
        }
        threadClose(&main_g_main_loop_watchdog);
    }
    ETD2X_LOGCW("JAVA", "main loop watchdog unavailable");
}

static void main_main_loop_watchdog_stop(void) {
    __atomic_store_n(&main_g_main_loop_mark_ms, 0, __ATOMIC_RELEASE);
    if (!main_g_main_loop_watchdog_running) return;
    __atomic_store_n(&main_g_main_loop_watchdog_stop, true, __ATOMIC_RELEASE);
    threadWaitForExit(&main_g_main_loop_watchdog);
    threadClose(&main_g_main_loop_watchdog);
    main_g_main_loop_watchdog_running = false;
}

/* Main-thread loop for an Activity that renders into its own SurfaceView from
 * an app-owned thread: dispatch main-Looper work and Choreographer callbacks
 * at display cadence until PLUS, applet exit or a process-exit request. */
static void main_java_touch_collect(const Etd2xInputSnapshot *input, const Etd2xDisplayPlan *plan,
                               main_JavaTouchSet *out);

/* Touchscreen frame -> Android MotionEvents through the Activity's view tree.
 * Used by the app-managed surface loop and by the GLSurfaceView loop when the
 * renderer exposes no direct input hooks. */
static void main_java_app_managed_dispatch_touches(const Etd2xInputSnapshot *input, const Etd2xDisplayPlan *plan) {
    main_JavaTouchSet set;
    main_java_touch_collect(input, plan, &set);
    Etd2xAndroidTouchPoint points[ETD2X_JAVA_TOUCH_MAX];
    for (int i = 0; i < set.count; ++i)
        points[i] = (Etd2xAndroidTouchPoint){.id = set.ids[i], .x = set.xs[i], .y = set.ys[i]};
    etd2x_android_dispatch_touches(etd2x_android_activity(), points, (size_t)set.count);
}

/* Rendering can continue while startup waits for an unrelated worker or
 * callback. Sample once without claiming that continuing frames are a stall. */
static void main_java_loop_diagnostics(uint64_t elapsed_ms, uint64_t buttons_down,
                                  bool exiting, bool *checkpoint_taken) {
    const char *reason = NULL;
    if (buttons_down & ETD2X_BTN_MINUS) reason = "manual-loading";
    else if (exiting) reason = "pre-exit";
    else if (!*checkpoint_taken && elapsed_ms >= 20000) {
        *checkpoint_taken = true;
        reason = "startup-checkpoint";
    }
    if (!reason) return;
    ETD2X_LOGCI("JAVA", "loop diagnostic reason=%s elapsedMs=%" PRIu64 " presents=%" PRIu64,
                reason, elapsed_ms, etd2x_graphics_present_count());
    etd2x_android_log_looper_states(reason);
    (void)etd2x_bionic_pthread_log_wait_states(reason);
}

typedef enum main_JavaActivityLoopResult {
    JAVA_ACTIVITY_WAIT, JAVA_ACTIVITY_EXIT, JAVA_ACTIVITY_ERROR,
    JAVA_ACTIVITY_TRANSITION, JAVA_ACTIVITY_GL_READY
} main_JavaActivityLoopResult;

static main_JavaActivityLoopResult main_java_activity_poll_render_state(Etd2xAndroidGLSurfaceViewInfo *info) {
    if (etd2x_android_has_pending_activity()) return JAVA_ACTIVITY_TRANSITION;
    if (etd2x_android_activity_finish_requested()) return JAVA_ACTIVITY_EXIT;
    Etd2xAndroidGLSurfaceViewInfo candidate = {0};
    Etd2xResult rc = etd2x_android_find_glsurfaceview(etd2x_jni_current_env(), &candidate);
    if (rc == ETD2X_OK && candidate.view_ref && candidate.renderer_ref && candidate.resumed) {
        *info = candidate;
        return JAVA_ACTIVITY_GL_READY;
    }
    if (candidate.view_ref) etd2x_jni_delete_local_ref_host(etd2x_jni_current_env(), candidate.view_ref);
    if (candidate.renderer_ref) etd2x_jni_delete_local_ref_host(etd2x_jni_current_env(), candidate.renderer_ref);
    if (rc != ETD2X_OK && rc != ETD2X_ERR_APK_UNSUPPORTED) return JAVA_ACTIVITY_ERROR;
    return JAVA_ACTIVITY_WAIT;
}

static main_JavaActivityLoopResult main_java_activity_event_loop(const char *apk_path, const Etd2xDisplayPlan *plan,
                                                       Etd2xAndroidGLSurfaceViewInfo *info) {
    uint64_t ticks = 0;
    const uint64_t begin_ms = main_monotonic_ms();
    uint64_t last_present_count = etd2x_graphics_present_count();
    uint64_t last_present_ms = begin_ms;
    uint64_t last_render_report_ms = 0;
    bool ok = true;
    main_JavaActivityLoopResult result = JAVA_ACTIVITY_EXIT;
    bool checkpoint_taken = false;
    const char *exit_reason = "applet";
    uint64_t last_input_us = 0, last_input_gap_report_us = 0;
    const bool input_ready = etd2x_input_init();
    if (input_ready) {
        /* Publish one snapshot before the first loop tick so InputManager
         * listeners registered during Activity.onCreate observe the already
         * connected controller as an Android device-add transition. */
        Etd2xInputSnapshot initial = {0};
        etd2x_input_update(&initial);
        etd2x_android_update_input_snapshot(&initial);
        ETD2X_LOGCI("INPUT", "app-managed input ready connected=%d style=0x%x handheld=%d",
                    initial.controller_connected ? 1 : 0, initial.style_set, initial.handheld ? 1 : 0);
    } else {
        ETD2X_LOGCW("INPUT", "app-managed input backend unavailable");
    }
    ETD2X_LOGI("Java Activity event loop active: main Looper + View frames + input; a rendering surface is optional; PLUS exits to hbmenu");
    main_main_loop_watchdog_start();
    for (;;) {
        main_JavaActivityLoopResult state = main_java_activity_poll_render_state(info);
        if (state != JAVA_ACTIVITY_WAIT) { result = state; exit_reason = "activity-state"; break; }
        const uint64_t turn_begin = armGetSystemTick();
        main_main_loop_mark("applet");
        if (!appletMainLoop()) {
            main_java_loop_diagnostics(main_monotonic_ms() - begin_ms, 0, true, &checkpoint_taken);
            break;
        }
        char process_exit_reason[256];
        if (etd2x_jni_process_exit_requested(process_exit_reason, sizeof(process_exit_reason))) {
            ETD2X_LOGT("stage14.java-process-exit: FAIL reason=%s", process_exit_reason);
            ok = false;
            exit_reason = "process-exit";
            break;
        }
        main_main_loop_mark("input");
        Etd2xInputSnapshot input = {0};
        etd2x_input_update(&input);
        const uint64_t input_us = main_fast_tick_us();
        if (last_input_us && input_us - last_input_us >= UINT64_C(50000) &&
            (!last_input_gap_report_us || input_us - last_input_gap_report_us >= UINT64_C(1000000))) {
            last_input_gap_report_us = input_us;
            ETD2X_LOGCI("INPUT", "main input gapUs=%" PRIu64 " touches=%d buttonsUp=0x%llx",
                        input_us - last_input_us, input.touch_count, (unsigned long long)input.buttons_up);
        }
        last_input_us = input_us;
        etd2x_android_update_input_snapshot(&input);
        etd2x_android_dispatch_controller_input(etd2x_android_activity(), &input);
        main_java_loop_diagnostics(main_monotonic_ms() - begin_ms, input.buttons_down,
                              (input.buttons_down & ETD2X_BTN_PLUS) != 0, &checkpoint_taken);
        if ((input.buttons_down & ETD2X_BTN_PLUS) != 0) { exit_reason = "plus"; break; }
        main_main_loop_mark("touch");
        main_java_app_managed_dispatch_touches(&input, plan);
        main_main_loop_mark("ui-events");
        if (!main_drain_activity_ui_events_budget(apk_path, UINT64_C(4000))) { ok = false; exit_reason = "ui-event"; break; }
        main_main_loop_mark("choreographer");
        if (!etd2x_android_do_frame()) { ok = false; exit_reason = "view-frame"; break; }
        etd2x_choreographer_pump();
        main_main_loop_mark("report");
        ++ticks;
        if (ticks % 600u == 0u)
            ETD2X_LOGCI("JAVA", "app-managed surface loop alive ticks=%" PRIu64 " elapsedMs=%" PRIu64,
                        ticks, main_monotonic_ms() - begin_ms);
        /* Inspect worker threads when presentation stops. Healthy idle worker
         * threads must not produce periodic "stall" dumps during gameplay. */
        const uint64_t presents = etd2x_graphics_present_count();
        const uint64_t now_ms = main_monotonic_ms();
        if (presents != last_present_count) {
            last_present_count = presents;
            last_present_ms = now_ms;
        }
        if ((presents || etd2x_android_has_attached_surface()) && now_ms - last_present_ms >= MAIN_LOOP_STALL_MS &&
            (!last_render_report_ms || now_ms - last_render_report_ms >= MAIN_LOOP_STALL_REPEAT_MS)) {
            last_render_report_ms = now_ms;
            ETD2X_LOGCW("JAVA", "app-managed presentation stalled elapsedMs=%" PRIu64 " presents=%" PRIu64,
                        now_ms - last_present_ms, presents);
            etd2x_android_log_looper_states("app-managed-loop");
            (void)etd2x_bionic_pthread_log_wait_states("app-managed-loop");
        }
        main_main_loop_mark("sleep");
        const uint64_t turn_ns = armTicksToNs(armGetSystemTick() - turn_begin);
        const uint64_t period_ns = UINT64_C(16666667);
        etd2x_android_main_looper_wait(turn_ns < period_ns ? period_ns - turn_ns : 0);
    }
    main_main_loop_watchdog_stop();
    etd2x_android_cancel_touches(etd2x_android_activity());
    if (input_ready) etd2x_input_shutdown();
    ETD2X_LOGT("stage14.java-app-managed-loop: %s ticks=%" PRIu64 " elapsedMs=%" PRIu64 " exit=%s",
               ok ? "PASS" : "FAIL", ticks, main_monotonic_ms() - begin_ms, exit_reason);
    return ok ? result : JAVA_ACTIVITY_ERROR;
}

static bool main_java_commit_activity_transition(const char *source_path, char *activity_class, size_t cap) {
    if (!etd2x_android_has_pending_activity()) return false;
    void *activity = etd2x_android_activity();
    const bool finishing = etd2x_android_activity_finish_requested();
    const Etd2xAndroidActivityState state = etd2x_android_activity_state();
    if ((state == ETD2X_ANDROID_ACTIVITY_RESUMED || state == ETD2X_ANDROID_ACTIVITY_STARTED) &&
        !main_java_activity_enter_background(source_path, activity_class, activity, NULL, "activity-transition")) return false;
    if (finishing) {
        etd2x_android_mark_activity_finishing();
        if (!main_activity_lifecycle_callback(source_path, activity_class, "onDestroy", activity, "stage14.java-transition-onDestroy")) return false;
    }
    etd2x_android_set_activity_window_attached(false);
    return !etd2x_jni_exception_pending_host(etd2x_jni_current_env()) &&
           etd2x_android_take_started_activity(etd2x_jni_current_env(), activity_class, cap);
}

static void main_java_activity_shutdown(const char *source_path, const char *activity_class,
                                   void *activity_ref, void *view_ref,
                                   bool already_backgrounded) {
    ETD2X_LOGCI("JAVA", "Activity graceful shutdown begin class=%s backgrounded=%d",
                activity_class, already_backgrounded ? 1 : 0);
    etd2x_android_mark_activity_finishing();
    if (!already_backgrounded)
        (void)main_java_activity_enter_background(source_path, activity_class, activity_ref,
                                             view_ref, "final-exit");
    if (!main_activity_lifecycle_callback(source_path, activity_class, "onDestroy",
                               activity_ref, "stage14.java-onDestroy"))
        ETD2X_LOGCW("JAVA", "Activity onDestroy callback incomplete class=%s", activity_class);
    else
        (void)main_java_require_activity_state(ETD2X_ANDROID_ACTIVITY_DESTROYED, "onDestroy");
    etd2x_android_set_activity_window_attached(false);
    etd2x_android_mark_activity_destroyed();
    main_java_storage_commit("final-destroy");
    ETD2X_LOGCI("JAVA", "Activity graceful shutdown complete class=%s", activity_class);
}

static main_JavaRendererInputCaps main_probe_java_renderer_input(const char *source_path,
                                                        const char *renderer_class) {
    main_JavaRendererInputCaps caps = {0};
    caps.touch_down = main_dex_exact_method_exists(source_path, renderer_class,
                                               "handleActionDown", "(IFFD)V");
    caps.touch_up = main_dex_exact_method_exists(source_path, renderer_class,
                                             "handleActionUp", "(IFFD)V");
    caps.touch_move = main_dex_exact_method_exists(source_path, renderer_class,
                                               "handleActionMove", "([I[F[FD)V");
    caps.touch_cancel = main_dex_exact_method_exists(source_path, renderer_class,
                                                 "handleActionCancel", "([I[F[FD)V");
    caps.key_down = main_dex_exact_method_exists(source_path, renderer_class,
                                             "handleKeyDown", "(I)V");
    ETD2X_LOGCI("INPUT", "Java renderer input class=%s down=%d up=%d move=%d cancel=%d key=%d",
                renderer_class, caps.touch_down, caps.touch_up, caps.touch_move,
                caps.touch_cancel, caps.key_down);
    return caps;
}

static void main_prewarm_java_renderer_input(const char *source_path,
                                        const char *renderer_class,
                                        const main_JavaRendererInputCaps *caps) {
    if (!source_path || !renderer_class || !caps) return;
    struct main_NativeInputMethod {
        const char *name;
        const char *signature;
        bool enabled;
    } methods[] = {
        {"nativeTouchesBegin", "(IFFD)V", caps->touch_down},
        {"nativeTouchesEnd", "(IFFD)V", caps->touch_up},
        {"nativeTouchesMove", "([I[F[FD)V", caps->touch_move},
        {"nativeTouchesCancel", "([I[F[FD)V", caps->touch_cancel},
        {"nativeKeyDown", "(I)Z", caps->key_down},
    };
    size_t warmed = 0;
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i) {
        if (!methods[i].enabled ||
            !main_dex_exact_method_exists(source_path, renderer_class,
                                     methods[i].name, methods[i].signature))
            continue;
        bool found = false;
        Etd2xResult rc = etd2x_dex_prewarm_native_method(renderer_class,
                                                          methods[i].name,
                                                          methods[i].signature,
                                                          &found);
        if (rc == ETD2X_OK && found) warmed++;
        else ETD2X_LOGCW("INPUT", "native input prewarm miss class=%s method=%s sig=%s rc=%s",
                         renderer_class, methods[i].name, methods[i].signature,
                         etd2x_result_string(rc));
    }
    ETD2X_LOGCI("INPUT", "Java renderer native input prewarm methods=%zu", warmed);
}

static bool main_dex_instance_call_quiet(const char *source_path, const char *class_name,
                                    const char *method_name, const char *signature,
                                    void *receiver_ref, const Etd2xJniValue *args,
                                    size_t arg_count, uint64_t frame_number) {
    Etd2xDexExecReport report = {0};
    Etd2xJniValue result = {0};
    Etd2xResult rc = etd2x_dex_execute_instance_method(source_path, class_name, method_name,
                                                        signature, etd2x_jni_current_env(),
                                                        receiver_ref, args, arg_count,
                                                        &result, &report);
    if (rc == ETD2X_OK && report.completed) return true;
    ETD2X_LOGCE("INPUT", "renderer input failed frame=%" PRIu64
                " class=%s method=%s sig=%s rc=%s pc=%#x opcode=0x%02x detail=%s",
                frame_number, class_name, method_name, signature,
                etd2x_result_string(rc), report.pc, report.opcode,
                report.detail[0] ? report.detail : "none");
    main_java_dispatch_uncaught("renderer-input");
    return false;
}

static int main_java_touch_find(const main_JavaTouchSet *set, int32_t id) {
    if (!set) return -1;
    for (int i = 0; i < set->count; ++i) if (set->ids[i] == id) return i;
    return -1;
}

static void main_java_touch_collect(const Etd2xInputSnapshot *input, const Etd2xDisplayPlan *plan,
                               main_JavaTouchSet *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!input || !plan) return;
    for (int i = 0; i < input->touch_count && out->count < ETD2X_JAVA_TOUCH_MAX; ++i) {
        float x = 0.0f, y = 0.0f;
        if (!etd2x_display_map_touch(plan, input->touches[i].x, input->touches[i].y, &x, &y))
            continue;
        const int n = out->count++;
        out->ids[n] = (int32_t)input->touches[i].id;
        out->xs[n] = x;
        out->ys[n] = y;
    }
    /* Controller convenience for touch-first Android games. A behaves like a
     * stable center-screen finger while held.  Use a reserved positive ID that
     * cannot collide with the Switch touchscreen's small finger IDs. */
    if ((input->buttons & ETD2X_BTN_A) != 0 && out->count < ETD2X_JAVA_TOUCH_MAX) {
        const int n = out->count++;
        out->ids[n] = INT32_MAX;
        out->xs[n] = (float)plan->logical_width * 0.5f;
        out->ys[n] = (float)plan->logical_height * 0.5f;
    }
}

static bool main_java_renderer_touch_scalar(const char *source_path, const char *renderer_class,
                                       void *renderer_ref, const char *method_name,
                                       int32_t id, float x, float y, double event_time,
                                       uint64_t frame_number, main_JavaInputPerf *perf) {
    Etd2xJniValue args[4] = {{0}};
    args[0].i = id;
    args[1].f = x;
    args[2].f = y;
    args[3].d = event_time;

    const char *native_name = NULL;
    if (strcmp(renderer_class, "org/cocos2dx/lib/Cocos2dxRenderer") == 0) {
        if (strcmp(method_name, "handleActionDown") == 0) native_name = "nativeTouchesBegin";
        else if (strcmp(method_name, "handleActionUp") == 0) native_name = "nativeTouchesEnd";
    }
    const uint64_t begin_ms = main_monotonic_ms();
    bool ok = false;
    if (native_name) {
        Etd2xJniValue ignored = {0};
        Etd2xResult rc = etd2x_dex_call_static_native_method(
            source_path, renderer_class, native_name, "(IFFD)V",
            etd2x_jni_current_env(), args, 4, &ignored);
        if (rc == ETD2X_OK) {
            ok = true;
            if (perf) perf->direct_native_calls++;
        }
    }
    if (!ok) {
        ok = main_dex_instance_call_quiet(source_path, renderer_class, method_name, "(IFFD)V",
                                     renderer_ref, args, 4, frame_number);
        if (perf) perf->dex_fallback_calls++;
    }
    if (perf && strcmp(method_name, "handleActionDown") == 0) {
        const uint64_t elapsed = main_monotonic_ms() - begin_ms;
        perf->down_calls++;
        perf->down_total_ms += elapsed;
        if (elapsed > perf->down_max_ms) perf->down_max_ms = elapsed;
    }
    return ok;
}

static bool main_java_renderer_touch_vector(const char *source_path, const char *renderer_class,
                                       void *renderer_ref, const char *method_name,
                                       const main_JavaTouchSet *touches, double event_time,
                                       uint64_t frame_number, main_JavaInputPerf *perf) {
    if (!touches || touches->count <= 0) return true;
    void *env = etd2x_jni_current_env();
    void *ids = etd2x_jni_new_local_int_array(env, touches->count);
    void *xs = etd2x_jni_new_local_float_array(env, touches->count);
    void *ys = etd2x_jni_new_local_float_array(env, touches->count);
    if (!ids || !xs || !ys ||
        etd2x_jni_int_array_write(ids, 0, touches->count, touches->ids) != ETD2X_OK ||
        etd2x_jni_float_array_write(xs, 0, touches->count, touches->xs) != ETD2X_OK ||
        etd2x_jni_float_array_write(ys, 0, touches->count, touches->ys) != ETD2X_OK) {
        if (ids) etd2x_jni_delete_local_ref_host(env, ids);
        if (xs) etd2x_jni_delete_local_ref_host(env, xs);
        if (ys) etd2x_jni_delete_local_ref_host(env, ys);
        ETD2X_LOGCE("INPUT", "failed to materialize Java touch arrays count=%d", touches->count);
        return false;
    }
    Etd2xJniValue args[4] = {{0}};
    args[0].l = ids; args[1].l = xs; args[2].l = ys; args[3].d = event_time;
    const char *native_name = NULL;
    if (strcmp(renderer_class, "org/cocos2dx/lib/Cocos2dxRenderer") == 0) {
        if (strcmp(method_name, "handleActionMove") == 0) native_name = "nativeTouchesMove";
        else if (strcmp(method_name, "handleActionCancel") == 0) native_name = "nativeTouchesCancel";
    }
    bool ok = false;
    if (native_name) {
        Etd2xJniValue ignored = {0};
        ok = etd2x_dex_call_static_native_method(source_path, renderer_class, native_name,
                                                  "([I[F[FD)V", env, args, 4, &ignored) == ETD2X_OK;
        if (ok && perf) perf->direct_native_calls++;
    }
    if (!ok) {
        ok = main_dex_instance_call_quiet(source_path, renderer_class, method_name,
                                     "([I[F[FD)V", renderer_ref, args, 4, frame_number);
        if (perf) perf->dex_fallback_calls++;
    }
    etd2x_jni_delete_local_ref_host(env, ys);
    etd2x_jni_delete_local_ref_host(env, xs);
    etd2x_jni_delete_local_ref_host(env, ids);
    return ok;
}

static bool main_java_renderer_dispatch_input(const char *source_path, const char *renderer_class,
                                         void *renderer_ref, const main_JavaRendererInputCaps *caps,
                                         const Etd2xInputSnapshot *input,
                                         const Etd2xDisplayPlan *plan,
                                         main_JavaTouchSet *previous,
                                         uint64_t frame_number, uint64_t *io_event_count,
                                         main_JavaInputPerf *perf) {
    if (!source_path || !renderer_class || !renderer_ref || !caps || !input || !plan || !previous)
        return false;
    main_JavaTouchSet current = {0};
    main_java_touch_collect(input, plan, &current);
    const double event_time = (double)main_monotonic_ms() / 1000.0;

    for (int i = 0; i < current.count; ++i) {
        if (main_java_touch_find(previous, current.ids[i]) >= 0) continue;
        if (caps->touch_down &&
            !main_java_renderer_touch_scalar(source_path, renderer_class, renderer_ref,
                                        "handleActionDown", current.ids[i], current.xs[i], current.ys[i],
                                        event_time, frame_number, perf)) return false;
        if (caps->touch_down && io_event_count) (*io_event_count)++;
    }

    bool moved = false;
    for (int i = 0; i < current.count; ++i) {
        const int old = main_java_touch_find(previous, current.ids[i]);
        if (old >= 0 && (previous->xs[old] != current.xs[i] || previous->ys[old] != current.ys[i])) {
            moved = true;
            break;
        }
    }
    if (moved && current.count > 0 && caps->touch_move) {
        if (!main_java_renderer_touch_vector(source_path, renderer_class, renderer_ref,
                                        "handleActionMove", &current, event_time,
                                        frame_number, perf)) return false;
        if (io_event_count) (*io_event_count)++;
    }

    for (int i = 0; i < previous->count; ++i) {
        if (main_java_touch_find(&current, previous->ids[i]) >= 0) continue;
        if (caps->touch_up &&
            !main_java_renderer_touch_scalar(source_path, renderer_class, renderer_ref,
                                        "handleActionUp", previous->ids[i], previous->xs[i], previous->ys[i],
                                        event_time, frame_number, perf)) return false;
        if (caps->touch_up && io_event_count) (*io_event_count)++;
    }

    if ((input->buttons_down & ETD2X_BTN_B) != 0 && caps->key_down) {
        Etd2xJniValue arg = {0};
        arg.i = 4; /* Android KEYCODE_BACK. */
        bool key_ok = false;
        if (strcmp(renderer_class, "org/cocos2dx/lib/Cocos2dxRenderer") == 0) {
            Etd2xJniValue key_result = {0};
            key_ok = etd2x_dex_call_static_native_method(
                source_path, renderer_class, "nativeKeyDown", "(I)Z",
                etd2x_jni_current_env(), &arg, 1, &key_result) == ETD2X_OK;
            if (key_ok && perf) perf->direct_native_calls++;
        }
        if (!key_ok) {
            key_ok = main_dex_instance_call_quiet(source_path, renderer_class, "handleKeyDown", "(I)V",
                                             renderer_ref, &arg, 1, frame_number);
            if (perf) perf->dex_fallback_calls++;
        }
        if (!key_ok) return false;
        if (io_event_count) (*io_event_count)++;
    }

    *previous = current;
    return true;
}

static bool main_java_renderer_cancel_input(const char *source_path, const char *renderer_class,
                                       void *renderer_ref, const main_JavaRendererInputCaps *caps,
                                       main_JavaTouchSet *previous, uint64_t frame_number,
                                       uint64_t *io_event_count, main_JavaInputPerf *perf) {
    if (!previous || previous->count == 0) return true;
    bool ok = true;
    if (caps && caps->touch_cancel)
        ok = main_java_renderer_touch_vector(source_path, renderer_class, renderer_ref,
                                        "handleActionCancel", previous,
                                        (double)main_monotonic_ms() / 1000.0,
                                        frame_number, perf);
    if (ok && caps && caps->touch_cancel && io_event_count) (*io_event_count)++;
    memset(previous, 0, sizeof(*previous));
    return ok;
}


static bool main_java_expand_component_class(const char *package_name, const char *declared,
                                        char *out, size_t out_cap) {
    if (!package_name || !package_name[0] || !declared || !declared[0] || !out || out_cap == 0)
        return false;
    int n;
    if (declared[0] == '.')
        n = snprintf(out, out_cap, "%s%s", package_name, declared);
    else if (strchr(declared, '.') == NULL)
        n = snprintf(out, out_cap, "%s.%s", package_name, declared);
    else
        n = snprintf(out, out_cap, "%s", declared);
    if (n <= 0 || (size_t)n >= out_cap) return false;
    for (char *c = out; *c; ++c) if (*c == '.') *c = '/';
    return true;
}

typedef struct main_JavaOrientationBridge {
    Etd2xDisplayPlan *plan;
    bool *graphics_ready;
} main_JavaOrientationBridge;

static bool main_java_orientation_preserves_current_aspect(int32_t requested) {
    switch (requested) {
        case ETD2X_SCREEN_ORIENTATION_USER:
        case ETD2X_SCREEN_ORIENTATION_BEHIND:
        case ETD2X_SCREEN_ORIENTATION_SENSOR:
        case ETD2X_SCREEN_ORIENTATION_NOSENSOR:
        case ETD2X_SCREEN_ORIENTATION_FULL_SENSOR:
        case ETD2X_SCREEN_ORIENTATION_FULL_USER:
        case ETD2X_SCREEN_ORIENTATION_LOCKED:
            return true;
        default:
            return false;
    }
}

static bool main_java_apply_requested_orientation(int32_t requested_orientation,
                                             int32_t *out_width_pixels,
                                             int32_t *out_height_pixels,
                                             int32_t *out_display_rotation,
                                             void *user_data) {
    main_JavaOrientationBridge *bridge = (main_JavaOrientationBridge *)user_data;
    if (!bridge || !bridge->plan || !out_width_pixels || !out_height_pixels ||
        !out_display_rotation) return false;

    Etd2xDisplayPlan next = *bridge->plan;
    if (main_java_orientation_preserves_current_aspect(requested_orientation)) {
        /* ETD2X has no device-rotation sensor contract yet. Sensor/user/locked
         * requests therefore keep the currently presented orientation instead
         * of spuriously snapping a portrait game back to Switch landscape. */
        next.screen_orientation = requested_orientation;
        next.orientation_declared = true;
    } else {
        const bool declared = requested_orientation != ETD2X_SCREEN_ORIENTATION_UNSPECIFIED;
        if (!etd2x_display_make_plan(requested_orientation, declared,
                                     bridge->plan->physical_width,
                                     bridge->plan->physical_height, &next))
            return false;
    }

    const bool presentation_changed =
        next.logical_width != bridge->plan->logical_width ||
        next.logical_height != bridge->plan->logical_height ||
        next.rotation != bridge->plan->rotation ||
        next.reverse != bridge->plan->reverse ||
        next.aspect != bridge->plan->aspect;

    if (presentation_changed && bridge->graphics_ready && *bridge->graphics_ready &&
        !etd2x_graphics_configure_presentation(&next)) {
        ETD2X_LOGCW("DISPLAY",
                    "requested orientation rejected requested=%d reason=presentation-config",
                    requested_orientation);
        return false;
    }

    if (presentation_changed)
        etd2x_native_window_prepare(next.logical_width, next.logical_height, 0);
    *bridge->plan = next;

    *out_width_pixels = next.logical_width;
    *out_height_pixels = next.logical_height;
    *out_display_rotation = etd2x_display_android_rotation(&next);
    ETD2X_LOGCI("DISPLAY",
                "requested orientation applied requested=%d mode=%s logical=%dx%d presentationRotation=%d androidRotation=%d changed=%u",
                requested_orientation,
                next.aspect == ETD2X_DISPLAY_PORTRAIT ? "portrait" : "landscape",
                next.logical_width, next.logical_height, (int)next.rotation,
                (int)*out_display_rotation, presentation_changed ? 1u : 0u);
    return true;
}

static void main_java_preflight_startup_root(const char *source_path,
                                        Etd2xLinker *linker,
                                        const char *class_name,
                                        const char *method_name,
                                        const char *signature,
                                        bool is_static,
                                        const char *phase) {
    if (!source_path || !linker || !class_name || !method_name || !signature) return;
    Etd2xDexPreflightReport report = {0};
    const Etd2xResult rc = etd2x_dex_preflight_method_tree(
        source_path, class_name, method_name, signature, is_static, &report);
    if (rc != ETD2X_OK) {
        ETD2X_LOGCW("DEX-PREFLIGHT",
                    "startup root scan failed phase=%s class=%s method=%s sig=%s rc=%d",
                    phase ? phase : "-", class_name, method_name, signature, (int)rc);
        return;
    }
    ETD2X_LOGCI("DEX-PREFLIGHT",
                "startup root phase=%s class=%s method=%s sig=%s methods=%zu invokes=%zu package=%zu framework=%zu unresolvedMethods=%zu unresolvedNative=%zu unresolvedFields=%zu unsupportedOpcodes=%zu truncated=%u",
                phase ? phase : "-", class_name, method_name, signature,
                report.methods_scanned, report.invoke_refs, report.package_targets,
                report.framework_targets, report.unresolved_methods,
                report.unresolved_native_methods, report.unresolved_fields,
                report.unsupported_opcode_sites, report.truncated ? 1u : 0u);
}

/* Android's ActivityThread installs manifest ContentProviders before any
 * Activity is created. A large number of SDKs use this as their process
 * initializer (AndroidX Startup, Firebase, Facebook, etc). Skipping this phase
 * leaves otherwise valid Java/native code in a partially initialized state.
 *
 * ETD2X models the main app process only. Providers explicitly assigned to a
 * different process are therefore left for a future multi-process layer. */
static size_t main_java_install_manifest_providers(const char *source_path,
                                              const Etd2xApkInfo *info,
                                              Etd2xLinker *linker,
                                              void **keepalive,
                                              size_t keepalive_cap) {
    if (!source_path || !info || !linker || !keepalive || keepalive_cap == 0) return 0;
    void *env = etd2x_jni_current_env();
    if (!env) return 0;

    size_t installed = 0;
    bool used[ETD2X_APK_PROVIDER_MAX] = {0};
    for (size_t pass = 0; pass < info->provider_count && pass < ETD2X_APK_PROVIDER_MAX; ++pass) {
        size_t pick = SIZE_MAX;
        int32_t best_order = INT32_MIN;
        for (size_t i = 0; i < info->provider_count && i < ETD2X_APK_PROVIDER_MAX; ++i) {
            const Etd2xManifestProvider *p = &info->providers[i];
            if (used[i] || !p->enabled) continue;
            if (pick == SIZE_MAX || p->init_order > best_order) {
                pick = i;
                best_order = p->init_order;
            }
        }
        if (pick == SIZE_MAX) break;
        used[pick] = true;
        const Etd2xManifestProvider *decl = &info->providers[pick];

        if (decl->process_name[0] && strcmp(decl->process_name, info->package_name) != 0) {
            ETD2X_LOGCI("JAVA", "ContentProvider skip class=%s process=%s reason=separate-process",
                        decl->class_name, decl->process_name);
            continue;
        }

        char class_name[ETD2X_APK_ACTIVITY_CAP + ETD2X_APK_PACKAGE_CAP + 4u];
        if (!main_java_expand_component_class(info->package_name, decl->class_name,
                                         class_name, sizeof(class_name))) {
            ETD2X_LOGCW("JAVA", "ContentProvider invalid class declaration=%s", decl->class_name);
            continue;
        }

        /* Advertising/monetization providers are optional platform integrations
         * on Android. ETD2X has no ad presentation or monetization backend, so
         * executing their manifest bootstrap is both useless and dangerous: it
         * can spawn SDK workers whose uncaught failures correctly terminate the
         * Android process before the game Activity starts. Report the service as
         * unavailable instead of manufacturing a successful SDK environment. */
        const char *unavailable_reason = etd2x_optional_sdk_unavailable_reason(class_name);
        if (unavailable_reason) {
            ETD2X_LOGCI("OPTIONAL", "ContentProvider skip class=%s reason=%s",
                        class_name, unavailable_reason);
            continue;
        }

        void *class_ref = etd2x_jni_find_class_ref(env, class_name);
        if (!class_ref) {
            ETD2X_LOGCW("JAVA", "ContentProvider class unavailable class=%s", class_name);
            if (etd2x_jni_exception_pending_host(env)) etd2x_jni_exception_clear_host(env);
            continue;
        }
        etd2x_jni_delete_local_ref_host(env, class_ref);

        main_java_preflight_startup_root(source_path, linker, class_name, "<init>", "()V", false,
                                    "content-provider-ctor");
        main_java_preflight_startup_root(source_path, linker, class_name, "onCreate", "()Z", false,
                                    "content-provider-onCreate");

        void *provider_ref = etd2x_jni_new_local_object(env, class_name);
        if (!provider_ref) {
            ETD2X_LOGCW("JAVA", "ContentProvider allocation failed class=%s", class_name);
            if (etd2x_jni_exception_pending_host(env)) etd2x_jni_exception_clear_host(env);
            continue;
        }

        Etd2xDexExecReport report = {0};
        Etd2xJniValue result = {0};
        Etd2xResult rc = etd2x_dex_execute_instance_method(
            source_path, class_name, "<init>", "()V", env, provider_ref,
            NULL, 0, &result, &report);
        if (rc == ETD2X_OK && report.completed) {
            memset(&report, 0, sizeof(report));
            memset(&result, 0, sizeof(result));
            rc = etd2x_dex_execute_instance_method(
                source_path, class_name, "onCreate", "()Z", env, provider_ref,
                NULL, 0, &result, &report);
        }

        if (rc != ETD2X_OK || !report.completed) {
            ETD2X_LOGCW("JAVA",
                        "ContentProvider startup failed class=%s pc=%#x opcode=0x%02x detail=%s",
                        class_name, report.pc, report.opcode,
                        report.detail[0] ? report.detail : etd2x_result_string(rc));
            main_java_dispatch_uncaught("manifest-content-provider");
            etd2x_jni_delete_local_ref_host(env, provider_ref);
            continue;
        }

        rc = etd2x_android_publish_content_provider(env, provider_ref);
        if (rc != ETD2X_OK) {
            ETD2X_LOGCE("JAVA", "ContentProvider publication failed class=%s rc=%d", class_name, (int)rc);
            etd2x_jni_delete_local_ref_host(env, provider_ref);
            main_java_dispatch_uncaught("manifest-content-provider-publish");
            continue;
        }
        void *global = etd2x_jni_new_global_ref_host(env, provider_ref);
        etd2x_jni_delete_local_ref_host(env, provider_ref);
        if (global && installed < keepalive_cap) keepalive[installed++] = global;
        else if (global) etd2x_jni_delete_global_ref_host(env, global);

        ETD2X_LOGCI("JAVA", "ContentProvider installed class=%s initOrder=%d onCreate=%d",
                    class_name, decl->init_order, result.z ? 1 : 0);
    }
    return installed;
}




/* java gl thread */
/* GLSurfaceView callbacks and queueEvent work belong to one GL thread. The
 * Activity thread keeps servicing its Looper while the renderer is busy or
 * waiting for a posted UI action. No guest callback runs under the job lock. */
typedef struct main_JavaGlThread {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    void (*run)(void *);
    void *argument;
    void *exception;
    bool started, ready, attached, busy, stop;
    const char *source_path;
} main_JavaGlThread;
static main_JavaGlThread main_g_java_gl_thread;

static void *main_java_gl_worker(void *opaque) {
    main_JavaGlThread *worker = opaque;
    void *env = NULL;
    const bool attached = etd2x_jni_attach_current_thread_host(&env) == ETD2X_OK;
    if (attached) {
        (void)etd2x_thread_policy_pin_render();
        ETD2X_LOGCI("JAVA-GL", "dedicated GL thread attached; Activity Looper stays on UI thread");
    }
    pthread_mutex_lock(&worker->lock);
    worker->attached = attached;
    worker->ready = true;
    pthread_cond_broadcast(&worker->cond);
    while (attached && !worker->stop) {
        while (!worker->busy && !worker->stop)
            pthread_cond_wait(&worker->cond, &worker->lock);
        if (worker->stop) break;
        void (*run)(void *) = worker->run;
        void *argument = worker->argument;
        pthread_mutex_unlock(&worker->lock);
        const bool framed = etd2x_jni_push_local_frame_host(env, 64) == ETD2X_OK;
        if (framed) run(argument);
        void *exception = etd2x_jni_exception_occurred_host(env);
        void *global = exception ? etd2x_jni_new_global_ref_host(env, exception) : NULL;
        if (exception) etd2x_jni_exception_clear_host(env);
        if (framed) (void)etd2x_jni_pop_local_frame_host(env, NULL);
        pthread_mutex_lock(&worker->lock);
        worker->exception = global;
        worker->busy = false;
        pthread_cond_broadcast(&worker->cond);
    }
    pthread_mutex_unlock(&worker->lock);
    if (attached) etd2x_jni_detach_current_thread_host();
    return NULL;
}

static bool main_java_gl_thread_start(const char *source_path) {
    main_JavaGlThread *w = &main_g_java_gl_thread;
    memset(w, 0, sizeof(*w));
    w->source_path = source_path;
    if (pthread_mutex_init(&w->lock, NULL) != 0) return false;
    if (pthread_cond_init(&w->cond, NULL) != 0) {
        pthread_mutex_destroy(&w->lock);
        return false;
    }
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        pthread_cond_destroy(&w->cond);
        pthread_mutex_destroy(&w->lock);
        return false;
    }
    /* Native renderer initialization previously used the launcher stack. */
    int rc = pthread_attr_setstacksize(&attr, 4u * 1024u * 1024u);
    if (rc == 0) rc = pthread_create(&w->thread, &attr, main_java_gl_worker, w);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        pthread_cond_destroy(&w->cond);
        pthread_mutex_destroy(&w->lock);
        return false;
    }
    w->started = true;
    pthread_mutex_lock(&w->lock);
    while (!w->ready) pthread_cond_wait(&w->cond, &w->lock);
    const bool attached = w->attached;
    pthread_mutex_unlock(&w->lock);
    return attached;
}

static bool main_java_gl_call(void (*run)(void *), void *argument) {
    main_JavaGlThread *w = &main_g_java_gl_thread;
    if (!w->started || pthread_equal(pthread_self(), w->thread)) {
        run(argument);
        return true;
    }
    if (!w->attached) return false;
    bool ok = true;
    pthread_mutex_lock(&w->lock);
    /* One Activity-thread producer; recursive submission from a UI callback
     * would violate the outstanding renderer's ordering. */
    if (w->busy) { pthread_mutex_unlock(&w->lock); return false; }
    w->run = run;
    w->argument = argument;
    w->busy = true;
    pthread_cond_broadcast(&w->cond);
    while (w->busy) {
        pthread_mutex_unlock(&w->lock);
        if (ok && !main_drain_activity_ui_events(w->source_path)) ok = false;
        pthread_mutex_lock(&w->lock);
        if (w->busy) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += 1000000;
            if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
            (void)pthread_cond_timedwait(&w->cond, &w->lock, &until);
        }
    }
    void *exception = w->exception;
    w->exception = NULL;
    pthread_mutex_unlock(&w->lock);
    if (exception) {
        void *env = etd2x_jni_current_env();
        if (!etd2x_jni_exception_pending_host(env)) (void)etd2x_jni_throw_host(env, exception);
        etd2x_jni_delete_global_ref_host(env, exception);
        ok = false;
    }
    return ok;
}

static void main_java_gl_thread_stop(void) {
    main_JavaGlThread *w = &main_g_java_gl_thread;
    if (!w->started) return;
    pthread_mutex_lock(&w->lock);
    w->stop = true;
    pthread_cond_broadcast(&w->cond);
    pthread_mutex_unlock(&w->lock);
    pthread_join(w->thread, NULL);
    pthread_cond_destroy(&w->cond);
    pthread_mutex_destroy(&w->lock);
    memset(w, 0, sizeof(*w));
}

typedef struct main_JavaGlEglJob {
    const Etd2xAndroidGLSurfaceViewInfo *info;
    main_Etd2xJavaEglState *egl;
    int width, height;
    bool ok;
    int32_t error;
} main_JavaGlEglJob;
static void main_java_gl_egl_start_job(void *arg) {
    main_JavaGlEglJob *j = arg;
    j->ok = main_java_egl_start(j->info, j->width, j->height, j->egl);
}
static bool main_java_gl_egl_start(const Etd2xAndroidGLSurfaceViewInfo *info, int width,
                              int height, main_Etd2xJavaEglState *egl) {
    main_JavaGlEglJob j = {.info=info, .egl=egl, .width=width, .height=height};
    return main_java_gl_call(main_java_gl_egl_start_job, &j) && j.ok;
}
static void main_java_gl_swap_job(void *arg) {
    main_JavaGlEglJob *j = arg;
    j->ok = etd2x_eglSwapBuffers(j->egl->display, j->egl->surface);
    if (!j->ok) j->error = etd2x_eglGetError();
}
static bool main_java_gl_swap(main_Etd2xJavaEglState *egl, int32_t *error) {
    main_JavaGlEglJob j = {.egl=egl};
    const bool ok = main_java_gl_call(main_java_gl_swap_job, &j) && j.ok;
    if (error) *error = j.error;
    return ok;
}
static void main_java_gl_egl_shutdown_job(void *arg) { main_java_egl_shutdown(arg); }

typedef struct main_JavaGlDexJob {
    const char *source_path, *class_name, *method, *signature;
    void *receiver;
    Etd2xJniValue args[8];
    size_t count;
    bool native;
    Etd2xResult rc;
    Etd2xDexExecReport *report;
} main_JavaGlDexJob;
static void main_java_gl_dex_job(void *arg) {
    main_JavaGlDexJob *j = arg;
    Etd2xJniValue ignored = {0};
    void *env = etd2x_jni_current_env();
    if (j->native)
        j->rc = etd2x_dex_call_static_native_method(j->source_path, j->class_name,
            j->method, j->signature, env, j->args, j->count, &ignored);
    else
        j->rc = etd2x_dex_execute_instance_method(j->source_path, j->class_name,
            j->method, j->signature, env, j->receiver, j->args, j->count,
            &ignored, j->report);
}
/* All renderer callbacks return void. Promote only reference arguments; the
 * worker's JNIEnv/local frame owns anything created while interpreting them. */
static Etd2xResult main_java_gl_execute(const char *source_path, const char *class_name,
    const char *method, const char *signature, void *receiver,
    const Etd2xJniValue *args, size_t count, bool native, Etd2xDexExecReport *report) {
    if (count > 8) return ETD2X_ERR_ARGUMENT;
    void *env = etd2x_jni_current_env();
    main_JavaGlDexJob j = {.source_path=source_path, .class_name=class_name,
        .method=method, .signature=signature, .count=count, .native=native,
        .rc=ETD2X_ERR_APK_UNSUPPORTED, .report=report};
    bool references[8] = {false}, promoted = true;
    j.receiver = receiver ? etd2x_jni_new_global_ref_host(env, receiver) : NULL;
    if (receiver && !j.receiver) promoted = false;
    const char *p = signature + 1;
    for (size_t i = 0; i < count; ++i) {
        j.args[i] = args[i];
        references[i] = *p == 'L' || *p == '[';
        while (*p == '[') ++p;
        if (*p == 'L') { while (*p && *p != ';') ++p; }
        if (*p) ++p;
        if (references[i] && args[i].l) {
            j.args[i].l = etd2x_jni_new_global_ref_host(env, args[i].l);
            if (!j.args[i].l) promoted = false;
        }
    }
    const bool ok = promoted && main_java_gl_call(main_java_gl_dex_job, &j);
    if (j.receiver) etd2x_jni_delete_global_ref_host(env, j.receiver);
    for (size_t i = 0; i < count; ++i)
        if (references[i] && j.args[i].l) etd2x_jni_delete_global_ref_host(env, j.args[i].l);
    return ok ? j.rc : ETD2X_ERR_APK_UNSUPPORTED;
}
static bool main_java_gl_callback(const char *source_path, const char *class_name,
    const char *method, const char *signature, void *receiver,
    const Etd2xJniValue *args, size_t count, const char *checkpoint) {
    Etd2xDexExecReport report = {0};
    Etd2xResult rc = main_java_gl_execute(source_path, class_name, method, signature,
                                      receiver, args, count, false, &report);
    if (rc != ETD2X_OK || !report.completed) {
        ETD2X_LOGT("%s: FAIL rc=%s detail=%s", checkpoint, etd2x_result_string(rc), report.detail);
        return false;
    }
    return true;
}

typedef struct main_JavaGlQueueJob { const char *source_path; void *view; bool ok; } main_JavaGlQueueJob;
static void main_java_gl_queue_job(void *arg) {
    main_JavaGlQueueJob *j = arg;
    j->ok = main_drain_glsurfaceview_events_direct(j->source_path, j->view);
}
static bool main_drain_glsurfaceview_events(const char *source_path, void *view) {
    void *env = etd2x_jni_current_env();
    main_JavaGlQueueJob j = {.source_path=source_path,
        .view=etd2x_jni_new_global_ref_host(env, view)};
    const bool ok = j.view && main_java_gl_call(main_java_gl_queue_job, &j) && j.ok;
    if (j.view) etd2x_jni_delete_global_ref_host(env, j.view);
    return ok;
}

typedef struct main_JavaGlInputJob {
    const char *source_path, *class_name;
    void *renderer;
    const main_JavaRendererInputCaps *caps;
    const Etd2xInputSnapshot *input;
    const Etd2xDisplayPlan *plan;
    main_JavaTouchSet *previous;
    uint64_t frame, *events;
    main_JavaInputPerf *perf;
    bool ok;
} main_JavaGlInputJob;
static void main_java_gl_input_job(void *arg) {
    main_JavaGlInputJob *j = arg;
    j->ok = main_java_renderer_dispatch_input(j->source_path, j->class_name, j->renderer,
        j->caps, j->input, j->plan, j->previous, j->frame, j->events, j->perf);
}
static bool main_java_gl_dispatch_input(const char *source_path, const char *class_name,
    void *renderer, const main_JavaRendererInputCaps *caps, const Etd2xInputSnapshot *input,
    const Etd2xDisplayPlan *plan, main_JavaTouchSet *previous, uint64_t frame,
    uint64_t *events, main_JavaInputPerf *perf) {
    void *env = etd2x_jni_current_env();
    main_JavaGlInputJob j = {.source_path=source_path, .class_name=class_name,
        .renderer=etd2x_jni_new_global_ref_host(env, renderer), .caps=caps,
        .input=input, .plan=plan, .previous=previous, .frame=frame, .events=events, .perf=perf};
    const bool ok = j.renderer && main_java_gl_call(main_java_gl_input_job, &j) && j.ok;
    if (j.renderer) etd2x_jni_delete_global_ref_host(env, j.renderer);
    return ok;
}



/* game launch */
typedef enum main_Etd2xStartupPhase {
    ETD2X_STARTUP_NONE = 0,
    ETD2X_STARTUP_APPLICATION_CTOR,
    ETD2X_STARTUP_APPLICATION_ATTACH,
    ETD2X_STARTUP_PROVIDERS,
    ETD2X_STARTUP_APPLICATION_ONCREATE,
    ETD2X_STARTUP_ACTIVITY_CTOR,
    ETD2X_STARTUP_ACTIVITY_ONCREATE,
    ETD2X_STARTUP_ACTIVITY_START,
    ETD2X_STARTUP_ACTIVITY_RESUME,
    ETD2X_STARTUP_WINDOW_FOCUS,
    ETD2X_STARTUP_SURFACE_DISCOVERY,
    ETD2X_STARTUP_EGL,
    ETD2X_STARTUP_FIRST_FRAME,
    ETD2X_STARTUP_APP_MANAGED_SURFACE,
    ETD2X_RUNTIME_SERVICES,
    ETD2X_RUNTIME_INPUT,
    ETD2X_RUNTIME_QUEUE,
    ETD2X_RUNTIME_DRAW,
    ETD2X_RUNTIME_SWAP,
    ETD2X_RUNTIME_BACKGROUND,
} main_Etd2xStartupPhase;

typedef struct main_Etd2xStartupWatchdog {
    pthread_t thread;
    atomic_bool stop;
    atomic_int phase;
    bool thread_valid;
    uint64_t start_ms;
    uint64_t baseline_present_count;
} main_Etd2xStartupWatchdog;

static const char *main_startup_phase_name(int phase) {
    switch ((main_Etd2xStartupPhase)phase) {
        case ETD2X_STARTUP_APPLICATION_CTOR: return "application-ctor";
        case ETD2X_STARTUP_APPLICATION_ATTACH: return "application-attach";
        case ETD2X_STARTUP_PROVIDERS: return "content-providers";
        case ETD2X_STARTUP_APPLICATION_ONCREATE: return "application-onCreate";
        case ETD2X_STARTUP_ACTIVITY_CTOR: return "activity-ctor";
        case ETD2X_STARTUP_ACTIVITY_ONCREATE: return "activity-onCreate";
        case ETD2X_STARTUP_ACTIVITY_START: return "activity-onStart";
        case ETD2X_STARTUP_ACTIVITY_RESUME: return "activity-onResume";
        case ETD2X_STARTUP_WINDOW_FOCUS: return "window-focus";
        case ETD2X_STARTUP_SURFACE_DISCOVERY: return "surface-discovery";
        case ETD2X_STARTUP_EGL: return "egl-bringup";
        case ETD2X_STARTUP_FIRST_FRAME: return "first-frame";
        case ETD2X_STARTUP_APP_MANAGED_SURFACE: return "app-managed-surface";
        case ETD2X_RUNTIME_SERVICES: return "runtime-services";
        case ETD2X_RUNTIME_INPUT: return "runtime-input";
        case ETD2X_RUNTIME_QUEUE: return "runtime-queue";
        case ETD2X_RUNTIME_DRAW: return "runtime-draw";
        case ETD2X_RUNTIME_SWAP: return "runtime-swap";
        case ETD2X_RUNTIME_BACKGROUND: return "runtime-background";
        default: return "unknown";
    }
}

static const char *main_startup_watchdog_classify(uint64_t instructions,
                                             uint64_t calls,
                                             uint64_t native_calls,
                                             const Etd2xThreadDiagSummary *threads) {
    if (instructions >= UINT64_C(100000)) return "busy-dex";
    if (instructions || calls || native_calls) return "progressing";
    if (threads && threads->waiting) return "waiting";
    if (threads && threads->active) return "native-or-runtime";
    return "idle-or-uninstrumented";
}

static void *main_startup_watchdog_worker(void *opaque) {
    main_Etd2xStartupWatchdog *wd = (main_Etd2xStartupWatchdog *)opaque;
    Etd2xDexRuntimeProgress previous = {0};
    etd2x_dex_get_runtime_progress(&previous);
    uint64_t previous_ms = main_monotonic_ms();
    uint64_t next_report_ms = previous_ms + UINT64_C(5000);
    unsigned report_count = 0;
    int last_phase = ETD2X_STARTUP_NONE;
    char last_classification[32] = "";
    bool first_present = false;
    uint64_t last_present_count = wd->baseline_present_count;
    uint64_t last_present_ms = previous_ms;

    while (!atomic_load_explicit(&wd->stop, memory_order_acquire)) {
        struct timespec nap = {0, 250000000L};
        (void)nanosleep(&nap, NULL);
        if (atomic_load_explicit(&wd->stop, memory_order_acquire)) break;

        const uint64_t presents = etd2x_graphics_present_count();
        const uint64_t now_ms = main_monotonic_ms();
        const int phase = atomic_load_explicit(&wd->phase, memory_order_acquire);
        if (!first_present && presents > wd->baseline_present_count) {
            first_present = true;
            ETD2X_LOGCI("STARTUP", "watchdog first-present observed frames=%" PRIu64
                        " elapsedMs=%llu; runtime monitoring enabled",
                        presents - wd->baseline_present_count,
                        (unsigned long long)(main_monotonic_ms() - wd->start_ms));
            etd2x_diag_flush();
        }
        if (presents != last_present_count || phase == ETD2X_RUNTIME_BACKGROUND) {
            last_present_count = presents;
            last_present_ms = now_ms;
        }
        if (now_ms < next_report_ms) continue;

        Etd2xDexRuntimeProgress current = {0};
        Etd2xThreadDiagSummary threads = {0};
        etd2x_dex_get_runtime_progress(&current);
        etd2x_bionic_pthread_diag_summary(&threads);

        const uint64_t delta_ms = now_ms > previous_ms ? now_ms - previous_ms : 1u;
        const uint64_t delta_instructions = current.instructions - previous.instructions;
        const uint64_t delta_calls = current.calls - previous.calls;
        const uint64_t delta_native = current.native_calls - previous.native_calls;
        const uint64_t ips = delta_instructions * UINT64_C(1000) / delta_ms;
        const uint64_t no_present_ms = now_ms >= last_present_ms ? now_ms - last_present_ms : 0;
        /* First present ends startup warnings, not monitoring of a blocked frame. */
        if (first_present && no_present_ms < UINT64_C(5000)) {
            last_phase = ETD2X_STARTUP_NONE;
            previous = current;
            previous_ms = now_ms;
            next_report_ms = now_ms + UINT64_C(5000);
            continue;
        }
        const char *classification = main_startup_watchdog_classify(
            delta_instructions, delta_calls, delta_native, &threads);
        const bool fresh_location = current.last_update_ms >= wd->start_ms;
        const uint64_t location_age = fresh_location && now_ms >= current.last_update_ms
                                      ? now_ms - current.last_update_ms : 0;

        const char *tag = first_present ? "RUNTIME" : "STARTUP";
        ETD2X_LOGCW(tag,
                    "watchdog elapsedMs=%llu phase=%s classification=%s "
                    "presents=%llu noPresentMs=%llu "
                    "dexInstructionsDelta=%llu dexInstructionsPerSec=%llu callsDelta=%llu nativeCallsDelta=%llu "
                    "threadsLive=%zu threadsWaiting=%zu threadsActive=%zu "
                    "last=%s %s.%s%s pc=%#x opcode=0x%02x lastAgeMs=%llu",
                    (unsigned long long)(now_ms - wd->start_ms), main_startup_phase_name(phase), classification,
                    (unsigned long long)(presents - wd->baseline_present_count),
                    (unsigned long long)no_present_ms,
                    (unsigned long long)delta_instructions, (unsigned long long)ips,
                    (unsigned long long)delta_calls, (unsigned long long)delta_native,
                    threads.live, threads.waiting, threads.active,
                    fresh_location && current.kind[0] ? current.kind : "-",
                    fresh_location && current.class_name[0] ? current.class_name : "-",
                    fresh_location && current.method_name[0] ? current.method_name : "-",
                    fresh_location && current.signature[0] ? current.signature : "",
                    fresh_location ? current.pc : 0u,
                    fresh_location ? current.opcode : 0u,
                    (unsigned long long)location_age);

        const bool state_changed = phase != last_phase || strcmp(classification, last_classification) != 0;
        if (report_count == 0 || state_changed || ((report_count + 1u) % 6u) == 0u) {
            char reason[96];
            snprintf(reason, sizeof(reason), "%s/%s/%s", first_present ? "runtime" : "startup", main_startup_phase_name(phase), classification);
            (void)etd2x_bionic_pthread_log_wait_states(reason);
        }
        etd2x_diag_flush();

        last_phase = phase;
        snprintf(last_classification, sizeof(last_classification), "%s", classification);
        previous = current;
        previous_ms = now_ms;
        next_report_ms = now_ms + UINT64_C(5000);
        report_count++;
    }
    return NULL;
}

static void main_startup_watchdog_set_phase(main_Etd2xStartupWatchdog *wd, main_Etd2xStartupPhase phase) {
    if (!wd || !wd->thread_valid) return;
    atomic_store_explicit(&wd->phase, (int)phase, memory_order_release);
}

static void main_startup_watchdog_start(main_Etd2xStartupWatchdog *wd, main_Etd2xStartupPhase phase) {
    if (!wd || wd->thread_valid) return;
    memset(wd, 0, sizeof(*wd));
    atomic_init(&wd->stop, false);
    atomic_init(&wd->phase, (int)phase);
    wd->start_ms = main_monotonic_ms();
    wd->baseline_present_count = etd2x_graphics_present_count();
    const int rc = pthread_create(&wd->thread, NULL, main_startup_watchdog_worker, wd);
    if (rc != 0) {
        ETD2X_LOGCW("STARTUP", "watchdog unavailable pthread_create rc=%d", rc);
        return;
    }
    wd->thread_valid = true;
    ETD2X_LOGCI("STARTUP", "watchdog enabled intervalMs=5000 threadDumpIntervalMs=30000 baselinePresent=%" PRIu64,
                wd->baseline_present_count);
}

static void main_startup_watchdog_stop(main_Etd2xStartupWatchdog *wd, const char *reason) {
    if (!wd || !wd->thread_valid) return;
    atomic_store_explicit(&wd->stop, true, memory_order_release);
    (void)pthread_join(wd->thread, NULL);
    wd->thread_valid = false;
    ETD2X_LOGCI("STARTUP", "watchdog stopped reason=%s elapsedMs=%llu",
                reason ? reason : "-",
                (unsigned long long)(main_monotonic_ms() - wd->start_ms));
}

static bool main_run_game(const char *apk_path, const Etd2xApkInfo *preprobed_info) {
    uint64_t phase_begin_ms = main_monotonic_ms();
    /* CPU reservations are per launched Android process. App-managed render
     * routes must begin with all application cores available. */
    etd2x_thread_policy_reset();
    ETD2X_LOGI("ETD2X 0.0.15 package launch target=%s", apk_path);

    struct stat st;
    if (stat(apk_path, &st) != 0) {
        ETD2X_LOGT("stage12.apk-present: FAIL path=%s errno=%d", apk_path, errno);
        ETD2X_LOGT("STAGE 14.8.84 GAME LAUNCH: FAIL");
        return false;
    }
    ETD2X_LOGT("stage12.apk-present: PASS bytes=%lld", (long long)st.st_size);

    phase_begin_ms = main_monotonic_ms();
    Etd2xApkInfo info;
    (void)preprobed_info;
    Etd2xResult rc = etd2x_package_probe(apk_path, &info);
    if (rc != ETD2X_OK) {
        ETD2X_LOGT("stage14.package-probe: FAIL (%s) type=%s apks=%zu selected=%zu obb=%zu arm64=%zu javaBootstrap=%u javaLibs=%zu dex=%zu",
                   etd2x_result_string(rc), info.source_xapk ? "xapk" : "apk",
                   info.package_apk_count, info.package_selected_apk_count,
                   info.package_obb_count, info.arm64_lib_count,
                   info.java_activity_bootstrap ? 1u : 0u,
                   info.java_bootstrap_lib_count, info.java_dex_count);
        return false;
    }
    ETD2X_LOGT("stage14.package-probe: PASS type=%s apks=%zu selected=%zu obb=%zu base=%s javaBootstrap=%u javaLibs=%zu dex=%zu",
               info.source_xapk ? "xapk" : "apk", info.package_apk_count,
               info.package_selected_apk_count, info.package_obb_count, info.base_apk_path,
               info.java_activity_bootstrap ? 1u : 0u,
               info.java_bootstrap_lib_count, info.java_dex_count);
    ETD2X_LOGCI("PERF", "stage14 phase=package-probe elapsedMs=%llu",
                (unsigned long long)(main_monotonic_ms() - phase_begin_ms));

    const bool native_route = info.native_activity && info.native_library_present &&
                              info.package_name[0] && info.native_lib_name[0];
    const bool java_route = !info.native_activity && info.package_name[0] && info.activity_name[0] &&
                            info.java_activity_bootstrap && info.java_bootstrap_lib_count > 0;
    ETD2X_LOGCI("JAVA", "launch route=%s package=%s activity=%s nativeLib=%s javaBootstrap=%u javaLibs=%zu dex=%zu",
                native_route ? "native-activity" : (java_route ? "java-native-bootstrap" : "unsupported"),
                info.package_name, info.activity_name, info.native_lib_name,
                info.java_activity_bootstrap ? 1u : 0u,
                info.java_bootstrap_lib_count, info.java_dex_count);
    if (!native_route && !java_route) {
        ETD2X_LOGT("stage14.launch-route: FAIL");
        return false;
    }
    ETD2X_LOGT("stage14.launch-route: PASS route=%s",
               native_route ? "native-activity" : "java-native-bootstrap");

    phase_begin_ms = main_monotonic_ms();
    rc = etd2x_package_prepare_launch_source(apk_path, &info);
    if (rc != ETD2X_OK) {
        ETD2X_LOGT("stage14.launch-source: FAIL (%s)", etd2x_result_string(rc));
        return false;
    }
    ETD2X_LOGT("stage14.launch-source: PASS base=%s",
               info.base_apk_path[0] ? info.base_apk_path : apk_path);
    ETD2X_LOGCI("PERF", "stage14 phase=launch-source elapsedMs=%llu",
                (unsigned long long)(main_monotonic_ms() - phase_begin_ms));

    ETD2X_LOGCI("DISPLAY",
                "launcher orientation contract activity=%s declared=%u value=%d name=%s",
                info.activity_name[0] ? info.activity_name : "<none>",
                info.screen_orientation_declared ? 1u : 0u, info.screen_orientation,
                etd2x_display_orientation_name(info.screen_orientation,
                                               info.screen_orientation_declared));
    Etd2xDisplayPlan display_plan;
    if (!etd2x_display_make_plan(info.screen_orientation, info.screen_orientation_declared,
                                 1280, 720, &display_plan)) {
        ETD2X_LOGT("stage12.orientation-plan: FAIL");
        return false;
    }
    ETD2X_LOGT("stage12.orientation-plan: PASS mode=%s logical=%dx%d physical=%dx%d content=%.1f,%.1f %.1fx%.1f reverse=%u presentationRotation=%d androidRotation=%d",
               display_plan.aspect == ETD2X_DISPLAY_PORTRAIT ? "portrait" : "landscape",
               display_plan.logical_width, display_plan.logical_height,
               display_plan.physical_width, display_plan.physical_height,
               display_plan.content_x, display_plan.content_y,
               display_plan.content_width, display_plan.content_height,
               display_plan.reverse ? 1u : 0u, (int)display_plan.rotation,
               (int)etd2x_display_android_rotation(&display_plan));
    /* Java Surface/ANativeWindow conversion uses the same single display-backed
     * window as NativeActivity. Prepare it before guest DSOs can call the JNI
     * bridge from JNI_OnLoad or later Java bootstrap work. */
    etd2x_native_window_prepare(display_plan.logical_width, display_plan.logical_height, 0);

    char package_component[ETD2X_APK_PACKAGE_CAP];
    char runtime_root[ETD2X_APK_PATH_CAP], lib_dir[ETD2X_APK_PATH_CAP];
    char app_root[ETD2X_APK_PATH_CAP], data_dir[ETD2X_APK_PATH_CAP];
    char external_dir[ETD2X_APK_PATH_CAP], obb_dir[ETD2X_APK_PATH_CAP];
    char split_asset_dir[ETD2X_APK_PATH_CAP];
    main_sanitize_package_component(info.package_name, package_component, sizeof(package_component));
    if (!main_etd2x_path_join(runtime_root, sizeof(runtime_root), NX_RUNTIME_APPS, package_component) ||
        !main_etd2x_path_join(lib_dir, sizeof(lib_dir), runtime_root, "lib") ||
        !main_etd2x_path_join(app_root, sizeof(app_root), NX_SAVES, package_component) ||
        !main_etd2x_path_join(data_dir, sizeof(data_dir), app_root, "data") ||
        !main_etd2x_path_join(external_dir, sizeof(external_dir), app_root, "external") ||
        !main_etd2x_path_join(obb_dir, sizeof(obb_dir), app_root, "obb") ||
        !main_etd2x_path_join(split_asset_dir, sizeof(split_asset_dir), runtime_root, "split-assets")) {
        ETD2X_LOGT("stage13.runtime-paths: FAIL package path too long");
        return false;
    }

    mkdir("sdmc:/switch/earntodie2_nx/cache/runtime", 0777);
    mkdir(NX_RUNTIME_APPS, 0777);
    mkdir(NX_SAVES, 0777);
    mkdir(runtime_root, 0777);
    mkdir(lib_dir, 0777);
    mkdir(app_root, 0777);
    mkdir(data_dir, 0777);
    mkdir(external_dir, 0777);
    mkdir(obb_dir, 0777);
    mkdir(split_asset_dir, 0777);
    phase_begin_ms = main_monotonic_ms();
    size_t extracted = 0, split_assets = 0, obb_files = 0;
    const bool runtime_cache_hit = main_runtime_library_cache_matches(runtime_root, lib_dir, apk_path, &st,
                                                                 info.arm64_lib_count, &split_assets, &obb_files);
    if (runtime_cache_hit) {
        extracted = info.arm64_lib_count;
        ETD2X_LOGCI("PACKAGE", "runtime extraction cache hit package=%s arm64=%zu obb=%zu",
                    info.package_name, extracted, obb_files);
    } else {
        main_clear_library_directory(lib_dir);
        main_clear_directory_tree_contents(split_asset_dir);
        if (info.package_obb_count > 0) main_clear_directory_tree_contents(obb_dir);

        rc = etd2x_package_extract_arm64(apk_path, lib_dir, &extracted);
        if (rc != ETD2X_OK || extracted == 0) {
            ETD2X_LOGT("stage12.arm64-extract: FAIL count=%zu rc=%s", extracted, etd2x_result_string(rc));
            return false;
        }
        rc = etd2x_package_extract_split_assets(apk_path, split_asset_dir, &split_assets);
        if (rc != ETD2X_OK) {
            ETD2X_LOGT("stage14.split-assets: FAIL count=%zu rc=%s", split_assets, etd2x_result_string(rc));
            return false;
        }
        rc = etd2x_package_extract_obb(apk_path, obb_dir, &obb_files);
        if (rc != ETD2X_OK) {
            ETD2X_LOGT("stage14.obb-extract: FAIL count=%zu rc=%s", obb_files, etd2x_result_string(rc));
            return false;
        }
        main_runtime_cache_commit(runtime_root, apk_path, &st, split_assets, obb_files);
        ETD2X_LOGCI("PACKAGE", "runtime extraction cache fill package=%s arm64=%zu splitAssets=%zu obb=%zu",
                    info.package_name, extracted, split_assets, obb_files);
    }
    ETD2X_LOGT("stage12.arm64-extract: PASS count=%zu cache=%s", extracted, runtime_cache_hit ? "hit" : "fill");
    ETD2X_LOGT("stage14.split-assets: PASS count=%zu cache=%s", split_assets, runtime_cache_hit ? "hit" : "fill");
    ETD2X_LOGT("stage14.obb-extract: PASS count=%zu cache=%s", obb_files, runtime_cache_hit ? "hit" : "fill");
    ETD2X_LOGCI("PERF", "stage14 phase=runtime-extract elapsedMs=%llu cache=%s",
                (unsigned long long)(main_monotonic_ms() - phase_begin_ms),
                runtime_cache_hit ? "hit" : "fill");

    phase_begin_ms = main_monotonic_ms();
    rc = etd2x_package_prepare_dex_cache(apk_path);
    if (rc != ETD2X_OK) {
        /* The DEX cache is a performance layer; class/method lookup retains a
           correctness fallback to direct APK scanning. */
        ETD2X_LOGCW("PERF", "DEX cache prepare failed rc=%s; falling back to direct lookup",
                    etd2x_result_string(rc));
    }
    ETD2X_LOGCI("PERF", "stage14 phase=dex-cache elapsedMs=%llu status=%s",
                (unsigned long long)(main_monotonic_ms() - phase_begin_ms),
                rc == ETD2X_OK ? "ready" : "fallback");

    /* One launch runs at a time; keep the 64-root path table off the stack. */
    static char root_paths[ETD2X_APK_JAVA_BOOTSTRAP_MAX][ETD2X_APK_PATH_CAP];
    const char *root_path_ptrs[ETD2X_APK_JAVA_BOOTSTRAP_MAX];
    static char root_names[ETD2X_APK_JAVA_BOOTSTRAP_MAX][ETD2X_APK_LIBNAME_CAP + 7];
    size_t root_count = 0;
    if (native_route) {
        const char *main_name = main_path_basename(info.native_entry_path);
        if (!main_name || !main_name[0] ||
            !main_etd2x_path_join(root_paths[0], sizeof(root_paths[0]), lib_dir, main_name)) {
            ETD2X_LOGT("stage12.main-library-path: FAIL");
            return false;
        }
        const size_t main_name_len = strlen(main_name);
        if (main_name_len >= sizeof(root_names[0])) {
            ETD2X_LOGT("stage12.main-library-name: FAIL name too long");
            return false;
        }
        memcpy(root_names[0], main_name, main_name_len + 1u);
        root_path_ptrs[0] = root_paths[0];
        root_count = 1;
    } else {
        for (size_t i = 0; i < info.java_bootstrap_lib_count && i < ETD2X_APK_JAVA_BOOTSTRAP_MAX; ++i) {
            const char *short_name = etd2x_apk_java_bootstrap_lib(&info, i);
            if (!short_name || !short_name[0]) continue;
            int nw = snprintf(root_names[root_count], sizeof(root_names[root_count]), "lib%s.so", short_name);
            if (nw <= 0 || (size_t)nw >= sizeof(root_names[root_count]) ||
                !main_etd2x_path_join(root_paths[root_count], sizeof(root_paths[root_count]), lib_dir,
                                 root_names[root_count])) {
                ETD2X_LOGT("stage14.java-root-path: FAIL library=%s", short_name);
                return false;
            }
            bool shared = false;
            rc = etd2x_loader_probe_shared_object(root_paths[root_count], &shared);
            if (rc != ETD2X_OK) {
                ETD2X_LOGT("stage14.java-root-kind: FAIL library=%s rc=%s",
                           short_name, etd2x_result_string(rc));
                return false;
            }
            if (!shared) {
                ETD2X_LOGCI("JAVA", "native candidate excluded path=%s reason=ELF-executable",
                            root_paths[root_count]);
                continue;
            }
            root_path_ptrs[root_count] = root_paths[root_count];
            ETD2X_LOGCI("JAVA", "System.loadLibrary candidate[%zu]=%s path=%s",
                        root_count, short_name, root_paths[root_count]);
            root_count++;
        }
        if (root_count == 0) {
            ETD2X_LOGT("stage14.java-root-paths: FAIL no native roots");
            return false;
        }
        ETD2X_LOGT("stage14.java-root-paths: PASS roots=%zu", root_count);
    }

    bool jni_ready = false;
    bool android_ready = false;
    bool linker_ready = false;
    bool graphics_ready = false;
    bool media_ready = false;
    bool audio_worker_ready = false;
    bool java_input_ready = false;
    bool java_applet_hook_ready = false;
    bool applet_exit_locked = false;
    AppletHookCookie java_applet_hook_cookie = {0};
    main_JavaAppletLifecycleState java_applet_lifecycle = {0};
    bool native_ready = false;
    bool lifecycle_ok = false;
    bool saw_present = false;
    bool pass = false;
    Etd2xAndroidGLSurfaceViewInfo java_gl_info = {0};
    bool java_gl_info_ready = false;
    bool java_activity_created = false;
    bool java_activity_shutdown_done = false;
    main_Etd2xStartupWatchdog startup_watchdog = {0};
    char java_application_class[512] = {0};
    char java_activity_class[512] = {0};
    void *java_provider_refs[ETD2X_APK_PROVIDER_MAX] = {0};
    size_t java_provider_ref_count = 0;
    main_Etd2xJavaEglState java_egl = {0};
    Etd2xLinker linker;
    etd2x_linker_init(&linker, lib_dir);
    main_JavaLoadLibraryContext load_library_ctx = { .linker = &linker };

    Etd2xAndroidCompatProfile compat_profile = etd2x_android_compat_profile_make(
        ETD2X_ANDROID_PLATFORM_API_DEFAULT,
        info.min_sdk ? (int32_t)info.min_sdk : ETD2X_ANDROID_MIN_SDK_DEFAULT,
        info.target_sdk ? (int32_t)info.target_sdk : ETD2X_ANDROID_TARGET_SDK_DEFAULT,
        ETD2X_ANDROID_ABI_ARM64_V8A);
    etd2x_android_compat_set_profile(&compat_profile);
    if (java_route) {
        if (info.application_name[0]) {
            if (!main_java_expand_component_class(info.package_name, info.application_name,
                                             java_application_class, sizeof(java_application_class))) {
                ETD2X_LOGT("stage14.java-application-class: FAIL declared=%s", info.application_name);
                goto cleanup;
            }
        } else {
            snprintf(java_application_class, sizeof(java_application_class), "%s",
                     "android/app/Application");
        }
        ETD2X_LOGCI("JAVA", "Application class=%s", java_application_class);
    }
    ETD2X_LOGCI("ANDROID",
                "compat profile platformApi=%d minSdk=%d targetSdk=%d abi=%s minSdkSupported=%u",
                compat_profile.platform_api, compat_profile.min_sdk, compat_profile.target_sdk,
                etd2x_android_compat_abi_name(compat_profile.abi),
                etd2x_android_compat_min_sdk_supported() ? 1u : 0u);

    phase_begin_ms = main_monotonic_ms();
    rc = etd2x_jni_init();
    if (rc != ETD2X_OK) {
        ETD2X_LOGT("stage12.jni-init: FAIL (%s)", etd2x_result_string(rc));
        goto cleanup;
    }
    jni_ready = true;
    const uint64_t jni_init_ms = main_monotonic_ms() - phase_begin_ms;

    /* Java framework construction creates the manifest Activity object. Make
     * package DEX hierarchy metadata available first so a provisional Activity
     * can be reconciled before (or during) object construction. */
    if (java_route) {
        etd2x_jni_set_class_resolver_ex(main_package_jni_class_resolver_ex, (void *)apk_path);
        etd2x_jni_set_class_initializer(main_package_jni_class_initializer, (void *)apk_path);
        etd2x_dex_set_java_method_source(apk_path);
        ETD2X_LOGCI("JAVA", "DEX-backed lazy JNI class/method resolvers enabled source=%s", apk_path);
    }

    main_JavaOrientationBridge orientation_bridge = {
        .plan = &display_plan,
        .graphics_ready = &graphics_ready,
    };

    Etd2xAndroidConfig cfg = {0};
    cfg.package_name = info.package_name;
    cfg.version_name = info.version_name[0] ? info.version_name : "1";
    cfg.version_code = info.version_code;
    cfg.min_sdk = compat_profile.min_sdk;
    cfg.target_sdk = compat_profile.target_sdk;
    cfg.sdk_int = compat_profile.platform_api;
    cfg.release = "11";
    cfg.codename = "REL";
    cfg.manufacturer = "ETD2X";
    cfg.brand = "ETD2X";
    cfg.model = "Nintendo Switch";
    cfg.device = "switch";
    cfg.product = "etd2x";
    cfg.hardware = "nintendo-switch";
    cfg.language = "en";
    cfg.country = "US";
    cfg.touchscreen = ETD2X_ACONFIGURATION_TOUCHSCREEN_FINGER;
    cfg.keyboard = ETD2X_ACONFIGURATION_KEYBOARD_NOKEYS;
    cfg.navigation = ETD2X_ACONFIGURATION_NAVIGATION_DPAD;
    cfg.keys_hidden = ETD2X_ACONFIGURATION_KEYSHIDDEN_YES;
    cfg.nav_hidden = ETD2X_ACONFIGURATION_NAVHIDDEN_NO;
    cfg.ui_mode_type = ETD2X_ACONFIGURATION_UI_MODE_TYPE_NORMAL;
    cfg.ui_mode_night = ETD2X_ACONFIGURATION_UI_MODE_NIGHT_NO;
    cfg.layout_direction = ETD2X_ACONFIGURATION_LAYOUTDIR_LTR;
    cfg.screen_round = ETD2X_ACONFIGURATION_SCREENROUND_NO;
    cfg.width_pixels = display_plan.logical_width;
    cfg.height_pixels = display_plan.logical_height;
    cfg.density_dpi = 160;
    cfg.density = 1.0f;
    cfg.scaled_density = 1.0f;
    cfg.xdpi = 160.0f;
    cfg.ydpi = 160.0f;
    cfg.refresh_rate_hz = 60.0f;
    cfg.display_rotation = etd2x_display_android_rotation(&display_plan);
    cfg.root_path = app_root;
    cfg.native_library_dir = lib_dir;
    cfg.source_path = info.base_apk_path[0] ? info.base_apk_path : apk_path;
    cfg.application_label = info.application_label[0] ? info.application_label : info.package_name;
    cfg.application_class_name = java_route ? java_application_class : NULL;
    cfg.manifest_providers = info.providers;
    cfg.manifest_provider_count = info.provider_count;
    cfg.activity_class_name = java_route ? info.activity_name : NULL;
    cfg.application_theme_resource = info.application_theme_resource;
    cfg.activity_theme_resource = info.activity_theme_resource;
    cfg.activity_screen_orientation = info.screen_orientation;
    cfg.activity_screen_orientation_declared = info.screen_orientation_declared;
    cfg.request_orientation = main_java_apply_requested_orientation;
    cfg.request_orientation_user_data = &orientation_bridge;
    cfg.asset_directory = split_assets ? split_asset_dir : NULL;
    cfg.asset_apk_path = info.base_apk_path[0] ? info.base_apk_path : apk_path;
    cfg.load_library = java_route ? main_java_system_load_library : NULL;
    cfg.load_library_user_data = java_route ? &load_library_ctx : NULL;

    Etd2xMetaData *meta_data = calloc(ETD2X_APK_META_DATA_MAX, sizeof(*meta_data));
    size_t meta_data_count = 0;
    if (meta_data && etd2x_apk_read_application_meta_data(cfg.asset_apk_path, meta_data,
                                                         ETD2X_APK_META_DATA_MAX,
                                                         &meta_data_count) == ETD2X_OK) {
        cfg.application_meta_data = meta_data;
        cfg.application_meta_data_count = meta_data_count;
    }

    etd2x_compat_set_app_storage(info.package_name, app_root);
    if (!etd2x_compat_set_package_apk(cfg.source_path)) {
        ETD2X_LOGT("stage12.package-code-path: FAIL host=%s", cfg.source_path);
        goto cleanup;
    }
    cfg.package_code_path = etd2x_compat_guest_package_apk();
    {
        /* dirname(sourceDir) must stat as a directory (FileStream checks it). */
        char code_dir[1024], host_dir[1024] = {0};
        snprintf(code_dir, sizeof(code_dir), "%s", cfg.package_code_path);
        char *slash = strrchr(code_dir, '/');
        if (slash) *slash = '\0';
        struct stat code_st;
        if (!slash || !etd2x_compat_map_guest_path(code_dir, host_dir, sizeof(host_dir)) ||
            stat(host_dir, &code_st) != 0 || !S_ISDIR(code_st.st_mode)) {
            ETD2X_LOGT("stage12.package-code-path: FAIL codeDir=%s host=%s", code_dir, host_dir);
            goto cleanup;
        }
    }
    ETD2X_LOGT("stage12.package-code-path: PASS guest=%s host=%s",
               cfg.package_code_path, cfg.source_path);
    const uint64_t android_init_begin_ms = main_monotonic_ms();
    ETD2X_LOGCI("PERF", "framework-init pre jniInitMs=%llu metadataAndPathsMs=%llu",
                (unsigned long long)jni_init_ms,
                (unsigned long long)(android_init_begin_ms - phase_begin_ms - jni_init_ms));
    rc = etd2x_android_init(&cfg);
    free(meta_data);
    if (rc != ETD2X_OK) {
        ETD2X_LOGT("stage12.framework-init: FAIL (%s)", etd2x_result_string(rc));
        goto cleanup;
    }
    android_ready = true;
    ETD2X_LOGT("stage12.framework-assets: PASS backends=%zu zip_assets=%zu activity=%s",
               etd2x_assets_backend_count(), etd2x_assets_zip_entry_count(),
               java_route ? info.activity_name : "android.app.NativeActivity");

    etd2x_bionic_cxa_reset();
    etd2x_bionic_tls_activate_current();
    if (!etd2x_bionic_tls_validate_current()) {
        ETD2X_LOGT("stage13.android-tls-stack-guard: FAIL");
        goto cleanup;
    }
    ETD2X_LOGT("stage13.android-tls-stack-guard: PASS tp=%p guard=%#" PRIxPTR,
               etd2x_bionic_tls_current_base(), etd2x_bionic_tls_stack_guard());
    ETD2X_LOGCI("PERF", "stage14 phase=framework-init elapsedMs=%llu",
                (unsigned long long)(main_monotonic_ms() - phase_begin_ms));

    Etd2xModule *root = NULL;
    main_log_memory_snapshot("before-game-link");
    phase_begin_ms = main_monotonic_ms();
    etd2x_dynamic_set_constructor_trace(etd2x_diag_full_trace_enabled());
    rc = java_route
           ? etd2x_linker_prepare_roots(&linker, root_path_ptrs, root_count, &root)
           : etd2x_linker_load_roots(&linker, root_path_ptrs, root_count, &root);
    etd2x_dynamic_set_constructor_trace(false);
    if (rc != ETD2X_OK || !root) {
        ETD2X_LOGT("stage12.game-link: FAIL (%s)", etd2x_result_string(rc));
        goto cleanup;
    }
    linker_ready = true;
    ETD2X_LOGT("stage12.game-link: PASS route=%s roots=%zu modules=%zu compat_binds=%zu needed=%zu",
               native_route ? "native-activity" : "java-native-bootstrap", root_count,
               etd2x_linker_module_count(&linker), etd2x_linker_compat_bind_count(&linker),
               root->dynamic.needed_count);
    ETD2X_LOGCI("PERF", "stage14 phase=game-link elapsedMs=%llu",
                (unsigned long long)(main_monotonic_ms() - phase_begin_ms));

    phase_begin_ms = main_monotonic_ms();
    if (!etd2x_graphics_configure_presentation(&display_plan)) {
        ETD2X_LOGT("stage12.orientation-present: FAIL");
        goto cleanup;
    }
    graphics_ready = true;
    ETD2X_LOGT("stage12.orientation-present: PASS backend=%s logical=%dx%d",
               display_plan.aspect == ETD2X_DISPLAY_PORTRAIT ? "single-context-egl-rotated-fullscreen" : "default-nwindow",
               display_plan.logical_width, display_plan.logical_height);

    if (!etd2x_media_init() || !etd2x_audio_init()) {
        ETD2X_LOGT("stage12.compressed-audio-init: FAIL");
        goto cleanup;
    }
    media_ready = true;
    audio_worker_ready = etd2x_audio_start_worker();
    if (!audio_worker_ready)
        ETD2X_LOGCW("AUDIO", "dedicated worker unavailable; falling back to frame-driven servicing");
    else
        ETD2X_LOGCI("AUDIO", "dedicated worker enabled; audout refill decoupled from render loop");
    ETD2X_LOGT("stage12.compressed-audio-init: PASS output=48000Hz stereo s16");
    ETD2X_LOGCI("PERF", "stage14 phase=graphics-audio elapsedMs=%llu",
                (unsigned long long)(main_monotonic_ms() - phase_begin_ms));

    if (java_route) {
        /* JNI exports remain available to provider/application workers through
         * every lifecycle callback and until guest teardown is quiescent. */
        etd2x_dex_set_native_resolver(main_resolve_dex_jni_export, &linker);
        ETD2X_LOGCI("JAVA", "DEX native resolver installed lifetime=application");
        main_startup_watchdog_start(&startup_watchdog, ETD2X_STARTUP_APPLICATION_CTOR);
        /* Java-route modules are relocated/mapped above, but constructors and
         * JNI_OnLoad stay deferred until the DEX executes System.loadLibrary.
         * This preserves Android load ordering and lets Activity/class state be
         * established before a native library observes it. */
        ETD2X_LOGT("stage14.java-jni-onload: DEFERRED to DEX System.loadLibrary roots=%zu", root_count);
        ETD2X_LOGT("stage14.java-native-bootstrap: PASS roots=%zu activity=%s",
                   root_count, info.activity_name);

        /* Android creates the Application object first, installs manifest
         * ContentProviders, then calls Application.onCreate(), all before the
         * first Activity.  Keep that process-main-thread ordering explicit. */
        void *env = etd2x_jni_current_env();
        if (!env || !etd2x_android_application()) {
            ETD2X_LOGT("stage14.java-application-ctor: FAIL reason=missing-application");
            goto cleanup;
        }
        main_startup_watchdog_set_phase(&startup_watchdog, ETD2X_STARTUP_APPLICATION_CTOR);
        if (strcmp(java_application_class, "android/app/Application") != 0)
            main_java_preflight_startup_root(apk_path, &linker, java_application_class, "<init>", "()V", false,
                                        "application-ctor");
        if (strcmp(java_application_class, "android/app/Application") == 0) {
            Etd2xJniValue ctor_result = {0};
            bool ctor_found = false;
            rc = etd2x_jni_invoke_method_host(env, etd2x_android_application(),
                                               "android/app/Application", "<init>", "()V",
                                               false, NULL, &ctor_result, &ctor_found);
            if (rc != ETD2X_OK || !ctor_found || etd2x_jni_exception_pending_host(env)) {
                ETD2X_LOGT("stage14.java-application-ctor: FAIL class=%s rc=%s found=%d",
                           java_application_class, etd2x_result_string(rc), ctor_found ? 1 : 0);
                main_java_dispatch_uncaught("stage14.java-application-ctor");
                goto cleanup;
            }
        } else {
            Etd2xDexExecReport app_ctor_report = {0};
            Etd2xJniValue ctor_result = {0};
            rc = etd2x_dex_execute_instance_method(apk_path, java_application_class,
                                                   "<init>", "()V", env,
                                                   etd2x_android_application(), NULL, 0,
                                                   &ctor_result, &app_ctor_report);
            if (rc != ETD2X_OK || !app_ctor_report.completed) {
                ETD2X_LOGT("stage14.java-application-ctor: FAIL class=%s detail=%s",
                           java_application_class, app_ctor_report.detail[0] ? app_ctor_report.detail : etd2x_result_string(rc));
                main_java_dispatch_uncaught("stage14.java-application-ctor");
                goto cleanup;
            }
        }
        ETD2X_LOGT("stage14.java-application-ctor: PASS class=%s", java_application_class);

        main_startup_watchdog_set_phase(&startup_watchdog, ETD2X_STARTUP_APPLICATION_ATTACH);
        if (strcmp(java_application_class, "android/app/Application") != 0)
            main_java_preflight_startup_root(apk_path, &linker, java_application_class, "attachBaseContext",
                                        "(Landroid/content/Context;)V", false,
                                        "application-attach");
        Etd2xJniValue attach_args[1] = {{0}};
        attach_args[0].l = etd2x_android_base_context();
        Etd2xJniValue attach_result = {0};
        const bool app_attached_ok = attach_args[0].l &&
            etd2x_android_call_virtual(env, etd2x_android_application(),
                                       "attachBaseContext", "(Landroid/content/Context;)V",
                                       attach_args, 1, &attach_result);
        if (!app_attached_ok) {
            ETD2X_LOGT("stage14.java-application-attach: FAIL class=%s", java_application_class);
            main_java_dispatch_uncaught("stage14.java-application-attach");
            goto cleanup;
        }
        ETD2X_LOGT("stage14.java-application-attach: PASS class=%s", java_application_class);

        main_startup_watchdog_set_phase(&startup_watchdog, ETD2X_STARTUP_PROVIDERS);
        java_provider_ref_count = main_java_install_manifest_providers(
            apk_path, &info, &linker, java_provider_refs, ETD2X_APK_PROVIDER_MAX);
        ETD2X_LOGCI("JAVA", "manifest ContentProvider phase declared=%zu installed=%zu",
                    info.provider_count, java_provider_ref_count);

        main_startup_watchdog_set_phase(&startup_watchdog, ETD2X_STARTUP_APPLICATION_ONCREATE);
        if (strcmp(java_application_class, "android/app/Application") != 0)
            main_java_preflight_startup_root(apk_path, &linker, java_application_class, "onCreate", "()V", false,
                                        "application-onCreate");
        Etd2xJniValue app_result = {0};
        const bool app_created_ok = etd2x_android_call_virtual(env, etd2x_android_application(),
                                                               "onCreate", "()V", NULL, 0,
                                                               &app_result);
        if (!app_created_ok) {
            ETD2X_LOGT("stage14.java-application-onCreate: FAIL class=%s", java_application_class);
            main_java_dispatch_uncaught("stage14.java-application-onCreate");
            goto cleanup;
        }
        ETD2X_LOGT("stage14.java-application-onCreate: PASS class=%s", java_application_class);

        const int activity_name_len = snprintf(java_activity_class, sizeof(java_activity_class), "%s", info.activity_name);
        if (activity_name_len <= 0 || (size_t)activity_name_len >= sizeof(java_activity_class)) {
            ETD2X_LOGT("stage14.java-activity-bytecode: FAIL reason=activity-class-name");
            goto cleanup;
        }
        for (char *p = java_activity_class; *p; ++p) if (*p == '.') *p = '/';
        const char *activity_class = java_activity_class;

        Etd2xDexExecReport dex_report = {0};
        size_t dex_total_instructions = 0, dex_total_calls = 0;
        unsigned activity_transitions = 0;
        phase_begin_ms = main_monotonic_ms();
        for (;;) {
            main_startup_watchdog_set_phase(&startup_watchdog, ETD2X_STARTUP_ACTIVITY_CTOR);
            /* Instrumentation.newActivity() runs the Activity's constructor
             * (field initializers included) before attach/onCreate. */
            memset(&dex_report, 0, sizeof(dex_report));
            Etd2xJniValue ctor_result = {0};
            rc = etd2x_dex_execute_instance_method(apk_path, activity_class, "<init>", "()V",
                                                   etd2x_jni_current_env(), etd2x_android_activity(),
                                                   NULL, 0, &ctor_result, &dex_report);
            if (rc != ETD2X_OK || !dex_report.completed) {
                ETD2X_LOGT("stage14.java-activity-ctor: FAIL class=%s method=%s sig=%s pc=%#x opcode=0x%02x detail=%s",
                           dex_report.class_name[0] ? dex_report.class_name : activity_class,
                           dex_report.method_name[0] ? dex_report.method_name : "<init>",
                           dex_report.signature[0] ? dex_report.signature : "()V",
                           dex_report.pc, dex_report.opcode,
                           dex_report.detail[0] ? dex_report.detail : etd2x_result_string(rc));
                main_java_dispatch_uncaught("stage14.java-activity-ctor");
                goto cleanup;
            }
            dex_total_instructions += dex_report.instructions;
            dex_total_calls += dex_report.calls;

            Etd2xJniValue activity_attach_args[1] = {{0}};
            activity_attach_args[0].l = etd2x_android_base_context();
            Etd2xJniValue activity_attach_result = {0};
            const bool activity_attached_ok = activity_attach_args[0].l &&
                etd2x_android_call_virtual(etd2x_jni_current_env(), etd2x_android_activity(),
                                           "attachBaseContext", "(Landroid/content/Context;)V",
                                           activity_attach_args, 1, &activity_attach_result);
            if (!activity_attached_ok) {
                ETD2X_LOGT("stage14.java-activity-attach: FAIL class=%s", activity_class);
                main_java_dispatch_uncaught("stage14.java-activity-attach");
                goto cleanup;
            }

            main_startup_watchdog_set_phase(&startup_watchdog, ETD2X_STARTUP_ACTIVITY_ONCREATE);
            memset(&dex_report, 0, sizeof(dex_report));
            /* Activity.performCreate: onActivityPreCreated, onCreate, onActivityPostCreated. */
            (void)etd2x_android_dispatch_activity_lifecycle_event(etd2x_jni_current_env(), "PreCreated");
            rc = etd2x_dex_execute_activity_on_create(apk_path, activity_class,
                                                       etd2x_jni_current_env(), etd2x_android_activity(),
                                                       &dex_report);
            if (rc == ETD2X_OK && dex_report.completed)
                (void)etd2x_android_dispatch_activity_lifecycle_event(etd2x_jni_current_env(), "PostCreated");
            dex_total_instructions += dex_report.instructions;
            dex_total_calls += dex_report.calls;
            if (rc != ETD2X_OK || !dex_report.completed) {
                ETD2X_LOGT("stage14.java-activity-bytecode: FAIL class=%s method=%s sig=%s pc=%#x opcode=0x%02x detail=%s instructions=%zu calls=%zu",
                           dex_report.class_name[0] ? dex_report.class_name : activity_class,
                           dex_report.method_name[0] ? dex_report.method_name : "onCreate",
                           dex_report.signature[0] ? dex_report.signature : "(Landroid/os/Bundle;)V",
                           dex_report.pc, dex_report.opcode,
                           dex_report.detail[0] ? dex_report.detail : etd2x_result_string(rc),
                           dex_total_instructions, dex_total_calls);
                main_java_dispatch_uncaught("stage14.java-activity-bytecode");
                ETD2X_LOGCI("PERF", "stage14 phase=dex-onCreate elapsedMs=%llu",
                            (unsigned long long)(main_monotonic_ms() - phase_begin_ms));
                goto cleanup;
            }
            if (!main_java_require_activity_state(ETD2X_ANDROID_ACTIVITY_CREATED, "onCreate"))
                goto cleanup;
            java_activity_created = true;
            if (etd2x_android_has_pending_activity())
                goto started_activity;
            ETD2X_LOGT("stage14.java-activity-bytecode: PASS activity=%s transitions=%u instructions=%zu calls=%zu",
                       activity_class, activity_transitions, dex_total_instructions, dex_total_calls);
            java_activity_created = true;
            ETD2X_LOGCI("PERF", "stage14 phase=dex-onCreate elapsedMs=%llu",
                        (unsigned long long)(main_monotonic_ms() - phase_begin_ms));
            ETD2X_LOGCI("JAVA", "Activity onCreate DEX execution completed activity=%s", activity_class);

            /* Continue the real Java Activity lifecycle after onCreate. Native
             * resolution remains installed for queued renderer and worker calls. */
            main_log_cocos_writable_path(apk_path);
            /* Diagnostic-only lifecycle inventory must never sit on the normal
             * Android launch critical path.  Recursive preflight can walk hundreds
             * of package methods (especially teardown/camera paths) before the real
             * Activity is allowed to run onStart()/onResume().  Keep it available
             * under full-trace.flag, but FAST mode executes the guest lifecycle
             * immediately and lets runtime resolution expose the first real
             * divergence. */
            if (etd2x_diag_full_trace_enabled()) {
                static const struct { const char *name, *sig; } lifecycle_preflight[] = {
                    {"onStart", "()V"}, {"onResume", "()V"}, {"onWindowFocusChanged", "(Z)V"},
                    {"onPause", "()V"}, {"onStop", "()V"}, {"onDestroy", "()V"},
                };
                for (size_t i = 0; i < sizeof(lifecycle_preflight) / sizeof(lifecycle_preflight[0]); ++i) {
                    Etd2xDexPreflightReport lifecycle_report = {0};
                    (void)etd2x_dex_preflight_method_tree(apk_path, activity_class, lifecycle_preflight[i].name,
                                                          lifecycle_preflight[i].sig, false, &lifecycle_report);
                }
            } else {
                ETD2X_LOGCI("DEX-PREFLIGHT", "lifecycle batch skipped in FAST mode; runtime executes callbacks directly");
            }
            phase_begin_ms = main_monotonic_ms();
            main_startup_watchdog_set_phase(&startup_watchdog, ETD2X_STARTUP_ACTIVITY_START);
            if (!main_activity_lifecycle_callback(apk_path, activity_class, "onStart",
                                       etd2x_android_activity(),
                                       "stage14.java-onStart"))
                goto cleanup;
            if (!main_java_require_activity_state(ETD2X_ANDROID_ACTIVITY_STARTED, "onStart"))
                goto cleanup;
            main_startup_watchdog_set_phase(&startup_watchdog, ETD2X_STARTUP_ACTIVITY_RESUME);
            if (!main_activity_lifecycle_callback(apk_path, activity_class, "onResume",
                                       etd2x_android_activity(),
                                       "stage14.java-onResume"))
                goto cleanup;
            if (!main_java_require_activity_state(ETD2X_ANDROID_ACTIVITY_RESUMED, "onResume"))
                goto cleanup;
            main_startup_watchdog_set_phase(&startup_watchdog, ETD2X_STARTUP_WINDOW_FOCUS);
            if (!main_java_activity_deliver_window_focus(apk_path, activity_class,
                                                    etd2x_android_activity(), true,
                                                    "stage14.java-window-focus"))
                goto cleanup;
            ETD2X_LOGCI("PERF", "stage14 phase=java-resume elapsedMs=%llu",
                        (unsigned long long)(main_monotonic_ms() - phase_begin_ms));

            /* startActivity is asynchronous and can be requested by onStart,
             * onResume, or the first main-Looper/focus callbacks too. Consume
             * those requests before discovering the foreground render surface. */
            if (!etd2x_android_has_pending_activity())
                break;
started_activity:
            if (!main_java_commit_activity_transition(apk_path, java_activity_class, sizeof(java_activity_class))) goto cleanup;
            pass = false;
            java_activity_created = false;
            java_activity_shutdown_done = false;
            activity_transitions++;
            if (activity_transitions > 8u) {
                ETD2X_LOGT("stage14.java-activity-bytecode: FAIL reason=activity-transition-loop count=%u",
                           activity_transitions);
                goto cleanup;
            }
            ETD2X_LOGCI("JAVA", "Activity transition executing onCreate class=%s index=%u",
                        activity_class, activity_transitions);
        }

        main_startup_watchdog_set_phase(&startup_watchdog, ETD2X_STARTUP_SURFACE_DISCOVERY);
        etd2x_android_log_surface_state(etd2x_jni_current_env(), "initial-discovery");
        rc = etd2x_android_find_glsurfaceview(etd2x_jni_current_env(), &java_gl_info);
        if (rc == ETD2X_ERR_APK_UNSUPPORTED ||
            (rc == ETD2X_OK && (!java_gl_info.view_ref || !java_gl_info.renderer_ref || !java_gl_info.resumed))) {
            if (java_gl_info.view_ref) etd2x_jni_delete_local_ref_host(etd2x_jni_current_env(), java_gl_info.view_ref);
            if (java_gl_info.renderer_ref) etd2x_jni_delete_local_ref_host(etd2x_jni_current_env(), java_gl_info.renderer_ref);
            memset(&java_gl_info, 0, sizeof(java_gl_info));
            ETD2X_LOGCI("JAVA", "surface discovery pending; Activity stays active surfaceAttached=%d",
                        etd2x_android_has_attached_surface() ? 1 : 0);
            const main_JavaActivityLoopResult loop = main_java_activity_event_loop(apk_path, &display_plan, &java_gl_info);
            if (loop == JAVA_ACTIVITY_TRANSITION) { activity_transitions = 0; goto started_activity; }
            if (loop != JAVA_ACTIVITY_GL_READY) {
                pass = loop == JAVA_ACTIVITY_EXIT;
                goto cleanup;
            }
            rc = ETD2X_OK;
        }
        if (rc != ETD2X_OK) goto cleanup;
        java_gl_info_ready = true;
        char view_class[512] = {0};
        char renderer_class[512] = {0};
        if (!etd2x_jni_object_class_name_host(java_gl_info.view_ref, view_class, sizeof(view_class)) ||
            !etd2x_jni_object_class_name_host(java_gl_info.renderer_ref, renderer_class, sizeof(renderer_class))) {
            ETD2X_LOGT("stage14.java-glsurface-discovery: FAIL reason=class-name");
            goto cleanup;
        }
        ETD2X_LOGT("stage14.java-glsurface-discovery: PASS view=%s renderer=%s resumed=%u client=%d renderMode=%d queued=%zu",
                   view_class, renderer_class, java_gl_info.resumed ? 1u : 0u,
                   java_gl_info.egl_client_version, java_gl_info.render_mode,
                   java_gl_info.queued_event_count);
        if (!java_gl_info.resumed) {
            ETD2X_LOGT("stage14.java-glsurface-resume: FAIL reason=GLSurfaceView.onResume-not-observed");
            goto cleanup;
        }

        phase_begin_ms = main_monotonic_ms();
        main_startup_watchdog_set_phase(&startup_watchdog, ETD2X_STARTUP_EGL);
        if (!main_java_gl_thread_start(apk_path) ||
            !main_java_gl_egl_start(&java_gl_info, display_plan.logical_width,
                            display_plan.logical_height, &java_egl)) {
            ETD2X_LOGT("stage14.java-glsurface-egl: FAIL");
            goto cleanup;
        }
        ETD2X_LOGT("stage14.java-glsurface-egl: PASS size=%dx%d",
                   java_egl.width, java_egl.height);

        Etd2xJniValue size_args[4] = {{0}};
        size_args[0].i = display_plan.logical_width;
        size_args[1].i = display_plan.logical_height;
        size_args[2].i = 0;
        size_args[3].i = 0;
        if (!main_dex_instance_callback(apk_path, view_class, "onSizeChanged", "(IIII)V",
                                   java_gl_info.view_ref, size_args, 4,
                                   "stage14.java-gl-size"))
            goto cleanup;

        void *gl10 = etd2x_jni_new_local_object(etd2x_jni_current_env(),
                                                 "javax/microedition/khronos/opengles/GL10");
        void *egl_config = etd2x_jni_new_local_object(etd2x_jni_current_env(),
                                                       "javax/microedition/khronos/egl/EGLConfig");
        if (!gl10 || !egl_config) {
            if (gl10) etd2x_jni_delete_local_ref_host(etd2x_jni_current_env(), gl10);
            if (egl_config) etd2x_jni_delete_local_ref_host(etd2x_jni_current_env(), egl_config);
            ETD2X_LOGT("stage14.java-gl-callback-objects: FAIL");
            goto cleanup;
        }
        Etd2xJniValue created_args[2] = {{0}};
        created_args[0].l = gl10;
        created_args[1].l = egl_config;
        if (!main_java_gl_callback(apk_path, renderer_class, "onSurfaceCreated",
                                   "(Ljavax/microedition/khronos/opengles/GL10;Ljavax/microedition/khronos/egl/EGLConfig;)V",
                                   java_gl_info.renderer_ref, created_args, 2,
                                   "stage14.java-gl-onSurfaceCreated")) {
            etd2x_jni_delete_local_ref_host(etd2x_jni_current_env(), egl_config);
            etd2x_jni_delete_local_ref_host(etd2x_jni_current_env(), gl10);
            goto cleanup;
        }
        ETD2X_LOGT("stage14.java-native-init: PASS renderer=%s size=%dx%d",
                   renderer_class, display_plan.logical_width, display_plan.logical_height);

        Etd2xJniValue changed_args[3] = {{0}};
        changed_args[0].l = gl10;
        changed_args[1].i = display_plan.logical_width;
        changed_args[2].i = display_plan.logical_height;
        if (!main_java_gl_callback(apk_path, renderer_class, "onSurfaceChanged",
                                   "(Ljavax/microedition/khronos/opengles/GL10;II)V",
                                   java_gl_info.renderer_ref, changed_args, 3,
                                   "stage14.java-gl-onSurfaceChanged")) {
            etd2x_jni_delete_local_ref_host(etd2x_jni_current_env(), egl_config);
            etd2x_jni_delete_local_ref_host(etd2x_jni_current_env(), gl10);
            goto cleanup;
        }
        etd2x_jni_delete_local_ref_host(etd2x_jni_current_env(), egl_config);

        /* queueEvent() work belongs to the GL thread.  On first attachment,
         * establish the surface and complete renderer nativeInit() before
         * delivering resume work, so queued nativeOnResume callbacks cannot
         * outrun creation of the native Cocos application. */
        if (!main_drain_glsurfaceview_events(apk_path, java_gl_info.view_ref)) {
            etd2x_jni_delete_local_ref_host(etd2x_jni_current_env(), gl10);
            ETD2X_LOGT("stage14.java-gl-queue-drain: FAIL");
            goto cleanup;
        }
        if (!main_drain_activity_ui_events(apk_path)) {
            etd2x_jni_delete_local_ref_host(etd2x_jni_current_env(), gl10);
            ETD2X_LOGT("stage14.java-ui-queue-drain: FAIL");
            goto cleanup;
        }
        ETD2X_LOGT("stage14.java-gl-queue-drain: PASS");
        ETD2X_LOGT("stage14.java-ui-queue-drain: PASS");

        ETD2X_LOGCI("PERF", "stage14 phase=java-gl-native-init elapsedMs=%llu",
                    (unsigned long long)(main_monotonic_ms() - phase_begin_ms));

        /* Stage 14.8.43 gameplay-loop bring-up. First retain the proven single
         * frame checkpoint, then keep the same EGL context, GL10 identity and
         * Java renderer alive for a real GLSurfaceView-style render loop. */
        phase_begin_ms = main_monotonic_ms();
        main_startup_watchdog_set_phase(&startup_watchdog, ETD2X_STARTUP_FIRST_FRAME);
        Etd2xJniValue draw_args[1] = {{0}};
        draw_args[0].l = gl10;
        Etd2xDexExecReport first_draw_report = {0};
        rc = main_java_gl_execute(
            apk_path, renderer_class, "onDrawFrame",
            "(Ljavax/microedition/khronos/opengles/GL10;)V",
            java_gl_info.renderer_ref,
            draw_args, 1, false, &first_draw_report);
        if (rc != ETD2X_OK || !first_draw_report.completed) {
            etd2x_jni_delete_local_ref_host(etd2x_jni_current_env(), gl10);
            ETD2X_LOGT("stage14.java-gl-onDrawFrame: FAIL class=%s method=%s pc=%#x opcode=0x%02x detail=%s instructions=%zu calls=%zu",
                       first_draw_report.class_name[0] ? first_draw_report.class_name : renderer_class,
                       first_draw_report.method_name[0] ? first_draw_report.method_name : "onDrawFrame",
                       first_draw_report.pc, first_draw_report.opcode,
                       first_draw_report.detail[0] ? first_draw_report.detail : etd2x_result_string(rc),
                       first_draw_report.instructions, first_draw_report.calls);
            main_java_dispatch_uncaught("stage14.java-gl-onDrawFrame");
            goto cleanup;
        }
        ETD2X_LOGT("stage14.java-gl-onDrawFrame: PASS class=%s method=onDrawFrame instructions=%zu calls=%zu",
                   renderer_class, first_draw_report.instructions, first_draw_report.calls);

        bool direct_render_enabled = false;
        if (strcmp(renderer_class, "org/cocos2dx/lib/Cocos2dxRenderer") == 0 &&
            first_draw_report.instructions == 2u && first_draw_report.calls == 1u) {
            bool native_render_found = false;
            Etd2xResult prc = etd2x_dex_prewarm_native_method(
                renderer_class, "nativeRender", "()V", &native_render_found);
            direct_render_enabled = prc == ETD2X_OK && native_render_found;
            ETD2X_LOGCI("JAVA-GL",
                        "Cocos direct render probe wrapperInstructions=%zu wrapperCalls=%zu nativeRender=%d rc=%s enabled=%d",
                        first_draw_report.instructions, first_draw_report.calls,
                        native_render_found ? 1 : 0, etd2x_result_string(prc),
                        direct_render_enabled ? 1 : 0);
        } else {
            ETD2X_LOGCI("JAVA-GL",
                        "Cocos direct render probe class=%s wrapperInstructions=%zu wrapperCalls=%zu enabled=0",
                        renderer_class, first_draw_report.instructions, first_draw_report.calls);
        }
        ETD2X_LOGT("stage14.java-native-render: PASS renderer=%s direct=%d",
                   renderer_class, direct_render_enabled ? 1 : 0);

        int32_t egl_error = 0;
        if (!main_java_gl_swap(&java_egl, &egl_error)) {
            etd2x_jni_delete_local_ref_host(etd2x_jni_current_env(), gl10);
            ETD2X_LOGT("stage14.java-first-swap: FAIL error=0x%04x",
                       (unsigned)egl_error);
            goto cleanup;
        }
        ETD2X_LOGT("stage14.java-first-swap: PASS size=%dx%d",
                   java_egl.width, java_egl.height);
        ETD2X_LOGCI("PERF", "stage14 phase=java-first-frame elapsedMs=%llu",
                    (unsigned long long)(main_monotonic_ms() - phase_begin_ms));
        ETD2X_LOGT("stage14.java-first-frame: PASS checkpoint=onDrawFrame-nativeRender-eglSwapBuffers");

        if (!etd2x_input_init()) {
            etd2x_jni_delete_local_ref_host(etd2x_jni_current_env(), gl10);
            ETD2X_LOGT("stage14.java-game-loop-input: FAIL");
            goto cleanup;
        }
        java_input_ready = true;
        const main_JavaRendererInputCaps java_input_caps =
            main_probe_java_renderer_input(apk_path, renderer_class);
        const bool java_renderer_input_enabled =
            java_input_caps.touch_down && java_input_caps.touch_up;
        main_prewarm_java_renderer_input(apk_path, renderer_class, &java_input_caps);
        ETD2X_LOGT("stage14.java-renderer-input: %s class=%s touch=%s move=%s key=%s",
                   java_renderer_input_enabled ? "PASS" : "SKIP",
                   renderer_class,
                   java_renderer_input_enabled ? "ready" : "unsupported",
                   java_input_caps.touch_move ? "yes" : "no",
                   java_input_caps.key_down ? "yes" : "no");

        uint64_t frame_count = 1;
        uint64_t last_reported = 0;
        uint64_t input_event_count = 0;
        bool exit_requested = false;
        bool frame_loop_ok = true;
        bool activity_transition_requested = false;
        main_JavaTouchSet java_touch_previous = {0};
        main_JavaInputPerf java_input_perf = {0};
        uint64_t interval_frame_max_us = 0;
        uint64_t interval_queue_max_us = 0;
        uint64_t interval_draw_max_us = 0;
        uint64_t interval_swap_max_us = 0;
        uint64_t interval_frame_total_us = 0;
        uint64_t interval_draw_total_us = 0;
        uint64_t interval_measured_frames = 0;
        uint64_t interval_over_16667 = 0;
        uint64_t interval_over_18000 = 0;
        uint64_t interval_over_20000 = 0;
        uint64_t direct_render_calls = 0;
        uint64_t render_dex_fallbacks = 0;
        uint64_t slow_frame_logs = 0;
        bool activity_backgrounded = false;
        const uint64_t loop_begin_ms = main_monotonic_ms();
        uint64_t last_report_ms = loop_begin_ms;
        bool checkpoint_taken = false;

        ETD2X_LOGCI("PERF", "Activity loop services UI; renderer runs on dedicated GL thread");
        Result focus_mode_rc = appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleepNotify);
        ETD2X_LOGCI("JAVA", "applet focus mode SuspendHomeSleepNotify rc=0x%08x", focus_mode_rc);
        appletHook(&java_applet_hook_cookie, main_java_applet_hook, &java_applet_lifecycle);
        java_applet_hook_ready = true;
        Result lock_exit_rc = appletLockExit();
        if (R_SUCCEEDED(lock_exit_rc)) applet_exit_locked = true;
        ETD2X_LOGCI("JAVA", "applet exit lock rc=0x%08x enabled=%d",
                    lock_exit_rc, applet_exit_locked ? 1 : 0);

        ETD2X_LOGI("ETD2X 0.0.15 Java GLSurfaceView gameplay loop active: isolated render core + guarded direct Cocos nativeRender + direct native input; touch + A=center tap + B=BACK; PLUS exits to hbmenu");
        while (!exit_requested) {
            main_startup_watchdog_set_phase(&startup_watchdog,
                activity_backgrounded ? ETD2X_RUNTIME_BACKGROUND : ETD2X_RUNTIME_SERVICES);
            const bool applet_alive = appletMainLoop();
            if (!applet_alive) {
                main_java_loop_diagnostics(main_monotonic_ms() - loop_begin_ms, 0, true, &checkpoint_taken);
                ETD2X_LOGCI("JAVA", "appletMainLoop exit requested hookExit=%d backgrounded=%d",
                            java_applet_lifecycle.exit_requested ? 1 : 0,
                            activity_backgrounded ? 1 : 0);
                break;
            }
            char process_exit_reason[256];
            if (etd2x_jni_process_exit_requested(process_exit_reason, sizeof(process_exit_reason))) {
                ETD2X_LOGT("stage14.java-process-exit: FAIL reason=%s", process_exit_reason);
                break;
            }
            if (java_applet_lifecycle.focus_pending) {
                const AppletFocusState focus_state = java_applet_lifecycle.focus_state;
                java_applet_lifecycle.focus_pending = false;
                ETD2X_LOGCI("JAVA", "applet focus transition state=%d events=%llu backgrounded=%d",
                            (int)focus_state,
                            (unsigned long long)java_applet_lifecycle.focus_events,
                            activity_backgrounded ? 1 : 0);
                if (focus_state == AppletFocusState_Background && !activity_backgrounded) {
                    main_startup_watchdog_set_phase(&startup_watchdog, ETD2X_RUNTIME_BACKGROUND);
                    if (!main_java_activity_enter_background(apk_path, activity_class,
                                                        etd2x_android_activity(),
                                                        java_gl_info.view_ref,
                                                        "switch-background")) {
                        frame_loop_ok = false;
                    }
                    activity_backgrounded = true;
                    continue;
                }
                if (focus_state == AppletFocusState_InFocus && activity_backgrounded) {
                    if (!main_java_activity_resume_from_background(apk_path, activity_class,
                                                              etd2x_android_activity(),
                                                              java_gl_info.view_ref)) {
                        frame_loop_ok = false;
                        break;
                    }
                    activity_backgrounded = false;
                } else if (focus_state == AppletFocusState_OutOfFocus) {
                    /* Library applets (notably the software keyboard) are not
                     * equivalent to Android Activity backgrounding. Keep the
                     * game lifecycle running and only record the transition. */
                    ETD2X_LOGCI("JAVA", "library-applet focus loss observed; Activity lifecycle retained");
                }
            }
            if (java_applet_lifecycle.exit_requested) {
                ETD2X_LOGCI("JAVA", "applet exit hook observed inside game loop");
                break;
            }

            /* Android IME edits arrive asynchronously after the keyboard UI
             * returns. The Switch software keyboard is a modal library applet,
             * so finish a pending Cocos edit only on this later GL-thread frame,
             * after appletMainLoop has processed focus-return notifications. */
            bool keyboard_commit_completed = false;
            Etd2xResult keyboard_commit_rc =
                etd2x_dex_pump_cocos_keyboard_commit(&keyboard_commit_completed);
            if (keyboard_commit_rc != ETD2X_OK) {
                ETD2X_LOGCW("KEYBOARD", "deferred Cocos keyboard commit rc=%s",
                            etd2x_result_string(keyboard_commit_rc));
            }

            /* AChoreographer callbacks are one-shot and execute on the thread
             * that owns the Choreographer, before that thread's next frame. */
            if (!etd2x_android_do_frame()) { frame_loop_ok = false; break; }
            etd2x_choreographer_pump();

            const uint64_t frame_begin_ms = main_monotonic_ms();
            main_startup_watchdog_set_phase(&startup_watchdog, ETD2X_RUNTIME_INPUT);
            const uint64_t frame_begin_us = main_fast_tick_us();
            const uint64_t down_calls_before = java_input_perf.down_calls;
            Etd2xInputSnapshot input = {0};
            etd2x_input_update(&input);
            etd2x_android_update_input_snapshot(&input);
            main_java_loop_diagnostics(main_monotonic_ms() - loop_begin_ms, input.buttons_down,
                                  (input.buttons_down & ETD2X_BTN_PLUS) != 0, &checkpoint_taken);
            if ((input.buttons_down & ETD2X_BTN_PLUS) != 0) {
                exit_requested = true;
                break;
            }

            if (java_renderer_input_enabled &&
                !main_java_gl_dispatch_input(apk_path, renderer_class,
                                              java_gl_info.renderer_ref,
                                              &java_input_caps, &input,
                                              &display_plan, &java_touch_previous,
                                              frame_count + 1u,
                                              &input_event_count, &java_input_perf)) {
                ETD2X_LOGT("stage14.java-frame-loop: FAIL reason=input frame=%" PRIu64,
                           frame_count + 1u);
                frame_loop_ok = false;
                break;
            }
            /* Without the renderer fast path, deliver touches the way Android
             * does: Activity.dispatchTouchEvent through the view tree to the
             * GLSurfaceView's onTouchEvent, which typically queueEvent()s the
             * work for the GL thread drained just below. */
            if (!java_renderer_input_enabled)
                main_java_app_managed_dispatch_touches(&input, &display_plan);

            /* Android GLSurfaceView executes queueEvent work on the GL thread
             * before rendering. Drain every frame so future Java-side input,
             * lifecycle and engine work does not starve behind the renderer. */
            const uint64_t queue_begin_us = main_fast_tick_us();
            main_startup_watchdog_set_phase(&startup_watchdog, ETD2X_RUNTIME_QUEUE);
            if (!main_drain_glsurfaceview_events(apk_path, java_gl_info.view_ref) ||
                !main_drain_activity_ui_events_budget(apk_path, UINT64_C(4000))) {
                ETD2X_LOGT("stage14.java-frame-loop: FAIL reason=queue-event frame=%" PRIu64,
                           frame_count + 1u);
                frame_loop_ok = false;
                break;
            }
            const uint64_t queue_us = main_fast_tick_us() - queue_begin_us;
            if (etd2x_android_has_pending_activity()) { activity_transition_requested = true; break; }
            if (etd2x_android_activity_finish_requested()) { exit_requested = true; break; }
            if (queue_us > interval_queue_max_us) interval_queue_max_us = queue_us;

            Etd2xDexExecReport frame_report = {0};
            const uint64_t draw_begin_us = main_fast_tick_us();
            main_startup_watchdog_set_phase(&startup_watchdog, ETD2X_RUNTIME_DRAW);
            bool frame_rendered = false;
            Etd2xResult frame_rc = ETD2X_ERR_APK_UNSUPPORTED;
            if (direct_render_enabled) {
                frame_rc = main_java_gl_execute(
                    apk_path, renderer_class, "nativeRender", "()V",
                    NULL, NULL, 0, true, NULL);
                if (frame_rc == ETD2X_OK) {
                    frame_rendered = true;
                    direct_render_calls++;
                } else {
                    ETD2X_LOGCW("JAVA-GL",
                                "direct nativeRender failed frame=%" PRIu64 " rc=%s; disabling fast path",
                                frame_count + 1u, etd2x_result_string(frame_rc));
                    direct_render_enabled = false;
                }
            }
            if (!frame_rendered) {
                frame_rc = main_java_gl_execute(
                    apk_path, renderer_class, "onDrawFrame",
                    "(Ljavax/microedition/khronos/opengles/GL10;)V",
                    java_gl_info.renderer_ref,
                    draw_args, 1, false, &frame_report);
                render_dex_fallbacks++;
                if (frame_rc != ETD2X_OK || !frame_report.completed) {
                    ETD2X_LOGT("stage14.java-frame-loop: FAIL reason=onDrawFrame frame=%" PRIu64
                               " rc=%s pc=%#x opcode=0x%02x detail=%s",
                               frame_count + 1u, etd2x_result_string(frame_rc),
                               frame_report.pc, frame_report.opcode, frame_report.detail);
                    main_java_dispatch_uncaught("stage14.java-frame-loop");
                    frame_loop_ok = false;
                    break;
                }
            }
            const uint64_t draw_us = main_fast_tick_us() - draw_begin_us;
            if (draw_us > interval_draw_max_us) interval_draw_max_us = draw_us;

            const uint64_t swap_begin_us = main_fast_tick_us();
            main_startup_watchdog_set_phase(&startup_watchdog, ETD2X_RUNTIME_SWAP);
            if (!main_java_gl_swap(&java_egl, &egl_error)) {
                ETD2X_LOGT("stage14.java-frame-loop: FAIL reason=swap frame=%" PRIu64
                           " error=0x%04x", frame_count + 1u, (unsigned)egl_error);
                frame_loop_ok = false;
                break;
            }
            const uint64_t swap_us = main_fast_tick_us() - swap_begin_us;
            if (swap_us > interval_swap_max_us) interval_swap_max_us = swap_us;
            frame_count++;
            if (!audio_worker_ready && !etd2x_audio_update())
                ETD2X_LOGCE("AUDIO", "Java game-loop fallback audio update failed");
            const uint64_t frame_us = main_fast_tick_us() - frame_begin_us;
            if (frame_us > interval_frame_max_us) interval_frame_max_us = frame_us;
            interval_frame_total_us += frame_us;
            interval_draw_total_us += draw_us;
            interval_measured_frames++;
            if (frame_us > UINT64_C(16667)) interval_over_16667++;
            if (frame_us > UINT64_C(18000)) interval_over_18000++;
            if (frame_us > UINT64_C(20000)) {
                interval_over_20000++;
                slow_frame_logs++;
                if (slow_frame_logs <= 12u || (slow_frame_logs % 30u) == 0u) {
                    const uint64_t known_us = queue_us + draw_us + swap_us;
                    const uint64_t other_us = frame_us > known_us ? frame_us - known_us : 0;
                    ETD2X_LOGCI("PERF",
                                "slow frame=%" PRIu64 " totalUs=%llu queueUs=%llu drawUs=%llu swapUs=%llu otherUs=%llu directRender=%d slowCount=%llu",
                                frame_count,
                                (unsigned long long)frame_us,
                                (unsigned long long)queue_us,
                                (unsigned long long)draw_us,
                                (unsigned long long)swap_us,
                                (unsigned long long)other_us,
                                direct_render_enabled ? 1 : 0,
                                (unsigned long long)slow_frame_logs);
                }
            }
            if (java_input_perf.down_calls != down_calls_before) {
                const uint64_t down_frame_ms = main_monotonic_ms() - frame_begin_ms;
                if (down_frame_ms > java_input_perf.down_frame_max_ms)
                    java_input_perf.down_frame_max_ms = down_frame_ms;
            }

            if (frame_count >= last_reported + 300u) {
                const uint64_t now_ms = main_monotonic_ms();
                const uint64_t interval_frames = frame_count - last_reported;
                const uint64_t interval_ms = now_ms - last_report_ms;
                const uint64_t fps_x100 = interval_ms ?
                    (interval_frames * UINT64_C(100000)) / interval_ms : 0;
                uint64_t dex_hits = 0, dex_loads = 0, dex_bytes = 0;
                uint32_t dex_warm = 0;
                uint64_t dex_evictions = 0;
                uint64_t native_hits = 0, native_misses = 0;
                etd2x_package_get_dex_runtime_cache_stats(&dex_hits, &dex_loads, &dex_bytes);
                etd2x_package_get_dex_runtime_cache_detail(&dex_warm, &dex_evictions);
                etd2x_dex_get_native_resolver_cache_stats(&native_hits, &native_misses);
                Etd2xAudioSessionStats audio_stats = {0};
                Etd2xAudioSnapshot audio_snapshot = {0};
                Etd2xSLPcmRuntimeStats opensl_stats = {0};
                etd2x_audio_get_stats(&audio_stats);
                etd2x_audio_get_snapshot(&audio_snapshot);
                etd2x_opensles_get_pcm_runtime_stats(&opensl_stats);
                const uint64_t frame_avg_us = interval_measured_frames ?
                    interval_frame_total_us / interval_measured_frames : 0;
                const uint64_t draw_avg_us = interval_measured_frames ?
                    interval_draw_total_us / interval_measured_frames : 0;
                ETD2X_LOGCI("JAVA-GL", "render heartbeat frames=%" PRIu64
                            " elapsedMs=%llu intervalFps=%llu.%02llu inputEvents=%llu activeTouches=%d "
                            "frameAvgUs=%llu frameMaxUs=%llu drawAvgUs=%llu drawMaxUs=%llu queueMaxUs=%llu swapMaxUs=%llu "
                            "over16=%llu over18=%llu over20=%llu directRender=%llu renderDexFallback=%llu "
                            "dexHotHits=%llu dexLoads=%llu dexReadMiB=%llu dexWarm=%u dexEvictions=%llu "
                            "nativeCacheHits=%llu nativeCacheMisses=%llu "
                            "inputDownCalls=%llu inputDownMaxMs=%llu inputDownFrameMaxMs=%llu "
                            "directInput=%llu dexInputFallback=%llu "
                            "audioWorker=%d audioQueued=%u audioMinQueued=%u audioUnderruns=%u audioRefills=%llu audioMaxGapUs=%llu audioMaxRefillUs=%llu "
                            "openSlPlayers=%u openSlQueued=%u openSlEnqueued=%llu openSlConsumed=%llu",
                            frame_count,
                            (unsigned long long)(now_ms - loop_begin_ms),
                            (unsigned long long)(fps_x100 / 100u),
                            (unsigned long long)(fps_x100 % 100u),
                            (unsigned long long)input_event_count,
                            java_touch_previous.count,
                            (unsigned long long)frame_avg_us,
                            (unsigned long long)interval_frame_max_us,
                            (unsigned long long)draw_avg_us,
                            (unsigned long long)interval_draw_max_us,
                            (unsigned long long)interval_queue_max_us,
                            (unsigned long long)interval_swap_max_us,
                            (unsigned long long)interval_over_16667,
                            (unsigned long long)interval_over_18000,
                            (unsigned long long)interval_over_20000,
                            (unsigned long long)direct_render_calls,
                            (unsigned long long)render_dex_fallbacks,
                            (unsigned long long)dex_hits,
                            (unsigned long long)dex_loads,
                            (unsigned long long)(dex_bytes / (1024u * 1024u)),
                            dex_warm,
                            (unsigned long long)dex_evictions,
                            (unsigned long long)native_hits,
                            (unsigned long long)native_misses,
                            (unsigned long long)java_input_perf.down_calls,
                            (unsigned long long)java_input_perf.down_max_ms,
                            (unsigned long long)java_input_perf.down_frame_max_ms,
                            (unsigned long long)java_input_perf.direct_native_calls,
                            (unsigned long long)java_input_perf.dex_fallback_calls,
                            etd2x_audio_worker_running() ? 1 : 0,
                            audio_snapshot.queued_buffers,
                            audio_stats.min_queued,
                            audio_stats.underruns,
                            (unsigned long long)audio_stats.worker_refills,
                            (unsigned long long)audio_stats.max_service_gap_us,
                            (unsigned long long)audio_stats.max_refill_us,
                            opensl_stats.players,
                            opensl_stats.queued_buffers,
                            (unsigned long long)opensl_stats.buffers_enqueued,
                            (unsigned long long)opensl_stats.buffers_consumed);
                last_reported = frame_count;
                last_report_ms = now_ms;
                interval_frame_max_us = 0;
                interval_queue_max_us = 0;
                interval_draw_max_us = 0;
                interval_swap_max_us = 0;
                interval_frame_total_us = 0;
                interval_draw_total_us = 0;
                interval_measured_frames = 0;
                interval_over_16667 = 0;
                interval_over_18000 = 0;
                interval_over_20000 = 0;
            }
        }
        if (java_renderer_input_enabled &&
            !main_java_renderer_cancel_input(apk_path, renderer_class, java_gl_info.renderer_ref,
                                        &java_input_caps, &java_touch_previous,
                                        frame_count, &input_event_count, &java_input_perf)) {
            ETD2X_LOGCE("INPUT", "renderer touch cancel failed during loop shutdown");
            frame_loop_ok = false;
        }
        if (!java_renderer_input_enabled) etd2x_android_cancel_touches(etd2x_android_activity());
        if (activity_transition_requested && frame_loop_ok) {
            if (!main_java_activity_enter_background(apk_path, activity_class, etd2x_android_activity(),
                                                 java_gl_info.view_ref, "activity-transition")) goto cleanup;
            (void)main_java_gl_call(main_java_gl_egl_shutdown_job, &java_egl);
            main_java_gl_thread_stop();
            etd2x_jni_delete_local_ref_host(etd2x_jni_current_env(), gl10);
            etd2x_jni_delete_local_ref_host(etd2x_jni_current_env(), java_gl_info.renderer_ref);
            etd2x_jni_delete_local_ref_host(etd2x_jni_current_env(), java_gl_info.view_ref);
            memset(&java_gl_info, 0, sizeof(java_gl_info)); java_gl_info_ready = false;
            if (java_input_ready) { etd2x_input_shutdown(); java_input_ready = false; }
            if (java_applet_hook_ready) { appletUnhook(&java_applet_hook_cookie); java_applet_hook_ready = false; }
            if (applet_exit_locked) { (void)appletUnlockExit(); applet_exit_locked = false; }
            activity_transitions = 0;
            goto started_activity;
        }
        main_java_activity_shutdown(apk_path, activity_class, etd2x_android_activity(),
                               java_gl_info.view_ref, activity_backgrounded);
        java_activity_shutdown_done = true;
        Etd2xCompatStorageStats storage_stats = {0};
        etd2x_compat_get_storage_stats(&storage_stats);
        ETD2X_LOGCI("STORAGE", "session mappedPaths=%llu writeOpens=%llu mutations=%llu",
                    (unsigned long long)storage_stats.mapped_paths,
                    (unsigned long long)storage_stats.write_opens,
                    (unsigned long long)storage_stats.mutations);
        etd2x_jni_delete_local_ref_host(etd2x_jni_current_env(), gl10);
        ETD2X_LOGT("stage14.java-frame-loop: %s frames=%" PRIu64 " inputEvents=%" PRIu64 " exit=%s",
                   frame_loop_ok ? "PASS" : "FAIL", frame_count,
                   input_event_count,
                   exit_requested ? "plus" : "applet");
        pass = frame_loop_ok && frame_count > 1u;
        goto cleanup;
    }

    void *entry_addr = NULL;
    if (!resolve(root, "ANativeActivity_onCreate", &entry_addr) || !entry_addr) {
        ETD2X_LOGT("stage12.native-entry-symbol: FAIL");
        goto cleanup;
    }

    /* Android passes guest-visible filesystem paths into ANativeActivity.
     * Host sdmc:/ paths are ETD2X implementation details and must not leak
     * into guest native code. Bionic path translation maps these namespaces
     * back onto app_root/files, app_root/external/files and app_root/obb. */
    char native_internal_path[ETD2X_APK_PATH_CAP];
    char native_external_path[ETD2X_APK_PATH_CAP];
    char native_obb_path[ETD2X_APK_PATH_CAP];
    if (snprintf(native_internal_path, sizeof(native_internal_path),
                 "/data/data/%s/files", info.package_name) >= (int)sizeof(native_internal_path) ||
        snprintf(native_external_path, sizeof(native_external_path),
                 "/storage/emulated/0/Android/data/%s/files", info.package_name) >= (int)sizeof(native_external_path) ||
        snprintf(native_obb_path, sizeof(native_obb_path),
                 "/storage/emulated/0/Android/obb/%s", info.package_name) >= (int)sizeof(native_obb_path)) {
        ETD2X_LOGT("stage12.native-activity-paths: FAIL package path too long");
        goto cleanup;
    }
    Etd2xNativeActivityConfig native_cfg = {0};
    native_cfg.internal_data_path = native_internal_path;
    native_cfg.external_data_path = native_external_path;
    native_cfg.obb_path = native_obb_path;
    native_cfg.sdk_version = 30;
    native_cfg.window_width = display_plan.logical_width;
    native_cfg.window_height = display_plan.logical_height;
    native_cfg.window_format = 0;
    native_cfg.display_plan = display_plan;
    if (!etd2x_native_activity_init(&native_cfg, etd2x_jni_vm(), etd2x_jni_current_env(),
                                    etd2x_android_activity(), etd2x_assets_manager())) {
        ETD2X_LOGT("stage12.native-activity-model: FAIL");
        goto cleanup;
    }
    native_ready = true;
    ETD2X_LOGT("stage12.native-activity-model: PASS activity=%p window=%p input=%p",
               (void *)etd2x_native_activity(), (void *)etd2x_native_window(),
               (void *)etd2x_native_input_queue());

    typedef void (*main_NativeActivityCreateFn)(Etd2xANativeActivity *, void *, size_t);
    ETD2X_LOGCI("NATIVE", "calling guest ANativeActivity_onCreate entry=%p", entry_addr);
    ((main_NativeActivityCreateFn)entry_addr)(etd2x_native_activity(), NULL, 0);
    ETD2X_LOGT("stage12.native-entry-called: PASS instance=%p", etd2x_native_activity()->instance);

    lifecycle_ok = etd2x_native_activity_start();
    ETD2X_LOGT("stage12.lifecycle-start: %s", lifecycle_ok ? "PASS" : "FAIL");
    bool exit_requested = false;
    uint64_t last_reported = 0;
    if (lifecycle_ok) {
        ETD2X_LOGI("ETD2X 0.0.15 NativeActivity game loop active: manifest orientation + transformed touch; PLUS exits to hbmenu");
        while (appletMainLoop() && !exit_requested) {
            etd2x_choreographer_pump();
            etd2x_native_activity_pump_input(&exit_requested);
            if (!audio_worker_ready && !etd2x_audio_update())
                ETD2X_LOGCE("AUDIO", "NativeActivity fallback audio update failed");
            const uint64_t presents = etd2x_graphics_present_count();
            if (presents > 0) saw_present = true;
            if (presents >= last_reported + 300) {
                ETD2X_LOGCI("NATIVE", "guest presentation heartbeat frames=%" PRIu64, presents);
                last_reported = presents;
            }
            svcSleepThread(4000000L);
        }
    }

    ETD2X_LOGT("stage12.game-present: %s frames=%" PRIu64,
               saw_present ? "PASS" : "FAIL", etd2x_graphics_present_count());
    if (lifecycle_ok) etd2x_native_activity_stop();
    lifecycle_ok = false;
    pass = saw_present;

cleanup:
    main_startup_watchdog_stop(&startup_watchdog, pass ? "launch-complete" : "launch-cleanup");
    phase_begin_ms = main_monotonic_ms();
    if (java_route && java_activity_created && !java_activity_shutdown_done &&
        android_ready && jni_ready && java_activity_class[0]) {
        ETD2X_LOGCI("JAVA", "failure-path graceful Activity shutdown begin class=%s",
                    java_activity_class);
        main_java_activity_shutdown(apk_path, java_activity_class, etd2x_android_activity(),
                               java_gl_info_ready ? java_gl_info.view_ref : NULL, false);
        java_activity_shutdown_done = true;
        ETD2X_LOGCI("JAVA", "failure-path graceful Activity shutdown complete class=%s",
                    java_activity_class);
    }
    if (jni_ready && java_provider_ref_count) {
        void *env = etd2x_jni_current_env();
        for (size_t i = 0; env && i < java_provider_ref_count; ++i) {
            if (java_provider_refs[i]) etd2x_jni_delete_global_ref_host(env, java_provider_refs[i]);
            java_provider_refs[i] = NULL;
        }
        java_provider_ref_count = 0;
    }
    if (java_input_ready) {
        etd2x_input_shutdown();
        java_input_ready = false;
    }
    if (java_applet_hook_ready) {
        appletUnhook(&java_applet_hook_cookie);
        java_applet_hook_ready = false;
        ETD2X_LOGCI("JAVA", "applet lifecycle hook removed");
    }
    if (applet_exit_locked) {
        Result unlock_rc = appletUnlockExit();
        ETD2X_LOGCI("JAVA", "applet exit unlock rc=0x%08x", unlock_rc);
        applet_exit_locked = false;
    }

    /* Guest pthreads execute both guest ELF text and ETD2X compatibility/JNI
     * callbacks. No runtime-owned subsystem may be finalized or unmapped until
     * every guest-created thread has exited. Returning an NRO to hbloader with
     * live guest threads is equally unsafe: hbloader unloads ETD2X itself while
     * those threads can still call back into its text. java.lang.Thread
     * workers are host threads in the same position; Android ends them with
     * the process, so a pending process exit unwinds their Looper/join/sleep
     * waits before they are counted. */
    if (etd2x_android_live_java_thread_count() != 0)
        etd2x_jni_request_process_exit("app process teardown");
    size_t live_java_threads = etd2x_android_wait_java_threads_idle(1500u);
    bool guest_threads_idle = etd2x_bionic_pthread_wait_guest_idle(1500u);
    size_t live_guest_threads = etd2x_bionic_pthread_live_guest_count();
    ETD2X_LOGCI("THREAD", "pre-teardown guest quiescence idle=%d live=%zu javaLive=%zu",
                guest_threads_idle ? 1 : 0, live_guest_threads, live_java_threads);
    if (!guest_threads_idle || live_java_threads != 0) {
        etd2x_android_log_java_threads("pre-teardown-survivors");
        (void)etd2x_bionic_pthread_log_wait_states("pre-teardown-survivors");
        ETD2X_LOGCE("THREAD",
                    "guest threads survived graceful shutdown; refusing NRO return live=%zu javaLive=%zu",
                    live_guest_threads, live_java_threads);
        ETD2X_LOGCE("THREAD",
                    "terminating process atomically before hbloader can unload ETD2X");
        etd2x_diag_flush();
#ifdef __SWITCH__
        (void)fsdevCommitDevice("sdmc");
        svcExitProcess();
        __builtin_unreachable();
#else
        /* Host regression binaries do not run through hbloader. Preserve the
         * mappings and report failure rather than invoking a platform exit. */
        return false;
#endif
    }
    /* Retain EGL through guest quiescence. Join its host owner before any
     * DEX/JNI/guest mappings can be reset or hbloader can unload ETD2X. */
    (void)main_java_gl_call(main_java_gl_egl_shutdown_job, &java_egl);
    main_java_gl_thread_stop();
    uint64_t dex_hot_hits = 0, dex_hot_loads = 0, dex_hot_bytes = 0;
    uint32_t dex_hot_warm = 0;
    uint64_t dex_hot_evictions = 0;
    etd2x_package_get_dex_runtime_cache_stats(&dex_hot_hits, &dex_hot_loads, &dex_hot_bytes);
    etd2x_package_get_dex_runtime_cache_detail(&dex_hot_warm, &dex_hot_evictions);
    ETD2X_LOGCI("PERF", "DEX runtime cache hits=%llu loads=%llu bytesRead=%llu warm=%u evictions=%llu",
                (unsigned long long)dex_hot_hits,
                (unsigned long long)dex_hot_loads,
                (unsigned long long)dex_hot_bytes,
                dex_hot_warm,
                (unsigned long long)dex_hot_evictions);
    uint64_t native_cache_hits = 0, native_cache_misses = 0;
    etd2x_dex_get_native_resolver_cache_stats(&native_cache_hits, &native_cache_misses);
    ETD2X_LOGCI("PERF", "DEX native resolver cache hits=%llu misses=%llu",
                (unsigned long long)native_cache_hits,
                (unsigned long long)native_cache_misses);
    etd2x_dex_set_native_resolver(NULL, NULL);
    etd2x_dex_set_java_method_source(NULL);
    etd2x_package_reset_dex_runtime_cache();
    if (java_gl_info_ready && jni_ready) {
        if (java_gl_info.renderer_ref)
            etd2x_jni_delete_local_ref_host(etd2x_jni_current_env(), java_gl_info.renderer_ref);
        if (java_gl_info.view_ref)
            etd2x_jni_delete_local_ref_host(etd2x_jni_current_env(), java_gl_info.view_ref);
        memset(&java_gl_info, 0, sizeof(java_gl_info));
        java_gl_info_ready = false;
    }
    if (native_ready) etd2x_native_activity_reset();
    if (media_ready) {
        Etd2xMediaStats media_stats;
        etd2x_media_get_stats(&media_stats);
        ETD2X_LOGCI("MEDIA", "session players=%llu realized=%llu started=%llu finished=%llu mp3Frames=%llu wavClips=%llu vorbisClips=%llu bytes=%llu mixedFrames=%llu starvations=%llu active=%u",
                    (unsigned long long)media_stats.players_created,
                    (unsigned long long)media_stats.players_realized,
                    (unsigned long long)media_stats.players_started,
                    (unsigned long long)media_stats.players_finished,
                    (unsigned long long)media_stats.mp3_frames_decoded,
                    (unsigned long long)media_stats.wav_clips_decoded,
                    (unsigned long long)media_stats.vorbis_clips_decoded,
                    (unsigned long long)media_stats.source_bytes_read,
                    (unsigned long long)media_stats.output_frames_mixed,
                    (unsigned long long)media_stats.decode_starvations,
                    media_stats.active_players);
    }
    if (media_ready) {
        Etd2xAudioSessionStats audio_stats = {0};
        Etd2xAudioSnapshot audio_snapshot = {0};
        Etd2xSLPcmRuntimeStats opensl_stats = {0};
        etd2x_audio_get_stats(&audio_stats);
        etd2x_audio_get_snapshot(&audio_snapshot);
        etd2x_opensles_get_pcm_runtime_stats(&opensl_stats);
        ETD2X_LOGCI("AUDIO",
                    "session workerStarted=%d workerRunning=%d wakeups=%llu refills=%llu timeouts=%llu queued=%u minQueued=%u underruns=%u played=%llu shortfallFrames=%llu maxGapUs=%llu maxRefillUs=%llu",
                    audio_stats.worker_started ? 1 : 0,
                    etd2x_audio_worker_running() ? 1 : 0,
                    (unsigned long long)audio_stats.worker_wakeups,
                    (unsigned long long)audio_stats.worker_refills,
                    (unsigned long long)audio_stats.worker_timeouts,
                    audio_snapshot.queued_buffers,
                    audio_stats.min_queued,
                    audio_stats.underruns,
                    (unsigned long long)audio_snapshot.played_samples,
                    (unsigned long long)audio_stats.renderer_shortfall_frames,
                    (unsigned long long)audio_stats.max_service_gap_us,
                    (unsigned long long)audio_stats.max_refill_us);
        ETD2X_LOGCI("OPENSL",
                    "session pcmPlayers=%u queued=%u framesMixed=%llu enqueued=%llu consumed=%llu",
                    opensl_stats.players,
                    opensl_stats.queued_buffers,
                    (unsigned long long)opensl_stats.frames_mixed,
                    (unsigned long long)opensl_stats.buffers_enqueued,
                    (unsigned long long)opensl_stats.buffers_consumed);
    }
    etd2x_audio_shutdown();
    audio_worker_ready = false;
    etd2x_opensles_reset();
    etd2x_aaudio_reset();

    if (linker_ready) {
        const size_t cxa_before = etd2x_bionic_cxa_pending_count();
        etd2x_bionic_cxa_finalize(NULL);
        const size_t cxa_after = etd2x_bionic_cxa_pending_count();
        ETD2X_LOGCI("CXA", "process finalization pending_before=%zu pending_after=%zu",
                    cxa_before, cxa_after);
    }
    etd2x_bionic_cxa_reset();
    etd2x_linker_unload(&linker);
    if (android_ready) etd2x_android_reset();
    if (jni_ready) etd2x_jni_reset();
    etd2x_media_reset();
    if (graphics_ready) etd2x_graphics_reset();
    etd2x_compat_set_app_storage(NULL, NULL);
    etd2x_android_compat_reset_profile();
    etd2x_thread_policy_reset();
    ETD2X_LOGCI("PERF", "stage14 phase=cleanup elapsedMs=%llu",
                (unsigned long long)(main_monotonic_ms() - phase_begin_ms));

    etd2x_dex_log_unresolved_summary();
    ETD2X_LOGT("STAGE 14.8.84 GAME LAUNCH: %s", pass ? "PASS" : "FAIL");
    return pass;
}




int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (etd2x_heap_cache_enable) etd2x_heap_cache_enable();
    if (!nx_storage_init()) return 1;
    etd2x_diag_init(NX_LOG);
    etd2x_diag_set_stdout(false);
    etd2x_diag_set_full_trace(access(NX_ROOT "/config/full-trace.flag", F_OK) == 0);
    etd2x_dex_set_strict_linkage(access(NX_ROOT "/config/strict-first-fault.flag", F_OK) == 0);
    ETD2X_LOGI("earntodie2_nx 1.0.0 starting; package=%s", NX_PACKAGE);
    Result romfs_rc = romfsInit();
    if (R_FAILED(romfs_rc)) {
        ETD2X_LOGCE("BOOT", "RomFS mount failed rc=0x%08x", romfs_rc);
        etd2x_diag_shutdown();
        return 1;
    }
    bool passed = false;
    Etd2xApkInfo info = {0};
    Etd2xResult rc = etd2x_package_probe(NX_GAME_APK, &info);
    if (rc != ETD2X_OK || strcmp(info.package_name, NX_PACKAGE)) {
        ETD2X_LOGCE("BOOT", "Place your ARM64 Earn to Die 2 XAPK at %s; probe=%s package=%s",
                    NX_GAME_APK, etd2x_result_string(rc), info.package_name);
    } else {
        /* Identity validation chooses the packaged application only. Android
         * metadata, signing, symbols and compatibility behavior stay intact. */
        passed = main_run_game(NX_GAME_APK, &info);
    }
    etd2x_dex_profiler_shutdown();
    etd2x_compat_network_shutdown();
    ETD2X_LOGI("earntodie2_nx ended status=%s", passed ? "clean" : "failed");
    etd2x_diag_shutdown();
    (void)fsdevCommitDevice("sdmc");
    romfsExit();
    return passed ? 0 : 1;
}

#pragma pop_macro("MAIN_LOOP_STALL_REPEAT_MS")
#pragma pop_macro("MAIN_LOOP_STALL_MS")
