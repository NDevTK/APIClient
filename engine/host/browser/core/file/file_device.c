/* HTML §4.10.5.1.17's FILE CONTROL PICKER — the `accept` filter, and the selection it answers a prompt with.
 *
 * WHAT IT IS NOW, AND WHAT IT WAS. This file used to BE the device: a malloc'd array of name/type/bytes
 * triples with no directories, no writes and no per-flow isolation. That array is DELETED. The device is
 * core/file/file_system.c's LOCAL FILE SYSTEM ROOT — one virtual filesystem for this engine, shared by the file
 * control's prompt, File System Access's pickers and everything §2 of the File System Standard defines — and
 * what remains here is the part that was always this section's and nobody else's: which of the files on that
 * device a control with an `accept` attribute may offer, and how many of them a control without `multiple` may
 * take.
 *
 * WHY THAT SPLIT IS THE RIGHT ONE. §4.10.5.1.17's prompt is "the user is shown a list of files and picks from
 * it"; the LIST is the device's and the FILTER is the control's. Keeping a second store here would have meant a
 * file written through `createWritable()` being invisible to an `<input type=file>` on the same page, which is
 * not a thing a real user agent does.
 *
 * THE Files ARE MINTED PER SELECTION, in the realm doing the selecting, out of the entry the device holds — one
 * constructor (file_system_file_new), so this door and §2.3.1's `getFile()` cannot disagree about a file's type
 * or about the SOURCE its bytes carry. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/file/blob.h"
#include "core/file/file_device.h"
#include "core/file/file_system.h"
#include "core/file/file_list.h"
#include "core/mime/mime_type.h"

static char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static bool ascii_ci_eq(const char *a, size_t alen, const char *b, size_t blen)
{
    size_t i;

    if (alen != blen) return false;
    for (i = 0; i < alen; i++)
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    return true;
}

/* HOW MANY FILES THE DEVICE HOLDS — the local file system root's children, counted where they live. A
   directory among them is not a file the prompt can offer, so it is not counted: §4.10.5.1.17's list of
   selected files is a list of FILES. */
uint32_t file_device_count(JSContext *ctx)
{
    JSValue root = file_system_root_entry(ctx, FS_ROOT_LOCAL);
    int n = file_system_child_count(ctx, root), i;
    uint32_t files = 0;

    for (i = 0; i < n; i++) {
        JSValue name = JS_UNDEFINED;
        JSValue child = file_system_child_at(ctx, root, i, &name);

        if (file_system_is_file(child)) files++;
        JS_FreeValue(ctx, name);
        JS_FreeValue(ctx, child);
    }
    JS_FreeValue(ctx, root);
    return files;
}

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
    JSValue root = file_system_root_entry(ctx, FS_ROOT_LOCAL);
    int n = file_system_child_count(ctx, root), i;
    uint32_t taken = 0;

    CHECK(!JS_IsException(files), "file-device: a selection list could not be allocated");
    for (i = 0; i < n; i++) {
        JSValue name_v = JS_UNDEFINED;
        JSValue child = file_system_child_at(ctx, root, i, &name_v);
        const char *name, *type;
        bool ok;

        JS_FreeValue(ctx, name_v);
        if (!file_system_is_file(child)) { JS_FreeValue(ctx, child); continue; }
        name = file_system_name_cstr(ctx, child);
        type = file_system_type_cstr(ctx, child);
        ok = file_device_accepts(accept, accept_len, name, type);
        JS_FreeCString(ctx, name);
        JS_FreeCString(ctx, type);
        if (ok) {
            /* DEFINED, not assigned — idl_slots.h's sibling rule: an engine-built list is creating OWN
               properties, and `Object.defineProperty(Array.prototype, "0", {set(){}})` swallows an assignment
               outright. */
            JS_DefinePropertyValueUint32(ctx, files, taken++, file_system_file_new(ctx, child), JS_PROP_C_W_E);
        }
        JS_FreeValue(ctx, child);
        /* §4.10.5.1.17's own bound on the list, enforced where the SELECTION is made — it is a requirement on
           what the user agent may let be selected, not a check to run over a list a page hands over. */
        if (taken && !multiple) break;
    }
    JS_FreeValue(ctx, root);
    DCHECK(multiple || taken <= 1, "the device selected more than one file for a control with no `multiple` — "
                                   "§4.10.5.1.17 allows no more than one file in that control's list");
    return file_list_new(ctx, files);
}
