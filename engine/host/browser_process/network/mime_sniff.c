/* MIME sniffing — see mime_sniff.h. WHATWG MIME Sniffing §6 (pattern matching) and §7 (the sniffing
   algorithm), over §4's record. No state: every entry is a pure function of the bytes and the header value. */
#include "network/mime_sniff.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>

/* §3's BYTE CLASSES, each named as the spec names it so a reader can check the range against the sentence.
   0xWS is the set §6's `ignored` column spells "Whitespace bytes"; 0xTT is the byte the tables write into a
   pattern; a BINARY DATA BYTE is what §7.1's last-but-one step and §7.2's third step ask about. */
static bool ws_byte(unsigned char c)
{
    return c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
}
static bool tt_byte(unsigned char c)
{
    return c == 0x20 || c == 0x3E;
}
static bool binary_data_byte(unsigned char c)
{
    return c <= 0x08 || c == 0x0B || (c >= 0x0E && c <= 0x1A) || (c >= 0x1C && c <= 0x1F);
}

/* ── §6 pattern matching ──────────────────────────────────────────────────────────────────────────────────
 *
 * A ROW IS THE SPEC'S FIVE COLUMNS, and `tt` is the fifth of them made expressible: the tables write a literal
 * "TT" into the byte pattern with mask FF, which is not a byte and cannot be one — it stands for "either of the
 * two tag-terminating bytes". Encoding it as an INDEX rather than as two rows per pattern keeps one table row
 * per spec table row, so a reader diffing this against the standard is comparing the same number of lines. */
typedef struct {
    const unsigned char *pat;
    const unsigned char *mask;
    unsigned char        len;
    signed char          tt;       /* index of the 0xTT byte in `pat`, or -1 for a row that has none */
    bool                 skip_ws;  /* §6's `ignored` — "Whitespace bytes" in the scriptable table, else empty */
    const char          *type;     /* the row's §4.2 essence; a static literal, so it outlives every caller */
} SniffRow;

/* §6's PATTERN MATCHING ALGORITHM. The spec's step 2 compares lengths BEFORE step 4 skips the ignored bytes,
   which leaves its own step 8 reading past the end of a short input that begins with whitespace; the bound is
   taken after the skip here, which is the only reading under which the algorithm cannot read a byte it was not
   given. Every row whose `ignored` is empty is unaffected, which is all of them outside §7.1's first table. */
static bool pattern_match(const unsigned char *in, size_t n, const SniffRow *r)
{
    size_t s = 0;
    int p;

    DCHECK(r->len > 0, "a sniff row carries an empty byte pattern — §6 asserts pattern and mask are the same "
                       "length and a zero-length one matches every resource, which would make the table's "
                       "order decide the answer for bytes nobody looked at");
    if (r->skip_ws)
        while (s < n && ws_byte(in[s])) s++;
    if (n - s < (size_t)r->len)
        return false;
    for (p = 0; p < (int)r->len; p++, s++) {
        if (p == r->tt) {
            if (!tt_byte(in[s])) return false;
            continue;
        }
        if ((unsigned char)(in[s] & r->mask[p]) != r->pat[p]) return false;
    }
    return true;
}

static const char *table_match(const unsigned char *in, size_t n, const SniffRow *rows, int nrows)
{
    int i;
    for (i = 0; i < nrows; i++)
        if (pattern_match(in, n, &rows[i])) return rows[i].type;
    return NULL;   /* §6.1/§6.2/§6.4's "Return undefined" */
}

/* ── §6.1 image type pattern matching ─────────────────────────────────────────────────────────────────── */

static const unsigned char P_ICO1[]  = { 0x00,0x00,0x01,0x00 };
static const unsigned char P_ICO2[]  = { 0x00,0x00,0x02,0x00 };
static const unsigned char M_4FF[]   = { 0xFF,0xFF,0xFF,0xFF };
static const unsigned char P_BMP[]   = { 0x42,0x4D };
static const unsigned char M_2FF[]   = { 0xFF,0xFF };
static const unsigned char P_GIF87[] = { 0x47,0x49,0x46,0x38,0x37,0x61 };
static const unsigned char P_GIF89[] = { 0x47,0x49,0x46,0x38,0x39,0x61 };
static const unsigned char M_6FF[]   = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
static const unsigned char P_WEBP[]  = { 0x52,0x49,0x46,0x46,0,0,0,0,0x57,0x45,0x42,0x50,0x56,0x50 };
static const unsigned char M_WEBP[]  = { 0xFF,0xFF,0xFF,0xFF,0,0,0,0,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
static const unsigned char P_PNG[]   = { 0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A };
static const unsigned char M_8FF[]   = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
static const unsigned char P_JPEG[]  = { 0xFF,0xD8,0xFF };
static const unsigned char M_3FF[]   = { 0xFF,0xFF,0xFF };

static const SniffRow IMAGE_ROWS[] = {
    { P_ICO1,  M_4FF,  4, -1, false, "image/x-icon" },
    { P_ICO2,  M_4FF,  4, -1, false, "image/x-icon" },
    { P_BMP,   M_2FF,  2, -1, false, "image/bmp" },
    { P_GIF87, M_6FF,  6, -1, false, "image/gif" },
    { P_GIF89, M_6FF,  6, -1, false, "image/gif" },
    { P_WEBP,  M_WEBP,14, -1, false, "image/webp" },
    { P_PNG,   M_8FF,  8, -1, false, "image/png" },
    { P_JPEG,  M_3FF,  3, -1, false, "image/jpeg" },
};

const char *mime_sniff_image_pattern(const unsigned char *header, size_t header_n)
{
    return table_match(header, header_n, IMAGE_ROWS, (int)(sizeof IMAGE_ROWS / sizeof *IMAGE_ROWS));
}

/* ── §6.2 audio or video type pattern matching ────────────────────────────────────────────────────────── */

static const unsigned char P_AIFF[] = { 0x46,0x4F,0x52,0x4D,0,0,0,0,0x41,0x49,0x46,0x46 };
static const unsigned char M_RIFF[] = { 0xFF,0xFF,0xFF,0xFF,0,0,0,0,0xFF,0xFF,0xFF,0xFF };
static const unsigned char P_ID3[]  = { 0x49,0x44,0x33 };
static const unsigned char P_OGG[]  = { 0x4F,0x67,0x67,0x53,0x00 };
static const unsigned char M_5FF[]  = { 0xFF,0xFF,0xFF,0xFF,0xFF };
static const unsigned char P_MIDI[] = { 0x4D,0x54,0x68,0x64,0x00,0x00,0x00,0x06 };
static const unsigned char P_AVI[]  = { 0x52,0x49,0x46,0x46,0,0,0,0,0x41,0x56,0x49,0x20 };
static const unsigned char P_WAVE[] = { 0x52,0x49,0x46,0x46,0,0,0,0,0x57,0x41,0x56,0x45 };

static const SniffRow AV_ROWS[] = {
    { P_AIFF, M_RIFF, 12, -1, false, "audio/aiff" },
    { P_ID3,  M_3FF,   3, -1, false, "audio/mpeg" },
    { P_OGG,  M_5FF,   5, -1, false, "application/ogg" },
    { P_MIDI, M_8FF,   8, -1, false, "audio/midi" },
    { P_AVI,  M_RIFF, 12, -1, false, "video/avi" },
    { P_WAVE, M_RIFF, 12, -1, false, "audio/wave" },
};

/* §6.2.1 SIGNATURE FOR MP4 — an algorithm rather than a pattern, because the brand it looks for may appear in
   any of the compatible-brand slots the file's own box-size names. */
static bool mp4_signature(const unsigned char *d, size_t n)
{
    unsigned long box_size, read;

    if (n < 12) return false;
    box_size = ((unsigned long)d[0] << 24) | ((unsigned long)d[1] << 16) |
               ((unsigned long)d[2] << 8)  | (unsigned long)d[3];
    if ((unsigned long)n < box_size || box_size % 4 != 0) return false;
    if (memcmp(d + 4, "ftyp", 4) != 0) return false;
    if (memcmp(d + 8, "mp4", 3) == 0) return true;
    /* 16, not 12: the four bytes after the major brand are its VERSION NUMBER, which the spec skips by name. */
    for (read = 16; read < box_size; read += 4) {
        if (read + 3 > (unsigned long)n) break;
        if (memcmp(d + read, "mp4", 3) == 0) return true;
    }
    return false;
}

/* §6.2.2's PARSE A VINT — EBML's variable-width integer, whose leading zero bits give its own width. Only the
   WIDTH is used by the signature (the value is skipped over), but both are returned because the algorithm is
   defined to return both and a reader checking this against the spec should find both. */
static unsigned long webm_vint(const unsigned char *d, size_t n, size_t at, unsigned *number_size)
{
    unsigned mask = 128, size = 1;
    unsigned long parsed;
    size_t index = at;
    unsigned remaining;

    while (size < 8 && index < n) {
        if (d[index] & mask) break;
        mask >>= 1;
        size++;
    }
    parsed = (index < n) ? (unsigned long)(d[index] & (unsigned char)~mask) : 0;
    index++;
    for (remaining = size - 1; remaining != 0; remaining--) {
        if (index >= n) break;
        parsed = (parsed << 8) | d[index];
        index++;
    }
    *number_size = size;
    return parsed;
}

/* §6.2.2's MATCHING A PADDED SEQUENCE: the pattern, in order, at the end of the range, preceded only by NULs. */
static bool webm_padded(const unsigned char *d, size_t n, size_t offset, size_t end, const char *pat)
{
    size_t plen = strlen(pat), i = offset;

    if (n <= end) return false;
    while (i < end && (end - i) > plen) {
        if (d[i] != 0x00) return false;
        i++;
    }
    if ((end - i) != plen) return false;
    return memcmp(d + i, pat, plen) == 0;
}

/* §6.2.2 SIGNATURE FOR WebM: the EBML magic, then the DocType element (0x42 0x82) within the first 38 bytes,
   whose value must be "webm". */
static bool webm_signature(const unsigned char *d, size_t n)
{
    size_t iter = 4;

    if (n < 4) return false;
    if (!(d[0] == 0x1A && d[1] == 0x45 && d[2] == 0xDF && d[3] == 0xA3)) return false;
    while (iter < n && iter < 38) {
        if (iter + 1 < n && d[iter] == 0x42 && d[iter + 1] == 0x82) {
            unsigned number_size = 1;
            iter += 2;
            if (iter >= n) break;
            webm_vint(d, n, iter, &number_size);
            iter += number_size;
            if (n < 4 || iter >= n - 4) break;
            if (webm_padded(d, n, iter, iter + 4, "webm")) return true;
        }
        iter++;
    }
    return false;
}

/* §6.2.3's SIGNATURE FOR MP3 WITHOUT ID3 IS NOT IMPLEMENTED, AND THAT IS A DECISION ABOUT THE SPEC RATHER THAN
   A GAP IN THIS FILE. Its own text contradicts itself in three places: `match an mp3 header` rejects only when
   BOTH the sync byte and the sync bits fail ("is not equal to 0xff AND ... is not equal to 0xe0"), which
   accepts almost any input; `parse an mp3 frame` reads MPEG-1 (version bits 3, so version & 1 is non-zero) out
   of the mp2.5-rates table and MPEG-2 out of mp3-rates, which is the two tables exchanged; and `compute an mp3
   frame size` takes scale 72 only for version 1, the RESERVED version, so MPEG-2 never reaches it. Written
   as published it would answer "audio/mpeg" for arbitrary bodies, and every such answer SUPPRESSES learning
   from a real reply — a product defect traded for a table row. Implementing what browsers do instead would be
   re-deriving an algorithm rather than following one, which is the thing CLAUDE.md §Browser-half forbids. An
   untagged MP3 therefore falls to §7.1's last two steps and computes as application/octet-stream, which is a
   defined outcome of this standard and not an error state. */

const char *mime_sniff_audio_video_pattern(const unsigned char *header, size_t header_n)
{
    const char *m = table_match(header, header_n, AV_ROWS, (int)(sizeof AV_ROWS / sizeof *AV_ROWS));
    if (m) return m;
    if (mp4_signature(header, header_n)) return "video/mp4";
    if (webm_signature(header, header_n)) return "video/webm";
    return NULL;
}

/* ── §6.4 archive type pattern matching ───────────────────────────────────────────────────────────────── */

static const unsigned char P_GZIP[] = { 0x1F,0x8B,0x08 };
static const unsigned char P_ZIP[]  = { 0x50,0x4B,0x03,0x04 };
static const unsigned char P_RAR[]  = { 0x52,0x61,0x72,0x21,0x1A,0x07,0x00 };
static const unsigned char M_7FF[]  = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

static const SniffRow ARCHIVE_ROWS[] = {
    { P_GZIP, M_3FF, 3, -1, false, "application/x-gzip" },
    { P_ZIP,  M_4FF, 4, -1, false, "application/zip" },
    { P_RAR,  M_7FF, 7, -1, false, "application/x-rar-compressed" },
};

const char *mime_sniff_archive_pattern(const unsigned char *header, size_t header_n)
{
    return table_match(header, header_n, ARCHIVE_ROWS, (int)(sizeof ARCHIVE_ROWS / sizeof *ARCHIVE_ROWS));
}

/* §6.3's FONT TABLE IS ABSENT, for the reason mime_type.h states about the groups: §7 and §7.1 never run it —
   only §8's font context does, and this engine has no font context. The §4.6 font GROUP is a question about a
   declared type and is answered by mime_type_is_font; the table is a question about BYTES and has no asker. */

/* ── §7.1 identifying a resource with an unknown MIME type ────────────────────────────────────────────── */

/* The scriptable table. Each row is the tag name with its trailing 0xTT, and the mask is 0xDF exactly where the
   spec makes the letter ASCII case-insensitive — 0xFF on `<`, on `!`, on the digit of `<H1` and on the `<!--`
   run, because none of those has a case. */
static const unsigned char P_DOCTYPE[] = { 0x3C,0x21,0x44,0x4F,0x43,0x54,0x59,0x50,0x45,0x20,0x48,0x54,0x4D,0x4C,0x00 };
static const unsigned char M_DOCTYPE[] = { 0xFF,0xFF,0xDF,0xDF,0xDF,0xDF,0xDF,0xDF,0xDF,0xFF,0xDF,0xDF,0xDF,0xDF,0xFF };
static const unsigned char P_HTML[]    = { 0x3C,0x48,0x54,0x4D,0x4C,0x00 };
static const unsigned char M_TAG5[]    = { 0xFF,0xDF,0xDF,0xDF,0xDF,0xFF };
static const unsigned char P_HEAD[]    = { 0x3C,0x48,0x45,0x41,0x44,0x00 };
static const unsigned char P_SCRIPT[]  = { 0x3C,0x53,0x43,0x52,0x49,0x50,0x54,0x00 };
static const unsigned char M_TAG7[]    = { 0xFF,0xDF,0xDF,0xDF,0xDF,0xDF,0xDF,0xFF };
static const unsigned char P_IFRAME[]  = { 0x3C,0x49,0x46,0x52,0x41,0x4D,0x45,0x00 };
static const unsigned char P_H1[]      = { 0x3C,0x48,0x31,0x00 };
static const unsigned char M_H1[]      = { 0xFF,0xDF,0xFF,0xFF };
static const unsigned char P_DIV[]     = { 0x3C,0x44,0x49,0x56,0x00 };
static const unsigned char M_TAG4[]    = { 0xFF,0xDF,0xDF,0xDF,0xFF };
static const unsigned char P_FONT[]    = { 0x3C,0x46,0x4F,0x4E,0x54,0x00 };
static const unsigned char P_TABLE[]   = { 0x3C,0x54,0x41,0x42,0x4C,0x45,0x00 };
static const unsigned char M_TAG6[]    = { 0xFF,0xDF,0xDF,0xDF,0xDF,0xDF,0xFF };
static const unsigned char P_A[]       = { 0x3C,0x41,0x00 };
static const unsigned char M_TAG2[]    = { 0xFF,0xDF,0xFF };
static const unsigned char P_STYLE[]   = { 0x3C,0x53,0x54,0x59,0x4C,0x45,0x00 };
static const unsigned char P_TITLE[]   = { 0x3C,0x54,0x49,0x54,0x4C,0x45,0x00 };
static const unsigned char P_B[]       = { 0x3C,0x42,0x00 };
static const unsigned char P_BODY[]    = { 0x3C,0x42,0x4F,0x44,0x59,0x00 };
static const unsigned char P_BR[]      = { 0x3C,0x42,0x52,0x00 };
static const unsigned char M_TAG3[]    = { 0xFF,0xDF,0xDF,0xFF };
static const unsigned char P_P[]       = { 0x3C,0x50,0x00 };
static const unsigned char P_COMMENT[] = { 0x3C,0x21,0x2D,0x2D,0x00 };
static const unsigned char M_COMMENT[] = { 0xFF,0xFF,0xFF,0xFF,0xFF };
static const unsigned char P_XML[]     = { 0x3C,0x3F,0x78,0x6D,0x6C };
static const unsigned char P_PDF[]     = { 0x25,0x50,0x44,0x46,0x2D };

static const SniffRow SCRIPTABLE_ROWS[] = {
    { P_DOCTYPE, M_DOCTYPE, 15, 14, true,  "text/html" },
    { P_HTML,    M_TAG5,     6,  5, true,  "text/html" },
    { P_HEAD,    M_TAG5,     6,  5, true,  "text/html" },
    { P_SCRIPT,  M_TAG7,     8,  7, true,  "text/html" },
    { P_IFRAME,  M_TAG7,     8,  7, true,  "text/html" },
    { P_H1,      M_H1,       4,  3, true,  "text/html" },
    { P_DIV,     M_TAG4,     5,  4, true,  "text/html" },
    { P_FONT,    M_TAG5,     6,  5, true,  "text/html" },
    { P_TABLE,   M_TAG6,     7,  6, true,  "text/html" },
    { P_A,       M_TAG2,     3,  2, true,  "text/html" },
    { P_STYLE,   M_TAG6,     7,  6, true,  "text/html" },
    { P_TITLE,   M_TAG6,     7,  6, true,  "text/html" },
    { P_B,       M_TAG2,     3,  2, true,  "text/html" },
    { P_BODY,    M_TAG5,     6,  5, true,  "text/html" },
    { P_BR,      M_TAG3,     4,  3, true,  "text/html" },
    { P_P,       M_TAG2,     3,  2, true,  "text/html" },
    { P_COMMENT, M_COMMENT,  5,  4, true,  "text/html" },
    { P_XML,     M_5FF,      5, -1, true,  "text/xml" },
    { P_PDF,     M_5FF,      5, -1, false, "application/pdf" },
};

const char *mime_sniff_scriptable_pattern(const unsigned char *header, size_t header_n)
{
    return table_match(header, header_n, SCRIPTABLE_ROWS,
                       (int)(sizeof SCRIPTABLE_ROWS / sizeof *SCRIPTABLE_ROWS));
}

static const unsigned char P_PS[]      = { 0x25,0x21,0x50,0x53,0x2D,0x41,0x64,0x6F,0x62,0x65,0x2D };
static const unsigned char M_PS[]      = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
static const unsigned char P_UTF16BE[] = { 0xFE,0xFF,0x00,0x00 };
static const unsigned char P_UTF16LE[] = { 0xFF,0xFE,0x00,0x00 };
static const unsigned char M_BOM2[]    = { 0xFF,0xFF,0x00,0x00 };
static const unsigned char P_UTF8[]    = { 0xEF,0xBB,0xBF,0x00 };
static const unsigned char M_BOM3[]    = { 0xFF,0xFF,0xFF,0x00 };

static const SniffRow UNKNOWN_ROWS[] = {
    { P_PS,      M_PS,   11, -1, false, "application/postscript" },
    { P_UTF16BE, M_BOM2,  4, -1, false, "text/plain" },
    { P_UTF16LE, M_BOM2,  4, -1, false, "text/plain" },
    { P_UTF8,    M_BOM3,  4, -1, false, "text/plain" },
};

static bool has_binary_data_byte(const unsigned char *d, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (binary_data_byte(d[i])) return true;
    return false;
}

/* Every exit from §7 goes through here, so the record the caller receives is always one THIS component parsed
   out of an essence it named — never a half-built record and never the caller's own input object aliased. */
static void computed(MimeType *out, const char *essence)
{
    DCHECK(essence != NULL && *essence,
           "§7 was about to answer with no MIME type at all — every one of its paths ends in a type, down to "
           "application/octet-stream, so an empty answer here is a step that fell through its own table");
    if (!mime_type_parse(out, essence, strlen(essence)))
        DFAIL("a MIME type this file spells as a literal did not parse — the essences below are the standard's "
              "own table entries, so a failure here is mime_type_parse disagreeing with §4.4 rather than a "
              "resource saying anything");
}

/* §7.1 RULES FOR IDENTIFYING AN UNKNOWN MIME TYPE. */
static void identify_unknown(MimeType *out, bool sniff_scriptable,
                             const unsigned char *header, size_t header_n)
{
    const char *m;

    if (sniff_scriptable) {
        m = mime_sniff_scriptable_pattern(header, header_n);
        if (m) { computed(out, m); return; }
    }
    m = table_match(header, header_n, UNKNOWN_ROWS, (int)(sizeof UNKNOWN_ROWS / sizeof *UNKNOWN_ROWS));
    if (m) { computed(out, m); return; }
    if ((m = mime_sniff_image_pattern(header, header_n)) != NULL)       { computed(out, m); return; }
    if ((m = mime_sniff_audio_video_pattern(header, header_n)) != NULL) { computed(out, m); return; }
    if ((m = mime_sniff_archive_pattern(header, header_n)) != NULL)     { computed(out, m); return; }
    computed(out, has_binary_data_byte(header, header_n) ? "application/octet-stream" : "text/plain");
}

/* §7.2 RULES FOR DISTINGUISHING IF A RESOURCE IS TEXT OR BINARY. Its own note is the reason it is a separate
   function and not three lines inlined into §7.1: it must NEVER answer a scriptable MIME type, and a step
   that shares a body with the scriptable table is one edit away from doing so. */
static void text_or_binary(MimeType *out, const unsigned char *header, size_t header_n)
{
    if (header_n >= 2 && ((header[0] == 0xFE && header[1] == 0xFF) ||
                          (header[0] == 0xFF && header[1] == 0xFE))) { computed(out, "text/plain"); return; }
    if (header_n >= 3 && header[0] == 0xEF && header[1] == 0xBB && header[2] == 0xBF)
        { computed(out, "text/plain"); return; }
    computed(out, has_binary_data_byte(header, header_n) ? "application/octet-stream" : "text/plain");
}

/* §5.1's CHECK-FOR-APACHE-BUG FLAG: the four EXACT supplied-type byte sequences some older Apache builds send
   in place of a type they do not recognise. Exact, not parsed — the flag is about the bytes of the header and
   not about the record they parse into, which is why a `text/plain;charset=UTF-8` without the space is NOT one
   of them and must not be treated as one. */
static bool apache_bug(const char *v)
{
    return v && (!strcmp(v, "text/plain") ||
                 !strcmp(v, "text/plain; charset=ISO-8859-1") ||
                 !strcmp(v, "text/plain; charset=iso-8859-1") ||
                 !strcmp(v, "text/plain; charset=UTF-8"));
}

void mime_sniff_compute(MimeType *out, const char *content_type_value, bool no_sniff,
                        const unsigned char *header, size_t header_n)
{
    MimeType supplied;
    char *essence;
    bool undefined_type, unknown_essence;

    /* WHAT USED TO STAND HERE WAS AN UNCONDITIONAL `DFAIL`, and it is deleted rather than softened, because
       the absence it named has been built. It said §7 was running in the RENDERER and told the next reader to
       build a browser-process instance across a real module boundary; `extension/browser-process.js` is that
       instance — a dedicated Worker of the offscreen with its own realm, its own module and its own thread,
       reached only by postMessage — and this translation unit is linked into THAT program and into no other.
       A DCHECK does not replace it: the invariant it was asserting is "no renderer-side caller reaches §7", and
       that is now enforced by the LINK (engine/build.mjs offers this object to one program's source list only),
       so a renderer that called it would fail to link instead of aborting at run time. An assert re-stating a
       structural guarantee cannot fire, and a check that cannot fire is prose wearing a macro. */
    DCHECK(out != NULL, "§7 was asked to compute a MIME type into nothing");
    DCHECK(header != NULL || header_n == 0,
           "a resource header of non-zero length was passed as a null pointer — §5.2's buffer is a byte "
           "sequence, and a caller that has no bytes says so with a length of zero");
    if (header_n > MIME_SNIFF_HEADER_MAX)
        header_n = MIME_SNIFF_HEADER_MAX;   /* §5.2: the resource header stops at 1445 bytes */

    /* §5.1's supplied MIME type detection, finished: a value that is not a MIME type IS "undefined".
       INITIALISED BEFORE THE TEST AND NOT BY IT. `mime_type_extract` does initialise the record on every path
       it takes — but a NULL value never reaches it (the `||` is satisfied first), and §7 step 2 below frees
       this record on exactly that branch. One line, and it is the difference between a header that is absent
       and a free of whatever the stack was holding. */
    mime_type_init(&supplied);
    undefined_type = (content_type_value == NULL) || !mime_type_extract(&supplied, content_type_value);

    unknown_essence = false;
    if (!undefined_type) {
        essence = mime_type_essence(&supplied);
        CHECK(essence, "mime sniff: OOM reading the supplied type's essence");
        unknown_essence = !strcmp(essence, "unknown/unknown") ||
                          !strcmp(essence, "application/unknown") ||
                          !strcmp(essence, "*/*");
        free(essence);

        /* §7 step 1 — an XML or HTML resource is what its server said it is, before any other question is
           asked. This is ALSO the step that keeps sniffing from ever upgrading a resource INTO a scriptable
           type it was not declared as, which is the privilege-escalation §7.2's note is about. */
        if (mime_type_is_xml(&supplied) || mime_type_is_html(&supplied)) {
            *out = supplied;   /* the record MOVES; `supplied` is not freed after this because `out` owns it */
            return;
        }
    }

    /* §7 step 2 */
    if (undefined_type || unknown_essence) {
        mime_type_free(&supplied);
        identify_unknown(out, !no_sniff, header, header_n);
        return;
    }
    /* §7 step 3 */
    if (no_sniff) { *out = supplied; return; }
    /* §7 step 4 */
    if (apache_bug(content_type_value)) {
        mime_type_free(&supplied);
        text_or_binary(out, header, header_n);
        return;
    }
    /* §7 steps 5 and 6. "supported by the user agent" is read as membership of the group: this engine has no
       decoder for any of them and a subset would make the answer depend on which media this build happens to
       play, which is not a fact about the resource. */
    if (mime_type_is_image(&supplied)) {
        const char *m = mime_sniff_image_pattern(header, header_n);
        if (m) { mime_type_free(&supplied); computed(out, m); return; }
    } else if (mime_type_is_audio_or_video(&supplied)) {
        const char *m = mime_sniff_audio_video_pattern(header, header_n);
        if (m) { mime_type_free(&supplied); computed(out, m); return; }
    }
    /* §7 step 7 */
    *out = supplied;
}
