/* JSAtom support */

#define JS_ATOM_TAG_INT (1U << 31)
#define JS_ATOM_MAX_INT (JS_ATOM_TAG_INT - 1)
#define JS_ATOM_MAX     ((1U << 30) - 1)

/* return the max count from the hash size */
#define JS_ATOM_COUNT_RESIZE(n) ((n) * 2)

static inline bool __JS_AtomIsConst(JSAtom v)
{
    return (int32_t)v < JS_ATOM_END;
}

static inline bool __JS_AtomIsTaggedInt(JSAtom v)
{
    return (v & JS_ATOM_TAG_INT) != 0;
}

static inline JSAtom __JS_AtomFromUInt32(uint32_t v)
{
    return v | JS_ATOM_TAG_INT;
}

static inline uint32_t __JS_AtomToUInt32(JSAtom atom)
{
    return atom & ~JS_ATOM_TAG_INT;
}

static inline int is_num(int c)
{
    return c >= '0' && c <= '9';
}

/* return true if the string is a number n with 0 <= n <= 2^32-1 */
static inline bool is_num_string(uint32_t *pval, JSString *p)
{
    uint32_t n;
    uint64_t n64;
    int c, i, len;

    len = p->len;
    if (len == 0 || len > 10)
        return false;
    c = string_get(p, 0);
    if (is_num(c)) {
        if (c == '0') {
            if (len != 1)
                return false;
            n = 0;
        } else {
            n = c - '0';
            for(i = 1; i < len; i++) {
                c = string_get(p, i);
                if (!is_num(c))
                    return false;
                n64 = (uint64_t)n * 10 + (c - '0');
                if ((n64 >> 32) != 0)
                    return false;
                n = n64;
            }
        }
        *pval = n;
        return true;
    } else {
        return false;
    }
}

/* XXX: could use faster version ? */
static inline uint32_t hash_string8(const uint8_t *str, size_t len, uint32_t h)
{
    size_t i;

    for(i = 0; i < len; i++)
        h = h * 263 + str[i];
    return h;
}

static inline uint32_t hash_string16(const uint16_t *str,
                                     size_t len, uint32_t h)
{
    size_t i;

    for(i = 0; i < len; i++)
        h = h * 263 + str[i];
    return h;
}

static uint32_t hash_string(JSString *str, uint32_t h)
{
    if (str->is_wide_char)
        h = hash_string16(str16(str), str->len, h);
    else
        h = hash_string8(str8(str), str->len, h);
    return h;
}

/* ONE SLOT MORE THAN THE CAP, because the rebalance walks the one rope that exceeds it: js_new_string_rope
   rebalances exactly when the new node's depth reaches JS_STRING_ROPE_MAX_DEPTH + 1, so the tree handed to the
   walk is that deep. The stack holds at most one entry per level of the left spine, so depth slots is the
   requirement and the DCHECK below is that invariant, not a bound on the input. */
typedef struct {
    JSValueConst stack[JS_STRING_ROPE_MAX_DEPTH + 1];
    int stack_len;
} JSStringRopeIter;

static void string_rope_iter_init(JSStringRopeIter *s, JSValueConst val)
{
    s->stack_len = 0;
    s->stack[s->stack_len++] = val;
}

/* iterate thru a rope and return the strings in order */
static JSString *string_rope_iter_next(JSStringRopeIter *s)
{
    JSValueConst val;
    JSStringRope *r;

    if (s->stack_len == 0)
        return NULL;
    val = s->stack[--s->stack_len];
    for(;;) {
        if (JS_VALUE_GET_TAG(val) == JS_TAG_STRING)
            return JS_VALUE_GET_STRING(val);
        r = JS_VALUE_GET_STRING_ROPE(val);
        DCHECK(s->stack_len < JS_STRING_ROPE_MAX_DEPTH + 1,
               "a rope deeper than js_new_string_rope's rebalance point reached the leaf walk");
        s->stack[s->stack_len++] = r->right;
        val = r->left;
    }
}

/* Uses the rope ITERATOR that already exists rather than a second walker: string_rope_iter_next yields the
   leaves left to right, which is exactly the order this recursion visited them in, and the iterator's own
   stack is the one the tree needs. The recursion was a duplicate traversal of a structure that already had a
   flat one — the fix was to notice that, not to write a third. */
static uint32_t hash_string_rope(JSValueConst val, uint32_t h)
{
    JSStringRopeIter it;
    JSString *p;

    if (JS_VALUE_GET_TAG(val) == JS_TAG_STRING)
        return hash_string(JS_VALUE_GET_STRING(val), h);
    string_rope_iter_init(&it, val);
    while ((p = string_rope_iter_next(&it)) != NULL)
        h = hash_string(p, h);
    return h;
}

static __maybe_unused void JS_DumpString(JSRuntime *rt, JSString *p)
{
    int i, c, sep;

    if (p == NULL) {
        printf("<null>");
        return;
    }
    /* THE REFCOUNT IS LABELLED, because unlabelled it is indistinguishable from the string. It printed as a
       bare number butted against the opening quote, so a leak report reading `name: 22""` says "the property
       `name` holds an empty string that 22 things share" and reads as "a name 22 characters long" — a length,
       which is the one other number anybody would expect there. It cost a careful reader a sentence of doubt
       in exactly the report that is supposed to remove doubt. And the ambiguity is not only in the reader:
       a string whose CONTENT begins with a digit is written with the same bytes as a refcount prefix, so
       `3"7 items"` and a shared `"37 items"` are the same six characters. `rc=` is two characters and the
       whole of the fix; the `L` below is already a labelled flag for the same reason. */
    if (JS_REF_COUNT(p) != 1)
        printf("rc=%d", JS_REF_COUNT(p));
    if (p->is_wide_char)
        putchar('L');
    sep = '\"';
    putchar(sep);
    for(i = 0; i < p->len; i++) {
        c = string_get(p, i);
        if (c == sep || c == '\\') {
            putchar('\\');
            putchar(c);
        } else if (c >= ' ' && c <= 126) {
            putchar(c);
        } else if (c == '\n') {
            putchar('\\');
            putchar('n');
        } else {
            printf("\\u%04x", c);
        }
    }
    putchar(sep);
}

static __maybe_unused void JS_DumpAtoms(JSRuntime *rt)
{
    JSAtomStruct *p;
    int h, i;
    /* This only dumps hashed atoms, not JS_ATOM_TYPE_SYMBOL atoms */
    printf("JSAtom count=%d size=%d hash_size=%d:\n",
           rt->atom_count, rt->atom_size, rt->atom_hash_size);
    printf("JSAtom hash table: {\n");
    for(i = 0; i < rt->atom_hash_size; i++) {
        h = rt->atom_hash[i];
        if (h) {
            printf("  %d:", i);
            while (h) {
                p = rt->atom_array[h];
                printf(" ");
                JS_DumpString(rt, p);
                h = p->hash_next;
            }
            printf("\n");
        }
    }
    printf("}\n");
    printf("JSAtom table: {\n");
    for(i = 0; i < rt->atom_size; i++) {
        p = rt->atom_array[i];
        if (!atom_is_free(p)) {
            printf("  %d: { %d %08x ", i, p->atom_type, p->hash);
            if (!(p->len == 0 && p->is_wide_char != 0))
                JS_DumpString(rt, p);
            printf(" %d }\n", p->hash_next);
        }
    }
    printf("}\n");
}

static int JS_ResizeAtomHash(JSRuntime *rt, int new_hash_size)
{
    JSAtomStruct *p;
    uint32_t new_hash_mask, h, i, hash_next1, j, *new_hash;

    DCHECK((new_hash_size & (new_hash_size - 1)) == 0, "(new_hash_size & (new_hash_size - 1)) == 0"); /* power of two */
    new_hash_mask = new_hash_size - 1;
    new_hash = js_mallocz_rt(rt, sizeof(rt->atom_hash[0]) * new_hash_size);
    if (!new_hash)
        return -1;
    for(i = 0; i < rt->atom_hash_size; i++) {
        h = rt->atom_hash[i];
        while (h != 0) {
            p = rt->atom_array[h];
            hash_next1 = p->hash_next;
            /* add in new hash table */
            j = p->hash & new_hash_mask;
            p->hash_next = new_hash[j];
            new_hash[j] = h;
            h = hash_next1;
        }
    }
    js_free_rt(rt, rt->atom_hash);
    rt->atom_hash = new_hash;
    rt->atom_hash_size = new_hash_size;
    rt->atom_count_resize = JS_ATOM_COUNT_RESIZE(new_hash_size);
    //    JS_DumpAtoms(rt);
    return 0;
}

static int JS_InitAtoms(JSRuntime *rt)
{
    int i, len, atom_type;
    const char *p;

    rt->atom_hash_size = 0;
    rt->atom_hash = NULL;
    rt->atom_count = 0;
    rt->atom_size = 0;
    rt->atom_free_index = 0;
    if (JS_ResizeAtomHash(rt, 512))     /* there are at least 504 predefined atoms */
        return -1;

    p = js_atom_init;
    for(i = 1; i < JS_ATOM_END; i++) {
        if (i == JS_ATOM_Private_brand)
            atom_type = JS_ATOM_TYPE_PRIVATE;
        else if (i >= JS_ATOM_Symbol_toPrimitive)
            atom_type = JS_ATOM_TYPE_SYMBOL;
        else
            atom_type = JS_ATOM_TYPE_STRING;
        len = strlen(p);
        if (__JS_NewAtomInit(rt, p, len, atom_type) == JS_ATOM_NULL)
            return -1;
        p = p + len + 1;
    }
    /* THE BASELINE THE TEARDOWN CENSUS COMPARES AGAINST, ASSERTED WHERE IT IS ESTABLISHED. JS_FreeRuntime's
       atom walk judges a BUILT-IN name by `refcount == 1`, and that constant is correct only because every
       atom-API path is const-guarded — __JS_NewAtom does not increment when the name it found is below
       JS_ATOM_END, and JS_DupAtom / JS_DupAtomRT / JS_FreeAtom / JS_FreeAtomRT all return immediately — so
       nothing that happens after this function can move it. What CAN break it is a change HERE: one init-time
       dup of a predefined atom and that name is reported as leaked by every teardown this engine ever performs,
       with the report naming a healthy atom and nothing to say so. Asserting it at the origin is one pass over
       ~500 entries, once per runtime, and it turns the census's constant from an assumption into a fact — which
       is also the answer to "is the baseline uniform across the atom kinds": it is, and this is what says so.
       Index 0 is included deliberately: __JS_NewAtom's growth path builds the JS_ATOM_NULL entry at refcount 1
       and calls it "not refcounted", which is the same statement this makes about every entry beside it.
       Dev-only because the message is composed at the failure, and a message nobody reads is not worth a
       snprintf on the init path of every runtime in a release build. */
#if APICLIENT_DEV
    for(i = 0; i < JS_ATOM_END; i++) {
        if (JS_REF_COUNT(rt->atom_array[i]) != 1) {
            char abuf[ATOM_GET_STR_BUF_SIZE], why[320];

            snprintf(why, sizeof(why),
                     "the predefined atom `%s` (id %d) left JS_InitAtoms at refcount %d rather than 1 — "
                     "JS_FreeRuntime's atom census judges every built-in name against that baseline, so this "
                     "one is now reported as a leak on every teardown for the rest of this engine's life",
                     JS_AtomGetStrRT(rt, abuf, sizeof(abuf), i), i, JS_REF_COUNT(rt->atom_array[i]));
            DFAIL(why);
        }
    }
#endif
    return 0;
}

#if APICLIENT_DEV
/* WHO TOOK A REFERENCE ON ONE KEY, CENSUSED AT THE ACT. js_define_prop_borrowed_key_at's ledger measures a
 * TOTAL — an atom's refcount before and after a define — and a total cannot name a taker, so an abort it
 * raises prints a magnitude and leaves the next reader to guess which owner the budget forgot. That guess is
 * what this partitions away.
 *
 * THE PARTITION IS OVER THE DOORS AND NOT OVER A LIST OF OWNERS, because a list of owners is a claim about a
 * call graph and goes wrong the way every hand-kept list here goes wrong. An atom's refcount word can be
 * raised through exactly two kinds of door: the ATOM API (JS_DupAtom, JS_DupAtomRT, the intern that finds a
 * name already in the table, and __JS_FreeAtom on the way back down), and a JSValue DUP of the atom's own
 * JSString — the two share one ref_count word, JSAtomStruct BEING JSString. Every atom-API door credits here.
 * The value door is credited where a data property's value is stored or replaced (KEY_OWNER_VALUE, at
 * JS_CreateProperty and JS_DefineProperty's data arm), not in js_dup, the hottest function in the engine.
 *
 * AN OWNER TAG NAMES THE SITE WITHOUT NAMING IT AT THE DOOR. The door cannot see its caller, so the three
 * shape-level owners a define can reach — add_shape_property's dup of the key it is installing, js_clone_shape's
 * dup of every key it copies, js_free_shape0's release of every key it drops — set the tag around their own
 * act and restore it after. An atom-API take under no tag is credited to KEY_OWNER_ATOM_API, which is a real
 * finding rather than a default: it says the reference went through the atom API from somewhere this
 * instrument does not name, and the next diff tags that site rather than widening anything.
 *
 * THE CAPTURE IS MEASURED AS A WINDOW AND ITS DOORS ARE SUPPRESSED INSIDE IT, so the two accounts cannot both
 * claim the same reference: the host's delta entry dups the key through the ordinary atom API, so a window
 * around the hook and an unsuppressed door would each count it and the parts would sum to more than the total.
 * The suppression is a depth rather than a flag because the hook may re-enter the engine.
 *
 * DEV-ONLY AND ARMED, so the cost in a dev build is one thread-local load and one compare per atom dup, and
 * nothing at all in release. The armed atom is set only inside js_define_prop_borrowed_key_at, and every credit
 * sits inside a caller's existing `!__JS_AtomIsConst` guard — a const atom maintains no refcount, so it can
 * never equal a non-const armed key and never needs the branch. */
typedef enum {
    KEY_OWNER_ATOM_API,     /* through the atom API, from a site this instrument does not tag */
    KEY_OWNER_SHAPE_ADD,    /* add_shape_property's dup of the key it installs */
    KEY_OWNER_SHAPE_CLONE,  /* js_clone_shape's dup of every key it copies */
    KEY_OWNER_SHAPE_FREE,   /* js_free_shape0's release of every key it drops (a DEBIT) */
    KEY_OWNER_CAPTURE,      /* the time-travel capture hook, measured as a window */
    KEY_OWNER_VALUE,        /* a data property's value that IS the key's own JSString, stored (+) or replaced (-) */
    KEY_OWNER_COUNT
} JSKeyLedgerOwner;

static const char *const js_key_ledger_owner_name[KEY_OWNER_COUNT] = {
    "an untagged atom-API caller",
    "add_shape_property",
    "js_clone_shape",
    "js_free_shape0",
    "the time-travel capture",
    "a property value store",
};

static _Thread_local JSAtom g_key_ledger_atom = JS_ATOM_NULL;
static _Thread_local int g_key_ledger_by[KEY_OWNER_COUNT];
static _Thread_local JSKeyLedgerOwner g_key_ledger_owner = KEY_OWNER_ATOM_API;
static _Thread_local int g_key_ledger_in_capture;

static inline void js_key_ledger_atom_api(JSAtom v, int n)
{
    if (unlikely(v == g_key_ledger_atom) && !g_key_ledger_in_capture)
        g_key_ledger_by[g_key_ledger_owner] += n;
}

/* The value door: `v` stored into (n = 1) or dropped from (n = -1) a data property's slot is a reference on the
   armed key's own word when `v` is that key's JSString. */
static inline void js_key_ledger_value(JSContext *ctx, JSValueConst v, int n)
{
    DCHECK(g_key_ledger_atom == JS_ATOM_NULL || !__JS_AtomIsConst(g_key_ledger_atom),
           "the key ledger is armed over a constant or tagged-integer atom, which has no atom_array entry to "
           "compare against and no refcount to move");
    if (unlikely(g_key_ledger_atom != JS_ATOM_NULL) && !g_key_ledger_in_capture &&
        JS_VALUE_GET_TAG(v) == JS_TAG_STRING &&
        JS_VALUE_GET_STRING(v) == ctx->rt->atom_array[g_key_ledger_atom])
        g_key_ledger_by[KEY_OWNER_VALUE] += n;
}
#define JS_KEY_LEDGER_VALUE(ctx, v, n) js_key_ledger_value((ctx), (v), (n))

/* Declares a variable, so one push per block. Expands to nothing in release — the enum does not exist there. */
#define JS_KEY_LEDGER_OWNER_PUSH(who)                                   \
    JSKeyLedgerOwner key_ledger_outer_owner_ = g_key_ledger_owner;      \
    g_key_ledger_owner = (who)
#define JS_KEY_LEDGER_OWNER_POP() g_key_ledger_owner = key_ledger_outer_owner_
#else
#define JS_KEY_LEDGER_OWNER_PUSH(who) ((void)0)
#define JS_KEY_LEDGER_OWNER_POP()     ((void)0)
#define JS_KEY_LEDGER_VALUE(ctx, v, n) ((void)0)
#endif

JSAtom JS_DupAtomRT(JSRuntime *rt, JSAtom v)
{
    JSAtomStruct *p;

    if (!__JS_AtomIsConst(v)) {
        p = rt->atom_array[v];
        JS_REF_COUNT(p)++;
#if APICLIENT_DEV
        js_key_ledger_atom_api(v, 1);
#endif
    }
    return v;
}

JSAtom JS_DupAtom(JSContext *ctx, JSAtom v)
{
    JSRuntime *rt;
    JSAtomStruct *p;

    if (!__JS_AtomIsConst(v)) {
        rt = ctx->rt;
        p = rt->atom_array[v];
        JS_REF_COUNT(p)++;
#if APICLIENT_DEV
        js_key_ledger_atom_api(v, 1);
#endif
    }
    return v;
}

#if APICLIENT_DEV
/* THE ATOM'S REFCOUNT, so a component can check its OWN ledger against it. `[atomleak]` in JS_FreeRuntime is
   the only mechanism this engine has for an unmatched JS_DupAtom, and it reports at teardown — the whole run
   is over, every frame that could name the site is gone, and the number it prints is a total rather than a
   first occurrence. A caller that knows how many references an operation is supposed to take can say so where
   the operation happens instead.
   A CONST atom (a built-in name or a tagged integer) maintains no refcount at all — JS_DupAtom and JS_FreeAtom
   above return immediately for one — so a delta over it means nothing and it answers 0 at both ends of any
   window, which satisfies every ledger trivially and correctly. */
static int js_atom_refcount(JSRuntime *rt, JSAtom v)
{
    JSAtomStruct *p;

    if (__JS_AtomIsConst(v))
        return 0;
    DCHECK(v < (JSAtom)rt->atom_size, "an atom id outside the runtime's atom table");
    p = rt->atom_array[v];
    DCHECK(!atom_is_free(p), "an atom id on the free list was asked for its refcount — whatever holds this id "
                             "released the atom and went on using it");
    return JS_REF_COUNT(p);
}
#endif

static JSAtomKindEnum JS_AtomGetKind(JSContext *ctx, JSAtom v)
{
    JSRuntime *rt;
    JSAtomStruct *p;

    rt = ctx->rt;
    if (__JS_AtomIsTaggedInt(v))
        return JS_ATOM_KIND_STRING;
    p = rt->atom_array[v];
    switch(p->atom_type) {
    case JS_ATOM_TYPE_STRING:
        return JS_ATOM_KIND_STRING;
    case JS_ATOM_TYPE_GLOBAL_SYMBOL:
        return JS_ATOM_KIND_SYMBOL;
    case JS_ATOM_TYPE_SYMBOL:
        switch(p->hash) {
        case JS_ATOM_HASH_SYMBOL:
            return JS_ATOM_KIND_SYMBOL;
        case JS_ATOM_HASH_PRIVATE:
            return JS_ATOM_KIND_PRIVATE;
        default:
            abort();
        }
    default:
        abort();
    }
    return (JSAtomKindEnum){-1}; // pacify compiler
}

static JSAtom js_get_atom_index(JSRuntime *rt, JSAtomStruct *p)
{
    uint32_t i = p->hash_next;  /* atom_index */
    if (p->atom_type != JS_ATOM_TYPE_SYMBOL) {
        JSAtomStruct *p1;

        i = rt->atom_hash[p->hash & (rt->atom_hash_size - 1)];
        p1 = rt->atom_array[i];
        while (p1 != p) {
            DCHECK(i != 0, "i != 0");
            i = p1->hash_next;
            p1 = rt->atom_array[i];
        }
    }
    return i;
}

/* string case (internal). Return JS_ATOM_NULL if error. 'str' is
   freed. */
static JSAtom __JS_NewAtom(JSRuntime *rt, JSString *str, int atom_type)
{
    uint32_t h, h1, i;
    JSAtomStruct *p;
    int len;

    if (atom_type < JS_ATOM_TYPE_SYMBOL) {
        /* str is not NULL */
        if (str->atom_type == atom_type) {
            /* str is the atom, return its index */
            i = js_get_atom_index(rt, str);
            /* reduce string refcount and increase atom's unless constant */
            if (__JS_AtomIsConst(i))
                JS_REF_COUNT(str)--;
            return i;
        }
        /* try and locate an already registered atom */
        len = str->len;
        h = hash_string(str, atom_type);
        h &= JS_ATOM_HASH_MASK;
        h1 = h & (rt->atom_hash_size - 1);
        i = rt->atom_hash[h1];
        while (i != 0) {
            p = rt->atom_array[i];
            if (p->hash == h &&
                p->atom_type == atom_type &&
                p->len == len &&
                js_string_memcmp(p, str, len) == 0) {
                if (!__JS_AtomIsConst(i)) {
                    JS_REF_COUNT(p)++;
#if APICLIENT_DEV
                    js_key_ledger_atom_api(i, 1);
#endif
                }
                goto done;
            }
            i = p->hash_next;
        }
    } else {
        h1 = 0; /* avoid warning */
        if (atom_type == JS_ATOM_TYPE_SYMBOL) {
            h = JS_ATOM_HASH_SYMBOL;
        } else {
            h = JS_ATOM_HASH_PRIVATE;
            atom_type = JS_ATOM_TYPE_SYMBOL;
        }
    }

    if (rt->atom_free_index == 0) {
        /* allow new atom entries */
        uint32_t new_size, start;
        JSAtomStruct **new_array;

        /* alloc new with size progression 3/2:
           4 6 9 13 19 28 42 63 94 141 211 316 474 711 1066 1599 2398 3597 5395 8092
           preallocating space for predefined atoms (at least 504).
         */
        new_size = max_int(711, rt->atom_size * 3 / 2);
        if (new_size > JS_ATOM_MAX)
            goto fail;
        /* XXX: should use realloc2 to use slack space */
        new_array = js_realloc_rt(rt, rt->atom_array, sizeof(*new_array) * new_size);
        if (!new_array)
            goto fail;
        /* Note: the atom 0 is not used */
        start = rt->atom_size;
        if (start == 0) {
            /* JS_ATOM_NULL entry */
            p = js_mallocz_rt(rt, sizeof(JSAtomStruct));
            if (!p) {
                js_free_rt(rt, new_array);
                goto fail;
            }
            JS_REF_COUNT(p) = 1;  /* not refcounted */
            p->atom_type = JS_ATOM_TYPE_SYMBOL;
#ifdef ENABLE_DUMPS // JS_DUMP_LEAKS
            list_add_tail(&p->link, &rt->string_list);
#endif
            new_array[0] = p;
            rt->atom_count++;
            start = 1;
        }
        rt->atom_size = new_size;
        rt->atom_array = new_array;
        rt->atom_free_index = start;
        for(i = start; i < new_size; i++) {
            uint32_t next;
            if (i == (new_size - 1))
                next = 0;
            else
                next = i + 1;
            rt->atom_array[i] = atom_set_free(next);
        }
    }

    if (str) {
        if (str->atom_type == 0) {
            p = str;
            p->atom_type = atom_type;
        } else {
            p = js_malloc_rt(rt, sizeof(JSString) +
                             (str->len << str->is_wide_char) +
                             1 - str->is_wide_char);
            if (unlikely(!p))
                goto fail;
            JS_REF_COUNT(p) = 1;
            p->is_wide_char = str->is_wide_char;
            p->len = str->len;
            p->kind = JS_STRING_KIND_NORMAL;
#ifdef ENABLE_DUMPS // JS_DUMP_LEAKS
            list_add_tail(&p->link, &rt->string_list);
#endif
            memcpy(str8(p), str8(str), (str->len << str->is_wide_char) +
                   1 - str->is_wide_char);
            js_free_string(rt, str);
        }
    } else {
        p = js_malloc_rt(rt, sizeof(JSAtomStruct)); /* empty wide string */
        if (!p)
            return JS_ATOM_NULL;
        JS_REF_COUNT(p) = 1;
        p->is_wide_char = 1;    /* Hack to represent NULL as a JSString */
        p->len = 0;
        p->kind = JS_STRING_KIND_NORMAL;
#ifdef ENABLE_DUMPS // JS_DUMP_LEAKS
        list_add_tail(&p->link, &rt->string_list);
#endif
    }

    /* use an already free entry */
    i = rt->atom_free_index;
    rt->atom_free_index = atom_get_free(rt->atom_array[i]);
    rt->atom_array[i] = p;

    p->hash = h;
    p->hash_next = i;   /* atom_index */
    p->atom_type = atom_type;
    p->first_weak_ref = NULL;

    rt->atom_count++;

    if (atom_type != JS_ATOM_TYPE_SYMBOL) {
        p->hash_next = rt->atom_hash[h1];
        rt->atom_hash[h1] = i;
        if (unlikely(rt->atom_count >= rt->atom_count_resize))
            JS_ResizeAtomHash(rt, rt->atom_hash_size * 2);
    }

    //    JS_DumpAtoms(rt);
    return i;

 fail:
    i = JS_ATOM_NULL;
 done:
    if (str)
        js_free_string(rt, str);
    return i;
}

// XXX: `str` must be pure ASCII. No UTF-8 encoded strings
// XXX: `str` must not be the string representation of a small integer
static JSAtom __JS_NewAtomInit(JSRuntime *rt, const char *str, int len,
                               int atom_type)
{
    JSString *p;
    p = js_alloc_string_rt(rt, len, 0);
    if (!p)
        return JS_ATOM_NULL;
    memcpy(str8(p), str, len);
    str8(p)[len] = '\0';
    return __JS_NewAtom(rt, p, atom_type);
}

// XXX: `str` must be raw 8-bit contents. No UTF-8 encoded strings
static JSAtom __JS_FindAtom(JSRuntime *rt, const char *str, size_t len,
                            int atom_type)
{
    uint32_t h, h1, i;
    JSAtomStruct *p;

    h = hash_string8((const uint8_t *)str, len, JS_ATOM_TYPE_STRING);
    h &= JS_ATOM_HASH_MASK;
    h1 = h & (rt->atom_hash_size - 1);
    i = rt->atom_hash[h1];
    while (i != 0) {
        p = rt->atom_array[i];
        if (p->hash == h &&
            p->atom_type == JS_ATOM_TYPE_STRING &&
            p->len == len &&
            p->is_wide_char == 0 &&
            memcmp(str8(p), str, len) == 0) {
            if (!__JS_AtomIsConst(i)) {
                JS_REF_COUNT(p)++;
#if APICLIENT_DEV
                js_key_ledger_atom_api(i, 1);
#endif
            }
            return i;
        }
        i = p->hash_next;
    }
    return JS_ATOM_NULL;
}

/* The atom's own storage, once it is out of the hash table and its index is free. */
static void js_free_atom_struct_memory(JSRuntime *rt, JSAtomStruct *p)
{
#ifdef ENABLE_DUMPS // JS_DUMP_LEAKS
    list_del(&p->link);
#endif
    js_free_rt(rt, p);
    rt->atom_count--;
    DCHECK(rt->atom_count >= 0, "rt->atom_count >= 0");
}

static void JS_FreeAtomStruct(JSRuntime *rt, JSAtomStruct *p)
{
    uint32_t i = p->hash_next;  /* atom_index */
    if (p->atom_type != JS_ATOM_TYPE_SYMBOL) {
        JSAtomStruct *p0, *p1;
        uint32_t h0;

        h0 = p->hash & (rt->atom_hash_size - 1);
        i = rt->atom_hash[h0];
        p1 = rt->atom_array[i];
        if (p1 == p) {
            rt->atom_hash[h0] = p1->hash_next;
        } else {
            for(;;) {
                DCHECK(i != 0, "i != 0");
                p0 = p1;
                i = p1->hash_next;
                p1 = rt->atom_array[i];
                if (p1 == p) {
                    p0->hash_next = p1->hash_next;
                    break;
                }
            }
        }
    }
    /* insert in free atom list */
    rt->atom_array[i] = atom_set_free(rt->atom_free_index);
    rt->atom_free_index = i;
    if (unlikely(p->first_weak_ref)) {
        /* Resetting the weak references releases their values, and a value that is a symbol keying further
           weak references comes back here, so the reset is the drain's item and not a call: a chain of weak
           keys is one loop. Its storage goes after the reset. */
        if (rt->weak_reset_n == rt->weak_reset_cap) {
            int cap = rt->weak_reset_cap ? rt->weak_reset_cap * 2 : 16;
            JSAtomStruct **q = js_realloc_rt(rt, rt->weak_reset_q, sizeof(*q) * (size_t)cap);
            CHECK(q != NULL, "JS_FreeAtomStruct: OOM queueing a symbol whose weak references are to be reset");
            rt->weak_reset_q = q;
            rt->weak_reset_cap = cap;
        }
        rt->weak_reset_q[rt->weak_reset_n++] = p;
        if (rt->gc_phase == JS_GC_PHASE_NONE)
            free_zero_refcount(rt);
        return;
    }
    js_free_atom_struct_memory(rt, p);
}

static void __JS_FreeAtom(JSRuntime *rt, uint32_t i)
{
    JSAtomStruct *p;

    p = rt->atom_array[i];
#if APICLIENT_DEV
    js_key_ledger_atom_api(i, -1);
#endif
    if (--JS_REF_COUNT(p) > 0)
        return;
    JS_FreeAtomStruct(rt, p);
}

/* Warning: 'p' is freed */
static JSAtom JS_NewAtomStr(JSContext *ctx, JSString *p)
{
    JSRuntime *rt = ctx->rt;
    uint32_t n;
    if (is_num_string(&n, p)) {
        if (n <= JS_ATOM_MAX_INT) {
            js_free_string(rt, p);
            return __JS_AtomFromUInt32(n);
        }
    }
    /* XXX: should generate an exception */
    return __JS_NewAtom(rt, p, JS_ATOM_TYPE_STRING);
}

/* `str` may be pure ASCII or UTF-8 encoded */
JSAtom JS_NewAtomLen(JSContext *ctx, const char *str, size_t len)
{
    JSValue val;

    if (len == 0 || !is_digit(*str)) {
        // TODO(chqrlie): this does not work if `str` has UTF-8 encoded contents
        // bug example: `({ "\u00c3\u00a9": 1 }).\u00e9` evaluates to `1`.
        JSAtom atom = __JS_FindAtom(ctx->rt, str, len, JS_ATOM_TYPE_STRING);
        if (atom)
            return atom;
    }
    val = JS_NewStringLen(ctx, str, len);
    if (JS_IsException(val))
        return JS_ATOM_NULL;
    return JS_NewAtomStr(ctx, JS_VALUE_GET_STRING(val));
}

/* `str` may be pure ASCII or UTF-8 encoded */
JSAtom JS_NewAtom(JSContext *ctx, const char *str)
{
    return JS_NewAtomLen(ctx, str, strlen(str));
}

JSAtom JS_NewAtomUInt32(JSContext *ctx, uint32_t n)
{
    if (n <= JS_ATOM_MAX_INT) {
        return __JS_AtomFromUInt32(n);
    } else {
        char buf[16];
        size_t len = u32toa(buf, n);
        JSValue val = js_new_string8_len(ctx, buf, len);
        if (JS_IsException(val))
            return JS_ATOM_NULL;
        return __JS_NewAtom(ctx->rt, JS_VALUE_GET_STRING(val),
                            JS_ATOM_TYPE_STRING);
    }
}

static JSAtom JS_NewAtomInt64(JSContext *ctx, int64_t n)
{
    if ((uint64_t)n <= JS_ATOM_MAX_INT) {
        return __JS_AtomFromUInt32((uint32_t)n);
    } else {
        char buf[24];
        size_t len = i64toa(buf, n);
        JSValue val = js_new_string8_len(ctx, buf, len);
        if (JS_IsException(val))
            return JS_ATOM_NULL;
        return __JS_NewAtom(ctx->rt, JS_VALUE_GET_STRING(val),
                            JS_ATOM_TYPE_STRING);
    }
}

/* 'p' is freed */
static JSValue JS_NewSymbolInternal(JSContext *ctx, JSString *p, int atom_type)
{
    JSRuntime *rt = ctx->rt;
    JSAtom atom;
    atom = __JS_NewAtom(rt, p, atom_type);
    if (atom == JS_ATOM_NULL)
        return JS_ThrowOutOfMemory(ctx);
    return JS_MKPTR(JS_TAG_SYMBOL, rt->atom_array[atom]);
}

/* descr must be a non-numeric string atom */
static JSValue JS_NewSymbolFromAtom(JSContext *ctx, JSAtom descr,
                                    int atom_type)
{
    JSRuntime *rt = ctx->rt;
    JSString *p;

    DCHECK(!__JS_AtomIsTaggedInt(descr), "!__JS_AtomIsTaggedInt(descr)");
    DCHECK(descr < rt->atom_size, "descr < rt->atom_size");
    p = rt->atom_array[descr];
    js_dup(JS_MKPTR(JS_TAG_STRING, p));
    return JS_NewSymbolInternal(ctx, p, atom_type);
}

/* `description` may be pure ASCII or UTF-8 encoded */
JSValue JS_NewSymbol(JSContext *ctx, const char *description, bool is_global)
{
    if (description == NULL) {
        if (!is_global) {
            /* Local symbol without description: Symbol() */
            return JS_NewSymbolInternal(ctx, NULL, JS_ATOM_TYPE_SYMBOL);
        }
        /* Global symbol without description: Symbol.for() 
           Per ES spec, ToString(undefined) becomes "undefined" */
        description = "undefined";    
    }
    JSAtom atom = JS_NewAtom(ctx, description);
    if (atom == JS_ATOM_NULL)
        return JS_EXCEPTION;
    int atom_type =
        is_global ? JS_ATOM_TYPE_GLOBAL_SYMBOL : JS_ATOM_TYPE_SYMBOL;
    JSValue symbol = JS_NewSymbolFromAtom(ctx, atom, atom_type);
    JS_FreeAtom(ctx, atom);
    return symbol;
}

#define ATOM_GET_STR_BUF_SIZE 64

static const char *JS_AtomGetStrRT(JSRuntime *rt, char *buf, int buf_size,
                                   JSAtom atom)
{
    if (__JS_AtomIsTaggedInt(atom)) {
        snprintf(buf, buf_size, "%u", __JS_AtomToUInt32(atom));
    } else if (atom == JS_ATOM_NULL) {
        snprintf(buf, buf_size, "<null>");
    } else if (atom >= rt->atom_size) {
        DCHECK(atom < rt->atom_size, "atom < rt->atom_size");
        snprintf(buf, buf_size, "<invalid %x>", atom);
    } else {
        JSAtomStruct *p = rt->atom_array[atom];
        *buf = '\0';
        if (atom_is_free(p)) {
            snprintf(buf, buf_size, "<free %x>", atom);
        } else if (p != NULL) {
            JSString *str = p;
            if (str->is_wide_char) {
                /* encode surrogates correctly */
                utf8_encode_buf16(buf, buf_size, str16(str), str->len);
            } else {
                utf8_encode_buf8(buf, buf_size, str8(str), str->len);
            }
        }
    }
    return buf;
}

static const char *JS_AtomGetStr(JSContext *ctx, char *buf, int buf_size, JSAtom atom)
{
    return JS_AtomGetStrRT(ctx->rt, buf, buf_size, atom);
}

static JSValue __JS_AtomToValue(JSContext *ctx, JSAtom atom, bool force_string)
{
    char buf[ATOM_GET_STR_BUF_SIZE];

    if (__JS_AtomIsTaggedInt(atom)) {
        size_t len = u32toa(buf, __JS_AtomToUInt32(atom));
        return js_new_string8_len(ctx, buf, len);
    } else {
        JSRuntime *rt = ctx->rt;
        JSAtomStruct *p;
        DCHECK(atom < rt->atom_size, "atom < rt->atom_size");
        p = rt->atom_array[atom];
        if (p->atom_type == JS_ATOM_TYPE_STRING) {
            goto ret_string;
        } else if (force_string) {
            if (p->len == 0 && p->is_wide_char != 0) {
                /* no description string */
                p = rt->atom_array[JS_ATOM_empty_string];
            }
        ret_string:
            return js_dup(JS_MKPTR(JS_TAG_STRING, p));
        } else {
            return js_dup(JS_MKPTR(JS_TAG_SYMBOL, p));
        }
    }
}

JSValue JS_AtomToValue(JSContext *ctx, JSAtom atom)
{
    return __JS_AtomToValue(ctx, atom, false);
}

JSValue JS_AtomToString(JSContext *ctx, JSAtom atom)
{
    return __JS_AtomToValue(ctx, atom, true);
}

/* return true if the atom is an array index (i.e. 0 <= index <=
   2^32-2 and return its value */
static bool JS_AtomIsArrayIndex(JSContext *ctx, uint32_t *pval, JSAtom atom)
{
    if (__JS_AtomIsTaggedInt(atom)) {
        *pval = __JS_AtomToUInt32(atom);
        return true;
    } else {
        JSRuntime *rt = ctx->rt;
        JSAtomStruct *p;
        uint32_t val;

        DCHECK(atom < rt->atom_size, "atom < rt->atom_size");
        p = rt->atom_array[atom];
        if (p->atom_type == JS_ATOM_TYPE_STRING &&
            is_num_string(&val, p) && val != -1) {
            *pval = val;
            return true;
        } else {
            *pval = 0;
            return false;
        }
    }
}

/* This test must be fast if atom is not a numeric index (e.g. a
   method name). Return JS_UNDEFINED if not a numeric
   index. JS_EXCEPTION can also be returned. */
static JSValue JS_AtomIsNumericIndex1(JSContext *ctx, JSAtom atom)
{
    JSRuntime *rt = ctx->rt;
    JSAtomStruct *p1;
    JSString *p;
    int c, len, ret;
    JSValue num, str;

    if (__JS_AtomIsTaggedInt(atom))
        return js_int32(__JS_AtomToUInt32(atom));
    DCHECK(atom < rt->atom_size, "atom < rt->atom_size");
    p1 = rt->atom_array[atom];
    if (p1->atom_type != JS_ATOM_TYPE_STRING)
        return JS_UNDEFINED;
    p = p1;
    len = p->len;
    if (p->is_wide_char) {
        const uint16_t *r = str16(p), *r_end = str16(p) + len;
        if (r >= r_end)
            return JS_UNDEFINED;
        c = *r;
        if (c == '-') {
            if (r >= r_end)
                return JS_UNDEFINED;
            r++;
            c = *r;
            /* -0 case is specific */
            if (c == '0' && len == 2)
                goto minus_zero;
        }
        if (!is_num(c)) {
            /* XXX: String should be normalized, therefore 8-bit only */
            const uint16_t nfinity16[7] = { 'n', 'f', 'i', 'n', 'i', 't', 'y' };
            const uint16_t aN16[2] = { 'a', 'N' };
            if (!(c =='I' && (r_end - r) == 8 &&
                  !memcmp(r + 1, nfinity16, sizeof(nfinity16))) &&
                !(c =='N' && (r_end - r) == 3 &&
                  !memcmp(r + 1, aN16, sizeof(aN16))))
                return JS_UNDEFINED;
        }
    } else {
        const uint8_t *r = str8(p), *r_end = str8(p) + len;
        if (r >= r_end)
            return JS_UNDEFINED;
        c = *r;
        if (c == '-') {
            if (r >= r_end)
                return JS_UNDEFINED;
            r++;
            c = *r;
            /* -0 case is specific */
            if (c == '0' && len == 2) {
            minus_zero:
                return js_float64(-0.0);
            }
        }
        if (!is_num(c)) {
            if (!(c =='I' && (r_end - r) == 8 &&
                  !memcmp(r + 1, "nfinity", 7)) &&
                !(c =='N' && (r_end - r) == 3 &&
                  !memcmp(r + 1, "aN", 2)))
                return JS_UNDEFINED;
        }
    }
    /* This prefix test only FILTERS; the round-trip below decides. CanonicalNumericIndexString accepts exactly
       the spellings ToString(ToNumber(P)) reproduces, so besides digits and a leading '-' the non-finite
       spellings "Infinity" and "NaN" both qualify — a typed array therefore treats obj["NaN"] as an
       (always invalid) integer index, not as a normal named property. */
    /* this is ECMA CanonicalNumericIndexString primitive */
    num = JS_ToNumber(ctx, JS_MKPTR(JS_TAG_STRING, p));
    if (JS_IsException(num))
        return num;
    str = JS_ToString(ctx, num);
    if (JS_IsException(str)) {
        JS_FreeValue(ctx, num);
        return str;
    }
    ret = js_string_eq(p, JS_VALUE_GET_STRING(str));
    JS_FreeValue(ctx, str);
    if (ret) {
        return num;
    } else {
        JS_FreeValue(ctx, num);
        return JS_UNDEFINED;
    }
}

/* return -1 if exception or true/false */
static int JS_AtomIsNumericIndex(JSContext *ctx, JSAtom atom)
{
    JSValue num;
    num = JS_AtomIsNumericIndex1(ctx, atom);
    if (likely(JS_IsUndefined(num)))
        return false;
    if (JS_IsException(num))
        return -1;
    JS_FreeValue(ctx, num);
    return true;
}

void JS_FreeAtom(JSContext *ctx, JSAtom v)
{
    if (!__JS_AtomIsConst(v))
        __JS_FreeAtom(ctx->rt, v);
}

void JS_FreeAtomRT(JSRuntime *rt, JSAtom v)
{
    if (!__JS_AtomIsConst(v))
        __JS_FreeAtom(rt, v);
}

/* return true if 'v' is a symbol with a string description */
static bool JS_AtomSymbolHasDescription(JSContext *ctx, JSAtom v)
{
    JSRuntime *rt;
    JSAtomStruct *p;

    rt = ctx->rt;
    if (__JS_AtomIsTaggedInt(v))
        return false;
    p = rt->atom_array[v];
    return (((p->atom_type == JS_ATOM_TYPE_SYMBOL &&
              p->hash == JS_ATOM_HASH_SYMBOL) ||
             p->atom_type == JS_ATOM_TYPE_GLOBAL_SYMBOL) &&
            !(p->len == 0 && p->is_wide_char != 0));
}

static __maybe_unused void print_atom(JSContext *ctx, JSAtom atom)
{
    char buf[ATOM_GET_STR_BUF_SIZE];
    const char *p;
    int i;

    /* XXX: should handle embedded null characters */
    /* XXX: should move encoding code to JS_AtomGetStr */
    p = JS_AtomGetStr(ctx, buf, sizeof(buf), atom);
    for (i = 0; p[i]; i++) {
        int c = (unsigned char)p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c == '_' || c == '$') || (c >= '0' && c <= '9' && i > 0)))
            break;
    }
    if (i > 0 && p[i] == '\0') {
        printf("%s", p);
    } else {
        putchar('"');
        printf("%.*s", i, p);
        for (; p[i]; i++) {
            int c = (unsigned char)p[i];
            if (c == '\"' || c == '\\') {
                putchar('\\');
                putchar(c);
            } else if (c >= ' ' && c <= 126) {
                putchar(c);
            } else if (c == '\n') {
                putchar('\\');
                putchar('n');
            } else {
                printf("\\u%04x", c);
            }
        }
        putchar('\"');
    }
}

/* free with JS_FreeCString() */
const char *JS_AtomToCStringLen(JSContext *ctx, size_t *plen, JSAtom atom)
{
    JSValue str;
    const char *cstr;

    str = JS_AtomToString(ctx, atom);
    if (JS_IsException(str)) {
        if (plen)
            *plen = 0;
        return NULL;
    }
    cstr = JS_ToCStringLen(ctx, plen, str);
    JS_FreeValue(ctx, str);
    return cstr;
}

#ifndef QJS_DISABLE_PARSER

/* return a string atom containing name concatenated with str1 */
/* `str1` may be pure ASCII or UTF-8 encoded */
// TODO(chqrlie): use string concatenation instead of UTF-8 conversion
static JSAtom js_atom_concat_str(JSContext *ctx, JSAtom name, const char *str1)
{
    JSValue str;
    JSAtom atom;
    const char *cstr;
    char *cstr2;
    size_t len, len1;

    str = JS_AtomToString(ctx, name);
    if (JS_IsException(str))
        return JS_ATOM_NULL;
    cstr = JS_ToCStringLen(ctx, &len, str);
    if (!cstr)
        goto fail;
    len1 = strlen(str1);
    cstr2 = js_malloc(ctx, len + len1 + 1);
    if (!cstr2)
        goto fail;
    memcpy(cstr2, cstr, len);
    memcpy(cstr2 + len, str1, len1);
    cstr2[len + len1] = '\0';
    atom = JS_NewAtomLen(ctx, cstr2, len + len1);
    js_free(ctx, cstr2);
    JS_FreeCString(ctx, cstr);
    JS_FreeValue(ctx, str);
    return atom;
 fail:
    JS_FreeCString(ctx, cstr);
    JS_FreeValue(ctx, str);
    return JS_ATOM_NULL;
}

static JSAtom js_atom_concat_num(JSContext *ctx, JSAtom name, uint32_t n)
{
    char buf[16];
    size_t len;

    len = u32toa(buf, n);
    buf[len] = '\0';
    return js_atom_concat_str(ctx, name, buf);
}

#endif // QJS_DISABLE_PARSER

static inline bool JS_IsEmptyString(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_STRING && JS_VALUE_GET_STRING(v)->len == 0;
}

