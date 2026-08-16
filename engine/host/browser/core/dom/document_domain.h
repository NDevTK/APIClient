/* HTML §7.1.1.2 RELAXING THE SAME-ORIGIN RESTRICTION — `document.domain`, its getter and its setter.
 *
 * WHY IT IS ITS OWN COMPONENT rather than a fifth member of core/dom/document_metadata.c, which owns the other
 * four members of the same partial interface. Those four are facts about the RESOURCE a document came from and
 * they answer by reading it. This one is a SECURITY OPERATION on the document's ORIGIN: it is the only thing in
 * the whole platform that mutates an origin (§7.1.1: origins "are generally immutable. Only the domain of a
 * tuple origin can be changed, and only through the document.domain API"), its setter is six ordered conditions
 * over the sandboxing flags, the agent cluster and the Public Suffix List, and every one of them decides who may
 * read whose DOM. One problem per file, and this is a different problem.
 *
 * WHAT IT CHANGES ONCE IT HAS RUN. §7.1.1's SAME ORIGIN-DOMAIN is the algorithm §7.3.1's `content document`
 * filters `iframe.contentDocument` by, and it and same ORIGIN disagree exactly where a domain has been set —
 * which is why the standard prints a table for the pair. Before this member existed every origin's domain was
 * null and the two agreed for every input, so the filter was passing a test that only checked the agreeing
 * rows. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_DOMAIN_H
#define ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_DOMAIN_H

#include "quickjs.h"

/* The AGENT's half: §3.1.1 declares `domain` read-write, and the setter's declaration is what performs Web IDL's
   USVString conversion before the body runs. Declared once per agent, beside document_metadata_init. */
void document_domain_init(JSContext *ctx);
/* The REALM's half: the accessor pair on Document.prototype. */
void document_domain_install(JSContext *ctx, JSValueConst proto);
/* Reached from document_agent_free — §7.1.1.2 is declared by document_init, so it is released by its declarer. */
void document_domain_free(void);

#endif
