/* THE FORMS DOCUMENT: HTML §4.10's legacy factory functions and form-control state.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FORMS_H
#define ENGINE_HOST_FIXTURE_FORMS_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FORMS[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    /* ─── Web IDL §3.7.2 "Legacy factory functions", AND §3.6 "Overload resolution algorithm"'s "missing" ─────
     * `Audio` (HTML §4.8.9 "The audio element", five top-level steps), `Image` (§4.8.3 "The img element",
     * five) and `Option` (§4.10.10 "The option element", seven) are the only `[LegacyFactoryFunction]`s HTML
     * declares; these statements are their falsifiers, executed.
     * EACH STATEMENT EMITS ONE LETTER PER CLAUSE, failing ones UPPERCASED: one address per algorithm keeps a
     * row's 0 at a spec section, and the letter localises it to a step.
     * AN ABSENT NAME IS NOT A WRONG ANSWER: every block is guarded by `typeof` and a failed guard emits
     * `absent`, since `Audio.length` on an engine with no `Audio` is a ReferenceError that would take the whole
     * statement with it and read as the schedule.
     * NOTHING HERE FORKS: every predicate is over objects built from literals. */

    /* Web IDL §3.7.2's FUNCTION OBJECT, asked once per factory through one page-level helper so that the
       ADDRESS names which factory and the letters name which clause. §3.7.2's own steps: `length` is "the
       length of the shortest argument list of the entries in S" and every position of all three factories is
       optional, so it is 0; `prototype` is "! DefinePropertyOrThrow(F, "prototype", PropertyDescriptor{
       [[Value]]: proto, [[Writable]]: false, [[Enumerable]]: false, [[Configurable]]: false})"; and step 1 of
       the function's steps is "If NewTarget is undefined, then throw a TypeError", which is the plain call.
       THE `constructor` CLAUSE IS THE ONE THAT SAYS THIS IS NOT AN INTERFACE OBJECT: §3.7.2 defines no
       `constructor` back-pointer, so `HTMLAudioElement.prototype.constructor` must still be
       `HTMLAudioElement` — a JS_SetConstructor here would answer `Audio` and nothing else would notice. */
    "function lfaFn(F, I) { var d = Object.getOwnPropertyDescriptor(F, 'prototype'), r = '';"
    " r += (F.length === 0 ? 'l' : 'L');"
    " r += (F.prototype === I.prototype ? 'p' : 'P');"
    " r += (I.prototype.constructor === I ? 'c' : 'C');"
    " r += (d && d.writable === false && d.enumerable === false && d.configurable === false ? 'd' : 'D');"
    " try { F(); r += 'N'; } catch (e) { r += (e instanceof TypeError ? 't' : 'E'); }"
    " return r; }"
    "var lfaFa = (typeof Audio === 'undefined' || typeof HTMLAudioElement === 'undefined') ? 'absent'"
    " : lfaFn(Audio, HTMLAudioElement);"
    "fetch('/api/lfafnaud?v=' + (lfaFa === 'lpcdt' ? 'isfnaud' : 'fa' + lfaFa));"
    "var lfaFi = (typeof Image === 'undefined' || typeof HTMLImageElement === 'undefined') ? 'absent'"
    " : lfaFn(Image, HTMLImageElement);"
    "fetch('/api/lfafnimg?v=' + (lfaFi === 'lpcdt' ? 'isfnimg' : 'fi' + lfaFi));"
    "var lfaFo = (typeof Option === 'undefined' || typeof HTMLOptionElement === 'undefined') ? 'absent'"
    " : lfaFn(Option, HTMLOptionElement);"
    "fetch('/api/lfafnopt?v=' + (lfaFo === 'lpcdt' ? 'isfnopt' : 'fo' + lfaFo));"

    /* HTML §4.8.9 "The audio element"'s five top-level steps, counted with list depth tracked against the
       standard's own <ol> so a nested item cannot be promoted to a peer. STEP 3 IS UNCONDITIONAL and is the
       whole reason this is not `createElement('audio')` — every element the factory makes carries
       `preload="auto"`, which §4.8.11 "Media elements" reads as the strongest hint an element can carry. STEP 4
       is "If src is given", and `new Audio(undefined)` is §3.6 step 15.4.2's "missing" rather than a `src` of
       the four characters `undefined`: an `argc` test answers "given" for exactly that call, which is the
       defect the byte-equality clause below refuses. Both `preload` readings are asserted because they are two
       questions — `a` is step 3's attribute and `p` is §4.8.11's reflection of it, and an engine missing the
       member would otherwise redden a row about step 3. */
    "var lfaAu = 'absent';"
    "if (typeof Audio !== 'undefined' && typeof HTMLAudioElement !== 'undefined') {"
    " var lau0 = new Audio(), lau1 = new Audio('a.mp3'), lau2 = new Audio(undefined);"
    " lfaAu = (lau0 instanceof HTMLAudioElement ? 'i' : 'I')"
    "  + (lau0.getAttribute('preload') === 'auto' ? 'a' : 'A')"
    "  + (lau0.preload === 'auto' ? 'p' : 'P')"
    "  + (lau0.hasAttribute('src') ? 'S' : 's')"
    "  + (lau1.getAttribute('src') === 'a.mp3' ? 'r' : 'R')"
    "  + (lau0.outerHTML === '<audio preload=\"auto\"></audio>' ? 'h' : 'H')"
    "  + (lau2.outerHTML === lau0.outerHTML ? 'm' : 'M'); }"
    "fetch('/api/lfaudio?v=' + (lfaAu === 'iapsrhm' ? 'islfaudio' : 'au' + lfaAu));"

    /* HTML §4.8.3 "The img element"'s five top-level steps and the same §3.6 pair at two positions.
       `new Image(undefined).outerHTML` was `<img width="undefined">` because the body read `argc` — how far the
       page REACHED — where §3.6 asks which kind of entry step 9 put in `values`; the two readings agree for
       `new Image()` and disagree for exactly this call, so the BYTE-EQUALITY of the two is the clause that
       carries the fix. `im2` is the mixed call the count cannot express at all: position 0 given, position 1
       reached and `undefined`, which is one attribute and not two. */
    "var lfaIm = 'absent';"
    "if (typeof Image !== 'undefined' && typeof HTMLImageElement !== 'undefined') {"
    " var lim0 = new Image(), lim1 = new Image(undefined), lim2 = new Image(5, undefined),"
    "     lim3 = new Image(5, 6);"
    " lfaIm = (lim0 instanceof HTMLImageElement ? 'i' : 'I')"
    "  + (lim0.outerHTML === '<img>' ? 'n' : 'N')"
    "  + (lim1.outerHTML === lim0.outerHTML ? 'm' : 'M')"
    "  + (lim2.outerHTML === '<img width=\"5\">' ? 'w' : 'W')"
    "  + (lim3.outerHTML === '<img width=\"5\" height=\"6\">' ? 'h' : 'H'); }"
    "fetch('/api/lfaimage?v=' + (lfaIm === 'inmwh' ? 'islfaimage' : 'im' + lfaIm));"

    /* HTML §4.10.10 "The option element"'s SEVEN top-level steps — depth-tracked; none of the seven holds a
       nested list, so these sub-numbers are the standard's own and are unambiguous. Step 3's test is "If text
       is not the empty string", so `new Option()` has NO child node at all, which `childNodes.length` sees.
       Step 4 is "If value is given" and `new Option('a', undefined)` is §3.6's "missing", so a `value="undefined"`
       refutes it. AND STEP 6 IS WHY THE COMPONENT EXISTS: it sets selectedness EXPLICITLY, after step 5 has
       added the very attribute whose change steps set it — "If selected is true, then set option's selectedness
       to true; otherwise set its selectedness to false (even if defaultSelected is true)" — which the standard
       restates in its own note: "The Option() constructor, when called with three or fewer arguments, overrides
       the initial state of the selectedness state to always be false even if the third argument is true". So
       `new Option('a','b',true)` SERIALIZES with the attribute and ANSWERS `selected === false`, and the two
       clauses are held apart because the content attribute alone cannot say the second. */
    "var lfaOp = 'absent';"
    "if (typeof Option !== 'undefined' && typeof HTMLOptionElement !== 'undefined') {"
    " var lop0 = new Option(), lop1 = new Option('a', 'b'), lop2 = new Option('a', undefined),"
    "     lop3 = new Option('a', 'b', true), lop4 = new Option('a', 'b', true, true);"
    " lfaOp = (lop0 instanceof HTMLOptionElement ? 'i' : 'I')"
    "  + (lop0.childNodes.length === 0 ? 'e' : 'E')"
    "  + (lop1.outerHTML === '<option value=\"b\">a</option>' ? 'v' : 'V')"
    "  + (lop2.outerHTML === '<option>a</option>' ? 'u' : 'U')"
    "  + (lop3.defaultSelected === true ? 'd' : 'D')"
    "  + (lop3.hasAttribute('selected') ? 'a' : 'A')"
    "  + (lop3.selected === false ? 's' : 'S')"
    "  + (lop4.selected === true ? 'f' : 'F'); }"
    "fetch('/api/lfaoption?v=' + (lfaOp === 'ievudasf' ? 'islfaoption' : 'op' + lfaOp));"

    /* §4.10.10's DIRTINESS — the state the content attribute cannot express, and the one the factory
       deliberately does NOT set. The attribute change steps are conditioned on it ("if its dirtiness is false,
       its selectedness must be set to false"), so a CLEAN option tracks `selected` at every add and remove and
       a DIRTY one stops; the `selected` SETTER is what makes an option dirty ("set the element's selectedness
       to the new value, set its dirtiness to true, and then cause the element to ask for a reset") and it
       writes NO attribute, which `B` asserts as an absence.
       BOTH HALVES ARE HERE BECAUSE EITHER ALONE IS UNFALSIFIABLE: an engine whose selectedness never tracked
       the attribute at all would satisfy `c` — the dirty option keeping its selectedness across an add and a
       remove — while being wrong about everything, and `d`/`e` are the clean option that refuses that. */
    "var lfaDy = 'absent';"
    "if (typeof Option !== 'undefined') {"
    " var lod = new Option(); lod.selected = true;"
    " lfaDy = (lod.selected === true ? 'a' : 'A') + (lod.hasAttribute('selected') ? 'B' : 'b');"
    " lod.setAttribute('selected', ''); lod.removeAttribute('selected');"
    " lfaDy += (lod.selected === true ? 'c' : 'C');"
    " var loc = new Option(); loc.setAttribute('selected', '');"
    " lfaDy += (loc.selected === true ? 'd' : 'D');"
    " loc.removeAttribute('selected');"
    " lfaDy += (loc.selected === false ? 'e' : 'E'); }"
    "fetch('/api/lfadirty?v=' + (lfaDy === 'abcde' ? 'islfadirty' : 'dy' + lfaDy));"

    /* §4.10.7 "The select element"'s SELECTEDNESS SETTING ALGORITHM, read through §4.10.10's `selected` getter
       — which runs it, because this engine's spelling of the standard's invocation points is to run it at the
       read. Step 1: "If element's multiple attribute is absent, and element's display size is 1, and no option
       elements in the element's list of options have their selectedness set to true, then set the selectedness
       of the first option element in the list of options in tree order that is not disabled, if any, to true",
       which is why `q` asserts that no `selected` ATTRIBUTE appeared anywhere while `p` is true. Step 2: "and
       two or more option elements … have their selectedness set to true, then set the selectedness of all but
       the last … to false", which is why `opts[1].selected = true` must leave `opts[0].selected` false.
       THE SELECT IS DETACHED ON PURPOSE: html_form_select_of_option walks the option's own ancestors, so this
       states §4.10.7 without adding a widget to the document the layout walk then has to have a box for —
       which is a different component's claim and would fold two spec sections into one row's 0. */
    "var lfaSe = 'absent';"
    "if (typeof Option !== 'undefined') {"
    " var lsl = document.createElement('select');"
    " var lsA = new Option('a'), lsB = new Option('b');"
    " lsl.appendChild(lsA); lsl.appendChild(lsB);"
    " lfaSe = (lsA.selected === true ? 'p' : 'P') + (lsA.hasAttribute('selected') ? 'Q' : 'q');"
    " lsB.selected = true;"
    " lfaSe += (lsB.selected === true ? 'r' : 'R') + (lsA.selected === false ? 's' : 'S'); }"
    "fetch('/api/lfareset?v=' + (lfaSe === 'pqrs' ? 'islfareset' : 'se' + lfaSe));"

    /* …AND THE SAME TWO STEPS AS AN ENTRY LIST, which is the half a member read cannot state. §4.10.22.4 step
       5.6 emits one entry per option whose selectedness is true and that is not disabled, so step 1's answer IS
       `sc=a` for a select nothing selected, and step 2's is `sd=b` ALONE for one where the factory made both
       options selected. `submit()` DERIVES the request without sending it, and a form must be CONNECTED to submit — which is why these two are appended where the
       `/api/lfareset` select is not. An option with no `value` attribute submits its collected text, which is
       what makes `a` and `b` readable as values at all. */
    "if (typeof Option !== 'undefined') {"
    " var lsf = document.createElement('form'); lsf.action = '/api/lfasel1';"
    " var ls1 = document.createElement('select'); ls1.name = 'sc';"
    " ls1.appendChild(new Option('a')); ls1.appendChild(new Option('b'));"
    " lsf.appendChild(ls1); document.body.appendChild(lsf); lsf.submit();"
    " var lsg = document.createElement('form'); lsg.action = '/api/lfasel2';"
    " var ls2 = document.createElement('select'); ls2.name = 'sd';"
    " ls2.appendChild(new Option('a', 'a', true, true));"
    " ls2.appendChild(new Option('b', 'b', true, true));"
    " lsg.appendChild(ls2); document.body.appendChild(lsg); lsg.submit(); }"

    /* THE CONTROL, AND IT IS A CONTROL BECAUSE ITS ANSWERS MUST NOT HAVE MOVED. Web IDL §3.6's OTHER arm is a
       position DECLARED WITH A DEFAULT, where steps 15.4.1 and 16.1 both append that value and 15.4.2's
       "missing" is never reached: `encode(optional USVString input = "")` and `constructor(optional DOMString
       label = "utf-8", …)`. The honest falsifier here is an EQUALITY rather than a difference — passing
       `undefined` and passing nothing must answer THE SAME — because the failure this guards against is the
       "missing" primitive being mistaken for a rule about every optional position, at which point these four
       start disagreeing and no row above would say so. */
    "var lfaTd = 'absent';"
    "if (typeof TextEncoder !== 'undefined' && typeof TextDecoder !== 'undefined') {"
    " var lte = new TextEncoder();"
    " lfaTd = (lte.encode().length === 0 ? 'a' : 'A')"
    "  + (lte.encode(undefined).length === 0 ? 'b' : 'B')"
    "  + (new TextDecoder().encoding === 'utf-8' ? 'c' : 'C')"
    "  + (new TextDecoder(undefined).encoding === 'utf-8' ? 'd' : 'D'); }"
    "fetch('/api/lfadefault?v=' + (lfaTd === 'abcd' ? 'islfadefault' : 'td' + lfaTd));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
