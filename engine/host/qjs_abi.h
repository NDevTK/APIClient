/* THE PRODUCTION ABI, DECLARED ONCE — the `qjs_*` entries `engine/host/main.c` DEFINES and every host that
 * DRIVES them reads through.
 *
 * WHY IT EXISTS AT ALL, GIVEN THAT MAIN.C IS THE ABI. Until there was a second HOST in this repository there
 * was no C caller, so the list of entries lived in exactly two places that could disagree — main.c's bodies
 * and `engine/build.mjs`'s `QJS_ABI` export list — and that pair is checked against each other by a regex over
 * main.c's own `QJS_EXPORT` markers, in both directions. A C caller cannot use that check: it needs
 * DECLARATIONS, and a caller that writes its own is the hand-aligned copy `engine/route.mjs` spends a page
 * warning about ("a fourth and fifth hand-aligned copy of a list that has now gone short twice"). Emscripten's
 * own wrapper is one-sided about the same skew — too MANY arguments assert, too few are zero-filled in silence
 * — so a short caller reaches a `const char *` parameter as NULL and every later argument one slot early.
 *
 * SO THE DEFINITION AND THE CALLER ARE CHECKED AGAINST ONE STATEMENT BY THE COMPILER, which is strictly
 * stronger than the regex and is the whole reason main.c includes this rather than merely agreeing with it. A
 * parameter added to an entry here and not to its body is a compile error at the definition; a caller passing
 * the wrong count is a compile error at the call. Neither can be silent, which is the property the two lists
 * that already existed could not have.
 *
 * `QJS_EXPORT` IS HERE FOR THE SAME SENTENCE. The marker and the declaration are one fact about an entry — it
 * is what puts the symbol in the module's export table and what `build.mjs`'s check reads — so a header that
 * declared the entries while the marker was defined somewhere else would be a caller and a definition agreeing
 * about the signature and disagreeing about whether the thing is exported at all.
 *
 * WHAT THIS HEADER IS NOT: an ABI DOCUMENT. Every entry's contract — what it may be handed, what it asserts,
 * which of the scheduler's three codes it answers, who owns the pointer it returns — is stated at its BODY in
 * main.c, where the code that enforces it is. A second prose copy here would be the stale-comment failure with
 * a header's authority behind it. */
#ifndef APICLIENT_QJS_ABI_H
#define APICLIENT_QJS_ABI_H

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define QJS_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define QJS_EXPORT
#endif

/* PHASE 1 — root an agent at a document, or JOIN a second document of that same origin-keyed agent cluster.
   The two take the identical parameter list because they answer the identical questions about a document that
   arrives from outside; main.c's bodies say which of them is a fact of the OPERATION and which of the TARGET. */
QJS_EXPORT int qjs_init(const char *html, unsigned html_len, const char *url, const char *doc_id,
                        const char *headers, const char *top_level_url,
                        const char *inherited_csp, const char *inherited_csp_self_origin,
                        const char *inherited_coep, const char *inherited_coep_endpoint,
                        const char *inherited_coep_report_only,
                        const char *inherited_coep_report_only_endpoint,
                        const char *parent_navigable, const char *container_policy,
                        const char *ancestor_origins);
QJS_EXPORT int qjs_join(const char *html, unsigned html_len, const char *url, const char *doc_id,
                        const char *headers, const char *top_level_url,
                        const char *inherited_csp, const char *inherited_csp_self_origin,
                        const char *inherited_coep, const char *inherited_coep_endpoint,
                        const char *inherited_coep_report_only,
                        const char *inherited_coep_report_only_endpoint,
                        const char *parent_navigable, const char *container_policy,
                        const char *ancestor_origins);
QJS_EXPORT void qjs_unload(const char *doc_id);
QJS_EXPORT unsigned qjs_bundle_id(void);

/* PHASE 2 — seed the frontier, then step it a cooperative quantum at a time. */
QJS_EXPORT void qjs_begin(const char *recipes);
QJS_EXPORT int qjs_step(void);
QJS_EXPORT const char *qjs_result(void);
QJS_EXPORT void qjs_teardown(void);

/* WHAT THE TRUSTED ZONE OWES THE FRONTIER, and how it pays. */
QJS_EXPORT const char *qjs_pending(void);
QJS_EXPORT void qjs_provide(const char *method, const char *url, const char *reply, const char *body,
                            unsigned body_len);
QJS_EXPORT const char *qjs_host_requests(void);
QJS_EXPORT const char *qjs_host_notices(void);
QJS_EXPORT void qjs_host_answer(unsigned req, const char *json, unsigned completion,
                                const char *body, unsigned body_len);

/* THE CROSS-INSTANCE SEAM — a record routed in, an operation performed here, a peer's completion coming back. */
QJS_EXPORT void qjs_route(const char *record, const char *sender_origin);
QJS_EXPORT void qjs_world_gone(const char *world);
QJS_EXPORT void qjs_perform(const char *token, const char *record);
QJS_EXPORT void qjs_host_answer_remote(unsigned req, const char *world, const char *completion);

/* LEVEL-1's two dials, RAM pressure, and the streamed partial. */
QJS_EXPORT double qjs_top_weight(void);
QJS_EXPORT void qjs_set_yield_floor(double floor);
QJS_EXPORT void qjs_request_park(void);
QJS_EXPORT void qjs_emit_partial(void);

#endif
