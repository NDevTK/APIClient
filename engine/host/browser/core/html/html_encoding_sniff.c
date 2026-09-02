/* HTML §13.2.3.2 "Determining the character encoding" — the ENCODING SNIFFING ALGORITHM, its PRESCAN, and
 * HTML §2.5.3 "Extracting character encodings from meta elements". Declared in html_encoding_sniff.h.
 *
 * ONE PROBLEM: given the bytes of a resource and whatever the transport layer said about them, WHICH ENCODING
 * IS THIS DOCUMENT. Nothing here builds a tree, decodes anything, or touches a Document — the answer is an id
 * in the Encoding registry and the caller does the rest, which is what makes the whole file exercisable with
 * one byte string and one expected id.
 *
 * WHY THE PRESCAN IS NOT "JUST LOOK FOR <meta charset>". It is a byte-level state machine with its own
 * attribute grammar, and every piece of it is load-bearing in a way a substring search is not:
 *   - `<!-- ... -->` is SKIPPED, so `<!-- <meta charset=big5> -->` does not decide the document's encoding.
 *   - a `<meta>` whose `charset` attribute is a label the registry does not know is a FAILURE that continues
 *     the scan rather than an answer, so `<meta charset=nonsense><meta charset=shift_jis>` is Shift_JIS.
 *   - `http-equiv`/`content` is the OTHER declaration form and it is PRAGMA-GATED: `<meta content="text/html;
 *     charset=big5">` with no `http-equiv=content-type` beside it decides nothing, which is the `need pragma` /
 *     `got pragma` pair below and the reason a `content` attribute cannot be read on its own.
 *   - the attribute grammar is the TOKENIZER'S, not the tree's: duplicate names are ignored after the first,
 *     `/` is whitespace between attributes, and an unquoted value ends at whitespace or `>`.
 *   - `x-user-defined` becomes `windows-1252` and UTF-16BE/LE becomes UTF-8, because a document cannot be
 *     parsed in either. That is why `Document-characterSet-normalization-1.html` lists `utf-16`, `utf-16le` and
 *     `utf-16be` under the name `UTF-8`.
 * A search for the string `charset` gets every one of those wrong, and gets them wrong SILENTLY — it answers a
 * real encoding, from a real declaration, that the standard says is not the one in force.
 *
 * THE SCAN NEVER READS PAST THE WINDOW IT WAS GIVEN. §13.2.3.2 states the abort as a property of the pointer —
 * "the user agent either runs out of bytes (meaning the position pointer created in the first step below goes
 * beyond the end of the byte stream obtained so far) or reaches its end condition, then abort the prescan a
 * byte stream to determine its encoding algorithm" — so every read below is guarded and
 * an overrun ABORTS rather than returning a partial answer. The guard is not defensive programming: running
 * off the end is a NORMAL outcome of prescanning a truncated window, and it has its own defined result. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/encoding/encoding.h"
#include "core/html/html_encoding_sniff.h"
#include "core/mime/mime_type.h"

/* Infra's ASCII WHITESPACE, which is what §2.5.3's "skip any ASCII whitespace" and §4.2's "remove any leading
   and trailing ASCII whitespace" both name. The prescan's own byte lists are written out at their sites
   instead, because they are NOT this set: the attribute grammar's separator set includes 0x2F (/) and its
   value terminators include 0x3E (>), and folding either into "whitespace" is how a grammar acquires a rule
   the standard never gave it. */
static bool sniff_ascii_ws(unsigned char c)
{
    return c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
}

/* ---- HTML §2.5.3 "Extracting character encodings from meta elements" ---------------------------------------
 *
 * "The algorithm for extracting a character encoding from a meta element, given a string s, is as follows. It
 * returns either a character encoding or nothing." */
int html_extract_encoding_from_meta(const char *s, size_t n)
{
    static const char CHARSET[] = "charset";
    size_t pos = 0;

    DCHECK(s != NULL || n == 0, "§2.5.3 was given a null string with a nonzero length");
    /* "Let position be a pointer into s, initially pointing at the start of the string." */
    for (;;) {
        size_t i, start, end;

        /* "Loop: Find the first seven characters in s after position that are an ASCII case-insensitive match
           for the word `charset`. If no such match is found, return nothing." */
        for (i = pos; i + 7 <= n; i++) {
            size_t k;
            for (k = 0; k < 7; k++) {
                unsigned char c = (unsigned char)s[i + k];
                if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
                if (c != (unsigned char)CHARSET[k]) break;
            }
            if (k == 7) break;
        }
        if (i + 7 > n) return -1;
        pos = i + 7;
        /* "Skip any ASCII whitespace that immediately follow the word `charset` (there might not be any)." */
        while (pos < n && sniff_ascii_ws((unsigned char)s[pos])) pos++;
        /* "If the next character is not a U+003D EQUALS SIGN (=), then move position to point just before that
           next character, and jump back to the step labeled loop." A string that ENDS here has no next
           character to point before; the jump-back then finds no further match and the loop returns nothing,
           which is the same answer reached one iteration later. `pos` is strictly greater than the `i` this
           iteration matched at, so the search always advances and the loop always terminates. */
        if (pos >= n || s[pos] != '=') continue;
        pos++;
        /* "Skip any ASCII whitespace that immediately follow the equals sign (there might not be any)." */
        while (pos < n && sniff_ascii_ws((unsigned char)s[pos])) pos++;
        /* "Process the next character as follows." "If there is no next character: return nothing." */
        if (pos >= n) return -1;
        if (s[pos] == '"' || s[pos] == '\'') {
            /* "If it is a U+0022 QUOTATION MARK character (") and there is a later U+0022 QUOTATION MARK
               character (") in s … Return the result of getting an encoding from the substring that is between
               this character and the next earliest occurrence of this character."
               "If it is an unmatched U+0022 QUOTATION MARK character (") … Return nothing." */
            const char *close = memchr(s + pos + 1, s[pos], n - pos - 1);
            if (!close) return -1;
            start = pos + 1;
            end = (size_t)(close - s);
            return encoding_lookup(s + start, end - start);
        }
        /* "Otherwise: Return the result of getting an encoding from the substring that consists of this
           character up to but not including the first ASCII whitespace or U+003B SEMICOLON character (;), or
           the end of s, whichever comes first." */
        start = pos;
        end = pos;
        while (end < n && !sniff_ascii_ws((unsigned char)s[end]) && s[end] != ';') end++;
        return encoding_lookup(s + start, end - start);
    }
}

/* ---- §13.2.3.2's PRESCAN: the collected attribute -----------------------------------------------------------
 *
 * "get an attribute" builds two strings a byte at a time and has no length limit of its own — a `content`
 * attribute is a whole `Content-Type` value and a page may write one of any length — so the collector GROWS.
 * A fixed buffer here would not be a bound on work, it would be a SILENT WRONG ANSWER: a truncated `content`
 * value fed to §2.5.3 finds a different encoding, or none, from the one the page declared. */
typedef struct { char *p; size_t n, cap; } SniffBuf;

static void sniff_buf_push(SniffBuf *b, char c)
{
    if (b->n == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 32;
        char *q = realloc(b->p, cap);
        CHECK(q != NULL, "html_encoding_sniff: OOM collecting a prescanned attribute");
        b->p = q;
        b->cap = cap;
    }
    b->p[b->n++] = c;
}

/* "Let attribute list be an empty list of strings … If the attribute's name is already in attribute list, then
   return to the step labeled attributes." The list is per-`<meta>`, so it is built and dropped inside the one
   arm below. It holds COPIES because the name buffer is reused for the next attribute. */
typedef struct { char **s; size_t *len; size_t n, cap; } SniffNames;

static bool sniff_names_has(const SniffNames *l, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < l->n; i++)
        if (l->len[i] == n && (n == 0 || memcmp(l->s[i], s, n) == 0)) return true;
    return false;
}

static void sniff_names_add(SniffNames *l, const char *s, size_t n)
{
    char *copy;

    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 8;
        char **ns = realloc(l->s, cap * sizeof *ns);
        size_t *nl;
        CHECK(ns != NULL, "html_encoding_sniff: OOM recording a prescanned attribute name");
        l->s = ns;
        nl = realloc(l->len, cap * sizeof *nl);
        CHECK(nl != NULL, "html_encoding_sniff: OOM recording a prescanned attribute name length");
        l->len = nl;
        l->cap = cap;
    }
    copy = malloc(n ? n : 1);
    CHECK(copy != NULL, "html_encoding_sniff: OOM copying a prescanned attribute name");
    if (n) memcpy(copy, s, n);
    l->s[l->n] = copy;
    l->len[l->n] = n;
    l->n++;
}

static void sniff_names_free(SniffNames *l)
{
    size_t i;
    for (i = 0; i < l->n; i++) free(l->s[i]);
    free(l->s);
    free(l->len);
    l->s = NULL;
    l->len = NULL;
    l->n = l->cap = 0;
}

/* §13.2.3.2's "get an attribute", verbatim, over `p[*pos .. n)`.
 *   1  — an attribute was read; `name` and `value` hold it, ASCII-lowercased as the algorithm's own steps
 *        lowercase them ("this converts the input to lowercase").
 *   0  — "abort the get an attribute algorithm. There isn't one." — position is left ON the 0x3E, which is
 *        what lets the caller's "next byte" step step over it.
 *  -1  — the position pointer went beyond the end of the byte stream, which aborts the whole PRESCAN. It is a
 *        separate answer from 0 because they lead to different places: 0 continues the scan at the next byte,
 *        -1 ends it and hands the bytes to "get an XML encoding". */
static int sniff_get_attribute(const char *p, size_t n, size_t *pos, SniffBuf *name, SniffBuf *value)
{
    size_t i = *pos;
    unsigned char c;

    name->n = 0;
    value->n = 0;
    /* "If the byte at position is one of 0x09 (HT), 0x0A (LF), 0x0C (FF), 0x0D (CR), 0x20 (SP), or 0x2F (/),
       then advance position to the next byte and redo this step." 0x2F IS IN THIS SET AND NOT IN INFRA'S ASCII
       whitespace — `<meta/charset=utf-8>` and `<meta charset=utf-8/>` are both real markup. */
    for (;;) {
        if (i >= n) { *pos = i; return -1; }
        c = (unsigned char)p[i];
        if (!(sniff_ascii_ws(c) || c == 0x2F)) break;
        i++;
    }
    /* "If the byte at position is 0x3E (>), then abort the get an attribute algorithm. There isn't one." */
    if (c == 0x3E) { *pos = i; return 0; }
    /* "Otherwise, the byte at position is the start of the attribute name. Let attribute name and attribute
       value be the empty string. Process the byte at position as follows:" */
    for (;;) {
        if (i >= n) { *pos = i; return -1; }
        c = (unsigned char)p[i];
        /* "If it is 0x3D (=), and the attribute name is longer than the empty string: advance position to the
           next byte and jump to the step below labeled value." */
        if (c == 0x3D && name->n > 0) { i++; goto value_step; }
        /* "If it is 0x09/0x0A/0x0C/0x0D/0x20: jump to the step below labeled spaces." */
        if (sniff_ascii_ws(c)) goto spaces_step;
        /* "If it is 0x2F (/) or 0x3E (>): abort … The attribute's name is the value of attribute name, its
           value is the empty string." */
        if (c == 0x2F || c == 0x3E) { *pos = i; return 1; }
        /* "If it is in the range 0x41 (A) to 0x5A (Z): append the code point b+0x20 to attribute name (this
           converts the input to lowercase). Anything else: append the code point with the same value as the
           byte at position to attribute name." */
        sniff_buf_push(name, (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
        i++;
    }

spaces_step:
    /* "Spaces: If the byte at position is one of 0x09/0x0A/0x0C/0x0D/0x20, then advance position to the next
       byte, then, repeat this step." */
    for (;;) {
        if (i >= n) { *pos = i; return -1; }
        if (!sniff_ascii_ws((unsigned char)p[i])) break;
        i++;
    }
    /* "If the byte at position is not 0x3D (=), abort … The attribute's name is the value of attribute name,
       its value is the empty string." */
    if ((unsigned char)p[i] != 0x3D) { *pos = i; return 1; }
    /* "Advance position past the 0x3D (=) byte." */
    i++;

value_step:
    /* "Value: If the byte at position is one of 0x09/0x0A/0x0C/0x0D/0x20, then advance position to the next
       byte, then, repeat this step." */
    for (;;) {
        if (i >= n) { *pos = i; return -1; }
        if (!sniff_ascii_ws((unsigned char)p[i])) break;
        i++;
    }
    c = (unsigned char)p[i];
    if (c == 0x22 || c == 0x27) {
        /* "If it is 0x22 (\") or 0x27 ('): Let b be the value of the byte at position. Quote loop: Advance
           position to the next byte. If the value of the byte at position is the value of b, then advance
           position to the next byte and abort the algorithm … Otherwise, if the value of the byte at position
           is in the range 0x41 (A) to 0x5A (Z), then append a code point to attribute value whose value is
           0x20 more than the value of the byte at position. Otherwise, append a code point to attribute value
           whose value is the same … Return to the step above labeled quote loop." */
        unsigned char b = c;
        for (;;) {
            i++;
            if (i >= n) { *pos = i; return -1; }
            c = (unsigned char)p[i];
            if (c == b) { *pos = i + 1; return 1; }
            sniff_buf_push(value, (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
        }
    }
    /* "If it is 0x3E (>): abort … The attribute's name is the value of attribute name, its value is the empty
       string." */
    if (c == 0x3E) { *pos = i; return 1; }
    /* "If it is in the range 0x41 (A) to 0x5A (Z): append a code point b+0x20 to attribute value. Advance
       position to the next byte. Anything else: append a code point with the same value … Advance position to
       the next byte." */
    sniff_buf_push(value, (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    i++;
    /* "Process the byte at position as follows: If it is 0x09/0x0A/0x0C/0x0D/0x20 or 0x3E (>): abort … The
       attribute's name is the value of attribute name and its value is the value of attribute value. …
       Advance position to the next byte and return to the previous step." */
    for (;;) {
        if (i >= n) { *pos = i; return -1; }
        c = (unsigned char)p[i];
        if (sniff_ascii_ws(c) || c == 0x3E) { *pos = i; return 1; }
        sniff_buf_push(value, (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
        i++;
    }
}

/* §13.2.3.2's "get an XML encoding": "When the prescan a byte stream to determine its encoding algorithm is
 * aborted without returning an encoding, get an XML encoding means doing this." The standard's own note gives
 * the reason it exists at all: "looking for syntax resembling an XML declaration, even in text/html, is
 * necessary for compatibility with existing content."
 *
 * `xmlDeclarationEnd` IS COMPUTED AND THEN NOT USED, which is the standard's text and not an omission here:
 * step 2 requires that a 0x3E exist after the `<?xml` and every later step searches from `encodingPosition`
 * without an upper bound. Narrowing the search to the declaration would be a different algorithm, and
 * §13.2.3.2's closing note asks in as many words that it not be one ("user agents should not use a pre-scan
 * algorithm that returns different results than the one described above"). */
static int sniff_get_xml_encoding(const char *p, size_t n)
{
    static const char XMLDECL[] = "<?xml";
    static const char ENCODING[] = "encoding";
    size_t i, end;
    unsigned char quote;

    /* "Let encodingPosition be a pointer to the start of the stream. If encodingPosition does not point to the
       start of a byte sequence 0x3C, 0x3F, 0x78, 0x6D, 0x6C (`<?xml`), then return failure." */
    if (n < sizeof XMLDECL - 1 || memcmp(p, XMLDECL, sizeof XMLDECL - 1) != 0) return -1;
    /* "Let xmlDeclarationEnd be a pointer to the next byte in the input byte stream which is 0x3E (>). If there
       is no such byte, then return failure." */
    if (!memchr(p, 0x3E, n)) return -1;
    /* "Set encodingPosition to the position of the first occurrence of the subsequence of bytes `encoding` at
       or after the current encodingPosition. If there is no such sequence, then return failure. Advance
       encodingPosition past the 0x67 (g) byte." */
    {
        const char *hit = NULL;
        if (n >= sizeof ENCODING - 1) hit = memmem(p, n, ENCODING, sizeof ENCODING - 1);
        if (!hit) return -1;
        i = (size_t)(hit - p) + (sizeof ENCODING - 1);
    }
    /* "While the byte at encodingPosition is less than or equal to 0x20 (i.e., it is either an ASCII space or
       control character), advance encodingPosition to the next byte." — NOT Infra's ASCII whitespace, which is
       why sniff_ascii_ws is not used here. */
    while (i < n && (unsigned char)p[i] <= 0x20) i++;
    /* "If the byte at encodingPosition is not 0x3D (=), then return failure. Advance encodingPosition to the
       next byte." */
    if (i >= n || (unsigned char)p[i] != 0x3D) return -1;
    i++;
    while (i < n && (unsigned char)p[i] <= 0x20) i++;
    /* "Let quoteMark be the byte at encodingPosition. If quoteMark is not either 0x22 (\") or 0x27 ('), then
       return failure. Advance encodingPosition to the next byte." */
    if (i >= n) return -1;
    quote = (unsigned char)p[i];
    if (quote != 0x22 && quote != 0x27) return -1;
    i++;
    /* "Let encodingEndPosition be the position of the next occurrence of quoteMark at or after
       encodingPosition. If quoteMark does not occur again, then return failure." */
    {
        const char *close = i < n ? memchr(p + i, (int)quote, n - i) : NULL;
        if (!close) return -1;
        end = (size_t)(close - p);
    }
    /* "Let potentialEncoding be the sequence of the bytes between encodingPosition (inclusive) and
       encodingEndPosition (exclusive). If potentialEncoding contains one or more bytes whose byte value is
       0x20 or below, then return failure." */
    {
        size_t k;
        for (k = i; k < end; k++)
            if ((unsigned char)p[k] <= 0x20) return -1;
    }
    /* "Let encoding be the result of getting an encoding given potentialEncoding isomorphic decoded."
       ISOMORPHIC DECODE IS THE IDENTITY FOR THIS COMPARISON AND NOT A STEP SKIPPED: it maps each byte to the
       code point of the same value, §4.2's label table is entirely ASCII, and `get an encoding` compares
       ASCII-lowercased code points — so a byte above 0x7F cannot match a label whether it is decoded or not,
       and every byte that can is its own code point. */
    {
        int enc = encoding_lookup(p + i, end - i);
        /* "If the encoding is UTF-16BE/LE, then change it to UTF-8. Return encoding." §14.2 Common
           infrastructure for UTF-16BE/LE: "UTF-16BE/LE is UTF-16BE or UTF-16LE", so the one name is two ids. */
        if (enc == encoding_lookup("utf-16be", 8) || enc == encoding_lookup("utf-16le", 8))
            return encoding_utf8();
        return enc;
    }
}

/* §13.2.3.2's "prescan a byte stream to determine its encoding". */
int html_prescan_byte_stream(const char *p, size_t n)
{
    SniffBuf name = { NULL, 0, 0 }, value = { NULL, 0, 0 };
    size_t pos;
    int result = -1;
    bool aborted = false;

    DCHECK(p != NULL || n == 0, "§13.2.3.2's prescan was given a null byte stream with a nonzero length");
    /* "Prescan for UTF-16 XML declarations: If position points to a sequence of bytes starting with 0x3C, 0x0,
       0x3F, 0x0, 0x78, 0x0 (case-sensitive UTF-16 little-endian `<?x`) return UTF-16LE. A sequence of bytes
       starting with 0x0, 0x3C, 0x0, 0x3F, 0x0, 0x78 (case-sensitive UTF-16 big-endian `<?x`) return UTF-16BE."
       The standard's own note on why the prefix is not the one XML Appendix F uses: "for historical reasons,
       the prefix is two bytes longer than in Appendix F of XML and the encoding name is not checked." */
    if (n >= 6 && !memcmp(p, "\x3C\x00\x3F\x00\x78\x00", 6)) return encoding_lookup("utf-16le", 8);
    if (n >= 6 && !memcmp(p, "\x00\x3C\x00\x3F\x00\x78", 6)) return encoding_lookup("utf-16be", 8);

    for (pos = 0; ; ) {
        /* The abort condition, tested where the algorithm states it: "the position pointer … goes beyond the
           end of the byte stream obtained so far". */
        if (pos >= n) { aborted = true; break; }
        if (n - pos >= 4 && !memcmp(p + pos, "<!--", 4)) {
            /* "Advance the position pointer so that it points at the first 0x3E byte which is preceded by two
               0x2D bytes (i.e. at the end of an ASCII `-->` sequence) and comes after the 0x3C byte that was
               found. (The two 0x2D bytes can be the same as those in the `<!--` sequence.)" The earliest byte
               that can satisfy all three conditions is pos+4 — `<!-->` is a complete comment — so the search
               starts there rather than at pos+1, which would examine bytes that cannot match. */
            size_t k;
            for (k = pos + 4; k < n; k++)
                if ((unsigned char)p[k] == 0x3E && (unsigned char)p[k - 1] == 0x2D &&
                    (unsigned char)p[k - 2] == 0x2D) break;
            if (k >= n) { aborted = true; break; }
            pos = k;
        } else if (n - pos >= 6 && (unsigned char)p[pos] == 0x3C &&
                   (p[pos + 1] == 'M' || p[pos + 1] == 'm') && (p[pos + 2] == 'E' || p[pos + 2] == 'e') &&
                   (p[pos + 3] == 'T' || p[pos + 3] == 't') && (p[pos + 4] == 'A' || p[pos + 4] == 'a') &&
                   (sniff_ascii_ws((unsigned char)p[pos + 5]) || (unsigned char)p[pos + 5] == 0x2F)) {
            /* "A sequence of bytes starting with: 0x3C, 0x4D or 0x6D, 0x45 or 0x65, 0x54 or 0x74, 0x41 or
               0x61, and one of 0x09, 0x0A, 0x0C, 0x0D, 0x20, 0x2F (case-insensitive ASCII `<meta` followed by
               a space or slash). Advance the position pointer so that it points at the next 0x09, 0x0A, 0x0C,
               0x0D, 0x20, or 0x2F byte (the one in sequence of characters matched above)." */
            SniffNames seen = { NULL, NULL, 0, 0 };
            /* "Let got pragma be false. Let need pragma be null. Let charset be the null value (which, for the
               purposes of this algorithm, is distinct from an unrecognized encoding or the empty string)."
               THREE STATES FOR `charset` AND THREE FOR `need pragma`, because the algorithm branches on all
               three: a NULL charset is "no declaration was seen", a FAILURE is "a declaration was seen and
               named a label the registry does not know", and an id is an answer. Collapsing null and failure
               would make `<meta charset=nonsense>` end the scan with nothing instead of continuing it. */
            bool got_pragma = false;
            int need_pragma = -1;     /* -1 null, 0 false, 1 true */
            int charset = -2;         /* -2 null, -1 failure, else an encoding id */
            int attr;

            pos += 5;
            for (;;) {
                /* "Attributes: Get an attribute and its value. If no attribute was sniffed, then jump to the
                   processing step below." */
                attr = sniff_get_attribute(p, n, &pos, &name, &value);
                if (attr < 0) { aborted = true; break; }
                if (attr == 0) break;
                /* "If the attribute's name is already in attribute list, then return to the step labeled
                   attributes. Add the attribute's name to attribute list." */
                if (sniff_names_has(&seen, name.p, name.n)) continue;
                sniff_names_add(&seen, name.p, name.n);
                /* "If the attribute's name is `http-equiv`: If the attribute's value is `content-type`, then
                   set got pragma to true." Both are already ASCII-lowercased by `get an attribute`. */
                if (name.n == 10 && !memcmp(name.p, "http-equiv", 10)) {
                    if (value.n == 12 && !memcmp(value.p, "content-type", 12)) got_pragma = true;
                } else if (name.n == 7 && !memcmp(name.p, "content", 7)) {
                    /* "If the attribute's name is `content`: Apply the algorithm for extracting a character
                       encoding from a meta element, giving the attribute's value as the string to parse. If a
                       character encoding is returned, and if charset is still set to null, let charset be the
                       encoding returned, and set need pragma to true." */
                    int e = html_extract_encoding_from_meta(value.p, value.n);
                    if (e >= 0 && charset == -2) { charset = e; need_pragma = 1; }
                } else if (name.n == 7 && !memcmp(name.p, "charset", 7)) {
                    /* "If the attribute's name is `charset`: Let charset be the result of getting an encoding
                       from the attribute's value, and set need pragma to false." UNCONDITIONALLY — this arm
                       overwrites an answer a `content` attribute already produced, which is why the `charset`
                       attribute wins on `<meta http-equiv=content-type content="text/html;charset=big5"
                       charset=shift_jis>` no matter which came first. */
                    charset = encoding_lookup(value.p, value.n);
                    need_pragma = 0;
                }
            }
            sniff_names_free(&seen);
            if (aborted) break;
            /* "Processing: If need pragma is null, then jump to the step below labeled next byte. If need
               pragma is true but got pragma is false, then jump to the step below labeled next byte. If
               charset is failure, then jump to the step below labeled next byte." */
            if (need_pragma >= 0 && !(need_pragma == 1 && !got_pragma) && charset >= 0) {
                /* "If charset is UTF-16BE/LE, then set charset to UTF-8. If charset is x-user-defined, then
                   set charset to windows-1252. Return charset." Neither can be the encoding a document is
                   PARSED in — §13.2.3.4 states the first half from the other end ("if the new encoding is
                   UTF-16BE/LE, then change it to UTF-8") — which is why the WPT normalization test lists
                   `utf-16`, `utf-16le` and `utf-16be` as labels of the NAME `UTF-8`. */
                if (charset == encoding_lookup("utf-16be", 8) || charset == encoding_lookup("utf-16le", 8))
                    charset = encoding_utf8();
                else if (charset == encoding_lookup("x-user-defined", 14))
                    charset = encoding_lookup("windows-1252", 12);
                result = charset;
                break;
            }
            /* Otherwise fall through to "next byte" — `pos` is wherever the attribute scan left it. */
        } else if (n - pos >= 2 && (unsigned char)p[pos] == 0x3C &&
                   (((unsigned char)p[pos + 1] >= 'A' && (unsigned char)p[pos + 1] <= 'Z') ||
                    ((unsigned char)p[pos + 1] >= 'a' && (unsigned char)p[pos + 1] <= 'z') ||
                    (n - pos >= 3 && (unsigned char)p[pos + 1] == 0x2F &&
                     (((unsigned char)p[pos + 2] >= 'A' && (unsigned char)p[pos + 2] <= 'Z') ||
                      ((unsigned char)p[pos + 2] >= 'a' && (unsigned char)p[pos + 2] <= 'z'))))) {
            /* "A sequence of bytes starting with a 0x3C byte (<), optionally a 0x2F byte (/), and finally a
               byte in the range 0x41-0x5A or 0x61-0x7A (A-Z or a-z): Advance the position pointer so that it
               points at the next 0x09 (HT), 0x0A (LF), 0x0C (FF), 0x0D (CR), 0x20 (SP), or 0x3E (>) byte.
               Repeatedly get an attribute until no further attributes can be found, then jump to the step
               below labeled next byte." THIS IS WHAT MAKES THE SCAN A SCAN RATHER THAN A SEARCH: every other
               tag's attributes are consumed by the same grammar, so a `<div title="<meta charset=big5>">` is
               one attribute value and not a declaration. */
            size_t k = pos + 1;
            while (k < n && !sniff_ascii_ws((unsigned char)p[k]) && (unsigned char)p[k] != 0x3E) k++;
            if (k >= n) { aborted = true; break; }
            pos = k;
            for (;;) {
                int a = sniff_get_attribute(p, n, &pos, &name, &value);
                if (a < 0) { aborted = true; break; }
                if (a == 0) break;
            }
            if (aborted) break;
        } else if (n - pos >= 2 && (unsigned char)p[pos] == 0x3C &&
                   ((unsigned char)p[pos + 1] == 0x21 || (unsigned char)p[pos + 1] == 0x2F ||
                    (unsigned char)p[pos + 1] == 0x3F)) {
            /* "A sequence of bytes starting with: 0x3C 0x21 (`<!`) / 0x3C 0x2F (`</`) / 0x3C 0x3F (`<?`):
               Advance the position pointer so that it points at the first 0x3E byte (>) that comes after the
               0x3C byte that was found." The `</` case is reached only for a `</` NOT followed by an ASCII
               letter, because the end-tag arm above claims the rest — the order of these arms is the order the
               standard writes them and is not interchangeable. */
            const char *gt = memchr(p + pos + 1, 0x3E, n - pos - 1);
            if (!gt) { aborted = true; break; }
            pos = (size_t)(gt - p);
        }
        /* "Any other byte: Do nothing with that byte."
           "Next byte: Move position so it points at the next byte in the input byte stream, and return to the
           step above labeled loop." */
        pos++;
    }
    free(name.p);
    free(value.p);
    /* "If the algorithm … is aborted … then abort the prescan a byte stream to determine its encoding algorithm
       and return the result of get an XML encoding applied to the same bytes that the prescan … was applied
       to. Otherwise, these steps will return a character encoding." */
    return aborted ? sniff_get_xml_encoding(p, n) : result;
}

/* §13.2.3.2's ENCODING SNIFFING ALGORITHM, in its own step order. Every step that this engine cannot take is
 * one the standard writes as OPTIONAL, and each says so at its site — none of them is a hole this file fills
 * with a plausible value. */
int html_encoding_sniff(const char *bytes, size_t n, const char *content_type_value, int parent_encoding)
{
    /* "The authoring conformance requirements for character encoding declarations limit them to only appearing
       in the first 1024 bytes. User agents are therefore encouraged to use the prescan algorithm below (as
       invoked by these steps) on the first 1024 bytes, but not to stall beyond that." The window is the
       ALGORITHM'S own end condition, stated by the standard as an authoring conformance limit — it bounds how
       far a DECLARATION may be, never how much work the engine may do, so it is not the kind of cap CLAUDE.md
       forbids: scanning further would find declarations no conforming document may contain and that no other
       browser honours. */
    const size_t PRESCAN_WINDOW = 1024;
    int enc;

    DCHECK(bytes != NULL || n == 0, "§13.2.3.2 was given a null byte stream with a nonzero length");
    DCHECK(parent_encoding >= -1,
           "§13.2.3.2's container-document step was handed neither an encoding nor its absence — -1 is the "
           "step's \"there is no same-origin container document\", and any other negative value is a caller "
           "passing on a raw `get an encoding` failure it never resolved");

    /* Step 1: "If the result of BOM sniffing is an encoding, return that encoding with confidence certain."
       The standard's own note on why this runs even though the decode would honour the BOM anyway: "although
       the decode algorithm will itself change the encoding to use based on the presence of a byte order mark,
       this algorithm sniffs the BOM as well in order to set the correct document's character encoding and
       confidence." Encoding §6.1 Legacy hooks for standards owns "BOM sniff"; it is asked rather than
       re-implemented, so this and the decode that follows can never disagree about what a BOM is. */
    enc = encoding_bom_sniff(bytes, n);
    if (enc >= 0) return enc;

    /* Step 2 — "If the user has explicitly instructed the user agent to override the document's character
       encoding with a specific encoding, OPTIONALLY return that encoding with the confidence certain." There
       is no user in a headless engine to have instructed anything, and the step is optional, so there is
       nothing here to build rather than something omitted.
       Step 3 is the standard's permission to WAIT for more bytes ("the user agent may wait for more bytes of
       the resource to be available"), which a caller that already holds the whole body has no use for. */

    /* Step 4: "If the transport layer specifies a character encoding, and it is supported, return that
       encoding with the confidence certain." Fetch §3.5's "legacy extract an encoding" is that step's two
       halves in one algorithm — it answers the FALLBACK both when there is no charset parameter and when the
       one there is names a label §4.2 cannot resolve, which is exactly "specifies … and it is supported" — so
       -1 as the fallback makes "no" a distinguishable answer without a second parse of the header. */
    {
        MimeType m;
        bool ok = mime_type_extract(&m, content_type_value);
        enc = mime_type_legacy_extract_encoding(ok ? &m : NULL, -1);
        mime_type_free(&m);
        if (enc >= 0) return enc;
    }

    /* Step 5: "Optionally, prescan the byte stream to determine its encoding, with the end condition being
       when the user agent decides that scanning further bytes would not be efficient … The aforementioned
       algorithm returns either a character encoding or failure. If it returns a character encoding, then
       return the same encoding, with confidence tentative." */
    enc = html_prescan_byte_stream(bytes, n < PRESCAN_WINDOW ? n : PRESCAN_WINDOW);
    if (enc >= 0) return enc;

    /* Step 6: "If the HTML parser for which this algorithm is being run is associated with a Document d whose
       container document is non-null: Let parentDocument be d's container document. If parentDocument's origin
       is same origin with d's origin and parentDocument's character encoding is not UTF-16BE/LE, then return
       parentDocument's character encoding, with the confidence tentative." -1 IS "there is no same-origin
       container document" and not "we did not look" — see the header for why that half is the caller's and
       this half is not. §14.2 Common infrastructure for UTF-16BE/LE: "UTF-16BE/LE is UTF-16BE or UTF-16LE", so
       the exclusion is two ids; a document cannot be parsed in either, which is why inheriting one would be
       worse than inheriting nothing. */
    if (parent_encoding >= 0 && parent_encoding != encoding_lookup("utf-16be", 8) &&
        parent_encoding != encoding_lookup("utf-16le", 8))
        return parent_encoding;

    /* Step 7 — "Otherwise, if the user agent has information on the likely encoding for this page, e.g. based
       on the encoding of the page when it was last visited …" — is about a HISTORY of visits, which is state
       this engine's one continuous frontier does not keep per origin-and-encoding; and step 8's autodetection
       is a "may" the standard then spends a paragraph discouraging ("user agents are generally discouraged
       from attempting to autodetect encodings for resources obtained over the network, since doing so involves
       inherently non-interoperable heuristics"). Both are optional and both are skipped, which is a different
       thing from being unimplemented: taking them would make this engine LESS interoperable, not more. */

    /* Step 9: "Otherwise, return an implementation-defined or user-specified default character encoding, with
       the confidence tentative." The standard's own table of suggested defaults by locale ends "All other
       locales: windows-1252", and that is the answer here — not because a locale was consulted, but because
       this engine has none and windows-1252 is the row for every locale it is not.
       IT IS NOT UTF-8, AND THE COST OF GETTING THIS WRONG IS VISIBLE IN THE CORPUS RATHER THAN ARGUABLE:
       wpt encoding/sniffing.html is titled "No (UTF-8) sniffing allowed" and asserts BOTH
       `document.characterSet === "windows-1252"` AND that a UTF-8-encoded `€€` in the document's own markup
       reads back as `Â€Â€` — the mojibake that IS windows-1252 applied to UTF-8 bytes. A
       default of UTF-8 would answer both of those wrongly while looking, from inside, exactly like a document
       that had been decoded correctly. */
    return encoding_lookup("windows-1252", 12);
}
