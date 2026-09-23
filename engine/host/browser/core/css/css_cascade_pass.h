/* WHAT A WHOLE-TREE RENDER KNOWS ABOUT THE CASCADE — css-cascade-5 §4.2 "Cascaded Values"' answer for one
 * (element, property), held for the span of one CSS 2.1 §E.2 "Painting order" walk and for no longer.
 *
 * THE DEFECT IT CLOSES IS NOT A SLOW FUNCTION, IT IS ONE ANSWER RE-DERIVED ONCE PER ASKER. css-cascade-5 §6
 * "Cascading" — "The cascade takes an unordered list of declared values for a given property on a given
 * element, sorts them by their declaration's precedence as determined below, and outputs a single cascaded
 * value" — is a function of the ELEMENT and the PROPERTY and of nothing about who is asking, and a render
 * asks for the same pair over and over for two reasons that compound:
 *   - css-cascade-5 §7.2 "Inheritance": "The inherited value of a property on an element is the computed
 *     value of the property on the element's parent element." So a read of an inherited property CLIMBS, and
 *     every element on that chain re-runs the whole cascade for every descendant that asks.
 *   - css-logical-1 §4 "Flow-Relative Box Model Properties" makes the pairing a PREREQUISITE of the cascade
 *     itself — "It also requires that writing-mode, direction, and text-orientation be computed as a
 *     prerequisite for cascading together the flow-relative and physical declarations of a logical property
 *     group" — and both of those are inherited, so every margin, padding, border and inset read on any
 *     element pays two climbs to the root before it looks at one declaration.
 *
 * THE COST IS A MULTIPLIER AND NOT AN INHERENT COST, WHICH IS A MEASUREMENT AND NOT AN ARGUMENT, AND THE
 * DERIVATION IS A COMMAND RATHER THAN A FIGURE: `node engine/layout_cost.mjs <native binary>` counts calls
 * with gdb breakpoints over two document SHAPES that hold the same number of elements, and the reading is
 * the pair of orders rather than either number. Before this record, `bp_visit` — CSS 2.1 §E.2's offer count,
 * which is the denominator every other row is a cost per — was LINEAR in both shapes while
 * `cssom_cascaded_value` was QUADRATIC over N siblings at constant depth and CUBIC over N nested boxes. An
 * order that RISES BY ONE when the depth grows names a per-ANCESTOR factor; one that is already superlinear
 * at constant depth names a second, per-SIBLING factor; and a LINEAR number of asks over superlinear work is
 * the signature core/layout/flow_placement.h names for a multiplier.
 *
 * WHICH SHAPE THE COST IS DECIDES THE INSTRUMENT, AND HERE IT IS NEITHER OF core/layout/flow_placement.h's
 * TWO. That component's CSS 2.1 §9.4.1 "Block formatting contexts" half is a WALK over a sibling list,
 * where a memo of the return value leaves N walks of O(N) and only the walk REPORTING removes the
 * multiplier; its CSS 2 §8.1 "Box dimensions" half is a RECURSION over
 * ancestors, where remembering the return value IS the collapse. This one is a THIRD shape and it is the
 * simplest of the three: ONE ANSWER ASKED BY MANY CALLERS. The per-sibling factor is N askers of N distinct
 * keys, and the per-ancestor factor is one key asked once per descendant — both collapse the moment the pair
 * is answered once, and neither needs a walk to report anything. That is why this record is keyed on the
 * QUESTION (element, property) rather than on the asker, and why it sits at the one function every asker
 * reaches rather than at any of them.
 *
 * ITS LIFETIME IS A PASS AND IT HAS NO INVALIDATION RULE AT ALL, which is the point and not an omission —
 * core/css/css_style_declaration.h states the standing objection to caching a cascade answer, that it would
 * be shared state the flow machinery does not swap, and that objection is exactly right about a record with
 * a LIFETIME. This one has none: it is EMPTY before a pass opens and EMPTY after it closes, so there is no
 * span in which one flow's answer can be served to another and nothing here decides whether an entry is
 * still good. A flow SWITCH inside the span is not reasoned about either; it is the case the tree version
 * below exists to crash on.
 *
 * THE SPAN IS ASSERTED AT THREE PLACES RATHER THAN DESCRIBED AT ONE, which is core/layout/flow_placement.h's
 * arrangement for the same reason — the premise is that the cascade's inputs do not move inside the walk,
 * and a premise stated in prose is a premise nobody can fail:
 *   - RE-ENTRY: a pass may not open inside a pass. Two passes would share one record with two lifetimes and
 *     the inner close would empty a record the outer walk is still reading.
 *   - THE TREE: `dom_cow_version()` is read at open and asserted unchanged at close AND at every ask this
 *     record answers. That number moves on an insert, a removal and — the case that matters — on the COW
 *     SWAP, so a flow switch during the walk crashes rather than serving one world's cascade into another.
 *   - EVERY OTHER CASCADE INPUT IS MADE IMPOSSIBLE AT ITS OWN WRITE, because no version number can see it.
 *     `dom_cow_version()` does not move for an attribute, and `class`, `id`, `style` and every
 *     presentational attribute reach the cascade without touching the tree's shape; nor does it move for a
 *     CSSOM write, and css-cascade-5 §6.2's author origin is the STYLE SHEET OBJECTS a page holds — whose
 *     rules it inserts and deletes, whose declarations it sets, and whose `disabled` flag it flips. Each of
 *     those writes asserts that no pass is open, at the write, which is the only place the two can be made
 *     exclusive by construction. If one fires, the write is real and the PASS is in the wrong place; the fix
 *     is where the pass opens and closes and never a re-check on the read side.
 *   - AND THE ONE THAT IS NEITHER THE TREE NOR A STYLE INPUT, which is here because a reader counting the
 *     three above would otherwise have to find it: selector matching has exactly ONE host-language answer,
 *     and it is `:defined` over DOM §4.9 "Interface Element"'s custom element state. An UPGRADE moves that
 *     state without touching the tree, so a selector that matched one way before it matches the other way
 *     after, and the tree version is blind to it. It is closed at ITS write like the rest.
 *
 * WHAT THIS RECORD DOES NOT DO, STATED BECAUSE THE PRECEDENT DOES IT AND A READER WILL LOOK FOR IT: it does
 * NOT re-derive its answer at every hit. core/layout/flow_placement.h's border-box origin can afford that —
 * its check re-reads CSS 2 §10.1 "Definition of 'containing block'"'s equation out of the CASCADE and is
 * O(1) because it takes the containing block's
 * point out of the record rather than recursing — and the equivalent here is re-running the cascade, which
 * is the whole of the cost this component exists to remove. So the memo-integrity half is bought by the
 * probe (a slot is occupied by ONE (element, property) and answers only that pair) and the span half is
 * bought entirely by the tree version and by the write-side crashes above. An instrument trusted past its
 * evidence is worse than none: those three are what holds this record, and there is no fourth check here
 * quietly agreeing with them.
 *
 * AND ONE INPUT IS HELD BY THE SPAN ITSELF RATHER THAN BY ANY OF THE THREE, WHICH IS SAID HERE BECAUSE A
 * READER COUNTING CHOKEPOINTS WOULD OTHERWISE FIND IT MISSING. A sheet's media query is evaluated against
 * the ENVIRONMENT — HTML §4.2.4.1 "Processing the media attribute" makes that attribute prescriptive
 * against the environment — and the environment is the viewport this render was handed. It cannot move
 * inside a pass because the pass is inside ONE render, and a second render is a second pass over an
 * empty table; the MediaList a page can rewrite is a different question and is one of the writes above.
 *
 * NAMED RESIDUAL — THIS RECORD HAS ONE OPENER AND IT IS A PAINT, SO THE SHIPPED PRODUCT NEVER OPENS IT.
 * WHAT IS NOT COVERED, AS A PROPERTY AND NOT A LIST: `css_cascade_pass_open` is called from exactly one
 * place, core/paint/document_paint.c's CSS 2.1 §E.2 "Painting order" walk, and `engine/host/qjs_abi.h`'s own
 * residual records that no party outside this process CALLS the paint ABI — the extension's step loop asks
 * no paint entry on its yield arm, and the fixtures and the Node render drivers are the only callers there
 * are. So on the one path the product actually runs, EVERY ask this record answers is a miss and the whole
 * of the multiplier described above is paid: the record is exercised at the cadence of a fixture and never
 * at the cadence of the product, which CLAUDE.md's §Testing rates the same as a translation unit that is in
 * the program and in nobody's build.
 * WHAT THE NEXT DIFF BUILDS — THE CLAUSE THAT STOOD HERE WAS WRONG IN BOTH ITS HALVES AND IS REWRITTEN
 * RATHER THAN DELETED, BECAUSE IT WAS DISPATCHED AS A BRIEF BEFORE ANYBODY RE-DERIVED IT AND A READER WILL
 * COMPOSE IT AGAIN. In its own wording, unquoted because a run of this tree's prose is not a spec quotation
 * and the citation auditor's channel cannot tell the two apart: nothing here, the span is correct and what
 * is absent is a CALLER, so this record's reach is bought by the trusted zone's yield arm asking for a
 * paint; and a SECOND opener added for the non-paint population would be a second span with a second
 * lifetime over one table, which the RE-ENTRY paragraph above forbids for its own reasons.
 * THE FIRST HALF NAMED A DIFF THAT SHOULD NOT BE MADE, AND qjs_abi.h's OWN RESIDUAL NOW RECORDS WHY AT
 * LENGTH. A paint is update-the-rendering's step 22, which core/rendering/rendering.c's note at that step
 * calls the only one of that algorithm's twenty-three steps with no headless equivalent; every step a page
 * can OBSERVE is one of the other twenty-two, and the machinery for them is WIRED on the shipped path from
 * the scheduler's own rendering rung. WIRED is a correction to a sentence here that said they RUN: that rung
 * sits inside solver/engine.c's `if (!f->frame)`, and that file's own note says the ladder below it is
 * unreachable for the population its live/framed census names, so whether those steps run is open and is not
 * a question this record or a paint answers. Either way the yield arm is not a caller this record was
 * waiting for, and the correction cuts toward the same answer: if those steps under-run, a paint recovers
 * none of them, because a paint is step 22 and no other. It is a render nobody
 * presents, which would open this span as a side effect of an output the product does not produce — and a
 * record whose reach is bought that way is reached for a reason that can be withdrawn at any time.
 * THE SECOND HALF CONFLATED TWO OPENERS WITH TWO LIFETIMES, AND ONLY THE SECOND IS WHAT RE-ENTRY FORBIDS.
 * That paragraph forbids a pass opening INSIDE a pass, because the inner close empties a table the outer
 * walk is still reading. It says nothing about a DISJOINT second opener, and this component already expects
 * many spans per run: it is EMPTY before a pass opens and EMPTY after it closes, and `passes` is a COUNT
 * rather than a flag. So the sentence foreclosed the only answer actually available to this record by citing
 * a rule that does not reach it, which is worse than leaving the question open.
 * THE MEASUREMENT THAT CLAUSE NAMED IS BUILT, AND THE CLAUSE IS REWRITTEN RATHER THAN DELETED BECAUSE IT WAS
 * RIGHT AND A READER WHO RE-DERIVES IT WILL COMPOSE IT AGAIN. In its own wording: this record's three
 * counters have exactly ONE reader — the identity DCHECK inside `css_cascade_pass_close`, which never runs on
 * a path the product takes — so the cost claimed above is unmeasured on the only path that matters and cannot
 * be measured from the product's own document at all; a census accessor beside core/layout/flow_placement.h's
 * `flow_placement_census`, read by solver/result.c into a `_cascade` block beside its `_layout` one, is what
 * turns the argument above into a number, and it also ARMS that identity on the product's path where a
 * miscounting arm is caught by nothing. `css_cascade_pass_census` below is that accessor and result.c
 * composes that block, so both halves are discharged: the number is published, and the identity is asserted
 * at a call the shipped path makes rather than only at a close no shipped path reaches.
 * AND THE SAME DIFF MADE A SENTENCE FOUR LINES ABOVE TRUE THAT WAS NOT TRUE WHEN IT WAS WRITTEN, which is
 * recorded here because it is the kind of claim nothing mechanical checks. `passes` is a COUNT rather than a
 * flag was written about THIS component, and this component held only `g_open`, a bool — the count it named
 * did not exist, so a reader sent to read that row would have found nothing and had no way to tell an absent
 * counter from a counter reading zero. It exists now as `passes_life`, and building it was the right repair
 * rather than striking the sentence: the argument the sentence was making is correct and the component was
 * what was short of it.
 * WHAT THE NEXT DIFF BUILDS IS STILL A SPAN, IT IS STILL NOT PROPOSED HERE AS WORK, AND THE ORDER IS
 * UNCHANGED: only with that number in hand is it worth asking whether a span belongs on an algorithm the
 * product DOES run. The candidates are the ones
 * that ask this record about many elements with no author code inside them, which is the property a span
 * needs and not a list of sites: core/intersection_observer/'s per-target geometry, which rendering.c drives
 * at update-the-rendering step 19 under a note recording that those steps run NO author callbacks, and
 * core/resize_observer/'s gather at step 16, both of which reach this record through core/layout/used_value.h
 * and core/css/css_computed_value.h. NEITHER IS PROPOSED HERE AS WORK: they are components this file does not
 * call, their magnitude is unmeasured, and a span attached on an argument rather than on a number is the
 * change CLAUDE.md refuses by name.
 * AND WHAT THE SHIPPED PATH'S ASKS ARE IS DERIVABLE BY READING CALLERS, WHICH THE COUNT CANNOT SAY AND WHICH
 * DECIDES THE NEXT DIFF. GREPPED: `cssom_cascaded_value` has three callers — this component's computed-value
 * entry, core/html/html_element_view.c's chain detector and core/layout/used_value.c's positioning
 * containing-block detector — and the first is reached from many layout and view components, which is a
 * DERIVATION rather than a figure because it grows with the tree: `git grep -l 'css_computed_value(' --
 * '*.c'`, minus that entry's own file. So the route is NOT one entry and no sentence naming one is true.
 * THE FIGURE THAT STOOD HERE WAS `about twenty` AND IT WAS READ OFF A TRUNCATED LIST, which is recorded
 * because the defect is this file's own and the repair is the one CLAUDE.md prescribes: a `head -20` answered
 * with exactly twenty rows, the cap was dropped in transcription, and a FLOOR was written down as a total —
 * the command above answers 30 today. A count over a population that grows is handed over as the command
 * that derives it, never as the number a reader will quote onward.
 * WHAT IS TRUE IS A STATEMENT ABOUT ORIGINS, and it is the one the span question needs: `document_paint` is
 * the only originator in this engine that is not either a JS member call or a step of HTML §8.1.7.3
 * "Processing model"'s update the rendering, and it has no shipped caller. So on a non-painting path every
 * ask begins in one of those two, and BETWEEN two of them arbitrary page code runs — which the three
 * write-side crashes above forbid inside a span, and which CLAUDE.md's per-opcode attention makes a possible
 * flow SWITCH, the one case the tree version exists to crash on. A SPAN HELD ACROSS TWO ORIGINS IS THEREFORE
 * NOT MERELY UNBUILT, IT IS UNSOUND, which is a stronger and more useful statement than `there is no
 * caller`.
 * A SPAN INSIDE ONE MEMBER CALL IS SOUND AND ITS WORTH IS UNMEASURED, WHICH IS THE HONEST STATE RATHER THAN A
 * PROPOSAL. One member call is one C activation: no page code runs in it, and the per-opcode preempt is in
 * the interpreter and not in a C body, so all three of this record's assertions hold over it by construction
 * — and that is true of BOTH kinds of origin, which is why the observer gathers named above are candidates
 * for the same reason a member call is and not for a different one. The two detectors are the clearest
 * shipped-path instances, each a C loop over the ancestor chain asking the cascade once per ancestor per
 * property, which is the many-elements property a span needs. What is unknown is whether a span would SERVE
 * anything, which turns on whether one activation asks one PAIR twice. INFERRED and not measured, so it is
 * written as the hypothesis it is: css-logical-1 §4's pairing climb asks `writing-mode` and `direction` once
 * per ancestor and the positioning detector asks its three properties once per ancestor, and those are
 * DISTINCT keys — so the repeats this record lives on may all be ACROSS member calls, which is the span that
 * cannot be held. If that is so, the sound span serves nothing and the span that would serve is unsound, and
 * the answer is neither an opener nor a memo but a caller that asks about many elements at once.
 * THE ONE NUMBER THAT DECIDES IT IS NOT IN THIS CENSUS AND `passes_life` IS NOT IT: that row separates an
 * absent CALLER from keys that do not repeat, and with no pass ever opened it separates nothing about
 * repeats. What answers it is `served_life` read from a span opened around ONE of the two detectors, which is
 * the candidate fix and its own measurement at once and costs a run that opens no pass nothing at all. It is
 * NOT proposed here as work — whether either detector is worth a span is a question about a magnitude nobody
 * has — and these paragraphs exist so the next reader spends their first command on that number rather than
 * on an opener.
 * RETIREMENT FOR THESE THREE PARAGRAPHS: they go when `served_life` has been read from a span opened anywhere
 * on a non-painting path, because the repeat question is then answered by a measurement instead of by the
 * inference above.
 * HOW ITS ABSENCE WOULD SHOW, AS AN OBSERVATION AND NOT AS AN INSTANCE: `node engine/layout_cost.mjs
 * <native binary>` on the `styled` shape reports `cascade_emit` EQUAL to `cssom_cascaded_value`, which that
 * file's own banner states is the sheet being flattened once per (element, property) resolution rather than
 * once per render — and it reports that under a driver that DOES paint, so a reader who sees it there is
 * seeing the residue this record cannot remove rather than its absence. The absence itself is observed one
 * level out, in a run of the shipped extension: no paint entry is asked, so no pass is opened, so every ask
 * is a miss.
 * THE ACT THAT RETIRES THIS CLAUSE IS PERFORMED AND THE RESIDUAL IS NOT, WHICH IS THE PAIR A READER MUST NOT
 * COLLAPSE. It read that the census was written, that the number was in this tree and in NO ARTIFACT, and
 * that a reader who ran the product would therefore still see no `_cascade` block; it is rewritten rather
 * than struck because a discharged act sitting inside a live residual reads as the whole record being spent.
 * MEASURED, by content in the installed artifact and with two invented negative controls answering zero,
 * against a stamp carrying an empty dirty cone: `_cascade`, `asksLife` and `passesLife` all occur in the
 * shipped bytes. So the observation below can be RUN now, and running it retires THIS CLAUSE and nothing
 * else — the RETIREMENT at the foot of this record is a shipped-path OPENER, and no build supplies one.
 * AND IT HAS BEEN RUN, WHICH IS WHAT TURNS THE ARGUMENT AT THE TOP OF THIS FILE INTO A NUMBER. RELAYED to
 * the lane that wrote this rather than taken by it, and labelled as a relay for that reason: a live run over
 * a real page read `asksLife 4380, servedLife 0, resolvedLife 4380, passesLife 0`. Three of those four are
 * what solver/result.c's own block predicts BY CONSTRUCTION for any path that does not paint, so they
 * corroborate the wiring and settle nothing. The MAGNITUDE is the new fact and it is the one this component
 * was justified by: four thousand asks, on one page, for an answer that is a function of the element and the
 * property and of nothing about who is asking, with this record never once consulted.
 * RETIREMENT — IT NO LONGER GOES WITH qjs_abi.h's, AND THE UNCOUPLING IS THE CORRECTION RATHER THAN A
 * DETAIL: this record goes when `css_cascade_pass_open` has a caller on a path the shipped product runs,
 * whatever that caller turns out to be. Tying the condition to a PAINT is what made every reader of it,
 * including the one who wrote the clause above, conclude that the paint was the answer.
 *
 * NAMED RESIDUAL — THE RECORD HOLDS ONE OF THE FOUR SHAPES A COMPUTED VALUE COMES IN, AND THE CLIMB ITSELF
 * IS UNTOUCHED. WHAT IS NOT COVERED: this is the CASCADED value, so css-cascade-5 §7's defaulting still runs
 * per ask and §7.2's inheritance still walks to the root once per descendant — each of those frames is now a
 * table probe and a string copy instead of a sheet walk, a UA-table scan and a sort, but the FRAME COUNT is
 * unchanged. core/css/css_computed_value.c's other three entries — the absolute length, css-inline-3 §5.1's
 * `line-height` union and CSS 2.1 §17.6.1's `border-spacing` pair — each climb the same way and each reads
 * an environment fact this record deliberately does not hold, so none of them may be answered out of it.
 * WHAT THE NEXT DIFF BUILDS: the COMPUTED value beside the cascaded one, as a second fact under this same
 * span and this same key, which terminates css-cascade-5 §7.2 "Inheritance"'s climb at the first answered
 * ancestor exactly as core/layout/flow_placement.h's CSS 2 §8.1 "Box dimensions" memo terminates its own;
 * the length-shaped entry needs its own field
 * because a `CssPx` carries a domain a string cannot. HOW ITS ABSENCE WOULD SHOW, as an observation and not
 * as an instance: with this record in place, the command above reports a row that is LINEAR in the shape
 * whose depth is constant and superlinear in the shape whose depth grows — the residue being a per-ancestor
 * frame count that no longer carries a cascade with it.
 *
 * RETIREMENT: this component goes when the cascade PRODUCES a computed-style object that a render reads,
 * because a cascaded value is then a field of that object rather than a function re-derived per ask, and
 * there is nothing left to record. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_CASCADE_PASS_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_CASCADE_PASS_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

/* OPEN AND CLOSE THE PASS. `open` empties the record and takes the tree version it will be held to; `close`
   empties it again and releases every string it owns, so no allocation and no entry outlives the span. Both
   belong in ONE function of ONE component — the entry that performs the whole-tree walk — because a pass
   whose two ends are in two callers is a pass a returning arm can leave open. */
void css_cascade_pass_open(void);
void css_cascade_pass_close(void);

/* IS A PASS OPEN — read by the write chokepoints of every cascade input, which assert it is NOT, and by
   nothing that branches on it to decide what to compute. It is a fact about the instrument, never an
   operand. */
bool css_cascade_pass_is_open(void);

/* THE ASK, COUNTED WHETHER OR NOT A PASS IS OPEN — the recording point is the QUESTION and never the
   outcome, so a run that opens no pass at all still COUNTS how many cascaded values it was asked for.
   AND THEY ARE REPORTED, WHICH IS WHAT THIS SENTENCE NOW SAYS AND IS THE ONE THING A READER ACTS ON.
   IT USED TO SAY THEY WERE NOT, AND THAT IS KEPT RATHER THAN STRUCK BECAUSE A READER WHO FINDS THE COUNTERS
   AND NOT THE ACCESSOR WILL RE-DERIVE IT: the three counters had exactly one reader, the identity DCHECK
   inside `css_cascade_pass_close`, and that function never runs on a path the product takes — so an ask
   counted here reached nobody. `css_cascade_pass_census` is the second reader and solver/result.c is its one
   caller, so the count taken at this line is published in the document the shipped path composes.
   Answers TRUE and writes `*out` when this record holds the pair; the value is an OWNED copy the caller
   frees exactly as it frees the cascade's own answer, and NULL is one of the values it can be — css-cascade-5
   §4.2 "Cascaded Values"' "if the output of the cascade is an empty list, there is no cascaded value" is a
   real answer and not a miss. FALSE means the caller owes the cascade and must report the answer with
   `css_cascade_pass_record`, so the census closes. */
bool css_cascade_pass_ask(lxb_dom_element_t *el, const char *name, char **out);

/* …AND THE CASCADE THAT ASK COST. Called by the one caller of `css_cascade_pass_ask` on its FALSE arm,
   immediately, so `asks == served + resolved` holds at every instant and not merely at the end. `value` is
   BORROWED — the record takes its own copy — and NULL is stored as the answer it is. A no-op for the storage
   when no pass is open, which is what keeps the cascade's own code free of a mode; the count is taken
   either way. */
void css_cascade_pass_record(lxb_dom_element_t *el, const char *name, const char *value);

/* THE CENSUS, in solver/result.c's vocabulary, and it is what gives the counters above a reader on a path the
   product takes. Until it existed they had exactly ONE — the identity DCHECK inside `css_cascade_pass_close`,
   which runs only under a paint, which the residual at the top of this file records that nothing outside this
   process asks for. So the multiplier this whole component is justified by was COUNTED and never REPORTED,
   which is core/css/css_style_declaration.h's write-with-no-reader defect one level up and is worse than an
   absent number: a number nobody reads cannot be wrong, so the cost premise could be neither confirmed nor
   refuted from the product's own document.

   EVERY FIELD IS A LIFETIME COUNTER OF THIS AGENT, AND EACH SAYS SO IN ITS OWN NAME RATHER THAN ONLY HERE.
   A GAUGE states what is true NOW and a LIFETIME COUNT states what has happened since this agent started, and
   only the second may be differenced across two samples or accumulated — so a consumer that mixes them is
   doing arithmetic on nothing while looking exactly like a measurement. The `Life` suffix is engine/build.mjs's
   @WFQ convention, declared there and used here for its reason: that census names `brBornLife*`, `brUsLife*`
   and `brRetiredUsLife` as LIFETIME against the unsuffixed rows beside them that are GAUGES, and it says the
   kinds are taken from the declaration and not guessed. A CONSUMER READS THE KEY AND NEVER THE COMMENT, which
   is not a general worry but a measured property of the document these rows land in: solver/result.c already
   carries `_routedZeroDelivery`, a gauge, between two lifetime counts, with only prose to say so.

   NOTHING HERE REPORTS THE RECORD'S LIVE SIZE, DELIBERATELY. That is this component's one genuine gauge, it
   is zero at every instant no pass is open, and publishing it beside these four is the exact mixing the
   suffix exists to prevent — so the omission is a decision and not a gap, and the day somebody wants it, it
   is a row that must carry a name saying it is not one of these.

   `asks_life` is how many times css-cascade-5 §4.2's cascaded value was asked for, `served_life` how many of
   those this record answered out of a pass, and `resolved_life` how many ran the cascade. The two outcomes
   are counted at the same event — the ask at the QUESTION and the resolution at the answer its one caller
   brings back, with no return between them — so `asks_life == served_life + resolved_life` holds at every
   instant, and it is ASSERTED HERE rather than left to a reader's arithmetic. That assert is the reason this
   accessor is more than a getter: it is the one property of the three that says a shortfall is an arm that
   took an answer without reporting which outcome it was, and the close already asserting it does not help,
   because the close does not run where the product runs.

   AND THEY ARE THIS AGENT'S AND NOT THIS PASS'S, which is flow_placement.h's distinction and is not pedantry
   here either. Both counts are taken BEFORE the storage gate, so a cascade resolved with no pass open is
   counted as a resolution and stores nothing; counting only the stored ones would put the ask in the
   numerator and in neither denominator. A run that opens no pass therefore has `asks_life == resolved_life`
   and `served_life` of zero, which is the correct reading of the shipped path and not an instrument that
   failed to see one.

   `passes_life` IS NOT DECORATION — IT IS WHAT MAKES A ZERO IN `served_life` READABLE AT ALL. That zero has
   two readings and they take opposite work: no pass was ever opened, so this record was never consulted and
   what is missing is a CALLER; or a pass opened and every ask was a genuine first ask, so the keys do not
   repeat and the record buys nothing. One number covering both is the several-states-behind-one-answer shape
   this project refuses, and `passes_life` is the discriminator. It is a COUNT and not a flag because this
   component is empty before a pass opens and empty after it closes, so many spans per run are expected and a
   flag could not say how many there were. It is NOT read out of `_layout`'s own `passes` even though one
   function opens both passes today: that is a fact about one caller rather than about either component, and
   the open question this file records is whether a SECOND, non-paint opener belongs here — the day one lands,
   the two rows legitimately differ and a reader who had been inferring one from the other is reading a number
   about the other component's spans.
   AND THIS ROW'S OWN PRECONDITION IS ASSERTED BESIDE THE IDENTITY RATHER THAN LEFT TO A READER'S ARITHMETIC:
   an answer is served only inside an open pass and a pass is opened only by the call that counts one, so
   `served_life > 0` implies `passes_life > 0`. It is checkable on ONE census, and it is the check that says
   this row may be TRUSTED as the discriminator at all — a second opener, which is the change this file's own
   residual names as the open question, could set the record open without counting a span, and would then
   leave this row at zero while answers were served out of its spans with the identity above still closing. */
typedef struct {
    long long asks_life;
    long long served_life;
    long long resolved_life;
    long long passes_life;
} CssCascadePassCensus;
void css_cascade_pass_census(CssCascadePassCensus *out);

#endif /* ENGINE_HOST_BROWSER_CORE_CSS_CSS_CASCADE_PASS_H */
