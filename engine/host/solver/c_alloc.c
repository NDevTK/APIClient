/* The malloc family, replaced so the C heap's live bytes are a running total (solver/c_alloc.h). Every entry that
 * hands out or takes back a block is here, because a block allocated through an entry this file does not own and
 * freed through one it does would be subtracted without having been added. glibc supports replacing the family
 * from the executable, and its own internal callers then reach these; emscripten's dlmalloc exports the family as
 * weak aliases for the same purpose. Blocks the C library takes through the allocator's own names (glibc's internal
 * mmap, emscripten's `emscripten_builtin_*` callers) are paired within the library and stay outside the total. A
 * sanitizer owns malloc itself, so under one nothing is replaced. */
#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "solver/c_alloc.h"

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define C_ALLOC_SANITIZED 1
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(leak_sanitizer)
#ifndef C_ALLOC_SANITIZED
#define C_ALLOC_SANITIZED 1
#endif
#endif
#endif

#if defined(C_ALLOC_SANITIZED)

size_t __sanitizer_get_current_allocated_bytes(void);

size_t c_alloc_live(void) { return __sanitizer_get_current_allocated_bytes(); }

size_t c_alloc_footprint(void)
{
    struct mallinfo2 m = mallinfo2();
    return m.arena + m.hblkhd;
}

void c_alloc_census(CAllocCensus *out)
{
    struct mallinfo2 m = mallinfo2();
    DCHECK(out != NULL, "the allocator census was asked to fill nothing");
    out->arena = m.arena + m.hblkhd;
    out->residue = -1;
}

void c_alloc_span_open(void) { }

size_t c_alloc_span_take(void)
{
    DFAIL("an allocation span was measured under a sanitizer, which owns malloc, so nothing here saw the span's "
          "blocks — build the span on the sanitizer's allocator hooks to run the RAM floor under one");
    return 0;
}

#else

/* The allocator underneath, by the names each C library exports for a replacement to forward to: glibc's `__libc_*`,
   and emscripten's `emscripten_builtin_*` (its dlmalloc's own entries are static). */
#if defined(__EMSCRIPTEN__)
#include <emscripten/heap.h>
#define C_ALLOC_MALLOC(n)      emscripten_builtin_malloc(n)
#define C_ALLOC_CALLOC(k, n)   emscripten_builtin_calloc((k), (n))
#define C_ALLOC_REALLOC(p, n)  emscripten_builtin_realloc((p), (n))
#define C_ALLOC_FREE(p)        emscripten_builtin_free(p)
#define C_ALLOC_MEMALIGN(a, n) emscripten_builtin_memalign((a), (n))
#else
extern void *__libc_malloc(size_t n);
extern void *__libc_calloc(size_t k, size_t n);
extern void *__libc_realloc(void *p, size_t n);
extern void  __libc_free(void *p);
extern void *__libc_memalign(size_t align, size_t n);
#define C_ALLOC_MALLOC(n)      __libc_malloc(n)
#define C_ALLOC_CALLOC(k, n)   __libc_calloc((k), (n))
#define C_ALLOC_REALLOC(p, n)  __libc_realloc((p), (n))
#define C_ALLOC_FREE(p)        __libc_free(p)
#define C_ALLOC_MEMALIGN(a, n) __libc_memalign((a), (n))
#endif

/* The running totals in the allocator's own unit, the chunk, relaxed atomics so a thread the runtime may start
   keeps them whole. An in-use heap chunk is its usable size plus one word on both allocators (glibc's musable,
   dlmalloc's CHUNK_OVERHEAD without FOOTERS), an mmapped one its usable size plus two; emscripten's dlmalloc has no
   mmap, so its mmapped total stays 0. Their sum is what c_alloc_live answers: the bytes the allocator holds for live
   blocks, headers included, since a heap of small blocks spends a word on each. */
static size_t g_heap_chunks, g_mmap_chunks;

/* The open span (c_alloc.h): live bytes at its open, the most it has held since, and the largest refused request.
   Read and written on the engine's thread only, as every span is opened and taken there. */
static int    g_span_open;
static size_t g_span_base, g_span_peak, g_span_refused;

/* A request of `n` bytes came back NULL inside the span: the span asked for at least that much. */
static void c_alloc_span_refused(size_t n)
{
    if (g_span_open && n > g_span_refused) g_span_refused = n;
}

#if !defined(__EMSCRIPTEN__)
/* glibc's chunk header: the size word before the block, whose bit 1 marks a chunk mmapped on its own. A heap chunk
   is its usable size plus one word, an mmapped one its usable size plus two (malloc/malloc.c's musable). */
static int c_alloc_mmapped(const void *p) { return (((const size_t *)p)[-1] & 2) != 0; }

/* glibc's tcache control block, which the allocator takes for itself inside a thread's first allocation and holds in
   use for the thread's life whatever the tcache tunables say. Measured as `uordblks`' rise across the process's
   first allocation less that allocation's own chunk, so the census can name it apart. 0 before the first entry,
   1 inside it, 2 once measured. The program break before that allocation is where glibc's main arena starts, so
   the arena's extent is the break's distance from it (c_alloc_footprint). */
static int       g_first;
static size_t    g_first_before, g_tcache_block;
static uintptr_t g_brk0;
#define C_ALLOC_ENTER() \
    do { if (g_first == 0) { g_first = 1; g_first_before = mallinfo2().uordblks; g_brk0 = (uintptr_t)sbrk(0); } } \
    while (0)
#else
#define C_ALLOC_ENTER() do { } while (0)
#endif

/* The chunk `p` occupies, and which total holds it. */
static size_t c_alloc_chunk(void *p, size_t **total)
{
    size_t u = malloc_usable_size(p);
#if !defined(__EMSCRIPTEN__)
    if (c_alloc_mmapped(p)) { *total = &g_mmap_chunks; return u + 2 * sizeof(size_t); }
#endif
    *total = &g_heap_chunks;
    return u + sizeof(size_t);
}

static void c_alloc_add(void *p)
{
    size_t *total, c;
#if !defined(__EMSCRIPTEN__)
    if (g_first == 1) {
        size_t own = p && !c_alloc_mmapped(p) ? malloc_usable_size(p) + sizeof(size_t) : 0;
        g_first = 2;
        g_tcache_block = mallinfo2().uordblks - g_first_before - own;
    }
#endif
    if (!p) return;
    c = c_alloc_chunk(p, &total);
    __atomic_add_fetch(total, c, __ATOMIC_RELAXED);
    if (g_span_open) {
        size_t live = c_alloc_live();
        if (live > g_span_peak) g_span_peak = live;
    }
}

static void c_alloc_sub(void *p)
{
    size_t *total, c;
    if (!p) return;
    c = c_alloc_chunk(p, &total);
    CHECK(__atomic_load_n(total, __ATOMIC_RELAXED) >= c,
          "the C heap's running total would fall below zero — a block freed here was handed out by an entry this "
          "file does not replace, so the total no longer describes the heap and RAM pressure reads it");
    __atomic_sub_fetch(total, c, __ATOMIC_RELAXED);
}

void *malloc(size_t n)
{
    void *p;
    C_ALLOC_ENTER();
    p = C_ALLOC_MALLOC(n);
    if (!p) c_alloc_span_refused(n);
    c_alloc_add(p);
    return p;
}

void *calloc(size_t k, size_t n)
{
    void *p;
    C_ALLOC_ENTER();
    p = C_ALLOC_CALLOC(k, n);
    if (!p && n && k <= SIZE_MAX / n) c_alloc_span_refused(k * n);
    c_alloc_add(p);
    return p;
}
void free(void *p) { c_alloc_sub(p); C_ALLOC_FREE(p); }

void *realloc(void *p, size_t n)
{
    void *q;
    if (!p) return malloc(n);
    c_alloc_sub(p);
    q = C_ALLOC_REALLOC(p, n);
    /* A failed growth leaves `p` standing, so it is counted again; a zero-size realloc that freed it is not. */
    if (q) c_alloc_add(q);
    else if (n != 0) { c_alloc_span_refused(n); c_alloc_add(p); }
    return q;
}

void *reallocarray(void *p, size_t k, size_t n)
{
    if (n && k > SIZE_MAX / n) { errno = ENOMEM; return NULL; }
    return realloc(p, k * n);
}

void *memalign(size_t align, size_t n)
{
    void *p;
    C_ALLOC_ENTER();
    p = C_ALLOC_MEMALIGN(align, n);
    if (!p) c_alloc_span_refused(n);
    c_alloc_add(p);
    return p;
}
void *aligned_alloc(size_t align, size_t n) { return memalign(align, n); }

int posix_memalign(void **out, size_t align, size_t n)
{
    void *p;
    if (align < sizeof(void *) || (align & (align - 1))) return EINVAL;
    p = memalign(align, n);
    if (!p && n) return ENOMEM;
    *out = p;
    return 0;
}

void *valloc(size_t n) { return memalign(4096, n); }
void *pvalloc(size_t n) { return memalign(4096, (n + 4095) & ~(size_t)4095); }

#if defined(__EMSCRIPTEN__)
/* emscripten's dlmalloc exports no in-place resize to forward to, and its contract lets it decline any request, so
   it declines every one here rather than let its weak alias resize a block behind the total. */
void *realloc_in_place(void *p, size_t n) { (void)p; (void)n; return NULL; }
#endif

size_t c_alloc_live(void)
{
    return __atomic_load_n(&g_heap_chunks, __ATOMIC_RELAXED) + __atomic_load_n(&g_mmap_chunks, __ATOMIC_RELAXED);
}

void c_alloc_span_open(void)
{
    DCHECK(!g_span_open, "an allocation span was opened inside another — the inner open would reset the outer's "
                         "base and its caller would read a peak measured from the wrong live bytes");
    g_span_open = 1;
    g_span_base = g_span_peak = c_alloc_live();
    g_span_refused = 0;
}

size_t c_alloc_span_take(void)
{
    DCHECK(g_span_open, "an allocation span was taken that was never opened");
    g_span_open = 0;
    return (g_span_peak - g_span_base) + g_span_refused;
}

#if defined(__EMSCRIPTEN__)
size_t malloc_footprint(void);   /* emscripten's dlmalloc exports it as a weak alias of dlmalloc_footprint */
size_t c_alloc_footprint(void) { return malloc_footprint(); }
#else
size_t c_alloc_footprint(void)
{
    DCHECK(g_first == 2, "the allocator's footprint was asked for before anything was allocated through it");
    return (size_t)((uintptr_t)sbrk(0) - g_brk0) + __atomic_load_n(&g_mmap_chunks, __ATOMIC_RELAXED);
}
#endif

void c_alloc_census(CAllocCensus *out)
{
    size_t heap = __atomic_load_n(&g_heap_chunks, __ATOMIC_RELAXED);
#if defined(__EMSCRIPTEN__)
    /* dlmalloc's mallinfo walks every chunk of the heap, which a census may not cost, so the wasm build states its
       footprint and no residue. */
    DCHECK(out != NULL, "the allocator census was asked to fill nothing");
    (void)heap;
    out->arena = c_alloc_footprint();
    out->residue = -1;
#else
    struct mallinfo2 m = mallinfo2();
    size_t mmapped = __atomic_load_n(&g_mmap_chunks, __ATOMIC_RELAXED);
    const char *tun = getenv("GLIBC_TUNABLES");
    int exact = tun && strstr(tun, "glibc.malloc.tcache_count=0") != NULL;

    DCHECK(out != NULL, "the allocator census was asked to fill nothing");
    DCHECK(g_first == 2, "the allocator was asked for its census before anything was allocated through it");
    DCHECKF(mmapped == m.hblkhd,
            "the C heap's running total of mmapped chunks (%zu) is not glibc's own (%zu) — an mmapped block was "
            "handed out or taken back through an entry solver/c_alloc.c does not replace", mmapped, m.hblkhd);
    DCHECKF(heap + g_tcache_block <= m.uordblks,
            "the C heap's running total of heap chunks (%zu) and glibc's tcache control block (%zu) exceed what "
            "glibc holds in use (%zu) — a block was counted twice, or freed without being taken off the total",
            heap, g_tcache_block, m.uordblks);
    DCHECKF(!exact || heap + g_tcache_block == m.uordblks,
            "with glibc's tcache off, the C heap's running total of heap chunks (%zu) and glibc's tcache control "
            "block (%zu) are not what glibc holds in use (%zu) — a block was handed out by an entry "
            "solver/c_alloc.c does not replace, or a second thread took a control block of its own",
            heap, g_tcache_block, m.uordblks);
    DCHECKF(c_alloc_footprint() == m.arena + m.hblkhd,
            "the allocator's footprint read from the program break and the mmapped total (%zu) is not glibc's own "
            "arena plus mmapped chunks (%zu) — a second arena (a thread's) or a main arena extended by mmap when the "
            "break could not move, neither of which the break measures", c_alloc_footprint(), m.arena + m.hblkhd);
    out->arena = m.arena + m.hblkhd;
    out->residue = (long long)(m.uordblks - heap - g_tcache_block);
#endif
}

#endif
