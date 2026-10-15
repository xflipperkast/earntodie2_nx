/* runtime diagnostics: preserved standalone implementation. */
#define _GNU_SOURCE 1
#include "runtime.h"


/* ---- log ---- */
#pragma push_macro("ETD2X_LOG_FLUSH_INTERVAL_NS")
#pragma push_macro("ETD2X_LOG_RING_SIZE")
#ifdef __SWITCH__



#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

/* Logging never performs I/O on the calling thread. A line is copied into a
 * ring buffer under a short lock; a writer thread moves it to the log file,
 * stdout and the debug channel. Previously every WARN/ERROR (and every 64th
 * line) fflush()ed to the SD card while holding the global log lock, so any
 * thread that logged - including the app's render thread - could stall behind
 * a card write, and behind the lowest-priority periodic flusher holding that
 * lock (priority inversion). Warnings still reach the card promptly: they wake
 * the writer, which flushes without the producers waiting for it. */
#define ETD2X_LOG_RING_SIZE (512u * 1024u)
#define ETD2X_LOG_FLUSH_INTERVAL_NS 100000000LL

static FILE *log_g_log_file = NULL;
static bool log_g_stdout_enabled = true;
static bool log_g_quiet = false;
static bool log_g_full_trace = false;
static char log_g_log_buffer[64 * 1024];

static char log_g_ring[ETD2X_LOG_RING_SIZE];
static size_t log_g_ring_head;   /* write position */
static size_t log_g_ring_used;   /* bytes pending */
static unsigned long log_g_ring_dropped;
static bool log_g_ring_urgent;
static pthread_mutex_t log_g_ring_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t log_g_ring_cond = PTHREAD_COND_INITIALIZER;
/* Serializes file/stdout/debug output (writer thread, explicit flushes). */
static pthread_mutex_t log_g_output_lock = PTHREAD_MUTEX_INITIALIZER;

static Thread log_g_flush_thread;
static bool log_g_flush_thread_running = false;
static atomic_bool log_g_flush_thread_stop = false;

/* Takes up to cap pending bytes out of the ring (caller holds g_ring_lock). */
static size_t log_ring_take_locked(char *out, size_t cap) {
    size_t n = log_g_ring_used < cap ? log_g_ring_used : cap;
    const size_t tail = (log_g_ring_head + ETD2X_LOG_RING_SIZE - log_g_ring_used) % ETD2X_LOG_RING_SIZE;
    const size_t first = n < ETD2X_LOG_RING_SIZE - tail ? n : ETD2X_LOG_RING_SIZE - tail;
    memcpy(out, log_g_ring + tail, first);
    memcpy(out + first, log_g_ring, n - first);
    log_g_ring_used -= n;
    return n;
}

static void log_output_chunk(const char *data, size_t n) {
    if (!n) return;
    if (log_g_stdout_enabled) fwrite(data, 1, n, stdout);
    svcOutputDebugString(data, n);
    if (log_g_log_file) fwrite(data, 1, n, log_g_log_file);
}

/* Moves everything pending to the outputs; flushes the file when asked. */
static void log_drain(bool flush_file) {
    static char chunk[16 * 1024];
    ETD2X_PERF_LOCK(&log_g_output_lock);
    for (;;) {
        ETD2X_PERF_LOCK(&log_g_ring_lock);
        const size_t n = log_ring_take_locked(chunk, sizeof(chunk));
        unsigned long dropped = log_g_ring_dropped;
        log_g_ring_dropped = 0;
        pthread_mutex_unlock(&log_g_ring_lock);
        if (dropped) {
            char note[96];
            const int len = snprintf(note, sizeof(note),
                                     "[ETD2X][LOG][WARN] %lu log lines dropped (ring full)\n", dropped);
            if (len > 0) log_output_chunk(note, (size_t)len);
        }
        if (!n) break;
        log_output_chunk(chunk, n);
    }
    if (flush_file && log_g_log_file) fflush(log_g_log_file);
    pthread_mutex_unlock(&log_g_output_lock);
}

void etd2x_diag_emergency_write(const char *text) {
    static char chunk[16 * 1024];
    bool output = false, ring = false;
    for (int i = 0; i < 100 && !(output = pthread_mutex_trylock(&log_g_output_lock) == 0); ++i) svcSleepThread(2000000);
    for (int i = 0; i < 100 && !(ring = pthread_mutex_trylock(&log_g_ring_lock) == 0); ++i) svcSleepThread(2000000);
    if (ring) {
        for (size_t n; (n = log_ring_take_locked(chunk, sizeof(chunk))) > 0;) log_output_chunk(chunk, n);
        pthread_mutex_unlock(&log_g_ring_lock);
    }
    if (text) log_output_chunk(text, strlen(text));
    if (log_g_log_file) fflush(log_g_log_file);
    if (output) pthread_mutex_unlock(&log_g_output_lock);
}

static void log_flush_thread_main(void *arg) {
    (void)arg;
    while (!atomic_load_explicit(&log_g_flush_thread_stop, memory_order_acquire)) {
        ETD2X_PERF_LOCK(&log_g_ring_lock);
        if (!log_g_ring_used && !log_g_ring_urgent) {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec += ETD2X_LOG_FLUSH_INTERVAL_NS;
            if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
            (void)pthread_cond_timedwait(&log_g_ring_cond, &log_g_ring_lock, &deadline);
        }
        log_g_ring_urgent = false;
        pthread_mutex_unlock(&log_g_ring_lock);
        /* A hang or CPU exception must not lose the tail: pending output is
         * on the card within one interval. */
        log_drain(true);
    }
}

static void log_flush_thread_start(void) {
    atomic_store_explicit(&log_g_flush_thread_stop, false, memory_order_release);
    /* Default application priority: the writer must keep up under load, and it
     * never holds a lock a producer waits on during I/O. */
    if (R_SUCCEEDED(threadCreate(&log_g_flush_thread, log_flush_thread_main, NULL, NULL,
                                 0x8000, 0x2C, -2))) {
        if (R_SUCCEEDED(threadStart(&log_g_flush_thread))) {
            log_g_flush_thread_running = true;
            return;
        }
        threadClose(&log_g_flush_thread);
    }
}

static void log_flush_thread_stop(void) {
    if (!log_g_flush_thread_running) return;
    atomic_store_explicit(&log_g_flush_thread_stop, true, memory_order_release);
    ETD2X_PERF_LOCK(&log_g_ring_lock);
    pthread_cond_broadcast(&log_g_ring_cond);
    pthread_mutex_unlock(&log_g_ring_lock);
    threadWaitForExit(&log_g_flush_thread);
    threadClose(&log_g_flush_thread);
    log_g_flush_thread_running = false;
}

bool etd2x_diag_init(const char *log_path) {
    ETD2X_PERF_LOCK(&log_g_output_lock);
    if (log_g_log_file) {
        pthread_mutex_unlock(&log_g_output_lock);
        return true;
    }

    if (log_path && log_path[0]) {
        /* Preserve the immediately previous hardware run. This matters for
         * persistence tests: the relaunch used to verify a save must not erase
         * the shutdown/write evidence from the run that created it. */
        char previous_path[1024];
        const int previous_len = snprintf(previous_path, sizeof(previous_path),
                                          "%s.prev", log_path);
        if (previous_len > 0 && (size_t)previous_len < sizeof(previous_path)) {
            (void)remove(previous_path);
            (void)rename(log_path, previous_path);
        }
        log_g_log_file = fopen(log_path, "w");
        if (log_g_log_file) setvbuf(log_g_log_file, log_g_log_buffer, _IOFBF, sizeof(log_g_log_buffer));
    }

    const bool ok = log_g_log_file != NULL;
    pthread_mutex_unlock(&log_g_output_lock);
    if (ok) log_flush_thread_start();
    return ok;
}

void etd2x_diag_set_stdout(bool enabled) {
    log_g_stdout_enabled = enabled;
}

void etd2x_diag_set_quiet(bool enabled) {
    log_g_quiet = enabled;
}

void etd2x_diag_set_full_trace(bool enabled) {
    log_g_full_trace = enabled;
}

bool etd2x_diag_full_trace_enabled(void) {
    return log_g_full_trace;
}

/* Synchronous: everything logged so far is on the card when this returns
 * (crash and shutdown paths rely on it). */
void etd2x_diag_flush(void) {
    log_drain(true);
}

void etd2x_diag_shutdown(void) {
    log_flush_thread_stop();
    log_drain(true);
    ETD2X_PERF_LOCK(&log_g_output_lock);
    if (log_g_log_file) {
        fclose(log_g_log_file);
        log_g_log_file = NULL;
    }
    pthread_mutex_unlock(&log_g_output_lock);
}

static void log_emit_line(const char *line, bool force_flush) {
    const size_t n = strlen(line);
    if (!log_g_flush_thread_running) {
        /* Before the writer exists (or without a log file) write directly. */
        ETD2X_PERF_LOCK(&log_g_output_lock);
        log_output_chunk(line, n);
        if (force_flush && log_g_log_file) fflush(log_g_log_file);
        pthread_mutex_unlock(&log_g_output_lock);
        return;
    }
    ETD2X_PERF_LOCK(&log_g_ring_lock);
    if (n <= ETD2X_LOG_RING_SIZE - log_g_ring_used) {
        const size_t first = n < ETD2X_LOG_RING_SIZE - log_g_ring_head ? n : ETD2X_LOG_RING_SIZE - log_g_ring_head;
        memcpy(log_g_ring + log_g_ring_head, line, first);
        memcpy(log_g_ring, line + first, n - first);
        log_g_ring_head = (log_g_ring_head + n) % ETD2X_LOG_RING_SIZE;
        log_g_ring_used += n;
    } else {
        log_g_ring_dropped++;
    }
    if (force_flush || log_g_ring_used >= ETD2X_LOG_RING_SIZE / 2u) {
        log_g_ring_urgent = true;
        pthread_cond_signal(&log_g_ring_cond);
    }
    pthread_mutex_unlock(&log_g_ring_lock);
}

void etd2x_vlog(const char *level, const char *fmt, va_list args) {
    char message[1024];
    char line[1152];

    vsnprintf(message, sizeof(message), fmt, args);
    const char *actual_level = level ? level : "LOG";
    if (log_g_quiet && strcmp(actual_level, "INFO") == 0) return;
    if (log_g_quiet && strcmp(actual_level, "TEST") == 0 && strstr(message, "FAIL") == NULL) return;
    snprintf(line, sizeof(line), "[ETD2X][%s] %s\n", actual_level, message);
    /* TEST lines are lifecycle checkpoints: each is durable before the next
     * guest callback runs. */
    const bool force_flush = strcmp(actual_level, "ERROR") == 0 || strcmp(actual_level, "WARN") == 0 ||
                             strcmp(actual_level, "TEST") == 0;
    log_emit_line(line, force_flush);
}

void etd2x_log(const char *level, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    etd2x_vlog(level, fmt, args);
    va_end(args);
}

/* Stage 14.8.36 FAST mode: startup executes hundreds of Java calls per frame.
 * These per-call traces are useful only for deep diagnostics and each line
 * also crosses svcOutputDebugString + buffered SD logging. Keep warnings/
 * errors and compact TEST/PERF checkpoints visible, while full-trace.flag
 * restores the detailed stream. Every rule matches a literal prefix of the
 * format string, so a dropped trace is rejected before it is formatted. */
static bool log_fast_mode_drops(const char *category, const char *text) {
    if (!category || !text) return false;
    if (strcmp(category, "PACKAGE") == 0) return strncmp(text, "plan cache hit ", 15) == 0;
    if (strcmp(category, "JAVA") == 0)
        return strncmp(text, "DEX class resolved(index) ", 26) == 0 ||
               strncmp(text, "DEX method resolved(index) ", 27) == 0 ||
               strncmp(text, "DEX static field resolved(index) ", 33) == 0;
    if (strcmp(category, "ANDROID") == 0)
        return strncmp(text, "Handler bound ", 14) == 0 ||
               strncmp(text, "FileChannel.lock ", 17) == 0 ||
               strncmp(text, "Resources.getString ", 20) == 0 ||
               (strncmp(text, "PackageManager.getPackageInfo ", 30) == 0 &&
                strstr(text, "result=ok") != NULL);
    if (strcmp(category, "JNI") == 0)
        return strncmp(text, "FindClass ", 10) == 0 ||
               strncmp(text, "NewStringUTF ", 13) == 0 ||
               strncmp(text, "GetStaticMethodID ", 18) == 0 ||
               strncmp(text, "GetMethodID ", 12) == 0 ||
               strncmp(text, "GetStaticFieldID ", 17) == 0 ||
               strncmp(text, "GetFieldID ", 11) == 0 ||
               strncmp(text, "lazy DEX class registered ", 26) == 0 ||
               strncmp(text, "DEX class hierarchy reconciled ", 31) == 0 ||
               strncmp(text, "DEX interface registered ", 25) == 0;
    if (strcmp(category, "DEX") == 0)
        return strncmp(text, "enter class=", 12) == 0 ||
               strncmp(text, "leave class=", 12) == 0 ||
               strncmp(text, "framework call class=", 21) == 0 ||
               strncmp(text, "framework inherited call class=", 31) == 0 ||
               strncmp(text, "framework intrinsic class=", 26) == 0 ||
               strncmp(text, "class init begin ", 17) == 0 ||
               strncmp(text, "class init complete ", 20) == 0 ||
               strncmp(text, "instance field materialized ", 28) == 0 ||
               strncmp(text, "static field materialized ", 26) == 0 ||
               strncmp(text, "JNI->DEX method dynamically bridgeable ", 39) == 0 ||
               strncmp(text, "JNI->DEX method bound ", 22) == 0 ||
               strncmp(text, "declared members materialized ", 30) == 0;
    return false;
}

void etd2x_log_category(const char *category, const char *level, const char *fmt, ...) {
    char message[960];
    char line[1152];
    va_list args;

    const char *actual_level = level ? level : "INFO";
    const bool info = strcmp(actual_level, "INFO") == 0;
    if (log_g_quiet && info) return;
    if (!log_g_full_trace && info && log_fast_mode_drops(category, fmt)) return;

    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    /* Normal hardware iteration mode keeps execution identical while avoiding
     * diagnostic work that used to dominate the Java/DEX feedback loop. Deep
     * constructor disassembly and per-symbol bind traces remain available via
     * full-trace.flag. Warnings/errors and compact summaries are never hidden. */
    if (!log_g_full_trace && info) {
        if (log_fast_mode_drops(category, message)) return; /* formats with a leading conversion */
        if (category && strcmp(category, "CTORTRACE") == 0) return;
        if (category && strcmp(category, "CTOR") == 0 &&
            (strstr(message, "init_array[") != NULL || strncmp(message, "DT_INIT ->", 10) == 0)) return;
        if (category && (strcmp(category, "COMPAT") == 0 || strcmp(category, "LINKER") == 0) &&
            strncmp(message, "symbol=", 7) == 0) return;
    }

    snprintf(line, sizeof(line), "[ETD2X][%s][%s] %s\n",
             category ? category : "LOG", actual_level, message);
    const bool force_flush = strcmp(actual_level, "ERROR") == 0 || strcmp(actual_level, "WARN") == 0 ||
                             (category && strcmp(category, "CTOR") == 0) ||
                             (category && strcmp(category, "JAVA") == 0 && strstr(message, "JNI_OnLoad") != NULL);
    log_emit_line(line, force_flush);
}

#endif

#pragma pop_macro("ETD2X_LOG_RING_SIZE")
#pragma pop_macro("ETD2X_LOG_FLUSH_INTERVAL_NS")


/* ---- result ---- */


const char *etd2x_result_string(Etd2xResult result) {
    switch (result) {
        case ETD2X_OK: return "OK";
        case ETD2X_ERR_ARGUMENT: return "invalid argument";
        case ETD2X_ERR_IO: return "I/O error";
        case ETD2X_ERR_NO_MEMORY: return "out of memory";
        case ETD2X_ERR_ELF_MAGIC: return "not an ELF file";
        case ETD2X_ERR_ELF_CLASS: return "ELF is not 64-bit";
        case ETD2X_ERR_ELF_ENDIAN: return "ELF is not little-endian";
        case ETD2X_ERR_ELF_MACHINE: return "ELF is not AArch64";
        case ETD2X_ERR_ELF_TYPE: return "ELF is not ET_DYN";
        case ETD2X_ERR_ELF_BOUNDS: return "ELF contains invalid offsets/sizes";
        case ETD2X_ERR_ELF_NO_LOAD: return "ELF contains no PT_LOAD segments";
        case ETD2X_ERR_VM_RESERVE: return "could not reserve guest virtual address space";
        case ETD2X_ERR_VM_MAP: return "could not map guest code memory";
        case ETD2X_ERR_VM_PERMISSIONS: return "could not apply guest memory permissions";
        case ETD2X_ERR_SYMBOL_TABLE: return "ELF dynamic symbol table unavailable";
        case ETD2X_ERR_SYMBOL_NOT_FOUND: return "exported symbol not found";
        case ETD2X_ERR_RELOCATIONS_REQUIRED: return "ELF requires relocations (not allowed in Stage 1)";
        case ETD2X_ERR_TEST_FAILED: return "conformance test failed";
        case ETD2X_ERR_DYNAMIC_MISSING: return "PT_DYNAMIC is missing";
        case ETD2X_ERR_DYNAMIC_INVALID: return "dynamic metadata is invalid";
        case ETD2X_ERR_RELOCATION_FORMAT: return "unsupported/invalid relocation table format";
        case ETD2X_ERR_RELOCATION_UNSUPPORTED: return "unsupported AArch64 relocation type";
        case ETD2X_ERR_RELOCATION_TARGET: return "relocation target is outside the guest image";
        case ETD2X_ERR_SYMBOL_INDEX: return "relocation references an invalid symbol index";
        case ETD2X_ERR_SYMBOL_UNRESOLVED: return "required symbol could not be resolved";
        case ETD2X_ERR_RELR_INVALID: return "RELR table is malformed";
        case ETD2X_ERR_CONSTRUCTOR: return "constructor metadata is invalid";
        case ETD2X_ERR_DEPENDENCY_NOT_FOUND: return "DT_NEEDED dependency could not be loaded";
        case ETD2X_ERR_DEPENDENCY_INVALID: return "DT_NEEDED dependency metadata/path is invalid";
        case ETD2X_ERR_APK_INVALID: return "APK/ZIP structure is invalid";
        case ETD2X_ERR_APK_UNSUPPORTED: return "APK uses an unsupported feature";
        case ETD2X_ERR_MANIFEST_INVALID: return "AndroidManifest.xml is invalid or unsupported";
        case ETD2X_ERR_ABI_NOT_FOUND: return "APK does not contain the requested ABI/native library";
        case ETD2X_ERR_CANCELLED: return "operation cancelled";
        default: return "unknown ETD2X error";
    }
}



/* ---- crash handler ---- */
#ifdef __SWITCH__
/* CPU exception (crash) reporting.
 *
 * Without a handler a fault in guest or host code terminates the process with
 * nothing in the ETD2X log: queued log lines are lost and the faulting
 * location is only in Atmosphere's crash report. When the kernel delivers the
 * exception to ETD2X's own entry point (ETD2X running as its own process),
 * libnx calls __libnx_exception_handler on the faulting thread (on its own
 * stack). Run as an NRO under hbloader, the process entry is hbloader's, so
 * Atmosphere's crash report remains the record there. This
 * writes the registers, the faulting module/offset and a frame-pointer
 * backtrace through the log's emergency path, then ends the process.
 * Unmapped or unreadable addresses are never dereferenced (svcQueryMemory). */



#include <stdalign.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

alignas(16) u8 __nx_exception_stack[0x10000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

/* __start__ is an absolute (unrelocated) linker symbol; _start is the first
 * instruction of the NRO text and moves with the load address. */
extern char _start[];

static bool crash_handler_crash_readable(uintptr_t address, size_t bytes) {
    MemoryInfo info;
    u32 page = 0;
    if (R_FAILED(svcQueryMemory(&info, &page, address))) return false;
    if (info.type == MemType_Unmapped || !(info.perm & Perm_R)) return false;
    return address + bytes <= info.addr + info.size;
}

static void crash_handler_crash_line(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n > sizeof(line) - 2) n = (int)sizeof(line) - 2;
    line[n] = '\n';
    line[n + 1] = '\0';
    etd2x_diag_emergency_write(line);
}

static void crash_handler_crash_describe(const char *label, uintptr_t address) {
    char where[256] = "";
    const uintptr_t host_base = (uintptr_t)_start;
    if (address >= host_base && address - host_base < UINT64_C(0x10000000))
        snprintf(where, sizeof(where), "etd2x+0x%lx", (unsigned long)(address - host_base));
    else if (!etd2x_compat_describe_guest_address(address, where, sizeof(where)))
        snprintf(where, sizeof(where), "?");
    crash_handler_crash_line("[ETD2X][CRASH][ERROR] %s=0x%lx %s", label, (unsigned long)address, where);
}

static const char *crash_handler_crash_kind(u32 desc) {
    switch (desc) {
        case ThreadExceptionDesc_InstructionAbort: return "instruction-abort";
        case ThreadExceptionDesc_MisalignedPC: return "misaligned-pc";
        case ThreadExceptionDesc_MisalignedSP: return "misaligned-sp";
        case ThreadExceptionDesc_SError: return "serror";
        case ThreadExceptionDesc_BadSVC: return "bad-svc";
        case ThreadExceptionDesc_Trap: return "trap";
        case ThreadExceptionDesc_Other: return "data-abort-or-other";
        default: return "unknown";
    }
}

void __libnx_exception_handler(ThreadExceptionDump *ctx) {
    static volatile int entered;
    if (__atomic_exchange_n(&entered, 1, __ATOMIC_ACQ_REL)) svcExitProcess(); /* fault while reporting */

    char thread_name[16] = "-";
    (void)etd2x_bionic_thread_get_current_name(thread_name);

    /* Phase 1: raw state only (no locks beyond the log's try-locks). */
    crash_handler_crash_line("[ETD2X][CRASH][ERROR] CPU exception %s desc=0x%x esr=0x%x far=0x%lx pc=0x%lx lr=0x%lx sp=0x%lx fp=0x%lx thread=%s",
               crash_handler_crash_kind(ctx->error_desc), ctx->error_desc, ctx->esr, (unsigned long)ctx->far.x,
               (unsigned long)ctx->pc.x, (unsigned long)ctx->lr.x, (unsigned long)ctx->sp.x,
               (unsigned long)ctx->fp.x, thread_name);
    for (int i = 0; i < 29; i += 4) {
        char regs[256];
        int off = 0;
        for (int j = i; j < i + 4 && j < 29; ++j)
            off += snprintf(regs + off, sizeof(regs) - (size_t)off, " x%d=0x%lx", j, (unsigned long)ctx->cpu_gprs[j].x);
        crash_handler_crash_line("[ETD2X][CRASH][ERROR]%s", regs);
    }

    /* Phase 2: symbolication and frame-pointer backtrace. */
    crash_handler_crash_describe("pc", ctx->pc.x);
    crash_handler_crash_describe("lr", ctx->lr.x);
    uintptr_t fp = ctx->fp.x;
    for (int frame = 0; frame < 24 && fp && (fp & 0xF) == 0 && crash_handler_crash_readable(fp, 16); ++frame) {
        const uintptr_t next = ((const uintptr_t *)fp)[0];
        const uintptr_t ret = ((const uintptr_t *)fp)[1];
        if (!ret) break;
        char label[16];
        snprintf(label, sizeof(label), "frame%d", frame);
        crash_handler_crash_describe(label, ret);
        if (next <= fp) break;
        fp = next;
    }
    char wx[160];
    if (etd2x_wx_describe_failure(wx, sizeof(wx))) crash_handler_crash_line("[ETD2X][CRASH][ERROR] %s", wx);
    crash_handler_crash_line("[ETD2X][CRASH][ERROR] terminating process after CPU exception");
    svcExitProcess();
}

#endif



/* ---- inline syscall ---- */
#ifdef __SWITCH__
/* Exception-time routing only. Syscalls run after ReturnFromException, on the
 * faulting thread's normal stack, so blocking calls cannot hold Horizon's
 * process-wide exception gate or ETD2X's shared exception stack. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <switch.h>

extern void etd2x_inline_syscall_entry(void);

static bool inline_syscall_inline_range(uintptr_t address, size_t size, unsigned perm)
    __attribute__((target("general-regs-only")));
static bool inline_syscall_inline_range(uintptr_t address, size_t size, unsigned perm) {
    if (!address || size > UINTPTR_MAX - address) return false;
    while (size) {
        MemoryInfo info;
        u32 page;
        if (R_FAILED(svcQueryMemory(&info, &page, address)) ||
            (info.perm & perm) != perm || address < info.addr ||
            address - info.addr >= info.size) return false;
        size_t n = info.size - (address - info.addr);
        if (n > size) n = size;
        address += n;
        size -= n;
    }
    return true;
}

/* Kernel frame layout: x0..x8, lr, sp, pc, pstate (libnx ThreadExceptionFrame).
 * Use words here because libnx exposes the full dump, not the kernel frame. */
__attribute__((target("general-regs-only")))
bool etd2x_inline_syscall_route(uint64_t *frame) {
    const uintptr_t pc = frame[11], sp = frame[10];
    if ((pc & 3u) || (sp & 15u) || sp < 816u ||
        !inline_syscall_inline_range(pc, 4, Perm_R | Perm_X) ||
        *(const uint32_t *)pc != ETD2X_LINUX_SYSCALL_BRK ||
        !inline_syscall_inline_range(sp - 816u, 816u, Perm_R | Perm_W)) return false;
    uint64_t *header = (uint64_t *)(sp - 16u);
    header[0] = pc + 4u;
    header[1] = frame[12]; /* original pstate, including NZCV */
    frame[10] = sp - 16u;
    frame[11] = (uintptr_t)etd2x_inline_syscall_entry;
    return true;
}

#endif



/* ---- perf account ---- */


#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __SWITCH__
#include <switch.h>
#include <reent.h>
#include <sys/lock.h>
#endif

static _Thread_local uint64_t perf_account_t_perf_us[ETD2X_PERF_KIND_COUNT];
static _Thread_local unsigned perf_account_t_perf_depth[ETD2X_PERF_KIND_COUNT];

uint64_t etd2x_perf_now_us(void) {
#ifdef __SWITCH__
    return armTicksToNs(armGetSystemTick()) / UINT64_C(1000);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000) + (uint64_t)ts.tv_nsec / UINT64_C(1000);
#endif
}

/* begin_us == 0 marks an inner scope of a kind that is already being timed. */
Etd2xPerfScope etd2x_perf_scope_begin(int kind) {
    Etd2xPerfScope scope = {kind, 0};
    if (kind < 0 || kind >= ETD2X_PERF_KIND_COUNT) { scope.kind = -1; return scope; }
    if (perf_account_t_perf_depth[kind]++ == 0) scope.begin_us = etd2x_perf_now_us();
    return scope;
}

void etd2x_perf_scope_end(Etd2xPerfScope *scope) {
    if (!scope || scope->kind < 0) return;
    if (perf_account_t_perf_depth[scope->kind]) perf_account_t_perf_depth[scope->kind]--;
    if (!scope->begin_us) return;
    const uint64_t now = etd2x_perf_now_us();
    perf_account_t_perf_us[scope->kind] += now > scope->begin_us ? now - scope->begin_us : 0;
}

void etd2x_perf_account_read(uint64_t out[ETD2X_PERF_KIND_COUNT]) {
    if (out) memcpy(out, perf_account_t_perf_us, sizeof(perf_account_t_perf_us));
}

/* ---- lock contention ---------------------------------------------------- */

static Etd2xPerfLockSite *perf_account_g_lock_sites; /* lock-free push-only list */

static void perf_account_lock_site_charge(Etd2xPerfLockSite *site, uint64_t waited_us) {
    if (!site) return;
    if (!__atomic_exchange_n(&site->registered, 1, __ATOMIC_ACQ_REL)) {
        Etd2xPerfLockSite *head = __atomic_load_n(&perf_account_g_lock_sites, __ATOMIC_ACQUIRE);
        do site->next = head;
        while (!__atomic_compare_exchange_n(&perf_account_g_lock_sites, &head, site, true,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    }
    __atomic_add_fetch(&site->wait_us, waited_us, __ATOMIC_RELAXED);
    __atomic_add_fetch(&site->contended, 1, __ATOMIC_RELAXED);
    perf_account_t_perf_us[ETD2X_PERF_HOST_LOCK] += waited_us;
}

void etd2x_perf_lock_contended(pthread_mutex_t *mutex, Etd2xPerfLockSite *site) {
    const uint64_t begin = etd2x_perf_now_us();
    pthread_mutex_lock(mutex);
    const uint64_t end = etd2x_perf_now_us();
    perf_account_lock_site_charge(site, end > begin ? end - begin : 0);
}

static int perf_account_lock_delta_cmp(const void *a, const void *b) {
    const Etd2xPerfLockDelta *x = a, *y = b;
    return x->wait_us < y->wait_us ? 1 : x->wait_us > y->wait_us ? -1 : 0;
}

/* Single consumer (the sampling profiler). */
size_t etd2x_perf_lock_take_deltas(Etd2xPerfLockDelta *out, size_t cap) {
    if (!out || !cap) return 0;
    size_t n = 0;
    for (Etd2xPerfLockSite *s = __atomic_load_n(&perf_account_g_lock_sites, __ATOMIC_ACQUIRE); s; s = s->next) {
        const uint64_t us = __atomic_load_n(&s->wait_us, __ATOMIC_RELAXED);
        const uint64_t cnt = __atomic_load_n(&s->contended, __ATOMIC_RELAXED);
        Etd2xPerfLockDelta d = {s, us - s->reported_us, cnt - s->reported_n};
        s->reported_us = us;
        s->reported_n = cnt;
        if (!d.wait_us) continue;
        if (n < cap) { out[n++] = d; continue; }
        /* Keep the largest `cap`: replace the current smallest when larger. */
        size_t min_i = 0;
        for (size_t i = 1; i < n; ++i) if (out[i].wait_us < out[min_i].wait_us) min_i = i;
        if (d.wait_us > out[min_i].wait_us) out[min_i] = d;
    }
    qsort(out, n, sizeof(*out), perf_account_lock_delta_cmp);
    return n;
}

#ifdef __SWITCH__
/* newlib serializes malloc/free/realloc behind __malloc_lock. Providing the
 * pair here (instead of libc.a's mlock.o) keeps the same recursive lock
 * semantics and charges contended acquisitions like any other runtime lock.
 * The uncontended path touches no TLS, so it is safe during early startup. */
static _LOCK_RECURSIVE_T perf_account_g_malloc_lock = __LOCK_INITIALIZER_RECURSIVE;
static Etd2xPerfLockSite perf_account_g_malloc_site = {"newlib", 0, "malloc", 0, 0, 0, 0, NULL, 0};

void __malloc_lock(struct _reent *reent) {
    (void)reent;
    if (__libc_lock_try_acquire_recursive(&perf_account_g_malloc_lock) == 0) return;
    /* Hold times are a few microseconds. Spin briefly before blocking: a
     * kernel-arbitrated handoff hands ownership to a waiter that must first
     * be scheduled, which serializes every allocating thread behind it. */
    for (unsigned spin = 0; spin < 200u; ++spin) {
        __asm__ volatile("yield" ::: "memory");
        if (__libc_lock_try_acquire_recursive(&perf_account_g_malloc_lock) == 0) return;
    }
    const uint64_t begin = etd2x_perf_now_us();
    __libc_lock_acquire_recursive(&perf_account_g_malloc_lock);
    const uint64_t end = etd2x_perf_now_us();
    perf_account_lock_site_charge(&perf_account_g_malloc_site, end > begin ? end - begin : 0);
}

void __malloc_unlock(struct _reent *reent) {
    (void)reent;
    __libc_lock_release_recursive(&perf_account_g_malloc_lock);
}
#endif

