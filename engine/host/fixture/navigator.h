/* THE NAVIGATOR DOCUMENT: forks on the user agent, the screen and injected identifiers.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_NAVIGATOR_H
#define ENGINE_HOST_FIXTURE_NAVIGATOR_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_NAVIGATOR[] =
    "<!doctype html><html>"
    "<body>"
    /* HTML §4.12.1 "The script element"'s DATA BLOCK, WHOSE BYTES ARE A QUERY — the one source whose EXAMPLE
       carries a `&` while its DISPLAY SHAPE cannot: the population `query_align` was widened for.
       The shape is `&`-free by construction: core/loader/data_block.c names a block by its `id` when
       `id_is_spellable` admits it (ASCII alphanumerics plus `_`, `-`, `.`, `$`) and by `script[N]` otherwise,
       so `{script#qrun}` cannot hold the separator, while the example is the block's own bytes. Every other
       unknown fails one half: `navigator.userAgent`'s example has no `&`, `location.hash`/`location.search`
       are empty at `https://x.test/p`, and a string method over a concolic composes its arguments into the
       shape (concolic.h). A served reply's header is concrete (core/fetch/reply_source.h names only the body),
       and a data block needs no delivery at all: the read is synchronous.
       IT ADDS NO PROGRAM AND MOVES NO INDEX: core/loader/document_scripts.c skips a non-executing type in both
       its walks, and the block is named by its `id`. */
    "<script type='application/x-www-form-urlencoded' id='qrun'>a=1&page=2</script>"
    "<script type='application/x-www-form-urlencoded' id='pq'>k/j?x=1</script>"
    "<script>"
    "fetch('/api/u?uid=' + state.id);"   /* concolic query param -> uid carries {state}.id */
    /* …AND THE HOLE WHOSE VALUE IS ITSELF SEVERAL SEGMENTS, which is the half an equal-count aligner cannot
       answer at all. `navigator.userAgent` is concolic with a real Chrome example and that example HOLDS `/`,
       so this address's FIVE shape segments stand over NINE of the example's and the matcher that required
       equal counts answered "not aligned" for the whole address — after which `param_add_val` skipped the
       empty value of every param on it. The address is otherwise the ordinary one, a LITERAL origin region
       with the hole mid-path, so the only thing this statement adds to that one
       is the example's segment count.
       IT SITS AHEAD OF THE UA GATE ON PURPOSE. `navigator.userAgent.indexOf('Chrome')` below forks on this
       same source, so running first is what keeps this row a claim about the ALIGNMENT rather than about
       which arm of that fork reached it. THE GATE IS NAMED AND NOT POINTED AT: this said "the line below"
       until the query-half twin of this statement was written between the two, at which point a sentence
       whose subject was a POSITION resolved to a statement it is not about — and a sweep over the gate's
       own spelling could not have found it. */
    "fetch('/api/ua/' + navigator.userAgent + '/v4');"
    /* …AND THE SAME QUESTION ASKED OF THE OTHER HALF OF THE ADDRESS: a query hole whose value is several
       pairs. The shape holds two `&`-separated pairs (`f={script#qrun}`, `tail=z`) and the example three
       (`f=a=1`, `page=2`, `tail=z`), so an aligner requiring equal counts refuses the whole query and
       `kv_pairs` emits the shape in place of the computed bytes — the product's own `fetch("/s?" + new
       URLSearchParams(f))` spelling, which `query_align`'s banner names.
       THE LITERAL PAIR AFTER THE HOLE MAKES THE READING UNIQUE: the first pair is anchored by the shape's own
       name (`f`) and the last by `tail=z`, so the hole covers `a=1&page=2` and nothing else.
       ITS REACHABILITY IS `path-run`'s: the two are adjacent statements of one program, so `query-run` at NOT
       REACHED beside `path-run` answered is about this statement (DOM §4.4's descendant walk can park). */
    "fetch('/api/qrun?f=' + document.getElementById('qrun').textContent + '&tail=z');"
    /* URL §4.4 "URL parsing" path state's two exits, over the path half. A segment ends at a U+002F (/) the PAGE
       wrote: the division spells `{screen.width}/2`, one unknown whose `/` is the operator's, so the address has
       one hole and not a hole beside a literal `2`. And the path ends at U+003F (?): the data block's example is
       `k/j?x=1`, so its hole stands over two example segments and the query after them, and the shape, which
       has no `?` of its own, puts none of it in a query param. */
    "fetch('/api/pdiv/' + screen.width / 2);"
    "fetch('/api/pend/' + document.getElementById('pq').textContent);"
    "if (navigator.userAgent.indexOf('Chrome') >= 0) { fetch('/api/uafork?v=chrome'); } else { fetch('/api/uafork?v=other'); }"   /* THE UA GATE: navigator.userAgent is concolic with a real Chrome example, so the string method computes on the example AND the comparison forks -> BOTH arms' endpoints are learned */
    "if (navigator.maxTouchPoints > 0) { fetch('/api/touch?v=touch'); } else { fetch('/api/touch?v=mouse'); }"
    "if (screen.width < 768) { fetch('/api/layout?v=mobile'); } else { fetch('/api/layout?v=desktop'); }"
    /* 13.12 Binary Bitwise Operators AND 13.9 Bitwise Shift Operators over UNKNOWN INPUT — the family that had
       no concolic semantics, so `h = (h << 5) ^ h | 0` (a minified bundle's string hash) reached the ToNumber
       boundary and aborted the whole document; three of thirty real signed-out product pages died there.
       `screen.width` is a concolic carrying a real example, so 6.1.6.1.16 NumberBitwiseOp / 6.1.6.1.9
       Number::leftShift / 6.1.6.1.11 Number::unsignedRightShift RUN on that example while the RESULT stays
       unknown — which the branch proves by forking. A collapse to NaN or to a bare number leaves one arm. */
    "var bwm = screen.width & 1023; if (bwm > 100) { fetch('/api/bwfork?v=bwwide'); } else { fetch('/api/bwfork?v=bwnarrow'); }"
    "fetch('/api/bwhash?h=' + (((screen.width << 5) ^ (screen.width >>> 3)) | 0));"
    /* THE OBJECT SIDE ON THE TRAMPOLINE, with a concolic on the other side.
       13.15.3 ApplyStringOrNumericBinaryOperator ( leftValue, opText, rightValue )
       step 3's ToNumeric(leftValue) must NOT be performed at the operator's C boundary when the left
       is unknown (its ToNumeric is step 7's operation, which the hook answers), and step 4's
       ToNumeric(rightValue) is where the PAGE's code runs — 7.1.3 ToNumeric ( arg )'s own step 1 is
       "ToPrimitive(arg, number)", which reaches this object's `valueOf`, and it has a loop in it, so it parks
       on the trampoline and resumes at the exact point. IT IS NOT STEP 1's ToPrimitive: that arm is the `+`
       arm and `&` never enters it, so a reader chasing the literal word "ToPrimitive" through 13.15.3 lands on
       a step this expression does not take. The result is still unknown, so the comparison forks — both arms
       are the claim. */
    "var bwo = { valueOf: function(){ var n = 0; for (var i = 0; i < 500; i++) { n += i; } return 12; } };"
    "fetch('/api/bwtramp?v=' + ((screen.width & bwo) !== 0 ? 'bwand' : 'bwzero'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
