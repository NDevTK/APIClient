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

/* §4.6's MIME TYPE GROUPS. Only the groups this engine's algorithms branch on are here — each arrives with the
   algorithm that first needs it, and a group nobody calls is a table no gate can audit. XHR §3.6.6's "set a
   document response" tests HTML and XML; §7's MIME type sniffing tests XML, HTML, image and audio-or-video
   (browser_process/network/mime_sniff.c — a different PROGRAM, which is why this record is a pure value type
   over strings with no dependency outside the C library); solver/reply_decode.c
   tests image, audio-or-video, font, ZIP-based and archive over the SUPPLIED type to decide whether a reply
   body has any structure to learn from. The JSON group is still ABSENT — nothing branches on it yet.
   THE JAVASCRIPT GROUP ARRIVED WITH ITS SECOND CALLER, which is this comment's own rule for when a group is
   written: browser_process/network/corb.c had a private copy (a `const char *` essence against a hand-written
   list), and resource_kind.c needs the same question answered to tell a SCRIPT from the API data some servers
   ship under a JavaScript type. Two copies of one group is the pair that can disagree about one response, and
   corb.c's had in fact already drifted from §4.6 in both directions — it carried `application/node`, which is
   not in the group, and lacked `text/livescript`, which is. */
bool mime_type_is_html(const MimeType *m);
bool mime_type_is_xml(const MimeType *m);
bool mime_type_is_image(const MimeType *m);
bool mime_type_is_audio_or_video(const MimeType *m);
bool mime_type_is_font(const MimeType *m);
bool mime_type_is_zip_based(const MimeType *m);
bool mime_type_is_archive(const MimeType *m);
bool mime_type_is_javascript(const MimeType *m);

/* Fetch §2.2.3 "extract a MIME type", over the `Content-Type` value as "get a header" has already joined the
   list's duplicates (0x2C 0x20 between them) — which is exactly the string Fetch §2.2's "get, decode, and
   split" takes, and why this takes a VALUE rather than a header list: its two callers hold their lists
   differently (a HeaderList in Fetch, a JS array in XHR) and both can produce the joined value.
   NULL — the header is absent — is the spec's "values is null", which is failure. So is a value in which every
   candidate either fails to parse or has the wildcard essence the algorithm skips. */
bool mime_type_extract(MimeType *out, const char *content_type_value);

/* Fetch §3.5 "legacy extract an encoding", the algorithm that turns a `Content-Type`'s charset PARAMETER into
   an encoding: "To legacy extract an encoding given failure or a MIME type mimeType and an encoding
   fallbackEncoding: If mimeType is failure, then return fallbackEncoding. If mimeType["charset"] does not
   exist, then return fallbackEncoding. Let tentativeEncoding be the result of getting an encoding from
   mimeType["charset"]. If tentativeEncoding is failure, then return fallbackEncoding. Return
   tentativeEncoding."
   IT IS DECLARED BESIDE `extract a MIME type` BECAUSE IT IS THE SAME HEADER'S OTHER HALF — Fetch states both in
   §3.5, and its own caller (HTML §8.1.4.2's fetch a classic script) runs them back to back on one header list.
   It is DEFINED in mime_type_encoding.c rather than in mime_type.c, and that file states why: it is the only
   line of core/mime that reaches outside the C library, and holding it in the record's own translation unit put
   the entire JS realm (core/encoding is the TextEncoder/TextDecoder component) behind a pure value type — which
   is what stopped a realm-free program from linking the record at all. `m` is
   NULL for the spec's FAILURE, which is the one value a parsed record can never be, so the two arms of "failure
   or a MIME type" are one parameter. Both the fallback and the answer are ids in the Encoding registry
   (core/encoding), so a caller never holds a label it has not resolved.
   THE ANSWER CAN BE `replacement`, and that is not an error to filter out: §4.2's get an encoding maps
   `iso-2022-kr` and its siblings to it deliberately, and a decode that runs the replacement decoder is what
   stops a server-chosen label from smuggling a different script past the client. */
int mime_type_legacy_extract_encoding(const MimeType *m, int fallback_encoding);

#endif
