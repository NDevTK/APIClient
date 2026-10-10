/* THE SORT DOCUMENT: Array.prototype.sort and %TypedArray%.prototype.sort's comparator step machine.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SORT_H
#define ENGINE_HOST_FIXTURE_SORT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SORT[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    /* SORT'S COMPARISON OVER UNKNOWN EXTERNAL INPUT — the ONE branch a builtin makes ON ITS OWN operand that
       must NOT fork. §23.1.3.30.2 CompareArrayElements ( x, y, comparator ) step 4.a coerces what the
       comparator returned, and when that answer is unknown input the ordering it states is meaningless: the
       comparison collapses to the +0 step 4.b already gives a NaN, and §23.1.3.30.1 SortIndexedProperties
       ( obj, length, sortCompare, holes )'s stability condition turns that into the identity permutation. The
       PROBE IS THAT THE ELEMENTS SURVIVE AS THEMSELVES: `sc0sc1-2` can only be read off a real Array of two
       real objects, so it is false both when the merge aborts and when the machine's completion is replaced
       by a derived unknown (which has no `[0]`, no `.u` and no `.length` the page ever computed). */
    "var _sc=[{u:'sc0'},{u:'sc1'}].sort(function(a,b){ return state.rank; }); fetch('/api/sortcollapse?v=' + _sc[0].u + _sc[1].u + '-' + _sc.length);"
    /* THE SECOND WAY IN, which a pre-check on the comparator's result cannot see: an ORDINARY object whose
       valueOf answers with unknown input, so §7.1.1 ToPrimitive is what discovers it, one hop after any
       look-before-you-coerce would have run. Same collapse, reached from the coercion's own report. */
    "var _sv=[{u:'sv0'},{u:'sv1'}].sort(function(a,b){ return { valueOf: function(){ return state.rank; } }; }); fetch('/api/sortvalueof?v=' + _sv[0].u + _sv[1].u);"
    /* A REST PARAMETER, which is the ONE argument shape that can see a callback frame borrowing slots it does
       not own: every other spelling takes the identical path and cannot find it. `-2` is the rest array having
       received both of CompareArrayElements' operands. */
    "var _srn=-1; var _sr=[{u:'sr0'},{u:'sr1'}].sort(function(...r){ _srn=r.length; return state.rank; }); fetch('/api/sortrest?v=' + _sr[0].u + _sr[1].u + '-' + _srn);"
    /* THE DEFAULT ORDERING'S HALF OF THE SAME QUESTION — no comparator, so §23.1.3.30.2 steps 5-6 ToString both
       operands and an unknown element has no String for steps 7-10 to test. The two concrete elements must
       still be ORDERED against each other (sd1 before sd2), which is what makes this different from suppressing
       the comparison altogether, and the unknown must still be an element (length 3). */
    "var _sd=['sd2','sd1',state.rank].sort(); fetch('/api/sortdefault?v=' + _sd[0] + _sd[1] + '-' + _sd.length);"
    /* AND THE TYPED-ARRAY SORT, whose elements are all real Numbers the engine read out of the buffer — so a
       completion replaced by a derived unknown costs a list of concrete values and gains nothing.
       §23.2.4.8 CompareTypedArrayElements ( x, y, comparator ) step 2.b is the same +0. `same` is the receiver
       coming back (a derived unknown is a different object), and the three digits are the write-back having
       run at all. */
    "var _ta=new Int32Array([3,1,2]); var _tr=_ta.sort(function(a,b){ return state.rank; }); fetch('/api/sortta?v=' + (_tr===_ta?'same':'other') + '-' + _tr.length + '-' + _ta[0] + _ta[1] + _ta[2]);"
    /* THREE CONTROLS, IDENTICAL ON BOTH SIDES OF THAT CHANGE. (1) NaN takes the very predicate the collapse
       now leaves through, so a collapse written as its own branch instead of as `v = 0` would show here first.
       (2) The default ordering with nothing unknown in it. (3) THE FORK THAT MUST SURVIVE: the branch is INSIDE
       the comparator's body, not over its result, so both orderings must still be reached — removing that one
       is how a collapse turns into the arm-pruning it is supposed to avoid. */
    "var _cn=['n1','n2','n3','n4'].sort(function(){ return NaN; }); fetch('/api/sortnan?v=' + _cn.join(''));"
    "var _cd=['cdb','cda','cdc'].sort(); fetch('/api/sortdefctl?v=' + _cd.join(''));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
