/* THE MIME TYPE RECORD — WHATWG MIME Sniffing §4. See mime_type.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_MIME_MIME_TYPE_H
#define ENGINE_HOST_BROWSER_CORE_MIME_MIME_TYPE_H
#include <stdbool.h>
#include <stddef.h>

/* §4.1's MIME TYPE RECORD: a type, a subtype, and an ORDERED MAP of parameters. All three are the record —
   a component that keeps only the essence has thrown away `charset`, which is the parameter every consumer in
   this engine actually reads, and one that keeps only the string has to re-derive the split at every read. */
typedef struct { char *name, *value; } MimeParam;
typedef struct {
    char      *type;      /* §4.1: a non-empty ASCII string, lowercased by the parser */
    char      *subtype;   /* §4.1: a non-empty ASCII string, lowercased by the parser */
    MimeParam *params;    /* §4.1's ordered map — an ARRAY, because the ORDER is what §4.5 serializes */
    int        nparams, cparams;
} MimeType;

void mime_type_init(MimeType *m);
void mime_type_free(MimeType *m);

/* §4.4 "parse a MIME type". False is the spec's FAILURE, and every caller has its own answer for one: Fetch
   §6 substitutes text/plain;charset=US-ASCII, XHR §3.6.6 substitutes text/xml, and XHR §3.6.7 substitutes
   application/octet-stream. `out` is left initialised-and-empty on failure, so the caller frees it either way. */
bool mime_type_parse(MimeType *out, const char *input, size_t len);

/* §4.5 "serialize a MIME type" — the essence, then every parameter in order, quoted where §4.5 quotes.
   Malloc'd; caller frees. This is the only defined STRING form of a record: a field typed as a string that
   holds a MIME type (a `Content-Type` header value, a Blob's type) holds this. */
char *mime_type_serialize(const MimeType *m);

/* §4.2's ESSENCE: type, U+002F (/), subtype. Malloc'd; caller frees. */
char *mime_type_essence(const MimeType *m);

/* §4.1's parameters[name], or NULL for "does not exist". `name` is matched as the parser stored it, which is
   ASCII-lowercase. The returned string is the record's own — it dies with the record. */
const char *mime_type_parameter(const MimeType *m, const char *name);

/* §4.6's MIME TYPE GROUPS. Only the two groups this engine's algorithms branch on are here: XHR §3.6.6's "set
   a document response" tests exactly these. The other groups (image, audio-or-video, font, ZIP-based, archive,
   JSON, JavaScript) are ABSENT rather than written out unused — each belongs with the algorithm that first
   needs to branch on it, and idlgen cannot audit a group nobody calls. */
bool mime_type_is_html(const MimeType *m);
bool mime_type_is_xml(const MimeType *m);

/* Fetch §2.2.3 "extract a MIME type", over the `Content-Type` value as "get a header" has already joined the
   list's duplicates (0x2C 0x20 between them) — which is exactly the string Fetch §2.2's "get, decode, and
   split" takes, and why this takes a VALUE rather than a header list: its two callers hold their lists
   differently (a HeaderList in Fetch, a JS array in XHR) and both can produce the joined value.
   NULL — the header is absent — is the spec's "values is null", which is failure. So is a value in which every
   candidate either fails to parse or has the wildcard essence the algorithm skips. */
bool mime_type_extract(MimeType *out, const char *content_type_value);

#endif
