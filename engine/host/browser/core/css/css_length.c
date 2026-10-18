/* CSS Values and Units §6 — the `<length>` in CSS pixels. See css_length.h for why this is a component, for
   what each unit family resolves against, and why each one this engine cannot absolutize crashes with its OWN
   message. */
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/agent_state.h"
#include "core/css/css_length.h"
#include "core/css/css_math.h"
#include "core/frame/viewport.h"
#include "core/realm.h"

static char *css_len_strdup(const char *s)
{
    char *out = strdup(s);
    CHECK(out != NULL, "cssom: OOM serializing a used length — a dropped one would read as undeclared");
    return out;
}

/* ---- the per-realm form table ------------------------------------------------------------------------------
   A form names facts of exactly one realm (css_px_combine asserts it), so the realm owns it: each realm's table
   holds every form a length of that realm was given, and the realm's teardown frees the table with it. Forms
   are immutable, so a form one flow interns is shared with its forked siblings, and a discarded flow's forms
   live until their realm is torn down: reclamation bounded by the realm's lifetime, since the forms a run makes
   are not bounded by the program's expressions (their constants are numbers the layout computes). */
/* An atom's name, interned under its key; and its definition, indexed by the name minus one. */
typedef struct { unsigned char *key; size_t len; unsigned id; } CssPxAtomSlot;
typedef struct { char *op; int nargs; CssPxAtomArg *args; } CssPxAtomDef;

typedef struct {
    CssPxForm    **slot;        /* open-addressed, a power of two long, at most half full; NULL is empty; OWNED */
    size_t         cap, n;
    CssPxAtomSlot *atoms;       /* the same shape over atom keys; each key OWNED */
    size_t         atoms_cap;
    unsigned       atoms_n;
    CssPxAtomDef  *atom_defs;   /* `atoms_n` of them, each OWNED, in storage for `atom_defs_cap` */
    size_t         atom_defs_cap;
} CssPxFormTable;

static JSClassID g_css_px_form_table_class;
static JSClassID g_css_px_form_table_slot;

/* The one form every opaque length shares: it names no fact, so no realm owns it. */
static const CssPxForm CSS_PX_FORM_OPAQUE = { false, 0, 0.0, { { 0 } } };

static void css_px_form_table_finalizer(JSRuntime *rt, JSValue v)
{
    CssPxFormTable *t = JS_GetOpaque(v, g_css_px_form_table_class);
    size_t i;

    (void)rt;
    if (t == NULL) return;
    for (i = 0; i < t->cap; i++) free(t->slot[i]);
    free(t->slot);
    for (i = 0; i < t->atoms_cap; i++) free(t->atoms[i].key);
    free(t->atoms);
    for (i = 0; i < t->atoms_n; i++) {
        int a;

        for (a = 0; a < t->atom_defs[i].nargs; a++) free((void *)t->atom_defs[i].args[a].m);
        free(t->atom_defs[i].args);
        free(t->atom_defs[i].op);
    }
    free(t->atom_defs);
    free(t);
}

static void css_length_install_realm(JSContext *ctx)
{
    JSValue obj = JS_NewObjectClass(ctx, (int)g_css_px_form_table_class);
    CssPxFormTable *t;

    CHECK(!JS_IsException(obj), "css_length: OOM creating a realm's form table");
    t = calloc(1, sizeof *t);
    CHECK(t != NULL, "css_length: OOM creating a realm's form table");
    JS_SetOpaque(obj, t);
    realm_value_set(ctx, g_css_px_form_table_slot, obj);
}

void css_length_init(JSContext *ctx)
{
    JSClassDef def = { "CssPxFormTable", .finalizer = css_px_form_table_finalizer };
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(g_css_px_form_table_slot == JS_INVALID_CLASS_ID, "css_length_init ran twice — the form table's class "
                                                            "and slot are the agent's");
    JS_NewClassID(rt, &g_css_px_form_table_class);
    CHECK(JS_NewClass(rt, g_css_px_form_table_class, &def) == 0, "css_length: the form table's class could not "
                                                                  "be declared");
    g_css_px_form_table_slot = realm_value_declare(ctx, "css_length.h's per-realm table of interned length forms");
    agent_state_class("css_length", &g_css_px_form_table_class, "the per-realm form table's class");
    agent_state_realm_slot("css_length", &g_css_px_form_table_slot, "the per-realm form table's slot");
    realm_declare_intrinsic(css_length_install_realm);
}

void css_length_free(void)
{
    g_css_px_form_table_slot = JS_INVALID_CLASS_ID;
    g_css_px_form_table_class = 0;
}

/* A form's identity, over exactly the fields equality reads: the constant and each monomial's atom, exponents,
   coefficient and atom value, a zero of either sign hashed as one. */
static uint64_t css_px_form_hash(const CssPxForm *f)
{
    uint64_t h = 1469598103934665603ull;
    int i;

#define CSS_PX_HASH_BYTES(p_, n_)                                                    \
    do {                                                                             \
        const unsigned char *b_ = (const unsigned char *)(p_);                       \
        size_t j_;                                                                   \
        for (j_ = 0; j_ < (n_); j_++) h = (h ^ b_[j_]) * 1099511628211ull;           \
    } while (0)
#define CSS_PX_HASH_DOUBLE(d_)                                                       \
    do {                                                                             \
        double x_ = (d_) == 0.0 ? 0.0 : (d_);                                        \
        CSS_PX_HASH_BYTES(&x_, sizeof x_);                                           \
    } while (0)
    CSS_PX_HASH_BYTES(&f->n, sizeof f->n);
    CSS_PX_HASH_DOUBLE(f->c);
    for (i = 0; i < f->n; i++) {
        CSS_PX_HASH_BYTES(&f->m[i].atom, sizeof f->m[i].atom);
        CSS_PX_HASH_BYTES(f->m[i].e, sizeof f->m[i].e);
        CSS_PX_HASH_DOUBLE(f->m[i].k);
        CSS_PX_HASH_DOUBLE(f->m[i].aval);
    }
#undef CSS_PX_HASH_DOUBLE
#undef CSS_PX_HASH_BYTES
    return h;
}

static bool css_px_form_equal(const CssPxForm *a, const CssPxForm *b)
{
    int i;

    if (a->n != b->n || a->c != b->c) return false;
    for (i = 0; i < a->n; i++)
        if (a->m[i].atom != b->m[i].atom || memcmp(a->m[i].e, b->m[i].e, sizeof a->m[i].e) != 0 ||
            a->m[i].k != b->m[i].k || a->m[i].aval != b->m[i].aval)
            return false;
    return true;
}

/* `realm`'s table of forms and atoms. */
static CssPxFormTable *css_px_realm_table(JSContext *realm)
{
    JSValue obj;
    CssPxFormTable *t;

    DCHECK(realm != NULL, "a realm's form table was asked of no realm");
    obj = realm_value_get(realm, g_css_px_form_table_slot);
    t = JS_GetOpaque(obj, g_css_px_form_table_class);
    JS_FreeValue(realm, obj);
    CHECK(t != NULL, "css_length: a realm's form table is missing — its install runs at every realm's creation");
    return t;
}

/* `realm`'s one copy of the known form `f`. */
static const CssPxForm *css_px_form_intern(JSContext *realm, const CssPxForm *f)
{
    CssPxFormTable *t;
    size_t i;

    DCHECK(realm != NULL && f->known, "a form was interned with no realm to own it, or opaque");
    t = css_px_realm_table(realm);
    if (2 * (t->n + 1) > t->cap) {
        size_t cap = t->cap ? 2 * t->cap : 64, j;
        CssPxForm **grown = calloc(cap, sizeof *grown);

        CHECK(grown != NULL, "css_length: OOM growing a realm's form table");
        for (j = 0; j < t->cap; j++) {
            if (t->slot[j] == NULL) continue;
            for (i = css_px_form_hash(t->slot[j]) & (cap - 1); grown[i]; i = (i + 1) & (cap - 1)) {}
            grown[i] = t->slot[j];
        }
        free(t->slot);
        t->slot = grown;
        t->cap = cap;
    }
    for (i = css_px_form_hash(f) & (t->cap - 1); t->slot[i]; i = (i + 1) & (t->cap - 1))
        if (css_px_form_equal(t->slot[i], f)) return t->slot[i];
    t->slot[i] = malloc(sizeof *f);
    CHECK(t->slot[i] != NULL, "css_length: OOM interning a form");
    *t->slot[i] = *f;
    t->n++;
    return t->slot[i];
}

/* ---- the CSS pixel, and the environment fact it may derive from (see css_length.h) ------------------------ */

CssPx css_px(double px)
{
    CssPx out;

    memset(&out, 0, sizeof out);
    out.px = px;
    return out;
}

CssPx css_px_env(CssEnvFact fact, JSContext *realm, double px)
{
    CssPx out = css_px(px);

    /* ASSERTED BEFORE THE SHIFT, because the fact IS the bit position: a fact outside the enum would shift by
       more than the set is wide and the length would come out of this entry a function of nothing. */
    DCHECK((unsigned)fact < (unsigned)CSS_ENV_FACT_COUNT && realm != NULL,
           "a length was said to derive from an ENVIRONMENT FACT with no fact or no realm to name it — the two "
           "travel together because the mint at the JS boundary needs both (which viewport, and which of its "
           "dimensions), and a length carrying one without the other could not be turned into a domain");
    out.env = CSS_ENV_BIT(fact);
    out.realm = realm;
    out.form = &CSS_PX_FORM_OPAQUE;
    return out;
}

CssPx css_px_fact(CssEnvFact fact, JSContext *realm)
{
    double value = viewport_env_fact_value(realm, fact);
    CssPx out = css_px_env(fact, realm, value);

    CssPxForm f;

    DCHECK(isfinite(value), "an environment fact's own value is not finite — every fact is a picked size or "
                            "ratio of the modelled device, and a NaN or an infinity has no polynomial form");
    memset(&f, 0, sizeof f);
    f.known = true;
    f.n = 1;
    f.m[0].e[fact] = 1;
    f.m[0].k = 1.0;
    out.form = css_px_form_intern(realm, &f);
    return out;
}

/* THE UNION, WHICH IS THE WHOLE OF THE PROPAGATION — a length is a joint function of every fact either operand
   was a function of. The result's form is opaque; the operations below restore it where they can.
   THE REALM IS ONE, and it is asserted rather than merged: §10.1's containing-block chain and §6's snap are
   stated over one document, so two realms meeting in one length would mean a layout spanning documents — at
   which point the SET would have to carry a realm per fact, since `{viewport#3}` and `{viewport#7}` are two
   different questions with the same member name. */
static CssPx css_px_combine(CssPx a, CssPx b, double px)
{
    CssPx out = css_px(px);

    DCHECK((a.env == CSS_ENV_NONE) == (a.realm == NULL) && (b.env == CSS_ENV_NONE) == (b.realm == NULL),
           "a length reached the arithmetic carrying facts without a realm, or a realm without facts — the two "
           "are written together by css_px_env and by nothing else, so one without the other is a length "
           "assembled field-by-field past that entry");
    DCHECK(a.realm == NULL || b.realm == NULL || a.realm == b.realm,
           "two lengths derived from the environments of DIFFERENT REALMS were combined into one. A child "
           "navigable's initial containing block is 300 CSS pixels wide and its parent's is 1280, so the two "
           "facts are different questions under one member name — and §10.1's containing-block chain walks one "
           "document, so this is arithmetic spanning two. BUILD the realm into the fact set (a realm per member "
           "rather than one per length) at the point a length is allowed to cross a document boundary");
    DCHECK((a.env == CSS_ENV_NONE) == (a.form == NULL) && (b.env == CSS_ENV_NONE) == (b.form == NULL),
           "a length reached the arithmetic with facts and no form, or a form and no facts — a form is written "
           "only beside the facts it names");
    out.env = a.env | b.env;
    out.realm = a.realm ? a.realm : b.realm;
    out.form = out.env == CSS_ENV_NONE ? NULL : &CSS_PX_FORM_OPAQUE;
    return out;
}

/* ---- the polynomial form (css_length.h's CssPxForm) ------------------------------------------------------- */

static bool css_px_is_known(CssPx a)
{
    return a.env == CSS_ENV_NONE || a.form->known;
}

/* `a`'s constant term: a determined length is its own constant. */
static double css_px_form_c(CssPx a)
{
    return a.env == CSS_ENV_NONE ? a.px : a.form->c;
}

/* A form being composed, before it is sorted, merged and checked against CSS_PX_FORM_MAX. */
typedef struct {
    double        c;
    int           n;
    CssPxMonomial m[CSS_PX_FORM_MAX * CSS_PX_FORM_MAX + 2 * CSS_PX_FORM_MAX];
} CssPxPoly;

static void css_px_poly_add(CssPxPoly *p, const CssPxMonomial *m, double k)
{
    DCHECK(p->n < (int)(sizeof p->m / sizeof p->m[0]), "a composed form outgrew its scratch space");
    p->m[p->n] = *m;
    p->m[p->n++].k = k;
}

/* `p += k·a`, `a` known. */
static void css_px_poly_axpy(CssPxPoly *p, double k, CssPx a)
{
    int i;

    p->c += k * css_px_form_c(a);
    if (a.env == CSS_ENV_NONE) return;
    for (i = 0; i < a.form->n; i++) css_px_poly_add(p, &a.form->m[i], k * a.form->m[i].k);
}

static int css_px_mono_cmp(const void *x, const void *y)
{
    const CssPxMonomial *a = x, *b = y;

    if (a->atom != b->atom) return a->atom < b->atom ? -1 : 1;
    return memcmp(a->e, b->e, CSS_ENV_FACT_COUNT);
}

/* THE ATOM'S NAME IS ITS DEFINITION: the operation and the forms of its operands, which are normal forms (sorted
   and merged), so two derivations of one function compose one key and one name wherever they run, and two
   different functions never share one. A fact enters a key as its index and never as its value, so the name is
   of the function and not of this realm's number. Interned in the realm's table, since its operands are forms
   over that realm's facts; the number is the realm's, and the definition it was interned with
   (css_px_atom_definition) is what a spelling another session shares names. */
static size_t css_px_atom_hash(const unsigned char *k, size_t n)
{
    size_t h = 1469598103934665603u, i;

    for (i = 0; i < n; i++) h = (h ^ k[i]) * 1099511628211u;
    return h;
}

/* An atom's name as bytes: the fields the definition is made of, each at its own width, so two definitions
   share a name exactly when every field is bitwise equal. Composed in `inline_` and moved to the heap only if
   it outgrows it. */
typedef struct {
    unsigned char *b;
    size_t         n, cap;
    unsigned char  inline_[256];
} CssPxKey;

static void css_px_key_begin(CssPxKey *k)
{
    k->b = k->inline_;
    k->n = 0;
    k->cap = sizeof k->inline_;
}

static void css_px_key_bytes(CssPxKey *k, const void *p, size_t n)
{
    if (k->n + n > k->cap) {
        size_t cap = 2 * (k->n + n);
        unsigned char *grown = malloc(cap);

        CHECK(grown, "css_length: OOM naming an atom");
        memcpy(grown, k->b, k->n);
        if (k->b != k->inline_) free(k->b);
        k->b = grown;
        k->cap = cap;
    }
    memcpy(k->b + k->n, p, n);
    k->n += n;
}

static void css_px_key_end(CssPxKey *k)
{
    if (k->b != k->inline_) free(k->b);
}

/* The name `key` (borrowed) is interned under in `realm`'s table; a new name is copied and records `op` over
   `args` (borrowed, copied) as its definition. */
static unsigned css_px_atom_intern(JSContext *realm, const CssPxKey *key, const char *op, const CssPxAtomArg *args,
                                   int nargs)
{
    CssPxFormTable *t = css_px_realm_table(realm);
    CssPxAtomDef *d;
    int a;

    size_t i;

    if (2 * ((size_t)t->atoms_n + 1) > t->atoms_cap) {
        size_t cap = t->atoms_cap ? 2 * t->atoms_cap : 64, j;
        CssPxAtomSlot *grown = calloc(cap, sizeof *grown);

        CHECK(grown, "css_length: OOM growing a realm's atom table");
        for (j = 0; j < t->atoms_cap; j++) {
            if (!t->atoms[j].key) continue;
            for (i = css_px_atom_hash(t->atoms[j].key, t->atoms[j].len) & (cap - 1); grown[i].key;
                 i = (i + 1) & (cap - 1)) {}
            grown[i] = t->atoms[j];
        }
        free(t->atoms);
        t->atoms = grown;
        t->atoms_cap = cap;
    }
    for (i = css_px_atom_hash(key->b, key->n) & (t->atoms_cap - 1); t->atoms[i].key;
         i = (i + 1) & (t->atoms_cap - 1))
        if (t->atoms[i].len == key->n && !memcmp(t->atoms[i].key, key->b, key->n)) return t->atoms[i].id;
    DCHECK(t->atoms_n != UINT_MAX, "a realm's atom table ran out of names");
    t->atoms[i].key = malloc(key->n ? key->n : 1);
    CHECK(t->atoms[i].key, "css_length: OOM interning an atom's name");
    memcpy(t->atoms[i].key, key->b, key->n);
    t->atoms[i].len = key->n;
    t->atoms[i].id = ++t->atoms_n;
    /* Grown by doubling, as the key table is: a layout mints an atom per undecided extreme, so growing by one
       would copy the whole array for each. */
    if ((size_t)t->atoms_n > t->atom_defs_cap) {
        size_t cap = t->atom_defs_cap ? 2 * t->atom_defs_cap : 64;
        CssPxAtomDef *grown = realloc(t->atom_defs, cap * sizeof *grown);

        CHECK(grown, "css_length: OOM recording an atom's definition");
        t->atom_defs = grown;
        t->atom_defs_cap = cap;
    }
    d = &t->atom_defs[t->atoms_n - 1];
    d->op = strdup(op);
    d->nargs = nargs;
    d->args = calloc((size_t)(nargs > 0 ? nargs : 1), sizeof *d->args);
    CHECK(d->op && d->args, "css_length: OOM recording an atom's definition");
    for (a = 0; a < nargs; a++) {
        CssPxMonomial *m = NULL;

        d->args[a] = args[a];
        if (!args[a].determined && args[a].n > 0) {
            m = malloc((size_t)args[a].n * sizeof *m);
            CHECK(m, "css_length: OOM recording an atom's definition");
            memcpy(m, args[a].m, (size_t)args[a].n * sizeof *m);
        }
        d->args[a].m = m;
    }
    return t->atoms[i].id;
}

const char *css_px_atom_definition(JSContext *realm, unsigned atom, int *nargs, const CssPxAtomArg **args)
{
    CssPxFormTable *t = css_px_realm_table(realm);

    DCHECKF(atom >= 1 && atom <= t->atoms_n, "atom %u was asked for its definition and no atom of this realm has "
            "that name", atom);
    *nargs = t->atom_defs[atom - 1].nargs;
    *args = t->atom_defs[atom - 1].args;
    return t->atom_defs[atom - 1].op;
}

/* One polynomial into a key: its monomial count, the constant, and every monomial's atom, exponents and
   coefficient, a double as its bits so the name is exact. */
static void css_px_key_poly(CssPxKey *k, double c, const CssPxMonomial *m, int n)
{
    int i;

    css_px_key_bytes(k, &n, sizeof n);
    css_px_key_bytes(k, &c, sizeof c);
    for (i = 0; i < n; i++) {
        css_px_key_bytes(k, &m[i].atom, sizeof m[i].atom);
        css_px_key_bytes(k, m[i].e, sizeof m[i].e);
        css_px_key_bytes(k, &m[i].k, sizeof m[i].k);
    }
}

/* An atom's key head: its operation, with the terminator that delimits it, and its operand count. */
static void css_px_key_op(CssPxKey *k, const char *op, int nargs)
{
    css_px_key_bytes(k, op, strlen(op) + 1);
    css_px_key_bytes(k, &nargs, sizeof nargs);
}

/* `out`'s form becomes the atom named `key` (borrowed), defined as `op` over `args`. A non-finite example has
   no form and stays opaque. */
static void css_px_form_atom_named(CssPx *out, const CssPxKey *key, const char *op, const CssPxAtomArg *args,
                                   int nargs)
{
    CssPxForm f;

    DCHECK(out->env != CSS_ENV_NONE, "an atom was minted for a length derived from no fact");
    if (!isfinite(out->px)) {
        out->form = &CSS_PX_FORM_OPAQUE;
        return;
    }
    memset(&f, 0, sizeof f);
    f.known = true;
    f.n = 1;
    f.m[0].atom = css_px_atom_intern(out->realm, key, op, args, nargs);
    f.m[0].k = 1.0;
    f.m[0].aval = out->px;
    out->form = css_px_form_intern(out->realm, &f);
}

/* `out`'s form becomes the atom that is `op` over `args`: an opaque function of the facts in `out.env`, named by
   its definition so every derivation of it subtracts it. An operand with no form leaves `out` opaque, since
   there is then no definition to name. */
static void css_px_form_atom(CssPx *out, const char *op, const CssPx *args, int nargs)
{
    CssPxKey key;
    CssPxAtomArg *def;
    int i;

    for (i = 0; i < nargs; i++)
        if (args[i].env != CSS_ENV_NONE && !args[i].form->known) {
            out->form = &CSS_PX_FORM_OPAQUE;
            return;
        }
    def = calloc((size_t)(nargs > 0 ? nargs : 1), sizeof *def);
    CHECK(def, "css_length: OOM naming an atom");
    for (i = 0; i < nargs; i++) {
        def[i].determined = args[i].env == CSS_ENV_NONE;
        def[i].px = args[i].px;
        if (!def[i].determined) {
            def[i].c = args[i].form->c;
            def[i].n = args[i].form->n;
            def[i].m = args[i].form->m;
        }
    }
    css_px_key_begin(&key);
    css_px_key_op(&key, op, nargs);
    for (i = 0; i < nargs; i++) {
        unsigned char determined = args[i].env == CSS_ENV_NONE;

        css_px_key_bytes(&key, &determined, 1);
        if (determined) css_px_key_bytes(&key, &args[i].px, sizeof args[i].px);
        else css_px_key_poly(&key, args[i].form->c, args[i].form->m, args[i].form->n);
    }
    css_px_form_atom_named(out, &key, op, def, nargs);
    css_px_key_end(&key);
    free(def);
}

CssPx css_px_atom(const char *op, const CssPx *args, int nargs, double px)
{
    CssPx out = css_px(px);
    int i;

    for (i = 0; i < nargs; i++) out = css_px_combine(out, args[i], px);
    if (out.env == CSS_ENV_NONE) return out;
    css_px_form_atom(&out, op, args, nargs);
    return out;
}

/* `p` in normal form into `*f`, answering true; or false where `out` needs no interned form of `p`: a length
   derived from no fact, or too wide a form, which becomes one atom here. The operands' facts are one realm's
   (css_px_combine asserts it), and a fact is one number per realm, so the form holds no values to agree. */
static bool css_px_form_normal(CssPx *out, CssPxPoly *p, CssPxForm *f)
{
    int i = 0, n = 0;

    /* An insertion sort: a composed form holds a handful of monomials, each wider than the C library's
       direct-sort threshold, so its sort allocates and indirects. */
    for (i = 1; i < p->n; i++) {
        CssPxMonomial t = p->m[i];
        int j = i;

        for (; j > 0 && css_px_mono_cmp(&p->m[j - 1], &t) > 0; j--) p->m[j] = p->m[j - 1];
        p->m[j] = t;
    }
    i = 0;
    while (i < p->n) {
        CssPxMonomial t = p->m[i++];

        while (i < p->n && css_px_mono_cmp(&t, &p->m[i]) == 0) {
            DCHECK(!t.atom || t.aval == p->m[i].aval,
                   "one atom reached a sum with two values — an atom is one function named by its definition, "
                   "and a definition over one realm's facts has one value");
            t.k += p->m[i++].k;
        }
        if (t.k != 0.0) p->m[n++] = t;
    }
    if (out->env == CSS_ENV_NONE) {
        DCHECK(n == 0, "a length derived from no fact composed a form with a monomial over one");
        return false;
    }
    if (n > CSS_PX_FORM_MAX) {
        CssPxKey key;
        CssPxAtomArg def = { false, 0.0, p->c, n, p->m };
        unsigned char determined = 0;

        /* The same name `css_px_form_atom` gives `poly` over one undetermined operand. */
        css_px_key_begin(&key);
        css_px_key_op(&key, "poly", 1);
        css_px_key_bytes(&key, &determined, 1);
        css_px_key_poly(&key, p->c, p->m, n);
        css_px_form_atom_named(out, &key, "poly", &def, 1);
        css_px_key_end(&key);
        return false;
    }
    memset(f, 0, sizeof *f);
    f->known = true;
    f->n = n;
    f->c = p->c;
    memcpy(f->m, p->m, (size_t)n * sizeof p->m[0]);
    return true;
}

/* `out`'s form becomes `p` in normal form (`css_px_form_normal`), interned. */
static void css_px_form_set(CssPx *out, CssPxPoly *p)
{
    CssPxForm f;

    if (css_px_form_normal(out, p, &f)) out->form = css_px_form_intern(out->realm, &f);
}

/* `out`'s form becomes `ka·a + kb·b`, over two known operands. */
static void css_px_form_lin(CssPx *out, CssPx a, double ka, CssPx b, double kb)
{
    CssPxPoly p;

    DCHECK(css_px_is_known(a) && css_px_is_known(b), "a linear combination was formed over an opaque length");
    p.c = 0.0;
    p.n = 0;
    css_px_poly_axpy(&p, ka, a);
    css_px_poly_axpy(&p, kb, b);
    css_px_form_set(out, &p);
}

double css_px_form_eval(CssPx a)
{
    double v;
    int i, f;

    DCHECK(css_px_is_known(a), "an opaque length's form was evaluated");
    v = css_px_form_c(a);
    if (a.env == CSS_ENV_NONE) return v;
    for (i = 0; i < a.form->n; i++) {
        double t = a.form->m[i].k;

        if (a.form->m[i].atom) t *= a.form->m[i].aval;
        for (f = 0; f < CSS_ENV_FACT_COUNT; f++)
            if (a.form->m[i].e[f]) t *= pow(viewport_env_fact_value(a.realm, (CssEnvFact)f), a.form->m[i].e[f]);
        v += t;
    }
    return v;
}

CssPx css_px_add(CssPx a, CssPx b)
{
    CssPx out = css_px_combine(a, b, a.px + b.px);

    if (css_px_is_known(a) && css_px_is_known(b)) css_px_form_lin(&out, a, 1.0, b, 1.0);
    return out;
}

void css_px_sum_init(CssPxSum *s)
{
    DCHECK(s != NULL, "a length sum was begun with nowhere to hold it");
    s->v = css_px(0.0);
    s->held = false;
}

/* `css_px_add`'s steps over the held form: the same combination and the same normal form, kept by value. */
void css_px_sum_add(CssPxSum *s, CssPx b)
{
    CssPx a, out;

    DCHECK(s != NULL, "a term was added to no length sum");
    a = s->v;
    if (s->held) a.form = &s->f;
    out = css_px_combine(a, b, a.px + b.px);
    s->held = false;
    if (css_px_is_known(a) && css_px_is_known(b)) {
        CssPxPoly p;

        p.c = 0.0;
        p.n = 0;
        css_px_poly_axpy(&p, 1.0, a);
        css_px_poly_axpy(&p, 1.0, b);
        s->held = css_px_form_normal(&out, &p, &s->f);
    }
    s->v = out;
}

CssPx css_px_sum_value(const CssPxSum *s)
{
    CssPx out;

    DCHECK(s != NULL, "a length sum was read from no sum");
    out = s->v;
    if (s->held) out.form = css_px_form_intern(out.realm, &s->f);
    return out;
}

CssPx css_px_sub(CssPx a, CssPx b)
{
    CssPx out = css_px_combine(a, b, a.px - b.px);

    if (css_px_is_known(a) && css_px_is_known(b)) css_px_form_lin(&out, a, 1.0, b, -1.0);
    return out;
}

/* `k` is a pure ratio — a percentage divided by 100 — so it changes no fact and scales the form. */
CssPx css_px_scale(CssPx a, double k)
{
    CssPx out = a;

    out.px = a.px * k;
    if (a.env != CSS_ENV_NONE && a.form->known) css_px_form_lin(&out, a, k, css_px(0.0), 0.0);
    return out;
}

/* The range each fact takes over every world this user agent can be in, which is what decides an extreme for
   every world at once. Every lower bound is zero or more, so a monomial is monotone in each of its facts and
   its range is the product of its facts' ranges. Indexed by the fact. */
static const struct { double lo, hi; } CSS_ENV_RANGE[CSS_ENV_FACT_COUNT] = {
    /* CSS 2.2 §10.1 Definition of "containing block": the initial containing block "has the dimensions of the
       viewport", and css-values-4 §6.1.2.1's small and dynamic sizes are that viewport sized twice more. */
    [CSS_ENV_ICB_WIDTH]               = { 0.0, INFINITY },
    [CSS_ENV_ICB_HEIGHT]              = { 0.0, INFINITY },
    [CSS_ENV_SMALL_VIEWPORT_WIDTH]    = { 0.0, INFINITY },
    [CSS_ENV_SMALL_VIEWPORT_HEIGHT]   = { 0.0, INFINITY },
    [CSS_ENV_DYNAMIC_VIEWPORT_WIDTH]  = { 0.0, INFINITY },
    [CSS_ENV_DYNAMIC_VIEWPORT_HEIGHT] = { 0.0, INFINITY },
    /* CSSOM VIEW §4 Extensions to the Window Interface: "the result of dividing CSS pixel size by device pixel
       size", a ratio of two sizes. */
    [CSS_ENV_DEVICE_PIXEL_RATIO]      = { 0.0, INFINITY },
    /* css-fonts-4 §2.5 Font size: the font-size property: `<length-percentage [0,∞]>`, and "Negative lengths
       are invalid". */
    [CSS_ENV_DEFAULT_FONT_SIZE]       = { 0.0, INFINITY },
    [CSS_ENV_DEFAULT_FIXED_FONT_SIZE] = { 0.0, INFINITY },
    /* CSS 2.2 §10.8.1 Leading and half-leading: "a characteristic height above the baseline and a depth below
       it". */
#define CSS_ENV_FONT_FACT_RANGE(id, family, fact, bytes) \
    [CSS_ENV_FONT_ASCENT_##id] = { 0.0, INFINITY }, [CSS_ENV_FONT_DESCENT_##id] = { 0.0, INFINITY }, \
    [CSS_ENV_FONT_LINE_GAP_##id] = { 0.0, INFINITY },
    INSTALLED_FONT_LIST(CSS_ENV_FONT_FACT_RANGE)
#undef CSS_ENV_FONT_FACT_RANGE
};

/* The bounds of `a - b` over every world, both operands known. */
static void css_px_diff_range(CssPx a, CssPx b, double *lo, double *hi)
{
    CssPx d = css_px_sub(a, b);
    int i, f;

    *lo = *hi = css_px_form_c(d);
    if (d.env == CSS_ENV_NONE) return;
    DCHECK(d.form->known, "the difference of two known lengths has an opaque form");
    for (i = 0; i < d.form->n; i++) {
        double k = d.form->m[i].k, mlo = 1.0, mhi = 1.0;

        /* An atom is a function the form cannot state, so nothing bounds it. */
        if (d.form->m[i].atom) { *lo = -INFINITY; *hi = INFINITY; return; }
        for (f = 0; f < CSS_ENV_FACT_COUNT; f++) {
            if (!d.form->m[i].e[f]) continue;
            DCHECK(CSS_ENV_RANGE[f].lo >= 0.0, "a fact's range reaches below zero, so its monomials are not "
                                               "monotone and their ranges are not their facts' products");
            mlo *= pow(CSS_ENV_RANGE[f].lo, d.form->m[i].e[f]);
            mhi *= pow(CSS_ENV_RANGE[f].hi, d.form->m[i].e[f]);
        }
        *lo += k > 0.0 ? k * mlo : k * mhi;
        *hi += k > 0.0 ? k * mhi : k * mlo;
    }
}

/* css-values-4 §10.9.2 Infinities, NaN, and Signed Zero: "When comparing 0⁺ and 0⁻, 0⁻ is less than 0⁺". C
   leaves `fmin(-0.0, 0.0)` unspecified. NaN is the caller's to answer first. */
static bool css_px_less(double a, double b)
{
    DCHECK(a == a && b == b, "two lengths were ordered over a NaN, which has no order: css-values-4 §10.5.1's note makes NaN infectious in every math function, so its caller answers it first");
    if (a == 0.0 && b == 0.0) return signbit(a) && !signbit(b);
    return a < b;
}

/* THE LARGER (AND THE SMALLER) EXAMPLE, CARRYING BOTH OPERANDS' FACTS — which is why this is `css_px_combine`
   and not `a.px > b.px ? a : b`. Returning the winner whole would drop the loser's fact, and the loser is the
   arm the OTHER viewport takes: css-sizing §5's floor picks the declared length at 1280 and the four-term
   surround at 320, so a result that kept only the winner would report a length as environment-independent in
   exactly the world where it is not. Over-reporting the dependence forks a world the page might not have
   needed; under-reporting it deletes one it did, and CLAUDE.md's §Headless errs toward the first.
   The winner's form survives where the ranges decide it for every world, and the examples must agree
   with that decision, which is what makes it falsifiable here. `want_hi` picks the larger. */
static CssPx css_px_extreme(CssPx a, CssPx b, bool want_hi)
{
    bool a_wins = want_hi ? css_px_less(b.px, a.px) : css_px_less(a.px, b.px);
    CssPx out = css_px_combine(a, b, a_wins ? a.px : b.px);
    const CssPx *win = NULL;
    double lo, hi, tol;

    if (!css_px_is_known(a) || !css_px_is_known(b)) return out;
    css_px_diff_range(a, b, &lo, &hi);
    if (lo >= 0.0) win = want_hi ? &a : &b;
    else if (hi <= 0.0) win = want_hi ? &b : &a;
    /* Undecided, so the winner moves with the facts. The result is `lo + max(hi - lo, 0)` (`min` for the
       smaller), one atom over the operands' DIFFERENCE added to one of them, so moving both operands by one
       length moves the result by it and leaves the atom alone: two boxes alike but for where they sit compose
       one atom, and their extents subtract to a constant. `lo` is the operand the difference's leading term
       orders first, a test translation and operand order both leave alone, so max(a, b) and max(b, a) are one
       form. The example stays the winner's own number. */
    if (win == NULL) {
        CssPx d = css_px_sub(a, b), base, delta, x;
        bool a_first;

        DCHECK(out.env != CSS_ENV_NONE, "an extreme of two determined lengths was left undecided");
        DCHECK(d.env != CSS_ENV_NONE && d.form->known && d.form->n > 0,
               "an extreme was left undecided over operands whose difference is a constant — the range of a "
               "constant decides it");
        a_first = d.form->m[0].k < 0.0;
        base = a_first ? a : b;
        delta = css_px_sub(a_first ? b : a, base);
        x = css_px_atom(want_hi ? "max(_, 0)" : "min(_, 0)", &delta, 1,
                        want_hi ? fmax(delta.px, 0.0) : fmin(delta.px, 0.0));
        out.form = css_px_add(base, x).form;
        return out;
    }
    tol = 1e-9 * fmax(1.0, fmax(fabs(a.px), fabs(b.px)));
    DCHECKF(fabs(out.px - win->px) <= tol,
            "an extreme over %g and %g was decided for every world by the facts' ranges (a - b in [%g, %g]) "
            "and the examples disagree with that decision — one operand's form is not the function its "
            "example was computed by", a.px, b.px, lo, hi);
    css_px_form_lin(&out, *win, 1.0, css_px(0.0), 0.0);
    return out;
}

CssPx css_px_max(CssPx a, CssPx b)
{
    return css_px_extreme(a, b, true);
}

CssPx css_px_min(CssPx a, CssPx b)
{
    return css_px_extreme(a, b, false);
}

/* A product keeps the form: the product of two polynomials is one. */
CssPx css_px_mul(CssPx a, CssPx b)
{
    CssPx out = css_px_combine(a, b, a.px * b.px);
    CssPxPoly p;
    int i, j, f;

    if (!css_px_is_known(a) || !css_px_is_known(b)) return out;
    p.c = css_px_form_c(a) * css_px_form_c(b);
    p.n = 0;
    if (b.env != CSS_ENV_NONE)
        for (j = 0; j < b.form->n; j++) css_px_poly_add(&p, &b.form->m[j], css_px_form_c(a) * b.form->m[j].k);
    if (a.env != CSS_ENV_NONE)
        for (i = 0; i < a.form->n; i++) css_px_poly_add(&p, &a.form->m[i], css_px_form_c(b) * a.form->m[i].k);
    if (a.env != CSS_ENV_NONE && b.env != CSS_ENV_NONE)
        for (i = 0; i < a.form->n; i++)
            for (j = 0; j < b.form->n; j++) {
                CssPxMonomial m;

                /* A monomial holds one atom, so a product of two is one atom of its own. */
                if (a.form->m[i].atom && b.form->m[j].atom) {
                    CssPx ab[2] = { a, b };

                    css_px_form_atom(&out, "mul", ab, 2);
                    return out;
                }
                memset(&m, 0, sizeof m);
                m.atom = a.form->m[i].atom ? a.form->m[i].atom : b.form->m[j].atom;
                m.aval = a.form->m[i].atom ? a.form->m[i].aval : b.form->m[j].aval;
                for (f = 0; f < CSS_ENV_FACT_COUNT; f++) {
                    DCHECK(a.form->m[i].e[f] + b.form->m[j].e[f] <= 255, "a monomial's exponent overflowed");
                    m.e[f] = (unsigned char)(a.form->m[i].e[f] + b.form->m[j].e[f]);
                }
                css_px_poly_add(&p, &m, a.form->m[i].k * b.form->m[j].k);
            }
    css_px_form_set(&out, &p);
    return out;
}

/* A quotient by a DETERMINED operand is a scaling and keeps the form; one by a derived length is a reciprocal,
   which no polynomial states, so it is one atom. */
CssPx css_px_div(CssPx a, CssPx b)
{
    CssPx out;

    DCHECK(b.px != 0.0,
           "a quotient of two lengths was taken with a ZERO divisor. Every algorithm that divides here states "
           "the non-zero branch itself — Intersection Observer §3.2.10 step 12 divides only 'if targetArea is "
           "non-zero' and answers 1 or 0 otherwise — so a zero arriving means the caller skipped its own "
           "branch rather than that this operation needs one");
    out = css_px_combine(a, b, a.px / b.px);
    if (b.env != CSS_ENV_NONE) {
        CssPx ab[2] = { a, b };

        css_px_form_atom(&out, "div", ab, 2);
    } else if (a.env != CSS_ENV_NONE && a.form->known) {
        css_px_form_lin(&out, a, 1.0 / b.px, css_px(0.0), 0.0);
    }
    return out;
}

/* CSS Values §6.2's ABSOLUTE UNITS, each as the spec's own definition of it in CSS pixels: "1in = 96px" is the
   anchor and the other five are exact fractions of the inch (1cm = 96/2.54, 1mm = 96/25.4, 1Q = 96/101.6,
   1pt = 96/72, 1pc = 16). Nothing here is a device measurement, which is why §Headless does not reach it. */
static const struct { const char *unit; double px; } CSS_ABSOLUTE[] = {
    { "px", 1.0 },
    { "in", 96.0 },
    { "cm", 96.0 / 2.54 },
    { "mm", 96.0 / 25.4 },
    { "q",  96.0 / 101.6 },
    { "pt", 96.0 / 72.0 },
    { "pc", 16.0 },
};

/* css-values-4 §6.1.1's FONT-RELATIVE units — ALL TWELVE, which its own section TITLE enumerates:
   "Font-relative Lengths: the em, rem, ex, rex, cap, rcap, ch, rch, ic, ric, lh, rlh units". §6.1.1 splits
   them into the LOCAL font-relative lengths, which "refer to the font metrics ... of the element on which they
   are used", and the ROOT font-relative lengths, which refer to the root element's — and every local one has
   an `r`-prefixed root twin, which is what makes the list twelve and not eight.
   FOUR OF THEM WERE MISSING AND THE COST WAS A WRONG CRASH, not a missing one: `rex`, `rcap`, `rch` and `ric`
   fell past this table and past css_len_is_viewport into `css_length_parse`'s last DFAIL, which says the unit
   is "a DIMENSION in a unit CSS Values §6 does not define as a length at all — an angle, a time, a frequency
   or a resolution". §6.1.1 defines all four as lengths, so that message sent the reader to look for a caller
   asking the wrong component when what is missing is a font metric. A crash that names the wrong absence is
   the stale-`DFAIL` failure with the coordinates still correct.
   THE TWELVE SPLIT BY WHERE THEIR MULTIPLIER COMES FROM, AND THE LINE IS THE SPEC'S OWN. §6.1.1 defines
   `em` as "the computed value of the font-size property of the element on which it is used" and `rem` as "the
   computed value of the em unit on the root element" — both are a computed `font-size` and nothing else. Six
   more are a FONT METRIC times one of those two sizes, which core/css/font_metrics.h owns: the two ADVANCES
   are measured off the first available font's own 'hmtx' whenever it has the glyph, and every one of the three
   falls back to what §6.1.1 states MUST be assumed when the metric cannot be determined (an x-height is 0.5em,
   the "0" glyph is "0.5em wide by 1em tall", the "水" glyph is 1em) — a real value for a user agent that
   cannot measure rather than a stand-in. `cap`/`rcap` join them on the same arithmetic and a DIFFERENT
   footing:
   §6.1.1 fixes no cap-height, only that an undeterminable one takes "the font's ASCENT", and that ascent is a
   metric of the modelled face that core/css/font_metrics.h reads — so the product carries an environment fact
   where the three above carry none. All of them resolve through the ONE `CssFontMetrics` pair the caller
   holding the tree answers — one table, because the arithmetic is the same and only the base and the multiplier
   differ.
   AND `lh`/`rlh` COMPLETES THE TWELVE, on the same arithmetic and a base that is a PROPERTY rather than a
   metric: §6.1.1 makes it "the computed value of the line-height property of the element on which it is used,
   converting normal to an absolute length by using only the metrics of the first available font", so the
   caller answers css-inline-3 §5.1's computed value with CSS 2.1 §10.8.1's `AD` substituted for `normal`.
   WHICH element answers is §6.1.1's own rule for these two units alone — inside `line-height` they resolve
   against the PARENT's, which core/css/font_size_functions.h's second predicate decides from the property
   exactly as the font-affecting one does. There is no arm left in this file for a font-relative unit to reach
   past the table, which is why there is no list beside it any more. */
static const struct { const char *unit; CssFontMetric metric; } CSS_FONT_RELATIVE[] = {
    { "em",  CSS_FONT_METRIC_EM  }, { "rem", CSS_FONT_METRIC_REM },
    { "ex",  CSS_FONT_METRIC_EX  }, { "rex", CSS_FONT_METRIC_REX },
    { "ch",  CSS_FONT_METRIC_CH  }, { "rch", CSS_FONT_METRIC_RCH },
    { "ic",  CSS_FONT_METRIC_IC  }, { "ric", CSS_FONT_METRIC_RIC },
    { "cap", CSS_FONT_METRIC_CAP }, { "rcap", CSS_FONT_METRIC_RCAP },
    { "lh",  CSS_FONT_METRIC_LH  }, { "rlh", CSS_FONT_METRIC_RLH },
};

/* WHICH METRIC A UNIT NAMES, or false for one this file cannot route. It is a lookup and not a test followed
   by a second lookup, because the two would be one list that can disagree about a spelling. */
static bool css_len_font_metric_of(const char *unit, CssFontMetric *out)
{
    unsigned i;

    for (i = 0; i < sizeof(CSS_FONT_RELATIVE) / sizeof(CSS_FONT_RELATIVE[0]); i++)
        if (strcmp(CSS_FONT_RELATIVE[i].unit, unit) == 0) { *out = CSS_FONT_RELATIVE[i].metric; return true; }
    return false;
}

/* §6.1.2.2's VIEWPORT-PERCENTAGE units, in their DEFAULT spelling — the `v*` family, which §6.1.2.1 defines
   against the LARGE viewport size. The `sv*`, `lv*` and `dv*` variants are the same six names with one letter
   in front, which is the spec's own naming rule and is why they are recognised by it below rather than
   tabulated a second time. */
static const char *const CSS_VIEWPORT_RELATIVE[] = {
    "vw", "vh", "vi", "vb", "vmin", "vmax",
};

static bool css_len_in(const char *const *set, unsigned n, const char *unit)
{
    unsigned i;

    for (i = 0; i < n; i++)
        if (strcmp(set[i], unit) == 0) return true;
    return false;
}

#define CSS_LEN_N(set) (sizeof(set) / sizeof((set)[0]))

static bool css_len_is_viewport(const char *unit)
{
    return css_len_in(CSS_VIEWPORT_RELATIVE, CSS_LEN_N(CSS_VIEWPORT_RELATIVE), unit);
}

/* §6.1.2.1's `sv*` / `lv*` / `dv*` — the SMALL, LARGE and DYNAMIC viewport sizes, spelled as the default six
   names with one letter in front. */
static bool css_len_is_viewport_variant(const char *unit)
{
    if (unit[0] != 's' && unit[0] != 'l' && unit[0] != 'd') return false;
    return css_len_is_viewport(unit + 1);
}

/* CSS Conditional 5 §7 Container Relative Lengths: the cqw, cqh, cqi, cqb, cqmin, cqmax units — "Container
   query length units specify a length relative to the dimensions of a query container." They are a LIST and
   not the `sv*`/`lv*`/`dv*` prefix test one letter wider, because these six are NOT the viewport names with a
   prefix: `cqi` and `cqb` are the inline and block sizes of a QUERY CONTAINER, and §7's own table defines
   `cqmin` and `cqmax` as "The smaller value of cqi or cqb" and "The larger value of cqi or cqb" — never of
   `cqw` or `cqh`, which is a different pairing from `vmin`/`vmax`'s. Reading them out of one table would be
   the coincidence of four spellings standing in for a definition. */
static const char *const CSS_CONTAINER_RELATIVE[] = {
    "cqw", "cqh", "cqi", "cqb", "cqmin", "cqmax",
};

static bool css_len_is_container(const char *unit)
{
    return css_len_in(CSS_CONTAINER_RELATIVE, CSS_LEN_N(CSS_CONTAINER_RELATIVE), unit);
}

/* §6.2's SEVEN ABSOLUTE UNITS, ASKED FROM OUTSIDE THE COMPUTED-VALUE CHAIN — see css_length.h for who asks and
   why it is not `css_length_parse`. The unit is compared ASCII-case-insensitively because a CSS unit identifier
   is, and the table's own spellings are lowercase; the comparison is written over a length rather than over a
   NUL, because a tokenizer hands its caller a span and copying it to ask this would be the copy that then has
   to be freed on every failure path. */
bool css_length_absolute_px(const char *unit, size_t unit_len, double n, double *px)
{
    unsigned i;
    size_t k;

    DCHECK(unit != NULL || unit_len == 0, "an absolute-unit test was handed a NULL span with a non-zero length");
    DCHECK(px != NULL, "an absolute-unit test was handed nowhere to write the length it converts");
    for (i = 0; i < CSS_LEN_N(CSS_ABSOLUTE); i++) {
        const char *u = CSS_ABSOLUTE[i].unit;

        if (strlen(u) != unit_len) continue;
        for (k = 0; k < unit_len; k++)
            if (tolower((unsigned char)unit[k]) != u[k]) break;
        if (k < unit_len) continue;
        *px = n * CSS_ABSOLUTE[i].px;
        return true;
    }
    return false;
}

/* §6.1.2's VIEWPORT-PERCENTAGE LENGTHS, absolutized: "the viewport-percentage lengths are relative to the size
   of the INITIAL CONTAINING BLOCK". `k` is the number divided by 100, because every one of these units is
   defined as 1% of a dimension. */
static CssPx css_len_viewport(JSContext *realm, const char *unit, double k)
{
    CssPx w, h;
    const char *axis;

    if (realm == NULL || !viewport_exists(realm))
        DFAIL("a VIEWPORT-PERCENTAGE length was absolutized for an element whose document is NOT BEING "
              "PRESENTED by any navigable — a DOMParser document, an XHR `responseXML`, a `<template>`'s "
              "contents owner, or the document of a navigable that has been destroyed. css-values §6.1.2 "
              "resolves these units against the INITIAL CONTAINING BLOCK and CSS 2.1 §10.1 gives that the "
              "dimensions of the VIEWPORT, so with no viewport there is no rectangle to be a percentage of. "
              "CSSOM §9 does not state this escape — its two conjuncts are the property's `Applies to:` line "
              "and the element's own computed `display`, and both can be true here — yet every user agent "
              "answers the computed value for an element that generates no box. BUILD §9's missing conjunct "
              "over the predicate that already decides it, core/dom/element_view.h's `element_view_has_box`, "
              "so the resolved value takes §9's computed-value escape before a length is ever absolutized "
              "(core/layout/used_value.c's uv_icb names the same one)");
    /* §6.1.2.1's THREE SIZES, PICKED BY THE ONE LETTER A VARIANT PUTS IN FRONT — the spec's own naming rule,
       which is why this strips a prefix rather than tabulating twenty-four spellings a second time. The
       default family has no prefix and its `v` falls through to the same arm as `lv*`, which is §6.1.2.1
       defining one size for both: "The large viewport-percentage units (lv*) and default viewport-percentage
       units (v*) are defined with respect to the large viewport size". So `100vh` and `100lvh` carry the ONE
       ICB fact and compare EQUAL without forking, which is the standard's identity rather than a collapse —
       while `sv*` and `dv*` carry facts of their own and a comparison against either forks. */
    switch (unit[0]) {
    case 's': w = viewport_small_width(realm);   h = viewport_small_height(realm);   break;
    case 'd': w = viewport_dynamic_width(realm); h = viewport_dynamic_height(realm); break;
    default:
        DCHECK(unit[0] == 'v' || unit[0] == 'l',
               "a unit reached §6.1.2.1's viewport-size selection whose first letter names none of its four "
               "families — css_len_is_viewport and css_len_is_viewport_variant are the two predicates that "
               "admit a unit here and between them they admit only `v`, `s`, `l` and `d`, so a fifth letter "
               "is a caller that tested neither");
        w = viewport_icb_width(realm);
        h = viewport_icb_height(realm);
        break;
    }
    /* THE AXIS NAME IS THE DEFAULT SPELLING, which is what §6.1.2.2 defines every unit over: it writes each
       definition once with a `*` standing for all four families, so the axis question and the size question
       are independent and are asked here as two. */
    axis = unit[0] == 'v' ? unit : unit + 1;
    /* §6.1.2.1's ONE DIVERGENCE, asserted rather than assumed: "if the value of overflow or scrollbar-gutter on
       the root element in either axis would cause scrollbars to appear … unconditionally … the computed values
       of the viewport-percentage lengths in that axis are reduced in accordance with the initial containing
       block. Otherwise … the viewport-percentage lengths are sized assuming that scrollbars do not exist (EVEN
       IF THIS DIVERGES FROM THE INITIAL CONTAINING BLOCK)." This user agent renders no scroll bar, so the ICB
       is the viewport and the two sentences answer the same number — which is what makes resolving these units
       against §10.1's rectangle, and inheriting ITS environment fact, the whole rule rather than half of it.
       The day a scroll bar reduces the ICB the two separate: `vw` keeps following the unreduced viewport unless
       the root element's `overflow` forces the bar unconditionally, so that number becomes a fact of its own
       and this arm stops being able to borrow the ICB's. */
    DCHECK(w.px == viewport_width(realm) && h.px == viewport_height(realm),
           "css-values §6.1.2.1 sizes a viewport-percentage length assuming SCROLLBARS DO NOT EXIST, even where "
           "that diverges from the initial containing block, and the ICB this engine answers is no longer the "
           "whole viewport — so `vw` can no longer be a percentage of it. Read the root element's computed "
           "`overflow` in the axis: where it forces a scroll bar unconditionally the unit follows the reduced "
           "ICB, and otherwise it follows the unreduced viewport, which is then a SECOND environment fact "
           "beside CSS_ENV_ICB_WIDTH and needs its own row in core/frame/viewport.c's seam");
    if (strcmp(axis, "vw") == 0)   return css_px_scale(w, k);
    if (strcmp(axis, "vh") == 0)   return css_px_scale(h, k);
    /* §6.1.2.2: "vmin: equal to the smaller of vw and vh", "vmax: … the larger". BOTH axes are operands, so
       both facts reach the result and the answer is a JOINT function of them — which is exactly what a page
       branching on a `100vmin` box is branching on, a relation between the two viewport dimensions, and what a
       domain over either one alone could not say. Which axis is smaller is decided on the modelled viewport
       (media_query.h's layering) and the fact the loser carried survives the decision, because at another
       viewport it is the winner. */
    if (strcmp(axis, "vmin") == 0) return css_px_min(css_px_scale(w, k), css_px_scale(h, k));
    if (strcmp(axis, "vmax") == 0) return css_px_max(css_px_scale(w, k), css_px_scale(h, k));
    DCHECK(strcmp(axis, "vi") == 0 || strcmp(axis, "vb") == 0,
           "a unit reached the viewport-percentage resolution that is not one of §6.1.2.2's six — this arm and "
           "CSS_VIEWPORT_RELATIVE are one list and have come apart");
    DFAIL("`vi` and `vb` are 1% of the viewport IN THE BOX'S INLINE AND BLOCK AXIS (css-values §6.1.2.2), so "
          "which of the two ICB dimensions they are a percentage of is a fact about the element's WRITING MODE "
          "— css-writing-modes-4 §3.2 \"Block Flow Direction: the writing-mode property\" and §2.1 \"Specifying "
          "Directionality: the direction property\". BOTH VALUES ARE READABLE NOW: each is a `Computed value: "
          "specified value` line that core/css/css_computed_value.c models and core/css/css_defaulting.h "
          "inherits. WHAT IS MISSING IS THE MAPPING — css-writing-modes-4 §6.4 \"Abstract-to-Physical "
          "Mappings\", which turns the box's inline and block axes into the physical pair this resolution has "
          "to pick an ICB dimension from. BUILD §6.4 as its own component: CSSOM §9's used-value list carries "
          "the LOGICAL box-model properties and needs the same mapping before §10 can be asked about them "
          "either (core/css/css_property_applies.c crashes for them by name), and core/layout/flow_position.c "
          "crashes for a vertical writing mode because §9.4.1's rules are stated physically");
    return css_px(0.0);
}

/* THE DIMENSION'S UNIT, lowercased — CSS Syntax §4 makes a unit ASCII case-insensitive, and lexbor serializes
   the ident as the author wrote it, so `4PX` and `4px` arrive here as different bytes and are one value. */
#define CSS_LEN_UNIT_MAX 8

static bool css_len_unit_of(const char *s, char *out)
{
    size_t i;

    for (i = 0; s[i] != '\0'; i++) {
        if (i + 1 >= CSS_LEN_UNIT_MAX) return false;
        out[i] = (char)tolower((unsigned char)s[i]);
    }
    out[i] = '\0';
    return true;
}

/* ONE §6 UNIT, ABSOLUTIZED — the whole of the family split css_length.h describes, with each arm naming the
   component that is missing rather than the one beside it. `unit` is already lowercased and NUL-terminated.
   IT IS A FUNCTION AND NOT THE TAIL OF THE PARSE because css-values-4 §10's math functions reach the same
   table through a different door: a `calc(100vw - 2em)` resolves `100vw` and `2em` through exactly these arms,
   and the alternative is core/css/css_math.c carrying a second copy of the one table this file exists to be. */
static CssPx css_len_unit_px(JSContext *realm, const CssFontMetrics *font, const char *unit, double num)
{
    CssFontMetric metric;
    unsigned i;

    for (i = 0; i < CSS_LEN_N(CSS_ABSOLUTE); i++)
        if (strcmp(CSS_ABSOLUTE[i].unit, unit) == 0) return css_px(num * CSS_ABSOLUTE[i].px);
    /* §6.1.1's EIGHT RESOLVABLE FONT-RELATIVE UNITS, which are ONE arm because they are one multiplication.
       `em` is "the computed value of the font-size property of the element on which it is used" and `rem` is
       "the computed value of the em unit on the root element"; `ex`, `ch` and `ic` are a metric of the FIRST
       AVAILABLE FONT, which core/css/font_metrics.h answers either off the face's own OpenType tables or, for
       a metric the face cannot supply, with §6.1.1's own must-assume ratio of the em; and each `r`-prefixed
       twin is "the value of the <unit> unit ON THE ROOT ELEMENT". So every one of them is a base times this
       declaration's own multiplier, and the whole of the difference between them is WHICH base — which is
       the caller's walk (css_length.h), asked here and nowhere else because it needs the element and the tree
       this file deliberately does not hold.
       THE SCALE CARRIES THE BASE'S ENVIRONMENT FACTS, so a `margin: 1em` reaches the page as a function of the
       reader's default font size exactly as a `50vw` reaches it as a function of the viewport, and the
       responsive ladder a `rem`-sized bundle builds on that number keeps its second arm. A `width: 20ch`
       carries that same fact and, since the face landed, is a function of a SECOND one this scale does not yet
       name — the advance measure is no longer a spec constant with no freedom in it. css_length.h states that
       open decision beside the unit list; it is a change to the fact vocabulary and not to this arithmetic. */
    if (css_len_font_metric_of(unit, &metric)) {
        CssPx base = font->resolve(font->ctx, metric);

        DCHECK(base.px >= 0.0,
               "a css-values-4 §6.1.1 font-relative unit was resolved against a NEGATIVE base. Every base this "
               "callback answers is a computed `font-size` or a non-negative ratio of one, and css-fonts-4 "
               "§2.5 (Font size: the font-size property) states that property's range in its own `Value:` "
               "line — `<length-percentage [0,∞]>` — and again in words twice, so a negative one is a "
               "declaration the grammar should have DROPPED rather than a length to be a multiple of");
        DCHECK(base.px == base.px && base.px * 0.0 == 0.0,
               "a css-values-4 §6.1.1 font-relative unit was resolved against a base that is not a FINITE "
               "number — every value css-fonts-4 §2.5 admits is an absolute length off a real declaration, a "
               "ratio of one, or §2.5.1's table entry, and §6.1.1's assumed metrics are finite ratios of "
               "those, so a NaN or an infinity is a derivation that lost an operand rather than a size this "
               "multiplication can use");
        return css_px_scale(base, num);
    }
    /* §6.1.2's DEFAULT FAMILY AND §6.1.2.1's THREE VARIANTS REACH ONE ARM, because §6.1.2.2 defines every
       one of the twenty-four spellings over a `*` that stands for all four families — the SIZE the unit is a
       percentage of and the AXIS it is taken in are independent questions, and css_len_viewport asks them as
       two. A variant is recognised by the prefix test rather than tabulated, which is §6.1.2.1's own naming
       rule; what each family resolves against, and why `lv*` shares the default family's facts while `sv*`
       and `dv*` carry their own, is stated once at that function and at core/css/css_length.h's fact list. */
    if (css_len_is_viewport(unit) || css_len_is_viewport_variant(unit))
        return css_len_viewport(realm, unit, num / 100.0);
    else if (css_len_is_container(unit))
        DFAIL("a length in one of CSS Conditional 5 §7 Container Relative Lengths' six units (`cqw`, `cqi`, "
              "`cqmax`, …). §7 states what one is worth in two parts, and this engine has neither: \"container "
              "query length units are evaluated using the same rules as container size queries on the relevant "
              "axis (or axes) described by the unit. The query container for each axis is the nearest ancestor "
              "container that accepts container size queries on that axis. If no eligible query container is "
              "available, then use the small viewport size for that axis.\" THE SECOND HALF IS BUILT — "
              "§6.1.2.1's SMALL viewport size is a picked fact of its own in core/frame/viewport.c "
              "(`viewport_small_width`/`viewport_small_height`, carrying CSS_ENV_SMALL_VIEWPORT_*), which "
              "the `sv*` arm above resolves through and which this arm must resolve its FALLBACK through "
              "rather than through the default `v*` pair: one fact, two callers, and answering this one "
              "from the ICB's source key would delete the arm where the two viewport sizes differ. SO "
              "WHAT IS LEFT IS THE FIRST HALF AND ONLY IT: the `container-type` cascade and the "
              "nearest-ancestor-container walk that decides WHICH element answers for each axis, which is "
              "CSS Conditional 5 §3's own machinery and is a component of its own. Until an element can "
              "BE a query container, §7's antecedent holds for every element there is and this arm is the "
              "fallback outright rather than an approximation of it — which is the reading "
              "core/css/media_query.c's `mq_container_extent` already states and answers for a media "
              "query, and the reason this one crashes instead is that a computed value carries FACTS "
              "where a media query carries its own serialization as the key");
    else
        DFAIL("a unit that is not a length reached the length table; both callers admit the unit first "
              "(css_length_parse through css_len_unit_known, a math leaf through css_length_is_length_unit)");
    return css_px(0.0);
}

/* css-values-4 §10's MATH FUNCTIONS, answered over the table above. `css_math_eval` calls this only for a unit
   `css_length_is_length_unit` already admitted, so there is no "not a length" answer to give — the crash for a
   unit that cannot be absolutized is the same one a bare dimension reaches, in the same place. */
typedef struct { JSContext *realm; const CssFontMetrics *font; } CssLenMathCtx;

static CssPx css_len_math_length(void *ctx, double n, const char *unit, size_t unit_len)
{
    CssLenMathCtx *c = (CssLenMathCtx *)ctx;
    char lower[CSS_LEN_UNIT_MAX];
    size_t i;

    DCHECK(unit_len > 0 && unit_len < CSS_LEN_UNIT_MAX,
           "a math function's `<length>` leaf carries a unit longer than any specification defines as a "
           "`<length>`. `css_math_eval` reaches this callback only for a unit `css_length_is_length_unit` "
           "has already admitted, and that entry refuses everything from CSS_LEN_UNIT_MAX bytes up, so one "
           "arriving here is those two tests having come apart");
    if (unit_len == 0 || unit_len >= CSS_LEN_UNIT_MAX) return css_px(0.0);
    for (i = 0; i < unit_len; i++) lower[i] = (char)tolower((unsigned char)unit[i]);
    lower[unit_len] = '\0';
    return css_len_unit_px(c->realm, c->font, lower, n);
}

static bool css_len_unit_known(const char *unit);

bool css_length_parse(JSContext *realm, const CssFontMetrics *font, const char *value, CssKeywordSet keywords,
                      CssLength *result)
{
    CssLength out = { CSS_LENGTH_ABSOLUTE, css_px(0.0), 0.0, CSS_KEYWORD_INVALID };
    char unit[CSS_LEN_UNIT_MAX];
    const char *p = value;
    char *end = NULL;
    double num;

    DCHECK(value != NULL, "a CSS length was parsed from a NULL value — the cascade answers every property this "
                          "engine models, so a NULL here is a cascade that stopped early");
    DCHECK(font != NULL && font->resolve != NULL,
           "a CSS length was absolutized with no source for css-values-4 §6.1.1's `em` and `rem`. Which "
           "computed `font-size` a font-relative unit means is CSS Cascade §7.2's inheritance walk over the "
           "flattened element tree, and this component holds no tree — so the pair is the CALLER's to answer "
           "and is required of every caller, not supplied where it happens to be convenient. It is consulted "
           "lazily, so a caller that knows its value carries no font-relative unit still passes it rather than "
           "deciding for this file which arm it will take");
    while (*p != '\0' && isspace((unsigned char)*p)) p++;
    num = strtod(p, &end);
    /* NOT A NUMBER AT ALL. Either a KEYWORD or a FUNCTION, told apart by the one character that distinguishes
       them in CSS Syntax's own tokenizer. */
    if (end == p) {
        if (strchr(p, '(') != NULL) {
            CssLenMathCtx mc;
            CssMathResolver res;
            CssMathValue v;

            mc.realm = realm;
            mc.font = font;
            res.length_px = css_len_math_length;
            res.ctx = &mc;
            res.realm = realm;
            /* §10.9.1 "Calculation Contexts": "Math functions always inherit the calculation context from
               wherever they're used." A length-valued property is §10.9's own worked example of a context that
               resolves percentages against a `<length>` ("such as in width, where <percentage> is resolved
               against a <length>"), so `<length-percentage>` is the production asked for and `calc(25% + 50px)`
               is valid here for exactly the reason `width: 25%` is. */
            if (css_math_eval(p, strlen(p), &res, CSS_MATH_PROD_LENGTH_PERCENTAGE, &v)) {
                /* §10.11 "Computed Value": "Where percentages are not resolved at computed-value time, they are
                   not resolved in math functions, e.g. calc(100% - 100% + 1px) resolves to calc(0% + 1px), not
                   to 1px." So a surviving percentage TERM is the whole test and the percentage's NUMBER is not
                   — §10.10.1 "Simplification"'s "zero-valued terms cannot be simply removed from a Sum" is why
                   `css_math_eval` answers `pct_term` beside `pct`, and reading `pct != 0` here would turn that
                   worked example's `calc(0% + 1px)` into the `1px` the sentence says it is not. */
                if (v.pct_term) {
                    out.kind = CSS_LENGTH_CALCULATED;
                    out.pct = v.pct;
                }
                else {
                    out.kind = CSS_LENGTH_ABSOLUTE;
                }
                out.px = v.num;
                *result = out;
                return true;
            }
            /* Any other function — `rgb(1, 2, 3)`, a math function whose type is not `<length-percentage>`
               (`calc(1px + 1s)`) — is not a value of a length-valued property's grammar. */
            return false;
        }
        /* The identifier, less any whitespace the serialization left after it, mapped once. One the property's
           set does not hold, including one that is no keyword at all, fails the grammar. */
        {
            size_t n = strlen(p);

            while (n > 0 && isspace((unsigned char)p[n - 1])) n--;
            out.keyword = css_keyword_of(p, n);
        }
        if (!css_keyword_set_has(keywords, out.keyword)) return false;
        out.kind = CSS_LENGTH_KEYWORD;
        *result = out;
        return true;
    }
    while (*end != '\0' && isspace((unsigned char)*end)) end++;
    if (*end == '\0') {
        /* css-values-4 §6 "Distance Units: the <length> type": "For zero lengths the unit identifier is
           optional". A unitless non-zero is not a `<length>`; a property whose grammar has a `<number>` arm
           (`line-height`) takes it before asking this. */
        if (num != 0.0) return false;
        out.kind = CSS_LENGTH_ABSOLUTE;
        out.px = css_px(0.0);
        *result = out;
        return true;
    }
    if (*end == '%' && end[1] == '\0') {
        out.kind = CSS_LENGTH_PERCENTAGE;
        out.pct = num;
        *result = out;
        return true;
    }
    /* A unit longer than any identifier, or one §6 does not define as a length (an angle, a time, a
       frequency, a resolution, or trailing text), is not a `<length>`. */
    if (!css_len_unit_of(end, unit) || !css_len_unit_known(unit)) return false;
    out.kind = CSS_LENGTH_ABSOLUTE;
    out.px = css_len_unit_px(realm, font, unit, num);
    *result = out;
    return true;
}

/* Is `unit` (already lowercased) a LENGTH unit — CSS Values §6's absolute, font-relative and
   viewport-percentage families in all four of their spellings, and CSS Conditional 5 §7's six container-
   relative ones? All of them, because §6's `<length>` production admits every unit a specification defines as
   one, and the ones this engine cannot absolutize are a missing component rather than a syntax error. */
static bool css_len_unit_known(const char *unit)
{
    unsigned i;

    for (i = 0; i < CSS_LEN_N(CSS_ABSOLUTE); i++)
        if (strcmp(CSS_ABSOLUTE[i].unit, unit) == 0) return true;
    {
        CssFontMetric metric;

        /* All twelve of §6.1.1's font-relative units are rows of the one table now, so this is the whole
           font-relative family and there is no second list beside it to fall out of step with. */
        if (css_len_font_metric_of(unit, &metric)) return true;
    }
    return css_len_is_viewport(unit) || css_len_is_viewport_variant(unit) || css_len_is_container(unit);
}

bool css_length_is_length_unit(const char *unit, size_t unit_len)
{
    char lower[CSS_LEN_UNIT_MAX];
    size_t i;

    DCHECK(unit != NULL || unit_len == 0,
           "§6's unit set was asked about through a NULL span with a non-zero length — a dimension token names "
           "its unit inside the buffer it was tokenized from, so an absent pointer is a caller that lost it");
    /* Every unit any specification defines as a `<length>` is at most five bytes (`dvmin`, `cqmax`), so one
       that does not fit is not one of them and the copy the comparison needs is never made for it. */
    if (unit_len == 0 || unit_len >= CSS_LEN_UNIT_MAX) return false;
    for (i = 0; i < unit_len; i++) lower[i] = (char)tolower((unsigned char)unit[i]);
    lower[unit_len] = '\0';
    return css_len_unit_known(lower);
}

/* §6's production over serialized text, with `want` naming which of the two questions below is being asked —
   both walks are identical apart from what a `<percentage>` and a math function answer, and writing them twice
   is how the two would come to disagree about `calc(2em)`. */
static bool css_len_is(const char *value, CssMathProduction want)
{
    const char *p = value;
    char *end = NULL;

    DCHECK(value != NULL, "the `<length>` grammar was asked about a NULL value — a declaration carries a value "
                          "by the time lexbor has serialized it, and an absent one is a caller that lost it");
    while (*p != '\0' && isspace((unsigned char)*p)) p++;
    (void)strtod(p, &end);
    /* Not a number token at all: a FUNCTION is the one remaining production a `<length>` can take (see the
       header), and every other spelling — a keyword, an identifier, a string — is not one. WHICH function is
       css-values-4 §10.9 "Type Checking"'s question and is now answerable: a math function IS a `<length>` when
       its type matches one, `rgb(1, 2, 3)` is not a math function at all, and `calc(1px + 1s)` is one whose
       type is failure. This is the entry css_length.h said "cannot tell them apart for the same reason". */
    /* The `(` is CSS Syntax's own one-character difference between a FUNCTION and an IDENT, and it is tested
       here so that `auto`, `none` and every other keyword answer without standing a tokenizer up. */
    if (end == p) return strchr(p, '(') != NULL && css_math_matches(p, strlen(p), want);
    while (*end != '\0' && isspace((unsigned char)*end)) end++;
    if (*end == '\0') return true;    /* §6's unitless zero; the parse above asserts on a unitless non-zero */
    if (*end == '%')
        /* §5.5's `<percentage>` is a SIBLING production of §6's `<length>` and never a `<length>`; it is one
           arm of `<length-percentage>`, which is the other question. */
        return want == CSS_MATH_PROD_LENGTH_PERCENTAGE && end[1] == '\0';
    return css_length_is_length_unit(end, strlen(end));
}

bool css_length_is_length(const char *value)
{
    return css_len_is(value, CSS_MATH_PROD_LENGTH);
}

bool css_length_is_length_percentage(const char *value)
{
    return css_len_is(value, CSS_MATH_PROD_LENGTH_PERCENTAGE);
}

/* CSS Values §6's SNAP A LENGTH AS A LINE WIDTH, in the spec's own three steps, over the DEVICE PIXEL the realm
   reports. The arithmetic is stated in device pixels because the algorithm is — a CSS pixel is not a whole
   number of them at a ratio of 1.5 — and the answer converts back, which is why a `1px` border is 1 CSS pixel
   at a ratio of 1 and two thirds of one at 1.5.
   THE RESULT DERIVES FROM THE RATIO, and that is the half this could not say while the computed-value path was
   text. viewport.h makes `devicePixelRatio` a PICKED environment fact for the reason it gives — `> 1` is the
   retina gate — so a snapped width is a function of a source a page can branch on, and reporting the modelled
   `1px` as a number the author's own declaration determined would delete that arm exactly as a bare 1280
   deletes the mobile one. The combine is what carries it, and it is also what makes a width that is ITSELF
   derived (`border-width: 1vw`) come out a function of the viewport AND of the ratio — two facts in one length,
   which is one joint domain and not a choice between them. */
CssPx css_length_snap_line_width(JSContext *realm, CssPx len)
{
    double ratio, device, snapped, out;
    CssPx res;

    DCHECK(len.px == len.px && len.px * 0.0 == 0.0,
           "CSS Values §6's snap-as-a-line-width was handed a length that is not a FINITE number — every "
           "border width reaching it is an absolute length off a real declaration or one of §3.3's three "
           "keywords, so a NaN or an infinity is a derivation that lost an operand");
    /* THE RATIO IS THE OUTPUT DEVICE'S AND THE KEY IS THE DOCUMENT'S, which is why a realm is required here
       even though CSSOM VIEW §4's `devicePixelRatio` is answered without asking which document is on the
       device: the domain this length carries is keyed on the document that read it, exactly as the viewport's
       is (core/frame/viewport.c's seam), and an element no navigable presents has no document to key on. */
    if (realm == NULL)
        DFAIL("a `border-*-width` was SNAPPED for an element whose document is not the active document of any "
              "navigable — a DOMParser document, an XHR `responseXML`, a `<template>`'s contents owner, or the "
              "document of a destroyed navigable. css-backgrounds-3 §3.3 snaps the COMPUTED value, so the "
              "answer derives from the device pixel ratio and needs a realm to key that fact on. Every user "
              "agent answers the computed value for an element that generates no box, and CSSOM §9 reaches "
              "that answer through a conjunct it does not state: BUILD it over "
              "core/dom/element_view.h's `element_view_has_box`, which is the same one the viewport-percentage "
              "arm above and core/layout/used_value.c's uv_icb name");
    ratio = viewport_device_pixel_ratio(realm);
    device = len.px * ratio;
    DCHECK(ratio > 0.0,
           "core/frame/viewport.h answered a DEVICE PIXEL RATIO that is not positive. CSSOM VIEW §4 defines it "
           "as the CSS pixel size divided by the device pixel size and returns 1 where there is no output "
           "device, so a zero or a negative one is a model with a device of no size");
    /* Step 1: "if len is an integer number of device pixels, do nothing." Step 2: "if the absolute value of len
       is greater than zero, but less than 1 device pixel, round it away from zero to 1 or -1 device pixel."
       Step 3: "if the absolute value of len is greater than 1 device pixel, round it towards zero to the
       nearest integer number of device pixels." */
    if (device == trunc(device))              snapped = device;
    else if (device > -1.0 && device < 1.0)   snapped = device > 0.0 ? 1.0 : -1.0;
    else                                      snapped = trunc(device);
    out = snapped / ratio;
    /* The three steps are a step function of the ratio, which no polynomial states: the result is one atom,
       named by the snap over `len`, so every derivation of this width subtracts the same one. */
    res = css_px_combine(len, css_px_env(CSS_ENV_DEVICE_PIXEL_RATIO, realm, out), out);
    css_px_form_atom(&res, "snap-line-width", &len, 1);
    return res;
}

/* THE SHORTEST DECIMAL THAT NAMES THIS DOUBLE, as its digits and its decimal exponent: the value is
   `D[0].D[1..ndig-1] x 10^e10`, signed separately. Printed at RISING PRECISION until strtod gives the value
   back, which is CSSOM §6.7.2's "in the shortest form possible" stated directly rather than a fixed `%g` that
   would report `4.0000000000000004px` for one length and truncate the next.
   THE `%e` FORM IS READ RATHER THAN PRINTED: it is the one printf conversion whose output separates the
   significand from the exponent, so the layout below is a positional rendering of the digits and never has to
   ask printf for one. */
static void css_num_shortest(double v, char *digits, int *ndig, int *e10)
{
    char sci[40];
    const char *p;
    int prec, n = 0;

    for (prec = 1; prec <= 17; prec++) {
        snprintf(sci, sizeof sci, "%.*e", prec - 1, v);
        if (strtod(sci, NULL) == v) break;
    }
    DCHECK(prec <= 17, "a double did not round-trip through 17 significant digits, which is more than IEEE 754 "
                       "binary64 needs — the printer and the parser disagree");
    for (p = sci + (*sci == '-' ? 1 : 0); *p != 'e'; p++)
        if (*p != '.') digits[n++] = *p;
    digits[n] = '\0';
    DCHECK(n == prec, "the significand printf wrote does not carry the number of digits it was asked for — the "
                      "layout below indexes them against that count");
    DCHECK(n == 1 || digits[n - 1] != '0',
           "the shortest round-tripping significand ends in a ZERO, so dropping that digit would name the same "
           "double in one character less and the search above stopped a digit late — CSSOM §6.7.2 asks for the "
           "shortest form and this one is not it");
    *ndig = n;
    *e10 = (int)strtol(p + 1, NULL, 10);
}

/* CSSOM §6.7.2's SERIALIZE A `<number>`, entire: "A base-ten number using digits 0-9 in the SHORTEST FORM
   POSSIBLE, using '.' to separate decimals (if any), ROUNDING THE VALUE IF NECESSARY TO NOT PRODUCE MORE THAN
   6 DECIMALS, preceded by '-' if it is negative", with its own note that "scientific notation is not used".
 * BOTH CLAUSES ARE THE RULE AND NEITHER IS OPTIONAL, and this used to implement the first alone. CSS's
 * `<number-token>` grammar admits an exponent on INPUT — CSS Syntax §4.3.3 consumes one — so `width: 1e-7px`
 * is a declaration a page can write, and there is no notation to write the answer back in: `%g` produces one
 * for every value whose exponent is below -4 or at or above its precision, which is where the assert that
 * REJECTED an exponent was firing. Rejecting it was the wrong half of the rule; the digits are laid out
 * POSITIONALLY instead, and the six-decimal clause is what bounds that layout from below.
 * PRINTING `%.6f` FOR EVERY VALUE WOULD ANSWER THE SECOND CLAUSE AND BREAK THE FIRST: the double nearest 1e23
 * is 99999999999999991611392, and `%.6f` writes exactly that while the shortest form that names it is a 1 with
 * twenty-three zeros after it. So the shortest form is found first, and only a form needing more than six
 * decimals is re-rounded — which puts the value below 1e11, since more than six decimals means the exponent is
 * below `ndig - 7`, so the fixed-point print that rounds it is bounded and the shortest form of the ROUNDED
 * value is what gets laid out.
 * `suffix` is the unit CSSOM §6.7.2 writes after the number — the only thing that differs between a `<length>`
 * and a `<percentage>`, and, since css_length.h made this entry public, between either of those and any other
 * CSS value whose serialization is stated over the same §6.7.2 sentence (CSS Typed OM 1 §6.4's `<angle>`,
 * `<time>`, `<frequency>` and `<resolution>` unit values reach it here). The buffer holds the widest positional
 * form a finite double can take: a sign, the 309 integer digits of DBL_MAX, a point, six decimals and the
 * unit — and the unit is bounded by the DCHECK below rather than by that arithmetic, because a caller outside
 * this file chooses it. */
#define CSS_NUM_MAX 336

char *css_length_serialize_number(double v, const char *suffix)
{
    char digits[24], rounded[32], out[CSS_NUM_MAX];
    int ndig, e10, decimals, i, n = 0;

    DCHECK(v == v && v * 0.0 == 0.0,
           "a value that is not a FINITE number reached CSSOM §6.7.2's serializer. Every length and every "
           "percentage this engine reports is off a real declaration or arithmetic over such values, so a NaN "
           "or an infinity is a derivation that lost an operand rather than a value to print");
    /* THE UNIT IS THE ONE PART OF THE LAYOUT A CALLER OUTSIDE THIS FILE CHOOSES, so its width is asserted here
       rather than reasoned about at each call site. `CSS_NUM_MAX` is the sign, DBL_MAX's 309 integer digits,
       the point, the six decimals the clause below bounds the fraction to, and the NUL — everything left over
       is the unit's, and the longest one any CSS specification defines is five bytes (`dvmin`, `cqmax`). */
    DCHECK(suffix != NULL, "CSSOM §6.7.2's serializer was asked to write a number followed by a NULL unit — "
                           "the empty unit is a real answer (§6.4's \"number\" arm writes nothing after the "
                           "digits) and is spelled \"\", so a NULL is a caller that lost the string");
    DCHECK(strlen(suffix) < (size_t)(CSS_NUM_MAX - 318),
           "a unit longer than CSSOM §6.7.2's positional layout leaves room for. The buffer is sized from "
           "DBL_MAX's decimal width plus the six-decimal clause, so a unit this wide would be written past its "
           "end — and no unit any CSS specification defines is anywhere near it, which makes this a caller "
           "passing something that is not a unit identifier");
    css_num_shortest(v, digits, &ndig, &e10);
    if (ndig - 1 - e10 > 6) {
        snprintf(rounded, sizeof rounded, "%.6f", v);
        v = strtod(rounded, NULL);
        css_num_shortest(v, digits, &ndig, &e10);
        DCHECK(ndig - 1 - e10 <= 6,
               "a value rounded to six decimal places came back from the shortest-form search needing more "
               "than six — the fixed-point print and the round-trip search disagree about what the rounded "
               "value is");
    }
    decimals = ndig - 1 - e10;
    DCHECK(e10 <= 308 && decimals <= 6,
           "the two facts that bound §6.7.2's positional layout to the buffer sized from DBL_MAX's decimal "
           "width — a FINITE double's decimal exponent, and the six-decimal clause applied above — do not both "
           "hold, so the layout below would write past it");
    /* "preceded by `-` if it is NEGATIVE" is a statement about the NUMBER, and negative zero is zero: CSS has
       no signed zero to distinguish, and a value the six-decimal clause rounded down to nothing keeps the sign
       bit of the length it came from. `v < 0.0` is the question the sentence asks; the printed sign is not. */
    if (v < 0.0) out[n++] = '-';
    if (e10 < 0) {
        /* Wholly fractional: §6.7.2's leading zero, then the exponent's own zeros, then every digit. */
        out[n++] = '0';
        out[n++] = '.';
        for (i = 0; i < -e10 - 1; i++) out[n++] = '0';
        for (i = 0; i < ndig; i++) out[n++] = digits[i];
    }
    else {
        /* The integer part is the first e10 + 1 digit positions, padded with the zeros a positional rendering
           of a large exponent needs and the shortest significand does not carry. */
        for (i = 0; i <= e10; i++) out[n++] = i < ndig ? digits[i] : '0';
        if (decimals > 0) {
            out[n++] = '.';
            for (i = e10 + 1; i < ndig; i++) out[n++] = digits[i];
        }
    }
    for (i = 0; suffix[i] != '\0'; i++) out[n++] = suffix[i];
    out[n] = '\0';
    return css_len_strdup(out);
}

char *css_length_serialize_px(double px)   { return css_length_serialize_number(px, "px"); }
char *css_length_serialize_pct(double pct) { return css_length_serialize_number(pct, "%"); }

/* §10.13's Sum branch over the two-term residue — see css_length.h for why the shape is pinned rather than
   chosen. The percentage is the FIRST child (sort a calculation's children nodes puts a number, then a
   percentage, then the dimensions), so the " - " rule is asked only of the length. */
char *css_length_serialize_calc(double pct, double px)
{
    char *pct_s = css_length_serialize_pct(pct);
    /* "if child is a NEGATIVE numeric value, append ' - ' to s, then serialize the NEGATION of child as
       normal" — so the sign is carried by the operator and never by the number after it. */
    char *px_s = css_length_serialize_px(px < 0.0 ? -px : px);
    size_t n = strlen(pct_s) + strlen(px_s) + sizeof("calc( + )");
    char *out = malloc(n);

    CHECK(out != NULL, "cssom: OOM serializing css-values-4 §10.13's math-function residue — a dropped one "
                       "would read as an undeclared property rather than as a `calc()`");
    snprintf(out, n, "calc(%s %c %s)", pct_s, px < 0.0 ? '-' : '+', px_s);
    free(pct_s);
    free(px_s);
    return out;
}

/* ---- css-values-4 §10.11 "Computed Value"'s USED-VALUE-TIME SIMPLIFICATION (see css_length.h) ------------- */

CssPx css_length_resolve_pct_at(CssLength len, CssPx basis, const char *file, int line)
{
    /* THE ADDRESS IS ASSERTED AND NOT TOLERATED. Every caller reaches this entry through
       css_length.h's macro, which spells __FILE__ at the call, so a NULL here is a hand-written
       call that bypassed the one spelling — and the address IS the claim these refusals make
       rather than decoration on it. */
    DCHECK(file != NULL,
           "a percentage was resolved with no caller site. core/css/css_length.h's macro spells "
           "__FILE__ and __LINE__ at the call and `CSS_LENGTH_SITE_INTERNAL` is what a path inside "
           "this file types instead, so neither of the two ways to reach this entry can pass NULL");
    /* THE OPERAND AND THE SITE, BECAUSE THE REMEDY IS AT THE CALLER AND THERE ARE SIXTEEN OF THEM. Which
       KIND arrived decides which repair: a `CSS_LENGTH_KEYWORD` is a grammar arm the caller's own dispatch
       let through and the keyword names it (`auto`, `none`, a css-sizing-3 §3.1.1 intrinsic size), while a
       `CSS_LENGTH_ABSOLUTE` reading exactly 0 with an EMPTY fact set is the signature of a zeroed `CssLength`
       rather than of a mis-dispatched real length — `CSS_LENGTH_ABSOLUTE` is 0, so a struct nobody wrote
       reads as one — and those two take opposite repairs, one at the dispatch and one at the producer. */
    DCHECKF(len.kind == CSS_LENGTH_PERCENTAGE || len.kind == CSS_LENGTH_CALCULATED,
            "%s:%d handed css-values-4 §10.11's used-value-time resolution a `%s` (keyword `%s`, length %g "
            "CSS px): a value with NO percentage in it. The two kinds that carry one are the whole of what a "
            "basis means, and an absolute length resolved against a containing block would be scaled by a "
            "measure it does not depend on — so this is a caller that dispatched on something other than the "
            "kind",
            file, line,
            len.kind == CSS_LENGTH_ABSOLUTE  ? "CSS_LENGTH_ABSOLUTE"
            : len.kind == CSS_LENGTH_KEYWORD ? "CSS_LENGTH_KEYWORD"
                                             : "kind outside the four css_length.h defines",
            len.kind == CSS_LENGTH_KEYWORD ? css_keyword_name(len.keyword) : "-",
            len.px.px);
    /* A `CSS_LENGTH_PERCENTAGE` COMES FROM ONE PRODUCTION AND NEVER FROM A MATH FUNCTION, which is why this
       arm's single expression is right: css-values-4 §5.5 "Percentages: the <percentage> type" is a bare
       `<percentage>` with no second term to state, and `css_length_parse` writes it over a zeroed pair.
       css-values-4 §10.10.1 "Simplification" is NOT the authority here, and reading it as one is the mistake
       that stood at this line. ONLY THE AIM WAS WRONG, AND THE REPAIR SAID OTHERWISE: it called the quotation
       that stood here a paraphrase whose words occur nowhere in §10.10.1, and they are that section's own Note,
       verbatim — "zero-valued terms cannot be simply removed from a Sum; they can only be combined with other
       values that have identical units" — which engine/citegen.mjs resolved by the stated title and VERIFIED
       against the section's text. A quotation check answers about SEVEN WORDS and never about the paragraph
       they were put in, so a comment caught arguing the wrong section is not thereby a comment that invented
       its sentence; the two are independent, and only the aim was ever wrong here.
       §10.10.1's Sum rule is "for each set of root's children that are numeric values with identical units,
       remove those children and replace them with a single numeric value containing the sum of the removed
       nodes and with the same unit", and it governs a MATH FUNCTION'S residue: a math function reaching this
       component is `CSS_LENGTH_CALCULATED` or `CSS_LENGTH_ABSOLUTE`, so the kind the assert below is about is
       unreachable through §10.10.1 altogether. What that assert is really about is a PRODUCER writing both
       halves of a pair only one of which this kind declares: this
       expression is only right if the unused half is the additive identity WITH AN EMPTY FACT SET. A length
       term left there would be added silently; an environment fact left there would be UNIONED into the
       result's domain and fork a world the value does not depend on. */
    /* THE SITE IS CARRIED HERE TOO AND IT IS NOT THE ADDRESS TO REPAIR, which is why it is worded as a route
       rather than as a culprit: the defect this one names is a PRODUCER's, and the caller is printed only
       because it names the PROPERTY whose parse to go and read. */
    DCHECKF(len.kind != CSS_LENGTH_PERCENTAGE || (len.px.px == 0.0 && len.px.env == CSS_ENV_NONE),
            "a `<percentage>` computed value carries a LENGTH term (%g CSS px), reached through %s:%d. "
            "css_length_parse writes the percentage arm over a zeroed pair and css-values-4 §5.5 "
            "\"Percentages: the <percentage> type\" has no second term to write, so this is a PRODUCER that "
            "set `pct` on a value it had already given a length — the repair is at whichever parse wrote it, "
            "and the site named here points only at the property to go and read",
            len.px.px, file, line);
    /* §5.6's "both values are converted to absolute lengths and added", as ONE expression — the scaled basis
       carries the basis's facts, the length term carries its own, and `css_px_add` is what makes the answer a
       joint function of both rather than of whichever operand was resolved last. */
    return css_px_add(css_px_scale(basis, len.pct / 100.0), len.px);
}
