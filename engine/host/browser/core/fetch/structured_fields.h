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
 * moment a fourth header arrives — a caller asking for a token must be able to tell "this is a valid field
 * carrying something else" from "this is not a structured field", because only the second means the sender is
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
 * ONLY "item" IS SPELLABLE. Fetch's get-a-structured-field-value takes a type of "dictionary", "list" or
 * "item", and the three headers this engine reads are all items. There is no entry here that takes a type, so
 * a caller needing a list finds nothing to call rather than a parameter that silently accepts one. A shape
 * that cannot express the case is not a check someone has to remember — and this is a claim about a PARSER
 * with two callers, which is the whole reason it holds: it is not a general rule about network verbs, and the
 * one this line used to cite (SECURITY.md's "there is no place to express a POST") is deleted, because a
 * Google API error-probe learns by sending a malformed POST and reading the rejection. */
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
   Returns FALSE for BOTH "the list contains no such header" and "the value did not parse", which is Fetch's
   own note and not a simplification — the two are deliberately indistinguishable so that every consumer on
   the platform handles them identically. */
bool sf_header_item(const HeaderList *l, const char *name, SfItem *out);

#endif
