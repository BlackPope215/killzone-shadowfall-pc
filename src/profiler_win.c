/* Sampling profiler and hang reporter (Windows; GOW3_PROFILE=<report file>).
 * A thread suspends every other thread of the process every GOW3_PROFILE_MS (default 5) ms,
 * records its call stack and resumes it. Host frames unwind through the modules' .pdata;
 * guest frames (no unwind data) follow the rbp chain. Every GOW3_PROFILE_PERIOD (default 5) s
 * the report gets, per thread: CPU time used in the window, the share of samples blocked in a
 * kernel wait, the most frequent stacks and the stack seen last (what a frozen thread is
 * doing). Host frames are module+offset: tools/symbolize_profile.py names them. */
#include "profiler.h"
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include "compat_win.h"

enum { MAX_THREADS = 96, MAX_FRAMES = 16, KEY_FRAMES = 8, STACKS = 256, TOP = 6 };
typedef struct { uintptr_t frame[MAX_FRAMES]; int depth; } Stack;
typedef struct { uintptr_t key[KEY_FRAMES]; int depth; unsigned count; } StackCount;
typedef struct {
    DWORD tid;
    HANDLE handle;
    int live;
    uintptr_t stack_low, stack_high;
    char name[64];
    unsigned samples, waiting;
    uint64_t cpu_start; /* 100 ns units at the window start */
    StackCount stacks[STACKS];
    unsigned distinct, dropped;
    Stack last;
    int flat; /* 0 not decided, 1 yes, -1 no (GOW3_PROFILE_FLAT) */
    struct FlatEntry *table;
    unsigned flat_samples, flat_dropped;
} ThreadInfo;

/* GOW3_PROFILE_FLAT=<name part>[|<name part>...]: for the threads whose name contains one of
 * them, every frame address with its self samples (leaf) and inclusive samples (anywhere on the
 * stack), so the cost of a function adds up across all the stacks that reach it. */
enum { FLAT_SIZE = 16384, FLAT_PRINT = 400 };
struct FlatEntry { uintptr_t address; unsigned self, incl; };
static const char *flat_names;

static ThreadInfo threads[MAX_THREADS];
static int thread_count;
static FILE *report;
static ProfilerDescribe describe;
static uintptr_t ntdll_base, ntdll_end;

typedef LONG (NTAPI *QueryThread)(HANDLE, int, void *, ULONG, ULONG *);
typedef struct { LONG status; void *teb; uintptr_t pid, tid; uintptr_t affinity; LONG prio, base; } ThreadBasic;
typedef HRESULT (WINAPI *GetDescription)(HANDLE, PWSTR *);
static QueryThread query_thread;
static GetDescription get_description;

static uint64_t thread_cpu(HANDLE h) {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
    return ((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime);
}

static void read_name(ThreadInfo *t) {
    PWSTR wide = NULL;
    t->name[0] = 0;
    if (get_description && SUCCEEDED(get_description(t->handle, &wide)) && wide) {
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, t->name, sizeof(t->name), NULL, NULL);
        LocalFree(wide);
    }
    if (!t->name[0]) snprintf(t->name, sizeof(t->name), "tid%lu", (unsigned long)t->tid);
}

/* The TEB's NT_TIB holds the stack bounds; the rbp walk stays inside them. */
static void read_stack_bounds(ThreadInfo *t) {
    ThreadBasic basic;
    t->stack_low = t->stack_high = 0;
    if (!query_thread || query_thread(t->handle, 0, &basic, sizeof(basic), NULL) < 0 || !basic.teb) return;
    NT_TIB *tib = (NT_TIB *)basic.teb;
    t->stack_high = (uintptr_t)tib->StackBase;
    t->stack_low = (uintptr_t)tib->StackLimit;
}

/* Adds the threads that started since the last scan; marks the ones that exited. */
static void scan_threads(DWORD self) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    for (int i = 0; i < thread_count; ++i) threads[i].live = 0;
    THREADENTRY32 e = {.dwSize = sizeof(e)};
    DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &e); ok; ok = Thread32Next(snap, &e)) {
        if (e.th32OwnerProcessID != pid || e.th32ThreadID == self) continue;
        int i = 0;
        while (i < thread_count && threads[i].tid != e.th32ThreadID) ++i;
        if (i == thread_count) {
            if (thread_count == MAX_THREADS) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, e.th32ThreadID);
            if (!h) continue;
            ThreadInfo *t = &threads[thread_count++];
            memset(t, 0, sizeof(*t));
            t->tid = e.th32ThreadID;
            t->handle = h;
            t->cpu_start = thread_cpu(h);
            read_stack_bounds(t);
        }
        threads[i].live = 1;
    }
    CloseHandle(snap);
    for (int i = 0; i < thread_count; ++i)
        if (threads[i].live) read_name(&threads[i]); /* guest threads name themselves after starting */
}

/* Module ranges read outside sampling: asking the loader while a thread is suspended could
 * wait on a loader lock that thread holds. */
enum { MAX_MODULES = 256 };
static struct { uintptr_t start, end; } host_modules[MAX_MODULES];
static int host_module_count;
static void scan_modules(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32 e = {.dwSize = sizeof(e)};
    int n = 0;
    for (BOOL ok = Module32First(snap, &e); ok && n < MAX_MODULES; ok = Module32Next(snap, &e)) {
        host_modules[n].start = (uintptr_t)e.modBaseAddr;
        host_modules[n].end = (uintptr_t)e.modBaseAddr + e.modBaseSize;
        ++n;
    }
    host_module_count = n;
    CloseHandle(snap);
}
static int in_host_module(uintptr_t address) {
    for (int i = 0; i < host_module_count; ++i)
        if (address >= host_modules[i].start && address < host_modules[i].end) return 1;
    return 0;
}

/* Runs with the thread suspended: no allocation and no stdio (the thread may hold their locks). */
static void walk(const ThreadInfo *t, CONTEXT *c, Stack *s) {
    s->depth = 0;
    uintptr_t low = t->stack_low, high = t->stack_high;
    while (s->depth < MAX_FRAMES && c->Rip) {
        s->frame[s->depth++] = c->Rip;
        DWORD64 base = 0;
        int host = in_host_module(c->Rip);
        PRUNTIME_FUNCTION f = host ? RtlLookupFunctionEntry(c->Rip, &base, NULL) : NULL;
        if (f) {
            void *handler_data; DWORD64 frame;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c->Rip, f, c, &handler_data, &frame, NULL);
        } else if (host) { /* leaf function */
            if (c->Rsp < low || c->Rsp + 8 > high) break;
            c->Rip = *(DWORD64 *)c->Rsp;
            c->Rsp += 8;
        } else { /* guest code: frame pointer chain */
            uintptr_t bp = c->Rbp;
            if (bp < c->Rsp || bp < low || bp + 16 > high || (bp & 7)) break;
            c->Rip = ((DWORD64 *)bp)[1];
            c->Rbp = ((DWORD64 *)bp)[0];
            c->Rsp = bp + 16;
        }
        if (c->Rsp < low || c->Rsp > high) break;
    }
}

static int flat_wanted(ThreadInfo *t) {
    if (!flat_names || !flat_names[0]) return -1;
    if (!strncmp(t->name, "tid", 3)) return 0; /* not named yet: decide later */
    const char *p = flat_names;
    while (*p) {
        const char *end = strchr(p, '|');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n) {
            char part[64];
            if (n >= sizeof(part)) n = sizeof(part) - 1;
            memcpy(part, p, n);
            part[n] = 0;
            if (strstr(t->name, part)) return 1;
        }
        if (!end) break;
        p = end + 1;
    }
    return -1;
}

static struct FlatEntry *flat_slot(ThreadInfo *t, uintptr_t address) {
    unsigned h = (unsigned)((address * 0x9E3779B97F4A7C15ull) >> 50) & (FLAT_SIZE - 1);
    for (unsigned probe = 0; probe < 64; ++probe, h = (h + 1) & (FLAT_SIZE - 1)) {
        struct FlatEntry *e = &t->table[h];
        if (e->address == address) return e;
        if (!e->address) { e->address = address; return e; }
    }
    ++t->flat_dropped;
    return NULL;
}

static void record_flat(ThreadInfo *t, const Stack *s) {
    if (t->flat == 0) t->flat = flat_wanted(t);
    if (t->flat != 1) return;
    if (!t->table && !(t->table = calloc(FLAT_SIZE, sizeof(struct FlatEntry)))) { t->flat = -1; return; }
    ++t->flat_samples;
    for (int i = 0; i < s->depth; ++i) {
        int seen = 0;
        for (int j = 0; j < i; ++j) seen |= s->frame[j] == s->frame[i];
        if (seen) continue;
        struct FlatEntry *e = flat_slot(t, s->frame[i]);
        if (!e) continue;
        ++e->incl;
        if (i == 0) ++e->self;
    }
}

static int by_incl(const void *a, const void *b) {
    unsigned x = ((const struct FlatEntry *)a)->incl, y = ((const struct FlatEntry *)b)->incl;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void record(ThreadInfo *t, const Stack *s) {
    record_flat(t, s);
    ++t->samples;
    t->last = *s;
    if (s->depth && s->frame[0] >= ntdll_base && s->frame[0] < ntdll_end) ++t->waiting; /* syscall stubs: waits/sleeps */
    int depth = s->depth < KEY_FRAMES ? s->depth : KEY_FRAMES;
    for (unsigned i = 0; i < t->distinct; ++i) {
        StackCount *e = &t->stacks[i];
        if (e->depth == depth && !memcmp(e->key, s->frame, depth * sizeof(uintptr_t))) { ++e->count; return; }
    }
    if (t->distinct == STACKS) { ++t->dropped; return; }
    StackCount *e = &t->stacks[t->distinct++];
    memcpy(e->key, s->frame, depth * sizeof(uintptr_t));
    e->depth = depth;
    e->count = 1;
}

static void print_stack(const uintptr_t *frames, int depth) {
    char where[MAX_PATH + 96];
    for (int i = 0; i < depth; ++i) {
        describe(where, sizeof(where), frames[i]);
        fprintf(report, "        %s\n", where);
    }
}

static int by_count(const void *a, const void *b) {
    unsigned x = ((const StackCount *)a)->count, y = ((const StackCount *)b)->count;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void write_report(uint64_t window_ms, unsigned round) {
    fprintf(report, "==== window %u (%" PRIu64 " ms) ====\n", round, window_ms);
    for (int i = 0; i < thread_count; ++i) {
        ThreadInfo *t = &threads[i];
        if (!t->samples) continue;
        uint64_t cpu = thread_cpu(t->handle);
        double busy = window_ms ? (double)(cpu - t->cpu_start) / 1e4 / (double)window_ms * 100.0 : 0.0;
        t->cpu_start = cpu;
        fprintf(report, "-- thread %lu '%s': cpu %.0f%%, samples %u, waiting %.0f%%%s\n", (unsigned long)t->tid, t->name, busy,
                t->samples, 100.0 * t->waiting / t->samples, t->live ? "" : " (exited)");
        qsort(t->stacks, t->distinct, sizeof(StackCount), by_count);
        for (unsigned k = 0; k < t->distinct && k < TOP; ++k) {
            if (t->stacks[k].count * 50 < t->samples) break; /* under 2% */
            fprintf(report, "   %4.1f%%\n", 100.0 * t->stacks[k].count / t->samples);
            print_stack(t->stacks[k].key, t->stacks[k].depth);
        }
        fprintf(report, "   last:\n");
        print_stack(t->last.frame, t->last.depth);
        if (t->table && t->flat_samples) {
            qsort(t->table, FLAT_SIZE, sizeof(struct FlatEntry), by_incl);
            fprintf(report, "   flat: %u samples, %u addresses dropped\n", t->flat_samples, t->flat_dropped);
            for (unsigned k = 0; k < FLAT_SIZE && k < FLAT_PRINT && t->table[k].incl; ++k) {
                fprintf(report, "   flat self %u incl %u\n", t->table[k].self, t->table[k].incl);
                print_stack(&t->table[k].address, 1);
            }
            memset(t->table, 0, FLAT_SIZE * sizeof(struct FlatEntry));
            t->flat_samples = t->flat_dropped = 0;
        }
        t->samples = t->waiting = t->distinct = t->dropped = 0;
    }
    fflush(report);
}

static DWORD WINAPI profiler_main(void *unused) {
    (void)unused;
    const char *ms = getenv("GOW3_PROFILE_MS"), *period = getenv("GOW3_PROFILE_PERIOD");
    uint64_t interval_ns = (uint64_t)(ms ? atof(ms) * 1e6 : 5e6), period_ms = (uint64_t)(period ? atof(period) * 1000 : 5000);
    DWORD self = GetCurrentThreadId();
    uint64_t window_start = GetTickCount64(), last_scan = 0;
    for (unsigned round = 1;;) {
        compat_sleep_ns(interval_ns);
        uint64_t now = GetTickCount64();
        if (now - last_scan >= 1000) { scan_modules(); scan_threads(self); last_scan = now; }
        for (int i = 0; i < thread_count; ++i) {
            ThreadInfo *t = &threads[i];
            if (!t->live || SuspendThread(t->handle) == (DWORD)-1) continue;
            CONTEXT c = {.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER};
            Stack s = {.depth = 0};
            if (GetThreadContext(t->handle, &c)) walk(t, &c, &s);
            ResumeThread(t->handle);
            if (s.depth) record(t, &s);
        }
        if (now - window_start >= period_ms) {
            write_report(now - window_start, round++);
            window_start = now;
        }
    }
    return 0;
}

void profiler_start(ProfilerDescribe describe_address) {
    const char *path = getenv("GOW3_PROFILE");
    if (!path || !path[0]) return;
    report = fopen(path, "w");
    if (!report) { fprintf(stderr, "Profiler: cannot write %s\n", path); return; }
    describe = describe_address;
    flat_names = getenv("GOW3_PROFILE_FLAT");
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    query_thread = (QueryThread)(void *)GetProcAddress(ntdll, "NtQueryInformationThread");
    get_description = (GetDescription)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((char *)ntdll + ((IMAGE_DOS_HEADER *)ntdll)->e_lfanew);
    ntdll_base = (uintptr_t)ntdll;
    ntdll_end = ntdll_base + nt->OptionalHeader.SizeOfImage;
    HANDLE h = CreateThread(NULL, 0, profiler_main, NULL, 0, NULL);
    if (!h) return;
    SetThreadPriority(h, THREAD_PRIORITY_TIME_CRITICAL);
    CloseHandle(h);
    printf("Profiler: sampling all threads into %s\n", path);
}
#else
void profiler_start(ProfilerDescribe describe_address) { (void)describe_address; }
#endif
