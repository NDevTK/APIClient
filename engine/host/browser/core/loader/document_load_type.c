/* HTML §7.4.5 "Populating a session history entry" — the "load a document" TYPE DECISION, both halves of it:
 * step 1's "Let type be the computed type of navigationParams's response", and the dispatch of that type onto
 * the §7.5 subsection that loads it. Nothing else.
 *
 * BOTH HALVES, BECAUSE A LOADER THAT CAN ONLY REACH THE SECOND ONE COMPUTES THE FIRST ITSELF. That is how the
 * one question below acquires a second answer, and it is how it acquired a second SILENCE: a loader that never
 * asked at all handed every response it fetched to the HTML parser.
 *
 * THE ALGORITHM, in its own words: "To load a document given navigation params navigationParams, source
 * snapshot params sourceSnapshotParams, and origin initiatorOrigin … Let type be the computed type of
 * navigationParams's response. … Otherwise, if the type is one of the following types: an HTML MIME type →
 * Return the result of loading an HTML document, given navigationParams. An XML MIME type that is not an
 * explicitly supported XML MIME type → Return the result of loading an XML document given navigationParams
 * and type. A JavaScript MIME type / a JSON MIME type that is not an explicitly supported JSON MIME type /
 * "text/css" / "text/plain" / "text/vtt" → Return the result of loading a text document given navigationParams
 * and type. …"
 *
 * WHY THIS IS A COMPONENT AND NOT AN `if` AT THE PARSE SITE. The question "which document does this response
 * load as" has exactly one right answer, it is decided by the RESPONSE and by nothing about the navigable, and
 * it is asked from every place a Document is built out of bytes. Answering it inline at one of them is how a
 * second place comes to answer it differently — and the shape that defect took here is the one CLAUDE.md
 * §Architecture describes as a plausible datum: there was no dispatch at all, every navigated response was
 * handed to the HTML parser, and an XML document therefore came back as a REAL tree with `<html>` and `<body>`
 * wrapped around its root element. Nothing crashed, nothing logged, and `documentElement.textContent` answered
 * a string one character longer than the document contains, because the newline after the root element — XML
 * §2.1's `Misc` after the document element, which is NOT part of the DOM — became a text node inside the
 * `<body>` the HTML parser synthesised (HTML §13.2.6.4.7 The "in body" insertion mode inserts a whitespace
 * character token into the current node; §13.2.6.4.20 The "after after body" insertion mode processes trailing
 * whitespace "using the rules for the 'in body' insertion mode").
 *
 * "EXPLICITLY SUPPORTED" IS A UA CONFIGURATION AND THIS ENGINE HAS NONE. §7.4.5 defines an explicitly supported
 * XML MIME type as "an XML MIME type for which the user agent is configured to use an external application to
 * render the content, or for which the user agent has dedicated processing rules" — its own example is a
 * built-in Atom feed viewer. This engine has no such configuration and no such viewer, so the "that is not an
 * explicitly supported XML MIME type" qualifier is satisfied by every XML MIME type and there is nothing here
 * to test it with. The day one is added, it is a set THIS function consults, not a condition sprinkled into
 * callers.
 */
#include <stdbool.h>
#include <string.h>

#include "core/loader/document_load_type.h"
#include "core/fetch/headers.h"
#include "core/html/media_element.h"   /* §7 steps 5/7's "supported by the user agent" — the device's own list */
#include "core/mime/mime_sniff.h"      /* §5.1's metadata and §7's sniff: WHICH type a response's bytes ARE */
#include "core/mime/mime_type.h"
#include "check.h"

/* §7.4.5's FIRST STEP, over the response — see the header. The two halves of "which document does this response
   load as" live in one file for the reason the paragraph above gives: a loader that can only reach the second
   half computes the first one itself, and then there are two answers to one question. */
void document_load_computed_type(MimeType *out, const HeaderList *response_headers,
                                 const void *body, size_t body_len)
{
    MimeType supplied;
    MimeSniffResource r;
    bool apache_bug = false, supplied_defined;

    DCHECK(out != NULL && response_headers != NULL && body != NULL,
           "§7.4.5's computed type was asked for without a response — it is a fact about a header list AND the "
           "bytes those headers describe, so half of it is a type computed for some other response, and no "
           "bytes at all is a caller that has no response and is not asking this question");
    /* §5.1 "Interpreting the resource metadata" — the SUPPLIED MIME type and §5's check-for-apache-bug flag.
       THE LAST `Content-Type` HEADER, UNJOINED, which is what §5.1 says and is NOT Fetch §2.2.3's "extract a
       MIME type" over the joined list: §5's apache-bug table is a byte-exact comparison against four literal
       header values that a joined list can never equal. A caller that also needs the JOINED value (HTML
       §13.2.3.2 "Determining the character encoding" does) reads it separately — two standards read this one
       header differently and both are right. */
    supplied_defined = mime_sniff_supplied(&supplied, &apache_bug,
                                           header_list_get_last(response_headers, "content-type"));
    /* NULL IS §5.1's "the supplied MIME type is undefined" as a POSITIVE statement, which §7 step 2 branches
       on: an EMPTY record and an ABSENT one are different facts and mime_sniff_computed asserts the difference. */
    r.supplied = supplied_defined ? &supplied : NULL;
    r.apache_bug = apache_bug;
    /* §5's NO-SNIFF flag, which Fetch §3.6 "`X-Content-Type-Options` header"'s determine-nosniff answers over
       the same list — values[0] with its HTTP whitespace stripped, not a substring test. */
    r.no_sniff = header_list_determine_nosniff(response_headers);
    /* §7 steps 5 and 7's "SUPPORTED BY THE USER AGENT", the conjunct core/mime deliberately does not own:
       §4.6's groups say what a byte stream IS and what this build can decode is its decoders' fact. An IMAGE
       type is never supported here — this engine has no image decoder, so step 5's set is EMPTY — and an
       audio-or-video type is supported exactly when the modelled media device renders it, which is the one
       list §4.8.11.3's canPlayType answers from. It is stated ONCE, here, because a second loader restating it
       is a second answer to a question about one device. */
    r.ua_renders_supplied = supplied_defined && !mime_type_is_image(&supplied) &&
                            media_device_renders(&supplied);
    /* §5.2 "Reading the resource header": at most 1445 bytes. It is a bound on WHAT §7 LOOKS AT and never a
       truncation of the body — the parse that follows still gets every byte. */
    r.header = (const unsigned char *)body;
    r.header_len = body_len < MIME_SNIFF_RESOURCE_HEADER_MAX ? body_len : MIME_SNIFF_RESOURCE_HEADER_MAX;
    mime_sniff_computed(out, &r);
    /* §4.4 leaves an initialised-and-empty record behind on failure, so this free is the same operation
       whether §5.1 answered a type or `undefined`, and there is no path out of here that skips it. */
    mime_type_free(&supplied);
}

DocumentLoadType document_load_type_of(const MimeType *m)
{
    /* THE INPUT IS mimesniff §7's COMPUTED type, which is never undefined (core/mime/mime_sniff.h states why),
       so there is no "could not tell" arm here any more and an empty record is a CALLER that ran something
       else — Fetch §2.2.3's extraction, say, whose failure is a real answer and is not this one. */
    DCHECK(m != NULL && m->type != NULL && m->subtype != NULL,
           "HTML §7.4.5's load-a-document dispatch was handed something that is not a MIME type — its input "
           "is the COMPUTED type of the response (mimesniff §7), which every arm of that algorithm produces, "
           "so an empty record here is a caller that skipped the sniff and passed an extraction's failure");
    /* THE ARMS IN §7.4.5's OWN ORDER. See the enum's note on why the order is load-bearing rather than
       stylistic: `image/svg+xml` matches BOTH the XML group and the image group, and the XML arm is written
       first, so an SVG navigation loads an XML document and gets a DOM. */
    if (mime_type_is_html(m))                                     return DOC_LOAD_HTML;
    if (mime_type_is_xml(m))                                      return DOC_LOAD_XML;
    if (mime_type_is_javascript(m) || mime_type_is_json(m))       return DOC_LOAD_TEXT;
    /* §7.4.5's three LITERAL essences, which are not a mimesniff group and are written out as the list they
       are. They live here rather than in core/mime because they are this algorithm's list: no other algorithm
       groups `text/css` with `text/vtt`, and a group in mime_type.h that only this caller reads would be the
       table that header's first paragraph refuses. */
    if (!strcmp(m->type, "text") &&
        (!strcmp(m->subtype, "css") || !strcmp(m->subtype, "plain") || !strcmp(m->subtype, "vtt")))
                                                                  return DOC_LOAD_TEXT;
    if (!strcmp(m->type, "multipart") && !strcmp(m->subtype, "x-mixed-replace"))
                                                                  return DOC_LOAD_MULTIPART;
    /* §7.4.5's media arm is "a supported image, video, or audio type", and SUPPORTED is the operative word:
       the group is what mimesniff answers, and whether this engine can decode it is a different question that
       core/html/media_element.c owns. Both arms lead to a crash naming §7.5.6 today, so nothing turns on the
       difference yet — and when a decoder exists, the test that narrows this is that one and not this list. */
    if (mime_type_is_image(m) || mime_type_is_audio_or_video(m))  return DOC_LOAD_MEDIA;
    return DOC_LOAD_EXTERNAL;
}

const char *document_load_type_section(DocumentLoadType t)
{
    switch (t) {
    case DOC_LOAD_HTML:      return "HTML §7.5.2 Loading HTML documents";
    case DOC_LOAD_XML:       return "HTML §7.5.3 Loading XML documents";
    case DOC_LOAD_TEXT:      return "HTML §7.5.4 Loading text documents";
    case DOC_LOAD_MULTIPART: return "HTML §7.5.5 Loading multipart/x-mixed-replace documents";
    case DOC_LOAD_MEDIA:     return "HTML §7.5.6 Loading media documents";
    /* NO QUOTATION MARKS IN ANY OF THESE. check.h's APICLIENT_ASSERT_EMIT interpolates a message into a JSON
       object with `%s` and does not escape it, so a `"` here would emit a @WHY line the bridge cannot parse —
       the crash would still abort, and the one machine-readable field naming what to build would be lost. */
    case DOC_LOAD_EXTERNAL:  return "HTML §7.4.5's final Otherwise arm (external software, or §7.5.7 Loading a "
                                    "document for inline content that doesn't have a DOM)";
    }
    /* NOT A DEFAULT ARM. The switch above is exhaustive over the enum, so reaching here means a value that is
       not one of §7.4.5's arms — a caller that invented one, or a cast — and the sentence this function exists
       to produce would be a fabricated citation. CLAUDE.md §Browser half: a wrong section number is worse than
       none, so this crashes rather than returning a plausible one. */
    DFAIL("document_load_type_section was asked for a value that is not one of HTML §7.4.5's arms — every arm "
          "has a §7.5 subsection and this one came from neither document_load_type_of nor the enum");
    return NULL;
}
