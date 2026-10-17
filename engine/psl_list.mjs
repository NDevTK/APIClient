/* The Public Suffix List's text, parsed into the three sorted rule arrays engine/pslgen.mjs generates the engine's
 * table from and testing/psl_agreement.mjs reads a browser's list with. Throws, naming the defect, for a list it
 * cannot vouch for. */
import { domainToASCII } from "node:url";

export function parseList(text) {
  /* §Divisions' TWO SECTIONS, PARSED AND ASSERTED BUT NOT SPLIT — and both halves of that are deliberate.
   *
   * PARSED, because the markers are the only structure the file has: a truncated or half-served download is a
   * list that still parses into rules and quietly answers `com` while missing `github.io`, and the missing END
   * marker is what catches it. A section that never opens or never closes stops the parse.
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
        throw new Error(`§Divisions' markers are not nested: ${marker[1]} ${marker[2]} inside ` +
                      `${section ?? "no section"} — this list is truncated or interleaved, and a rule set built ` +
                      `from it would be missing entries with nothing to say so.`);
      }
      section = opening ? marker[2] : null;
      continue;
    }
    const line = raw.split(/\s/)[0];
    if (!line || line.startsWith("//")) continue;
    if (section === null) {
      throw new Error(`rule ${JSON.stringify(line)} lies outside both of §Divisions' sections`);
    }
    SECTIONS[section]++;
    let kind = normal, body = line;
    if (body.startsWith("!")) { kind = exception; body = body.slice(1); }
    else if (body.startsWith("*.")) { kind = wildcard; body = body.slice(2); }
    /* §Format's INVALID forms, refused rather than mis-stored: a wildcard anywhere but the leftmost whole label
       would be matched here as a literal "*" byte, which no host can ever contain, so the rule would be dead
       weight that reads as coverage. */
    if (body.includes("*") || body.includes("!") || !body) {
      throw new Error(`rule ${JSON.stringify(line)} is not one of the three forms §Format defines`);
    }
    /* The PSL's own canonicalization requirement — lower-case, Punycode — run through the same IDNA the URL
       parser runs, so the two sides of every comparison are one representation. */
    const ascii = domainToASCII(body);
    if (!ascii) {
      throw new Error(`rule ${JSON.stringify(line)} has no A-label form — domain-to-ASCII refused it, so ` +
                    `no host the URL parser produces could ever be compared against it`);
    }
    kind.push(ascii);
  }
  if (section !== null) {
    throw new Error(`§Divisions' ${section} section never closed — the download is truncated`);
  }
  if (!SECTIONS.ICANN || !SECTIONS.PRIVATE) {
    throw new Error(`the list carries ${SECTIONS.ICANN} ICANN and ${SECTIONS.PRIVATE} PRIVATE rules — ` +
                  `HTML §7.1.1.2's own worked table turns on a PRIVATE entry, so a list missing either section ` +
                  `answers that table wrong.`);
  }

  /* SORTED AND DEDUPED, because the lookup is a binary search: public_suffix.c asks "is this exact suffix a rule
     of this kind" three times per label boundary, and a sorted array of strings answers it with no index to keep
     in step with the data. */
  const prep = (a) => [...new Set(a)].sort();
  const N = prep(normal), W = prep(wildcard), E = prep(exception);
  return { N, W, E, sections: SECTIONS };
}
