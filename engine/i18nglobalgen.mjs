/* THE NAMES **ECMA-402** OWNS ON THE GLOBAL OBJECT — the third of the vocabularies that answer
 * solver/absent.c's question, derived by the same discipline as the other two and never typed.
 *
 * absent.c asks, of a global name that resolves nowhere, whether it is a component this engine OWES (absent,
 * so the page's ReferenceError names it) or server-injected app state (unknown input, so the gate FORKS). Its
 * suppressions were browser/platform_names.h (Web IDL on Window) and browser/language_names.h (ECMAScript §19),
 * and THREE standards put names on the global object. So `Intl` was in neither table, fell through to app
 * state, and every read of it minted an unknown whose gate forked — measured over this project's committed
 * mirrors at 259 references on 13 of 18 real sites, of which 144 are a bare `new Intl.X`. That is the same
 * defect absent.c's header records twice already: once for the 22-name list that preceded platform_names.h,
 * and once for the ECMAScript vocabulary that was empty until esglobalgen.mjs filled it. One standard further
 * out, and the identical shape — an answer that is a SUPPRESSION over a DEFAULT is wrong about exactly the
 * population its suppression does not cover.
 *
 * THE RULE, STATED SO IT CAN BE CHECKED RATHER THAN TRUSTED, AND IT IS NOT §19's RULE. esglobalgen keys on
 * SUBCLAUSE TITLES because ECMAScript §19 states its global properties as subclauses, one per name. ECMA-402
 * §8 The Intl Object is shaped the other way round: the CLAUSE is the global and its subclauses are that
 * object's MEMBERS. Run esglobalgen's rule over §8 and it yields the EMPTY SET at every depth — at depth 2 the
 * three titles are grouping headings, and at depth 3 they are spelled `Intl.Collator ( . . . )`, which fails
 * the IdentifierName test because of the dot.
 *
 * SO THE NAME IS THE RECEIVER THE MEMBERS SHARE, AND TAKING IT FROM THERE IS WHAT MAKES THIS SAFE BY
 * CONSTRUCTION RATHER THAN BY CARE. The repair that rule invites is to widen the IdentifierName test to admit
 * a dot, and that is the worst of the available outcomes: it would enter `Intl.Collator` in a table of GLOBAL
 * names. Reading the leading IdentifierName IMMEDIATELY followed by a dot and keeping the text BEFORE it
 * cannot produce that, whatever the member list becomes.
 *
 * ITS FAILURE MODE IS MEASURED RATHER THAN CAUTIONED, WHICH IS WHY THE RULE IS SCOPED TO ONE CLAUSE AND NEVER
 * RUN OVER A DOCUMENT. Applied to the whole of the committed ECMAScript index the same rule recovers 37
 * receivers, of which 32 are in language_names.h and the five that are not — TypedArray, NativeError,
 * GeneratorFunction, AsyncFunction, AsyncGeneratorFunction — are exactly the ANONYMOUS intrinsics ECMAScript
 * gives no global name to. Unscoped it admits those; scoped to a clause whose own title declares it to be an
 * object ON the global, it cannot. Applied to the 19.x subtree esglobalgen reads it yields NOTHING, so the two
 * derivations are additive and neither can move a name into or out of the other's table.
 *
 * WHY THIS FETCHES WHERE esglobalgen READS A COMMITTED INDEX, WHICH IS A DIFFERENCE TO CLOSE AND NOT A STYLE.
 * esglobalgen says "No network, no npm — the corpus is the artifact", and it can, because
 * engine/specindex/ecmascript.json is committed for citegen.mjs. There is no ecma402.json, and committing one
 * is not free: citegen's audit() iterates `for (const s of SPECS)` and loads indexes BY KEY — nothing
 * enumerates engine/specindex — so an index earns a regen path only by getting a SPECS row, and a SPECS row
 * makes ECMA-402 a NEIGHBOUR of every indexed standard, whose §8 collides head-on with ECMAScript §8
 * "Syntax-Directed Operations". That collision has to be measured over the WHOLE corpus, which is a different
 * and larger diff. The two costs are SEPARABLE, so this one claims the name and defers the registration, and
 * the fetch is the price of the deferral rather than a rejection of esglobalgen's rule. It is the same fetch
 * citegen's own --regen makes, and it happens only when a person runs this generator: what the BUILD reads is
 * the committed header, exactly as with language_names.h.
 * RETIREMENT: this paragraph and the fetch both go the day ECMA-402 has a SPECS row and a committed index —
 * point INDEX at it, drop curl(), and the two generators become one discipline again.
 *
 * NAMED RESIDUAL — NOTHING CHECKS THAT THE COMMITTED HEADER IS CURRENT. WHAT IS NOT COVERED: this generator
 * runs only when a person runs it, so browser/i18n_names.h can fall behind ECMA-402 with no gate saying so —
 * the same standing as browser/language_names.h, which esglobalgen writes the same way. Only
 * browser/platform_names.h is checked, and only because idlgen runs as a build stage and prints
 * `platform_names.h current — N global names` against `STALE`. WHAT THE NEXT DIFF BUILDS: the same
 * current/STALE line for the two hand-run tables — regenerate into memory, compare against the committed
 * bytes, and print rather than write, which is what makes it a gate a build can carry rather than a step a
 * build has to perform. For this one that means a build stage that FETCHES, so it belongs with the SPECS-row
 * diff that gives ECMA-402 a committed index rather than ahead of it. HOW ITS ABSENCE WOULD SHOW: a reader
 * comparing this header's `Corpus edition` stamp against the document's own <h1 class=version> finds them
 * different, and nothing in any build output has said so.
 * RETIREMENT: goes when a build stage prints a current/STALE line for this table.
 *
 * Run: node engine/i18nglobalgen.mjs   (writes engine/host/browser/i18n_names.h) */
import { writeFileSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const ENGINE = dirname(fileURLToPath(import.meta.url));
const OUT = join(ENGINE, "host", "browser", "i18n_names.h");
/* SINGLE-PAGE, AND THAT IS THE DOCUMENT'S OWN SHAPE RATHER THAN A CHOICE MADE HERE: tc39.es/ecma402/multipage/
   answers 404 where tc39.es/ecma262/multipage/ answers 200, which is why citegen's `tc39-multipage` reader
   cannot be pointed at it and why registering this standard needs a reader kind that does not exist yet. */
const BASE = "https://tc39.es/ecma402/";

/* curl rather than node's fetch, for citegen's reason: curl is what carries this environment's proxy. */
const html = execFileSync("curl", ["-sSL", "--fail", "--max-time", "180", BASE],
                          { encoding: "utf8", maxBuffer: 96 * 1024 * 1024 });

const stamp = /<h1 class=["']?version["']?[^>]*>([^<]+)/.exec(html);
if (!stamp) throw new Error(`${BASE} carries no <h1 class=version> — the page shape has changed and this ` +
                            `header would be stamped with nothing`);

/* number -> title, read off the spec's own section headings. */
const sections = new Map();
for (const m of html.matchAll(/title="([^"]*)"><span class=secnum>([0-9]+(?:\.[0-9]+)*)<\/span>/g))
  sections.set(m[2], m[1]);
if (!sections.size) throw new Error(`${BASE} yielded no numbered sections — the heading shape has changed`);

/* THE CLAUSE THIS DERIVATION IS ABOUT, ASSERTED BY ITS OWN TITLE AND NOT BY ITS NUMBER, exactly as esglobalgen
   asserts §19: a renumber that moved §8 would otherwise silently produce a table of something else's members. */
/* AND THE TITLE ASSERT IS THE LOAD-BEARING ONE, WHICH IS MEASURED RATHER THAN ASSUMED: the receiver check
   below does NOT distinguish this clause from its neighbours. Run the same rule over §10 Collator Objects or
   §11 DateTimeFormat Objects and each also yields `Intl` — from `Intl.Collator.prototype…` member titles —
   so a mis-scoped ROOT would produce the right answer for the wrong reason and go on doing so until the day
   it did not. §9 and §6 yield nothing and would throw. It is the title, not the vote, that says this clause
   is the one putting a name ON the global. */
const ROOT = "8", ROOT_TITLE = "The Intl Object";
if (sections.get(ROOT) !== ROOT_TITLE)
  throw new Error(`ECMA-402 §${ROOT} is "${sections.get(ROOT)}", not "${ROOT_TITLE}" — the standard has ` +
                  `renumbered and this derivation is reading the wrong clause`);

/* The leading IdentifierName IMMEDIATELY followed by a dot, over every subclause of the root. The `.` is
   REQUIRED and is the whole of the safety: `Intl [ %Symbol.toStringTag% ]` is skipped because a SPACE follows
   the identifier, which is what keeps that title's own interior dot out of the answer. */
const RECEIVER = /^([A-Za-z_$][A-Za-z0-9_$]*)\./;
const receivers = new Map();
for (const [num, title] of sections) {
  if (num === ROOT || !num.startsWith(ROOT + ".")) continue;
  const m = RECEIVER.exec(title.trim());
  if (m) receivers.set(m[1], (receivers.get(m[1]) || 0) + 1);
}
if (receivers.size !== 1)
  throw new Error(`ECMA-402 §${ROOT}'s members name ${receivers.size} distinct receivers ` +
                  `(${[...receivers.keys()].join(", ") || "none"}) — this derivation reads the ONE object a ` +
                  `clause puts on the global, and anything else is a shape it must not guess at`);

const [[name, votes]] = [...receivers];
const body = `    ${JSON.stringify(name)},\n`;
writeFileSync(OUT,
`/* GENERATED by engine/i18nglobalgen.mjs from ${BASE} — DO NOT EDIT.
 * Every global property name ECMA-402 §${ROOT} ${ROOT_TITLE} defines. It is derived as the RECEIVER that
 * clause's member subclauses share — they spell themselves \`${name}.Collator ( . . . )\` and the text before
 * the dot is the object they are members OF — never by widening an IdentifierName test to admit the dot,
 * which would enter \`${name}.Collator\` in a table of GLOBAL names. solver/absent.c reads this beside
 * browser/platform_names.h and browser/language_names.h: the three vocabularies that own names on the global
 * object, which is what lets it tell a component this engine OWES from server-injected app state.
 * Derived from: ${BASE}
 * Corpus edition: ${stamp[1].trim()}
 * Members voting for this receiver: ${votes} */
#ifndef APICLIENT_I18N_NAMES_H
#define APICLIENT_I18N_NAMES_H

static const char *const I18N_NAMES[] = {
${body}};

#endif
`);
process.stdout.write(`i18n_names.h: ${name} (${votes} member subclause(s) voting) from ${stamp[1].trim()}\n`);
