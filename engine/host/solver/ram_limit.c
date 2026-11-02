/* The memory this instance may hold (solver/ram_limit.h). */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "solver/ram_limit.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten/heap.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

const char *ram_limit_source_json(RamLimitSource source)
{
    switch (source) {
    case RAM_LIMIT_NONE:          return "{\"none\":1}";
    case RAM_LIMIT_WASM_HEAP_MAX: return "{\"wasm-heap-max\":1}";
    case RAM_LIMIT_RLIMIT_AS:     return "{\"rlimit-as\":1}";
    case RAM_LIMIT_MEMCG:         return "{\"memcg\":1}";
    }
    DFAIL("a RAM limit source this file does not enumerate was asked for its census arm");
    return "{\"none\":1}";
}

#if defined(__EMSCRIPTEN__)

void ram_limit_read(RamLimit *out, uint64_t heap_footprint)
{
    extern unsigned char __heap_base;

    (void)heap_footprint;
    out->source = RAM_LIMIT_WASM_HEAP_MAX;
    out->bytes = (uint64_t)emscripten_get_heap_max();
    out->nonheap = (uint64_t)(uintptr_t)&__heap_base;
    CHECK(out->nonheap < out->bytes,
          "the instance's static data and stack already fill its linear memory, so no heap block could be allocated");
}

uint64_t ram_limit_cgroup_max(void)
{
    DFAIL("a memory cgroup was asked of the wasm build, whose instance is bounded by its linear memory");
    return UINT64_MAX;
}

#else

/* The memory cgroups on this process's path, innermost first: each directory and whether it is cgroup v2's
   unified hierarchy. Read once per process from /proc/self/cgroup, since a process stays in its cgroup. A level
   whose files do not exist (a hybrid mount's unified root, which has no memory controller) states no limit. */
#define RL_LEVELS_MAX 32
static struct { char dir[1100]; bool v2; } g_rl_levels[RL_LEVELS_MAX];
static int g_rl_n = -1;

static void rl_levels_read(void)
{
    char line[1024];
    FILE *cg = fopen("/proc/self/cgroup", "r");

    g_rl_n = 0;
    if (!cg) return;
    while (fgets(line, sizeof line, cg)) {
        char *c1 = strchr(line, ':'), *c2 = c1 ? strchr(c1 + 1, ':') : NULL, *nl;
        const char *root;
        bool v2;

        if (!c2) continue;
        if ((nl = strchr(c2, '\n'))) *nl = '\0';
        *c2 = '\0';
        if (strcmp(c1 + 1, "") == 0) { root = "/sys/fs/cgroup"; v2 = true; }
        else if (strstr(c1 + 1, "memory")) { root = "/sys/fs/cgroup/memory"; v2 = false; }
        else continue;
        for (char *path = c2 + 1;;) {
            CHECK(g_rl_n < RL_LEVELS_MAX, "this process's cgroup path is deeper than the levels ram_limit.c holds");
            snprintf(g_rl_levels[g_rl_n].dir, sizeof g_rl_levels[g_rl_n].dir, "%s%s", root,
                     strcmp(path, "/") == 0 ? "" : path);
            g_rl_levels[g_rl_n++].v2 = v2;
            char *slash = strrchr(path, '/');
            if (!slash || slash == path) { if (path[0] && path[1]) { path[1] = '\0'; continue; } break; }
            *slash = '\0';
        }
    }
    fclose(cg);
}

/* The first unsigned number in `dir`/`file`, or false when the file is absent or states none (v2's "max"). */
static bool rl_read_u64(const char *dir, const char *file, uint64_t *out)
{
    char fn[1200];
    unsigned long long v;
    FILE *f;
    bool ok;

    snprintf(fn, sizeof fn, "%s/%s", dir, file);
    if (!(f = fopen(fn, "r"))) return false;
    ok = fscanf(f, "%llu", &v) == 1;
    fclose(f);
    if (ok) *out = v;
    return ok;
}

/* The value of `key` in `dir`/memory.stat, or false when the file or the key is absent. */
static bool rl_read_stat(const char *dir, const char *key, uint64_t *out)
{
    char fn[1200], k[64];
    unsigned long long v;
    FILE *f;
    bool ok = false;

    snprintf(fn, sizeof fn, "%s/memory.stat", dir);
    if (!(f = fopen(fn, "r"))) return false;
    while (fscanf(f, "%63s %llu", k, &v) == 2)
        if (strcmp(k, key) == 0) { *out = v; ok = true; break; }
    fclose(f);
    return ok;
}

uint64_t ram_limit_cgroup_max(void)
{
    uint64_t best = UINT64_MAX, v;
    int i;

    if (g_rl_n < 0) rl_levels_read();
    for (i = 0; i < g_rl_n; i++)
        if (rl_read_u64(g_rl_levels[i].dir, g_rl_levels[i].v2 ? "memory.max" : "memory.limit_in_bytes", &v) &&
            v < best)
            best = v;
    return best;
}

/* The least headroom over every level that states a limit: the limit less what the level is charged and would
   not reclaim first (its usage less its inactive file cache, both hierarchical). False when no level does. */
static bool rl_cgroup_headroom(uint64_t *out)
{
    bool any = false;
    int i;

    if (g_rl_n < 0) rl_levels_read();
    for (i = 0; i < g_rl_n; i++) {
        const char *d = g_rl_levels[i].dir;
        bool v2 = g_rl_levels[i].v2;
        uint64_t limit, usage, inactive = 0, charged, room;

        if (!rl_read_u64(d, v2 ? "memory.max" : "memory.limit_in_bytes", &limit)) continue;
        CHECK(rl_read_u64(d, v2 ? "memory.current" : "memory.usage_in_bytes", &usage),
              "a memory cgroup states a limit and no usage, so its headroom cannot be read");
        (void)rl_read_stat(d, v2 ? "inactive_file" : "total_inactive_file", &inactive);
        charged = usage > inactive ? usage - inactive : 0;
        room = limit > charged ? limit - charged : 0;
        if (!any || room < *out) *out = room;
        any = true;
    }
    return any;
}

/* /proc/self/statm's field `i` (0: mapped size, 1: resident), in bytes. */
static uint64_t rl_statm(int i)
{
    unsigned long f[2] = { 0, 0 };
    FILE *statm = fopen("/proc/self/statm", "r");

    CHECK(statm && fscanf(statm, "%lu %lu", &f[0], &f[1]) == 2,
          "the native host could not read its sizes from /proc/self/statm, which RAM pressure's limit subtracts");
    fclose(statm);
    return (uint64_t)f[i] * (uint64_t)sysconf(_SC_PAGESIZE);
}

void ram_limit_read(RamLimit *out, uint64_t heap_footprint)
{
    struct rlimit rl;
    uint64_t room;

    out->source = RAM_LIMIT_NONE;
    out->bytes = 0;
    out->nonheap = 0;
    CHECK(getrlimit(RLIMIT_AS, &rl) == 0, "the native host could not read its address-space limit");
    if (rl.rlim_cur != RLIM_INFINITY) {
        uint64_t mapped = rl_statm(0);

        DCHECKF(mapped >= heap_footprint, "the process maps %llu bytes and its allocator's footprint is %llu — the "
                "footprint is the break's extent plus mmapped chunks, all of them mapped",
                (unsigned long long)mapped, (unsigned long long)heap_footprint);
        out->source = RAM_LIMIT_RLIMIT_AS;
        out->bytes = (uint64_t)rl.rlim_cur;
        out->nonheap = mapped - heap_footprint;
        CHECK(out->nonheap < out->bytes,
              "the process's non-heap mappings already fill its address-space limit, so no heap block could be "
              "allocated");
    }
    if (rl_cgroup_headroom(&room)) {
        uint64_t resident = rl_statm(1);
        /* The resident bytes outside the heap's footprint; none while part of the footprint is not yet resident,
           since then the resident size cannot be split and the footprint is charged as if it were. */
        uint64_t nonheap = resident > heap_footprint ? resident - heap_footprint : 0;

        /* The cgroup binds when it leaves the heap less room than the address space does. */
        if (out->source == RAM_LIMIT_NONE || resident + room - nonheap < out->bytes - out->nonheap) {
            out->source = RAM_LIMIT_MEMCG;
            out->bytes = resident + room;
            out->nonheap = nonheap;
        }
    }
}

#endif
