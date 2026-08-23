/* WHICH DOCUMENT A RESPONSE LOADS AS — HTML §7.4.5 "Populating a session history entry"'s "load a document"
   type dispatch. See document_load_type.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_LOADER_DOCUMENT_LOAD_TYPE_H
#define ENGINE_HOST_BROWSER_CORE_LOADER_DOCUMENT_LOAD_TYPE_H

/* §7.4.5's ARMS, one value per `<dt>` of its "if the type is one of the following types" list, in the order the
   list is written. THE ORDER IS THE SPEC AND NOT AN IMPLEMENTATION DETAIL: `image/svg+xml` is BOTH an XML MIME
   type and a supported image type, and it is the XML arm that claims it because XML is written first — an
   engine that tested the image group first would render SVG as a media document and never build its DOM.
   Each value names the §7.5 subsection that loads it, because that subsection is what a caller with no loader
   for this arm has to build, and a `@WHY` is read at the site. */
typedef enum {
    DOC_LOAD_HTML,       /* §7.5.2 Loading HTML documents */
    DOC_LOAD_XML,        /* §7.5.3 Loading XML documents */
    DOC_LOAD_TEXT,       /* §7.5.4 Loading text documents */
    DOC_LOAD_MULTIPART,  /* §7.5.5 Loading multipart/x-mixed-replace documents */
    DOC_LOAD_MEDIA,      /* §7.5.6 Loading media documents */
    DOC_LOAD_EXTERNAL    /* §7.4.5's final "Otherwise" — external software, or §7.5.7's DOM-less inline content */
} DocumentLoadType;

/* §7.4.5's "Let type be the computed type of navigationParams's response", classified into the arm that loads
   it. `content_type_value` is the response's `Content-Type` header value as Fetch §2.2's "get" joined it —
   the same string mime_type_extract takes, and NULL for a header the response did not carry.
   A RESPONSE WITH NO USABLE TYPE IS `DOC_LOAD_EXTERNAL`, never HTML. That is the whole reason this returns an
   arm rather than a bool: "I could not tell" and "it is HTML" are different answers, and defaulting the first
   to the second is what makes an XML document silently answer as an HTML one. mimesniff's SNIFFING is what
   turns an absent or generic type into a real one and this engine does not run it yet, so the arm an unsniffed
   response reaches is the one whose crash names that. */
DocumentLoadType document_load_type_of(const char *content_type_value);

/* The §7.5 subsection that loads this arm, number and title, for the crash at a caller that has no loader for
   it. Never NULL — every arm has one, and a caller formatting an arm this does not know has asked the wrong
   question. */
const char *document_load_type_section(DocumentLoadType t);

#endif
