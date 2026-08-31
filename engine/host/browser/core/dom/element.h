/* The Element interface — Blink core/dom. One JS object per Lexbor element, per document. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_ELEMENT_H
#define ENGINE_HOST_BROWSER_CORE_DOM_ELEMENT_H
#include <stddef.h>
#include <lexbor/dom/dom.h>
#include "quickjs.h"
#include "core/idl_args.h"
/* §2.3.3's keyword/state machinery, for the REFLECT_ENUM row below. The reflection registry is HTML's §2.6.1
   already — its URL and unsigned long models are §2.6.2's `[ReflectURL]` and `[ReflectRange]` — so this is the
   same layer the file has always spoken and not a new dependency for the DOM. */
#include "core/html/enumerated_attribute.h"

void    element_init(JSContext *ctx);
void    element_free(JSRuntime *rt);

/* HTML §13.4's FRAGMENT PARSE, and WHICH MEMBER is driving it — the magic every declaration of the one machine
   carries. Five members over one parse: they differ in the TARGET whose children the fragment replaces, in the
   CONTEXT element the markup is parsed in, and in whether §13.4's `allowDeclarativeShadowRoots` is true (which
   is what makes `<template shadowrootmode>` inside the markup a real shadow root, and which only the two
   `Unsafe` members pass). ShadowRoot's two are declared HERE rather than in a second machine beside it,
   because §8.5.4's and §8.5.2's steps for a ShadowRoot receiver are the same algorithm over a different
   target — the difference the standard states is §13.4 step 2's "otherwise target's host". */
enum {
    ELEMENT_SET_INNER_HTML = 0,   /* §8.5.4's innerHTML setter, on Element */
    ELEMENT_SET_OUTER_HTML,       /* §8.5.5's outerHTML setter */
    ELEMENT_SET_HTML_UNSAFE,      /* §8.5.2's setHTMLUnsafe, on Element */
    SHADOW_ROOT_SET_INNER_HTML,   /* §8.5.4's innerHTML setter, on ShadowRoot */
    SHADOW_ROOT_SET_HTML_UNSAFE,  /* §8.5.2's setHTMLUnsafe, on ShadowRoot */
    /* §8.5.2's `setHTML` — the SAFE member — on the same two interfaces. It is the same machine and not a
       sixth algorithm: §8.6.4 set and filter HTML's `set and filter HTML` is what all four of these are, and `safe` is the one
       argument that differs. What `safe` decides is stated where §8.6.4 set and filter HTML states it — the sanitizer the options
       resolve to, and §8.6.4 `sanitize`'s step 3 removal of what is unsafe from it — `sanitize`'s OWN step 3,
       not `set and filter HTML`'s, whose step 3 is the safe-`script` early return. */
    ELEMENT_SET_HTML,             /* §8.5.2's setHTML, on Element */
    SHADOW_ROOT_SET_HTML,         /* §8.5.2's setHTML, on ShadowRoot */
};
const IdlStepDecl *element_set_html_decl(void);
/* §8.5.2's `setHTMLUnsafe` DECLARED for one of the two interfaces that have it — one argument list and one
   dictionary, because the IDL states the same line on Element and on ShadowRoot. Returns the step id. */
int element_declare_set_html_unsafe(JSContext *ctx, int magic);
/* §8.5.2's `setHTML` for one of the same two interfaces. Its own declaration and not a magic on the one above,
   because the IDL is a DIFFERENT line: `setHTML(DOMString html, optional SetHTMLOptions options = {})` takes no
   TrustedHTML union (there is nothing to trust — the sanitizer is what makes it safe) and its dictionary has
   ONE member where SetHTMLUnsafeOptions has two. Returns the step id. */
int element_declare_set_html(JSContext *ctx, int magic);

/* The wrapper for `el`, or JS_NULL. The SAME Lexbor element always yields the SAME JS object: a page compares
   nodes by identity constantly, and a fresh wrapper per lookup makes every such comparison silently false. */
JSValue element_wrap(JSContext *ctx, lxb_dom_element_t *el);

/* Element.prototype, borrowed — what HTMLElement is built on top of. */
/* §4.9's prototype for ONE realm — declared into core/realm.h's list. */
void element_install_proto(JSContext *ctx);
/* PER REALM. OWNED: the caller frees. */
JSValue element_proto(JSContext *ctx);

/* A [Reflect]ed content attribute: the pair of names that IS the reflection, plus which kind of value it holds.
   A STRING reflection is the attribute's value ("" when absent); a BOOLEAN one is its PRESENCE, because
   `<input disabled="false">` is disabled and a string reflection would report the word.
   A NULLABLE STRING is HTML §2.6.1's `DOMString?` processing model and it is a THIRD kind rather than the
   first one read leniently: its getter answers NULL for an absent attribute where `DOMString` answers "", and
   its setter DELETES the attribute for null where `DOMString` would write the four characters "null". Every
   one of ARIAMixin's 44 string members is this kind, and `el.ariaLabel === null` is how a page tests one.
   A URL is HTML §2.6.1's "type USVString, treated as a URL" — §2.6.2's `[ReflectURL]`, which that section says
   may appear on nothing but a `USVString`. Its getter RESOLVES: encoding-parse-and-serialize the attribute
   value against the ELEMENT'S NODE DOCUMENT, so `<img src="/x">` reads back `http://host/x` and not `/x`. A
   REFLECT_STRING row for one of these is not a lenient reading, it is a different value — the mirror answers
   every relative URL wrong, silently, and twelve members were declared that way until they were corrected in
   core/html/html_element.c.

   THERE IS NO KIND FOR `[ReflectSetter]`, AND ADDING ONE WOULD BE A HALF-IMPLEMENTED ROW. §2.6.2 gives that
   extended attribute one meaning: the SETTER reflects, and the getter is the member's OWN algorithm — so those
   members are not registry rows at all, they are components. `<a>`/`<area>` `href` is §4.6.3's URL record
   serialized, shared with the ten sibling members that re-parse and write the same attribute back, and it falls
   back to the RAW attribute where §2.6.1 falls back to a scalar value string; `<base>` `href` falls back to the
   fallback base URL; `form.action` falls back to the document's URL. A `REFLECT_SETTER_ONLY` kind could carry
   the setter and would have nothing to answer the getter with, so it would answer the mirror — which is exactly
   the wrong-value-that-passes-a-presence-test the twelve rows above were. This is written here because the
   count invites the mistake: 10 DOMString + 7 double + 6 USVString + 4 unsigned long + `tabIndex`'s long are
   unmodelled and they look like a pattern. They are twenty-eight component getters.

   A KIND MUST ANSWER BOTH DIRECTIONS FROM THE ATTRIBUTE ALONE. That is the test for whether something belongs
   in this enum, and it is also why core/html/global_attributes.c's enumerated globals are not rows: their
   getters walk the TREE — an ancestor chain for `translate`, `spellcheck`, `writingSuggestions` and
   `contentEditable`, the element's FORM OWNER for `autocorrect` and for §6.8.7's own autocapitalization hint.
   THE TEST CUTS BOTH WAYS AND THE OTHER SIDE IS WHERE ROWS WENT MISSING: being an ENUMERATED attribute is not
   what puts a member in global_attributes.c. `dir`, `inputMode`, `enterKeyHint` and `popover` are enumerated
   and their getters walk nothing — each is §2.6.1's limited-to-only-known-values reflection of one attribute on
   one element — so they are REFLECT_ENUM rows, and they were REFLECT_STRING rows answering raw bytes for as
   long as the enum had no kind for them.

   AN UNSIGNED LONG is §2.6.1's `unsigned long` model, and it is the first kind whose answer is not the
   attribute's bytes: §2.3.4.2's rules parse them and the reflection then applies its OWN range. The range and
   the default are §2.6.2's `[ReflectRange]` and `[ReflectDefault]`, declared per row below, and they are NOT
   the same mechanism — a value outside a RANGE is pulled to the nearest end, a value that fails to PARSE falls
   to the default. `<td colspan="0">` is 1 (the range starts at 1) and `<td colspan="x">` is also 1 (the
   default), by two different steps that happen to agree here and do not for `rowspan`, whose range starts at
   0 and whose default is 1.

   AN ENUM is §2.6.1's "LIMITED TO ONLY KNOWN VALUES", and it is a KIND rather than a component for exactly the
   reason stated three paragraphs up: it answers both directions from the attribute alone. Its setter is
   §2.6.1's plain "set the content attribute with the given value" — the same one `DOMString` runs, which is why
   there is no separate setter body below — and its getter is the section's own two branches over §2.3.3's
   determine-the-state: "if contentAttributeValue does not correspond to any state of attributeDefinition ..., or
   if it is in a state of attributeDefinition with no associated keyword value, then return the empty string.
   Return the canonical keyword for the state ... that contentAttributeValue corresponds to."
   A REFLECT_STRING ROW FOR ONE OF THESE IS A DIFFERENT VALUE AND NOT A LENIENT READING, the same way a
   REFLECT_URL row was: the mirror answers the attribute's RAW BYTES, so `<div dir="RTL">.dir` read "RTL" where
   §3.2.6.4 answers "rtl" and `<div dir="banana">.dir` read "banana" where it answers "". The IDL cannot state
   this — "limited to only known values" is prose, so §2.6.2 gives it no extended attribute — which is why the
   spec-versus-tree audit that catches a missing member cannot see it, and why the whole class had to be found
   by reading §2.6.1 against every row rather than by any gate.
   AND THE ENUM HAS A NULLABLE TWIN, because §2.6.1 states the limited-to-only-known-values branch TWICE and
   the two endings differ. `DOMString`'s is "then return the empty string"; `DOMString?`'s is "then return
   null", and its setter is the nullable string's — "If the given value is null, then run this's delete the
   content attribute." So a `DOMString?` enumerated member declared REFLECT_ENUM is wrong in BOTH directions,
   and the four `crossOrigin` rows were: §2.5.4's No CORS state has no keyword and IS the missing value
   default, so `<video>.crossOrigin` answered "" where a browser answers null — and `img.crossOrigin === null`
   is exactly how a page tests it — while `img.crossOrigin = null` wrote the four characters "null" into the
   attribute instead of removing it. It is a KIND rather than a flag beside REFLECT_ENUM for the reason every
   other row here is one: the pair (kind, definition) is what the declaration asserts over, and a boolean the
   assert did not cover would be a third thing a row could get wrong silently.

   THE DEFINITION IS §2.3.3'S, NOT THIS TABLE'S: a row points at an `EnumeratedAttribute` that the section
   defining the attribute owns, so `method` and `formmethod` share one keyword table and differ only in their
   defaults, and the six interfaces that reflect `referrerpolicy` share one definition rather than six copies. */
enum { REFLECT_STRING = 0, REFLECT_BOOL, REFLECT_STRING_NULLABLE, REFLECT_URL, REFLECT_ULONG,
       REFLECT_ENUM, REFLECT_ENUM_NULLABLE };
/* The numeric fields are TRAILING so that every row declaring none of them is unchanged — an omitted brace
   initialiser zeroes them, and each is read only through the `has_` flag beside it. That flag is a POSITIVE
   statement that the IDL declares no default/range, never a hole a `?:` fills: §2.6.1's steps ask "if the
   reflected IDL attribute HAS a default value", so absence is one of the algorithm's own branches.
   `en` IS THE SAME KIND OF POSITIVE STATEMENT and its `has_` flag is the KIND: it is non-NULL exactly when the
   row is REFLECT_ENUM, asserted at the declaration in both directions — a row that carries a definition on
   another kind would silently reflect raw bytes for an attribute somebody had already written the table for,
   which is the defect this kind exists to end arriving through the fix for it. */
typedef struct {
    const char *idl;
    const char *attr;
    int         kind;
    long long   dflt;       /* §2.6.2 [ReflectDefault] — read only when has_dflt */
    bool        has_dflt;
    long long   rmin, rmax; /* §2.6.2 [ReflectRange] — read only when has_range */
    bool        has_range;
    const EnumeratedAttribute *en;   /* §2.3.3's attribute definition — REFLECT_ENUM rows only */
} ElReflect;

/* Install an interface's OWN reflections on its prototype. Each is assigned a magic out of one shared registry,
   so the two bodies that implement every reflection still take exactly one index. */
/* DECLARE a table of reflections once per AGENT; returns the BASE registry index the install names them by.
   `iface` NAMES THE INTERFACE THE TABLE BELONGS TO, and it is an argument rather than something this function
   could derive because the declaration's should-never-happens are checked HERE while the tables are written
   forty files-worth away — one call site walks every per-interface table in a loop, so a crash stamped with
   this function's own file and line names one line for the whole platform and its remedy ("fix the row") then
   has no object. The row's IDL name alone does not close that: `type` is a row on nine interfaces and `name` on
   more. So the interface travels with the operation and the abort says `HTMLLinkElement.type`. */
int  element_declare_reflections(JSContext *ctx, const char *iface, const ElReflect *r, int n);
/* INSTALL the `n` reflections declared at `base` onto THIS realm's prototype. */
void element_install_reflections(JSContext *ctx, JSValueConst proto, int base, int n);

/* AN ELEMENT'S CONTENT ATTRIBUTE, through the same chokepoint the reflections use — so a component that reads
   and writes one (§4.6.3's hyperlink members re-serialise a URL back into `href`) stays captured in the
   running flow's DOM delta. element_attr_get returns an OWNED string, or NULL when the attribute is absent. */
/* THE VALUE PAIR — what a component reads and writes when the attribute may hold a SOURCE. An attacker string
   stashed in an attribute keeps its provenance and its domain in §@S's (element, name) shadow
   (solver/attr_shadow.h), and these two are the only accessors that carry the whole triple: the getter answers
   with the concolic itself (JS_NULL when the attribute is absent), the setter hands the value to §4.9's write,
   which records the taint and stores the shape. The `char *` pair below is these two plus a ToString, and that
   ToString is where provenance dies — so it ASSERTS rather than performing it on a concolic. */
JSValue element_attr_get_value(JSContext *ctx, JSValueConst el, const char *name);
void    element_attr_set_value(JSContext *ctx, JSValueConst el, const char *name, JSValueConst value);
char *element_attr_get(JSContext *ctx, JSValueConst el, const char *name);
void  element_attr_set(JSContext *ctx, JSValueConst el, const char *name, const char *value);

/* HTML §2.6.1 "Reflecting content attributes in IDL attributes" — THE URL MODEL'S RESOLVING HALF, for the
 * members whose getter is §2.6.1's steps 2-3 under a DIFFERENT step 1.
 *
 * §2.6.2's `[ReflectURL]` getter is three steps: an absent attribute answers the empty string; a present one is
 * encoding-parsed-and-serialized against the element's node document; a parse failure answers the attribute as
 * a scalar value string. Only the FIRST of those is peculiar to the reflection — and it is exactly the step
 * §4.10.19.6 Form submission attributes replaces, in both of the algorithms it states: "If attribute is null or
 * attribute's value is the empty string, then return this's node document's URL", where §2.6.1 returns "". The
 * remaining two steps are word for word the same, which is why they are EXPORTED rather than copied: a second
 * parse-and-serialize is a second place for the base to be wrong (the node document's base URL, not the running
 * realm's, and its BASE rather than its address), and that exact mistake has been made in this tree twice.
 * `raw` is the attribute's value and is CONSUMED; it must NOT be JS_NULL — the null case is step 1's, and step
 * 1 is the caller's whole reason for being here. `member` NAMES THE DERIVATION for a concolic attribute value,
 * the same way the reflection registry passes the row's IDL name: a source stashed in `formaction` comes back
 * carrying its provenance, and the name is what tells two members' derivations apart. */
JSValue element_reflect_url_get(JSContext *ctx, lxb_dom_element_t *el, JSValue raw, const char *member);

/* HTML §2.6.1 "Reflecting content attributes in IDL attributes" — THE `double` MODEL, both directions, for the
 * members the enum above deliberately does not carry.
 *
 * WHY IT IS A PAIR OF FUNCTIONS AND NOT A `REFLECT_DOUBLE` ROW. §2.6.2's `[ReflectSetter]` says only the SETTER
 * reflects, so every `double` member of §4.10.13 and §4.10.14 has a getter that is its own algorithm and a
 * setter that is these steps — a row could not answer the getter, which is the half-implemented row the enum's
 * own comment refuses. The one member whose BOTH directions are §2.6.1's is §4.10.13's `max`
 * (`[ReflectPositive, ReflectDefault=1.0]`), and a row for it would compute "the maximum value of the progress
 * bar" a SECOND time: §4.10.13's `value` and `position` are both defined over that number, so the component
 * must have it anyway and two implementations of one number is the duplication a shared registry exists to
 * remove. So §2.6.1's double model lives here, with §2.6.1's other models, and its callers are components.
 *
 * `member` NAMES THE DERIVATION, not the attribute: an unknown parked in a content attribute keeps its
 * provenance through the parse exactly as REFLECT_ULONG's does, and the operation name is what tells
 * `meter.low` and `meter.high` apart when both derive from one source.
 *
 * THE GETTER'S THREE FALLBACKS ARE §2.6.1'S OWN and are not interchangeable: a value that PARSES and is
 * positive is returned; a value that parses and is not positive is returned only when the member is not
 * `positive_only`; anything else is `dflt` when the member declares one and 0 when it does not. */
JSValue element_reflect_double_get(JSContext *ctx, JSValueConst el, const char *attr, const char *member,
                                   bool positive_only, bool has_dflt, double dflt);
/* §2.6.1's double SETTER steps: "If the reflected IDL attribute is limited to only positive numbers and the
   given value is not greater than 0, then return. Run this's set the content attribute with the given value,
   converted to the best representation of the number as a floating-point number."
   `val` has already crossed Web IDL §3.2.7's `double`, so a NaN or an infinity threw before this is reached —
   EXCEPT for unknown external input, which §3.2.7 crosses as itself (idl_concolic_rule). */
void element_reflect_double_set(JSContext *ctx, JSValueConst el, const char *attr, const char *member,
                                JSValueConst val, bool positive_only);

/* HTML §4.12.1.1 "Processing model"'s CHILDREN CHANGED STEPS, RECORDED FOR THE ONE DRAIN THAT CAN RUN THEM —
   see element.c. Their step 2 runs the post-connection steps, which end at step 36's "immediately execute the
   script element el", and the hook that carries them is called from inside the DOM mutation chokepoint, where
   there is no flow base to run the page's code on. So the node joins the record insert step 12 is drained
   from, and BOTH doors into §4.12.1.1's post-connection steps reach the page's code at one place.
   THE CONNECTEDNESS TEST IS THE CALLER'S — it is the children changed steps' own step 1 and is asked at the
   moment the standard asks it; insert step 12's is a separate and later read the drain performs per entry. */
void element_post_connection_record(lxb_dom_node_t *n);

/* The element behind a wrapper, or NULL when the value is not one. */
lxb_dom_element_t *element_of_value(JSValueConst v);
/* WEB IDL §3.2.15's "V implements Element" — the narrowing an `Element` argument brands with, since every node
   wrapper is ONE class and a class id therefore cannot say which interface the node implements. */
bool element_is(JSValueConst v);
/* THE ELEMENT AS §3.8 NAMES IT — its namespace URL and its local name, which together decide which interface it
   implements and therefore which row of the Trusted Types table it can match. Borrowed from Lexbor's interned
   strings into the caller's buffers; `*ns` is NULL for an element in no namespace. */
void element_ns_and_local(lxb_dom_element_t *el, const char **ns, const char **local,
                          char *nsbuf, size_t nscap, char *lobuf, size_t locap);
/* DOM §4.9 `prefix` — the reader above's nullable third name, BORROWED from the document's prefix hash and NUL
   terminated by nothing. NULL with `*len` 0 is §1.4's null prefix; `*len` is written on every path, which is
   the whole reason this exists rather than lxb_dom_element_prefix (see element.c). */
const char *element_prefix(lxb_dom_element_t *el, size_t *len);
/* DOM §4.9 "create an element internal"'s storage step — "Set element's namespace to namespace, namespace
   prefix to prefix, local name to localName". The WRITER whose reader is the function above, which is why the
   two are declared together: what the standard calls "as given" is a property of how those three strings are
   interned, and lexbor's own element creation interns all three CASE-FOLDED. See element.c for the
   measurement. `ns` and `prefix` are NULL when there is none (§1.4's null, not the empty string); all three
   slices are borrowed and none is NUL-terminated. Never returns NULL — an allocation failure is fatal. */
lxb_dom_element_t *element_create_ns(lxb_dom_document_t *doc, const char *ns, size_t ns_len,
                                     const char *local, size_t local_len,
                                     const char *prefix, size_t prefix_len);

#endif
