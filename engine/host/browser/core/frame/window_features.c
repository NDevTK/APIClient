/* HTML §7.4's features argument — see window_features.h. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/frame/window_features.h"

/* §7.4: a FEATURE SEPARATOR is ASCII whitespace, U+003D (=) or U+002C (,). Named because the tokenizer below
   asks the question five times and the spec asks it by name. */
static bool feat_sep(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '=' || c == ',';
}

static char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

/* §7.4's NORMALIZING THE FEATURE NAME — four legacy aliases, and they are part of the algorithm rather than a
   compatibility shim: `screenx` IS `left` to the spec, and the corpus tests both spellings against the same
   expectation (open-features-tokenization-screenx-screeny). */
static const char *feat_normalize(const char *name)
{
    if (!strcmp(name, "screenx"))     return "left";
    if (!strcmp(name, "screeny"))     return "top";
    if (!strcmp(name, "innerwidth"))  return "width";
    if (!strcmp(name, "innerheight")) return "height";
    return NULL;   /* not an alias — the caller keeps the name it already owns */
}

/* §7.4's PARSE A BOOLEAN FEATURE, exactly. The empty string is TRUE — which is what makes bare `popup` and
   `noopener` work — and anything that is not "yes"/"true" is read as an INTEGER, with a parse failure meaning
   0 and therefore false. So `left=141` is true, `popup=0` is false, and `popup=banana` is false. */
static bool feat_parse_bool(const char *value)
{
    long parsed;
    char *end;

    if (!*value)                return true;
    if (!strcmp(value, "yes"))  return true;
    if (!strcmp(value, "true")) return true;
    /* §7.4 says "parsing value as an integer", which is §2.4.4.1's rule: optional sign, then ASCII digits, and
       an error if there are no digits. strtol's trailing-garbage tolerance is the same shape — "141abc" parses
       as 141 — and its failure is `end == value`, which the spec's error is. */
    parsed = strtol(value, &end, 10);
    if (end == value) parsed = 0;
    return parsed != 0;
}

/* THE TOKENIZED MAP, as a flat list. The spec's map is ORDERED with later entries overwriting earlier ones by
   name, so a list with a linear lookup IS the ordered map — a hash here would be machinery for six entries.
   THE ENTRIES AND THE STRINGS ARE HEAP-OWNED AND UNBOUNDED. They were `char name[32]; char value[32];` in a
   32-slot array, and all three of those limits changed answers rather than merely dropping tails: two names
   agreeing in their first 31 characters became ONE entry with the later value, so a page could overwrite an
   entry it never named; a value whose significant digits sat past the cut parsed differently from the value the
   page wrote (`left=` followed by thirty zeroes and a one is TRUE and truncates to FALSE); and the 33rd entry
   vanished, which decides the popup check outright when the entry it dropped was `popup`. A cap that changes an
   answer is not a tail being dropped, and the map the algorithm is defined over has no size in it. */
typedef struct { char *name; char *value; } Feature;
typedef struct { Feature *e; int n, cap; } FeatureMap;

/* An ASCII-lowercased copy of exactly `len` bytes. Both the name and the value are lowercased by §7.4, and both
   are page-controlled and of page-controlled length, so the length is measured and the copy is sized to it. */
static char *feat_dup_lower(const char *s, size_t len)
{
    char *d = malloc(len + 1);
    size_t i;
    CHECK(d != NULL, "window feature token allocation failed");
    for (i = 0; i < len; i++) d[i] = ascii_lower(s[i]);
    d[len] = 0;
    return d;
}

static void feat_map_init(FeatureMap *m) { m->e = NULL; m->n = 0; m->cap = 0; }

static void feat_map_free(FeatureMap *m)
{
    int i;
    for (i = 0; i < m->n; i++) { free(m->e[i].name); free(m->e[i].value); }
    free(m->e);
    feat_map_init(m);
}

/* TAKES OWNERSHIP of both strings, on every path. The map is the only owner once they are handed over, and a
   replace frees the value it displaced — the name it displaced is the one already stored, so the caller's name
   is freed instead and the map's key stays the pointer the earlier entry established. */
static void feat_set(FeatureMap *m, char *name, char *value)
{
    int i;
    /* §7.4's map is keyed by name: a repeated name REPLACES, which is what `,left=1,left=2,` means. */
    for (i = 0; i < m->n; i++) {
        if (!strcmp(m->e[i].name, name)) {
            free(m->e[i].value);
            m->e[i].value = value;
            free(name);
            return;
        }
    }
    if (m->n == m->cap) {
        int cap = m->cap ? m->cap * 2 : 8;
        Feature *e = realloc(m->e, (size_t)cap * sizeof *e);
        /* A dropped feature is a different popup answer and a different navigable, so this is one of the
           allocation failures CLAUDE.md names as always-fatal rather than a dev-only invariant. */
        CHECK(e != NULL, "window feature map growth failed");
        m->e = e;
        m->cap = cap;
    }
    m->e[m->n].name = name;
    m->e[m->n].value = value;
    m->n++;
}

/* §7.4 REMOVES `noopener` and `noreferrer` FROM THE MAP once it has read them, and that removal is not
   bookkeeping — the popup check's FIRST step is "if tokenizedFeatures is empty, return false", so
   `open(url, name, "noopener")` leaves an EMPTY map and is a TAB. Without the removal the map still holds one
   entry, the emptiness test never fires, and every chrome default reads false: 29 subtests that expected a tab
   got a popup. */
static void feat_remove(FeatureMap *m, const char *name)
{
    int i;
    for (i = 0; i < m->n; i++) {
        if (!strcmp(m->e[i].name, name)) {
            free(m->e[i].name);
            free(m->e[i].value);
            /* The map is ORDERED and the order is the page's; closing the gap preserves it. */
            memmove(&m->e[i], &m->e[i + 1], (size_t)(m->n - i - 1) * sizeof m->e[0]);
            m->n--;
            return;
        }
    }
}

static const char *feat_get(const FeatureMap *m, const char *name)
{
    int i;
    for (i = 0; i < m->n; i++)
        if (!strcmp(m->e[i].name, name)) return m->e[i].value;
    return NULL;
}

/* §7.4's CHECKING IF A WINDOW FEATURE IS SET: the parsed boolean when the name is present, the default when it
   is not. The DEFAULTS differ per feature and they are not decoration — `resizable` defaults to TRUE and every
   other one this reads defaults to false, which is why `open(url, name, "resizable=no")` is a popup and
   `open(url, name, "location=yes,toolbar=yes,menubar=yes,scrollbars=yes,status=yes")` is a tab. */
static bool feat_is_set(const FeatureMap *m, const char *name, bool dflt)
{
    const char *v = feat_get(m, name);
    return v ? feat_parse_bool(v) : dflt;
}

/* §7.4's TOKENIZE THE FEATURES ARGUMENT, step for step. The structure below is the spec's loop and not a
   paraphrase of it: the two inner walks (past a run of separators looking for `=`, then past a run of
   separators before the value) are what make `left==141` set `left` to the empty string — true — while
   `,left=141,,` sets it to "141", and a single split on ',' then '=' gets both wrong. */
static void feat_tokenize(const char *features, FeatureMap *m)
{
    const char *p = features, *end = features + strlen(features);

    while (p < end) {
        const char *name_at, *value_at = NULL;
        size_t name_len, value_len = 0;
        const char *alias;
        char *name, *value;

        /* Step 3.3: skip a run of feature separators. */
        while (p < end && feat_sep(*p)) p++;
        /* Step 3.4: the NAME is everything up to a separator or `=`, ASCII-lowercased. Its EXTENT is taken
           first and the copy is sized from it, so the name the map stores is the name the page wrote. */
        name_at = p;
        while (p < end && !feat_sep(*p)) p++;
        name_len = (size_t)(p - name_at);

        /* Step 3.6: advance to the `=`, stopping at a `,` or at anything that is not a separator — so
           `left , = 141` still reaches the `=` while `left , 141` does not. */
        while (p < end && *p != '=') {
            if (*p == ',' || !feat_sep(*p)) break;
            p++;
        }

        /* Step 3.7: if we are standing on a separator, the VALUE follows — after skipping the run of
           separators that is not a comma. */
        if (p < end && feat_sep(*p)) {
            while (p < end && feat_sep(*p)) {
                if (*p == ',') break;
                p++;
            }
            value_at = p;
            while (p < end && !feat_sep(*p)) p++;
            value_len = (size_t)(p - value_at);
        }

        /* Step 3.8: an empty name contributes nothing — which is what makes `,,,` and trailing commas inert. */
        if (name_len == 0) continue;

        name = feat_dup_lower(name_at, name_len);
        value = value_at ? feat_dup_lower(value_at, value_len) : feat_dup_lower("", 0);
        /* The four legacy aliases REPLACE the name, so the tokenized one is released here rather than leaking
           behind a literal the map would then own inconsistently with every other key. */
        alias = feat_normalize(name);
        if (alias) { free(name); name = feat_dup_lower(alias, strlen(alias)); }
        feat_set(m, name, value);
    }
}

/* §7.4's CHECK IF A POPUP WINDOW IS REQUESTED, over what is LEFT after steps 6-8 removed the two.
   Step 1: an empty map is a TAB. Step 2: `popup` decides on its own when present. Otherwise the question is
   whether the features ask for the FULL set of chrome, and any one missing makes it a popup — so the DEFAULTS
   are the whole of the answer and they are written beside each name. `resizable` defaults TRUE and every other
   one defaults false, which is why `open(url, name, "resizable=no")` is a popup and
   `location=yes,toolbar=yes,menubar=yes,scrollbars=yes,status=yes` is a tab.
   IT IS A FUNCTION because the map is now heap-owned: the four early returns this used to be spelled as each
   needed the release repeated, and a release repeated at four exits is the shape that grows a fifth exit
   without one. */
static bool feat_popup_requested(const FeatureMap *m)
{
    const char *v;

    if (m->n == 0) return false;
    if ((v = feat_get(m, "popup")) != NULL) return feat_parse_bool(v);
    {
        bool location = feat_is_set(m, "location", false);
        bool toolbar  = feat_is_set(m, "toolbar",  false);
        return !(location || toolbar)
            || !feat_is_set(m, "menubar",    false)
            || !feat_is_set(m, "resizable",  true)
            || !feat_is_set(m, "scrollbars", false)
            || !feat_is_set(m, "status",     false);
    }
}

WindowFeatures window_features_parse(const char *features)
{
    WindowFeatures f = { false, false, false };
    FeatureMap m;
    const char *v;

    feat_map_init(&m);
    /* §7.4 takes `optional DOMString features = ""`, so an absent third argument is the empty string and
       tokenizes to nothing. A caller that has none passes NULL. */
    if (!features || !*features) return f;

    feat_tokenize(features, &m);

    /* §7.4 steps 6-8, IN THIS ORDER: read each of the two, REMOVE it from the map, then let `noreferrer` imply
       `noopener` — a window with no referrer has no opener either, because the opener is how it would have
       one. The removal is what makes the popup check below see the map the spec hands it. Each value is PARSED
       before its entry is removed, because the removal frees the bytes `v` points at. */
    if ((v = feat_get(&m, "noopener")) != NULL)   { f.noopener   = feat_parse_bool(v); feat_remove(&m, "noopener"); }
    if ((v = feat_get(&m, "noreferrer")) != NULL) { f.noreferrer = feat_parse_bool(v); feat_remove(&m, "noreferrer"); }
    if (f.noreferrer) f.noopener = true;

    /* A WINDOW WITH NO OPENER IS NOT A POPUP, and this one is stated from the corpus rather than from my
       reading of the algorithm above — which is why it is written out here instead of folded in silently.
       window-open-popup-behavior.html asserts a TAB for EVERY features string with `noopener` or `noreferrer`
       in it, including `,noopener,noreferrer,popup` where `popup` is explicit and would otherwise decide on its
       own. Tokenize-remove-and-check does not produce that: after the removal those strings still carry chrome
       features and the check would return a popup. The test is the spec editor's own and Chrome passes it, so
       the observable rule is this one; if §7.4's prose turns out to place it elsewhere (in the rules for
       choosing a navigable, which is where `noopener` changes what gets created), it moves there rather than
       changing what it says. */
    if (!f.noopener) f.is_popup = feat_popup_requested(&m);

    feat_map_free(&m);
    return f;
}
