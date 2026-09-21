// THE BUNDLE HALF of gated-hole.html — the one document in this control where a value the page GATES is also
// a value that reaches a request AS A HOLE-CARRYING PARAM. Every other channel here keeps those two
// populations apart, which is why site.mjs's four domain columns have never been able to speak.
//
// THE POPULATIONS ARE DISJOINT EVERYWHERE ELSE AND IT IS NOT AN ACCIDENT OF STYLE. injected-state.html gates
// `admin` and `beta` and sends each to a CONSTANT address, so those gates mint no param and leave no hole to
// look a domain up under; the only members of that record reaching an address as a hole (`statusCode`) are
// members it never gates. A domain is FILED under a hole by solver/decide.c and READ BACK under that same
// hole by solver/endpoint.c's `kv_add`, so where the gated set and the hole-carrying set do not intersect,
// `withExcl`/`withBnd`/`withPred`/`withLeq` cannot rise however well the engine narrows. A zero there was
// therefore an unarmed control -- CLAUDE.md's rule that a control which has never produced a finding is not
// a control -- and this file is what makes a zero a statement about the engine again.
//
// ONE RUNG PER RECORDER, BECAUSE FOUR COLUMNS SUMMED INTO ONE SUBJECT CANNOT SAY WHICH ONE FAILED. Each
// member below is gated by exactly one of the four shapes solver/decide.c's `decide_branch` records, and
// each is then spliced into a path so its hole carries that recorder's key:
//   region  §7.2.11-shaped CALL predicate  -> concolic_strpred_file, BOTH arms -> `_predicates`     -> withPred
//   tier    §7.2.15 strict equality, FALSE -> concolic_exclude, the false arm  -> `_excludedValues` -> withExcl
//   rank    §7.2.12 ordering, BOTH arms    -> concolic_bound                   -> `_bounds`         -> withBnd
//   code    §7.2.13 loose equality, HOLDS  -> concolic_looseeq, holding arm    -> `_looselyEquals`  -> withLeq
//
// WHICH ARM CARRIES THE FETCH IS THE WHOLE OF WHETHER A RUNG ARMS, AND IT DIFFERS PER RECORDER. A strict
// equality's TRUE arm PINS its operand (CLAUDE.md §CONCRETIZE-ON-PIN), so a later read returns the pinned
// bytes and the address holds a CONCRETE segment with no hole to read a domain out of -- so the `tier` rung
// puts its fetch on the `else`, which is the arm `concolic_exclude` is about. A LOOSE equality's holding arm
// determines no single value, which is precisely why decide.c files it instead of pinning it, so `code` can
// fetch on the `if`. Ordering and call predicates record on both arms and fetch on both.
//
// AND `zone` IS THIS DOCUMENT'S OWN NEGATIVE CONTROL, which is what makes a nonzero reading mean anything: it
// is spliced into a path exactly like the four above and is GATED BY NOTHING, so it must appear as a param
// whose `_astValueClass` is "unknown" carrying no domain key at all. The pair is the reading:
//   four gated members carrying a hole AND a domain, `zone` carrying a hole and NO domain  -> armed, engine narrows
//   all five carrying a hole and none carrying a domain                                    -> a finding about the engine
//   `zone` carrying a domain                                                               -> a finding about the RECORDER,
//                                                                                             which has filed a fact no
//                                                                                             predicate of this page observed
// HOW ITS ABSENCE WOULD SHOW: read site.mjs's `domains` on this origin -- `astHoleParams` at 5 with all four
// domain columns at 0 is the armed-and-silent reading, and it is the one that was indistinguishable from an
// unarmed control before this file existed.
var s = window.__CTL_GATED__;

/* A CALL PREDICATE OVER A SYMBOLIC STRING. solver/concolic.c records the SUBJECT, the METHOD NAME and the
   ARGS without asking what the method means, so what is observed is "this flow took the arm on which
   region.startsWith('us-') is true" and never a value. Both arms fetch because both are recorded. */
if (s.region.startsWith('us-')) fetch('/api/' + s.region + '/pred-us');
else                           fetch('/api/' + s.region + '/pred-other');

/* THE EQUALITY'S FALSE ARM, WHICH IS THE ONLY ARM THAT LEAVES A HOLE. The true arm pins `tier` to the token,
   so its address is a concrete segment and mints no hole -- it is here, and deliberately constant, so a run
   that took only that arm is visibly different from a run that took neither. */
if (s.tier === 'silver') fetch('/api/tier-silver');
else                     fetch('/api/' + s.tier + '/excl-not-silver');

/* AN ORDERING, RECORDED ON BOTH ARMS as an interval rather than a value: `> 3` on one arm and `<= 3` on the
   other are each a fact this flow observed, and neither determines `rank`. */
if (s.rank > 3) fetch('/api/' + s.rank + '/bound-gt3');
else            fetch('/api/' + s.rank + '/bound-le3');

/* LOOSE EQUALITY, HOLDING ARM. §7.2.13 admits more than one value -- `200` and `"200"` both hold -- so this
   is a narrowing and not a pin, and the operand is still a hole on the arm that took it. */
if (s.code == 200) fetch('/api/' + s.code + '/leq-200');

/* THE NEGATIVE CONTROL -- spliced identically, gated by nothing. See the banner. */
fetch('/api/' + s.zone + '/ungated');
