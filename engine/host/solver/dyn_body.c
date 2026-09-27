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
    /* CENSUS ONLY, AND NEITHER OF THESE IS EVER READ TO DECIDE ANYTHING — see dyn_body.h for what they are
       for and CLAUDE.md §NO BOUNDS for why that sentence is here rather than only there. `parsed` is
       monotone: it is set by the first parse of these bytes that finishes and never cleared, so it cannot
       become a fixpoint or a seen-set gating work. `own_decode` is written once at creation from WHICH ENTRY
       made the body and is a fact about the buffer's provenance rather than about any flow. */
    char   parsed;      /* some flow of this process has finished parsing these bytes */
    char   own_decode;  /* the buffer came from `dyn_body_adopt` — one flow's own decode of a reply */
    /* THE PARSE OF `text`, AND THE KEY IT WAS COMPILED UNDER — see dyn_body.h for the whole contract. Unlike
       the two census bytes above, this IS read to decide something: whether a timeline reaching these bytes
       spends a parse on them. It decides no WORK (no program is skipped and no arm is chosen), which is the
       distinction CLAUDE.md §NO BOUNDS draws and the reason both sentences are here.
       `parse_realm` IS THE HELD/NOT-HELD PREDICATE and the two are asserted to agree at every read: a closure
       is an object, so a held parse is never JS_UNDEFINED, and the pair is written by separate statements and
       can therefore come apart. It is also the whole of the free path — see dyn_body_unref. */
    JSValue    parse;        /* JS_UNDEFINED when none is held */
    JSContext *parse_realm;  /* NULL when none is held; the realm the bytecode names and this file frees through */
    char      *parse_name;   /* the program NAME it was compiled under, owned here; NULL is a distinct key */
    int        parse_flags;  /* the eval flags it was compiled under */
};

/* THE INSTANCE'S TOTAL, kept incrementally rather than walked, because the walk that would answer it is the
   per-flow one this file exists to stop paying: there is no list of live bodies and there must not be one —
   a registry would be a second owner of every program in the engine. */
static long g_dyn_body_bytes;
static long g_dyn_body_live;
/* …AND HOW MANY OF THOSE BODIES ARE HOLDING A PARSE, WHICH IS A GAUGE AND NOT A TOTAL — see
   dyn_body_parses_held. It is kept here for g_dyn_body_live's reason exactly: there is no list of live bodies
   and there must not be one, so a walk that would answer it does not exist. */
static long g_dyn_body_parses;

/* `own_decode` IS A PARAMETER AND NOT A DEFAULT THE ENTRIES OVERWRITE, because the two entries are the two
   values and a default would make ONE of them the one a later entry silently inherits. It is the only field
   here whose value depends on which entry was used, so it is the one field this helper may not choose. */
static DynBody *dyn_body_wrap(char *text, size_t len, int own_decode)
{
    DynBody *b = (DynBody *)malloc(sizeof(DynBody));

    if (!b) { free(text); return NULL; }
    b->refs = 1;
    b->len = len;
    b->text = text;
    b->parsed = 0;
    b->own_decode = (char)(own_decode != 0);
    /* NO PARSE YET, AND THE PAIR IS WRITTEN TOGETHER at the one site that mints a body, so the predicate
       (`parse_realm`) and the value it guards cannot begin life disagreeing. */
    b->parse = JS_UNDEFINED;
    b->parse_realm = NULL;
    b->parse_name = NULL;
    b->parse_flags = 0;
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
    /* A COPY THE CALLER MADE, WHICH SEVERAL ROWS MAY REFERENCE — the seed table's body is one of these and
       every flow of the document holds it, which is exactly the population dyn_body.h's `note_parsed` answer
       has its force over. */
    return dyn_body_wrap(copy, len, /*own_decode*/0);
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
    /* ONE FLOW'S OWN DECODE OF A REPLY — both callers of this entry are a response arriving into a single
       timeline, so a sibling parked on the same row adopts a SECOND buffer over the same bytes. dyn_body.h's
       `is_own_decode` is what publishes that population, because a repeat inside it cannot be observed. */
    return dyn_body_wrap(text, len, /*own_decode*/1);
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
    /* THE HELD PARSE GOES BACK FIRST, THROUGH THE REALM RECORDED BESIDE IT — dyn_body.h states why that
       pointer cannot dangle: the bytecode holds a counted reference on its realm, so the realm outlives every
       closure compiled in it and there is a valid context to free through for exactly as long as there is
       something to free. This entry therefore takes no `JSContext` argument and none of its callers has one.
       ASSERTED BOTH WAYS, because the predicate and the value are two fields written by separate statements: a
       closure is an object, so a held parse is never JS_UNDEFINED and an unheld one is never anything else.
       Either disagreement is a reference this file would then leak or free twice. */
    DCHECK((b->parse_realm != NULL) == !JS_IsUndefined(b->parse),
           "a program text's held parse and the realm it names disagree about whether one is held — the two "
           "are written together at the mint and at the hold, so a body carrying one without the other is a "
           "closure this release either leaks or frees through nothing");
    if (b->parse_realm != NULL) {
        JS_FreeValue(b->parse_realm, b->parse);
        b->parse = JS_UNDEFINED;
        b->parse_realm = NULL;
        g_dyn_body_parses--;
        DCHECK(g_dyn_body_parses >= 0,
               "the held-parse gauge went negative — a body holds at most one parse and gives it back once, "
               "so a negative count is a release performed by something that is not this file");
    }
    free(b->parse_name);
    b->parse_name = NULL;
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

/* See dyn_body.h. THE ANSWER AND THE MARK ARE ONE CALL, which is what makes the row it feeds honest: a
   caller that could read the bit without setting it would report every later parse of these bytes as a
   first, and a caller that set it without reading would report the first parse as a repeat. */
int dyn_body_note_parsed(DynBody *b)
{
    int was;

    DCHECK(b != NULL,
           "a parse was recorded against no program text — the cursor only ever names a row that was queued, "
           "and a queued row holds one");
    DCHECK(b->refs > 0,
           "a parse was recorded against a program text whose last holder has already released it — the "
           "buffer is freed with that release, so this write is to memory the allocator has given away");
    was = b->parsed != 0;
    b->parsed = 1;
    return was;
}

int dyn_body_is_own_decode(const DynBody *b)
{
    DCHECK(b != NULL, "a program text's provenance was asked of no body");
    DCHECK(b->refs > 0, "a program text's provenance was asked after its last holder released it");
    return b->own_decode != 0;
}

/* THE KEY'S NAME HALF, WHICH IS NULL-SAFE BECAUSE A PROGRAM NAME LEGITIMATELY MAY BE NEITHER SET NOR EMPTY.
   `document_base_url` answers a document's address and `flow_dyn_url` a row's own, and neither is this file's
   to promise non-NULL — so NULL is a DISTINCT key rather than a wildcard, and a body compiled under no name
   answers only a caller that also has none. Treating it as a wildcard is the one reading that would hand a
   closure to a program whose `import()` base is not the one it was compiled against. */
static int dyn_body_name_eq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) return a == b;
    return strcmp(a, b) == 0;
}

/* See dyn_body.h. Answers 1 with `*pfn` holding ONE MORE REFERENCE the caller frees; 0 with `*pfn`
   JS_UNDEFINED, which is the caller parsing the program itself. */
int dyn_body_parse_ref(const DynBody *b, JSContext *realm, const char *name, int flags, JSValue *pfn)
{
    int agrees;

    DCHECK(b != NULL,
           "a held parse was asked of no program text — the cursor only ever names a row that was queued, and "
           "a queued row holds one");
    DCHECK(b->refs > 0,
           "a held parse was asked of a program text whose last holder has already released it — the closure "
           "went back with that release, so this read is of memory the allocator has given away");
    DCHECK(realm != NULL,
           "a held parse was asked for with no realm to compare against — the realm is half the key and "
           "JS_CallInternal takes the running realm from the BYTECODE, so a caller with none cannot say which "
           "global the program it is about to start would run against");
    DCHECK(pfn != NULL, "a held parse was asked for with nowhere to put the reference it answers");
    *pfn = JS_UNDEFINED;
    DCHECK((b->parse_realm != NULL) == !JS_IsUndefined(b->parse),
           "a program text's held parse and the realm it names disagree about whether one is held — the two "
           "are written together, so this read would answer a closure through a realm that is not its own");
    if (b->parse_realm == NULL) return 0;
    /* THE KEY, ASKED ONCE AND THEN ASSERTED AND ACTED ON, which is the shape CLAUDE.md §Offensive-programming
       prescribes for a state that is a defect HERE and must still be DEFINED in release: a disagreement is an
       engine invariant violation (dyn_body.h derives why every reference to one body names one realm, one name
       and one flag set), so dev ABORTS at it, and the release arm answers 0 — the caller parses the program
       itself, which costs one parse and is a wrong-nothing answer rather than a frame built over a foreign
       realm's closure. It is not a fallback by §C-stack's test: delete the hold side entirely and this
       question still has to be asked, because it is this entry's own precondition. */
    agrees = (b->parse_realm == realm)
             && dyn_body_name_eq(b->parse_name, name)
             && (b->parse_flags == flags);
    DCHECK(agrees,
           "a program text holds a parse compiled under a DIFFERENT realm, name or flag set than the row now "
           "asking for it — all three are baked into the bytecode (the running realm JS_CallInternal takes "
           "from it, the module-map key and import base, the inline-script and strictness flags), and every "
           "reference to one body is derived from rows of ONE document, so a disagreement is two rows sharing "
           "a program they do not agree about");
    if (!agrees) return 0;
    *pfn = JS_DupValue(realm, b->parse);
    return 1;
}

/* See dyn_body.h. Takes ONE MORE reference; the caller still owns the one it passed. */
void dyn_body_parse_hold(DynBody *b, JSContext *realm, const char *name, int flags, JSValueConst fn)
{
    char *own = NULL;

    DCHECK(b != NULL, "a parse was held against no program text");
    DCHECK(b->refs > 0,
           "a parse was held against a program text whose last holder has already released it — the release "
           "is what frees a held closure, so this reference would never be given back");
    DCHECK(realm != NULL,
           "a parse was held with no realm named — the realm is half the key AND the whole of the free path, "
           "so a body holding a closure without one is a reference nothing can release");
    DCHECK(JS_IsObject(fn),
           "a parse was held that is not a program at all — what this slot answers is instantiated directly "
           "into a preemptible frame, so a non-object here is a frame built over something with no bytecode");
    /* THE MARK AND THE HOLD ARE TWO STATEMENTS ABOUT ONE EVENT, ASSERTED ACROSS THE FILES THAT MAKE THEM. The
       compile site raises `dyn_body_note_parsed` and then holds, on adjacent lines of one straight-line block;
       a hold against an unmarked body is therefore a caller that shared a parse the repeat census does not
       know happened, which would make every later timeline's reuse of it invisible to the one row that reports
       on this mechanism. */
    DCHECK(b->parsed,
           "a parse was held against a program text no finished parse has been recorded against — the mark and "
           "the hold are raised at one event by one block, so a body holding a closure it was never marked for "
           "is a share the repeat census cannot see");
    DCHECK((b->parse_realm != NULL) == !JS_IsUndefined(b->parse),
           "a program text's held parse and the realm it names disagree about whether one is held — the two "
           "are written together, so this hold would overwrite a closure without releasing it");
    if (b->parse_realm != NULL) {
        /* ALREADY HELD, AND THE FIRST ONE STAYS — this is not a race to be asserted away. A flow may PARK
           mid-parse of a row (quickjs's JS_FlowCompileStep hands the parse back), a sibling may finish the
           same bytes while it is parked, and the parked flow then finishes its own parse and arrives here. Both
           closures are correct and interchangeable for every purpose but ONE: `entered` is a mutable bit on the
           JSFunctionBytecode and it IS the orphan identity, consumed destructively, so keeping the FIRST keeps
           that population stable rather than letting it depend on which flow was picked last. The caller still
           owns the reference it passed and frees it as it would have anyway. */
        DCHECK((b->parse_realm == realm) && dyn_body_name_eq(b->parse_name, name) && (b->parse_flags == flags),
               "a second parse of one program text was offered under a DIFFERENT realm, name or flag set than "
               "the one already held — see dyn_body_parse_ref for why every reference to one body agrees about "
               "all three");
        return;
    }
    /* THE NAME IS COPIED BEFORE ANYTHING IS TAKEN, so a failure to copy it leaves the body exactly as it was.
       A KEY THIS FILE CANNOT STATE IS A KEY IT MAY NOT COMPARE, so the parse is simply not shared: that costs
       one re-parse per later timeline and truncates nothing, because a parse is RE-DERIVABLE from the text this
       body is holding (CLAUDE.md §OOM's third category). It is not a CHECK for that reason — an allocator
       refusing a program's NAME is not a dropped flow, and aborting the instance over a memo would trade a
       recomputation for the whole document's findings. */
    if (name != NULL) {
        own = strdup(name);
        if (own == NULL) return;
    }
    b->parse = JS_DupValue(realm, fn);
    b->parse_name = own;
    b->parse_flags = flags;
    /* THE PREDICATE LAST, so that a body is never observable as holding a parse whose key is half written. */
    b->parse_realm = realm;
    g_dyn_body_parses++;
}

long dyn_body_total_bytes(void) { return g_dyn_body_bytes; }
long dyn_body_live_count(void)  { return g_dyn_body_live; }
long dyn_body_parses_held(void) { return g_dyn_body_parses; }
