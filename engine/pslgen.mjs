/* THE PUBLIC SUFFIX LIST, generated from publicsuffix.org's own `public_suffix_list.dat`.
 *
 * WHY THIS EXISTS. HTML §7.1.1.2's `document.domain` setter step 4 asks whether the assigned value "is a
 * registrable domain suffix of or is equal to" the document's effective domain, and two of that algorithm's own
 * steps are stated over a host's PUBLIC SUFFIX (URL §3.2). There is no way to answer them without the list: a
 * hand-written table of TLDs answers `com` and gets `compute.amazonaws.com`, `github.io` and `*.ck` wrong, and
 * the whole point of the condition is those cases — it is what stops `www.example.com` claiming `com`.
 *
 * WHY IT IS VENDORED AND PINNED RATHER THAN FETCHED. §Testing's frozen-snapshot rule: a gate whose INPUT can
 * change under it is not a gate, and this list changes several times a week. A build that downloaded it would
 * make every measurement a fact about the day it ran — the same defect as running a gate from the working tree,
 * one layer out. So it is pinned by the list's own COMMIT line, generated into a committed header, and re-pinned
 * DELIBERATELY as a commit of its own, exactly the way idnagen.mjs pins Unicode and wpt.mjs pins WPT_REV. The
 * build has no network and this table is not optional.
 *
 * WHY THE RULES ARE PUNYCODED HERE AND NOT AT LOOKUP TIME. The list is "Unicode, not Punycode ... encoded using
 * UTF-8", and the PSL's own formal algorithm requires that "the domain, as well as all rules from the Public
 * Suffix List, must be canonicalized in the normal way for hostnames - lower-case, Punycode (RFC 3492) - prior
 * to being compared". A host reaching public_suffix.c has already been through URL §4.2's domain-to-ASCII, so it
 * is an A-label; a rule left as a U-label would then match NOTHING, and it would fail SILENTLY for exactly the
 * IDN domains the condition matters most for. Canonicalizing at generation time makes the two sides one
 * representation, and a rule this generator cannot canonicalize STOPS it rather than being dropped.
 *
 * Usage:  node engine/pslgen.mjs
 */
import { writeFileSync } from "node:fs";
import { join } from "node:path";
import { domainToASCII } from "node:url";

const ENGINE = import.meta.dirname;
const OUT = join(ENGINE, "host", "browser", "core", "url", "public_suffix_table.h");

/* THE PIN. publicsuffix.org serves one URL and no versioned directories ("Please pull this list from, and only
   from https://publicsuffix.org/list/public_suffix_list.dat"), so the pin is the ASSERTION below over the list's
   own COMMIT header — if the list has moved, this generator STOPS instead of silently re-pinning the table under
   a build that was checked against the old one. */
const URL_SRC = "https://publicsuffix.org/list/public_suffix_list.dat";
const COMMIT = "a77cfe0674a4b05c6e2448c01f3cb2c965a1b6d8";

const res = await fetch(URL_SRC);
if (!res.ok) { console.error(`[pslgen] ${URL_SRC}: HTTP ${res.status}`); process.exit(1); }
const text = await res.text();
const commit = /^\/\/\s*COMMIT:\s*(\S+)/m.exec(text);
const version = /^\/\/\s*VERSION:\s*(\S+)/m.exec(text);
if (!commit || !version) {
  console.error("[pslgen] the list carries no VERSION/COMMIT header — that header IS the pin, so a list " +
                "without one cannot be vendored at a revision anyone can reproduce.");
  process.exit(1);
}
if (commit[1] !== COMMIT) {
  console.error(`[pslgen] the list is at ${commit[1]} and this generator is pinned to ${COMMIT} — re-pin ` +
                `deliberately, as a commit of its own, so the diff in results has one cause.`);
  process.exit(1);
}

/* §Divisions' TWO SECTIONS, PARSED AND ASSERTED BUT NOT SPLIT — and both halves of that are deliberate.
 *
 * PARSED, because the markers are the only structure the file has: a truncated or half-served download is a
 * list that still parses into rules and quietly answers `com` while missing `github.io`, and the missing END
 * marker is what catches it. A section that never opens or never closes stops this generator.
 *
 * NOT SPLIT, because the caller is HTML §7.1.1.2 and it is one of the applications §Divisions itself names as
 * treating all entries the same ("some applications, such as browsers when considering cookie-setting, treat
 * all entries the same"). URL §3.2 says "the public suffix determined by running the Public Suffix List
 * algorithm" over the list, with no section argument, and HTML's own worked table for the `document.domain`
 * condition turns on `*.compute.amazonaws.com` — a PRIVATE entry. Emitting the division as per-rule data no
 * caller reads would be a field written here and read nowhere; the counts below are the artifact's PROVENANCE,
 * stated with the commit that produced it. */
const SECTIONS = { ICANN: 0, PRIVATE: 0 };
let section = null;

/* §Format: "Each line is only read up to the first whitespace; entire lines can also be commented using //."
   A rule may begin with "!" (an exception rule) or contain "*" as a whole leftmost label (a wildcard rule). */
const normal = [], wildcard = [], exception = [];
for (const raw of text.split("\n")) {
  const marker = /^\/\/\s*===(BEGIN|END) (ICANN|PRIVATE) DOMAINS===/.exec(raw);
  if (marker) {
    const opening = marker[1] === "BEGIN";
    if (opening === (section !== null)) {
      console.error(`[pslgen] §Divisions' markers are not nested: ${marker[1]} ${marker[2]} inside ` +
                    `${section ?? "no section"} — this list is truncated or interleaved, and a rule set built ` +
                    `from it would be missing entries with nothing to say so.`);
      process.exit(1);
    }
    section = opening ? marker[2] : null;
    continue;
  }
  const line = raw.split(/\s/)[0];
  if (!line || line.startsWith("//")) continue;
  if (section === null) {
    console.error(`[pslgen] rule ${JSON.stringify(line)} lies outside both of §Divisions' sections`);
    process.exit(1);
  }
  SECTIONS[section]++;
  let kind = normal, body = line;
  if (body.startsWith("!")) { kind = exception; body = body.slice(1); }
  else if (body.startsWith("*.")) { kind = wildcard; body = body.slice(2); }
  /* §Format's INVALID forms, refused rather than mis-stored: a wildcard anywhere but the leftmost whole label
     would be matched here as a literal "*" byte, which no host can ever contain, so the rule would be dead
     weight that reads as coverage. */
  if (body.includes("*") || body.includes("!") || !body) {
    console.error(`[pslgen] rule ${JSON.stringify(line)} is not one of the three forms §Format defines`);
    process.exit(1);
  }
  /* The PSL's own canonicalization requirement — lower-case, Punycode — run through the same IDNA the URL
     parser runs, so the two sides of every comparison are one representation. */
  const ascii = domainToASCII(body);
  if (!ascii) {
    console.error(`[pslgen] rule ${JSON.stringify(line)} has no A-label form — domain-to-ASCII refused it, so ` +
                  `no host the URL parser produces could ever be compared against it`);
    process.exit(1);
  }
  kind.push(ascii);
}
if (section !== null) {
  console.error(`[pslgen] §Divisions' ${section} section never closed — the download is truncated`);
  process.exit(1);
}
if (!SECTIONS.ICANN || !SECTIONS.PRIVATE) {
  console.error(`[pslgen] the list carries ${SECTIONS.ICANN} ICANN and ${SECTIONS.PRIVATE} PRIVATE rules — ` +
                `HTML §7.1.1.2's own worked table turns on a PRIVATE entry, so a list missing either section ` +
                `answers that table wrong.`);
  process.exit(1);
}

/* SORTED AND DEDUPED, because the lookup is a binary search: public_suffix.c asks "is this exact suffix a rule
   of this kind" three times per label boundary, and a sorted array of strings answers it with no index to keep
   in step with the data. */
const prep = (a) => [...new Set(a)].sort();
const N = prep(normal), W = prep(wildcard), E = prep(exception);

const emit = (name, rules) => {
  const out = [`static const char *const ${name}[] = {`];
  for (let i = 0; i < rules.length; i += 4)
    out.push("    " + rules.slice(i, i + 4).map((r) => JSON.stringify(r) + ",").join(" "));
  out.push("};");
  return out;
};

const lines = [];
lines.push("/* GENERATED by engine/pslgen.mjs — DO NOT EDIT. Public Suffix List " + version[1] + ",");
lines.push(" * commit " + commit[1] + ", from " + URL_SRC + " —");
lines.push(` * ${SECTIONS.ICANN} rules in §Divisions' ICANN section and ${SECTIONS.PRIVATE} in its PRIVATE` +
           " section, unified here");
lines.push(" * because URL §3.2 runs the algorithm over the list and HTML §7.1.1.2's own table turns on a");
lines.push(" * PRIVATE entry (`*.compute.amazonaws.com`).");
lines.push(" *");
lines.push(" * §Format's three rule shapes, each as its own SORTED array of the rule's matchable BODY — the");
lines.push(" * leading `!` of an exception rule and the leading `*.` of a wildcard rule are the rule's KIND and");
lines.push(" * are carried by which array it is in, so a lookup never has to re-parse a rule at run time. The");
lines.push(" * bodies are A-labels: the PSL's formal algorithm requires both sides of a comparison to be");
lines.push(" * lower-case Punycode, and the host side arrives that way from URL §4.2's domain-to-ASCII.");
lines.push(" *");
lines.push(" * Re-pin DELIBERATELY, as a commit of its own — see engine/pslgen.mjs. */");
lines.push("#ifndef ENGINE_HOST_BROWSER_CORE_URL_PUBLIC_SUFFIX_TABLE_H");
lines.push("#define ENGINE_HOST_BROWSER_CORE_URL_PUBLIC_SUFFIX_TABLE_H");
lines.push("");
lines.push(`#define PSL_VERSION "${version[1]}"`);
lines.push(`#define PSL_COMMIT  "${commit[1]}"`);
lines.push("");
lines.push("/* An ordinary rule: it matches a domain whose right-most labels are exactly these. */");
lines.push(...emit("PSL_NORMAL", N));
lines.push("");
lines.push("/* A wildcard rule `*.BODY`: it matches a domain whose right-most labels are BODY preceded by ONE");
lines.push("   more label of any content — §Format restricts the wildcard to the leftmost position and to a");
lines.push("   whole label, which is why the body alone is enough to express it. */");
lines.push(...emit("PSL_WILDCARD", W));
lines.push("");
lines.push("/* An exception rule `!BODY`: it matches the same domains an ordinary rule with this body would, and");
lines.push("   §Algorithm gives it priority over every other matching rule. */");
lines.push(...emit("PSL_EXCEPTION", E));
lines.push("");
lines.push("#endif");
writeFileSync(OUT, lines.join("\n") + "\n");
console.log(`[pslgen] PSL ${version[1]} (${commit[1]}): ${N.length} ordinary + ${W.length} wildcard + ` +
            `${E.length} exception rules -> ` + OUT.replace(ENGINE + "/", "engine/"));
