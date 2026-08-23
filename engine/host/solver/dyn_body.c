/* solver/dyn_body.c — the shared immutable source text of a queued program. See dyn_body.h for what it is for
   and why it is a C allocation rather than a JS string. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "solver/dyn_body.h"

struct DynBody {
    long   refs;   /* holders; the text is freed with the last one */
    size_t len;    /* strlen(text), kept so the census does not walk every byte of every bundle */
    char  *text;   /* NUL-terminated, never written through */
};

/* THE INSTANCE'S TOTAL, kept incrementally rather than walked, because the walk that would answer it is the
   per-flow one this file exists to stop paying: there is no list of live bodies and there must not be one —
   a registry would be a second owner of every program in the engine. */
static long g_dyn_body_bytes;
static long g_dyn_body_live;

static DynBody *dyn_body_wrap(char *text, size_t len)
{
    DynBody *b = (DynBody *)malloc(sizeof(DynBody));

    if (!b) { free(text); return NULL; }
    b->refs = 1;
    b->len = len;
    b->text = text;
    g_dyn_body_bytes += (long)len + 1;
    g_dyn_body_live++;
    return b;
}

DynBody *dyn_body_new(const char *text)
{
    size_t len;
    char *copy;

    DCHECK(text != NULL,
           "a program was queued with no source text — every row of a flow's sequence is a program or the "
           "ADDRESS of one, and both are strings, so a NULL here is a caller that has neither");
    len = strlen(text);
    copy = (char *)malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, text, len + 1);
    return dyn_body_wrap(copy, len);
}

DynBody *dyn_body_adopt(char *text, size_t len)
{
    DCHECK(text != NULL,
           "a program's source text was adopted from nothing — the decode that produces one answers a "
           "malloc'd buffer or fails, and a failure is CHECKed at the decode rather than handed on");
    /* THE LENGTH AND THE TERMINATOR ARE ONE FACT, asserted where the buffer is taken over rather than trusted
       from wherever it was decoded. Every reader compiles this text as a NUL-terminated program while the
       census reports `len` bytes for it, so a pair that disagrees runs one program and measures another —
       and a source that decoded a U+0000 runs truncated at it with the rest of the bundle simply absent,
       which is the same thing engine.c's REPLY_SOURCE_WHOLE says at the decode. Both ends, one invariant. */
    DCHECK(strlen(text) == len,
           "a program's source text was adopted with a length that is not its NUL-terminated length — the "
           "queue hands every body to the compiler as a NUL-terminated string and reports `len` bytes for it, "
           "so the program that runs and the program that is measured are different lengths of the same buffer");
    return dyn_body_wrap(text, len);
}

DynBody *dyn_body_ref(DynBody *b)
{
    DCHECK(b != NULL,
           "a reference was taken on no program text — a row of a flow's sequence always holds one, so a fork "
           "or a queue reaching here with NULL is inheriting a row that was never filled");
    DCHECK(b->refs > 0,
           "a reference was taken on a program text whose last holder has already released it — the buffer is "
           "freed with that release, so this reference names memory the allocator has given away");
    b->refs++;
    return b;
}

void dyn_body_unref(DynBody *b)
{
    DCHECK(b != NULL,
           "a reference was released on no program text — every row of a flow's sequence holds one from the "
           "moment it is queued, so a free walk reaching NULL is walking a row that was never filled");
    DCHECK(b->refs > 0,
           "a program text was released more times than it was referenced — the row that holds it takes one "
           "reference and gives one back, so a second release here is a second owner of one buffer");
    if (--b->refs > 0) return;
    g_dyn_body_bytes -= (long)b->len + 1;
    g_dyn_body_live--;
    DCHECK(g_dyn_body_bytes >= 0 && g_dyn_body_live >= 0,
           "the program-text census went negative — every body adds its bytes once when it is made and "
           "removes them once when its last holder releases it, so a negative total is a body freed by "
           "something that is not this file");
    free(b->text);
    free(b);
}

const char *dyn_body_text(const DynBody *b)
{
    DCHECK(b != NULL,
           "a program's source text was read off no body — the cursor only ever names a row that was queued, "
           "and a queued row holds one");
    DCHECK(b->refs > 0,
           "a program's source text was read after its last holder released it — the buffer is freed with "
           "that release, so this read is of memory the allocator has given away");
    return b->text;
}

size_t dyn_body_len(const DynBody *b)
{
    DCHECK(b != NULL, "a program's length was asked of no body");
    DCHECK(b->refs > 0, "a program's length was asked after its last holder released it");
    return b->len;
}

long dyn_body_total_bytes(void) { return g_dyn_body_bytes; }
long dyn_body_live_count(void)  { return g_dyn_body_live; }
