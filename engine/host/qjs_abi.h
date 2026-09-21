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

#include <stdint.h>   /* the pixel run below is BYTES, and a byte has a spelling this header must name */

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
                        const char *ancestor_origins, const char *creation_sandbox_flags);
QJS_EXPORT int qjs_join(const char *html, unsigned html_len, const char *url, const char *doc_id,
                        const char *headers, const char *top_level_url,
                        const char *inherited_csp, const char *inherited_csp_self_origin,
                        const char *inherited_coep, const char *inherited_coep_endpoint,
                        const char *inherited_coep_report_only,
                        const char *inherited_coep_report_only_endpoint,
                        const char *parent_navigable, const char *container_policy,
                        const char *ancestor_origins, const char *creation_sandbox_flags);
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
/* …AND HOW IT REFUSES ONE, WHICH IS THE OTHER HALF OF PAYING AND NOT A KIND OF FAILURE. `qjs_provide` with the
   JSON `null` is Fetch §5.6 "Fetch methods"' NETWORK ERROR, and that is the honest answer wherever a real
   browser performing this same request would also produce one. Where none would — the firing policy declining
   to spend an act, the destructive deny list, a method the chokepoint cannot issue — this entry is the answer:
   nothing is fabricated about the origin, the flow keeps its park, and the engine forks the arm that runs the
   page's failure path so the `catch` is explored without the wait being spent. */
QJS_EXPORT void qjs_decline(const char *method, const char *url, const char *reason);
QJS_EXPORT const char *qjs_host_requests(void);
QJS_EXPORT const char *qjs_host_notices(void);
QJS_EXPORT void qjs_host_answer(unsigned req, const char *json, unsigned completion,
                                const char *body, unsigned body_len);

/* THE CROSS-INSTANCE SEAM — a record routed in, an operation performed here, a peer's completion coming back.
   `qjs_set_referenced` is the seam's LIFETIME half and is stated before the frontier is seeded, because it
   decides whether this instance's last timeline may finish at all. Everything below it is a record arriving at
   a document that is still there to receive one; without it there eventually is not. */
QJS_EXPORT void qjs_set_referenced(int referenced);
QJS_EXPORT void qjs_route(const char *record, const char *sender_origin);
QJS_EXPORT void qjs_world_gone(const char *world);
QJS_EXPORT void qjs_perform(const char *token, const char *record);
QJS_EXPORT void qjs_host_answer_remote(unsigned req, const char *world, const char *completion);

/* LEVEL-1's two dials, RAM pressure, and the streamed partial. */
QJS_EXPORT double qjs_top_weight(void);
QJS_EXPORT void qjs_set_yield_floor(double floor);
QJS_EXPORT void qjs_request_park(void);
QJS_EXPORT void qjs_emit_partial(void);

/* NAMED RESIDUAL OVER THE WHOLE PAINT SURFACE BELOW — IT HAS NO CONSUMER IN THE TRUSTED ZONE, AND THE
   ENGINE HALF IS NOT WHAT IS MISSING. Every paint entry declared below is in build.mjs's `QJS_ABI`, so each
   is `--export=`'d on wasm-ld and reaches the shipped module as `Module._qjs_*`; a host could call one today
   with no change to this file, to main.c or to the solver. What no party outside this process does is CALL
   one.
   WHAT IS NOT COVERED — A PROPERTY AND NOT A LIST: an ABI entry whose only caller is a fixture is exercised
   at exactly the cadence the fixture is run and never at the one the product is, so the per-world reach
   `qjs_request_paint_every_world` buys is spent only by a host that already renders. §Testing rates that the
   same as a translation unit that is in the program and in nobody's build, and the paint surface is thirteen
   entries in that state rather than one.
   WHAT THE NEXT DIFF BUILDS: the YIELD arm of the step loop in the trusted zone — the branch on
   ENGINE_STEP_YIELD, which is the moment the scheduler hands back a MARKED member standing with its COW and
   DOM deltas applied — asks `qjs_paint`, reads the twelve entries beside it in the order `qjs_paint_bytes`
   states, and hands the bytes and the world to the zone that may present them. test_forced.c's `abi_paint`
   is that read already written, against this same ABI. It is a CROSS-BOUNDARY diff in CLAUDE.md's sense — the
   zone's JavaScript is live on WRITE and this half is live only after a BUILD — so its two halves land
   together or neither lands.
   HOW ITS ABSENCE WOULD SHOW — STATED AS AN OBSERVATION AND NOT AS AN INSTANCE: a run of the shipped
   extension over a document that forks emits its findings and no image of any world, and nothing in the
   result says a picture was ever available — so a reader asking what one of this engine's forced arms LOOKS
   like has neither an artifact nor a statement that there could have been one. It is observed as the absence
   of any paint entry in the extension's own step loop, which is the one place a host that wanted a picture
   would have to ask; the same loop already records what this shape cost once, at the NEED_FETCH branch that
   made a whole reply path unreachable in the shipped extension while every gate stayed green.
   THE ACT THAT RETIRES IT, AND WHO MAY PERFORM IT, BECAUSE A PASSIVE CONDITION READS AS MERELY PENDING: the
   observation is about the SHIPPED extension, so what changes it is a trusted-zone diff landed TOGETHER with
   a build and an install of the module — and a lane in this project may not build, so the act belongs to the
   one role that may. A reader who runs the observation, gets the defer answer and correctly leaves this
   standing is asked to SAY SO rather than to wait, since nothing anywhere accumulates those readings.
   RETIREMENT — THE CONDITION THAT STOOD HERE IS REWRITTEN RATHER THAN DELETED, BECAUSE IT FOLLOWS FROM THE
   HEADLINE ABOVE AND A READER WILL RE-DERIVE IT. It read `this record goes when qjs_paint has a caller
   outside engine/host/`, which is what `what no party outside this process does is CALL one` invites — and it
   fires on a diff that leaves every other clause here answering DEFER. A Node driver that loads the glue and
   `ccall`s these entries is outside `engine/host/`; testing/render_engine.mjs is that driver already written,
   against `qjs_request_dump`; and such a driver is worth making and is not this record's subject, which the
   observation clause above states is the SHIPPED extension. A retirement that fires on a diff worth making is
   worse than one that never fires, because it will actually fire.
   THE CONDITION IS THEREFORE THE OBSERVATION'S OWN: this record goes when the extension's own step loop asks
   a paint entry on its ENGINE_STEP_YIELD arm. */

/* AN IMAGE OF THE DOCUMENT — the ONLY pair of entries in this ABI that carries BYTES outward, and two
   entries rather than one because linear memory has no length. `qjs_paint` PERFORMS the render and answers
   where the run starts; `qjs_paint_bytes` answers how long the run `qjs_paint` last produced is. A pointer
   and a count are answers of two different KINDS, and two of those taken from one call are free to be read
   into the variable named for the other — separate entries make that mistake a different CALL instead. The
   ownership, the lifetime and the order they must be asked in are at their bodies in main.c. */
QJS_EXPORT const uint8_t *qjs_paint(void);
QJS_EXPORT unsigned qjs_paint_bytes(void);

/* AND THE FACTS THAT MAKE THOSE BYTES AN IMAGE RATHER THAN A BAG OF PIXELS. `qjs_paint_width` and
   `qjs_paint_height` are the SHAPE, which the extent does not determine — `4 * w * h` has many factorisations,
   so a host holding only the pointer and the length cannot write the picture into any container, every one of
   which states its dimensions in its own header. `qjs_paint_offers`, `qjs_paint_marks` and
   `qjs_paint_complete` are core/paint/document_paint.h's `DocumentPaintCount` made readable, and that header
   is the whole argument for them: "a surface with no ink on it means the walk reached no box, or reached
   boxes that painted nothing, or was STOPPED — and a caller handed only the bitmap cannot separate them".
   `complete` FALSE is a PARTIAL picture, which is the one of the three a host must never publish as a whole
   one. Each asserts that a render happened, for `qjs_paint_bytes`' own reason: zero is already an answer.
   AND THE THREE OF THEM ARE A SUMMARY WHOSE PARTITION IS THE BLOCK BELOW — `qjs_paint_offers` is a total over
   four outcomes that entry states and `qjs_paint_marks` is the ink those outcomes are counted against.
   THE BODIES AND THE REST OF THE CONTRACT ARE IN main.c, including why `spans` and `pixels` have no entry. */
QJS_EXPORT unsigned qjs_paint_width(void);
QJS_EXPORT unsigned qjs_paint_height(void);
QJS_EXPORT unsigned qjs_paint_offers(void);
QJS_EXPORT unsigned qjs_paint_marks(void);
QJS_EXPORT int qjs_paint_complete(void);

/* …AND WHAT BECAME OF EACH OF THOSE OFFERS, WHICH IS THE NUMBER ABOVE BROKEN APART. `qjs_paint_offers` beside
   `qjs_paint_marks` separates a walk that ran from one that did not; it does NOT separate the three things a
   walk that ran and laid nothing can mean, and those take opposite work. Every box was legitimately
   transparent — the picture is correct and the document has no ink in it. A step or a replaced element's kind
   has no ink IN THIS ENGINE — a capability to build, named by core/paint/box_paint.h's residuals. The painter
   STOPPED — a partial picture whose remaining boxes were never looked at.
   `qjs_paint_offer_outcome` IS A PARTITION AND `qjs_paint_decline` IS NOT, which is why they are two entries
   and not one indexed register. The four outcomes sum to the offer count and core/paint/box_paint.c asserts
   it; the reasons are NOT exclusive, because one offer is one of CSS 2.1 §E.2 "Painting order"'s STEPS and a
   step's sub-list holds several items — a box declaring neither a background nor a border meets two of them.
   Separate entries are what make reading a reason as a share of the offers a different CALL rather than a
   different index.
   THE INDEX IS A `BoxPaintOutcome` AND A `BoxPaintDecline`, which core/paint/box_paint.h declares and every
   host of this ABI includes. Indexing is sound here for the one reason it ever is: the set is THIS ENGINE's
   own and is fixed at its declaration, so a position is a fact about the operand rather than an ordinal over
   something that moves. Both crash on an index outside their enum.
   THEY ARE LIFETIME COUNTERS OVER ONE RENDER, like every number beside them, and their DENOMINATOR is
   `qjs_paint_offers` — a host quoting one without it has published a numerator alone. */
QJS_EXPORT unsigned qjs_paint_offer_outcome(unsigned outcome);
QJS_EXPORT unsigned qjs_paint_decline(unsigned reason);

/* …AND HOW BIG THE TREE WAS THAT THE WALK RAN OVER, which is the one fact none of the numbers above can
   carry because every one of them is the WALK's own. An offer count is a statement about the painter and this
   is a statement about the DOCUMENT, taken at the same instant, by the same walker, over the same subtree.
   IT IS THE ONLY THING THAT SEPARATES AN EMPTY DOCUMENT FROM AN UNREACHED ONE. Seven offers over three
   elements is a page with nothing in it; seven offers over nine hundred is a walk that reached almost none of
   a page that HAS something in it — and for a document whose UI its own JavaScript builds, those are the two
   readings a host holding an image most needs to tell apart.
   IT IS A COMPARISON AND NEVER A BOUND. core/paint/document_paint.h states why no sound inequality holds in
   either direction, and the answer is not asserted anywhere. */
QJS_EXPORT unsigned qjs_paint_elements(void);

/* …AND WHICH WORLD THE PICTURE IS OF, WHICH IS THE ONE FACT THAT MAKES AN IMAGE OF THIS ENGINE DIFFERENT FROM
   AN IMAGE OF A BROWSER. Everything above states what the ink IS; this states whose TIMELINE it was laid in.
   A document under this engine has as many appearances as it has flows — a forced sibling that took the other
   arm of `if (__FLAGS.admin)` holds a DOM and a heap its primary never had — so `qjs_paint`'s bytes are an
   answer to what this page looks like only once somebody has said WHOSE page. Without it two renders of
   two worlds are two files, and a reader holding them cannot say they are two worlds rather than two reads of
   one; that is the same unattributable-artifact defect `qjs_paint_bytes` closes one axis over, a render
   rather than a length.
   IT IS NEVER EMPTY AND IT NEVER ANSWERS `NULL`. The BASELINE — the document as no flow has written it — is a
   real world to paint and is what every run that ends with its frontier drained is standing in, so it is
   stated in a word rather than left as an absence a reader fills in. THE BODY IN main.c IS WHERE THAT WORD IS
   AND WHY A NAME AND A BASELINE CANNOT BE CONFUSED.
   It asserts that a render happened for the same reason its five neighbours do, and it answers about the
   render `qjs_paint` LAST PERFORMED rather than about this instance now: the world the engine is standing in
   moves with every `qjs_step`, so a live read would name a timeline that is not the one in the picture. */
QJS_EXPORT const char *qjs_paint_world(void);

/* …AND WHETHER THAT TIMELINE STANDS ON A CONTRADICTION, WHICH IS THE HALF A NAME CANNOT CARRY. The entry
   above says WHICH of a document's appearances this is; it says nothing about whether the path that laid the
   ink ever took an arm its own concrete example CONTRADICTS, and a picture of such a path is a picture of a
   page no session reaches. solver/flow.h's `path_forced` is that fact and main.c's body is where the words
   are — including, as a NAMED RESIDUAL, the one thing this is NOT: an answer to whether this is the
   appearance a browser would have produced. It is NECESSARY for that and it is not sufficient. */
QJS_EXPORT const char *qjs_paint_forced(void);

/* THE INSTRUMENT SEAM — a program a DRIVER wants evaluated in the analysed document's realm, and the dumped
   completion values coming back, one per live timeline. Its contract is at its body in main.c and its whole
   design at solver/engine.h's engine_request_dump; what is worth knowing from a declaration is that the two
   are a PAIR and that neither is part of the product's finding surface. */
QJS_EXPORT void qjs_request_dump(const char *program);
QJS_EXPORT const char *qjs_dumps(void);


/* …AND EVERY WORLD THIS RUN GOES ON TO MINT, WHICH THE ASK ABOVE CANNOT COVER AT ANY CADENCE — @PERWORLD. The
   ask above names the timelines alive when it is called; a host reaches this engine only BETWEEN two steps, so
   a timeline that is forked, run and finished inside one step is standing at no boundary and is photographed by
   no number of re-asks. This one is that ask plus a standing mode read where a timeline is MINTED, so the two
   together name every world of the run. It is one-way, it renders nothing, and it costs one extra return per
   timeline that ever ends. The contract is at solver/engine.h's engine_request_paint_every_world. */
QJS_EXPORT void qjs_request_paint_every_world(void);

#endif
