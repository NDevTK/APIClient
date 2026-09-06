/* STRUCTURED FIELD VALUES — RFC 9651, and Fetch §2.2.2's "get a structured field value" over a header list.
 *
 * WHY THIS EXISTS AT ALL: THREE HEADERS THAT DECIDE WHAT A DOCUMENT *IS* ARE STRUCTURED FIELDS. HTML §7.1.2's
 * `Origin-Agent-Cluster` "is a structured header whose value must be a boolean"; §7.1.3.1's
 * `Cross-Origin-Opener-Policy` and §7.1.4.1's `Cross-Origin-Embedder-Policy` "are structured headers whose
 * values must be token", with an optional `report-to` parameter. Not one of them can be read with a string
 * compare: `?1` is a boolean and `?1;foo=bar` is the SAME boolean, `require-corp` and `require-corp ` are the
 * same token, and — the case that makes this a parser rather than a `strcmp` — `require-corp, require-corp`
 * is NOT a token at all. §7.1.4.1 prints that as a table for a reason: two identical `Cross-Origin-Embedder-
 * Policy` headers combine (Fetch's "get" joins with 0x2C 0x20) into a LIST, an ITEM parse of a list FAILS, and
 * the processing model then FAILS OPEN to `unsafe-none`. A substring test would have answered `require-corp`
 * and made a document cross-origin isolated that no browser isolates.
 *
 * PARSING FAILURE IS A VALUE, NEVER A CRASH. Fetch says so in its own note: "get a structured field value
 * intentionally does not distinguish between a header not being present and its value failing to parse as a
 * structured field value. This ensures uniform processing across the web platform." So `sf_header_item`
 * answers FALSE for both, its callers treat both as "the header was not delivered", and a page serving
 * `Origin-Agent-Cluster: %%%` gets the same Document a page serving no header gets. An engine that DCHECKed on
 * malformed input here would abort on ordinary hostile web content while implementing the spec's own
 * fail-open behavior — which is the one shape of assert CLAUDE.md calls wrong, an "unexpected state" that the
 * standard expects.
 *
 * THE WHOLE ITEM GRAMMAR IS PARSED EVEN THOUGH ONLY TWO KINDS ARE READ. A parser that failed on an integer
 * would report "malformed" for a field that is perfectly well formed, and the two are different facts the
 * moment a fourth header arrives — a caller asking for a token must be able to tell `this is a valid field
 * carrying something else` from `this is not a structured field`, because only the second means the sender is
 * broken. RFC 9651 §4.2.3.1's eight bare-item kinds are therefore all here, including the two that RFC 8941
 * did not have (Date and Display String), because Fetch normatively references RFC 9651.
 *
 * ONE KIND IS VALIDATED WITHOUT BEING DECODED, AND THE LINE IS WHERE THE VALUE STOPS BEING TEXT. A Display
 * String's escapes ARE its parse — §4.2.10 resolves `%xx`, requires the escapes to be LOWERCASE hex, and then
 * fails if the octets are not valid UTF-8 — so it is decoded here and the UTF-8 walk that decides it is here
 * too. A Byte Sequence is the other side: §4.2.7's decode produces OCTETS, not text, and no consumer of a
 * structured field in this engine has ever asked for octets — so the base64 is validated to exactly the depth
 * that decides whether it WOULD decode (RFC 4648's alphabet, padding as a trailing run of at most two, and a
 * length that is not 4n+1) and retained AS DELIVERED. That keeps the parse contract above exact — valid input
 * parses, invalid input fails — without this file growing a second copy of a codec for nobody.
 *
 * "item" AND "dictionary" ARE SPELLABLE AND "list" IS NOT, AND THE RULE THAT DECIDES THAT IS UNCHANGED. Fetch's
 * get-a-structured-field-value takes a type of "dictionary", "list" or "item", and there is still no entry
 * here that TAKES a type: a caller states which grammar it wants by which function it calls, so a caller
 * needing a list finds nothing to call rather than a parameter that silently accepts one. What changed is the
 * population, and it changed for a header whose type is the whole trap — Permissions Policy §6.1
 * "`Permissions-Policy` HTTP Header Field": "Its value must be a dictionary". §5.2 "Structured header
 * serialization" then makes each Member Name a feature and each Member Value an allowlist, and §9.1 "Process
 * response policy" gets it with type "dictionary" by name. An ITEM parse of `geolocation=(), camera=(self)`
 * does not fail SAFELY, it fails at the comma, which Fetch's own note then makes indistinguishable from the
 * header not being sent — and §9.5's «[], []» declared policy is the MOST PERMISSIVE one, so every restriction
 * that server wrote would be read as silence. That is why the grammar is a parse and not a `strchr` loop.
 *
 * AND THE DICTIONARY BRINGS THE INNER LIST WITH IT, WHICH THE ITEM GRAMMAR NEVER NEEDED. §4.2.2's member value
 * is §4.2.1.1's "Parsing an Item or Inner List", and §5.2 uses BOTH arms in one header: `camera=self` is the
 * item arm and `camera=(self "https://a.example")` is the inner-list arm. Parsing only the item arm would fail
 * the whole dictionary member for the spelling every real policy uses. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_STRUCTURED_FIELDS_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_STRUCTURED_FIELDS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/fetch/headers.h"

/* RFC 9651 §3.3's BARE ITEM KINDS, all eight. */
typedef enum {
    SF_INTEGER = 0,
    SF_DECIMAL,
    SF_STRING,
    SF_TOKEN,
    SF_BYTE_SEQUENCE,
    SF_BOOLEAN,
    SF_DATE,
    SF_DISPLAY_STRING,
} SfKind;

/* ONE BARE ITEM. `text` is owned and NUL-terminated for the four kinds that have one, and NULL for the four
   that do not — a kind is what says which field to read, and reading the wrong one is what `kind` exists to
   stop. SF_STRING's text is UNESCAPED (§4.2.5 resolves the two escapes), SF_DISPLAY_STRING's is the DECODED
   UTF-8 (§4.2.10 resolves `%xx`), SF_TOKEN's is the token, and SF_BYTE_SEQUENCE's is the base64 AS DELIVERED —
   for the reason the file header gives. */
typedef struct {
    SfKind   kind;
    int64_t  integer;   /* SF_INTEGER, and SF_DATE's seconds since the Unix epoch (§4.2.8) */
    double   decimal;   /* SF_DECIMAL */
    bool     boolean;   /* SF_BOOLEAN */
    char    *text;
} SfBareItem;

/* §4.2.3.2's PARAMETERS — an ordered map, and ORDER MATTERS to nothing here, which is why it is a plain array:
   a duplicate key REPLACES (a map set), so the lookup below answers with the last one written. */
typedef struct {
    char       *key;
    SfBareItem  value;
} SfParam;

/* §4.2.3's ITEM: a bare item plus its parameters. */
typedef struct {
    SfBareItem  item;
    SfParam    *params;
    int         n_params;
    int         cap_params;
} SfItem;

/* RFC 9651 §4.2 "parse structured fields" with header_type = "item": discard leading SP, parse an item,
   discard leading SP, and FAIL if anything is left. `out` is filled only on success and is the caller's to
   sf_item_free; on failure nothing is left allocated. */
bool sf_parse_item(const char *input, size_t len, SfItem *out);

/* §4.2.3.2's map lookup, for a caller that wants ONE parameter by name (`report-to` is the only one HTML
   defines for the two policy headers). BORROWED, and NULL when the item carries no such key — which is
   exactly the spec's "if parsedItem[1][...] exists" test, so a caller writes that condition as this call. */
const SfBareItem *sf_item_param(const SfItem *it, const char *key);

void sf_item_free(SfItem *it);

/* FETCH §2.2.2's "get a structured field value given a header name `name` and a string type from a header
   list", with type fixed to "item" (see the file header). `name` must already be LOWERCASE, which is how a
   header list stores names and how every caller spells the constant.
   Returns FALSE for BOTH `the list contains no such header` and `the value did not parse`, which is Fetch's
   own note and not a simplification — the two are deliberately indistinguishable so that every consumer on
   the platform handles them identically. */
bool sf_header_item(const HeaderList *l, const char *name, SfItem *out);

/* ---- §3.2's DICTIONARY, and the §4.2.1.1 tuple its values are ------------------------------------------- */

/* §4.2.1.1's "(item_or_inner_list, parameters)" TUPLE — "item_or_inner_list can be either a single bare item
 * or an array of (bare_item, parameters) tuples".
 *
 * THE TUPLE'S PARAMETERS ARE ONE FIELD ACROSS BOTH ARMS AND THAT IS THE POINT. §4.2.3 gives the item arm its
 * parameters and §4.2.1.2 step 3.2.2 gives the inner-list arm its own, and Permissions Policy §5.2 defines
 * `report-to` as a parameter of the MEMBER VALUE — which is either arm — so a reader that had to ask which arm
 * it was holding before it could look up a parameter would be a reader with two chances to get one lookup
 * wrong. `tuple.params` is therefore §4.2.1.1's `parameters` in both arms, and sf_member_param reads it
 * without branching.
 * `tuple.item` IS THE BARE ITEM OF THE ITEM ARM AND MEANS NOTHING IN THE OTHER, which is why it is read
 * through sf_member_bare rather than off the struct: a `kind` of SF_INTEGER on an inner-list member is the
 * zero this struct was calloc'd with and not a number the sender wrote, and reading it as one is the defaulted
 * field defect with a grammar behind it. */
typedef struct {
    bool    inner_list;
    SfItem  tuple;
    SfItem *items;        /* the inner-list arm's members, each with its OWN §4.2.3.2 parameters (owned) */
    int     n_items;
    int     cap_items;
} SfMember;

/* §3.2's DICTIONARY: "an ordered map of key-value pairs". ORDER IS KEPT — §9.2's "for each feature-name →
   (value, params) of dictionary" iterates it, and a later member of a real header can only ever be the LAST
   declaration of that feature because §4.2.2 step 2.4 overwrites in place rather than appending. */
typedef struct {
    char     *key;
    SfMember  value;
} SfDictMember;

typedef struct {
    SfDictMember *members;
    int           n_members;
    int           cap_members;
} SfDictionary;

/* RFC 9651 §4.2 "Parsing Structured Fields" with field_type = "dictionary": discard leading SP, run §4.2.2
   "Parsing a Dictionary", discard leading SP, and FAIL if anything is left. `out` is filled only on success
   and is the caller's to sf_dictionary_free; on failure nothing is left allocated.
   AN EMPTY INPUT IS A SUCCESSFUL PARSE OF AN EMPTY DICTIONARY, which is §4.2.2 step 3 ("No structured data has
   been found; return dictionary (which is empty)") and not a failure to report. It matters here: a
   `Permissions-Policy:` with an empty value declares nothing, and §9.1 returning an empty ordered map for it
   is a DIFFERENT statement from §9.1 returning one because the header was absent only in that the first is
   what the sender wrote. Both leave §9.6's merge with nothing to do, which is why neither needs a caller
   branch — but a parser that FAILED on it would be reporting a malformed field for a well-formed one. */
bool sf_parse_dictionary(const char *input, size_t len, SfDictionary *out);

/* FETCH §2.2.2's "get a structured field value given a header name `name` and a string type from a header
   list", with type "dictionary". `name` must already be LOWERCASE. Returns FALSE for BOTH `the list contains
   no such header` and `the value did not parse`, for sf_header_item's reason and Fetch's own note. */
bool sf_header_dictionary(const HeaderList *l, const char *name, SfDictionary *out);

/* §4.2.1.1's BARE ITEM, for the item arm. ASSERTS the arm — see SfMember. */
const SfBareItem *sf_member_bare(const SfMember *m);

/* §4.2.3.2's map lookup over §4.2.1.1's `parameters`, in either arm. BORROWED, NULL when absent. */
const SfBareItem *sf_member_param(const SfMember *m, const char *key);

void sf_dictionary_free(SfDictionary *d);

#endif
