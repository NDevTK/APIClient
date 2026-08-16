/* Asset-or-API classification — see resource_kind.h for whose judgement this is and what it replaced. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/mime/mime_type.h"   /* §4.2's essence and §4.6's groups — the vocabulary every rule is stated in */
#include "network/json_sniff.h"
#include "network/mime_sniff.h"
#include "network/resource_kind.h"

static void decide(ResourceKind *out, bool asset, const char *reason)
{
    int n;

    DCHECK(reason != NULL && *reason,
           "a resource-kind verdict was built with no reason — every rule below names itself, and the name is "
           "the whole of what crosses back, so an unnamed one is a rule that decided and cannot be read");
    out->asset = asset;
    n = snprintf(out->reason, sizeof out->reason, "%s", reason);
    DCHECK(n > 0 && (size_t)n < sizeof out->reason,
           "a resource-kind reason did not fit its field — every one is a literal spelled in this file or a "
           "§4.2 essence out of mime_sniff.c's own tables, so a truncation is a rule name that grew without "
           "this bound growing and would be reported as a DIFFERENT rule with the same prefix");
}

/* THE SNIFFED REASON, which is the one place a rule name is not a literal: `sniffed-` and then the essence the
   standard's own table matched. The essence is a static string from mime_sniff.c, so it holds the property
   main.c asserts about every field it writes into JSON — §4.4 restricts a type and a subtype to HTTP token
   code points, which contain no quote, no solidus-escape and no control character. */
static void decide_sniffed(ResourceKind *out, const char *essence)
{
    char buf[RESOURCE_KIND_REASON_MAX];
    int n = snprintf(buf, sizeof buf, "sniffed-%s", essence);

    DCHECK(n > 0 && (size_t)n < sizeof buf,
           "a sniffed essence did not fit a resource-kind reason — the essences are §6 and §7.1's own table "
           "entries and the longest is application/x-rar-compressed, so a truncation here is a table row this "
           "field was not sized for");
    decide(out, true, buf);
}

/* §4.6's ASSET GROUPS, together — the same five solver/reply_decode.c names, and named the same way on purpose:
   a body in one of them is BYTES a decoder turns into pixels, samples or glyphs, and there is no request shape,
   no schema and no address inside it. THE DIFFERENCE IS THE INPUT, and it is the whole reason this file exists:
   that one is a RENDERER and asks its five questions of the SUPPLIED type, because §7's answer is a computation
   it is not entitled to run; this one is the network service and asks them of the COMPUTED type. */
static const char *asset_group(const MimeType *m)
{
    if (mime_type_is_image(m))           return "image";
    if (mime_type_is_audio_or_video(m))  return "audio-or-video";
    if (mime_type_is_font(m))            return "font";
    if (mime_type_is_zip_based(m))       return "zip-based";
    if (mime_type_is_archive(m))         return "archive";
    return NULL;
}

/* THE CONFIRMATION SNIFFS, over §6.1/§6.2/§6.4's tables and §7.1's scriptable one, run even where §7 has
   already answered — which needs saying, because §7 refused to run them and was right to.
   §7 answers the SUPPLIED type unchanged whenever no rule of its own fires, so a PNG a server labels
   `application/json` computes as `application/json`: step 2 does not fire (that essence is not one of the three
   unknown ones), step 5 does not (the supplied type is not an image), and step 7 hands back what was claimed.
   That is correct for §7 — its step 1 and its refusal to upgrade exist so sniffing can never turn a resource
   INTO a scriptable type — and it is the WRONG answer to this file's question, because a body that opens with
   §6.1's PNG signature is a PNG whatever its server called it.
   THE ASYMMETRY IS WHAT MAKES IT SOUND. Every answer these tables can give is a MEDIA or DOCUMENT type, so the
   only direction this rule moves a resource is OUT of the learnable set — it can suppress a schema, it can
   never grant an execution, and it cannot manufacture an endpoint. That is the opposite of §7.2's privilege
   escalation and is why the same tables are safe here and unsafe there. */
static const char *sniff_media(const unsigned char *header, size_t header_n)
{
    const char *m;

    if ((m = mime_sniff_image_pattern(header, header_n)) != NULL)       return m;
    if ((m = mime_sniff_audio_video_pattern(header, header_n)) != NULL) return m;
    if ((m = mime_sniff_archive_pattern(header, header_n)) != NULL)     return m;
    return NULL;
}

void resource_kind_classify(ResourceKind *out, const char *content_type_value, bool no_sniff, bool opaque,
                            const unsigned char *header, size_t header_n)
{
    MimeType computed;
    char *essence;
    const char *group, *sniffed;

    DCHECK(out != NULL, "a resource was asked to be classified into nothing");
    DCHECK(header != NULL || header_n == 0,
           "a resource header of non-zero length was passed as a null pointer — a caller that has no bytes "
           "says so with a length of zero, which is a body it read and found empty");

    /* FETCH §2.2.6 FIRST, before a single byte is looked at, because there are no bytes: an opaque filtered
       response has a NULL body and an EMPTY header list, so every input below is empty by construction and
       every rule would be deciding about a resource nobody read. This is not a guess about what such a
       response usually is — the old JS's comment said "overwhelmingly tracking pixels", which is a heuristic —
       it is the observation that a body which cannot be read carries no schema. */
    if (opaque) { decide(out, true, "opaque-filtered-response"); return; }

    /* §7, and every rule below runs on WHAT THE RESOURCE IS rather than on what its header claimed. */
    mime_sniff_compute(&computed, content_type_value, no_sniff, header, header_n);
    essence = mime_type_essence(&computed);
    CHECK(essence, "resource kind: OOM reading the computed essence");

    if ((group = asset_group(&computed)) != NULL) { decide(out, true, group); goto done; }

    /* A DOCUMENT is an asset here even though it is text and even though it parses: an HTML or XML body has a
       tree, not a request shape. TWO OF §7's PATHS REACH THIS ARM and both are correct — step 1, where the
       server STATED markup and §7 refuses to be overruled by bytes, and step 2, where the type was absent or
       one of the three unknown essences and §7.1's scriptable table read the markup out of the body. A
       mislabelled document reaches neither and is caught by the last rule below instead. */
    if (mime_type_is_html(&computed) || mime_type_is_xml(&computed)) { decide(out, true, "markup"); goto done; }

    /* A SCRIPT, unless its bytes open a JSON object. Servers do ship API data under a JavaScript MIME type —
       that is what the JS this replaces was reaching for with `firstCh !== 0x7B` — and Chromium already has the
       algorithm for exactly that question, so json_sniff.c answers it for both of this program's callers.
       IT ALSO MEANS AN ARRAY BODY IS A SCRIPT, deliberately: `[1,2,3]` is a valid JavaScript program, so a
       server sending it under `text/javascript` has not mislabelled anything. */
    if (mime_type_is_javascript(&computed)) {
        if (json_sniff(header, header_n)) { decide(out, false, "javascript-mime-json-body"); goto done; }
        decide(out, true, "script");
        goto done;
    }

    /* A STYLESHEET. §4.6 has no CSS group and no standard sniffs for one, so this is the DECLARED type and
       nothing else — which is all a stylesheet ever needs, because a server that serves CSS sends `text/css`.
       It is spelled as an essence compare rather than added to mime_type.c: every group in that file is one
       §4.6 states by name, and inventing one more would put this product's judgement inside the standard's
       vocabulary where a later reader would check it against a table that does not have it. */
    if (!strcmp(essence, "text/css")) { decide(out, true, "stylesheet"); goto done; }

    if ((sniffed = sniff_media(header, header_n)) != NULL) { decide_sniffed(out, sniffed); goto done; }

    /* §7.1's scriptable table for the MISLABELLED DOCUMENT — an HTML error page served as `application/json`,
       a login page returned where a fragment was expected. `text/html` and `text/xml` are the markup rows;
       `application/pdf` is the table's own last row and is an asset by the same sentence as §6.1's images. */
    if ((sniffed = mime_sniff_scriptable_pattern(header, header_n)) != NULL) {
        if (!strcmp(sniffed, "text/html") || !strcmp(sniffed, "text/xml") ||
            !strcmp(sniffed, "application/pdf")) { decide_sniffed(out, sniffed); goto done; }
    }

    /* EVERYTHING ELSE IS LEARNED, and that is the direction to fall in. Unrecognised binary is protobuf,
       gRPC-Web or a wire format with a schema; unrecognised text is JSON, NDJSON or a framing this engine
       reads elsewhere. CLAUDE.md §Attacker sources: err toward MORE exploration — an over-learned asset costs
       one log entry, an under-learned endpoint is surface the tool exists to find and silently does not. */
    decide(out, false, "structured");

done:
    free(essence);
    mime_type_free(&computed);
}
