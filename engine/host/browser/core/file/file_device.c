/* THE MOCK FILE DEVICE — the storage an `<input type=file>`'s picker chooses from.
 *
 * WHY IT EXISTS. HTML §4.10.5.1.17 gives a file control a LIST OF SELECTED FILES and says how it is filled:
 * the user is shown a prompt, picks files "from the filesystem or created on the fly", and the user agent then
 * updates the file selection. Everything in that sentence is defined behaviour EXCEPT the physical device, and
 * a missing device is not a missing algorithm — it is the same gap `storage.c` fills for a disk and the File
 * System Access spec fills with a mock filesystem. So the device is modelled: a list of files with names,
 * types and bytes, put there through one edge, and a SELECT that answers a control's prompt by the rules the
 * section states (`accept` filters, `multiple` bounds).
 *
 * WHAT IS HONESTLY ABSENT: nothing puts a file on the device until something does. That is a device with no
 * files on it, which is a different statement from "this engine has no such thing" — the state exists, the
 * filter over it exists, and a caller that adds one file makes every path a bundle takes over a chosen file
 * reachable.
 *
 * IT HOLDS BYTES, NEVER JSValues. A device is BASELINE state shared by every flow, so a JS object held here
 * would be one realm's object answering another realm's read and one flow's object surviving another flow's
 * rewind. The Files are minted per selection, in the realm doing the selecting, and THOSE time-travel. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/file/blob.h"
#include "core/file/file_device.h"
#include "core/file/file_list.h"
#include "core/mime/mime_type.h"

typedef struct {
    char   *name;    /* the file name, path components already stripped */
    char   *type;    /* the MIME type the storage records, "" for none */
    char   *bytes;   /* the file body; NULL only when `len` is 0 */
    size_t  len;
    int64_t last_modified;
} DeviceFile;

static DeviceFile *g_dev;
static int         g_dev_n, g_dev_cap;

static char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static bool ascii_ci_eq(const char *a, size_t alen, const char *b, size_t blen)
{
    size_t i;

    if (alen != blen) return false;
    for (i = 0; i < alen; i++)
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    return true;
}

static char *dup_len(const char *s, size_t n)
{
    char *p = malloc(n + 1);

    CHECK(p != NULL, "file-device: OOM copying a file's bytes");
    if (n) memcpy(p, s, n);
    p[n] = 0;
    return p;
}

void file_device_add(const char *name, const char *type, const char *bytes, size_t len, int64_t last_modified)
{
    const char *base;
    size_t nlen;

    DCHECK(name != NULL, "a file was put on the device with no name — §4.10.5.1.17's file is a filename, a "
                         "file type and a file body, and `files[0].name` has to answer with something");
    /* §4.10.5.1.17: "Filenames must not contain path components, even in the case that a user has selected an
       entire directory hierarchy ... Path components ... are those parts of filenames that are separated by
       U+005C REVERSE SOLIDUS." Stripped HERE, at the one edge a name enters through, so no reader downstream
       has to know the rule — and so §4.10.5.4's `C:\fakepath\` prefix can never be mistaken for one. */
    base = strrchr(name, '\\');
    base = base ? base + 1 : name;
    nlen = strlen(base);
    if (g_dev_n == g_dev_cap) {
        int cap = g_dev_cap ? g_dev_cap * 2 : 4;
        DeviceFile *g = realloc(g_dev, (size_t)cap * sizeof *g);

        CHECK(g != NULL, "file-device: OOM growing the device's file list");
        g_dev = g;
        g_dev_cap = cap;
    }
    g_dev[g_dev_n].name = dup_len(base, nlen);
    g_dev[g_dev_n].type = dup_len(type ? type : "", type ? strlen(type) : 0);
    g_dev[g_dev_n].bytes = dup_len(bytes ? bytes : "", len);
    g_dev[g_dev_n].len = len;
    g_dev[g_dev_n].last_modified = last_modified;
    g_dev_n++;
}

uint32_t file_device_count(void) { return (uint32_t)g_dev_n; }

/* ---- §4.10.5.1.17's `accept` TOKENS -------------------------------------------------------------------------
 *
 * "If specified, the attribute must consist of a set of comma-separated tokens, each of which must be an ASCII
 * case-insensitive match for one of the following: the string `audio/*`, the string `video/*`, the string
 * `image/*`, a valid MIME type string with no parameters, a string whose first character is U+002E." */

/* ONE TOKEN against one file. */
static bool accept_token_matches(const char *tok, size_t tlen, const char *name, const char *type)
{
    static const char *const WILD[] = { "audio", "video", "image" };
    size_t i;

    if (!tlen) return false;   /* the empty token is one of the set's possible tokens and names no file */
    /* The extension form: "a string whose first character is a U+002E FULL STOP character (.) indicates that
       files with the specified file extension are accepted" — matched against the END of the name, ASCII
       case-insensitively, which is how a file system names an extension. */
    if (tok[0] == '.') {
        size_t nlen = strlen(name);

        return nlen >= tlen && ascii_ci_eq(name + nlen - tlen, tlen, tok, tlen);
    }
    /* The three WILDCARD forms, which are named STRINGS and not a general `type/*` rule — `text/*` is not one
       of the listed tokens, so it names no file rather than accepting every text type. */
    for (i = 0; i < sizeof WILD / sizeof WILD[0]; i++) {
        size_t wlen = strlen(WILD[i]);
        char buf[16];

        if (wlen + 2 != tlen) continue;
        memcpy(buf, WILD[i], wlen);
        buf[wlen] = '/';
        buf[wlen + 1] = '*';
        if (!ascii_ci_eq(tok, tlen, buf, wlen + 2)) continue;
        {
            /* The file's own type decides it, parsed by MIME Sniffing §4.4 exactly as every other consumer of
               a type string in this engine parses one — the group is "its type is `image`", not "its string
               starts with `image/`", and a type the parser rejects is in no group at all. */
            MimeType m;
            bool hit;

            mime_type_init(&m);
            hit = mime_type_parse(&m, type, strlen(type)) && !strcmp(m.type, WILD[i]);
            mime_type_free(&m);
            return hit;
        }
    }
    /* "A valid MIME type string with NO PARAMETERS": the token must parse and must carry none, or it is not
       one of the listed forms and names no file. `image/png;charset=x` accepts nothing rather than accepting
       every PNG. */
    {
        MimeType t, f;
        bool hit = false;

        mime_type_init(&t);
        mime_type_init(&f);
        if (mime_type_parse(&t, tok, tlen) && t.nparams == 0 && mime_type_parse(&f, type, strlen(type))) {
            char *te = mime_type_essence(&t), *fe = mime_type_essence(&f);

            hit = te && fe && !strcmp(te, fe);   /* both essences are the parser's own lowercase */
            free(te);
            free(fe);
        }
        mime_type_free(&t);
        mime_type_free(&f);
        return hit;
    }
}

bool file_device_accepts(const char *accept, size_t accept_len, const char *name, const char *type)
{
    size_t start = 0, i;
    bool any_token = false;

    DCHECK(name != NULL && type != NULL, "the accept filter was asked about a file with no name or no type");
    if (!accept) return true;   /* the attribute is absent: nothing is restricted */
    /* §2.3.8's SET OF COMMA-SEPARATED TOKENS: split on U+002C, then each token is what remains once its
       leading and trailing ASCII whitespace is dropped — and "the empty string can be a token". */
    for (i = 0; i <= accept_len; i++) {
        size_t a, z;

        if (i != accept_len && accept[i] != ',') continue;
        a = start;
        z = i;
        while (a < z && (accept[a] == '\t' || accept[a] == '\n' || accept[a] == '\f' || accept[a] == '\r' ||
                         accept[a] == ' ')) a++;
        while (z > a && (accept[z - 1] == '\t' || accept[z - 1] == '\n' || accept[z - 1] == '\f' ||
                         accept[z - 1] == '\r' || accept[z - 1] == ' ')) z--;
        start = i + 1;
        if (z == a) continue;
        any_token = true;
        if (accept_token_matches(accept + a, z - a, name, type)) return true;
    }
    /* A SET WITH NO TOKEN IN IT RESTRICTS NOTHING. The requirement is stated over "these tokens", and an
       `accept=""` (or `accept=" , "`) has none for a file to fail against, so the filter is vacuous rather
       than total — which is also what Chrome does with `accept=""`, and the alternative reading would make an
       empty attribute mean "no file may ever be chosen". */
    return !any_token;
}

/* ---- the picker's answer ------------------------------------------------------------------------------------ */

JSValue file_device_select(JSContext *ctx, const char *accept, size_t accept_len, bool multiple)
{
    JSValue files = JS_NewArray(ctx);
    uint32_t n = 0;
    int i;

    CHECK(!JS_IsException(files), "file-device: a selection list could not be allocated");
    for (i = 0; i < g_dev_n; i++) {
        JSValue f;

        if (!file_device_accepts(accept, accept_len, g_dev[i].name, g_dev[i].type)) continue;
        f = file_new(ctx, g_dev[i].bytes, g_dev[i].len, g_dev[i].type, strlen(g_dev[i].type),
                     g_dev[i].name, strlen(g_dev[i].name), g_dev[i].last_modified);
        CHECK(!JS_IsException(f), "file-device: a selected File could not be allocated");
        /* DEFINED, not assigned — idl_slots.h's sibling rule: an engine-built list is creating OWN properties,
           and `Object.defineProperty(Array.prototype, "0", {set(){}})` swallows an assignment outright. */
        JS_DefinePropertyValueUint32(ctx, files, n++, f, JS_PROP_C_W_E);
        /* §4.10.5.1.17's own bound on the list, enforced where the SELECTION is made — it is a requirement on
           what the user agent may let be selected, not a check to run over a list a page hands over. */
        if (!multiple) break;
    }
    DCHECK(multiple || n <= 1, "the device selected more than one file for a control with no `multiple` — "
                               "§4.10.5.1.17 allows no more than one file in that control's list");
    return file_list_new(ctx, files);
}

void file_device_free(void)
{
    int i;

    for (i = 0; i < g_dev_n; i++) {
        free(g_dev[i].name);
        free(g_dev[i].type);
        free(g_dev[i].bytes);
    }
    free(g_dev);
    g_dev = NULL;
    g_dev_n = g_dev_cap = 0;
}
