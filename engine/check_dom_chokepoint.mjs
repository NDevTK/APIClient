/* THE DOM MUTATION CHOKEPOINT, ENFORCED.
 *
 * dom_cow.h states that a browser component mutates the tree ONLY through the per-flow chokepoints, "enforced
 * structurally by poisoning the raw lxb_dom_* mutators in the component build". That was not true: nothing
 * poisoned anything and nothing checked. A comment claiming an enforcement that does not exist is worse than no
 * claim at all — it is what the next person reads instead of looking, and a raw mutator that slips past writes
 * the shared baseline tree with no entry in the running flow's delta, so a forked arm sees the sibling's write
 * and the unapply cannot put it back.
 *
 * `#pragma GCC poison` was the obvious way and is the wrong one: it errors on the DECLARATION too, so it only
 * works in a file that includes it after every lexbor header, which is per-file discipline — exactly what an
 * enforcement is supposed to remove. A source check has neither problem and reads the whole component.
 *
 * WHAT IS BANNED is the mutating half of the Lexbor DOM API in engine/host/browser. The reading half is the
 * point of using Lexbor and is untouched. dom_cow.c IS the chokepoint and is not part of the component. */
import { readdirSync, readFileSync, statSync } from "node:fs";
import { join, relative } from "node:path";
import { fileURLToPath } from "node:url";

const ENGINE = join(fileURLToPath(import.meta.url), "..");
const ROOT = join(ENGINE, "host", "browser");

/* Every Lexbor entry point that CHANGES the tree, an attribute, or character data — the four things a flow can
   change about the document, which is the same list dom_cow's four entry kinds cover. */
const BANNED = [
  "lxb_dom_node_insert_child",
  "lxb_dom_node_insert_before",
  "lxb_dom_node_insert_after",
  "lxb_dom_node_remove",
  "lxb_dom_node_destroy",
  "lxb_dom_element_set_attribute",
  "lxb_dom_element_remove_attribute",
  "lxb_dom_character_data_replace",
  /* The attribute LIST's own mutators. They were not on this list, so `attr.c` could have appended or destroyed
     an attribute with nothing to say so — a hole the size of the four operations §4.9 is made of. They live in
     exactly one file now, which is why naming them here costs nothing and closes it. */
  "lxb_dom_element_attr_append",
  "lxb_dom_element_attr_remove",
  "lxb_dom_attr_interface_create",
  "lxb_dom_attr_interface_destroy",
  "lxb_dom_attr_set_value",
  "lxb_dom_attr_set_name",
  "lxb_dom_attr_set_name_ns",
];

/* THE ONE FILE BELOW THE CHOKEPOINT. dom_cow.c is not a browser component and is never walked; this one is,
   because §4.9's attribute list is a DOM algorithm and belongs beside the DOM. It is its own file so that the
   exemption is a FILE rather than a set of lines in a component — attr.c beside it stays fully banned. */
const BELOW = "core/dom/attr_list.c";

function sources(dir) {
  const out = [];
  for (const e of readdirSync(dir)) {
    const p = join(dir, e);
    if (statSync(p).isDirectory()) out.push(...sources(p));
    else if (e.endsWith(".c") || e.endsWith(".h")) out.push(p);
  }
  return out;
}

const bad = [];
for (const f of sources(ROOT)) {
  if (f.endsWith(BELOW)) continue;
  const lines = readFileSync(f, "utf8").split("\n");
  for (let i = 0; i < lines.length; i++) {
    /* A COMMENT NAMING THE BAN IS NOT A VIOLATION OF IT. The chokepoint's own documentation says what it
       replaces, and a check that cannot tell the two apart makes the documentation unwritable. */
    const code = lines[i].replace(/\/\*.*?\*\//g, "").replace(/^\s*\*.*$/, "").replace(/\/\/.*$/, "");
    for (const b of BANNED)
      if (new RegExp("\\b" + b + "\\s*\\(").test(code))
        bad.push(relative(ENGINE, f) + ":" + (i + 1) + ": " + b);
  }
}

if (bad.length) {
  console.error("[dom-chokepoint] a browser component calls a raw Lexbor mutator — every tree write goes through\n" +
                "                 dom_cow's chokepoint, or it is invisible to the per-flow delta and a forked arm\n" +
                "                 reads its sibling's write:\n" + bad.map(s => "  " + s).join("\n"));
  process.exit(1);
}
console.log("[dom-chokepoint] ok: " + BANNED.length + " raw Lexbor mutators, 0 reached from the browser component");
