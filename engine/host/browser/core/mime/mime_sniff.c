/* SNIFFING — WHATWG MIME Sniffing §5 "Handling a resource", §6 "Matching a MIME type pattern", §7 "Determining
 * the computed MIME type of a resource".
 *
 * WHAT THIS ANSWERS AND WHY IT IS NOT A DEFAULT. A response that carries no `Content-Type` still has a type,
 * and every browser computes it from the BYTES. Without §7 the only two things an engine can do with such a
 * response are both wrong: assume `text/html` — which makes an XML or a PDF response parse as HTML and hands
 * back a real tree nobody can tell is a fabrication — or refuse it, which is what this engine did, so a
 * corpus document served by a handler that sets no header aborted the run before any of it was read.
 * §7 is the third answer and it is the standard's: `<!doctype html>` IS text/html because the first fourteen
 * bytes say so under §7.1's table, and `%PDF-` is not, and neither answer is a guess.
 *
 * THE SPLIT AGAINST mime_type.c. That file is §4 — the RECORD and its groups, a pure value type over a string
 * with no bytes anywhere in it. This one is §5–§7 — the algorithms that decide WHICH record a stream of bytes
 * has. §7 is stated entirely in terms of §4's groups, so they are one component; the contracts are different
 * enough (one takes a header value, the other takes a resource header) that they are two translation units,
 * the same split mime_type_encoding.c already makes for the same reason.
 *
 * THE TABLES ARE THE STANDARD'S, CELL FOR CELL. §6.1, §6.2, §6.4, §7.1's two tables and §5.1's four literal
 * header values are transcribed below with the hex the spec prints. They are not extended: §7.1's own note
 * says a user agent "should not implicitly extend this table to include additional byte patterns for any
 * computed MIME type already present in this table, as doing so could introduce privilege escalation
 * vulnerabilities", and the row an engine invents is exactly the row that would.
 *
 * WHAT IS DELIBERATELY NOT HERE. §6.3 "Matching a font type pattern" — §7 never calls it (only §8.7's
 * font-context sniffing does), and CLAUDE.md's rule for core/mime is that a table arrives with the algorithm
 * that first needs it, because a table nobody calls is a table no gate can audit. §8.2–§8.9's other
 * context-specific algorithms are absent for the same reason; §8.1 IS §7 and is what this file exports.
 */
#include <string.h>

#include "check.h"
#include "core/mime/mime_sniff.h"

/* ---- §3's byte classes the algorithms below are written on ---------------------------------------------- */

/* §3: "A whitespace byte (abbreviated 0xWS) is any one of the following bytes: 0x09 (HT), 0x0A (LF), 0x0C
   (FF), 0x0D (CR), 0x20 (SP)." It is NOT Fetch §2.2's HTTP whitespace, which excludes FF and is what
   mime_type.c parses headers with — the two lists differ by exactly that byte and this is the reason both are
   spelled out at their own site rather than shared. */
static bool sniff_ws(unsigned char c)
{
    return c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
}

/* §3: "A binary data byte is a byte in the range 0x00 to 0x08 (NUL to BS), the byte 0x0B (VT), a byte in the
   range 0x0E to 0x1A (SO to SUB), or a byte in the range 0x1C to 0x1F (FS to US)." */
static bool sniff_binary_byte(unsigned char c)
{
    return c <= 0x08 || c == 0x0B || (c >= 0x0E && c <= 0x1A) || (c >= 0x1C && c <= 0x1F);
}

static bool sniff_contains_binary(const unsigned char *h, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (sniff_binary_byte(h[i])) return true;
    return false;
}

/* ---- §6 "Matching a MIME type pattern" ------------------------------------------------------------------ */

/* ONE ARRAY CARRIES BOTH OF §6's COLUMNS. A row is a sequence of CELLS, each holding the pattern mask byte in
   bits 8-15 and the byte pattern byte in bits 0-7 — the two values the spec prints side by side, kept side by
   side. It is one array rather than two because §6's very first step is "Assert: pattern's length is equal to
   mask's length", and a pair of arrays is the shape that assert exists to catch: two lengths written by hand,
   in a forty-row transcription, with nothing but care between them and a mask read off the end of its array.
   With one array the lengths cannot disagree.
   §3's 0xTT IS A THIRD STATE AND GETS ITS OWN BIT. "A tag-terminating byte (abbreviated 0xTT) is any one of
   the following bytes: 0x20 (SP), 0x3E (">")" — so a cell the table prints as TT is a MEMBERSHIP test and not
   an equality, and it cannot be encoded as a byte value because every byte value is a legal pattern byte
   somewhere in these tables. */
typedef unsigned int SniffCell;
#define SC(mask, byte) ((((SniffCell)(mask)) << 8) | (SniffCell)(byte))
#define SC_TT(mask)    (SC(mask, 0) | 0x10000u)
#define SC_END         0xFFFFFFFFu   /* not SC(0,0): the WEBP and AIFF rows both hold a real mask-00 cell */

typedef struct {
    const SniffCell *cells;
    /* §6's `ignored`, which every table prints as one of exactly two values: "Whitespace bytes." or "None." */
    bool ignore_ws;
    const char *type;   /* the row's fourth column */
} SniffRow;

static size_t sniff_row_len(const SniffCell *cells)
{
    size_t n = 0;

    while (cells[n] != SC_END) n++;
    return n;
}

/* §6's PATTERN MATCHING ALGORITHM, given a byte sequence `input`, a byte pattern, a pattern mask, and a set of
   bytes to be ignored. Returns true or false. */
static bool sniff_pattern_match(const unsigned char *input, size_t input_len, const SniffRow *row)
{
    size_t plen = sniff_row_len(row->cells);
    size_t s = 0, p;

    if (input_len < plen) return false;                                  /* step 2 */
    if (row->ignore_ws)                                                  /* steps 3-4 */
        while (s < input_len && sniff_ws(input[s])) s++;
    for (p = 0; p < plen; p++) {                                         /* steps 5-6 */
        unsigned char masked;
        /* §6 BOUNDS THE INPUT AT STEP 2, BEFORE STEP 4 ADVANCES PAST THE IGNORED BYTES, so a row whose
           `ignored` is non-empty can leave `s` fewer than `plen` bytes from the end and step 6.1's read runs
           off the sequence. The read is bounded here instead, and it cannot turn a match into a non-match: a
           byte that is not in the input cannot equal a pattern byte, so every input this rejects is one §6
           rejects at step 2 for the sequence that remains. */
        if (s >= input_len) return false;
        masked = (unsigned char)(input[s] & (unsigned char)((row->cells[p] >> 8) & 0xFFu));
        if (row->cells[p] & 0x10000u) {
            DCHECK(((row->cells[p] >> 8) & 0xFFu) == 0xFF,
                   "a sniffing table row applies a mask other than 0xFF to a 0xTT cell — §3 defines a "
                   "tag-terminating byte as the BYTE 0x20 or 0x3E, so a masked comparison against it is a "
                   "test the standard does not state and the row was transcribed wrong");
            if (masked != 0x20 && masked != 0x3E) return false;
        } else if (masked != (unsigned char)(row->cells[p] & 0xFFu)) {
            return false;
        }
        s++;
    }
    return true;                                                         /* step 7 */
}

/* "Execute the following steps for each row row in the following table … If patternMatched is true, return the
   value in the fourth column of row" — the shape §6.1, §6.2, §6.4 and both of §7.1's tables share. NULL is the
   spec's "undefined". */
static const char *sniff_table(const SniffRow *rows, const unsigned char *h, size_t n)
{
    int i;

    for (i = 0; rows[i].cells; i++)
        if (sniff_pattern_match(h, n, &rows[i])) return rows[i].type;
    return NULL;
}

/* ---- §6.1 "Matching an image type pattern" -------------------------------------------------------------- */

static const SniffCell IMG_ICON[]   = { SC(0xFF,0x00), SC(0xFF,0x00), SC(0xFF,0x01), SC(0xFF,0x00), SC_END };
static const SniffCell IMG_CURSOR[] = { SC(0xFF,0x00), SC(0xFF,0x00), SC(0xFF,0x02), SC(0xFF,0x00), SC_END };
static const SniffCell IMG_BMP[]    = { SC(0xFF,0x42), SC(0xFF,0x4D), SC_END };
static const SniffCell IMG_GIF87[]  = { SC(0xFF,0x47), SC(0xFF,0x49), SC(0xFF,0x46), SC(0xFF,0x38),
                                        SC(0xFF,0x37), SC(0xFF,0x61), SC_END };
static const SniffCell IMG_GIF89[]  = { SC(0xFF,0x47), SC(0xFF,0x49), SC(0xFF,0x46), SC(0xFF,0x38),
                                        SC(0xFF,0x39), SC(0xFF,0x61), SC_END };
static const SniffCell IMG_WEBP[]   = { SC(0xFF,0x52), SC(0xFF,0x49), SC(0xFF,0x46), SC(0xFF,0x46),
                                        SC(0x00,0x00), SC(0x00,0x00), SC(0x00,0x00), SC(0x00,0x00),
                                        SC(0xFF,0x57), SC(0xFF,0x45), SC(0xFF,0x42), SC(0xFF,0x50),
                                        SC(0xFF,0x56), SC(0xFF,0x50), SC_END };
static const SniffCell IMG_PNG[]    = { SC(0xFF,0x89), SC(0xFF,0x50), SC(0xFF,0x4E), SC(0xFF,0x47),
                                        SC(0xFF,0x0D), SC(0xFF,0x0A), SC(0xFF,0x1A), SC(0xFF,0x0A), SC_END };
static const SniffCell IMG_JPEG[]   = { SC(0xFF,0xFF), SC(0xFF,0xD8), SC(0xFF,0xFF), SC_END };

static const SniffRow IMAGE_TABLE[] = {
    { IMG_ICON,   false, "image/x-icon" },   /* a Windows Icon signature */
    { IMG_CURSOR, false, "image/x-icon" },   /* a Windows Cursor signature */
    { IMG_BMP,    false, "image/bmp"    },   /* the string "BM" */
    { IMG_GIF87,  false, "image/gif"    },   /* the string "GIF87a" */
    { IMG_GIF89,  false, "image/gif"    },   /* the string "GIF89a" */
    { IMG_WEBP,   false, "image/webp"   },   /* "RIFF", four bytes, "WEBPVP" */
    { IMG_PNG,    false, "image/png"    },   /* an error-checking byte, "PNG", CR LF SUB LF */
    { IMG_JPEG,   false, "image/jpeg"   },   /* the JPEG Start of Image marker and another marker's indicator */
    { NULL, false, NULL }
};

/* §6.1's algorithm is exactly its table, so this is the table's walk under the algorithm's name. */
static const char *sniff_image_pattern(const unsigned char *h, size_t n)
{
    return sniff_table(IMAGE_TABLE, h, n);
}

/* ---- §6.2 "Matching an audio or video type pattern" ----------------------------------------------------- */

static const SniffCell AV_AIFF[]  = { SC(0xFF,0x46), SC(0xFF,0x4F), SC(0xFF,0x52), SC(0xFF,0x4D),
                                      SC(0x00,0x00), SC(0x00,0x00), SC(0x00,0x00), SC(0x00,0x00),
                                      SC(0xFF,0x41), SC(0xFF,0x49), SC(0xFF,0x46), SC(0xFF,0x46), SC_END };
static const SniffCell AV_ID3[]   = { SC(0xFF,0x49), SC(0xFF,0x44), SC(0xFF,0x33), SC_END };
static const SniffCell AV_OGG[]   = { SC(0xFF,0x4F), SC(0xFF,0x67), SC(0xFF,0x67), SC(0xFF,0x53),
                                      SC(0xFF,0x00), SC_END };
static const SniffCell AV_MIDI[]  = { SC(0xFF,0x4D), SC(0xFF,0x54), SC(0xFF,0x68), SC(0xFF,0x64),
                                      SC(0xFF,0x00), SC(0xFF,0x00), SC(0xFF,0x00), SC(0xFF,0x06), SC_END };
static const SniffCell AV_AVI[]   = { SC(0xFF,0x52), SC(0xFF,0x49), SC(0xFF,0x46), SC(0xFF,0x46),
                                      SC(0x00,0x00), SC(0x00,0x00), SC(0x00,0x00), SC(0x00,0x00),
                                      SC(0xFF,0x41), SC(0xFF,0x56), SC(0xFF,0x49), SC(0xFF,0x20), SC_END };
static const SniffCell AV_WAVE[]  = { SC(0xFF,0x52), SC(0xFF,0x49), SC(0xFF,0x46), SC(0xFF,0x46),
                                      SC(0x00,0x00), SC(0x00,0x00), SC(0x00,0x00), SC(0x00,0x00),
                                      SC(0xFF,0x57), SC(0xFF,0x41), SC(0xFF,0x56), SC(0xFF,0x45), SC_END };

static const SniffRow AV_TABLE[] = {
    { AV_AIFF, false, "audio/aiff"      },   /* "FORM", four bytes, "AIFF" */
    { AV_ID3,  false, "audio/mpeg"      },   /* "ID3", the ID3v2-tagged MP3 signature */
    { AV_OGG,  false, "application/ogg" },   /* "OggS" followed by NUL */
    { AV_MIDI, false, "audio/midi"      },   /* "MThd" followed by the number 6 in 32 bits big-endian */
    { AV_AVI,  false, "video/avi"       },   /* "RIFF", four bytes, "AVI " */
    { AV_WAVE, false, "audio/wave"      },   /* "RIFF", four bytes, "WAVE" */
    { NULL, false, NULL }
};

/* §6.2.1 "Signature for MP4". */
static bool sniff_sig_mp4(const unsigned char *seq, size_t length)
{
    unsigned long box_size, bytes_read;

    if (length < 12) return false;                                                     /* step 3 */
    box_size = ((unsigned long)seq[0] << 24) | ((unsigned long)seq[1] << 16) |
               ((unsigned long)seq[2] << 8)  |  (unsigned long)seq[3];                 /* step 4 */
    if (length < box_size || box_size % 4 != 0) return false;                          /* step 5 */
    if (memcmp(seq + 4, "ftyp", 4) != 0) return false;                                 /* step 6 */
    if (memcmp(seq + 8, "mp4", 3) == 0) return true;                                   /* step 7 */
    /* Step 8: "Let bytes-read be 16." — the standard's own note: "This ignores the four bytes that correspond
       to the version number of the major brand." */
    bytes_read = 16;
    /* Step 9 reads three bytes at `bytes-read` while its condition bounds only `bytes-read` itself, so the
       last turn of the standard's loop reads up to two bytes past `box-size` — and `box-size` is the bound
       step 5 established against `length`. The loop is written to require the whole three-byte comparison to
       lie inside `box-size`, which rejects nothing: a three-byte compare against fewer than three bytes cannot
       succeed. */
    while (bytes_read + 3 <= box_size) {
        if (memcmp(seq + bytes_read, "mp4", 3) == 0) return true;
        bytes_read += 4;
    }
    return false;                                                                      /* step 10 */
}

/* §6.2.2's "parse a vint", of which §6.2.2 uses ONLY the returned `number size` — its `parsed number` is
   computed by the standard and read by nobody, so building it here would be a value with no consumer. The
   standard's own steps index the sequence with a variable it initialises to 0 and never advances before the
   read; the operation is stated as "starting at index iter", which is the byte this reads. */
static size_t sniff_webm_vint_size(const unsigned char *seq, size_t length, size_t iter)
{
    unsigned mask = 128;
    size_t number_size = 1;

    while (number_size < 8 && number_size < length) {
        if ((seq[iter] & mask) != 0) break;
        mask >>= 1;
        number_size++;
    }
    return number_size;
}

/* §6.2.2 "Signature for WebM". */
static bool sniff_sig_webm(const unsigned char *seq, size_t length)
{
    static const unsigned char EBML[4] = { 0x1A, 0x45, 0xDF, 0xA3 };
    size_t iter;

    if (length < 4) return false;                                                      /* step 3 */
    if (memcmp(seq, EBML, 4) != 0) return false;                                       /* step 4 */
    iter = 4;                                                                          /* step 5 */
    while (iter < length && iter < 38) {                                               /* step 6 */
        /* Step 6.1 compares TWO bytes while the loop bounds only the first, so the standard's read runs one
           byte past the sequence on its last turn. Requiring both bytes rejects nothing — a two-byte compare
           against one byte cannot succeed. */
        if (iter + 1 < length && seq[iter] == 0x42 && seq[iter + 1] == 0x82) {
            iter += 2;                                                                 /* step 6.1.1 */
            if (iter >= length) break;                                                 /* step 6.1.2 */
            iter += sniff_webm_vint_size(seq, length, iter);                           /* steps 6.1.3-6.1.4 */
            if (iter >= length - 4) break;                                             /* step 6.1.5 */
            /* Step 6.1.6's "matching a padded sequence": the four bytes at `iter` are "webm". Step 6.1.5 has
               just established that exactly that many remain, which is what leaves the padded form — "eventually
               preceded by bytes with a value of 0x00" — no room the caller could give it. */
            if (memcmp(seq + iter, "webm", 4) == 0) return true;                       /* step 6.1.7 */
        }
        iter += 1;                                                                     /* step 6.2 */
    }
    return false;                                                                      /* step 7 */
}

/* §6.2.3's three rate tables, printed by index. */
static const unsigned long MP3_RATES[15] = { 0, 32000, 40000, 48000, 56000, 64000, 80000, 96000,
                                             112000, 128000, 160000, 192000, 224000, 256000, 320000 };
static const unsigned long MP25_RATES[15] = { 0, 8000, 16000, 24000, 32000, 40000, 48000, 56000,
                                              64000, 80000, 96000, 112000, 128000, 144000, 160000 };
static const unsigned long MP3_SAMPLE_RATE[3] = { 44100, 48000, 32000 };

/* §6.2.3's "match an mp3 header", using a byte sequence of length `length` at offset `s`.
   THE OPERATOR PRECEDENCE IS THE STANDARD'S OWN, TAKEN FROM THE STEP THAT PARENTHESISES IT. Steps 3, 5 and 6
   are printed as `sequence[s + 1] & 0x06 >> 1` and read literally that is `sequence[s+1] & (0x06 >> 1)`; step
   9 prints the SAME quantity as `4 - ((sequence[s + 1] & 0x06) >> 1)`, so the mask-then-shift order is the
   standard's and the missing parentheses are typography.
   STEP 2 IS TRANSCRIBED AS WRITTEN, `and` AND NOT `or`: "If sequence[s] is not equal to 0xff and
   sequence[s + 1] & 0xe0 is not equal to 0xe0, return false." A sequence that fails only one of those two
   halves therefore reaches step 3, and what rejects it is steps 4 and 9 — a byte whose 0x06 bits are 0 has
   layer 0, and one whose final-layer is not 3 returns false. */
static bool sniff_mp3_match_header(const unsigned char *seq, size_t length, size_t s)
{
    unsigned layer, bit_rate, sample_rate, final_layer;

    /* Step 1 is stated as "If length is less than 4" while steps 2-8 read sequence[s + 2]; the bound the reads
       need is the bytes REMAINING at `s`, which at the operation's first caller (s is 0) is the same number. */
    if (length - s < 4) return false;
    if (seq[s] != 0xff && (seq[s + 1] & 0xe0) != 0xe0) return false;
    layer = (unsigned)(seq[s + 1] & 0x06) >> 1;
    if (layer == 0) return false;
    bit_rate = (unsigned)(seq[s + 2] & 0xf0) >> 4;
    if (bit_rate == 15) return false;
    sample_rate = (unsigned)(seq[s + 2] & 0x0c) >> 2;
    if (sample_rate == 3) return false;
    final_layer = 4 - ((unsigned)(seq[s + 1] & 0x06) >> 1);
    if (final_layer != 3) return false;
    return true;
}

/* §6.2.3's "parse an mp3 frame" followed by its "compute an mp3 frame size" — one function because the second
   reads four locals the first computes and the standard states them back to back over one offset. */
static unsigned long sniff_mp3_framesize(const unsigned char *seq, size_t s)
{
    unsigned version = (unsigned)(seq[s + 1] & 0x18) >> 3;
    unsigned bitrate_index = (unsigned)(seq[s + 2] & 0xf0) >> 4;
    unsigned samplerate_index = (unsigned)(seq[s + 2] & 0x0c) >> 2;
    unsigned pad = (unsigned)(seq[s + 2] & 0x02) >> 1;
    unsigned long bitrate, samplerate, scale, size;

    DCHECK(bitrate_index < 15 && samplerate_index < 3,
           "an mp3 frame was parsed at an offset whose header match a bitrate index of 15 or a sample-rate "
           "index of 3 would have rejected — §6.2.3 runs match an mp3 header FIRST and both of those are its "
           "own early returns, so reaching here means the two were run out of order");
    bitrate = (version & 0x01) ? MP25_RATES[bitrate_index] : MP3_RATES[bitrate_index];
    samplerate = MP3_SAMPLE_RATE[samplerate_index];
    if (version == 2) samplerate /= 2;
    if (version == 0) samplerate /= 4;
    scale = (version == 1) ? 72 : 144;
    DCHECK(samplerate != 0,
           "an mp3 frame size was computed against a sample rate of zero — §6.2.3's sample-rate table holds "
           "44100, 48000 and 32000 and its two divisors are 2 and 4, so a zero here is a table this file "
           "transcribed wrong and the division that follows is the standard's");
    size = bitrate * scale / samplerate;
    if (pad != 0) size += 1;
    return size;
}

/* §6.2.3 "Signature for MP3 without ID3". */
static bool sniff_sig_mp3_no_id3(const unsigned char *seq, size_t length)
{
    size_t s = 0;                                                                      /* step 3 */
    unsigned long skipped_bytes;

    if (!sniff_mp3_match_header(seq, length, s)) return false;                          /* step 4 */
    skipped_bytes = sniff_mp3_framesize(seq, s);                                        /* steps 5-6 */
    /* Step 7 is printed as "If skipped-bytes is less than 4, or skipped-bytes is greater than s - length,
       return false." `s` is 0 and `length` is the sequence length, so `s - length` is not a byte count at all
       and the literal reading rejects EVERY input — the operation could never return true and step 8's
       "Increment s by skipped-bytes" could never run. The bound the step needs, and the only one under which
       step 8 and step 9's second header match are reachable, is the bytes remaining at `s`. */
    if (skipped_bytes < 4 || skipped_bytes > (unsigned long)(length - s)) return false;
    s += (size_t)skipped_bytes;                                                        /* step 8 */
    return sniff_mp3_match_header(seq, length, s);                                     /* step 9 */
}

/* §6.2's algorithm: its table, then its three signature operations. */
static const char *sniff_av_pattern(const unsigned char *h, size_t n)
{
    const char *m = sniff_table(AV_TABLE, h, n);

    if (m) return m;
    if (sniff_sig_mp4(h, n)) return "video/mp4";
    if (sniff_sig_webm(h, n)) return "video/webm";
    if (sniff_sig_mp3_no_id3(h, n)) return "audio/mpeg";
    return NULL;
}

/* ---- §6.4 "Matching an archive type pattern" ------------------------------------------------------------ */

static const SniffCell ARC_GZIP[] = { SC(0xFF,0x1F), SC(0xFF,0x8B), SC(0xFF,0x08), SC_END };
static const SniffCell ARC_ZIP[]  = { SC(0xFF,0x50), SC(0xFF,0x4B), SC(0xFF,0x03), SC(0xFF,0x04), SC_END };
static const SniffCell ARC_RAR[]  = { SC(0xFF,0x52), SC(0xFF,0x61), SC(0xFF,0x72), SC(0xFF,0x21),
                                      SC(0xFF,0x1A), SC(0xFF,0x07), SC(0xFF,0x00), SC_END };

static const SniffRow ARCHIVE_TABLE[] = {
    { ARC_GZIP, false, "application/x-gzip"           },   /* the GZIP archive signature */
    { ARC_ZIP,  false, "application/zip"              },   /* "PK" followed by ETX EOT */
    { ARC_RAR,  false, "application/x-rar-compressed" },   /* "Rar!" followed by SUB BEL NUL */
    { NULL, false, NULL }
};

static const char *sniff_archive_pattern(const unsigned char *h, size_t n)
{
    return sniff_table(ARCHIVE_TABLE, h, n);
}

/* ---- §7.1's two tables ---------------------------------------------------------------------------------- */

/* The FIRST table — the one §7.1 walks only when the sniff-scriptable flag is set, because every type it can
   return is one a page can be made to run. */
static const SniffCell U_DOCTYPE[] = { SC(0xFF,0x3C), SC(0xFF,0x21), SC(0xDF,0x44), SC(0xDF,0x4F),
                                       SC(0xDF,0x43), SC(0xDF,0x54), SC(0xDF,0x59), SC(0xDF,0x50),
                                       SC(0xDF,0x45), SC(0xFF,0x20), SC(0xDF,0x48), SC(0xDF,0x54),
                                       SC(0xDF,0x4D), SC(0xDF,0x4C), SC_TT(0xFF), SC_END };
static const SniffCell U_HTML[]    = { SC(0xFF,0x3C), SC(0xDF,0x48), SC(0xDF,0x54), SC(0xDF,0x4D),
                                       SC(0xDF,0x4C), SC_TT(0xFF), SC_END };
static const SniffCell U_HEAD[]    = { SC(0xFF,0x3C), SC(0xDF,0x48), SC(0xDF,0x45), SC(0xDF,0x41),
                                       SC(0xDF,0x44), SC_TT(0xFF), SC_END };
static const SniffCell U_SCRIPT[]  = { SC(0xFF,0x3C), SC(0xDF,0x53), SC(0xDF,0x43), SC(0xDF,0x52),
                                       SC(0xDF,0x49), SC(0xDF,0x50), SC(0xDF,0x54), SC_TT(0xFF), SC_END };
static const SniffCell U_IFRAME[]  = { SC(0xFF,0x3C), SC(0xDF,0x49), SC(0xDF,0x46), SC(0xDF,0x52),
                                       SC(0xDF,0x41), SC(0xDF,0x4D), SC(0xDF,0x45), SC_TT(0xFF), SC_END };
static const SniffCell U_H1[]      = { SC(0xFF,0x3C), SC(0xDF,0x48), SC(0xFF,0x31), SC_TT(0xFF), SC_END };
static const SniffCell U_DIV[]     = { SC(0xFF,0x3C), SC(0xDF,0x44), SC(0xDF,0x49), SC(0xDF,0x56),
                                       SC_TT(0xFF), SC_END };
static const SniffCell U_FONT[]    = { SC(0xFF,0x3C), SC(0xDF,0x46), SC(0xDF,0x4F), SC(0xDF,0x4E),
                                       SC(0xDF,0x54), SC_TT(0xFF), SC_END };
static const SniffCell U_TABLE[]   = { SC(0xFF,0x3C), SC(0xDF,0x54), SC(0xDF,0x41), SC(0xDF,0x42),
                                       SC(0xDF,0x4C), SC(0xDF,0x45), SC_TT(0xFF), SC_END };
static const SniffCell U_A[]       = { SC(0xFF,0x3C), SC(0xDF,0x41), SC_TT(0xFF), SC_END };
static const SniffCell U_STYLE[]   = { SC(0xFF,0x3C), SC(0xDF,0x53), SC(0xDF,0x54), SC(0xDF,0x59),
                                       SC(0xDF,0x4C), SC(0xDF,0x45), SC_TT(0xFF), SC_END };
static const SniffCell U_TITLE[]   = { SC(0xFF,0x3C), SC(0xDF,0x54), SC(0xDF,0x49), SC(0xDF,0x54),
                                       SC(0xDF,0x4C), SC(0xDF,0x45), SC_TT(0xFF), SC_END };
static const SniffCell U_B[]       = { SC(0xFF,0x3C), SC(0xDF,0x42), SC_TT(0xFF), SC_END };
static const SniffCell U_BODY[]    = { SC(0xFF,0x3C), SC(0xDF,0x42), SC(0xDF,0x4F), SC(0xDF,0x44),
                                       SC(0xDF,0x59), SC_TT(0xFF), SC_END };
static const SniffCell U_BR[]      = { SC(0xFF,0x3C), SC(0xDF,0x42), SC(0xDF,0x52), SC_TT(0xFF), SC_END };
static const SniffCell U_P[]       = { SC(0xFF,0x3C), SC(0xDF,0x50), SC_TT(0xFF), SC_END };
static const SniffCell U_COMMENT[] = { SC(0xFF,0x3C), SC(0xFF,0x21), SC(0xFF,0x2D), SC(0xFF,0x2D),
                                       SC_TT(0xFF), SC_END };
static const SniffCell U_XML[]     = { SC(0xFF,0x3C), SC(0xFF,0x3F), SC(0xFF,0x78), SC(0xFF,0x6D),
                                       SC(0xFF,0x6C), SC_END };
static const SniffCell U_PDF[]     = { SC(0xFF,0x25), SC(0xFF,0x50), SC(0xFF,0x44), SC(0xFF,0x46),
                                       SC(0xFF,0x2D), SC_END };

static const SniffRow UNKNOWN_SCRIPTABLE[] = {
    { U_DOCTYPE, true,  "text/html"       },
    { U_HTML,    true,  "text/html"       },
    { U_HEAD,    true,  "text/html"       },
    { U_SCRIPT,  true,  "text/html"       },
    { U_IFRAME,  true,  "text/html"       },
    { U_H1,      true,  "text/html"       },
    { U_DIV,     true,  "text/html"       },
    { U_FONT,    true,  "text/html"       },
    { U_TABLE,   true,  "text/html"       },
    { U_A,       true,  "text/html"       },
    { U_STYLE,   true,  "text/html"       },
    { U_TITLE,   true,  "text/html"       },
    { U_B,       true,  "text/html"       },
    { U_BODY,    true,  "text/html"       },
    { U_BR,      true,  "text/html"       },
    { U_P,       true,  "text/html"       },
    { U_COMMENT, true,  "text/html"       },
    { U_XML,     true,  "text/xml"        },
    { U_PDF,     false, "application/pdf" },   /* the PDF signature — "None." and not whitespace */
    { NULL, false, NULL }
};

/* The SECOND table, which §7.1 walks unconditionally: a PostScript program and the three BOMs, none of which
   a page can be made to execute. */
static const SniffCell U_PS[]      = { SC(0xFF,0x25), SC(0xFF,0x21), SC(0xFF,0x50), SC(0xFF,0x53),
                                       SC(0xFF,0x2D), SC(0xFF,0x41), SC(0xFF,0x64), SC(0xFF,0x6F),
                                       SC(0xFF,0x62), SC(0xFF,0x65), SC(0xFF,0x2D), SC_END };
static const SniffCell U_UTF16BE[] = { SC(0xFF,0xFE), SC(0xFF,0xFF), SC(0x00,0x00), SC(0x00,0x00), SC_END };
static const SniffCell U_UTF16LE[] = { SC(0xFF,0xFF), SC(0xFF,0xFE), SC(0x00,0x00), SC(0x00,0x00), SC_END };
static const SniffCell U_UTF8[]    = { SC(0xFF,0xEF), SC(0xFF,0xBB), SC(0xFF,0xBF), SC(0x00,0x00), SC_END };

static const SniffRow UNKNOWN_ALWAYS[] = {
    { U_PS,      false, "application/postscript" },
    { U_UTF16BE, false, "text/plain"             },   /* UTF-16BE BOM */
    { U_UTF16LE, false, "text/plain"             },   /* UTF-16LE BOM */
    { U_UTF8,    false, "text/plain"             },   /* UTF-8 BOM */
    { NULL, false, NULL }
};

/* ---- the computed type, and the two rule sets that produce one from bytes alone ------------------------- */

/* Every type §7.1 and §7.2 return is a LITERAL from their tables, and it becomes a record by §4.4 rather than
   by a second construction path — so an entry that is not a MIME type is caught here rather than travelling
   as a half-built record. */
static void sniff_literal(MimeType *out, const char *essence)
{
    bool ok = mime_type_parse(out, essence, strlen(essence));

    DCHECK(ok, "a computed MIME type taken from a sniffing table did not parse — §4.4 parses every essence "
               "these tables print, so this is a row transcribed with something that is not a MIME type");
    (void)ok;
}

/* §7.1 "Identifying a resource with an unknown MIME type" — the RULES FOR IDENTIFYING AN UNKNOWN MIME TYPE. */
static void sniff_unknown(MimeType *out, const MimeSniffResource *r, bool sniff_scriptable)
{
    const char *m = NULL;

    if (sniff_scriptable) {                                                            /* step 1 */
        m = sniff_table(UNKNOWN_SCRIPTABLE, r->header, r->header_len);
        if (m) { sniff_literal(out, m); return; }
    }
    m = sniff_table(UNKNOWN_ALWAYS, r->header, r->header_len);                         /* step 2 */
    if (m) { sniff_literal(out, m); return; }
    m = sniff_image_pattern(r->header, r->header_len);                                 /* steps 3-4 */
    if (!m) m = sniff_av_pattern(r->header, r->header_len);                            /* steps 5-6 */
    if (!m) m = sniff_archive_pattern(r->header, r->header_len);                       /* steps 7-8 */
    if (m) { sniff_literal(out, m); return; }
    /* Steps 9-10. "It is critical that the rules for distinguishing if a resource is text or binary never
       determine the computed MIME type to be a scriptable MIME type" is §7.2's note, and it is the same
       reason these two arms are the only ones left: a resource nothing above recognised is either text or an
       opaque stream, and neither is something to run. */
    sniff_literal(out, sniff_contains_binary(r->header, r->header_len) ? "application/octet-stream"
                                                                      : "text/plain");
}

/* §7.2 "Sniffing a mislabeled binary resource" — the RULES FOR DISTINGUISHING IF A RESOURCE IS TEXT OR BINARY,
   which §7 step 4 reaches for exactly the four `Content-Type` values §5.1's check-for-apache-bug table names. */
static void sniff_text_or_binary(MimeType *out, const MimeSniffResource *r)
{
    const unsigned char *h = r->header;
    size_t n = r->header_len;                                                          /* step 1 */

    if (n >= 2 && ((h[0] == 0xFE && h[1] == 0xFF) || (h[0] == 0xFF && h[1] == 0xFE))) { /* step 2 */
        sniff_literal(out, "text/plain");
        return;
    }
    if (n >= 3 && h[0] == 0xEF && h[1] == 0xBB && h[2] == 0xBF) {                       /* step 3 */
        sniff_literal(out, "text/plain");
        return;
    }
    sniff_literal(out, sniff_contains_binary(h, n) ? "application/octet-stream" : "text/plain");
}

/* ---- §5.1 "Interpreting the resource metadata" ----------------------------------------------------------- */

bool mime_sniff_supplied(MimeType *out, bool *apache_bug, const char *value)
{
    /* §5.1's check-for-apache-bug table, byte for byte — the exact `Content-Type` values "some older
       installations of Apache" send "when serving files with unrecognized MIME types". The comparison is
       EXACT, not a parse-and-compare: `text/plain;charset=UTF-8` without the space is a different byte
       sequence and is not one of these, which is the whole point of a table of byte sequences. */
    static const char *const APACHE_BUG[] = { "text/plain",
                                              "text/plain; charset=ISO-8859-1",
                                              "text/plain; charset=iso-8859-1",
                                              "text/plain; charset=UTF-8",
                                              NULL };
    int i;

    mime_type_init(out);
    DCHECK(apache_bug != NULL,
           "§5.1's supplied MIME type detection was asked for a supplied type with nowhere to put the "
           "check-for-apache-bug flag — §5 keeps both as metadata of ONE resource and §7 step 4 branches on "
           "the flag, so a caller that drops it has asked half the question");
    /* WRITTEN ON EVERY PATH, INCLUDING THE ONE THAT FAILS. A flag the caller has to remember to clear is a
       flag that reads as set, which is CLAUDE.md's defaulted-field defect with the default on the producer's
       side of the seam. */
    *apache_bug = false;
    if (!value) return false;   /* no `Content-Type` header: step 5's "the supplied MIME type is undefined" */
    for (i = 0; APACHE_BUG[i]; i++)
        if (!strcmp(value, APACHE_BUG[i])) { *apache_bug = true; break; }
    if (!mime_type_parse(out, value, strlen(value))) {   /* step 5 */
        mime_type_free(out);
        return false;
    }
    return true;
}

/* ---- §7 "Determining the computed MIME type of a resource" ----------------------------------------------- */

/* §7 step 2's second condition: the supplied MIME type's essence is `unknown/unknown`, `application/unknown`,
   or the WILDCARD ESSENCE — U+002A, U+002F, U+002A, which cannot be written out in a C comment and is the
   third entry of the step's own list.
   THE WILDCARD REACHES HERE BECAUSE §5.1 PARSES THE HEADER VALUE WITH §4.4 and does not run Fetch §2.2.2 "Headers"'s
   extraction, which SKIPS a wildcard candidate ("if temporaryMimeType is failure or its essence is the
   wildcard, then continue"). Two algorithms over one header, and this step exists precisely for the value the
   other one throws away — which is one of the reasons mime_sniff_supplied is not mime_type_extract. */
static bool sniff_essence_unknown(const MimeType *m)
{
    if (!m->type || !m->subtype) return false;
    if (!strcmp(m->type, "unknown") && !strcmp(m->subtype, "unknown")) return true;
    if (!strcmp(m->type, "application") && !strcmp(m->subtype, "unknown")) return true;
    return !strcmp(m->type, "*") && !strcmp(m->subtype, "*");
}

static void sniff_computed_run(MimeType *out, const MimeSniffResource *r)
{
    const char *m;

    /* Step 1. An XML or HTML supplied type is FINAL — the bytes are never consulted, which is why a server
       that says `text/html` gets an HTML document even when its body is a PNG. */
    if (r->supplied && (mime_type_is_xml(r->supplied) || mime_type_is_html(r->supplied))) {
        mime_type_copy(out, r->supplied);
        return;
    }
    /* Step 2. The sniff-scriptable flag is "the inverse of the no-sniff flag": a server that said `nosniff`
       does not get its unknown resource run as HTML. */
    if (!r->supplied || sniff_essence_unknown(r->supplied)) {
        sniff_unknown(out, r, !r->no_sniff);
        return;
    }
    if (r->no_sniff) {                                                                 /* step 3 */
        mime_type_copy(out, r->supplied);
        return;
    }
    if (r->apache_bug) {                                                               /* step 4 */
        sniff_text_or_binary(out, r);
        return;
    }
    /* Steps 5-6, then 7-8. The GROUP half of each condition is §4.6's and is asked here; the "supported by the
       user agent" half is the caller's, for the reason mime_sniff.h states. */
    if (r->ua_renders_supplied && mime_type_is_image(r->supplied)) {
        m = sniff_image_pattern(r->header, r->header_len);
        if (m) { sniff_literal(out, m); return; }
    }
    if (r->ua_renders_supplied && mime_type_is_audio_or_video(r->supplied)) {
        m = sniff_av_pattern(r->header, r->header_len);
        if (m) { sniff_literal(out, m); return; }
    }
    mime_type_copy(out, r->supplied);                                                  /* step 9 */
}

void mime_sniff_computed(MimeType *out, const MimeSniffResource *r)
{
    mime_type_init(out);
    DCHECK(r != NULL, "§7's MIME type sniffing algorithm was run with no resource — §5 states its metadata as "
                      "a property of a resource and every step below reads one of them");
    DCHECK(r->header != NULL || r->header_len == 0,
           "§5.2's resource header is a byte sequence and this one has a length with no bytes behind it — an "
           "empty resource is length 0, and every §6 pattern match reads through this pointer");
    DCHECK(r->header_len <= MIME_SNIFF_RESOURCE_HEADER_MAX,
           "a whole response body was handed to §7 as §5.2's RESOURCE HEADER, which reads at most 1445 bytes. "
           "It is not a harmless excess: §7.1's and §7.2's last step is `contains no binary data bytes` over "
           "the resource header, so a binary byte past the 1445th turns text/plain into "
           "application/octet-stream and the answer stops being the one every browser computes");
    DCHECK(r->supplied != NULL || !r->ua_renders_supplied,
           "§7 was told the user agent renders the SUPPLIED type of a resource that has no supplied type — "
           "steps 5 and 7 ask that question of a type, so an answer without one came from a caller that "
           "computed it against something else");
    DCHECK(!r->supplied || (r->supplied->type && r->supplied->subtype),
           "§5.1's supplied MIME type was handed over as a record with no type or no subtype — the algorithm "
           "answers `undefined` with a NULL record, and an EMPTY one is what mime_sniff_supplied leaves "
           "behind on failure: the caller passed the record it should have passed NULL for");

    sniff_computed_run(out, r);

    /* THE COMPUTED MIME TYPE IS NEVER UNDEFINED — a property of §7 and not of this file. Every arm that keeps
       the supplied type is below step 2, which has already established that the supplied type is defined, and
       every other arm returns a literal from §7.1's or §7.2's tables. A caller may therefore read `out` as a
       type without testing for one, which is what lets HTML §7.4.5's dispatch take a record. */
    DCHECK(out->type != NULL && out->subtype != NULL,
           "§7's MIME type sniffing algorithm produced no computed MIME type — every one of its arms produces "
           "one, so an empty record here is an arm that fell through without writing its answer");
}
