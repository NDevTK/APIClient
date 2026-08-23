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

DynBody *dyn_body_new(const char *text, size_t len)
{
    char *copy;

    DCHECK(text != NULL,
           "a program was queued with no source text — every row of a flow's sequence is a program or the "
           "ADDRESS of one, and both are strings, so a NULL here is a caller that has neither");
    copy = (char *)malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, text, len);
    /* THE GUARD, WRITTEN HERE AND NOT COPIED FROM THE SOURCE. `text` is `len` bytes and this call makes no
       claim about what — if anything — follows them, so reading a terminator out of the caller's buffer would
       be a read past the range it handed over. */
    copy[len] = '\0';
    return dyn_body_wrap(copy, len);
}

DynBody *dyn_body_adopt(char *text, size_t len)
{
    DCHECK(text != NULL,
           "a program's source text was adopted from nothing — the decode that produces one answers a "
           "malloc'd buffer or fails, and a failure is CHECKed at the decode rather than handed on");
    /* THE GUARD IS ASSERTED, THE LENGTH IS NOT DERIVED FROM IT. This assertion used to read `strlen(text) ==
       len`, and that was the truncation stated as an invariant: it fired on a bundle that legitimately
       contains a U+0000 (ECMAScript §11.1 "Source Text" permits every code point from U+0000 up), which is
       what it was FOR — it named the missing length so it could be built, and the queue now carries one end to
       end. What is left to assert is the one thing a holder may still rely on: there is a NUL AT `len`, so a
       C read that walks off the end stops at the boundary this file owns rather than in the allocator's.
       BOTH SIDES, because this is where a length and a NUL-terminated read can disagree: a caller that meant
       `strlen` and passed something shorter would leave the guard byte inside its own text and the body would
       report a length whose last byte is not the program's. `text[len]` is the only byte in the buffer whose
       value this file gets to state. */
    DCHECK(text[len] == '\0',
           "a program's source text was adopted with no NUL at its stated length — the body carries (text, "
           "len) and every reader takes the pair, but the guard byte is what stops a C read that walks past "
           "the end inside this allocation instead of in the heap after it");
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
