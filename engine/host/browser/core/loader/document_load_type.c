/* HTML §7.4.5 "Populating a session history entry" — the "load a document" TYPE DISPATCH, and nothing else.
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
#include <string.h>

#include "core/loader/document_load_type.h"
#include "core/mime/mime_type.h"
#include "check.h"

DocumentLoadType document_load_type_of(const char *content_type_value)
{
    MimeType m;
    DocumentLoadType t;
    bool ok;

    /* Fetch §2.2.3 "extract a MIME type" is what "the computed type" is built on, and its FAILURE is a real
       answer rather than an error: a response with no `Content-Type`, or one whose every candidate fails to
       parse, has no type for §7.4.5 to match and falls to the "Otherwise" arm. mime_type_extract leaves the
       record initialised-and-empty on failure, so the free below runs either way. */
    ok = mime_type_extract(&m, content_type_value);
    if (!ok) {
        mime_type_free(&m);
        return DOC_LOAD_EXTERNAL;
    }
    /* THE ARMS IN §7.4.5's OWN ORDER. See the enum's note on why the order is load-bearing rather than
       stylistic: `image/svg+xml` matches BOTH the XML group and the image group, and the XML arm is written
       first, so an SVG navigation loads an XML document and gets a DOM. */
    if (mime_type_is_html(&m))                                      t = DOC_LOAD_HTML;
    else if (mime_type_is_xml(&m))                                  t = DOC_LOAD_XML;
    else if (mime_type_is_javascript(&m) || mime_type_is_json(&m))  t = DOC_LOAD_TEXT;
    /* §7.4.5's three LITERAL essences, which are not a mimesniff group and are written out as the list they
       are. They live here rather than in core/mime because they are this algorithm's list: no other algorithm
       groups `text/css` with `text/vtt`, and a group in mime_type.h that only this caller reads would be the
       table that header's first paragraph refuses. */
    else if (m.type && m.subtype && !strcmp(m.type, "text") &&
             (!strcmp(m.subtype, "css") || !strcmp(m.subtype, "plain") || !strcmp(m.subtype, "vtt")))
                                                                    t = DOC_LOAD_TEXT;
    else if (m.type && m.subtype &&
             !strcmp(m.type, "multipart") && !strcmp(m.subtype, "x-mixed-replace"))
                                                                    t = DOC_LOAD_MULTIPART;
    /* §7.4.5's media arm is "a supported image, video, or audio type", and SUPPORTED is the operative word:
       the group is what mimesniff answers, and whether this engine can decode it is a different question that
       core/html/media_element.c owns. Both arms lead to a crash naming §7.5.6 today, so nothing turns on the
       difference yet — and when a decoder exists, the test that narrows this is that one and not this list. */
    else if (mime_type_is_image(&m) || mime_type_is_audio_or_video(&m))
                                                                    t = DOC_LOAD_MEDIA;
    else                                                            t = DOC_LOAD_EXTERNAL;
    mime_type_free(&m);
    return t;
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
