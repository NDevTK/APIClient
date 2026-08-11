# SPEC_STEPS.md — the numbered steps of the DOM/HTML/Web IDL algorithms, and exactly where the page's own code can run

**What this file is.** A step machine in this engine rests at an EXACT numbered spec step
(`JSTrampStepDef.algorithm` / `.steps[]` in `engine/qjs/quickjs-step.h`; the worked example is
`js_regexp_exec_steps[]` in `engine/qjs/quickjs.c`). A stage may not SPAN two steps between which
the page's own code can run — that would be a suspension the engine cannot express. This document
is the answer to "where are those points" for the seven algorithm groups the conversion takes next,
**in conversion order**.

**What this file is NOT.** It does not describe what this codebase does today. It describes what the
standards say. Someone else is diffing the two.

**Network was available.** Everything below was read from the live standards on 2026-08-09:

| Standard | Source | Version read |
| --- | --- | --- |
| WHATWG DOM | `https://dom.spec.whatwg.org/` | Living Standard, Last Updated **18 July 2026** |
| WHATWG HTML | `https://html.spec.whatwg.org/multipage/` | Living Standard, Last Updated **20 July 2026** |
| Web IDL | `https://webidl.spec.whatwg.org/` | Living Standard, Last Updated **3 July 2026** |
| CSSOM View | `https://drafts.csswg.org/cssom-view/` | Editor's Draft, read 2026-08-09 |
| IntersectionObserver | `https://w3c.github.io/IntersectionObserver/` | Editor's Draft, read 2026-08-09 |
| Resize Observer | `https://drafts.csswg.org/resize-observer/` | Editor's Draft, read 2026-08-09 |
| CSS View Transitions 1 | `https://drafts.csswg.org/css-view-transitions-1/` | Editor's Draft, read 2026-08-09 |
| Web Animations 1 | `https://drafts.csswg.org/web-animations-1/` | Editor's Draft, read 2026-08-09 |
| Trusted Types | `https://w3c.github.io/trusted-types/dist/spec/` | Editor's Draft, read 2026-08-09 |

Step numbers below are the standards' own numbering as of those dates. Where a step is a substep it
is written `7.7.3.2` meaning step 7 → substep 7 → substep 3 → substep 2.

---

## 0. Reading conventions, and three cross-cutting mechanisms

### 0.1 Marks

- **`[S]`** — a **SUSPENSION POINT**: at this step control can pass to code the page supplied
  (a getter/setter, a callback, `toString`/`valueOf`/`Symbol.toPrimitive`, a custom-element
  reaction, a Proxy trap, a promise reaction, or an event listener). **A stage boundary must exist
  here.**
- **`(walk)`** — no user code, but the step is **O(page size)** (a tree walk, a parse, a
  serialization). `quickjs-step.h`'s `JS_STEP_YIELD` exists for exactly this: "Running no user code
  is NOT what makes a C body safe to leave un-parkable; being O(1) is." These are not suspension
  points but they are yield points, and they are marked because the conversion needs both.
- **`(enqueue)`** — the step appends to a queue (custom element reaction queue, mutation record
  queue, microtask queue, task queue). **No user code runs here.** The queue is drained somewhere
  else, and that somewhere else is a suspension point. Confusing these two is the single most common
  error in reading these algorithms: `attributeChangedCallback` is *enqueued* by
  `handle attribute changes` and *invoked* by the `[CEReactions]` wrapper.
- unmarked — no user code, O(1) or bounded by the algorithm's own operands.

### 0.2 The Web IDL prologue runs BEFORE step 1 of every algorithm

Every algorithm below is entered through a Web IDL operation or attribute, and Web IDL §3.7.7
converts the arguments **before** the algorithm's step 1 runs. `DOMString qualifiedName` is
`ToString(V)` — the page's `Symbol.toPrimitive`/`toString`/`valueOf`. `optional
ElementCreationOptions options` is a dictionary conversion — a `[[Get]]` per member, which on a
Proxy is the `get` trap. So **the first suspension point of every one of these algorithms precedes
its own step 1**, and a machine whose stage 0 is "the prologue has not run" (which is what
`JSStepHdr.stage == 0` means) must place the argument conversions in their own stages. Algorithm 7
below is the full statement of this; algorithms 1–6 name their prologue but do not re-derive it.

### 0.3 `[CEReactions]` — HTML §4.13.6 "Custom element reactions"

Every operation/attribute-setter annotated `[CEReactions]` **runs different steps from the ones
written in its own definition**. HTML §4.13.6 replaces them with:

1. Push a new element queue onto the relevant agent's custom element reactions stack. —
2. Run the originally-specified steps, catching exceptions. — *(this is where the algorithm's own
   text, as listed below, executes; its own suspension points are inside here)*
3. Pop that queue. —
4. **`[S]`** Invoke custom element reactions in queue. — **author code: constructors and lifecycle
   callbacks.**
5. Rethrow the exception if the original steps threw. —
6. Return the value if the original steps returned one. —

`invoke custom element reactions in an element queue` (HTML §4.13.6) is itself:

1. While queue is not empty:
   1. Dequeue element. — *(walk)*
   2. Let reactions be element's custom element reaction queue. —
   3. Repeat until reactions is empty:
      1. Remove the first reaction and switch on its type:
         - **upgrade reaction → `[S]`** Upgrade element (HTML §4.13.5) — runs the author's
           constructor. On throw: **`[S]`** report it for the constructor's realm's global
           (HTML §8.1.4.6 "report an exception" **fires an `error` event at the global**, so the
           page's `onerror` runs).
         - **callback reaction → `[S]`** Invoke the callback function with its arguments and
           "report", `this` = element.

**`enqueue an element on the appropriate element queue`** (HTML §4.13.6) has the branch that matters
for anything *not* reached through a `[CEReactions]` API: if the reactions stack is **empty**, the
element goes on the **backup element queue** and a **microtask** is queued to invoke it. So a DOM
mutation performed from C, from the parser, or from an editing operation still reaches author code —
just at the next microtask checkpoint rather than at the API's return.

`upgrade an element` (HTML §4.13.5), for reference, since three algorithms below delegate to it:

1. Early-exit if state is not "undefined"/"uncustomized". —
2. Set custom element definition. —
3. Set custom element state to "failed". —
4. For each attribute, **(enqueue)** `attributeChangedCallback`. —
5. If connected, **(enqueue)** `connectedCallback`. —
6. Push element onto definition's construction stack. —
7. Let C be definition's constructor. —
8–9. Save/replace the agent's active custom element constructor map[C]. —
10. Catching exceptions:
    1. `disable shadow` check → throw NotSupportedError. —
    2. Set state to "precustomized". —
    3. **`[S]`** `constructResult` = **construct C, no arguments** — *the author's constructor*.
       Note the spec's own note: if C non-conformantly calls a `[CEReactions]` API, the reactions
       enqueued at steps 4–5 execute **inside this step**.
    4. SameValue(constructResult, element) → TypeError. —
    then unconditionally: restore map[C]; pop the construction stack. —
    then on throw: null the definition; empty the reaction queue; rethrow. —
11. If form-associated: reset the form owner, **(enqueue)** `formAssociatedCallback` /
    `formDisabledCallback`. —
12. Set state to "custom". —

### 0.4 Trusted Types — the step nobody expects in `setAttribute`

`setAttribute` step 3 and `innerHTML`'s setter step 1 call **get trusted type compliant string**
(Trusted Types §3.4), whose step 4 calls **process value with a default policy** (§3.5), whose step 2
calls **get trusted type policy value** — which **invokes the page's own
`createHTML`/`createScript`/`createScriptURL` policy callback**. That is author code, running
synchronously, in the middle of `setAttribute`. It is not optional and not a Chrome-ism: it is what
the algorithm says.

`get trusted type compliant string` (Trusted Types §3.4):
1. If input is already an instance of expectedType, return its stringification. —
2. requireTrustedTypes = "Does sink type require trusted types?" —
3. If false, return the stringified input. —
4. **`[S]`** convertedInput = process value with a default policy — **runs the page's policy
   callback**.
5. Rethrow if it threw. —
6. If convertedInput is null/undefined: CSP report, then throw TypeError. —
7. Assert. —
8. Return stringified convertedInput. —

---

## 1. DOM §4.5 "Interface Document" — `createElement`, `createElementNS`, `getElementById`, and DOM §4.2.6's tree-order lookups

### 1.0 Two corrections to the brief, stated rather than picked silently

- **`getElementById` is not in §4.5.** It is **DOM §4.2.4 "Mixin NonElementParentNode"**
  (`Document includes NonElementParentNode`). §4.5's IDL block does not contain it.
- **§4.2.6 is titled "Mixin ParentNode"**, not "Interface ParentNode". Older editions of DOM
  (pre-2018, when mixins were still written as `interface ParentNode`) titled it "Interface
  ParentNode"; the section number is unchanged.

### 1.1 `createElement(localName, options)` — DOM §4.5, `[CEReactions, NewObject]`

IDL: `[CEReactions, NewObject] Element createElement(DOMString localName, optional (DOMString or ElementCreationOptions) options = {});`

Prologue (Web IDL §3.2, before step 1):

- P1. **`[S]`** `DOMString localName` ← ToString(V).
- P2. **`[S]`** `(DOMString or ElementCreationOptions) options` ← union conversion (Web IDL §3.2.25).
  If V is an Object it takes the dictionary arm: `[[Get]](V, "customElementRegistry")`.
- P3. **`[S]`** dictionary member `[[Get]](V, "is")` (dictionary members are converted in
  **lexicographical order**, so `customElementRegistry` precedes `is`).
- P4. **`[S]`** ToString of the `is` member value (`DOMString is`).
  If V is not an Object, P2–P4 collapse to a single ToString(V) for the `DOMString` arm.
- P0. `[CEReactions]` step 1: push an element queue. —

Steps:

1. If `localName` is not a **valid element local name** → throw `InvalidCharacterError`. —
2. If this is an HTML document, lowercase `localName` (ASCII). —
3. `registry`, `is` ← **flatten element creation options** given options and this. — *(no user code:
   the dictionary was already converted in the prologue; this step only reads its members, calls
   **look up a custom element registry**, and can throw `NotSupportedError` when both
   `customElementRegistry` and `is` are supplied)*
4. `namespace` ← HTML namespace if this is an HTML document or its content type is
   `application/xhtml+xml`; otherwise null. —
5. **`[S]` (delegated)** Return **create an element** given this, `localName`, `namespace`, null,
   `is`, **`synchronousCustomElements` = true**, `registry`. — *the `true` is why a custom element's
   constructor runs **inside** `createElement`, synchronously; see §1.4.*

Epilogue:

- E1. **`[S]`** `[CEReactions]` step 4: invoke custom element reactions in the popped queue.

### 1.2 `createElementNS(namespace, qualifiedName, options)` — DOM §4.5, `[CEReactions, NewObject]`

Prologue: **`[S]`** ToString(`namespace`) when not null (`DOMString?`); **`[S]`** ToString(
`qualifiedName`); **`[S]`×3** the same `options` union/dictionary trio as §1.1.

Method steps: "return the result of running the **internal createElementNS steps**, given this,
namespace, qualifiedName, and options."

Internal createElementNS steps:

1. `(namespace, prefix, localName)` ← **validate and extract** namespace and qualifiedName given
   "element". — *(throws `InvalidCharacterError` / `NamespaceError`; no user code)*
2. `registry`, `is` ← **flatten element creation options**. —
3. **`[S]` (delegated)** Return **create an element** given document, `localName`, `namespace`,
   `prefix`, `is`, **true**, `registry`.

Epilogue: **`[S]`** `[CEReactions]` invoke.

### 1.3 `flatten element creation options(options, document)` — DOM §4.5

1. `registry` ← look up a custom element registry given document. —
2. `is` ← null. —
3. If options is a dictionary:
   1. If `options["is"]` exists, set `is` to it. —
   2. If `options["customElementRegistry"]` exists:
      1. If `is` is non-null → throw `NotSupportedError`. —
      2. Set `registry` to it. —

No user code anywhere in this algorithm. The `[[Get]]`s the page can observe already happened in the
binding prologue.

### 1.4 `create an element(document, localName, namespace, prefix, is, synchronousCustomElements, registry)` — DOM §4.9

This is where the author code is. It is defined in **§4.9 "Interface Element"**, not §4.5, even
though §4.5's two methods are its only callers in DOM.

1. `result` ← null. —
2. If `registry` is "default", `registry` ← look up a custom element registry given document. —
3. `definition` ← **look up a custom element definition** given registry, namespace, localName, is.
   — *(a map lookup; no user code)*
4. If `definition` is non-null and its **name ≠ its local name** (a **customized built-in**):
   1. `interface` ← the element interface for localName and the HTML namespace. —
   2. `result` ← **create an element internal**(…, state `"undefined"`, …). —
   3. If `synchronousCustomElements` is true, catching exceptions:
      1. **`[S]`** **Upgrade `result` using `definition`** — HTML §4.13.5, whose step 10.3 constructs
         the author's class.
      - on exception: 1. **`[S]`** **Report exception** for the constructor's realm's global —
        HTML §8.1.4.6 fires an `error` event at that global, so `window.onerror` runs.
        2. Set result's custom element state to "failed". —
   4. Otherwise, **(enqueue)** a custom element upgrade reaction given result and definition. —
5. Otherwise, if `definition` is non-null (an **autonomous** custom element):
   1. If `synchronousCustomElements` is true:
      1. `C` ← definition's constructor. —
      2. `previousRegistry` ← agent's active custom element constructor map[C] (default null). —
      3. Set agent's active custom element constructor map[C] ← registry. —
      4. Catching exceptions:
         1. **`[S]`** `result` ← **constructing `C`, with no arguments** — *the author's
           constructor, running synchronously inside `document.createElement`.*
         2–3. Asserts on result's custom element state and namespace. —
         4. If result's attribute list is not empty → throw `NotSupportedError`. —
         5. If result has children → throw `NotSupportedError`. —
         6. If result's parent is non-null → throw `NotSupportedError`. —
         7. If result's node document ≠ document → throw `NotSupportedError`. —
         8. If result's local name ≠ localName → throw `NotSupportedError`. —
         9. Set result's namespace prefix ← prefix. —
         10. Set result's is value ← null. —
         11. Set result's custom element registry ← registry. —
         - on exception: 1. **`[S]`** **Report exception** (fires `error` at the global).
           2. `result` ← create an element internal(HTMLUnknownElement, …, `"failed"`, …). —
      5–6. Restore agent's active custom element constructor map[C]. —
   2. Otherwise:
      1. `result` ← create an element internal(HTMLElement, …, `"undefined"`, …). —
      2. **(enqueue)** a custom element upgrade reaction. —
6. Otherwise (not a custom element):
   1. `interface` ← the element interface for localName and namespace. —
   2. `result` ← create an element internal(…, `"uncustomized"`, …). —
   3. If namespace is the HTML namespace and (localName is a valid custom element name or is is
      non-null), set state to `"undefined"`. —
7. Return `result`. —

### 1.5 `create an element internal(document, interface, localName, namespace, prefix, state, is, registry)` — DOM §4.9

1. `element` ← **create a node that implements `interface`**, given document. — *(DOM §4.4: "return
   a new node that implements interface, in document's relevant realm". This does **not** run a
   constructor and does **not** consult the page — but it DOES read the **realm's** interface
   prototype object, which is why this is per-realm state and never a module static.)*
2. Set namespace, namespace prefix, local name, custom element registry, custom element state,
   custom element definition (null), is value. —
3. Assert: attribute list is empty. —
4. Return element. —

No suspension points.

### 1.6 `getElementById(elementId)` — DOM §4.2.4 "Mixin NonElementParentNode"

Prologue: **`[S]`** ToString(`elementId`).

Method steps: "return the result of running **get an element by ID** given this and elementId."

`get an element by ID(node, elementId)`: "return the first element, **in tree order**, within node's
descendants, whose ID is elementId; otherwise null." — **(walk)**, no user code.

The ID is maintained by DOM §4.9's own **attribute change steps** ("If localName is `id`, namespace
is null, …"), so a UA index is an optimization of a defined observable, not a new concept.

### 1.7 DOM §4.2.6 "Mixin ParentNode" — tree-order lookups

IDL:
```
[SameObject] readonly attribute HTMLCollection children;
readonly attribute Element? firstElementChild;
readonly attribute Element? lastElementChild;
readonly attribute unsigned long childElementCount;
[CEReactions, Unscopable] undefined prepend((Node or DOMString)... nodes);
[CEReactions, Unscopable] undefined append((Node or DOMString)... nodes);
[CEReactions, Unscopable] undefined replaceChildren((Node or DOMString)... nodes);
[CEReactions] undefined moveBefore(Node node, Node? child);
Element? querySelector(DOMString selectors);
[NewObject] NodeList querySelectorAll(DOMString selectors);
```

| Member | Steps | User code |
| --- | --- | --- |
| `children` | return an `HTMLCollection` rooted at this matching only element children | none. `[SameObject]` — the same object every read; it is **live**, so it must not be snapshotted |
| `firstElementChild` | return the first child that is an element; otherwise null | none |
| `lastElementChild` | return the last child that is an element; otherwise null | none |
| `childElementCount` | return the number of element children | none, **(walk)** |
| `querySelector(selectors)` | return the **first** result of **scope-match a selectors string** against this, else null | prologue **`[S]`** ToString(selectors). Algorithm: **(walk)**, no user code |
| `querySelectorAll(selectors)` | return the **static** result of scope-match | same |

**`scope-match a selectors string(selectors, node)`** — DOM §1.3 "Selectors":
1. `selector` ← parse a selector `selectors`. —
2. If failure → throw `SyntaxError`. —
3. Return match a selector against a tree with selector and node's root, **scoping root** node. —
   **(walk)**

**`convert nodes into a node(nodes, document)`** — DOM §4.2.6, used by `prepend`/`append`/
`replaceChildren`:
1. Replace each string in `nodes` with a text node created from it. — *(the `(Node or DOMString)`
   union conversion, and its ToString, already ran in the prologue: **`[S]`** once per string
   argument)*
2. If `nodes`'s size is 1, return `nodes[0]`. —
3. `fragment` ← create a document fragment given document. —
4. For each node of nodes: **append** node to fragment. — *(delegates to Algorithm 3 —
   `pre-insert` → `insert` — into a fragment that is not connected, so no `connectedCallback` and no
   post-connection script execution; but `insert` step 9's **children changed steps** still run)*
5. Return fragment. —

`prepend`: 1. convert nodes into a node. 2. **`[S]` (delegated)** pre-insert before first child.
`append`: 1. convert. 2. **`[S]` (delegated)** append.
`replaceChildren`: 1. convert. 2. ensure pre-insert validity. 3. **`[S]` (delegated)** replace all.
`moveBefore`: 1–2. resolve referenceChild. 3. **`[S]` (delegated)** move (Algorithm 3).
All four then hit the **`[S]`** `[CEReactions]` invoke.

### 1.8 Headless note

Nothing in this group is device-dependent. `create an element` computes a real interface, a real
registry lookup and a real state machine with no display involved; `querySelector` is a real
selector match over a real tree. There is no step here whose value may be left unknown.

**Suspension points in Algorithm group 1: 18.**
(P1–P4 of `createElement` = 4; `createElementNS`'s namespace+qualifiedName = 2 more distinct
conversions; `getElementById`'s elementId = 1; `querySelector`/`querySelectorAll`'s selectors = 1;
`create an element` 4.3.1, its report path, 5.1.4.1, its report path = 4; `[CEReactions]` step 4 = 1,
whose three internal arms — upgrade / callback / report — are 3 more; §4.2.6's variadic `(Node or
DOMString)` ToString = 1; §4.2.6's four mutating members delegating into Algorithm 3 = 1.)

---

## 2. DOM §4.9 "Interface Element" — `setAttribute`, `setAttributeNS`, `getAttribute`, `removeAttribute`, and the attribute change steps

IDL (DOM §4.9), which is load-bearing for where the suspension points are:

```
DOMString? getAttribute(DOMString qualifiedName);
DOMString? getAttributeNS(DOMString? namespace, DOMString localName);
[CEReactions] undefined setAttribute(DOMString qualifiedName, (TrustedType or DOMString) value);
[CEReactions] undefined setAttributeNS(DOMString? namespace, DOMString qualifiedName,
                                       (TrustedType or DOMString) value);
[CEReactions] undefined removeAttribute(DOMString qualifiedName);
[CEReactions] undefined removeAttributeNS(DOMString? namespace, DOMString localName);
[CEReactions] boolean toggleAttribute(DOMString qualifiedName, optional boolean force);
[CEReactions] attribute DOMString id;
[CEReactions] attribute DOMString className;
```

`getAttribute` is **not** `[CEReactions]`; the four mutators are.

### 2.1 `getAttribute(qualifiedName)` — DOM §4.9

Prologue: **`[S]`** ToString(qualifiedName).

1. `attr` ← **getting an attribute** given qualifiedName and this. — *(lowercases for an HTML
   element in an HTML document, then a linear scan of the attribute list; no user code)*
2. If `attr` is null, return null. —
3. Return `attr`'s value. —

`getAttributeNS(namespace, localName)` is the same three steps against the namespace/localName
lookup. Prologue: **`[S]`** ToString(namespace) when not null, **`[S]`** ToString(localName).

### 2.2 `setAttribute(qualifiedName, value)` — DOM §4.9

Prologue: **`[S]`** ToString(qualifiedName); **`[S]`** the `(TrustedType or DOMString)` union
conversion (Web IDL §3.2.25 — a platform-object arm for a real `TrustedHTML`/`TrustedScript`/
`TrustedScriptURL`, otherwise the string arm's ToString). `[CEReactions]` push.

1. If `qualifiedName` is not a **valid attribute local name** → throw `InvalidCharacterError`. —
   *(the spec's own note: the parameter is named qualifiedName but is validated as a local name,
   because it is only used as a qualified name if such an attribute already exists.)*
2. If this is in the HTML namespace and its node document is an HTML document, lowercase
   `qualifiedName`. —
3. **`[S]`** `verifiedValue` ← **get trusted type compliant attribute value** with qualifiedName,
   null, this, and value — **Trusted Types §3.4/§3.5: the page's default-policy callback runs
   here.** (See §0.4. `get trusted type compliant attribute value` first maps
   (element, attribute) → an expected type via **get Trusted Type data for attribute**; when there
   is no mapping it returns the string unchanged and no callback runs.)
4. `attribute` ← the **first** attribute in this's attribute list whose qualified name is
   qualifiedName, else null. —
5. If `attribute` is non-null → **change** attribute to verifiedValue and **return**. —
   *(delegates to §2.5; that path is `(enqueue)`-only)*
6. `attribute` ← **create an attribute** given this's node document, qualifiedName, null, null,
   verifiedValue. —
7. **Append** attribute to this. — *(delegates to §2.5; `(enqueue)`-only)*

Epilogue: **`[S]`** `[CEReactions]` step 4 — this is where `attributeChangedCallback` actually runs.

### 2.3 `setAttributeNS(namespace, qualifiedName, value)` — DOM §4.9

Prologue: **`[S]`** ToString(namespace) when not null; **`[S]`** ToString(qualifiedName);
**`[S]`** the `(TrustedType or DOMString)` union. `[CEReactions]` push.

1. `(namespace, prefix, localName)` ← **validate and extract** namespace and qualifiedName given
   "attribute". — *(throws `InvalidCharacterError` / `NamespaceError`)*
2. **`[S]`** `verifiedValue` ← get trusted type compliant attribute value with localName, namespace,
   this, value.
3. **Set an attribute value** for this using localName, verifiedValue, prefix, namespace. —
   *(§2.6; `(enqueue)`-only)*

Epilogue: **`[S]`** `[CEReactions]` invoke.

Note the **ordering difference from `setAttribute`**: `setAttributeNS` does the Trusted Types call
**after** validate-and-extract (step 2 of 3), `setAttribute` does it **after** the lowercase
(step 3 of 7). A machine that shares one stage table across both gets the order wrong.

### 2.4 `removeAttribute(qualifiedName)` / `removeAttributeNS(namespace, localName)` — DOM §4.9

Prologue: **`[S]`** ToString per argument. `[CEReactions]` push.

Method steps: "remove an attribute given qualifiedName and this, and then return undefined."

**`remove an attribute by name(qualifiedName, element)`** — DOM §4.9:
1. `attr` ← getting an attribute given qualifiedName and element. —
2. If `attr` is non-null, then **remove** attr. — *(§2.5; `(enqueue)`-only)*
3. Return attr. —

**`remove an attribute by namespace and local name(namespace, localName, element)`**: the same three
steps. Epilogue: **`[S]`** `[CEReactions]` invoke.

### 2.5 The attribute mutation primitives — DOM §4.9

**`handle attribute changes(attribute, element, oldValue, newValue)`** — the algorithm the brief calls
"the attribute change steps they invoke". It has three steps, and **none of them runs user code**:

1. **(enqueue)** **Queue a mutation record** of `"attributes"` for element with attribute's local
   name, namespace, oldValue, « », « », null, null. — *(DOM §4.3.2: appends a `MutationRecord` to
   each interested observer's record queue, appends the observer to the agent's pending mutation
   observers, and **queues a mutation observer microtask**. The observer's **callback runs in that
   microtask**, i.e. at the next microtask checkpoint — see Algorithm 5.)*
2. **(enqueue)** If element is **custom**, enqueue a custom element callback reaction with element,
   `"attributeChangedCallback"`, and « local name, oldValue, newValue, namespace ». — *(HTML
   §4.13.6's enqueue filters on `observed attributes` and drops the reaction if the name is not
   observed. If the reactions stack is empty this queues the **backup-queue microtask** instead.)*
3. **Run the attribute change steps** with element, local name, oldValue, newValue, namespace. —
   **This is the extension point other standards hook**, and it is worth being exact about what runs
   there:
   - DOM's own: update the element's **ID** (`localName is id`, `namespace is null`); update a
     `DOMTokenList`'s token set (`class`, `rel`, …); a `slot`'s name. **No user code.**
   - HTML's: `<script>`'s (HTML §4.12.1) — "If namespace is not null, return. If localName is
     `src`, value is not null, and element is connected, then run the script HTML element
     post-connection steps" → **prepare the script element**. Because the branch requires `src`,
     that path takes the **fetch** arm, which is asynchronous; **no author code runs
     synchronously here**. (The synchronous execution lives in the *children changed steps* and the
     *post-connection steps* — see Algorithm 3.)
   - HTML's event handler content attributes (`onclick`, …): the attribute change steps set the
     event handler's value to an **internal raw uncompiled handler**. Compilation into a function
     happens later, when the handler is invoked. **No user code here.**
   - `<img src>`, `<iframe src>`, `<input type>`, form-association: all start asynchronous work or
     run pure computation. **No user code here.**

   If you find a spec whose attribute change steps do run script synchronously, that is a genuine
   third suspension point and this table is wrong; as of the versions read above there is none.

**`change an attribute(attribute, value)`**:
1. `oldValue` ← attribute's value. —
2. Set attribute's value ← value. —
3. Handle attribute changes for attribute with attribute's element, oldValue, value. — *(above)*

**`append an attribute(attribute, element)`**:
1. Append attribute to element's attribute list. —
2. Set attribute's element ← element. —
3. Set attribute's node document ← element's node document. —
4. Handle attribute changes with element, null, attribute's value. —

**`remove an attribute(attribute)`**:
1. `element` ← attribute's element. —
2. Remove attribute from element's attribute list. —
3. Set attribute's element ← null. —
4. Handle attribute changes with element, attribute's value, null. —

**`set an attribute(attr, element)`** (the `setAttributeNode` path) additionally begins with
**`[S]`** get trusted type compliant attribute value (step 1) and then step 2's `InUseAttributeError`
check.

### 2.6 The value-level helpers — DOM §4.9

**`set an attribute value(element, localName, value, prefix, namespace)`**:
1. `attribute` ← getting an attribute given namespace, localName, element. —
2. If null → append a newly created attribute → *(handle attribute changes)*; return. —
3. Change attribute to value → *(handle attribute changes)*. —

**`get an attribute value(element, localName, namespace)`**:
1. `attr` ← getting an attribute. — 2. If null, return "". — 3. Return attr's value. — No user code.

These two are what DOM §4.9's `id`/`className`/`slot` reflect through, and what HTML §2.6.1's
`set the content attribute` / `get the content attribute` call (Algorithm 4).

### 2.7 Headless note

Every value here is fully defined without a device. There is no attribute whose value the spec leaves
to the UA.

**Suspension points in Algorithm group 2: 13.**
(`getAttribute` ToString ×1; `getAttributeNS` ×2; `setAttribute` ToString + union ×2 and its step 3
Trusted Types ×1; `setAttributeNS` ×3 conversions and its step 2 Trusted Types ×1; `removeAttribute`
/ `removeAttributeNS` conversions ×2 — counted once as a shared shape; the `[CEReactions]` invoke
epilogue ×1.) **Inside `handle attribute changes` itself: zero.**

---

## 3. DOM §4.2.3 "Mutation algorithms" — pre-insert, insert, append, replace, remove

The single most important fact in this section: **DOM forbids the insertion steps from running
script and explicitly permits the post-connection steps to.** The spec's words for the insertion
steps: "These steps must not modify the node tree that insertedNode participates in, create browsing
contexts, fire events, or otherwise execute JavaScript. These steps may queue tasks to do these
things asynchronously, however." The **moving steps** carry the identical prohibition. The
**post-connection steps** exist precisely to be the place that may.

### 3.1 `ensure pre-insert validity(node, parent, child, childrenToExclude)` — DOM §4.2.3

Steps 1–11: `HierarchyRequestError` / `NotFoundError` checks over node types, host-including
ancestry, doctype/element ordering. **No user code at any step.**

1. parent must be Document/DocumentFragment/Element. — 2. node must not be a host-including inclusive
ancestor of parent. — 3. child's parent must be parent. — 4. node must be DocumentFragment/
DocumentType/Element/CharacterData. — 5. If parent is not a document: doctype check, return. —
6. Text into a document. — 7. CharacterData → return. — 8. DocumentFragment element/Text child
counts. — 9. DocumentFragment or Element: element-child / doctype-following checks, return. —
10. Assert node is a doctype. — 11. doctype-child / element-preceding checks. —

### 3.2 `pre-insert(node, parent, child)` — DOM §4.2.3

1. Ensure pre-insert validity given node, parent, child, « ». —
2. `referenceChild` ← child. —
3. If referenceChild is node, set it to node's next sibling. —
4. **`[S]` (delegated)** **Insert** node into parent before referenceChild. —
5. Return node. —

`appendChild`/`insertBefore` are `[CEReactions]`, so the **`[S]`** `[CEReactions]` invoke epilogue
applies on top.

### 3.3 `insert(node, parent, child, suppressObservers)` — DOM §4.2.3 — **the algorithm to get right**

1. `nodes` ← node's children if node is a DocumentFragment; otherwise « node ». —
2. `count` ← nodes's size. —
3. If count is 0, return. — *(this is why appending an empty fragment fires nothing)*
4. If node is a DocumentFragment:
   1. **Remove** its children with suppressObservers = true. — *(delegates to §3.6: `(enqueue)`s
      `disconnectedCallback` per custom descendant and runs the **removing steps**; and its own step
      17 **children changed steps** is **`[S]`**)*
   2. **(enqueue)** Queue a tree mutation record for node with « », nodes, null, null. —
      *(intentionally ignores suppressObservers)*
5. If child is non-null: adjust live ranges' start/end offsets by count. — *(two substeps; no user
   code)*
6. `previousSibling` ← child's previous sibling, or parent's last child if child is null. —
7. **For each node in `nodes`, in tree order:** — **(walk)**
   1. **Adopt** node into parent's node document. — *(DOM §4.5 `adopt a node`: if the document
      changes, for each shadow-including inclusive descendant it sets the node document, fixes the
      custom element registry, **(enqueue)**s `adoptedCallback` for custom descendants, and runs the
      **adopting steps**. HTML's adopting steps run no author code.)*
   2. If child is null, append node to parent's children. —
   3. Otherwise insert node into parent's children before child's index. —
   4. If parent is a shadow host with `"named"` slot assignment and node is a slottable → assign a
      slot for node. —
   5. If parent's root is a shadow root and parent is an empty `slot` → **signal a slot change**. —
      *(**(enqueue)**: appends to the agent's signal slots and queues the mutation observer
      microtask; `slotchange` fires from that microtask)*
   6. Run **assign slottables for a tree** with node's root. — **(walk)**
   7. **For each shadow-including inclusive descendant `inclusiveDescendant`, in shadow-including
      tree order:** — **(walk)**
      1. Run the **insertion steps** with inclusiveDescendant. — **explicitly may not execute
         JavaScript.** *(They still have script-observable consequences: DOM's own example has
         `<style>`'s insertion steps apply style rules that a later post-connection script reads
         back through `getComputedStyle`.)*
      2. If not connected, continue. —
      3. If it is an element with a non-null custom element registry:
         1. Scoped-registry bookkeeping. —
         2. If **custom** → **(enqueue)** `connectedCallback`. —
         3. Otherwise → **try to upgrade** → **(enqueue)** an upgrade reaction. —
      4. Otherwise, shadow-root scoped-registry bookkeeping. —
8. If suppressObservers is false → **(enqueue)** queue a tree mutation record for parent with nodes,
   « », previousSibling, child. —
9. **`[S]`** Run the **children changed steps** for parent. — **This step CAN run author code.**
   DOM does **not** apply the insertion-steps prohibition to children changed steps, and HTML uses
   that: `<script>`'s children changed steps **prepare the script element**, and for an inline
   classic script "prepare the script element" ends at "**Otherwise, immediately execute the script
   element el, even if other scripts are already executing**". HTML's own worked example spells
   this out: appending three children to a `<script>` in one call runs the insertion steps (no
   consequences), then the children changed steps **execute the script body**, then the
   post-connection steps run for a nested script.
10. `staticNodeList` ← « ». — *(the spec's note explains why: the post-connection steps can modify
    the tree, so a live traversal would be unsafe)*
11. For each node of nodes, in tree order: for each shadow-including inclusive descendant, append to
    staticNodeList. — **(walk)**
12. **`[S]`** For each node of staticNodeList: if **connected**, run the **post-connection steps**
    with node. — **This step CAN run author code, by design.** `<script>`'s post-connection steps
    prepare the script element (→ immediate execution for an inline classic script); `<iframe>`'s
    create a child navigable. Every node in the batch is inserted **before** any of them runs.

**`append(node, parent)`** — DOM §4.2.3: one step, "pre-insert node into parent before null", so it
inherits every point above.

### 3.4 `replace(child, node, parent)` — DOM §4.2.3

1. Ensure pre-insert validity given node, parent, child, « child ». —
2. `referenceChild` ← child's next sibling. —
3. If referenceChild is node, set it to node's next sibling. —
4. `previousSibling` ← child's previous sibling. —
5. `removedNodes` ← ∅. —
6. **Adopt** node into parent's node document. — *(**(enqueue)** `adoptedCallback`)*
7. If child's parent is non-null:
   1. `removedNodes` ← « child ». —
   2. **`[S]` (delegated)** **Remove** child with suppressObservers = true. — *(§3.6; its step 17
      children changed steps is a suspension point)*
8. `nodes` ← node's children if a DocumentFragment, otherwise « node ». —
9. **`[S]` (delegated)** **Insert** node into parent before referenceChild with
   suppressObservers = true. — *(§3.3; carries steps 9 and 12)*
10. **(enqueue)** Queue a tree mutation record for parent with nodes, removedNodes, previousSibling,
    referenceChild. —
11. Return child. —

**`replace all(node, parent)`** — DOM §4.2.3 (what `innerHTML` and `replaceChildren` use):
1. `removedNodes` ← parent's children. —
2. `addedNodes` ← ∅. —
3. If node is a DocumentFragment, `addedNodes` ← node's children. —
4. Otherwise if node is non-null, `addedNodes` ← « node ». —
5. **`[S]` (delegated)** **Remove all** parent's children, in tree order, with
   suppressObservers = true. —
6. If node is non-null → **`[S]` (delegated)** **Insert** node into parent before null with
   suppressObservers = true. —
7. If either set is non-empty → **(enqueue)** queue a tree mutation record. —

### 3.5 `move(node, newParent, child)` — DOM §4.2.3 (`moveBefore`)

Included because it is a *separate primitive* from insert+remove and its reaction set differs.
Steps 1–8: `HierarchyRequestError`/`NotFoundError` validity + `oldParent`. Steps 9–17: live-range
and `NodeIterator` pre-remove steps, sibling capture, removal from the old children list, slot
assignment, offset fixups. Steps 18–23: insertion into the new children list, slot assignment,
assign-slottables. Step 24: for each shadow-including inclusive descendant — **24.2 run the moving
steps** (**must not execute JavaScript**, same prohibition as the insertion steps) and **24.3
(enqueue)** `connectedMoveCallback` when custom and newParent is connected. Steps 25–26:
**(enqueue)** two tree mutation records. **No `[S]` inside `move` itself** — its author code is the
`[CEReactions]` epilogue on `moveBefore`.

### 3.6 `remove(node, suppressObservers)` — DOM §4.2.3

1. `parent` ← node's parent. —
2. Assert parent is non-null. —
3. Run the **live range pre-remove steps** given node. —
4. For each `NodeIterator` whose root's node document is node's node document: run the
   **NodeIterator pre-remove steps**. — **(walk)** *(this is bookkeeping on the iterator's
   reference node; the `NodeFilter` callback is **not** invoked here)*
5. `oldPreviousSibling` ← node's previous sibling. —
6. `oldNextSibling` ← node's next sibling. —
7. Remove node from its parent's children. —
8. If node is assigned → assign slottables for node's assigned slot. —
9. If parent's root is a shadow root and parent is an empty slot → **(enqueue)** signal a slot
   change. —
10. If node has an inclusive descendant that is a slot → assign slottables for a tree (×2). —
    **(walk)**
11. Run the **removing steps** with node, true, parent. — *(same prohibition class as insertion
    steps)*
12. `isParentConnected` ← parent's connected. —
13. If node is **custom** and isParentConnected → **(enqueue)** `disconnectedCallback`. —
14. For each shadow-including **descendant**, in shadow-including tree order: — **(walk)**
    1. Run the removing steps with descendant, false, parent. —
    2. If custom and isParentConnected → **(enqueue)** `disconnectedCallback`. —
15. For each inclusive ancestor of parent, for each registered observer with `subtree: true`, append
    a **transient registered observer** to node's registered observer list. — **(walk)**
16. If suppressObservers is false → **(enqueue)** queue a tree mutation record for parent with « »,
    « node », oldPreviousSibling, oldNextSibling. —
17. **`[S]`** Run the **children changed steps** for parent. — same reasoning as `insert` step 9:
    removing a `<script>`'s text child re-runs `prepare the script element` on the parent, which for
    an inline classic script executes it.

`removeChild(child)` = **pre-remove**: 1. if child's parent is not this → `NotFoundError`;
2. **`[S]` (delegated)** remove child; 3. return child. Plus the **`[S]`** `[CEReactions]` epilogue.

### 3.7 Where the reactions and records actually surface

| Thing queued | Queued at | Drained at |
| --- | --- | --- |
| `connectedCallback` / `disconnectedCallback` / `adoptedCallback` / `connectedMoveCallback` / `attributeChangedCallback` / upgrade | insert 7.7.3, remove 13/14.2, adopt 3.3.3, move 24.3, handle attribute changes 2 | **`[S]`** the `[CEReactions]` wrapper's step 4 on the API that started it — or, if there was no `[CEReactions]` API on the stack, the **backup element queue microtask** |
| `MutationRecord` | insert 4.2/8, remove 16, replace 10, replace all 7, handle attribute changes 1 | **`[S]`** `notify mutation observers`, run from the **mutation observer microtask** (DOM §4.3.1 step 6.4: "invoke mo's callback with « records, mo » and `report`") |
| `slotchange` | signal a slot change (insert 7.5, remove 9) | **`[S]`** the same mutation observer microtask (DOM §4.3.1 steps 4–5, 7) |

### 3.8 Headless note

All of it is device-independent. The `<style>` example in DOM §4.2.3 is worth restating for this
engine: the insertion steps of a `<style>` **apply its rules immediately**, and a script inserted in
the same batch reads them back through `getComputedStyle` in the same turn. A `getComputedStyle`
that returns opaque makes that example produce the wrong answer; the spec computes a real value.

**Suspension points in Algorithm group 3: 8.**
(`insert` step 9 children changed; `insert` step 12 post-connection; `remove` step 17 children
changed; `replace` step 7.2 and step 9 delegating into those; `replace all` steps 5 and 6 delegating
into those; the `[CEReactions]` epilogue shared by `appendChild`/`insertBefore`/`replaceChild`/
`removeChild`/`moveBefore`. **Zero** inside the insertion steps, the moving steps, the removing
steps, `ensure pre-insert validity`, or `adopt`.)

---

## 4. HTML §2.6.1 reflected IDL attributes, and HTML §8.5.4/§8.5.5 `innerHTML`/`outerHTML`

### 4.0 Two corrections to the brief, stated rather than picked silently

- **Reflection is HTML §2.6.1 "Reflecting content attributes in IDL attributes"** (inside §2.6
  "Common DOM interfaces"), not §3.2.2. HTML §3.2.2 is "Elements in the DOM". §2.6.2 is "Using
  reflect via IDL extended attributes" and §2.6.3 is "Using reflect in specifications".
- **`innerHTML`/`outerHTML` are no longer in "DOM Parsing and Serialization".** That specification
  has been **merged into HTML**: `innerHTML` is **HTML §8.5.4 "The `innerHTML` property"**,
  `outerHTML` is **§8.5.5**, `insertAdjacentHTML()` is **§8.5.6**, `createContextualFragment()` is
  **§8.5.7**, `DOMParser` is **§8.5.1**, `XMLSerializer` is **§8.5.8** — all under §8.5 "DOM parsing
  and serialization APIs". The old spec still exists as a stub with an issue tracker; HTML links to
  it only for the outstanding-issues note. If a reader is holding the DOM Parsing spec, they are
  holding a document that no longer defines these.

### 4.1 Reflection — HTML §2.6.1

Three building blocks: a **reflected target** (an `Element` or an `ElementInternals`), a **reflected
IDL attribute**, a **reflected content attribute name**. Four per-target algorithms, none of which
runs user code:

For an element target: `get the element` → return element. `get the content attribute` → 1. getting
an attribute by namespace and local name (null, name, element); 2. if null return null; 3. return its
value. `set the content attribute(value)` → **set an attribute value**(element, name, value)
(DOM §4.9 — which reaches `handle attribute changes`, so it **(enqueue)**s the mutation record and
the `attributeChangedCallback`). `delete the content attribute` → **remove an attribute by namespace
and local name**.

Per-type getter/setter steps — the point of listing them is that **the type decides how much
computation happens**, and none of it is user code:

| IDL type | getter steps | setter steps | user code |
| --- | --- | --- | --- |
| `DOMString` | 1. get the element; 2. get the content attribute; 3. find the attribute definition; 4. if enumerated **and** limited to only known values → map to the canonical keyword or ""; 5. if null return ""; 6. return the value | set the content attribute with the value | setter prologue only: **`[S]`** ToString |
| `DOMString?` | 1–3 as above; 4. if enumerated: assert limited-to-known-values, assert it corresponds to a state, return null for a keywordless state, else the canonical keyword; 5. return the value | 1. if null → delete the content attribute; 2. otherwise set it | **`[S]`** ToString (non-null) |
| `USVString`, optionally **treated as a URL** | 1–2 as above; 3. if treated as a URL: 3.1 null → ""; 3.2 **encoding-parsing-and-serializing a URL** relative to the element's **node document**; 3.3 return it if not failure; 4. return the value converted to a **scalar value string** | set the content attribute | **`[S]`** ToString, then USVString's unpaired-surrogate replacement (§7.2) — no user code in the replacement itself |
| `boolean` | 1. get the content attribute; 2. null → false; 3. → true | 1. false → delete; 2. true → set to "" | **`[S]`** ToBoolean is never user code |
| `long` (± non-negative, ± default) | 1. get; 2. if non-null: **integer parsing** (or non-negative integer parsing) and range-check; 3. default; 4. −1 for non-negative; 5. 0 | 1. `IndexSizeError` for a negative when limited to non-negative; 2. set to the shortest valid-integer string | **`[S]`** the IDL `long` conversion (ToNumber → modulo 2³²) |
| `unsigned long` (± positive, ± clamped [min,max], ± default) | 1. get; 2–5. compute minimum/maximum; 6. non-negative integer parsing, in-range → value, clamped → min/max; 7. default; 8. minimum | 1. `IndexSizeError` for 0 when limited to positive; 2–7. compute newValue and set the shortest valid non-negative-integer string. **Clamped has no effect on the setter.** | **`[S]`** the IDL `unsigned long` conversion |
| `double` (± positive, ± default) | analogous, via **valid floating-point number** parsing | analogous | **`[S]`** the IDL `double` conversion |

**Headless note:** the URL case computes a real absolute URL against the document's base URL — a
value, never an opaque. `img.src` on a headless document is `https://host/path/x.png`, not unknown.

**Where user code can run in reflection: only the binding-layer conversion of the assigned value, and
the `[CEReactions]` invoke on the reflected attribute's setter.** DOM §4.9's `id`, `className` and
`slot` are all `[CEReactions] attribute DOMString`, and `classList` is
`[SameObject, PutForwards=value]` — a write to `el.classList` forwards to
`el.classList.value = …`, which is `DOMTokenList`'s `[CEReactions]` value setter.

### 4.2 `innerHTML` — HTML §8.5.4

IDL: `[CEReactions] attribute (TrustedHTML or [LegacyNullToEmptyString] DOMString) innerHTML;`
on both `Element` and `ShadowRoot`.

**Getter** (Element and ShadowRoot alike): "return the result of running **fragment serializing
algorithm steps** with this and true."

`fragment serializing algorithm steps(node, require well-formed)` — HTML §8.5.4:
1. `context document` ← node's node document. —
2. If it is an **HTML document** → return the **HTML fragment serialization algorithm** (HTML §13.3)
   with node, false, « ». — **(walk)**, no user code
3. Return the **XML serialization** of node given require well-formed. — **(walk)**; throws
   `InvalidStateError` when not well-formed

**No user code in the getter.** It is a full subtree serialization, so it is a `JS_STEP_YIELD` walk,
not an O(1) body.

**Setter** — `Element`:
1. **`[S]`** `compliantString` ← **get trusted type compliant string** with `TrustedHTML`, this's
   relevant global object, the given value, `"Element innerHTML"`, `"script"`. — **the page's
   Trusted Types default policy `createHTML` runs here** (§0.4).
2. `target` ← this. —
3. If target is a `template` element, set target to its **template contents** (a
   `DocumentFragment`). —
4. **`[S]`** `fragment` ← **fragment parsing algorithm steps** with target and compliantString. —
   see §4.4: **the parser can run author code** (a `<script>` in the markup under a non-Inert
   scripting mode, and custom element constructors through "create an element for the token").
5. **`[S]` (delegated)** **Replace all** with fragment within target. — DOM §4.2.3 (Algorithm 3
   §3.4): the remove-all and insert it delegates to carry `insert` step 9 (children changed) and step
   12 (post-connection).

Epilogue: **`[S]`** the `[CEReactions]` invoke.

`ShadowRoot`'s setter is the same minus the `template` step: 1. get trusted type compliant string
with `"ShadowRoot innerHTML"`; 2. fragment parsing algorithm steps; 3. replace all.

Prologue: the union `(TrustedHTML or [LegacyNullToEmptyString] DOMString)` — a real `TrustedHTML`
takes the platform-object arm; anything else takes the string arm, where `[LegacyNullToEmptyString]`
turns `null` into `""` **before** ToString, and everything else is **`[S]`** ToString.

### 4.3 `outerHTML` — HTML §8.5.5

**Getter**: 1. let `element` be a **fictional node whose only child is this**; 2. return the fragment
serializing algorithm steps with element and true. — **(walk)**, no user code.

**Setter**:
1. **`[S]`** `compliantString` ← get trusted type compliant string with `TrustedHTML`, …,
   `"Element outerHTML"`, `"script"`.
2. `parent` ← this's parent. —
3. If parent is null, **return** (the spec's reason: there would be no way to obtain a reference to
   the nodes created). —
4. If parent is a `Document` → throw `NoModificationAllowedError`. —
5. If parent is a `DocumentFragment`, set parent ← **create an element** given this's node document,
   `"body"`, the HTML namespace. — *(this is `create an element` with `synchronousCustomElements`
   defaulting to **false**, so no constructor runs; it is a plain `<body>`)*
6. **`[S]`** `fragment` ← fragment parsing algorithm steps given **parent** and compliantString.
   — *(note: parsed with **parent** as context, not `this` — that is what makes
   `td.outerHTML = "<tr>"` behave differently from `td.innerHTML`)*
7. **`[S]` (delegated)** **Replace** this with fragment within this's parent. — DOM §4.2.3 §3.4.

Epilogue: **`[S]`** the `[CEReactions]` invoke.

### 4.4 The fragment parsing algorithm — HTML §8.5.4 wrapper + HTML §13.4

`fragment parsing algorithm steps(target, markup, scriptingMode = Inert)` — HTML §8.5.4:
1. Assert scriptingMode is Inert or Fragment. —
2. If target's node document is an **XML document** → **XML fragment parsing algorithm**. —
3. Return the **HTML fragment parsing algorithm** given target, markup, **false**
   (allowDeclarativeShadowRoots), scriptingMode. —

**`innerHTML`/`outerHTML`/`insertAdjacentHTML` pass no scriptingMode, so it defaults to `Inert`** —
which is why `innerHTML = "<script>…"` does not execute the script. `setHTMLUnsafe()` and
`createContextualFragment()` are the members that pass `Fragment`.

**HTML fragment parsing algorithm** — HTML §13.4 "Parsing HTML fragments":
1. Assert scriptingMode is Inert or Fragment. —
2. `context` ← target if an Element, otherwise target's host. —
3. Assert context is non-null. —
4. `document` ← a new Document whose type is `"html"`. —
5. `contextDocument` ← context's node document. —
6–7. Inherit quirks / limited-quirks mode from contextDocument. —
8. Create a new HTML parser, associate it with document. —
9. If contextDocument's scripting is disabled, set scriptingMode ← **Disabled**. —
10. Set the parser's scripting mode. —
11. Set the tokenizer state from `context`'s local name: `title`/`textarea` → **RCDATA**;
    `style`/`xmp`/`iframe`/`noembed`/`noframes` → **RAWTEXT**; `script` → **script data**;
    `noscript` → RAWTEXT unless scripting mode is Disabled; `plaintext` → **PLAINTEXT**; anything
    else → **data**. —
12. **`[S]` (conditional)** `root` ← **create an element** given document, `"html"`, the HTML
    namespace, null, null, **false**, look up a custom element registry given target. — *false, so
    no synchronous constructor here.*
13. Append root to document. —
14. Set the stack of open elements to just « root ». —
15. `fragment` ← create a document fragment given target's node document. —
16. Set the parser's **root insertion target** ← fragment. —
17. If context is a `template` → push `"in template"` onto the stack of template insertion modes. —
18. Create a start tag token from context's local name and attributes; make it context's start tag
    token. —
19. **Reset the insertion mode appropriately** (referencing the context element). —
20. Set the form element pointer to the nearest ancestor-or-self `form` element. —
21. Place `input` into the parser's input stream. —
22. **`[S]`** **Start the HTML parser and let it run until it has consumed all the characters.** —
    **(walk)** over the whole markup, and a suspension point for two reasons: tree construction calls
    **create an element for the token**, which runs a custom element **constructor synchronously**
    when the parser's scripting mode is Fragment (and pushes onto the **backup element queue**
    otherwise); and under a non-Inert scripting mode a `<script>` token reaches "prepare the script
    element". Under **Inert** the `<script>` does not execute, but the constructor path is still
    reachable.
23. Return fragment. —

**Headless note:** the fragment parse is exactly the mXSS surface this project's `@S` half depends
on. The tokenizer-state switch at step 11 and the context-driven reset at step 19 are what make
`div.innerHTML = s` and `td.innerHTML = s` produce different trees from the same bytes, and what a
sink's parse context has to be derived from.

**Suspension points in Algorithm group 4: 11.**
(Reflection: the setter's IDL conversion ×1, the reflected setter's `[CEReactions]` invoke ×1.
`innerHTML` setter: step 1 Trusted Types, step 4 fragment parse, step 5 replace-all, the union
prologue, the `[CEReactions]` invoke = 5. `outerHTML` setter: step 1 Trusted Types, step 6 fragment
parse, step 7 replace, the `[CEReactions]` invoke = 4 — of which the `[CEReactions]` invoke is
already counted, so 3 new. Fragment parsing: step 12's create-an-element and step 22's parser run
= 2, of which step 12 is non-firing at `false`, so 1 new counted plus 1 noted. Getters: 0.)

---

## 5. HTML §8.1.7 "Event loops" — the microtask checkpoint and the task processing model

Section map, exactly as HTML numbers it: **§8.1.7 Event loops** → **§8.1.7.1 Definitions**,
**§8.1.7.2 Queuing tasks**, **§8.1.7.3 Processing model**, **§8.1.7.4 Generic task sources**,
**§8.1.7.5 Dealing with the event loop from other specifications**.

### 5.1 Definitions that decide what a stage may assume — HTML §8.1.7.1

- An **event loop** belongs to an **agent**, one per agent. A **window event loop** serves a
  similar-origin window agent — i.e. **one event loop across every same-origin document in the
  browsing-context group**, which is the same boundary `SECURITY.md` draws for a WASM instance.
- An event loop has **one or more task queues**. **A task queue is a set, not a queue** — the
  processing model takes the first **runnable** task from it, not the first task.
- **The microtask queue is NOT a task queue** and is never chosen in step 2.1.
- A **task** is a struct of {steps, source, document, script evaluation environment settings object
  set}. A task is **runnable** iff its document is null or **fully active**.
- Each event loop has a **currently running task**, a **microtask queue**, a **performing a microtask
  checkpoint** boolean, a **last render opportunity time**, and a **last idle period start time**.

### 5.2 Queuing — HTML §8.1.7.2

`queue a task(source, steps, event loop, document)`: steps 1–9 build the task struct and **append it
to the task queue that `source` is associated with on that event loop**. No user code.
`queue a global task` derives the event loop and document from a global; `queue an element task`
derives the global from an element. **`queue a microtask(steps, document)`**: steps 1–9, ending
"**Enqueue** microtask on eventLoop's microtask queue". No user code. Note step 1's assert: a
microtask cannot be queued from "in parallel".

### 5.3 The processing model — HTML §8.1.7.3

An event loop runs these steps continually:

1. `oldestTask`, `taskStartTime` ← null. —
2. **If the event loop has a task queue with at least one runnable task:**
   1. **`taskQueue` ← one such task queue, chosen in an implementation-defined manner.** — **This is
      the ONLY freedom the spec gives the scheduler at this level.** A UA may prefer one source over
      another; it may not reorder within a source.
   2. `taskStartTime` ← the unsafe shared current time. —
   3. **`oldestTask` ← the FIRST RUNNABLE task in taskQueue; remove it.** — the "first runnable", not
      the first: a task whose document is not fully active is skipped, not dropped.
   4. Record task start time. —
   5. Set the event loop's currently running task ← oldestTask. —
   6. **`[S]`** **Perform `oldestTask`'s steps.** — *the task is an event dispatch, a callback, a
      parser turn, a rendering update, an `unhandledrejection` firing…; this is where author code
      runs.*
   7. Set currently running task ← null. —
   8. **`[S]`** **Perform a microtask checkpoint.** — §5.4.
3. `taskEndTime` ← the unsafe shared current time. —
4. If oldestTask is not null: collect the top-level browsing contexts from the task's script
   evaluation environment settings object set, **report long tasks**, record task end time. — no
   user code (a `PerformanceObserver` entry is queued; its callback runs from a task).
5. If this is a **window event loop** with **no runnable task**: set last idle period start time,
   build `computeDeadline` (deadline = last idle period start + **50 ms**, lowered by the nearest
   timer deadline, and by `last render opportunity time + 1000/refresh rate` when any window has
   pending animation frame callbacks or pending rendering), then **start an idle period** for each
   same-loop window. — no user code at this step; the `requestIdleCallback` callbacks are invoked by
   the idle-period algorithm's own task.
6. If this is a **worker event loop**: optionally run the animation frame callbacks for a
   `DedicatedWorkerGlobalScope` and update its rendering; destroy the event loop when the task
   queues are empty and the closing flag is set. — **`[S]`** at the animation frame callbacks.

**And, in parallel, a window event loop also runs the rendering loop** — that is Algorithm 6.

### 5.4 `perform a microtask checkpoint` — HTML §8.1.7.3

1. If the event loop's **performing a microtask checkpoint** is true, **return**. — *the reentrancy
   guard; this is why a checkpoint inside a checkpoint is a no-op*
2. Set performing a microtask checkpoint ← true. —
3. **While the microtask queue is not empty:**
   1. `oldestMicrotask` ← **dequeue** from the microtask queue. — *a real FIFO dequeue, unlike step
      2.3's set-scan*
   2. Set currently running task ← oldestMicrotask. —
   3. **`[S]`** **Run `oldestMicrotask`.** — *promise reactions, `queueMicrotask` callbacks,
     `notify mutation observers`, the custom-element **backup element queue** drain. The spec's own
     note: this "might involve invoking scripted callbacks, which eventually calls the clean up
     after running script steps, which call this perform a microtask checkpoint algorithm again,
     which is why we use the performing a microtask checkpoint flag to avoid reentrancy."*
   4. Set currently running task ← null. —
4. **`[S]`** For each environment settings object whose responsible event loop is this one,
   **notify about rejected promises** — HTML §8.1.4.7: steps 1–3 clone and empty the
   about-to-be-notified list, then step 4 **queues a global task on the DOM manipulation task
   source** which **fires `unhandledrejection`** at the global. *The firing is in a later task, not
   here; this step itself only queues. Marked `[S]` because the queued task's author code is the
   whole point and a machine has to be able to be parked across it.*
5. **Cleanup Indexed Database transactions.** — no user code (it can fire `complete`/`abort` events
   via queued tasks).
6. **Perform `ClearKeptObjects()`.** — no user code. *(The observable: a value a `WeakRef.deref()`
   returned stays alive until this runs.)*
7. Set performing a microtask checkpoint ← false. —
8. Record timing info for microtask checkpoint. —

### 5.5 The other place a checkpoint happens — HTML §8.1.4.4 "Calling scripts"

`clean up after running script(settings)`:
1. Assert settings's realm execution context is the running JavaScript execution context. —
2. Remove it from the JavaScript execution context stack. —
3. **`[S]`** **If the JavaScript execution context stack is now empty, perform a microtask
   checkpoint.** — *"(If this runs scripts, these algorithms will be invoked reentrantly.)"*

This is the rule "microtasks run when the JS stack empties", stated normatively. It is **not** step
2.8 of the processing model — 2.8 is a second, unconditional checkpoint after the task's steps
finish. Both exist.

### 5.6 `spin the event loop` — HTML §8.1.7.3

Worth naming because it is the one way a **microtask becomes a task**: 1. `task` ← the currently
running task (which "could be a microtask"); 2. `task source` ← its source; 3–4. copy and **empty the
JavaScript execution context stack**; 5. **`[S]`** perform a microtask checkpoint (a no-op if
`task` is a microtask); 6. in parallel, wait for the goal, then queue a task on `task source` that
restores the stack and continues. This is the note attached to processing-model step 2.1 about a
task queue to which the microtask task source is associated.

### 5.7 Headless note

Nothing here is device-dependent except the **refresh rate** in step 5's `computeDeadline` and the
existence of a **rendering opportunity** in Algorithm 6. The spec explicitly leaves both to the UA
("The refresh rate can be hardware- or implementation-specific"). Modeling a fixed 60 Hz and a
document that always has a rendering opportunity is a defensible UA choice, not an approximation of
an unknown — and it makes `requestAnimationFrame` a real, running state machine with no display.

**Suspension points in Algorithm group 5: 7.**
(Processing model step 2.6 perform the task's steps; step 2.8 the checkpoint; step 6's worker
animation frame callbacks. Checkpoint step 3.3 run a microtask; step 4 notify about rejected
promises. `clean up after running script` step 3. `spin the event loop` step 5.)

---

## 6. "Update the rendering" — HTML §8.1.7.3, **not** §14.3

### 6.0 A correction to the brief, stated rather than picked silently

**There is no HTML §14.3 "update the rendering".** In the HTML Living Standard as read:

- **§14 is "The XML syntax"**; **§15 is "Rendering"** (§15.1 Introduction, §15.2 The CSS user agent
  style sheet and presentational hints, **§15.3 Non-replaced elements**, …). §15 is a **CSS
  requirements** section — it defines the UA stylesheet and presentational hints. It contains no
  algorithm named "update the rendering". Older editions numbered Rendering as §14; the brief's
  "§14.3" most likely comes from that older numbering, where §14.3 was likewise "Non-replaced
  elements".
- **"Update the rendering" is defined in §8.1.7.3 "Processing model"**, in the *in-parallel* half of
  the window event loop, as the steps of a task queued on the **rendering task source**. Its
  definition id is `#update-the-rendering`.

### 6.1 The in-parallel loop that schedules it — HTML §8.1.7.3

A window event loop `eventLoop` must also run, in parallel, for as long as it exists:

1. **Wait until at least one navigable whose active document's agent's event loop is `eventLoop`
   might have a rendering opportunity.** — the scheduling seam; no user code.
2. Set `eventLoop`'s **last render opportunity time** ← the unsafe shared current time. —
3. **For each navigable that has a rendering opportunity, queue a global task on the rendering task
   source given that navigable's active window to _update the rendering_:** — the 23 steps below are
   that task's steps. *(The spec's note: this can queue redundant calls, which are harmless because
   the "Unnecessary rendering" step removes them.)*

**A rendering opportunity is UA-determined** ("the user agent might decide to drop that page to a
much slower 4 rendering opportunities per second, or even less"). It is a modeled quantity, not an
unknowable one.

### 6.2 The 23 steps of "update the rendering", in order

1. `frameTimestamp` ← eventLoop's last render opportunity time. —
2. **`docs`** ← all **fully active** `Document`s whose agent's event loop is eventLoop, sorted so
   that **a container document precedes the documents it contains**, and siblings follow
   **shadow-including tree order of their navigable containers**. Every "for each doc of docs" step
   below uses this order. —
3. **Filter non-renderable documents:** remove any doc that is render-blocked, whose visibility state
   is `"hidden"`, whose rendering is suppressed for view transitions, or whose node navigable does
   not currently have a rendering opportunity. —
4. **Unnecessary rendering:** remove any doc for which the UA believes updating the rendering would
   have no visible effect **and** whose map of animation frame callbacks is empty. —
5. Remove any doc the UA prefers to skip for other reasons. — *(the spec names the use case: coalesce
   timer callbacks with only microtask checkpoints interleaved and no animation frame callbacks
   between them)*
6. **`[S]`** For each doc: **reveal doc.** — HTML §7.4.6.3: **fires `pagereveal`** at the relevant
   global (a `PageRevealEvent` carrying the `viewTransition`). **Author code.**
7. **`[S]`** For each doc: **flush autofocus candidates** for doc if its node navigable is a
   **top-level traversable**. — HTML §6.6's algorithm, whose step 5.11.3 is "**Run the focusing
   steps** for target" → fires `blur`/`focusout`/`focus`/`focusin`. **Author code.**
8. **`[S]`** For each doc: **run the resize steps** [CSSOM VIEW §13.1]. —
   1. If the viewport's width or height changed since the last run → **fire `resize`** at the
      `Window`.
   2. If the `VisualViewport`'s scale/width/height changed → **fire `resize`** at the
      `VisualViewport`. **Author code.**
9. **`[S]`** For each doc: **run the scroll steps** [CSSOM VIEW §13.2]. —
   1. For each scrolling box that was scrolled: resolve its doc/target, run the update
      scrollsnapchange targets steps, and append `(target, "scrollend")` to doc's **pending scroll
      events** if not already there.
   2. For each `(target, type)` in doc's pending scroll events, **in the order they were added**:
      **fire** `scroll`/`scrollend` (bubbling at a Document) or
      `scrollsnapchange`/`scrollsnapchanging`. **Author code.**
10. **`[S]`** For each doc: **evaluate media queries and report changes** [CSSOM VIEW §4.2]. —
    "For each `MediaQueryList` object target that has doc as its document, **in the order they were
    created, oldest first**: if target's matches state has changed since the last time these steps
    were run, **fire `change`**" with `MediaQueryListEvent`. **Author code.** *(This is the step
    `matchMedia().onchange` runs from; the `.matches` value it reports is a real computed value from
    the modeled viewport, which is why a `matchMedia` that returns opaque breaks this step rather
    than merely under-reporting.)*
11. **`[S]`** For each doc: **update animations and send events** [WEB ANIMATIONS §4.4], passing the
    relative high resolution time of `frameTimestamp`. — internally:
    1. Update the current time of all timelines (which updates animations, runs "update an
       animation's finished state", and **queues animation events**). —
    2. Remove replaced animations. —
    3. **`[S]`** **Perform a microtask checkpoint.** — *explicitly, "to ensure that any microtasks
      queued up as a result of resolving or rejecting Promise objects as part of updating timelines
      in the previous step, run their callbacks prior to dispatching animation events". A full
      microtask drain **inside** step 11 of update-the-rendering.*
    4–6. Copy and clear the pending animation event queue; **stable sort** by scheduled event time,
      then composite order. —
    7. **`[S]`** **Dispatch each event** at its target — `animationstart`/`animationend`/
      `animationiteration`/`animationcancel`, `transitionrun`/`transitionstart`/`transitionend`/
      `transitioncancel`, `finish`/`cancel`/`remove`. **Author code.**
12. **`[S]`** For each doc: **run the fullscreen steps** [FULLSCREEN]. — fires
    `fullscreenchange`/`fullscreenerror` and resolves the `requestFullscreen()` promise.
    **Author code.**
13. **`[S]`** For each doc, for each `CanvasRenderingContext2D`/`OffscreenCanvasRenderingContext2D`
    whose backing storage has been lost, run the **context lost steps**:
    1. `canvas` ← the context's canvas (or the associated `OffscreenCanvas`). —
    2. Set context lost ← true. —
    3. Reset the rendering context to its default state. —
    4. **`[S]`** `shouldRestore` ← **fire `contextlost`** at canvas, **cancelable**. **Author code,
       and its return value steers the algorithm.**
    5. If shouldRestore is false → abort. —
    6. Attempt to restore by creating a backing storage; abort on failure. —
    7. Set context lost ← false. —
    8. **`[S]`** **Fire `contextrestored`** at canvas. **Author code.**
14. **`[S]`** For each doc: **run the animation frame callbacks** with the relative high resolution
    time of `frameTimestamp`. — HTML §8.9: 1. `callbacks` ← the target's map of animation frame
    callbacks; 2. `callbackHandles` ← **the keys, snapshotted**; 3. for each handle **if it still
    exists in callbacks**: take the callback, **remove it from the map**, then **invoke it with
    « now » and "report"**. *(The snapshot-then-recheck is what makes a `requestAnimationFrame`
    registered from inside a rAF callback run on the **next** frame, and a `cancelAnimationFrame`
    from inside one take effect on this frame. A machine must not re-read the map.)* **Author code.**
15. `unsafeStyleAndLayoutStartTime` ← the unsafe shared current time. —
16. **For each doc — the style/layout + ResizeObserver loop:**
    1. `resizeObserverDepth` ← 0. —
    2. **While true:**
       1. **Recalculate styles and update layout for doc.** — **(walk)**, no user code.
       2. `hadInitialVisibleContentVisibilityDetermination` ← false. —
       3. For each element with `content-visibility: auto`: determine proximity to the viewport, and
          record whether this was the element's **initial** determination. — **(walk)**
       4. If any initial determination made an element relevant to the user → **`continue`** (re-run
          from 16.2.1 so the determination is reflected in the style/layout of this same frame). —
       5. **Gather active resize observations at depth `resizeObserverDepth`** for doc [RESIZE
          OBSERVER §3.4.1] — for each observer, clear `[[activeTargets]]`/`[[skippedTargets]]`, and
          for each active observation put it in activeTargets if its **calculated depth** exceeds
          `resizeObserverDepth`, else in skippedTargets. — **(walk)**, no user code.
       6. If doc **has active resize observations**:
          1. **`[S]`** `resizeObserverDepth` ← **broadcast active resize observations** for doc
             [RESIZE OBSERVER §3.4.5] — which, per observer with a non-empty activeTargets, builds
             `ResizeObserverEntry` objects, records `lastReportedSizes`, tracks the shallowest
             target depth, and **invokes `observer.[[callback]]` with (entries, observer) and
             `this` = observer, reporting any exception**. **Author code — and it runs inside a
             `while (true)`.**
          2. **`continue`** → back to 16.2.1, re-running style and layout. *(This is the
            ResizeObserver loop: an author callback that resizes an observed element causes another
            layout pass and another callback, at strictly increasing depth.)*
       7. Otherwise **break**. —
    3. If doc **has skipped resize observations** → **`[S]`** **deliver resize loop error** [RESIZE
       OBSERVER §3.4.6]: create an `ErrorEvent` with message "ResizeObserver loop completed with
       undelivered notifications." and **report the exception** — which fires `error` at the global,
       so `window.onerror` runs. **Author code.**
17. **`[S]`** For each doc: if the **focused area** of doc is not a focusable area, **run the
    focusing steps for doc's viewport** and clear the navigation API's "focus changed during ongoing
    navigation". — the spec's own note: "**This will usually fire `blur` events, and possibly
    `change` events.**" **Author code.**
18. **`[S]`** For each doc: **perform pending transition operations** [CSS VIEW TRANSITIONS §7.2].
    — 1. if the active view transition's phase is `"pending-capture"` → **setup view transition**,
    whose step 2 flushes the update callback queue and whose later steps **call the author's
    `ViewTransitionUpdateCallback`**; 1.2 otherwise if `"animating"` → handle transition frame.
    **Author code**, plus the `ready`/`updateCallbackDone`/`finished` promise settlements.
19. For each doc: **run the update intersection observations steps** [INTERSECTION OBSERVER §3.2.2]
    with the relative high resolution time. — **(walk)**, and **NOT author code.** For each observer
    and each target it computes the geometry, decides the threshold index, and **queues an
    `IntersectionObserverEntry`**, which calls **queue an intersection observer task** — that
    algorithm sets a flag and **queues a task on the IntersectionObserver task source** to **notify
    intersection observers**. **The observer callbacks are invoked from that later task
    (§3.2.5 step 3.5), not from this step.** This is the difference between IntersectionObserver and
    ResizeObserver in the rendering loop, and getting it wrong changes observable ordering.
20. For each doc: **record rendering time** given `unsafeStyleAndLayoutStartTime`. — queues
    performance entries; no user code.
21. For each doc: **mark paint timing**. — queues performance entries; no user code.
22. For each doc: **update the rendering or user interface** of doc and its node navigable to reflect
    the current state. — **the actual paint.** No user code, and **the only step with no headless
    equivalent** — everything before it computes values that exist whether or not anything is drawn.
23. For each doc: **process top layer removals**. — no user code.

After the task's steps finish, the event loop is back at processing-model step 2.7/2.8, so a
**microtask checkpoint** follows the whole of update-the-rendering.

### 6.3 Headless note

Only step 22 needs a device. Steps 1–21 and 23 compute defined values from the modeled viewport and
layout: `resize` needs a viewport width/height, `scroll` needs scrolling boxes, `evaluate media
queries` needs a real `matches`, `broadcast active resize observations` needs real box sizes, and
IntersectionObserver needs real intersection rectangles. A `getComputedStyle`/layout that answers
opaque does not make these steps unknown — it makes them **wrong**, because their author callbacks
receive fabricated geometry and branch on it.

**Suspension points in Algorithm group 6: 17.**
(Steps 6, 7, 8, 9, 10, 11 — plus 11's internal microtask checkpoint and its event dispatch as two
distinct points — 12, 13 — plus 13's `contextlost` and `contextrestored` as two distinct points —
14, 16.2.6.1, 16.3, 17, 18. Steps 1–5, 15, 19–23 have none.)

---

## 7. Web IDL §3.2 "JavaScript type mapping" — `DOMString`, `USVString`, `sequence<T>`, `record<K,V>`

Section numbers: **§3.2.10 DOMString**, §3.2.11 ByteString, **§3.2.12 USVString**,
**§3.2.21 Sequences — `sequence<T>`** (with **§3.2.21.1 Creating a sequence from an iterable**),
**§3.2.23 Records — `record<K, V>`**, §3.2.25 Union types. (Older editions of Web IDL numbered these
differently — the type list in §2.13 gained `async_sequence<T>` at §2.13.29 and §3.2.22, which
shifted `record` from §3.2.22 to §3.2.23. Cite the title as well as the number.)

### 7.1 `DOMString` — Web IDL §3.2.10

Conversion of a JavaScript value `V`:

1. If `V` is **null** and the IDL type carries **`[LegacyNullToEmptyString]`**, return the empty
   string. — no user code. *(This is why `el.innerHTML = null` clears rather than writing
   `"null"`.)*
2. **`[S]`** `x` ← **? ToString(V)**. — **ECMAScript §7.1.17**, which for an Object is
   `ToPrimitive(V, string)` → **`V[Symbol.toPrimitive]`** if present (a **user function**, and the
   `[[Get]]` of the property is itself a Proxy `get` trap), else `OrdinaryToPrimitive` calling
   **`toString`** then **`valueOf`**. Every one of those is author code that can loop, throw, or
   mutate the DOM.
3. Return the IDL `DOMString` for the same **sequence of code units**. — no user code. *(Code units,
   not scalar values: a lone surrogate survives a `DOMString` intact.)*

The reverse direction (IDL → JS) is the identical String value; no user code.

**Every `DOMString` argument in Algorithms 1–4 goes through step 2.** That is the single most
frequent suspension point in this entire document.

### 7.2 `USVString` — Web IDL §3.2.12

1. **`[S]`** `string` ← the result of converting `V` **to a `DOMString`** — i.e. all of §7.1,
   including its ToString.
2. Return the `USVString` that is the result of **converting `string` to a sequence of scalar
   values** — Infra's algorithm, which **replaces each unpaired surrogate with U+FFFD**. — no user
   code.

The reverse direction is `S` unchanged. The engine-side note in `quickjs-step.h` for
`JS_ToScalarValueString` is the same fact: step 2's replacement cannot be done outside the engine,
because a host only sees the UTF-8 the C-string conversion already chose.

### 7.3 `sequence<T>` — Web IDL §3.2.21

Conversion of a JavaScript value `V`:

1. If `V` is not an Object → **TypeError**. — no user code.
2. **`[S]`** `method` ← **? GetMethod(V, %Symbol.iterator%)**. — a `[[Get]]` of a well-known-symbol
   property: **an accessor or a Proxy `get` trap is author code**, and `GetMethod` additionally
   throws a TypeError for a non-callable non-undefined result.
3. If `method` is undefined → **TypeError**. — no user code.
4. **`[S]` (delegated)** Return the result of **creating a sequence from `V` and `method`**. —

**`create a sequence from an iterable(iterable, method)`** — Web IDL §3.2.21.1:

1. **`[S]`** `iteratorRecord` ← **? GetIteratorFromMethod(iterable, method)** — **calls `method`**
   (author code), then **`[[Get]]`s `next`** off the returned iterator (author code).
2. `i` ← 0. —
3. **Repeat:** — **(walk)**, unbounded: the iterator decides how many times this runs
   1. **`[S]`** `next` ← **? IteratorStepValue(iteratorRecord)** — **calls `iterator.next()`**
      (author code), then **`[[Get]]`s `done`** and **`[[Get]]`s `value`** off the result object
      (author code, twice). On an abrupt completion from the `value` read the iterator record's
      `[[Done]]` is set, which is what suppresses a redundant `return()`.
   2. If `next` is **done**, return the sequence of length `i`. —
   3. **`[S]`** `S[i]` ← the result of **converting `next` to an IDL value of type `T`** — which for
      `T = DOMString` is §7.1's ToString, for `T` an interface type is a brand check, and for `T` a
      dictionary is a further pile of `[[Get]]`s. **Author code, once per element.**
   4. `i` ← `i + 1`. —

**The IDL→JS direction** (§3.2.21, second algorithm) builds a fresh `Array` with
`CreateDataPropertyOrThrow` per element — **no user code**, because it is a plain data property on a
brand-new ordinary Array.

### 7.4 `record<K, V>` — Web IDL §3.2.23

Conversion of a JavaScript value `O`:

1. If `O` is not an Object → **TypeError**. — no user code.
2. `result` ← a new empty `record<K, V>`. — no user code.
3. **`[S]`** `keys` ← **? `O.[[OwnPropertyKeys]]()`** — **on a Proxy this is the `ownKeys` trap**,
   author code, and the invariant check that follows it is the engine's.
4. **For each `key` of `keys`:** — **(walk)**, length decided by the object
   1. **`[S]`** `desc` ← **? `O.[[GetOwnProperty]](key)`** — **on a Proxy this is the
      `getOwnPropertyDescriptor` trap**, author code.
   2. If `desc` is not undefined **and `desc.[[Enumerable]]` is true**:
      1. **`[S]`** `typedKey` ← `key` converted to IDL type `K` — for `K = DOMString`/`USVString`
         this is §7.1/§7.2 on a String or Symbol key. *(The spec's own note: two distinct keys can
         collide in `result` when `K` is `USVString` and they differ only in unpaired surrogates.)*
      2. **`[S]`** `value` ← **? Get(O, key)** — **an accessor or the Proxy `get` trap**, author
         code.
      3. **`[S]`** `typedValue` ← `value` converted to IDL type `V` — author code again for a string
         or dictionary `V`.
      4. `result[typedKey]` ← `typedValue`. —
5. Return `result`. — no user code.

**The operation SEQUENCE is observable and is pinned by tests** — `ownKeys`, then per key
`getOwnPropertyDescriptor` **then** `get`. Skipping the descriptor does not merely lose the trap; it
silently admits **non-enumerable** properties, which is observable as a count. (`quickjs-step.h`'s
`keys_phase`/`desc_phase` fields exist because both sub-sequences are in flight in the same stage.)

**The IDL→JS direction** builds an `OrdinaryObjectCreate(%Object.prototype%)` and
`CreateDataProperty` per entry — no user code.

### 7.5 The union that decides which of the above runs — Web IDL §3.2.25

Relevant because `(DOMString or ElementCreationOptions)`, `(TrustedType or DOMString)` and
`(TrustedHTML or DOMString)` all appear in Algorithms 1, 2 and 4, and the union's step order decides
whether the ToString happens at all:

1. undefined member + `V` is undefined → the undefined value. —
2. nullable member + `V` is null/undefined → null. —
3. `types` ← the flattened member types. —
4. `V` null/undefined + a dictionary member → convert to that dictionary. —
5. **`V` is a platform object** implementing an interface member → that reference. — *this is the arm
   a real `TrustedHTML` takes, and it runs no user code.*
6–9. ArrayBuffer / SharedArrayBuffer / DataView / typed-array arms. —
10. `IsCallable(V)` + a callback-function member → that callback. —
11. **`V` is an Object:**
    1. async sequence member → **`[S]`** `GetMethod(V, %Symbol.asyncIterator%)`, then **`[S]`**
       `GetMethod(V, %Symbol.iterator%)`. —
    2. **sequence member → `[S]` `GetMethod(V, %Symbol.iterator%)`; if defined, create a sequence
       (§7.3).** —
    3. frozen array member → the same shape. —
    4. dictionary member → **`[S]`** convert to that dictionary (a `[[Get]]` per member, in
       **lexicographical member order**). —
    5. record member → **`[S]`** convert to that record (§7.4). —
    6. callback interface member → that conversion. —
    7. `object` member → the reference. —
12. `V` is a Boolean + a boolean member → boolean. —
13. `V` is a Number + a numeric member → that numeric type. —
14. …then bigint, then **string/enum/numeric/boolean fallbacks**, where the **`DOMString` arm's
    ToString (§7.1) finally runs**. —

The consequence for Algorithm 1: `document.createElement("div", {is: x})` takes step 11.4 and does
**two `[[Get]]`s in lexicographical order** — `customElementRegistry`, then `is` — before
`createElement`'s own step 1 runs. `document.createElement("div", "foo")` takes step 14 and does one
ToString. A machine cannot assume which.

### 7.6 Headless note

None of §3.2 is device-dependent; it is pure ECMAScript. The only "no observable result" case is the
IDL→JS direction of `sequence`/`record`, which is a fresh ordinary object and therefore fully
defined.

**Suspension points in Algorithm group 7: 14.**
(DOMString step 2. USVString step 1. `sequence<T>` steps 2 and 4. `create a sequence from an
iterable` step 1 — which is two: `method` call and `next` `[[Get]]` — and step 3.1 — which is three:
`next()` call, `done` read, `value` read — and step 3.3. `record<K,V>` steps 3, 4.1, 4.2.1, 4.2.2,
4.2.3. Union steps 11.1/11.2/11.3 `GetMethod`, 11.4 dictionary, 11.5 record, 14 ToString — counted
as the four distinct shapes they delegate to, already listed.)

---

## 8. Summary — suspension points per algorithm group

| # | Algorithm group | Spec section(s) | Suspension points | The ones that will surprise an implementer |
| --- | --- | --- | --- | --- |
| 1 | Document: `createElement`, `createElementNS`, `getElementById`, ParentNode lookups | DOM §4.5, §4.2.4, §4.2.6, §4.9 (`create an element`), HTML §4.13.5/§4.13.6 | **18** | `createElement` passes `synchronousCustomElements = true`, so the author's constructor runs **inside** it; `getElementById` is §4.2.4, not §4.5; "report an exception" fires an `error` event |
| 2 | Element attributes: `setAttribute`, `setAttributeNS`, `getAttribute`, `removeAttribute`, attribute change steps | DOM §4.9, HTML §4.13.6, Trusted Types §3.4/§3.5 | **13** | `setAttribute` **step 3** runs the page's Trusted Types default policy; `handle attribute changes` runs **zero** author code — `attributeChangedCallback` is enqueued there and invoked by the `[CEReactions]` wrapper; `setAttribute` and `setAttributeNS` put the Trusted Types call at **different step positions** |
| 3 | Mutation algorithms: pre-insert, insert, append, replace, remove, move | DOM §4.2.3, §4.3.2, §4.5 (adopt), HTML §4.12.1 | **8** | `insert` **step 9** (children changed steps) and **step 12** (post-connection steps) both execute scripts; the **insertion steps, moving steps and removing steps may not**; every custom-element callback in this algorithm is enqueued, never called |
| 4 | Reflection + `innerHTML`/`outerHTML` + fragment parsing | HTML §2.6.1, §8.5.4, §8.5.5, §13.4 | **11** | reflection is §2.6.1 not §3.2.2; `innerHTML` is HTML §8.5.4 (DOM Parsing was merged into HTML); the setter's **step 1** is Trusted Types; the fragment parse defaults to **`Inert`**, so no `<script>` runs — but custom element constructors still can; `outerHTML` parses with **`parent`** as context, not `this` |
| 5 | Event loop: microtask checkpoint + task processing model | HTML §8.1.7.1–§8.1.7.3, §8.1.4.4 | **7** | a task queue is a **set** and the model takes the first **runnable** task; the microtask queue is not a task queue; there are **two** checkpoints (processing-model step 2.8, and `clean up after running script` step 3 when the JS stack empties); step 4 of the checkpoint only **queues** `unhandledrejection` |
| 6 | Update the rendering | HTML §8.1.7.3 (**not** §14.3 — §15 is Rendering and holds no such algorithm) | **17** | step **11** performs a **full microtask checkpoint** inside itself; step **16** is a `while (true)` around style+layout that re-enters after every ResizeObserver callback; step **19** (IntersectionObserver) runs **no** callbacks — they are a later task; step **14** snapshots the rAF handles before invoking |
| 7 | Web IDL conversions: `DOMString`, `USVString`, `sequence<T>`, `record<K,V>` (+ the unions that select them) | Web IDL §3.2.10, §3.2.12, §3.2.21, §3.2.21.1, §3.2.23, §3.2.25 | **14** | the `record` operation order — `ownKeys`, then per key `getOwnPropertyDescriptor`, **then** `get` — is observable; the enumerable check is not an optimization; a `sequence` element conversion is author code **per element**, inside the iteration |
| | **Total** | | **88** | |

### 8.1 The four facts that carry the most weight for stage placement

1. **The binding prologue is part of the algorithm.** Every `DOMString`, every union, every
   dictionary member is a `[[Get]]` or a `ToString` that runs before step 1. A machine whose stage 0
   is "the prologue has not run" must place each of those in its own stage.
2. **Enqueue is not invoke.** `handle attribute changes`, `insert`, `remove`, `adopt` and `move`
   enqueue every custom-element reaction and every mutation record. The reactions are invoked at the
   `[CEReactions]` boundary (or the backup-queue microtask); the records at the mutation observer
   microtask. Those two places are the suspension points; the enqueues are not.
3. **DOM draws the script line explicitly, and it is not where intuition puts it.** The insertion
   steps, moving steps and removing steps may not execute JavaScript. The **children changed steps**
   and the **post-connection steps** may, and HTML uses both to run `<script>`.
4. **Two algorithms contain a nested full drain.** Update-the-rendering step 11.3 performs a
   microtask checkpoint, and step 16.2 is an unbounded `while (true)` whose body invokes author
   callbacks. Neither can be a single stage.

---

## 9. DOM §4.9 attributes IN FULL — the attribute model, the two key spaces, and every entry point into them

**Why this section exists beside Algorithm group 2.** Group 2 covered the four mutators an
implementer reaches for first (`getAttribute`, `setAttribute`, `setAttributeNS`, `removeAttribute`)
and the change-steps they run. It did not cover the *model*: the `Attr` node, the `NamedNodeMap`,
the node-valued entry points (`getAttributeNode`/`setAttributeNode`/`removeAttributeNode`), the
`Document` factories, or DOM §1.4's validation predicates. Those are where the two key spaces
(qualified name vs (namespace, local name)) actually diverge, and where the live text has moved
furthest from what a reasonable person remembers. Nothing in group 2 is contradicted below; §9.3
and §9.4 restate the primitives group 2 quoted so that this section reads standalone.

**Network was available.** Everything below was read from the live standards on **2026-08-10**:

| Standard | Source | Version read |
| --- | --- | --- |
| WHATWG DOM | `https://dom.spec.whatwg.org/` | Living Standard, Last Updated **18 July 2026** |
| WHATWG Infra | `https://infra.spec.whatwg.org/` | Living Standard, Last Updated **17 July 2026** |
| WHATWG HTML | `https://html.spec.whatwg.org/multipage/parsing.html` | Living Standard, Last Updated **20 July 2026** (`last-modified: Mon, 20 Jul 2026 08:01:45 GMT`) |
| Trusted Types | `https://w3c.github.io/trusted-types/dist/spec/` | Editor's Draft, read 2026-08-10 |

Step numbers are the standards' own list numbering as of those dates.

### 9.0 Five things the live text says that a reasonable person would remember differently

Stated first, because they are the point of the section. Each is expanded, with its deciding step,
further down.

1. **There is no `validate` algorithm any more, and name validation is no longer XML-based.**
   DOM §1.4 is titled **"Name validation"** and contains exactly three predicates plus
   `validate and extract`. The old standalone "validate" (which checked `qualifiedName` against
   XML's `Name`/`QName` productions) is **gone**, and the spec says so in its own note: *"Various
   APIs in this specification used to validate namespace prefixes, attribute local names, element
   local names, and doctype names more strictly. […] This was found to be annoying for web
   developers, especially since it meant there were some names that could be created by the HTML
   parser, but not by DOM APIs. So, the validations have been loosened to just those described
   above."* Concretely: **`el.setAttribute("1abc", "x")` does not throw**, and neither does
   `el.setAttribute("<", "x")` or `el.setAttribute("a:b", "x")`. The only rejected code points in an
   attribute local name are ASCII whitespace, U+0000, `/`, `=`, `>`. See §9.2.
2. **DOM §1.5 "Namespaces" no longer exists.** The six namespace constants live in **Infra §8**.
   DOM §1 is now: 1.1 Trees, 1.2 Ordered sets, 1.3 Selectors, 1.4 Name validation — and stops.
   Any citation of "DOM §1.5" for the XML/XMLNS/XLink namespace strings is stale.
3. **`set an existing attribute value` checks the same field twice, deliberately, around a
   suspension point** (steps 1 and 4 both test "attribute's element is null", with the Trusted Types
   call at step 3 between them). This is the spec instructing you that author code runs in the
   middle and that the field must be **re-read after resuming**. It is the only algorithm in §4.9
   that re-validates across an `[S]`, and it is exactly the shape a step machine gets wrong by
   caching. See §9.4.10.
4. **Trusted Types on attributes is not a short URL/HTML list — it includes every event handler
   content attribute.** `get Trusted Type data for attribute` returns `TrustedScript` for *any*
   attribute that is the name of an event handler content attribute on an element in the HTML, SVG
   or MathML namespace, before it even consults its 4-row table. So
   **`el.setAttribute("onclick", s)` invokes the page's default policy.** See §9.5.4.
5. **Reflection does not go through Trusted Types, but `Attr.value`, `Attr.nodeValue` and
   `Attr.textContent` do.** `Element.id`/`className`/`slot` are defined to *reflect*, whose setter
   steps are **`set an attribute value`** — no TT call anywhere on that path. The three `Attr`-side
   setters all route through **`set an existing attribute value`**, whose step 3 is the TT call.
   `el.id = x` cannot run a policy; `el.getAttributeNode("id").value = x` can.

### 9.1 The data model — DOM §4.9 / §4.9.2

An **attribute** (an `Attr` node) has exactly five fields:

- **namespace** — null or a **non-empty** string
- **namespace prefix** — null or a **non-empty** string
- **local name** — a **non-empty** string
- **value** — a string
- **element** — null or an element

(The spec's own aside: *"If designed today they would just have a name and value."*)

An attribute's **qualified name** is *"its local name if its namespace prefix is null, and its
namespace prefix, followed by `":"`, followed by its local name, otherwise."* The spec adds:
*"User agents could have this as an internal slot as an optimization."*

An **element** has an **attribute list**, *"which is a list exposed through a `NamedNodeMap`. Unless
explicitly given when an element is created, its attribute list is empty."* An element **has an
attribute** A *"if its attribute list contains A"*.

**An `A` attribute** is defined as *"an attribute whose local name is `A` and whose namespace and
namespace prefix are null."* This three-part definition is what phrases like "an `id` attribute" and
"a `class` attribute" mean throughout DOM and HTML — a namespaced `svg:id` is **not** an `id`
attribute.

**`create an attribute(document, localName [, namespace = null [, prefix = null [, value = "" ]]])`**
— DOM §4.9.2:

1. Let `attribute` be the result of **creating a node that implements `Attr`**, given `document`. —
2. Set `attribute`'s namespace to `namespace`, namespace prefix to `prefix`, local name to
   `localName`, and value to `value`. —
3. Return `attribute`. —

**No validation happens here.** `create an attribute` is a raw constructor; every caller that needs
a name check does it before calling. Note the parameter order — `(document, localName, namespace,
prefix, value)` — `localName` second, and `namespace` **before** `prefix`.

`DCHECK` targets this section hands you directly: namespace non-null implies non-empty; prefix
non-null implies non-empty; local name always non-empty; prefix non-null implies namespace non-null
(enforced by `validate and extract` step 8, and by the fact that `create an attribute`'s only
callers that pass a prefix are `setAttributeNS`/`createAttributeNS`, both post-validation).

### 9.2 DOM §1.4 "Name validation" — the character sets, quoted exactly

> A string is a **valid namespace prefix** if its length is at least 1 and it does not contain ASCII
> whitespace, U+0000 NULL, U+002F (`/`), or U+003E (`>`).

> A string is a **valid attribute local name** if its length is at least 1 and it does not contain
> ASCII whitespace, U+0000 NULL, U+002F (`/`), U+003D (`=`), or U+003E (`>`).

> A string `name` is a **valid element local name** if the following steps return true:
> 1. If `name`'s length is 0, then return false. —
> 2. If `name`'s 0th code point is an **ASCII alpha**:
>    1. If `name` contains ASCII whitespace, U+0000 NULL, U+002F (`/`), or U+003E (`>`), then return
>       false. —
>    2. Return true. —
> 3. If `name`'s 0th code point is not U+003A (`:`), U+005F (`_`), or in the range U+0080 to
>    U+10FFFF, inclusive, then return false. —
> 4. If `name`'s subsequent code points, if any, are not ASCII alphas, ASCII digits, U+002D (`-`),
>    U+002E (`.`), U+003A (`:`), U+005F (`_`), or in the range U+0080 to U+10FFFF, inclusive, then
>    return false. —
> 5. Return true. —

The spec supplies its own reference implementation, quoted verbatim and worth copying into a test:

```
/^(?:[A-Za-z][^\0\t\n\f\r\u0020/>]*|[:_\u0080-\u{10FFFF}][A-Za-z0-9-.:_\u0080-\u{10FFFF}]*)$/u
```

and its own explanation: *"The intention is to allow any name that is possible to construct using
the HTML parser (the branch where the first code point is an ASCII alpha), plus some additional
possibilities. For those additional possibilities, the ASCII range is restricted for historical
reasons, but beyond ASCII anything is allowed."*

> A string is a **valid doctype name** if it does not contain ASCII whitespace, U+0000 NULL, or
> U+003E (`>`). The empty string is a valid doctype name.

**ASCII whitespace** is Infra's, and it has **five** members: *"U+0009 TAB, U+000A LF, U+000C FF,
U+000D CR, or U+0020 SPACE."* Infra's own note is the trap: *"The XML, JSON, and parts of the HTTP
specifications exclude U+000C FF in their definition of whitespace"* — an implementation that reuses
an XML or JSON whitespace predicate here accepts a form feed in an attribute name and is wrong.
**To ASCII lowercase** a string is *"replace all ASCII upper alphas in the string with their
corresponding code point in ASCII lower alpha"* — U+0041 to U+005A only, never a Unicode case fold,
never locale-sensitive.

Three asymmetries between the three predicates, each of which a shared helper would erase:

- `=` is forbidden in an **attribute** local name and **permitted** in an **element** local name.
- `:` is permitted in **both** (which is why `setAttribute("a:b", v)` succeeds — see §9.11 Q3).
- The attribute and prefix predicates are pure character-set tests with no first-code-point rule;
  only the **element** predicate has one, and it is the only one of the three whose ASCII branch and
  non-ASCII branch accept **different** subsequent character sets.

**`validate and extract(namespace, qualifiedName, context)`** — DOM §1.4. `context` is `"attribute"`
or `"element"`. Returns a `(namespace, prefix, localName)` triple.

1. If `namespace` is the empty string, then set it to null. —
2. Let `prefix` be null. —
3. Let `localName` be `qualifiedName`. —
4. If `qualifiedName` contains a U+003A (`:`):
   1. Set `prefix` to the part of `qualifiedName` before the **first** U+003A (`:`). —
   2. Set `localName` to the part of `qualifiedName` after the **first** U+003A (`:`). —
   3. If `prefix` is not a **valid namespace prefix**, then throw an **`InvalidCharacterError`**
      `DOMException`. —
5. Assert: `prefix` is either null or a valid namespace prefix. —
6. If `context` is `"attribute"` and `localName` is not a **valid attribute local name**, then throw
   an **`InvalidCharacterError`** `DOMException`. —
7. If `context` is `"element"` and `localName` is not a **valid element local name**, then throw an
   **`InvalidCharacterError`** `DOMException`. —
8. If `prefix` is non-null and `namespace` is null, then throw a **`NamespaceError`**
   `DOMException`. —
9. If `prefix` is `"xml"` and `namespace` is not the **XML namespace**, then throw a
   **`NamespaceError`** `DOMException`. —
10. If **either `qualifiedName` or `prefix`** is `"xmlns"` and `namespace` is not the **XMLNS
    namespace**, then throw a **`NamespaceError`** `DOMException`. —
11. If `namespace` is the **XMLNS namespace** and **neither `qualifiedName` nor `prefix`** is
    `"xmlns"`, then throw a **`NamespaceError`** `DOMException`. —
12. Return `(namespace, prefix, localName)`. —

No step here runs user code. Every throw is one of exactly two types, and step 4.1/4.2 split on the
**first** colon only — `"a:b:c"` yields prefix `"a"`, localName `"b:c"`, and `"b:c"` is a valid
attribute local name, so `setAttributeNS(ns, "a:b:c", v)` **succeeds**.

Steps 10 and 11 are a biconditional written as two one-way tests, and both halves check
`qualifiedName` **or** `prefix` — so the unprefixed `qualifiedName === "xmlns"` case is covered by
the same steps as the prefixed `"xmlns:foo"` case. This is why
`setAttributeNS(XMLNS_NS, "xmlns", v)` is legal and `setAttributeNS(null, "xmlns", v)` throws
`NamespaceError` (step 10), while `setAttribute("xmlns", v)` throws nothing at all and produces a
null-namespace attribute (§9.11 Q3).

The namespace constants (**Infra §8**, not DOM):

| Name | String |
| --- | --- |
| HTML namespace | `http://www.w3.org/1999/xhtml` |
| MathML namespace | `http://www.w3.org/1998/Math/MathML` |
| SVG namespace | `http://www.w3.org/2000/svg` |
| XLink namespace | `http://www.w3.org/1999/xlink` |
| XML namespace | `http://www.w3.org/XML/1998/namespace` |
| XMLNS namespace | `http://www.w3.org/2000/xmlns/` |

### 9.3 The two lookup primitives — DOM §4.9

**`get an attribute by name(qualifiedName, element)`** — the **qualified-name** key space:

1. If `element` is in the **HTML namespace** *and* its **node document is an HTML document**, then
   set `qualifiedName` to `qualifiedName` in **ASCII lowercase**. —
2. Return **the first attribute** in `element`'s attribute list whose **qualified name** is
   `qualifiedName`; otherwise null. —

**`get an attribute by namespace and local name(namespace, localName, element)`** — the
**(namespace, local name)** key space:

1. If `namespace` is the empty string, then set it to null. —
2. Return **the attribute** in `element`'s attribute list whose **namespace** is `namespace` **and
   local name** is `localName`, if any; otherwise null. —

Both are referred to in prose as *"getting an attribute"*; which of the two runs is decided purely
by the argument list at the call site (`(qualifiedName, element)` vs `(namespace, localName,
element)`). Neither runs user code.

The difference between step 2 of each is not cosmetic. The name-keyed lookup says **"the first"**,
because an element's attribute list may legitimately hold two attributes with the same qualified
name (e.g. `{ns: XLink, prefix: "xlink", local: "href"}` and `{ns: null, prefix: null, local:
"xlink:href"}`, both with qualified name `"xlink:href"`). The namespace-keyed lookup says **"the"**,
because `(namespace, localName)` is a **uniqueness key** on the list — a `DCHECK` target: appending
an attribute whose `(namespace, localName)` already occurs in the list must be impossible.

**`get an attribute value(element, localName [, namespace = null])`**:

1. Let `attr` be the result of **getting an attribute** given `namespace`, `localName`, and
   `element`. —
2. If `attr` is null, then return **the empty string**. —
3. Return `attr`'s value. —

Note the argument order differs from the primitive it calls (`element` first here, last there) and
that it keys on **(namespace, local name)** with `namespace` defaulting to null — this is the getter
half of reflection, so `el.id` is a null-namespace `id` lookup and returns `""` when absent, never
null.

### 9.4 The mutation primitives — DOM §4.9

#### 9.4.1 `set an attribute(attr, element)` — the `setAttributeNode` path

1. **`[S]`** Let `verifiedValue` be the result of calling **get trusted type compliant attribute
   value** with `attr`'s local name, `attr`'s namespace, `element`, and `attr`'s value.
   `[TRUSTED-TYPES]` —
2. If `attr`'s **element** is neither null nor `element`, throw an **`InUseAttributeError`**
   `DOMException`. —
3. Let `oldAttr` be the result of **getting an attribute** given `attr`'s namespace, `attr`'s local
   name, and `element`. — *(the (namespace, local name) key space)*
4. If `oldAttr` **is** `attr`, return `attr`. —
5. Set `attr`'s value to `verifiedValue`. —
6. If `oldAttr` is non-null, then **replace** `oldAttr` with `attr`. —
7. Otherwise, **append** `attr` to `element`. —
8. Return `oldAttr`. —

**The suspension point is step 1, before the validity check at step 2.** A page's Trusted Types
default policy therefore runs even for a call that is about to throw `InUseAttributeError`, and the
policy callback can itself move `attr` onto another element — so **step 2 must read `attr`'s element
after the resume, not a value captured before step 1**. Step 4's `oldAttr is attr` early return
(before the value is written) is what makes `el.setAttributeNode(el.getAttributeNode("x"))` a no-op
that still runs the policy.

#### 9.4.2 `set an attribute value(element, localName, value [, prefix = null [, namespace = null]])`

1. Let `attribute` be the result of **getting an attribute** given `namespace`, `localName`, and
   `element`. —
2. If `attribute` is null, then **append** the result of **creating an attribute** given `element`'s
   node document, `localName`, `namespace`, `prefix`, and `value` to `element`, and then return. —
3. **Change** `attribute` to `value`. —

**No Trusted Types call.** This is the algorithm reflection's setter steps use and the algorithm
`setAttributeNS` step 3 uses (the TT call in `setAttributeNS` is in `setAttributeNS` itself, at its
own step 2 — not here). Step 3 is the reason **the prefix of an existing attribute is never
updated**: the lookup at step 1 ignores prefix, and `change an attribute` writes only the value.
`el.setAttributeNS(XLINK, "a:href", v1)` then `el.setAttributeNS(XLINK, "b:href", v2)` leaves one
attribute whose prefix is still `"a"` and whose value is `v2`.

#### 9.4.3 `remove an attribute by name(qualifiedName, element)`

1. Let `attr` be the result of **getting an attribute** given `qualifiedName` and `element`. —
2. If `attr` is non-null, then **remove** `attr`. —
3. Return `attr`. —

#### 9.4.4 `remove an attribute by namespace and local name(namespace, localName, element)`

1. Let `attr` be the result of **getting an attribute** given `namespace`, `localName`, and
   `element`. —
2. If `attr` is non-null, then **remove** `attr`. —
3. Return `attr`. —

Both return null when nothing matched, and **neither throws**. The `NotFoundError` that
`removeNamedItem` produces is added by that wrapper (§9.6), not by these.

#### 9.4.5 `append an attribute(attribute, element)`

1. Append `attribute` to `element`'s **attribute list**. —
2. Set `attribute`'s **element** to `element`. —
3. Set `attribute`'s **node document** to `element`'s node document. —
4. **Handle attribute changes** for `attribute` with `element`, **null**, and `attribute`'s value. —

#### 9.4.6 `change an attribute(attribute, value)`

1. Let `oldValue` be `attribute`'s value. —
2. Set `attribute`'s value to `value`. —
3. **Handle attribute changes** for `attribute` with `attribute`'s **element**, `oldValue`, and
   `value`. —

#### 9.4.7 `remove an attribute(attribute)`

1. Let `element` be `attribute`'s element. —
2. Remove `attribute` from `element`'s attribute list. —
3. Set `attribute`'s **element** to null. —
4. **Handle attribute changes** for `attribute` with `element`, `attribute`'s value, and **null**. —

Step 3 clears **element** and **nothing else**: the removed `Attr`'s **node document is not reset**,
so a detached attribute keeps pointing at the document it was last appended into. Step 1 captures
`element` before step 2 precisely so step 4 can still name it.

#### 9.4.8 `replace an attribute(oldAttribute, newAttribute)`

1. Let `element` be `oldAttribute`'s element. —
2. Replace `oldAttribute` by `newAttribute` in `element`'s attribute list. — *(in place — the
   position in the list, and therefore `NamedNodeMap` index order, is preserved)*
3. Set `newAttribute`'s **element** to `element`. —
4. Set `newAttribute`'s **node document** to `element`'s node document. —
5. Set `oldAttribute`'s **element** to null. —
6. **Handle attribute changes** for **`oldAttribute`** with `element`, **`oldAttribute`'s value**,
   and **`newAttribute`'s value**. —

Step 6 is the one to read twice. A replacement produces **one** change notification, not a
remove-then-append pair, and it is reported against the **old** attribute — so the mutation record's
`attributeName`/`attributeNamespace` and the `attributeChangedCallback`'s local name and namespace
come from `oldAttribute`, while the new value comes from `newAttribute`. Since the two attributes
match on (namespace, local name) by construction (`set an attribute` step 3 found `oldAttr` by that
key), the only field that can actually differ is the **prefix**, which is not reported at all.

#### 9.4.9 `handle attribute changes(attribute, element, oldValue, newValue)`

1. **(enqueue)** **Queue a mutation record** of `"attributes"` for `element` with `attribute`'s
   local name, `attribute`'s namespace, `oldValue`, « », « », null, and null. —
2. **(enqueue)** If `element` is **custom**, then **enqueue a custom element callback reaction** with
   `element`, callback name `"attributeChangedCallback"`, and « `attribute`'s local name,
   `oldValue`, `newValue`, `attribute`'s namespace ». —
3. Run the **attribute change steps** with `element`, `attribute`'s local name, `oldValue`,
   `newValue`, and `attribute`'s namespace. —

**Zero suspension points**, as group 2 established. Note the argument asymmetry that survives into
every observer: the mutation record at step 1 receives **`oldValue` only** — the new value is not in
the record, an observer must read the attribute back — while the callback at step 2 receives
**both**.

DOM's own attribute change steps for the element **ID** concept, in full:

1. If `localName` is `id`, `namespace` is null, and `value` is null **or the empty string**, then
   **unset** `element`'s ID. —
2. Otherwise, if `localName` is `id`, `namespace` is null, then set `element`'s ID to `value`. —

So `el.id = ""` unsets the ID rather than setting it to the empty string, and a namespaced
`id` attribute never participates. The spec's framing note: *"Historically elements could have
multiple identifiers e.g., by using the HTML `id` attribute and a DTD. This specification makes ID a
concept of the DOM and allows for only one per element."*

#### 9.4.10 `set an existing attribute value(attribute, value)` — DOM §4.9.2

1. If `attribute`'s **element is null**, then set `attribute`'s value to `value` and **return**. —
2. Let `element` be `attribute`'s element. —
3. **`[S]`** Let `verifiedValue` be the result of calling **get trusted type compliant attribute
   value** with `attribute`'s local name, `attribute`'s namespace, `element`, and `value`.
   `[TRUSTED-TYPES]` —
4. If `attribute`'s **element is null**, then set `attribute`'s value to `verifiedValue` and
   **return**. —
5. **Change** `attribute` to `verifiedValue`. —

This is §9.0 item 3. Steps 1 and 4 test the same field for the same condition; the only thing
between them is the author-code call at step 3. The spec is stating outright that the policy
callback may detach the attribute and that the algorithm must notice. For a step machine: the
`element` captured at step 2 is used **only** as an argument to step 3; step 4 must re-read
`attribute`'s element from the object, and step 5's `change an attribute` re-reads it again (its own
step 3 uses `attribute`'s element). Caching `element` across the stage boundary is the bug, and it
is a bug that only reproduces when a page installs a default policy that touches the DOM.

This algorithm is reached from **three** places, all of them `[CEReactions]`:
`Attr.value`'s setter, `Node.nodeValue`'s setter when `this` is an `Attr`, and `Node.textContent`'s
setter when `this` is an `Attr` (via **set text content**).

### 9.5 The `Element` methods — DOM §4.9

IDL, verbatim, attribute-related members only:

```
[Exposed=Window]
interface Element : Node {
  readonly attribute DOMString? namespaceURI;
  readonly attribute DOMString? prefix;
  readonly attribute DOMString localName;
  readonly attribute DOMString tagName;

  [CEReactions] attribute DOMString id;
  [CEReactions] attribute DOMString className;
  [SameObject, PutForwards=value] readonly attribute DOMTokenList classList;
  [CEReactions, Unscopable] attribute DOMString slot;

  boolean hasAttributes();
  [SameObject] readonly attribute NamedNodeMap attributes;
  sequence<DOMString> getAttributeNames();
  DOMString? getAttribute(DOMString qualifiedName);
  DOMString? getAttributeNS(DOMString? namespace, DOMString localName);
  [CEReactions] undefined setAttribute(DOMString qualifiedName, (TrustedType or DOMString) value);
  [CEReactions] undefined setAttributeNS(DOMString? namespace, DOMString qualifiedName,
                                         (TrustedType or DOMString) value);
  [CEReactions] undefined removeAttribute(DOMString qualifiedName);
  [CEReactions] undefined removeAttributeNS(DOMString? namespace, DOMString localName);
  [CEReactions] boolean toggleAttribute(DOMString qualifiedName, optional boolean force);
  boolean hasAttribute(DOMString qualifiedName);
  boolean hasAttributeNS(DOMString? namespace, DOMString localName);

  Attr? getAttributeNode(DOMString qualifiedName);
  Attr? getAttributeNodeNS(DOMString? namespace, DOMString localName);
  [CEReactions] Attr? setAttributeNode(Attr attr);
  [CEReactions] Attr? setAttributeNodeNS(Attr attr);
  [CEReactions] Attr removeAttributeNode(Attr attr);
  ...
};
```

Three IDL facts that are load-bearing and easy to lose in a hand-written binding:
`setAttributeNode`/`setAttributeNodeNS` return **`Attr?`** while `removeAttributeNode` returns a
**non-nullable `Attr`** (it throws instead of returning null); `toggleAttribute`'s `force` is
`optional boolean` with **no default**, so "not given" and `false` are distinguishable and the
algorithm distinguishes them; `slot` is `[Unscopable]` and `classList` is
`[SameObject, PutForwards=value]`.

#### 9.5.1 `hasAttributes()` / `attributes` / `getAttributeNames()`

> The `hasAttributes()` method steps are to **return false if this's attribute list is empty;
> otherwise true**.

> The `attributes` getter steps are to **return the associated `NamedNodeMap`**.

> The `getAttributeNames()` method steps are to **return the qualified names of the attributes in
> this's attribute list, in order; otherwise a new list.**

That last sentence is quoted exactly as written, trailing oddity included ("otherwise a new list" =
an empty list when there are none). Its note: **"These are not guaranteed to be unique."** No
lowercasing, no de-duplication, no filtering — which is the exact opposite of what
`NamedNodeMap`'s supported property names do (§9.6), and the two are therefore observably different
views of the same list.

#### 9.5.2 `getAttribute(qualifiedName)` / `getAttributeNS(namespace, localName)`

`getAttribute`, prologue **`[S]`** `ToString(qualifiedName)`:

1. Let `attr` be the result of **getting an attribute** given `qualifiedName` and `this`. —
2. If `attr` is null, return null. —
3. Return `attr`'s value. —

`getAttributeNS`, prologue **`[S]`** `ToString(namespace)` when not null, **`[S]`**
`ToString(localName)`:

1. Let `attr` be the result of **getting an attribute** given `namespace`, `localName`, and `this`. —
2. If `attr` is null, return null. —
3. Return `attr`'s value. —

Not `[CEReactions]`. Cannot throw. Return null (not `""`) when absent — unlike `get an attribute
value` (§9.3), which is why `el.id` and `el.getAttribute("id")` differ on an element without one.

#### 9.5.3 `setAttribute(qualifiedName, value)`

Prologue: **`[S]`** `ToString(qualifiedName)`; **`[S]`** the `(TrustedType or DOMString)` union
conversion (Web IDL §3.2.25 — see Algorithm group 7). `[CEReactions]` push.

1. If `qualifiedName` is not a **valid attribute local name**, then throw an
   **`InvalidCharacterError`** `DOMException`. —
   > *Spec note: "Despite the parameter naming, `qualifiedName` is only used as a qualified name if
   > an attribute already exists with that qualified name. Otherwise, it is used as the local name
   > of the new attribute. We only need to validate it for the latter case."*
2. If **`this` is in the HTML namespace and its node document is an HTML document**, then set
   `qualifiedName` to `qualifiedName` in **ASCII lowercase**. —
3. **`[S]`** Let `verifiedValue` be the result of calling **get trusted type compliant attribute
   value** with `qualifiedName`, **null**, `this`, and `value`. `[TRUSTED-TYPES]` —
4. Let `attribute` be **the first attribute** in `this`'s attribute list whose **qualified name** is
   `qualifiedName`, and null otherwise. —
5. If `attribute` is non-null, then **change** `attribute` to `verifiedValue` and **return**. —
6. Set `attribute` to the result of **creating an attribute** given `this`'s node document,
   `qualifiedName`, **null**, **null**, and `verifiedValue`. —
7. **Append** `attribute` to `this`. —

Epilogue: **`[S]`** the `[CEReactions]` invoke — where `attributeChangedCallback` actually runs.

Step ordering that a step machine must not collapse: the lowercase at step 2 happens **before** the
TT call at step 3, so the policy receives the **lowercased** name as its `attributeName` argument
(and thus an `ONCLICK` attribute is matched as an event handler content attribute); and the lookup
at step 4 happens **after** step 3, so it must run against the attribute list **as the policy
callback left it**. A machine that hoists step 4 to run beside step 2 — an obvious optimization,
since neither depends on `verifiedValue` — is wrong.

Step 4 duplicates `get an attribute by name` step 2 inline rather than calling the primitive,
because the primitive would lowercase a second time under a different condition. Step 6 passes
**null namespace and null prefix**: see §9.11 Q3.

#### 9.5.4 The Trusted Types mapping for attributes — Trusted Types §3.7 / §3.8

**`get trusted type compliant attribute value(attributeName, attributeNs, element, newValue)`**
(TT §3.7):

1. If `attributeNs` is the empty string, set `attributeNs` to null. —
2. Set `attributeData` to the result of **get Trusted Type data for attribute** with `element`,
   `attributeName`, `attributeNs`. —
3. If `attributeData` is null, then: if `newValue` is a string, **return `newValue`**; assert
   `newValue` is `TrustedHTML`/`TrustedScript`/`TrustedScriptURL`; return its associated data. —
   *(this is the no-mapping fast path: no policy, no author code)*
4. Let `expectedType` be the fourth member of `attributeData`; let `sink` be the fifth. —
5. **`[S]`** Return the result of executing **get trusted type compliant string** with
   `expectedType`, `newValue` as input, `element`'s node document's relevant global object as
   global, `sink`, and `'script'` as sinkGroup; rethrow if it threw. — *(§0.4 has the eight steps of
   `get trusted type compliant string`; its step 4 runs the page's default policy callback)*

**`get Trusted Type data for attribute(element, attribute, attributeNs)`** (TT §3.8):

1. Let `data` be null. —
2. **If `attributeNs` is null, « HTML namespace, SVG namespace, MathML namespace » contains
   `element`'s namespace, and `attribute` is the name of an event handler content attribute:**
   return `(Element, null, attribute, TrustedScript, "Element " + attribute)`. —
3. Find the row in the following table where `element` is in the first column, `attributeNs` is in
   the second, and `attribute` is in the third. If a matching row is found, set `data` to that row. —

   | Element | Attribute namespace | Attribute local name | TrustedType | Sink |
   | --- | --- | --- | --- | --- |
   | `HTMLIFrameElement` | null | `"srcdoc"` | `TrustedHTML` | `"HTMLIFrameElement srcdoc"` |
   | `HTMLScriptElement` | null | `"src"` | `TrustedScriptURL` | `"HTMLScriptElement src"` |
   | `SVGScriptElement` | null | `"href"` | `TrustedScriptURL` | `"SVGScriptElement href"` |
   | `SVGScriptElement` | XLink namespace | `"href"` | `TrustedScriptURL` | `"SVGScriptElement href"` |

4. Return `data`. —

Step 2 is §9.0 item 4 — and it is checked **before** the table, so it applies to every element in
those three namespaces, not only to the four rows. The spec flags its own weakness here: *"The event
handler content attribute concept used below is ambiguous. This spec needs a better mechanism to
identify event handler attributes."* For the engine that means the set of event handler content
attribute names is HTML's, and it is the same set that governs `onclick`-style parsing.

The practical consequence for the solver half: **`setAttribute` on any `on*` attribute of an HTML
element is a Trusted Types sink**, so a page with `require-trusted-types-for 'script'` blocks it,
and a page with a default policy runs author code inside it.

#### 9.5.5 `setAttributeNS(namespace, qualifiedName, value)`

Prologue: **`[S]`** `ToString(namespace)` when not null; **`[S]`** `ToString(qualifiedName)`;
**`[S]`** the `(TrustedType or DOMString)` union. `[CEReactions]` push.

1. Let `(namespace, prefix, localName)` be the result of **validating and extracting** `namespace`
   and `qualifiedName` given **`"attribute"`**. — *(throws `InvalidCharacterError` /
   `NamespaceError` per §9.2 steps 4.3, 6, 8, 9, 10, 11)*
2. **`[S]`** Let `verifiedValue` be the result of calling **get trusted type compliant attribute
   value** with `localName`, `namespace`, `this`, and `value`. `[TRUSTED-TYPES]` —
3. **Set an attribute value** for `this` using `localName`, `verifiedValue`, `prefix`, and
   `namespace`. —

Epilogue: **`[S]`** `[CEReactions]` invoke.

**No ASCII-lowercasing anywhere on this path.** `setAttributeNS(null, "FOO", v)` on an HTML element
in an HTML document creates an attribute whose local name is `"FOO"`, which `getAttribute("foo")`
then cannot find and `getAttribute("FOO")` also cannot find (step 1 of the by-name lookup lowercases
the *query*). It is reachable only via `getAttributeNS(null, "FOO")` — and via
`getAttributeNames()`, which reports it unlowercased, while `el.attributes` named access hides it
(§9.6).

Also note the position difference from `setAttribute`, already flagged in group 2: TT runs at
**step 2 of 3** here and **step 3 of 7** there, and here it is passed `localName` + the real
`namespace`, where `setAttribute` passes the whole (lowercased) `qualifiedName` + null.

#### 9.5.6 `removeAttribute` / `removeAttributeNS`

> The `removeAttribute(qualifiedName)` method steps are to **remove an attribute** given
> `qualifiedName` and `this`, and then **return undefined**.

> The `removeAttributeNS(namespace, localName)` method steps are to **remove an attribute** given
> `namespace`, `localName`, and `this`, and then **return undefined**.

One step each, discarding the primitive's return value. `[CEReactions]`. **Never throws** —
removing an attribute that does not exist is silent.

#### 9.5.7 `hasAttribute(qualifiedName)` / `hasAttributeNS(namespace, localName)`

`hasAttribute`:

1. If **`this` is in the HTML namespace and its node document is an HTML document**, then set
   `qualifiedName` to `qualifiedName` in **ASCII lowercase**. —
2. Return true if `this` **has an attribute** whose **qualified name** is `qualifiedName`; otherwise
   false. —

`hasAttributeNS`:

1. If `namespace` is the empty string, then set it to null. —
2. Return true if `this` **has an attribute** whose **namespace** is `namespace` and **local name**
   is `localName`; otherwise false. —

Both open-code the lookup rather than delegating (there is no "first" tiebreak needed for a boolean).
Neither is `[CEReactions]`; neither throws.

#### 9.5.8 `toggleAttribute(qualifiedName, force)`

Prologue: **`[S]`** `ToString(qualifiedName)`; `force` is `optional boolean` — Web IDL's boolean
conversion is `ToBoolean`, which **never** calls user code. `[CEReactions]` push.

1. If `qualifiedName` is not a **valid attribute local name**, then throw an
   **`InvalidCharacterError`** `DOMException`. —
   > *Spec note: "See the discussion above about why we validate it as a local name, instead of a
   > qualified name."*
2. If **`this` is in the HTML namespace and its node document is an HTML document**, then set
   `qualifiedName` to `qualifiedName` in **ASCII lowercase**. —
3. Let `attribute` be **the first attribute** in `this`'s attribute list whose **qualified name** is
   `qualifiedName`, and null otherwise. —
4. If `attribute` is null:
   1. If `force` is **not given or is true**, then **append** the result of **creating an attribute**
      given `this`'s node document and `qualifiedName` to `this`, and then **return true**. —
   2. Return false. —
5. If `force` is **not given or is false**, **remove an attribute** given `qualifiedName` and
   `this`, and then **return false**. —
6. Return true. —

**`toggleAttribute` never calls Trusted Types.** It is the one attribute-creating `Element` method
with no TT step, because the value it creates is the default empty string (step 4.1 passes only two
arguments to `create an attribute`). So `el.toggleAttribute("onclick")` produces an `onclick=""`
attribute under a policy that would have intercepted `el.setAttribute("onclick", "")`.

Step 5's re-lookup by name is redundant with step 3's result but is what the spec says; it re-enters
`get an attribute by name`, which lowercases **again** — harmless because step 2 already did, and
ASCII lowercase is idempotent.

#### 9.5.9 The node-valued methods

> The `getAttributeNode(qualifiedName)` method steps are to return the result of **getting an
> attribute** given `qualifiedName` and `this`.

> The `getAttributeNodeNS(namespace, localName)` method steps are to return the result of **getting
> an attribute** given `namespace`, `localName`, and `this`.

> The `setAttributeNode(attr)` and `setAttributeNodeNS(attr)` methods steps are to return the result
> of **setting an attribute** given `attr` and `this`.

`removeAttributeNode(attr)`:

1. If `this`'s attribute list does **not contain** `attr`, then throw a **`NotFoundError`**
   `DOMException`. —
2. **Remove** `attr`. —
3. Return `attr`. —

`setAttributeNode` and `setAttributeNodeNS` are **the same algorithm, verbatim, in one sentence** —
the `NS` suffix carries no behavioural difference at all. (They differ from each other only in name;
both key on (namespace, local name) via `set an attribute` step 3.) Step 1 of
`removeAttributeNode` is an **identity** containment test on the list, not a name match.

#### 9.5.10 `id`, `className`, `slot` — reflection, DOM's own definition

DOM defines reflection for these three itself (it does not use HTML §2.6.1's machinery):

> IDL attributes that are defined to **reflect** a string `name`, must have these getter and setter
> steps:
> - **getter steps**: Return the result of running **get an attribute value** given `this` and
>   `name`.
> - **setter steps**: **Set an attribute value** for `this` using `name` and the given value.

> The `id` attribute must reflect `"id"`. The `className` attribute must reflect `"class"`. The
> `classList` getter steps are to return a `DOMTokenList` object whose associated element is `this`
> and whose associated attribute's local name is `class`. The `slot` attribute must reflect
> `"slot"`.

> *"`id`, `class`, and `slot` are effectively superglobal attributes as they can appear on any
> element, regardless of that element's namespace."*

Because the setter is `set an attribute value` with only `(element, name, value)` — prefix and
namespace defaulting to null — **reflection is null-namespace-keyed and never lowercases anything**
(the name is a spec literal, already lowercase) and, as §9.0 item 5 says, **never runs Trusted
Types**. The getter returning `""` for an absent attribute comes from `get an attribute value`
step 2.

### 9.6 DOM §4.9.1 `NamedNodeMap`, in full

```
[Exposed=Window,
 LegacyUnenumerableNamedProperties]
interface NamedNodeMap {
  readonly attribute unsigned long length;
  getter Attr? item(unsigned long index);
  getter Attr? getNamedItem(DOMString qualifiedName);
  Attr? getNamedItemNS(DOMString? namespace, DOMString localName);
  [CEReactions] Attr? setNamedItem(Attr attr);
  [CEReactions] Attr? setNamedItemNS(Attr attr);
  [CEReactions] Attr removeNamedItem(DOMString qualifiedName);
  [CEReactions] Attr removeNamedItemNS(DOMString? namespace, DOMString localName);
};
```

> A `NamedNodeMap` has an associated **element** (an element). A `NamedNodeMap` object's **attribute
> list** is its element's attribute list.

It is a **live view**, not a copy — and `Element.attributes` is `[SameObject]`, so the identity is
stable for the element's lifetime.

**Supported property indices**: *"the numbers in the range zero to its attribute list's size minus
1, unless the attribute list is empty, in which case there are no supported property indices."*

**`length`** getter: return the attribute list's **size**.

**`item(index)`**:

1. If `index` is equal to or greater than `this`'s attribute list's size, then return null. —
2. Otherwise, return `this`'s attribute list[`index`]. —

**Supported property names** — the algorithm that makes named access disagree with
`getAttributeNames()`:

1. Let `names` be **the qualified names** of the attributes in this `NamedNodeMap` object's
   attribute list, **with duplicates omitted**, in order. —
2. If this `NamedNodeMap` object's **element is in the HTML namespace and its node document is an
   HTML document**, then for each `name` of `names`:
   1. Let `lowercaseName` be `name`, in **ASCII lowercase**. —
   2. If `lowercaseName` is **not equal to** `name`, **remove `name` from `names`**. —
3. Return `names`. —

Read step 2.2 carefully: it does **not** lowercase the exposed name, it **deletes** any name that is
not already lowercase. So on an HTML element in an HTML document, an attribute created as
`setAttributeNS(null, "FOO", v)` is **absent from `el.attributes`' named properties entirely** while
being present in `el.getAttributeNames()` and reachable by index. Combined with
`LegacyUnenumerableNamedProperties`, the named properties do not show up in `Object.keys` /
`for...in` either.

**`getNamedItem(qualifiedName)`**: return the result of **getting an attribute** given
`qualifiedName` and element. — *(qualified-name key space)*

**`getNamedItemNS(namespace, localName)`**: return the result of **getting an attribute** given
`namespace`, `localName`, and element. — *((namespace, local name) key space)*

**`setNamedItem(attr)` and `setNamedItemNS(attr)`**: *"to return the result of **setting an
attribute** given `attr` and element."* — identical, one sentence, both of them; §9.4.1 applies,
including the `InUseAttributeError` and the Trusted Types call at its step 1.

**`removeNamedItem(qualifiedName)`**:

1. Let `attr` be the result of **removing an attribute** given `qualifiedName` and element. —
2. If `attr` is null, then throw a **`NotFoundError`** `DOMException`. —
3. Return `attr`. —

**`removeNamedItemNS(namespace, localName)`**:

1. Let `attr` be the result of **removing an attribute** given `namespace`, `localName`, and
   element. —
2. If `attr` is null, then throw a **`NotFoundError`** `DOMException`. —
3. Return `attr`. —

This is the one behavioural difference between the `NamedNodeMap` surface and the `Element` surface
over the same primitives: **`removeNamedItem` throws on a miss, `removeAttribute` does not.**

### 9.7 DOM §4.9.2 `Attr`, in full

```
[Exposed=Window]
interface Attr : Node {
  readonly attribute DOMString? namespaceURI;
  readonly attribute DOMString? prefix;
  readonly attribute DOMString localName;
  readonly attribute DOMString name;
  [CEReactions] attribute DOMString value;

  readonly attribute Element? ownerElement;

  readonly attribute boolean specified; // historical; always returns true
};
```

Member by member, quoting the getter steps:

| Member | Getter steps | Notes |
| --- | --- | --- |
| `namespaceURI` | *"return this's **namespace**"* | nullable; never the empty string |
| `prefix` | *"return this's **namespace prefix**"* | nullable; never the empty string |
| `localName` | *"return this's **local name**"* | non-nullable, never empty |
| `name` | *"return this's **qualified name**"* | i.e. `prefix + ":" + localName`, or `localName` |
| `value` | *"return this's **value**"* | setter = **set an existing attribute value** (§9.4.10) |
| `ownerElement` | *"return this's **element**"* | nullable |
| `specified` | *"return **true**"* | the IDL's own comment: `// historical; always returns true` |

There is **no `Attr` constructor** and there is no way to set namespace, prefix or local name after
creation — the only mutable field is `value`. `Attr` has no `Node` children (its "value" is not a
`Text` child in this DOM), which is why `Node.textContent`'s **set text content** switches on `Attr`
to `set an existing attribute value` rather than replacing children.

Inherited `Node` members whose behaviour is `Attr`-specific:

- **`nodeName`** getter, switching on interface: for `Attr`, *"Its **qualified name**"* (for
  `Element` it is the HTML-uppercased qualified name — `Attr` is **not** uppercased).
- **`nodeValue`** getter: for `Attr`, *"this's **value**"*. Setter: *"if the given value is null, act
  as if it was the empty string instead"*, then for `Attr`, **set an existing attribute value** with
  `this` and the given value.
- **`textContent`**: getter runs **get text content**; setter runs **set text content**, which for
  `Attr` is **set an existing attribute value** with `node` and `value`.

So all three of `attr.value = s`, `attr.nodeValue = s`, `attr.textContent = s` are `[CEReactions]`
and all three can run a Trusted Types default policy. The IDL type of `Attr.value` is plain
**`DOMString`**, *not* the `(TrustedType or DOMString)` union that `setAttribute` uses — so passing a
`TrustedScript` to `attr.value` stringifies it in the Web IDL prologue, and the TT algorithm at
`set an existing attribute value` step 3 then sees a **plain string** and takes the default-policy
path rather than the "already an instance of expectedType" fast path. That asymmetry is a real
observable difference between `el.setAttribute("onclick", trustedScript)` and
`el.getAttributeNode("onclick").value = trustedScript`.

`Node.nodeValue` and `Node.textContent` are both `[CEReactions] attribute DOMString?` — **nullable**,
so a null assignment skips `ToString` entirely and is turned into `""` by the setter's own preamble.

### 9.8 `Document` — creation, cloning, adoption

**`createAttribute(localName)`** — DOM §4.5:

1. If `localName` is not a **valid attribute local name**, then throw an **`InvalidCharacterError`**
   `DOMException`. —
2. If **`this` is an HTML document**, then set `localName` to `localName` in **ASCII lowercase**. —
3. Return the result of **creating an attribute** given `this` and `localName`. —

Step 2's condition is **only** "this is an HTML document" — there is no HTML-namespace conjunct,
because there is no element yet. This is the single place in the attribute surface where the
lowercasing condition has one term instead of two. `htmlDoc.createAttribute("FOO").name === "foo"`;
`xmlDoc.createAttribute("FOO").name === "FOO"`.

Step 3 passes only two arguments, so the new attribute has **namespace null, prefix null, value
`""`** — and, critically, **element null**: `createAttribute` does not attach anything.

**`createAttributeNS(namespace, qualifiedName)`** — DOM §4.5:

1. Let `(namespace, prefix, localName)` be the result of **validating and extracting** `namespace`
   and `qualifiedName` given **`"attribute"`**. —
2. Return the result of **creating an attribute** given `this`, `localName`, `namespace`, and
   `prefix`. —

**No lowercasing.** Value defaults to `""`.

**`clone a single node(node, document, fallbackRegistry)`** — DOM §4.4, the attribute-relevant arms:

- If `node` is an element, after `copy` is created by **create an element**:
  - For each `attribute` of `node`'s attribute list:
    1. Let `copyAttribute` be the result of **cloning a single node** given `attribute`, `document`,
       and **null**. —
    2. **Append** `copyAttribute` to `copy`. —
- The per-interface "additional requirements" arm for **`Attr`**: *"Set copy's **namespace**,
  **namespace prefix**, **local name**, and **value** to those of node."*

Step 2 is **`append an attribute`** (§9.4.5), not a list push — so cloning an element **queues a
mutation record and enqueues `attributeChangedCallback` per attribute on the clone**. Note the
`Attr` arm copies four fields and **not** `element` — the clone starts detached and is attached by
the caller.

**`adopt(node, document)`** — DOM §4.5, the attribute-relevant steps:

1. Let `oldDocument` be `node`'s node document. —
2. If `node`'s **parent** is non-null, then **remove** `node`. —
3. If `document` is not `oldDocument`, then for each `inclusiveDescendant` of `node`'s
   shadow-including inclusive descendants, in shadow-including tree order:
   1. Set `inclusiveDescendant`'s node document to `document`. —
   2. ... (shadow root / custom element registry arms) ...
   3. Otherwise, if `inclusiveDescendant` is an **element**:
      1. **Set the node document of each attribute in `inclusiveDescendant`'s attribute list to
         `document`.** —
      2. ... (custom element registry arm) ...

Two consequences worth a `DCHECK`: an element's attributes are **not** descendants, so they are
re-documented by the explicit step 3.3.1 and by nothing else; and step 2's "if node's parent is
non-null, remove node" **never fires for an `Attr`**, because an attribute's owner is its
**element**, not its **parent** (an `Attr`'s parent is always null). **`doc.adoptNode(attrOnAnElement)`
therefore changes the attribute's node document while leaving it attached to an element in another
document.**

**`importNode(node, options)`** — DOM §4.5: steps 1 to 6 handle `subtree`/`registry` and then
*"Return the result of **cloning a node** given `node`"* with the target document — so importNode's
effect on attributes is entirely `clone a single node`'s, above. `adoptNode(node)` is: throw
`NotSupportedError` for a document, `HierarchyRequestError` for a shadow root, **adopt** `node` into
`this`, return `node`.

### 9.9 HTML — "adjust foreign attributes"

From HTML's tree-construction section (the "Creating and inserting nodes" area of §13.2.6):

> When the steps below require the user agent to **adjust foreign attributes** for a token, then, if
> any of the attributes on the token match the strings given in the first column of the following
> table, let the attribute be a namespaced attribute, with the **prefix** being the string given in
> the corresponding cell in the second column, the **local name** being the string given in the
> corresponding cell in the third column, and the **namespace** being the namespace given in the
> corresponding cell in the fourth column. (This fixes the use of namespaced attributes, in
> particular `lang` attributes in the XML namespace.)

| Attribute name | Prefix | Local name | Namespace |
| --- | --- | --- | --- |
| `xlink:actuate` | `xlink` | `actuate` | XLink namespace |
| `xlink:arcrole` | `xlink` | `arcrole` | XLink namespace |
| `xlink:href` | `xlink` | `href` | XLink namespace |
| `xlink:role` | `xlink` | `role` | XLink namespace |
| `xlink:show` | `xlink` | `show` | XLink namespace |
| `xlink:title` | `xlink` | `title` | XLink namespace |
| `xlink:type` | `xlink` | `type` | XLink namespace |
| `xml:lang` | `xml` | `lang` | XML namespace |
| `xml:space` | `xml` | `space` | XML namespace |
| `xmlns` | *(none)* | `xmlns` | XMLNS namespace |
| `xmlns:xlink` | `xmlns` | `xlink` | XMLNS namespace |

**Exactly eleven rows.** Things to note against common belief: only **seven** `xlink:` attributes
are listed (`actuate`, `arcrole`, `href`, `role`, `show`, `title`, `type` — there is no
`xlink:label`, no `xlink:from`/`to`); only **two** `xml:` attributes (`lang`, `space` — **no
`xml:base`**); and `xmlns` is the **only** row with **no prefix**, giving an attribute whose
qualified name is `xmlns` and whose namespace is XMLNS. A table implemented as "split on `:` and map
the prefix" gets the `xmlns` row wrong.

Called from exactly **three** places in the tree construction: the "in body" insertion mode's
`<math>` start tag, the "in body" insertion mode's `<svg>` start tag, and the "in foreign content"
insertion mode's any-other-start-tag rule. In all three it runs **after** `adjust MathML attributes`
/ `adjust SVG attributes` and **before** "insert a foreign element".

Its two neighbours, for completeness, since they run on the same token and are frequently confused
with it:

- **`adjust MathML attributes`**: *"if the token has an attribute named `definitionurl`, change its
  name to `definitionURL`"* — a single **rename**, no namespace, one row.
- **`adjust SVG attributes`**: a rename table (`attributename` to `attributeName`,
  `attributetype` to `attributeType`, `basefrequency` to `baseFrequency`, ... `zoomandpan` to
  `zoomAndPan`) — again **renames only**, no namespace, no prefix.

Only "adjust foreign attributes" produces a namespaced attribute; the other two only fix case.

The one place this leaks into scripting: an attribute the parser produced this way has a **non-null
prefix**, so its qualified name has a colon in it, so `el.getAttribute("xlink:href")` finds it (by
qualified name) while `el.getAttributeNS(null, "xlink:href")` does not, and
`el.getAttributeNS(XLINK, "href")` does. And `el.setAttribute("xlink:href", v)` on a *fresh* element
creates a **different, null-namespace** attribute that merely prints the same (§9.11 Q3).

### 9.10 Headless note

Nothing in the attribute surface depends on a device, a layout, or a network. Every value here is
fully determined by the algorithms above. The only external inputs are the page's Trusted Types
default policy (author code) and the document's "is an HTML document" flag.

### 9.11 The three questions, answered with the deciding step

#### Q1. Which lookups are keyed on the QUALIFIED name and which on (namespace, local name)?

**Qualified-name-keyed** — every one of these ends at `get an attribute by name` **step 2** ("the
first attribute ... whose **qualified name** is `qualifiedName`") or open-codes it:

| Entry point | Deciding step |
| --- | --- |
| `Element.getAttribute(q)` | its step 1, into `get an attribute by name` step 2 |
| `Element.getAttributeNode(q)` | its only step, same |
| `Element.hasAttribute(q)` | its **step 2** (open-coded: "whose qualified name is") |
| `Element.removeAttribute(q)` | `remove an attribute by name` **step 1**, same |
| `Element.setAttribute(q, v)` | its **step 4** (open-coded: "the first attribute ... whose qualified name is") |
| `Element.toggleAttribute(q, f)` | its **step 3** (open-coded), and again its step 5 |
| `NamedNodeMap.getNamedItem(q)` | its only step, same |
| `NamedNodeMap.removeNamedItem(q)` | its **step 1**, into `remove an attribute by name` step 1 |
| `NamedNodeMap` supported property names | its **step 1** ("the **qualified names** of the attributes") |
| `Element.getAttributeNames()` | its only step ("the **qualified names**") |
| `Attr.name`, `Node.nodeName` for `Attr` | "its qualified name" |

**(namespace, local name)-keyed** — every one of these ends at `get an attribute by namespace and
local name` **step 2** ("whose **namespace** is `namespace` and **local name** is `localName`") or
open-codes it:

| Entry point | Deciding step |
| --- | --- |
| `Element.getAttributeNS(ns, ln)` | its step 1, into `get an attribute by namespace and local name` step 2 |
| `Element.getAttributeNodeNS(ns, ln)` | its only step, same |
| `Element.hasAttributeNS(ns, ln)` | its **step 2** (open-coded) |
| `Element.removeAttributeNS(ns, ln)` | `remove an attribute by namespace and local name` **step 1**, same |
| `Element.setAttributeNS(ns, q, v)` | its step 3, into `set an attribute value` **step 1**, same |
| `Element.setAttributeNode(attr)` / `setAttributeNodeNS(attr)` | `set an attribute` **step 3** |
| `NamedNodeMap.getNamedItemNS(ns, ln)` | its only step, same |
| `NamedNodeMap.setNamedItem(attr)` / `setNamedItemNS(attr)` | `set an attribute` **step 3** |
| `NamedNodeMap.removeNamedItemNS(ns, ln)` | its **step 1**, same |
| `Element.id` / `className` / `slot` **getters** | reflect getter steps, into `get an attribute value` **step 1** (namespace null) |
| `Element.id` / `className` / `slot` **setters** | reflect setter steps, into `set an attribute value` **step 1** (namespace null) |
| DOM's ID attribute change steps | its steps 1 and 2 (`localName is id` **and** `namespace is null`) |

**Neither** (identity-keyed): `Element.removeAttributeNode(attr)` **step 1** is a *containment* test
on the list, and `set an attribute` **step 4** (`oldAttr is attr`) is an identity comparison.
**Index-keyed**: `NamedNodeMap.item(index)` **step 2**, and the supported property indices.

The asymmetry to internalise: **`setAttribute`/`toggleAttribute`/`hasAttribute` are name-keyed while
`setAttributeNS`/`setAttributeNode` are namespace-keyed**, so the two families can each find an
attribute the other cannot, and the name-keyed family is the only one that can encounter a
**duplicate** (hence "the first"). The uniqueness invariant on the attribute list is over
**(namespace, local name)**, never over the qualified name.

#### Q2. Where exactly does `setAttribute` ASCII-lowercase its argument, and under what condition?

**`setAttribute` step 2**, and the condition is a **conjunction of two terms**:

> If **this is in the HTML namespace** **and** its **node document is an HTML document**, then set
> `qualifiedName` to `qualifiedName` in **ASCII lowercase**.

Not "if the document is HTML" alone, and not "if the element is an HTML element" alone. An `<svg>`
element's `<circle>` child in an HTML document is in the SVG namespace, so
`circle.setAttribute("viewBox", v)` **preserves the capital B**; a `<div>` in an XML document is in
the HTML namespace but the document is not an HTML document, so `div.setAttribute("FOO", v)`
preserves the capitals too.

Position matters as much as condition: step 2 is **after** the validity check at step 1 (which
therefore tests the caller's original casing — irrelevant, since the character sets are
case-blind) and **before** the Trusted Types call at step 3 (which therefore receives the lowercased
name) and before the lookup at step 4 (which therefore matches lowercased).

The same two-term condition, at the corresponding step, governs:
`get an attribute by name` **step 1**, `hasAttribute` **step 1**, `toggleAttribute` **step 2**, and
`NamedNodeMap`'s supported property names **step 2** (on the map's element). It is
**`Document.createAttribute` step 2** that has the **one-term** version ("If this is an HTML
document"), and **`setAttributeNS`, `createAttributeNS`, `setAttributeNode`, `getAttributeNS`,
`hasAttributeNS` and reflection lowercase nothing at all**.

"ASCII lowercase" is Infra's: U+0041 to U+005A become U+0061 to U+007A, nothing else. A `tolower()`
under a Turkish locale, or a Unicode `toLowerCase`, is a fidelity bug.

#### Q3. What namespace does an attribute created by `setAttribute` have?

**Null.** The deciding step is **`setAttribute` step 6**:

> Set `attribute` to the result of **creating an attribute** given `this`'s node document,
> `qualifiedName`, **null**, **null**, and `verifiedValue`.

Matched against `create an attribute(document, localName, namespace, prefix, value)`, the third and
fourth arguments are the **namespace** and the **prefix**, both explicitly null. So the new
attribute has:

- **local name** = the (possibly lowercased) `qualifiedName`, **colon and all**;
- **namespace** = null;
- **namespace prefix** = null;
- and therefore **qualified name == local name**.

`toggleAttribute` **step 4.1** is the same (it passes only `document` and `qualifiedName`, so
namespace and prefix take their null defaults and value takes `""`).

The consequence that bites: `el.setAttribute("xlink:href", v)` creates an attribute whose **local
name is the ten-character string `xlink:href`** in **no namespace**. It is not the XLink `href`
attribute the parser would have produced for the same source text (§9.9), `getAttributeNS(XLINK,
"href")` will not find it, and if the element already carries a parser-produced XLink `href` then
**both** attributes now exist in the list with the **same qualified name** — which is precisely the
duplicate that `get an attribute by name` step 2's "the **first**" exists to resolve, and precisely
why the uniqueness key is (namespace, local name). None of this throws, because `:` is not in the
excluded set of a **valid attribute local name** (§9.2).

The mirror-image fact: `setAttributeNS(namespace, qualifiedName, value)` reaches
`set an attribute value` with a **non-null prefix** only through `validate and extract` step 4,
and step 8 guarantees a non-null prefix implies a non-null namespace — so **an attribute with a
prefix but no namespace is unconstructible** through any API. That is a `DCHECK`, not a comment.

### 9.12 Suspension points per entry point

`[S]` = the page's own code can run. `TT` = the Trusted Types default-policy call. `CE` = the
`[CEReactions]` invoke epilogue (§0.3). Argument conversions are the Web IDL prologue (§0.2);
`unsigned long` and `boolean` conversions are `ToNumber`/`ToBoolean`, of which only `ToNumber` can
call user code.

| Entry point | `[S]` | Which |
| --- | --- | --- |
| `Element.getAttributeNames()` | 0 | — |
| `Element.hasAttributes()` | 0 | — |
| `Element.getAttribute(q)` | 1 | ToString(q) |
| `Element.getAttributeNS(ns, ln)` | 2 | ToString times 2 |
| `Element.setAttribute(q, v)` | 4 | ToString(q); union(v); **TT step 3**; CE |
| `Element.setAttributeNS(ns, q, v)` | 5 | ToString times 2; union(v); **TT step 2**; CE |
| `Element.removeAttribute(q)` | 2 | ToString(q); CE |
| `Element.removeAttributeNS(ns, ln)` | 3 | ToString times 2; CE |
| `Element.toggleAttribute(q, force)` | 2 | ToString(q); CE — **no TT** |
| `Element.hasAttribute(q)` | 1 | ToString(q) |
| `Element.hasAttributeNS(ns, ln)` | 2 | ToString times 2 |
| `Element.getAttributeNode(q)` | 1 | ToString(q) |
| `Element.getAttributeNodeNS(ns, ln)` | 2 | ToString times 2 |
| `Element.setAttributeNode(attr)` / `setAttributeNodeNS(attr)` | 2 | **TT** (`set an attribute` step 1, **before** the `InUseAttributeError` check); CE — counted once as one shape |
| `Element.removeAttributeNode(attr)` | 1 | CE |
| `Element.id` / `className` / `slot` **setters** | 2 | ToString; CE — **no TT** — counted once as one shape |
| `NamedNodeMap.item(i)` / indexed getter | 1 | ToNumber(i) |
| `NamedNodeMap.getNamedItem(q)` | 1 | ToString(q) |
| `NamedNodeMap.getNamedItemNS(ns, ln)` | 2 | ToString times 2 |
| `NamedNodeMap.setNamedItem(attr)` / `setNamedItemNS(attr)` | 2 | **TT**; CE — counted once as one shape |
| `NamedNodeMap.removeNamedItem(q)` | 2 | ToString(q); CE |
| `NamedNodeMap.removeNamedItemNS(ns, ln)` | 3 | ToString times 2; CE |
| `Attr.value` setter | 3 | ToString; **TT** (`set an existing attribute value` step 3); CE |
| `Attr.nodeValue` setter | 3 | ToString (nullable — null skips it); **TT**; CE |
| `Attr.textContent` setter | 3 | ToString (nullable); **TT**; CE |
| `Attr` getters (`namespaceURI`/`prefix`/`localName`/`name`/`value`/`ownerElement`/`specified`) | 0 | — |
| `Document.createAttribute(ln)` | 1 | ToString(ln) |
| `Document.createAttributeNS(ns, q)` | 2 | ToString times 2 |

**Suspension points in Algorithm group 9: 53.** Inside `handle attribute changes`, `append an
attribute`, `change an attribute`, `remove an attribute`, `replace an attribute`, `get an attribute
by name`, `get an attribute by namespace and local name`, `get an attribute value`, `set an
attribute value`, `create an attribute`, and `validate and extract`: **zero** — every one of those
primitives is straight-line C with no author-code reachable step. The entire author-code surface of
the DOM attribute model is (a) the Web IDL argument conversions, (b) **exactly two** Trusted Types
call sites reached from four algorithms (`setAttribute` step 3, `setAttributeNS` step 2, `set an
attribute` step 1, `set an existing attribute value` step 3), and (c) the `[CEReactions]` epilogue.

### 9.13 Stage-boundary consequences for a step machine

1. **`setAttribute` needs a boundary between step 3 and step 4**, because the policy callback at
   step 3 can add, remove or reorder attributes and step 4's "first attribute whose qualified name
   is" must see the post-callback list.
2. **`set an attribute` needs a boundary between step 1 and step 2**, and step 2 must re-read
   `attr`'s element rather than a value captured before step 1.
3. **`set an existing attribute value` needs a boundary between step 3 and step 4**, and step 4 is
   the spec explicitly telling you to re-read the field — it is the same test as step 1 for the
   same reason. Steps 1 and 4 are not a redundancy to fold.
4. **`setAttributeNS` needs a boundary between step 2 and step 3**, because `set an attribute
   value`'s own step 1 lookup must run after the callback.
5. Everything else in group 9 is one straight-line stage between its prologue and its
   `[CEReactions]` epilogue — including all of `handle attribute changes`, which is the algorithm
   an implementer is most likely to wrongly split.

---

## 10. Observable — `Subscriber`, `Observable`, `subscribe`, `from()`, and the operators

**Why this section exists.** `dom/observable` was the largest completely-unimplemented cluster in the
DOM gate, and the reason it is worth writing down separately from the nine groups above is that it
inverts their ratio. In groups 1–9 the author-code surface is a handful of Trusted Types calls and
`[CEReactions]` epilogues buried in otherwise straight-line tree work; **in this standard there is
almost nothing that is not author code.** A producer is a callback, every consumer is a callback,
every teardown is a callback, every abort algorithm is a callback, and the two dictionaries
(`SubscriptionObserver`, `SubscribeOptions`) are read with `[[Get]]`. So the interesting question is
not "where are the suspension points" but "is there a step anywhere that is *not* one".

**Network was available.** Everything below was read from the live standard on **2026-08-10**:

| Standard | Source | Version read |
| --- | --- | --- |
| Observable | `https://wicg.github.io/observable/` | Editor's Draft, read 2026-08-10 |
| WHATWG DOM (`AbortSignal`, "signal abort") | `https://dom.spec.whatwg.org/` | Living Standard, §3.2 |
| Web IDL (callback invocation, dictionaries) | `https://webidl.spec.whatwg.org/` | Living Standard, §3.2.18, §3.12 |

The standard is **not** in WHATWG DOM yet. `https://dom.spec.whatwg.org/` contains no `Observable`,
no `Subscriber` and no `subscribe` — a `curl` of it and a `grep` for those three words answers zero,
zero and zero — so a step number cited as "DOM §…" for this component would name nothing. The WPT
directory is `dom/observable/tentative` for that reason, and the citations below are the WICG
draft's own section numbers.

### 10.0 The one fact that decides every stage boundary

**Web IDL's "invoke with `report`" is a suspension point that *cannot* raise.** Every callback in
this standard is invoked either with `"report"` (the observer's `next`/`error`/`complete`, every
teardown, the abort algorithm, `inspect`'s five handlers on their reporting paths) or with
`"rethrow"` (the subscribe callback, and the operators' `mapper`/`predicate`/`reducer`, whose throw
the caller immediately turns into `subscriber.error(E)`). Both are `[S]`. The difference is what the
machine does when the request comes back abrupt, and it is why this component's definitions declare
`catches_abrupt`: with `"report"` the exception is a value to be reported and the walk continues —
which is what lets the standard assert *"Assert: No exception was thrown"* after each observer's
steps — and with `"rethrow"` it is a value that becomes the next algorithm's argument. Neither ever
unwinds the machine. A definition without `catches_abrupt` turns both into a raise, and a raise out
of `subscriber.next()` is an exception escaping into whatever the producer was doing.

### 10.1 §2.1 `Subscriber` — `next`, `error`, `complete`, `addTeardown`

`next(value)`:

1. If `active` is false, return. —
2. If the relevant global is a `Window` whose Document is not fully active, return. —
3. Let *copy* be **a copy of** the internal observers. — *(not a live walk — see 10.5)*
4. For each observer of *copy*: **`[S]`** run its next steps given value.

`error(error)`:

1. If `active` is false, **`[S]`** report an exception with error, and return. — *(HTML §8.1.4.6
   fires `error` at the global, so the page's `onerror` runs)*
2. Fully-active check. —
3. **`[S]`** Close this. — *(the whole of 10.3, which runs abort algorithms, listeners and teardowns)*
4. Let *copy* be a copy of the internal observers. —
5. For each observer of *copy*: **`[S]`** run its error steps given error.

`complete()` is `error` without step 1's report and with complete steps at step 5.

`addTeardown(teardown)`:

1. Fully-active check. —
2. If `active` is true, append teardown to the teardown callbacks. —
3. Otherwise, **`[S]`** invoke teardown with «» and `"report"`. — *this is the branch an
   implementation forgets, and WPT pins it: a teardown added to an already-closed subscription runs
   **synchronously, during `addTeardown`**, and two of them added that way run in **FIFO** order —
   the reverse-order rule belongs to step 4 of `close`, not to this step.*

**Suspension points: 6.** The prologue adds none — `next(any value)` and `error(any error)` convert
nothing, and `addTeardown`'s `VoidFunction` is a brand check.

### 10.2 §2.2 The `Observable` constructor and `subscribe`

`new Observable(callback)` is one step ("set this's subscribe callback to callback"), and its only
author-code reach is Web IDL §3.7.1's `Get(newTarget, "prototype")` — **`[S]`**, before the
algorithm.

`subscribe(observer, options)` is `subscribe to an Observable` (§2.2.1), whose steps are:

1. Fully-active check. —
2. Let *internal observer* be a new internal observer. —
3. Process *observer*: a callable IS the next steps; an object is a `SubscriptionObserver`, whose
   three members are read **`[S]` × 3** in Web IDL §3.2.18's **lexicographic** order —
   `complete`, `error`, `next` — *not* the order the standard's prose lists them in.
4. Assert on the error steps. —
5. **If the weak subscriber is non-null and still active**, the subscription **JOINS** it:
   1. Let *subscriber* be that one. —
   2. Append *internal observer* to its internal observers. —
   3. If `options`'s signal exists: aborted → remove it again; otherwise add the abort algorithm
      of step 9.2. —
   4. **Return** — *the subscribe callback is NOT invoked a second time.*
6. Let *subscriber* be a new `Subscriber`. —
7. Append *internal observer*. —
8. Set the weak subscriber. —
9. If `options`'s signal exists:
   1. aborted → **`[S]`** close *subscriber* with the signal's abort reason;
   2. otherwise add an abort algorithm that removes this internal observer and, **only when the
      list is then empty**, closes *subscriber* with the signal's abort reason. **`[S]`** when it
      later runs.
10. **`[S]`** Invoke the subscribe callback with «subscriber» and `"rethrow"`; on exception E,
    **`[S]`** run `subscriber.error(E)`.

**Step 10 is not conditional on step 9.1.** A subscription whose signal was *already* aborted still
runs the producer, and every `subscriber.next()` it makes is dropped by 10.1 step 1 while its
`addTeardown` takes 10.1's step-3 branch and fires immediately. Making step 10 conditional is the
single easiest way to get this component wrong and have most tests still pass.

**Suspension points: 7** (3 dictionary reads + 1 options read + close + the callback + the error).

### 10.3 §2.1 `close a subscription`

1. If `active` is false, **return** — the re-entrancy guard, and the standard's own example is a
   teardown that aborts a controller whose abort algorithm re-enters this algorithm. —
2. Set `active` to false. —
3. **`[S]`** Signal abort the subscription controller with the reason, if one was given. — DOM §3.2:
   this runs the signal's **abort algorithms** and then fires `abort` at it, so it is two kinds of
   author code, and it is where the whole upstream chain unsubscribes.
4. For each teardown **sorted in reverse insertion order**:
   1. Re-check fully-active — *the standard says this step "runs repeatedly because each teardown
      could result in the above Document becoming inactive"*. —
   2. **`[S]`** Invoke teardown with «» and `"report"`.

**Suspension points: 2 (one of them per teardown).** Step 3 must be its own stage from step 4:
an abort algorithm registered on the subscriber's own signal can add nothing to the teardown list
(`addTeardown` on an inactive subscriber fires immediately instead), but it CAN close another
subscription, so the list is read **after** step 3 has finished, never captured before it.

### 10.4 §2.3.1 `from()` — four arms, and the order they are tried in

`convert to an Observable` is stated as an abstract operation precisely so the operators can reach it
without the Web IDL bindings. Its arms, in order:

0. If Type(value) is not Object, **throw a TypeError** — no primitive is coerced, so a String is not
   an iterable here. —
1. **From Observable**: if value's specific type is `Observable`, return it. —
2. **From async iterable**: **`[S]`** `GetMethod(value, %Symbol.asyncIterator%)` — `GetMethod`, not
   `GetIterator`, so a value with no async iterator does not throw. Undefined or null → fall to 3.
3. **From iterable**: **`[S]`** `GetMethod(value, %Symbol.iterator%)`. Undefined → fall to 4.
4. **From Promise**: `IsPromise(value)` — the brand, not a `then` read. —
5. Otherwise **throw a TypeError**. —

Each arm returns an Observable whose subscribe callback is native steps, and those steps are where
the rest of the author code lives:

- **sync iterable**: **`[S]`** `GetIterator(value, sync)` *(re-invoking the `@@iterator` getter — the
  standard notes this is deliberate and matches test expectations)*, then register an
  `IteratorClose` abort algorithm, then a `while (true)` of **`[S]`** `IteratorStepValue` →
  **`[S]`** `subscriber.next(value)` → check the signal. The next() is what re-enters 10.1, and the
  loop is unbounded, so it is a **yield point as well as a suspension point**.
- **async iterable**: **`[S]`** `GetIterator(value, async)` **synchronously**, whose throw reaches
  `subscriber.error()` **synchronously** — the standard calls this out as the only synchronous error
  an async iterable can produce — then a promise-reaction chain, each turn of which is
  **`[S]`** `IteratorNext` + `[S]` the reaction + `[S]` `subscriber.next()`.
- **Promise**: a reaction that runs `next` then `complete`, or `error`.

### 10.5 What a step machine gets wrong here, in the order it will get it wrong

1. **Walking the live internal-observer list.** Step 3 of `next` copies it, and WPT pins the copy
   directly: a `next` handler that subscribes to the same Observable adds an observer that must NOT
   receive the value currently in flight, and must receive the following one.
2. **Treating the weak subscriber as "one subscription per subscribe()".** Two `subscribe()` calls
   on one Observable share a producer and one teardown, which runs only when the **last** of them
   unsubscribes — in any order.
3. **Closing on the first unsubscribe.** Step 9.2's abort algorithm closes only when the internal
   observer list is EMPTY afterwards.
4. **Capturing the teardown list before step 3 of `close`.**
5. **Reading the `SubscriptionObserver` members in prose order.** They are lexicographic.
6. **Letting a `"report"` invocation unwind.** See 10.0.
7. **Declaring `catches_abrupt` and then making a KEYED READ.** This one is not about the standard, it is
   about this engine, and it cost a whole test file: a CALL request reports its throw through its own
   `out`, and `step_getprop_run` has no way to say "it threw" — so under `catches_abrupt` a throwing
   `[[Get]]` is delivered as `JS_EXCEPTION`, the read reports "not started", and the machine asks for the
   same property again, forever. `Observable.from(obj)` where obj's `@@iterator` **getter** throws is a
   test of its own, and it took `observable-from.any.js` from 48 results to none — a timeout, with every
   passing subtest before it discarded. A machine that both catches and reads must take the abrupt
   delivery **before** the stage runs, at exactly the stages whose request is a read.

### 10.6 Summary

| Algorithm | Suspension points | The one that surprises |
| --- | --- | --- |
| §2.1 `next` / `error` / `complete` / `addTeardown` | 6 | `addTeardown` on an inactive subscription invokes **during** `addTeardown`, FIFO |
| §2.2.1 `subscribe to an Observable` | 7 | step 10 runs the producer **even when** step 9.1 already closed the subscriber |
| §2.1 `close a subscription` | 2 (one per teardown) | the teardown list is read **after** the signal abort, and runs in **reverse** order |
| §2.3.1 `convert to an Observable` | 3 + the arm's own loop | `GetMethod`, never `GetIterator`, for the probe; a sync iterable's loop is unbounded |
| §2.3.2 an operator's subscribe callback | 1-2 subscriptions + the arm's own | takeUntil's step 5 skips the SOURCE entirely when the notifier already fired |
| §2.3.2 an operator's `next` steps | 1 callback + 1 emit (+1 convert +1 subscribe for flatMap/switchMap/catch) | the mapper is invoked with «value, idx» and "rethrow"; idx moves only on the NON-throwing path |
| §2.3.3 the promise operators | 1 read + 1 settle + 1 abort per decision | the settle precedes the abort, and the dependent signal is §3.2's, never an algorithm pair |
| §3 `when()` | 3 reads, and NO subscription | the listener's signal is the subscription controller's — that is the whole teardown |
| **Total for the core** | **18** | |

### 10.7 §2.3.2 The Observable-returning operators — one shape, ten times

**Every one of them is the same three parts**, and reading them as anything else is how they drift apart:

1. the METHOD builds an `Observable` whose subscribe callback is native steps closed over the source and the
   operator's argument;
2. that callback makes a per-subscription STATE record and an INTERNAL OBSERVER whose three algorithms read
   and write it;
3. it SUBSCRIBES to the source with **`subscriber`'s own subscription controller's signal** as the options
   signal.

Part 3 is the whole unsubscription story and it is one line in every operator. When the downstream consumer
goes away, §2.1's close-a-subscription signals that controller, §2.2.1 step 9.2's abort algorithm on the
upstream subscription removes this internal observer and — **only when the list is then empty** — closes the
upstream `Subscriber`. Nothing in an operator arranges its own teardown; a chain of ten unwinds itself.

**Which methods are step machines, and why the other six are not.** A method is a machine when something
between its entry and its return can run the page's code. `map`, `filter`, `flatMap`, `switchMap`, `catch` and
`finally` convert exactly one argument each and every one of those conversions is a Web IDL **callback function
type**, which is a brand check — no `[[Get]]`, no coercion, no user algorithm. A machine there would DECLARE a
suspension point the standard does not have. The four that are machines are machines for a stated reason:

| Method | Why it reaches the page's code |
| --- | --- |
| `takeUntil(value)` | step 2 **converts** `value` to an Observable — §2.3.1's `GetMethod` on `@@asyncIterator` then `@@iterator`, both the page's |
| `take(amount)` / `drop(amount)` | Web IDL `unsigned long long` is ToNumber, so `take({valueOf(){…}})` is the page's loop |
| `inspect(inspectorUnion)` | five dictionary members, read with `[[Get]]` in Web IDL §3.2.18's **lexicographic** order — `abort`, `complete`, `error`, `next`, `subscribe` |

**takeUntil is the only operator that subscribes twice, and the ORDER is load-bearing.** The notifier is
subscribed to FIRST, with the same subscriber's signal; its `next` **and** its `error` steps both run
`subscriber.complete()`, and it has **no complete steps at all** — a notifier that completes without emitting
must leave the mirror running. Then step 5 asks whether `subscriber`'s `active` is still true, and a notifier
that emitted synchronously has already closed it, so **the source is never subscribed to**. The internal
observer's error steps being PRESENT is what keeps §2.2.1's default error algorithm — a report — from firing
instead.

**`flatMap` and `switchMap` are the same operator with opposite answers to "a second value arrived".** flatMap
QUEUES it behind an `activeInnerSubscription` boolean and drains the queue from the inner observer's complete
steps; switchMap ABORTS the previous inner subscription's own `AbortController` and derives a new one. Both
then run the identical *process next value* steps — mapper, `idx + 1`, convert, subscribe — and both hold the
outer `complete` back: `outerSubscriptionHasCompleted` is set, and whoever finishes last runs
`subscriber.complete()`. switchMap's inner subscription is where §3.2's **dependent abort signal** is
unavoidable: its options signal is a dependent over «the inner controller's signal, the subscriber's», because
either one ending must end that subscription and neither may end the other.

**`inspect`'s abort handler is registered and then REMOVED by three of its own algorithms.** The standard's
note says why: the handler is for CONSUMER-initiated unsubscription only, so a producer that is about to
`error()` or `complete()` — which signals the very controller the handler is registered on — must unregister it
first. Its `next` handler removes it too, but only on the path where it THREW.

### 10.8 §2.3.3 The promise-returning operators — six shared steps and one difference

All eight open with the same prologue, and the ONE thing that differs is whether the operator owns an
`AbortController` of its own:

| Operator | Own controller? | Because |
| --- | --- | --- |
| `toArray`, `last` | no | they never end the subscription early, so the internal options signal IS the caller's |
| `forEach`, `every`, `first`, `find`, `some`, `reduce` | yes | their observer DECIDES to stop — a false predicate, the first value, a callback that threw |

For the six, the internal options signal is **§3.2's "create a dependent abort signal" over «controller's
signal, options's signal if non-null»**, and that primitive is not optional and not replaceable by "add an
algorithm to each that aborts the result". §3.2 states the propagation as STATE: step 4 of *signal abort* sets
every non-aborted dependent's abort reason **before** any of the abort steps run, so by the time the source's
own algorithms and `abort` listeners execute, every dependent already reads `aborted === true`. An
algorithm-based imitation aborts the dependent DURING the source's algorithm walk — one turn late, and
page-visible. It is also why a dependent signal FLATTENS (step 4.2 takes the SOURCE signals of a dependent
input), so a chain of operators is one hop deep however long it is, and step 4.2.1's assert says the flattening
is total.

Three details the WPT files pin:

- **The abort algorithm rejects `p`, and it stays registered.** `toArray`'s and `last`'s is on the CALLER's
  signal and outlives the subscription. Rejecting an already-settled promise is a no-op, which is what makes
  the "resolve, then abort my own controller" order below safe.
- **The settle comes BEFORE the abort.** `every()` resolves `false` and *then* signals its controller;
  `first()` resolves the value and *then* signals. Aborting first would run the subscription's teardown chain
  before the promise a handler is waiting on had settled.
- **`reduce`'s accumulator is "uninitialized", not "undefined".** `reduce(f, undefined)` HAS an initial value,
  and the difference is whether the first emitted value is fed to the reducer or *becomes* the accumulator.
  `hasAcc` is therefore `argc > 1`, never a test of the value.

### 10.9 §3 `EventTarget.when()` — and the listener entry point it needed

`when(type, options)` is a partial interface on `EventTarget`, so the member goes on `EventTarget.prototype`.
Its Observable's subscribe callback **subscribes to nothing**: it adds an event listener whose callback runs
the *observable event listener invoke algorithm* (`subscriber.next(event)`) and whose **signal is the
subscription controller's**. That signal is the entire unsubscription mechanism — §2.1's close signals it,
§2.7 step 6's abort steps remove the listener — and it is why this component keeps no registration of its own.

It was blocked on the C entry point, and the block was the shape of the entry rather than the storage behind
it. §2.7's listener is five fields and DOM §2.9's rebuild already stored all five (including the tristate
`passive` and a real `signal` abort algorithm), but `event_target_add_listener` took only a callback and
hard-coded the other four. `when()` names FOUR of them explicitly, so the entry point now takes the whole
listener. `passive` must stay the TRISTATE: §3 says "options's `passive` if this member EXISTS; null
otherwise", and collapsing an absent member to `false` would deny the type its default passive value.

### 10.10 HTML §7.3.1 "fully active" — the guard this whole standard opens with

§2.1's `next`/`error`/`complete`/`addTeardown`, §2.2.1's subscribe and §3's `when()` each begin with *"If the
relevant global object is a `Window` object, and its associated `Document` is not fully active, then return"*,
and §2.1's close-a-subscription re-asks it **per teardown**, because the standard says each teardown could
result in that Document becoming inactive.

The answer is a WALK, not a flag, and the two differ exactly where it matters: removing an `<iframe>` destroys
THAT navigable, and every document nested inside it stops being fully active without anything having been done
to its own navigable. So `document_fully_active` asks §7.3.1's own question — this navigable and every one
containing it — and asks it rather than remembering it, because the tree it walks is per-flow: one arm of a
fork removed the frame and its sibling did not.

The guard sits AFTER the brand test and after Web IDL's argument conversion, because those precede step 1:
`subscriber.next()` with no arguments is still a TypeError in a detached document.

## 11. DOM §2.7 / §2.8 / §2.9 — the event listener list, the event path, and dispatch

**Why this section exists.** `dispatchEvent` is the only DOM member whose RETURN VALUE depends on
what the page's own code did, and its algorithm runs page code at up to three different depths in
one call — a `handleEvent` property read, the listener itself, and the activation behaviour. It is
therefore the algorithm in which "where can this machine rest" is the whole design, and the one this
engine got structurally wrong: the walk was written as THREE legs (capture over the ancestors,
AT_TARGET over the target with both kinds of listener in registration order, bubble over the
ancestors) and the standard's walk is TWO passes over the whole path. The two shapes agree on every
example an implementer invents and disagree on the one the corpus asks for directly.

**Network was available.** Everything below was read from the live standard on **2026-08-10**:

| Standard | Source | Version read |
| --- | --- | --- |
| WHATWG DOM | `https://dom.spec.whatwg.org/` | Living Standard, §2.7, §2.8, §2.9 |
| Web IDL (call a user object's operation) | `https://webidl.spec.whatwg.org/` | Living Standard, §3.12 |

Step numbers are the standard's own list numbering as of that date.

### 11.0 Four things the live text says that a reasonable person would remember differently

1. **At the target, a CAPTURING listener runs before a bubbling one however they were registered.**
   There is no "AT_TARGET phase" in the walk. There are two loops — step 6.13 over the path in
   REVERSE with phase `"capturing"`, step 6.14 over the path in order with phase `"bubbling"` — and
   the target is simply the path item that both loops set `eventPhase` to `AT_TARGET` for. Inner
   invoke's steps 2.3/2.4 then filter by `capture` in BOTH loops. So
   `el.addEventListener("click", a, false); el.addEventListener("click", b, true)` fires `b` then
   `a`, which is exactly what `dom/events/Event-dispatch-order-at-target.html` asserts.
2. **A non-bubbling event still reaches the target in the BUBBLING loop.** Step 6.14.2.1's
   `continue` for `bubbles === false` is inside the OTHERWISE branch — the branch for a path item
   whose shadow-adjusted target is null. The target's item never reaches it.
3. **Dispatch UNSETS the stop propagation and stop immediate propagation flags at the end**
   (step 10), together with the dispatch flag. Leaving them set is invisible until the same event
   object is dispatched a second time, at which point it propagates nowhere.
4. **Removal has an effect on a dispatch already in flight, and addition does not.** The walk clones
   the listener list (invoke step 8) so an added listener does not run; a REMOVED one is skipped by
   inner invoke step 2's "whose removed is false". A clone with no `removed` field gets the second
   half backwards and re-runs a listener the page has just removed.

### 11.1 §2.7 "add an event listener", given eventTarget and listener

1. ServiceWorkerGlobalScope console warning. — *not applicable*
2. If listener's **signal** is non-null and is **aborted**, then return. —
3. If listener's **callback** is null, then return. —
4. If listener's **passive** is null, then set it to the **default passive value** given listener's
   type and eventTarget. —
5. If eventTarget's event listener list does not contain an event listener whose **type**,
   **callback** and **capture** are the listener's, then append listener. —
6. If listener's signal is non-null, then add abort steps to it: **remove an event listener** with
   eventTarget and listener. —

No step here is `[S]`. Every one of them is straight-line C over values the Web IDL conversion has
already produced. The **suspension points of `addEventListener` are all in the conversion**: `type`
is a DOMString (ToString `[S]`), and the third argument is `(AddEventListenerOptions or boolean)`,
whose dictionary conversion is four `[[Get]]`s (`capture`, `once`, `passive`, `signal`), each `[S]`.

**`flatten more options`** is what makes `passive` a TRISTATE rather than a boolean:

1. Let capture be the result of **flattening** options (a boolean IS `capture`).
2. Let once be false.
3. Let **passive and signal be null**.
4. If options is a dictionary: set once; **if options["passive"] EXISTS**, set passive; **if
   options["signal"] EXISTS**, set signal.

So an ABSENT `passive` is null and step 4 of "add an event listener" fills it from the default
passive value; a written `{passive: false}` is false and stays false. A declaration that types the
member as an IDL boolean with a `= false` default destroys that distinction before the algorithm
starts, because ToBoolean(undefined) is false and "absent" is then unrepresentable.

**`default passive value`, given type and eventTarget** — true iff BOTH:

- type is one of `"touchstart"`, `"touchmove"`, `"wheel"`, `"mousewheel"`; **and**
- eventTarget is a `Window`, **or** is a node whose node document is eventTarget, **or** whose node
  document's document element is eventTarget, **or** whose node document's body element is
  eventTarget.

### 11.2 §2.7 "remove an event listener"

1. ServiceWorkerGlobalScope console warning. — *not applicable*
2. **Set listener's removed to true** and remove listener from eventTarget's event listener list. —

Both halves, in that order, are one operation. The first is what a dispatch holding a snapshot of
this list observes; dropping it from the live list alone is invisible to the walk in flight.

### 11.3 §2.9 `dispatch`, given event and target

1. Set event's **dispatch flag**. —
2. Let targetOverride be target (or target's associated Document under the legacy target override
   flag, which only HTML uses and only for a Window). —
3. Let activationTarget be null. —
4. Let relatedTarget be the result of **retargeting** event's relatedTarget against target. —
   *(identity with no shadow trees)*
5. Let clearTargets be false. —
6. If target is not relatedTarget or target is event's relatedTarget:
   1. Let touchTargets be a new list. —
   2. Retarget each of event's touch targets into it. —
   3. **Append to an event path** with event, target, targetOverride, relatedTarget, touchTargets,
      false. —
   4. Let **isActivationEvent** be true if event is a `MouseEvent` and its type is `"click"`. —
   5. If isActivationEvent and **target** has activation behavior, set activationTarget to target. —
      *(no `bubbles` condition — this is the difference from 6.8.7)*
   6. Let slottable be target if it is assigned, otherwise null. —
   7. Let slotInClosedTree be false. —
   8. Let parent be the result of invoking target's **get the parent** with event. — **[S]** *(the
      DOM's own get the parent is straight-line; a host that defines it otherwise makes this one)*
   9. **While parent is non-null:** append it to the path, retarget relatedTarget and the touch
      targets against it, and set parent to the result of invoking ITS get the parent. Inside it,
      **6.8.7**: if isActivationEvent, **event's bubbles is true**, activationTarget is null and
      parent has activation behavior, set activationTarget to parent. — **walk of page size**
   10. Let clearTargetsItem be the last path item with a non-null shadow-adjusted target. —
   11. Set clearTargets if that item's target/relatedTarget/touch target is in a shadow tree. —
   12. Run activationTarget's **legacy-pre-activation behavior**, if it has one. —
   13. **For each item of event's path, IN REVERSE ORDER:** set eventPhase to `AT_TARGET` if the
       item's shadow-adjusted target is non-null and `CAPTURING_PHASE` otherwise, then **invoke**
       with phase `"capturing"`. — **[S] per listener**
   14. **For each item of event's path:** if the item's shadow-adjusted target is non-null set
       eventPhase to `AT_TARGET`; **otherwise**, if event's bubbles is false **continue**, else set
       eventPhase to `BUBBLING_PHASE`. Then **invoke** with phase `"bubbling"`. — **[S] per listener**
7. Set event's eventPhase to `NONE`. —
8. Set event's currentTarget to null. —
9. Set event's **path to the empty list**. —
10. **Unset event's dispatch flag, stop propagation flag, and stop immediate propagation flag.** —
11. If clearTargets, set target, relatedTarget and the touch target list to null/empty. —
12. If activationTarget is non-null: **1.** if event's canceled flag is unset, run activationTarget's
    **activation behavior** with event; **2.** otherwise run its legacy-canceled-activation
    behavior. — **[S]** *(§4.6.3's is a navigation, and a navigation fetches)*
13. Return false if event's canceled flag is set; otherwise true. —

**Which of `event`'s state is the EVENT's and not the dispatch's.** Steps 9 and 10 are the tell:
`path` is a field of the event, because `composedPath()` reads *this's path* and a dispatch that
kept the path privately can only answer with the one target it is standing on. `eventPhase`,
`currentTarget`, `target` and the three flags are the event's for the same reason.

### 11.4 §2.9 `invoke`, given a path item, event and phase

1. Let targetItem be pathItem; **while** its shadow-adjusted target is null, step back. —
2. Set event's **target** to targetItem's shadow-adjusted target. —
3. Set event's relatedTarget and touch target list from pathItem. —
4. **If event's stop propagation flag is set, return.** — *(tested per PATH ITEM, not per listener)*
5. Initialize event's **currentTarget** to pathItem's invocation target. —
6. Let listeners be a **clone** of currentTarget's event listener list. —
7. **inner invoke** with event, listeners, phase, invocationTargetInShadowTree. —

With no shadow trees every item's shadow-adjusted target is the target, so step 2 is one write for
the whole walk and step 1's backward scan is empty.

### 11.5 §2.9 `inner invoke`, given event, listeners and phase

1. Let found be false. —
2. For each listener **whose removed is false**:
   1. If event's type is not listener's type, continue. —
   2. Set found to true. —
   3. If phase is `"capturing"` and listener's capture is false, continue. —
   4. If phase is `"bubbling"` and listener's capture is true, continue. —
   5. If listener's **once** is true, **remove an event listener** given event's currentTarget and
      listener. — *(BEFORE the call, so a re-entrant dispatch cannot see it)*
   6. Let global be the listener callback's associated realm's global object. —
   7-8. Save the global's **current event** and set it to event. —
   9. If listener's **passive** is true, set event's **in passive listener flag**. —
   10. Record timing info. — *(no scriptable result headless)*
   11. **Call a user object's operation** with listener's callback, `"handleEvent"`, « event », and
       event's currentTarget. **If this throws, REPORT the exception** and continue. — **[S]**
   12. Unset the in passive listener flag. —
   13. Restore the global's current event. —
3. Return found. —

**Step 11 is TWO suspension points, not one** — Web IDL §3.12 "call a user object's operation":

- 9. Let X be O.
- 10. **If IsCallable(O) is false:**
  - 1. Let getResult be **Get(O, opName)**. — **[S]** *(an accessor or a Proxy trap)*
  - 2. Abrupt → return it.
  - 3. Set X to getResult.[[Value]].
  - 4. If IsCallable(X) is false, **throw a TypeError** (in O's realm).
  - 5. **Set thisArg to O**, overriding the provided value.
- 11-12. Call X with thisArg and the argument list. — **[S]**

So `el.addEventListener("x", {handleEvent(e){…}})` is an ordinary registration whose method is read
**per invocation**, and the receiver of that call is the OBJECT, not `currentTarget`. A component
that requires a callable listener at registration time drops the registration silently; one that
requires it at invocation time drops the call silently. Both are wrong in the same place: the
callback type is a callback INTERFACE (§2.8 `callback interface EventListener`), not a function.

**And step 2.11's "report" is a THIRD suspension point.** HTML §8.1.4.6 "report an exception":

1. Let notHandled be true.
2. Let errorInfo be the result of **extracting error information** from exception — `error` is the
   exception; `message`, `filename`, `lineno` and `colno` are *implementation-defined values derived
   from exception*, which is the standard's own wording and is why a headless engine with no script
   position is conforming here rather than stubbed.
3-4. The **muted errors** branch, for a classic script fetched cross-origin without CORS. —
5. If global is **not in error reporting mode**: set the flag; fire an event named `error` at global,
   using **ErrorEvent**, **cancelable**, with errorInfo's attributes, and set notHandled to the
   result; unset the flag. — **[S]**
6-7. The worker propagation, then the developer console. —

The in-error-reporting-mode flag is what stops a throwing `onerror` reporting itself forever, and it
is **per global**, which is why it lives on the global object and not in the reporting component.

### 11.6 Suspension points per entry point

| Entry point | Suspension points | The one that surprises |
| --- | --- | --- |
| `addEventListener(type, cb, options)` | 5 | four of them are the OPTIONS dictionary's `[[Get]]`s; the algorithm itself has none |
| `removeEventListener(type, cb, options)` | 2 | `type`'s ToString runs even when the callback is null |
| `dispatchEvent(event)` | 2 + 3 per listener + 1 | the second per-listener one is the `handleEvent` READ; the third is the `error` event REPORTING what the listener threw, and the walk continues after it |
| the engine's own fire (`load`, `DOMContentLoaded`, `abort`) | the same | it is the SAME machine; only the reach differs |

### 11.7 Stage-boundary consequences for a step machine

1. **The path walk (step 6.9) is its own stage.** It is a walk of the page's tree, so it must be
   able to yield between parents; and it must not restart at step 1 when it resumes.
2. **The capturing pass and the bubbling pass are two stages, not one stage with a leg counter.**
   The counter shape cannot express "the target item is invoked in BOTH passes with `AT_TARGET`",
   which is the only way §2.9 orders at-target listeners.
3. **The `handleEvent` read needs its own resume marker, distinct from the call's — and the REPORT
   needs a third.** One listener suspends at up to three different steps (its operation lookup, its
   own body, and the `error` event fired for what that body threw), and a machine with one marker
   resumes one of them into another.
4. **`once` removal happens before the call**, so a resume must NOT re-read the record it came from:
   that record is now `removed` and the walk would skip the very listener whose answer is arriving.
5. **The activation behaviour is a stage after the cleanup**, not before it — a behaviour that reads
   `currentTarget` must see null.

## 12. DOM §5 "Ranges" and §4.10/§4.11 — the live range, and the three tree algorithms that move it

**Why this section exists.** §5's boundary points are only "live" because five *other* algorithms
adjust them: `insert`, `remove`, `replace data`, `split a Text node`, and `move`. Four of those live
outside §5, and the engine had two of them — insert's step 6 and the pre-remove steps — while
`replace data` carried a comment saying its steps 8-11 were "honestly absent" and `split` did not
exist at all. That is not a §5 gap: a page reading `range.startOffset` after `t.appendData("x")` got
the *unadjusted* offset, and nothing in §5 could have told it so. This section is the step lists for
all of them, plus §5.5's five content-moving members, which were absent for the stated reason that
`replace data` and `split` did not exist.

**Network was available.** Read from the live standard on **2026-08-10**:

| Standard | Source | Version read |
| --- | --- | --- |
| WHATWG DOM | `https://dom.spec.whatwg.org/` | Living Standard, §5 and §4.10/§4.11 |

### 12.0 Three things the live text says that a reasonable person would remember differently

1. **A boundary point offset is in UTF-16 CODE UNITS, because §4.4's "length" is.** `node_length`
   returned the *byte* length of a CharacterData node's UTF-8 store while `CharacterData.length`
   returned code units, so the two disagreed for every non-ASCII text node and a Range's offsets
   were byte offsets into a string the page indexes in code units. Every §5 algorithm is stated over
   `length`; this is a §4.4 bug that §5 inherits wholesale.
2. **The `data` setter is `replace data`, not a write.** §4.10's `data` setter and §4.4's
   `nodeValue` setter are both defined as *replace data with node this, offset 0, count this's
   length, and data the new value*. Writing the bytes straight through skips steps 8-11, so
   `t.data = "x"` left every live range inside `t` pointing past the end of it.
3. **`extract` and `clone the contents` are one algorithm with three differences.** The standard
   states them as two lists; they differ only in that extract repositions the range (steps 12-15),
   MOVES a contained child where clone deep-copies it (step 18 vs 14), and empties the CharacterData
   it took bytes from. Every other line is word for word the same, including the whole
   partial-containment rule.

### 12.1 §5.5 "live range pre-remove steps", given a node `node`

1. Let `parent` be `node`'s parent. —
2. Assert: `parent` is non-null. — *(a DCHECK; `remove` is never reached for a parentless node)*
3. Let `index` be `node`'s index. —
4. For each live range whose **start node** is an inclusive descendant of `node`, set its start to
   (`parent`, `index`). —
5. The same for **end**. —
6. For each live range whose start node **is** `parent` and start offset is **greater than**
   `index`, decrease its start offset by 1. —
7. The same for **end**. —

**Steps 6-7 must re-read the boundary node**, because steps 4-5 may have just moved it onto
`parent`. The standard runs the four in this order for exactly that reason. `>` and not `>=`: an
offset **equal** to the index already names the removal site.

### 12.2 §4.2.3 `insert`'s step 6 — the live-range half

> If `child` is non-null: for each live range whose start node is `parent` and start offset is
> greater than `child`'s index, increase its start offset by `count`; the same for end.

`count` is the number of nodes actually inserted (a DocumentFragment contributes its children). This
engine's DOM-mutation chokepoint fires per node, so it runs with `count` = 1 and the inserted node's
own post-insertion index — which is **equivalent**: inserting N nodes one at a time at successive
indices moves an offset past all of them by exactly N, and an offset equal to the first index is
moved by neither form.

### 12.3 §4.10 `replace data(node, offset, count, data)` — steps 8-11

1. Let `length` be `node`'s length. —
2. If `offset` > `length`, throw **`IndexSizeError`**. —
3. If `offset` + `count` > `length`, set `count` to `length` − `offset`. —
4. Queue a `characterData` mutation record. — *(MutationObserver is not built; honestly absent)*
5. Insert `data` into `node`'s data after `offset` code units. —
6. Let `deleteOffset` be `offset` + `data`'s length. —
7. Starting from `deleteOffset` code units, remove `count` code units. —
8. For each live range whose start node is `node` and start offset is **greater than `offset` but
   less than or equal to `offset` + `count`**: set its start offset to `offset`. —
9. The same for **end**. —
10. For each live range whose start node is `node` and start offset is **greater than `offset` +
    `count`**: increase it by `data`'s length and decrease it by `count`. —
11. The same for **end**. —

**Steps 8-9 and 10-11 cannot interfere.** Step 8 only ever writes `offset`, which is never greater
than `offset + count`, so a boundary point step 8 moved can never satisfy step 10's test. Writing
them as an if/else is a different algorithm only in appearance — but the four operands are in **code
units**, and a caller that hands over a UTF-8 byte length moves a boundary point by two where
`insertData(0, "é")` moves it by one.

### 12.4 §4.11 `split a Text node(node, offset)`

1. Let `length` be `node`'s length. —
2. If `offset` > `length`, throw **`IndexSizeError`**. —
3. Let `count` be `length` − `offset`. —
4. Let `newData` be **substring data** of `node` with `offset` and `count`. —
5. Let `newNode` be a **new text node** on **`node`'s node document** and `newData`. —
6. Let `parent` be `node`'s parent. —
7. If `parent` is non-null:
   1. **Insert** `newNode` into `parent` before `node`'s next sibling. —
   2. For each live range whose start node is `node` and start offset > `offset`: set its start node
      to `newNode` and decrease its start offset by `offset`. —
   3. The same for **end**. —
   4. For each live range whose start node is `parent` and start offset **equals** `node`'s index +
      1: increase it by 1. —
   5. The same for **end**. —
8. **Replace data** of `node` with `offset`, `count`, and the empty string. —
9. Return `newNode`. —

**7.1's own live-range step and 7.4 are not redundant.** 7.1's insert moves every offset in `parent`
**greater than** `node`'s index + 1; 7.4 handles the one that is **equal** to it. Together: every
offset at or after the split point moves along by one. And step 8's own steps 8-11 have nothing left
to move, because 7.2-7.3 already took every boundary point past the split point off `node` — which
is why the standard runs them before it. Step 5 names **`node`'s** node document, not the running
realm's, so a split inside an adopted subtree keeps that subtree's document.

### 12.5 §5.5 `deleteContents()`

1. If this is **collapsed**, return. —
2. Let `originalStartNode`/`Offset`, `originalEndNode`/`Offset` be this's boundary points. —
3. If `originalStartNode` **is** `originalEndNode` and it is a CharacterData node: **replace data**
   with (`originalStartOffset`, `originalEndOffset − originalStartOffset`, `""`) and return. —
4. Let `nodesToRemove` be all nodes **contained in** this, in tree order, **omitting any node whose
   parent is also contained**. —
5. Let `newNode`, `newOffset` be null. —
6. If `originalStartNode` is an inclusive ancestor of `originalEndNode`: `newNode` =
   `originalStartNode`, `newOffset` = `originalStartOffset`. —
7. Otherwise: walk `referenceNode` up from `originalStartNode` while its parent is non-null and not
   an inclusive ancestor of `originalEndNode`; `newNode` = `referenceNode`'s parent, `newOffset` =
   `referenceNode`'s index + 1. —
8. Set this's start **and** end to (`newNode`, `newOffset`). —
9. If `originalStartNode` is CharacterData: **replace data** with (`originalStartOffset`, its length
   − `originalStartOffset`, `""`). —
10. For each node of `nodesToRemove`, in tree order: **remove** it. —
11. If `originalEndNode` is CharacterData: **replace data** with (`0`, `originalEndOffset`, `""`). —

**It is NOT `extract` with the fragment dropped.** Step 4's omission rule is what replaces the spine
`extract` descends, and there is no recursion in this algorithm at all. Step 4's walk is bounded by
the **common ancestor** — every contained node is a descendant of it — and a contained node's
descendants are all contained with a contained parent, so the walk skips their subtrees rather than
visiting them to discard them.

### 12.6 §5.5 `extract` a live range / `clone the contents` of one

1. Let `fragment` be a new DocumentFragment on **range's start node's node document**. —
2. If range is collapsed, return `fragment`. —
3. Snapshot the four boundary values as `original*`. —
4. If `originalStartNode` **is** `originalEndNode` and it is CharacterData: clone it, set the
   clone's data to **substring data** (`originalStartOffset`, `originalEndOffset −
   originalStartOffset`), append the clone to `fragment`, *(extract only)* **replace data** of the
   original with the same span and `""`, and return `fragment`. —
5. Let `commonAncestor` be **get the common ancestor** of range. —
6. Let `firstPartiallyContainedChild` be null. —
7. If `originalStartNode` is **not** an inclusive ancestor of `originalEndNode`, set it to the
   **first** child of `commonAncestor` that is **partially contained**. —
8. Let `lastPartiallyContainedChild` be null. —
9. If `originalEndNode` is **not** an inclusive ancestor of `originalStartNode`, set it to the
   **last** such child. —
10. Let `containedChildren` be all children of `commonAncestor` **contained in** range, in tree
    order. —
11. If any member of `containedChildren` is a doctype, throw **`HierarchyRequestError`**. —
12-15. *(extract only)* `newNode`/`newOffset` exactly as `deleteContents` steps 5-7, then set
    range's start **and** end to (`newNode`, `newOffset`). —
16. If `firstPartiallyContainedChild` is CharacterData *(then it **is** `originalStartNode`)*: clone
    it with data **substring** (`originalStartOffset`, its length − `originalStartOffset`), append
    to `fragment`, *(extract only)* replace data of the original with the same span and `""`. —
17. Otherwise, if it is non-null: clone it **shallowly**, append the clone to `fragment`, build a
    subrange (`originalStartNode`, `originalStartOffset`) → (`firstPartiallyContainedChild`, its
    length), **recurse**, and append the subfragment to the clone. —
18. For each `containedChildren`: *(extract)* **append** it to `fragment` — which pre-inserts and so
    **removes** it, running §12.1 — or *(clone)* deep-clone it and append the copy. —
19-20. The mirror of 16-17 for `lastPartiallyContainedChild`, with (`0`, `originalEndOffset`) and a
    subrange (`lastPartiallyContainedChild`, 0) → (`originalEndNode`, `originalEndOffset`). —
21. Return `fragment`. —

**Two consequences for a step machine.** (a) Step 17's recursion descends one level per iteration
between a boundary node and the common ancestor — a **page-controlled** depth, so it is an explicit
frame stack and never C recursion. (b) `containedChildren` must be **snapshotted at step 10**: step
18 removes them from the sibling chain and steps 12-15 have already moved the range, so a cursor
that asked "is the next sibling still contained" would be asking a question whose answer this
algorithm is changing.

**Step 18's clone arm is this same algorithm.** A deep copy of a contained child is the contents of
the range that selects all of it, so it is one more frame — which is what keeps it preemptible and
keeps the engine from growing a second recursive copier beside §4.4's.

### 12.7 §5.5 `insert a node` into a live range

1. If range's start node is a **ProcessingInstruction** or **Comment**, is a **Text** node whose
   parent is null, or **is** `node`, throw **`HierarchyRequestError`**. —
2. Let `referenceNode` be null. —
3. If range's start node is a Text node, set `referenceNode` to it. —
4. Otherwise, set it to the child of range's start node at index range's start offset, or null. —
5. Let `parent` be range's start node if `referenceNode` is null, otherwise `referenceNode`'s
   parent. —
6. **Ensure pre-insert validity** given `node`, `parent`, `referenceNode`, and « ». —
7. If range's start node is a Text node, set `referenceNode` to the result of **splitting** it at
   range's start offset. —
8. If `node` **is** `referenceNode`, set `referenceNode` to its next sibling. —
9. If `node`'s parent is non-null, **remove** `node`. —
10. Let `newOffset` be `parent`'s length if `referenceNode` is null, otherwise `referenceNode`'s
    index. —
11. Increase `newOffset` by `node`'s **length** if `node` is a DocumentFragment, otherwise by 1. —
12. **Pre-insert** `node` into `parent` before `referenceNode`. —
13. If range **is collapsed**, set range's end to (`parent`, `newOffset`). —

**Step 13 is evaluated after step 12**, so it reads the range as the insertion's own live-range
steps have just left it. Step 6 is the **whole** of `ensure pre-insert validity` — DOM §4.2.3's
eleven-step list, which the engine's other insertion sites carried only the ancestor half of.

### 12.8 §5.5 `surroundContents(newParent)`

1. If a **non-Text** node is partially contained in this, throw **`InvalidStateError`**. —
2. If `newParent` is a Document, DocumentType or DocumentFragment, throw
   **`InvalidNodeTypeError`**. — *(CharacterData is deliberately not checked here; it throws later
   from step 5, "for historical reasons")*
3. Let `fragment` be the result of **extracting** this. —
4. If `newParent` has children, **replace all** with null within it. —
5. **Insert** `newParent` into this. — *(§12.7, which is where a CharacterData `newParent` throws)*
6. **Append** `fragment` to `newParent`. —
7. **Select** `newParent` within this. —

**Step 1 is O(depth), not O(document).** The partially contained nodes are exactly the inclusive
ancestors of one boundary node up to (but not including) the common ancestor, so the check walks two
chains rather than the subtree.

### 12.9 Suspension points in Algorithm group 12

| Entry point | `[S]` count | Where |
| --- | --- | --- |
| `Range.deleteContents()` | 1 | CE epilogue |
| `Range.extractContents()` / `cloneContents()` | 1 | CE epilogue |
| `Range.insertNode(node)` | 1 | CE epilogue |
| `Range.surroundContents(newParent)` | 1 | CE epilogue |
| `Text.splitText(offset)` | 1 | ToIndex-ish `unsigned long` conversion |
| `CharacterData.appendData/insertData/deleteData/replaceData` | 1 | the `DOMString`/`unsigned long` conversions |
| `Range` boundary-point setters, `compareBoundaryPoints`, `comparePoint`, `isPointInRange`, `intersectsNode` | 1 | the `unsigned long` conversion where there is one |
| the live-range steps of §12.1-§12.4 | **0** | none of them can reach author code |

**Suspension points in Algorithm group 12: none inside the algorithms themselves.** Every one of
§5's members is straight-line tree work between its Web IDL prologue and its `[CEReactions]`
epilogue. What makes four of them **machines** is not author code — it is that they walk the page's
tree: the stringifier, `deleteContents`, `extract`/`clone the contents` and `surroundContents` are
each O(document) and rest one node per step, exactly as §4.4's `cloneNode` and §8.4's serialiser do.

---

## 13. The COLD TIER — what a parked flow's snapshot is made of, and what has to exist before any of it can cross

**Why this section exists.** CLAUDE.md promises this mechanism in four places — §Time-travel-resume
("the cold low-value tail serializes to IDB as suspended snapshots and resumes on demand"),
§scheduler ("STARVE means deprioritize-and-page (resumable, cross-session), NEVER terminate"; "an
engine self-parks its residue to the IDB cold tier under pressure") and §Disposition's RAM→DISK
floor — and `engine.c` names it at its own flow-compile OOM as "the cold tier that pages the lowest-
value tail to disk". **This build has none.** Every parked flow is hot in RAM, for the life of the
session. This section is what a reader has to know before writing one, in the order the subproblems
come.

**The standard here is CLAUDE.md, not a W3C document**, so there is no "network was available"
table; what takes its place is a MEASUREMENT table, because the first subproblem is not code. The
numbers below are from `node engine/build.mjs min` — test_forced.c's minimal fixture, whose
`Array.from(state.items)` over unknown injected state forks one sibling per position and therefore
never drains (§NO BOUNDS: every length is a world, and the walk takes the longer arm forever).

### 13.0 The measurement had to come first, and the instrument was blind

Two things had to be built before anything could be measured at all, and both are landed.

**The progress stream's cadence was keyed on `g_switches`.** In this run the walking flow is never
outranked — `flow_weight` is reward + optimism − aging, the walker's reward is every endpoint it
emitted earlier and the aging is 1e-6 per scheduler step — so the switch count is **2 for the whole
run**, and the stream emitted ONE line while RSS climbed past 2.7 GB. A cadence keyed on a counter
that stops is a report that goes silent exactly when there is something to report: the same defect
as a corpus file the collector does not collect, because the output LOOKS complete. Both the cadence
and the seam verdict now read one `engine_work_done()` — forks + flows + jobs + switches — so they
measure the same quantity and cannot drift.

**Nothing reported what a parked flow costs.** `@SWAP` says what a context SWITCH costs;
`@HEAP` says what the RUNTIME holds; between them a parked flow's own state had no row. `@COLD`
(`solver/cold.c`) is that row, per part, and it separates PER-FLOW from SHARED — because that
distinction is the whole design question for a pager, not a detail of it.

### 13.1 What a snapshot is made of — measured

`min`, at four points in one run (KiB unless stated):

| flows | decision vectors | pending replies | pin chain | delta heads | Flow structs | per-flow total | frame chains (runtime) | C alloc live |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1006 | 506 | 705 | 198 | 30 | 157 | 1399 | ~930 | 8001 |
| 4014 | 31708 | 5648 | 1600 | 118 | 627 | 38837 | ~3700 | 51865 |
| 9518 | 44357 | 6322 | 1791 | 280 | 1487 | 47699 | 8579 | 62226 |
| 16046 | 125915 | 11265 | 3206 | 442 | 2507 | 140130 | 14648 | 165039 |

Read it as four facts.

1. **The decision vector was the whole problem, and it was quadratic.** 16046 flows held
   **128,745,080** decision slots; 16046²/2 is 128,736,529. `decide_fork_blob` gave every sibling
   its own `malloc(cursor)` + `memcpy` of the parent's prefix — 123 MB of the 137 MB of per-flow
   snapshot the frontier was holding. §13.2.
2. **The suspended heap-frame chains are the SMALL part.** Every frame chain in the run together is
   14.6 MB at 16046 flows — about 930 bytes per flow, flat — and the JS object count is *flat at
   2093* for the whole run. The part with no encoder is not the part with the bytes.
3. **The shared chains are already right.** The heap COW chain holds 2 KiB and the DOM chain 250 KiB
   across sixteen thousand flows; the delta heads total 442 KiB. cow.c's and dom_cow.c's structural
   sharing works, which is exactly why a naive per-flow serializer would be wrong: it would write
   each shared segment once per flow and multiply the sharing back out.
4. **The pending register is inherited whole at every fork.** Eight entries per flow (the fixture's
   eight outstanding fetches), each with its URL, method and headers deep-copied — 11 MB at 16046
   flows. It is O(1) per flow, so it is not the shape the decision vector was, but it is the largest
   remaining per-flow row and it is named here rather than left for the next reader to rediscover.

### 13.2 Subproblem 1 (LANDED): the decision vector is a chain, not a copy

The fourth and last instance of cow.c's refcounted immutable segment, and the one that was still
copying. A sibling's decision vector is, by construction, the parent's decisions up to the branch
plus one byte — and that prefix is frozen at that instant, because the cursor only moves forward and
a replayed slot is read, never written. So `decide.c` now holds a mutable HEAD over a chain of
`DecSeg`, `decide_fork_blob` freezes the head and hands the sibling one more segment holding its
arm, and a park/resume with no decisions between them pushes no segment at all (the same guard
`concolic_pins_suspend` keeps, for the same reason: the chain's depth must count FORKS, not
switches). Every reference is released by `flow_registry_free`, which is the one teardown every host
already runs — a `decide_free()` line copied into the three hosts' teardowns would be the hand-
picked list `build.mjs` warns about, and one of them would eventually not have it.

Measured, same fixture, same four points (KiB):

| flows | 1006 | 4014 | 9006 | 16046 |
| --- | --- | --- | --- | --- |
| copied — the whole prefix, per flow | 506 | 7916 | 39720 | 125915 |
| chained — one blob header per flow | 7 | 31 | 70 | 125 |
| …plus the shared chain, counted once | 41 | 164 | 369 | 658 |
| per-flow snapshot total | 1399 → 901 | 11473 → 3588 | 47699 → 8049 | 140130 → 14340 |
| whole engine, C allocator live | 4057 → 3633 | 18559 → 10974 | 62226 → 23249 | 165039 → 40447 |

The ratio is not the point; the SHAPE is. The first row is quadratic and the other two are linear,
so the gap is unbounded — and it also removes a quadratic in TIME, since every fork was doing that
`memcpy`. The same wall clock now reaches 29550 flows where it reached 16046, which is why RSS per
SECOND can look similar while RSS per FLOW is four times lower: the honest comparison is at a
matched flow count, and that is what the table is.

### 13.3 Subproblem 2 (NOT BUILT): the snapshot value codec, which is what actually blocks the tier

A cold tier only helps if paging a flow out **frees** what it held. A parked flow's roots are its
suspended heap-frame chain, its COW delta head, its DOM head, its queued jobs, its pending replies
and its `fn` — and every one of those holds live `JSValue`s. So the encoder must answer, for each
value, one of two things:

- **SHARED with the live heap** (a baseline object, a prototype, the global) — externalize by a
  stable id and keep the identity on resume. The primitive exists and is the right one:
  `remote_object_export` mints one id per object and holds a reference, which is how
  `w.document === w.document` stays true across an agent boundary. It frees nothing, and it is not
  supposed to: those objects are alive anyway.
- **PRIVATE to this flow** — serialize by value and free. This is where the RAM comes back, and the
  discriminator already exists in the engine: `JS_ObjFlowGen(obj)` against the delta's `fork_gen` is
  precisely "was this object created after the last fork", which is the test `cow_capture` uses to
  keep a delta O(shared-state-touched).

**What does not exist is the second half's encoder.** `structured_clone.c` is HTML §2.7 and refuses
by design exactly the kinds a parked flow is made of: a function, a Proxy, a Promise, a platform
object. A suspended activation additionally holds closure `var_ref`s, a `JSFunctionBytecode` with a
`cur_pc` into it, generator and async states, and a `cont_state` per continuation-holding builtin —
354 step machines, each with its own struct. `JS_FlowClone`'s `clone_deep_flow` is the existing
proof that the shapes CAN be walked; an encoder is that walk with a wire format and an object table
instead of a `js_dup`.

**Do not build a partial one.** An encoder that externalizes everything it cannot serialize compiles,
runs, reports a paged-out count, and frees nothing — a cold tier that is a stub in §NO STUBS' sense,
and worse than none because the counter would say it works.

### 13.4 Subproblem 3 (NOT BUILT): the store edge, and where it is

Per SECURITY.md the persistent store is IndexedDB **in the offscreen**, reached through the trusted
bridge; `chrome.storage.local` is banned and the untrusted WASM may not touch IDB. So the engine
side of this is a host edge, and the edge it should be is the one that already exists:
`engine_host_request` / `engine_host_answer`. A flow paged out is a flow whose bytes the host holds;
a flow paged **in** is a flow BLOCKED on a host request for its own snapshot — which is already a
first-class state (`FLOW_PENDING_HOSTREQ`, `flow_blocked`), already ranked by the one WFQ, and
already resumable. There is no second queue and no second pump to add.

**The unit that crosses is the SEGMENT, not the flow.** §13.1's third fact is why: a flow's chains
are shared with every sibling forked below them, so a segment is written once under its own key and
a flow's record names its chain as a list of those keys. A store that wrote a flow's whole chain per
flow would turn 250 KiB of DOM chain into 16046 copies of it.

### 13.5 Subproblem 4 (NOT BUILT): the admission step

§scheduler: "Cold-tail resume is the SAME admission step (not a separate loop), ranked by a
`frontierWeight` estimator (emit-per-visit, since a parked flow has no live CPU to age by)." So this
is not a new loop in `engine_sched_step` — it is one question asked where the WFQ already picks:
over the RAM working-set floor, the lowest-`frontierWeight` PARKED flow serializes and is released;
`flow_best` selecting a cold flow is what pages it back. **The razor is §scheduler's:** a resume
that drops, starves, skips, reorders or forgets any flow is a CAP, banned — so the assertion this
step owes is that a paged-out-and-resumed flow's decision vector, cursor, constraint and delta are
byte-identical to what it parked with, checked at the resume rather than hoped for.

### 13.6 What `min` does NOT do, and why that is correct

It does not terminate, and it must not be made to. The walk is unbounded by design — every length of
an unknown collection is a world — and `run_scheduler`'s completion condition is that the frontier
drains. The cold tier does not change that and is not meant to: what it changes is whether the
unboundedness is paid in RAM or on disk. A run that grows linearly and pages its tail is the
designed behaviour; a run that grows quadratically in RAM was a data-structure bug, which is §13.2.

---

## 14. HTML §4.13.2 / §4.13.5 / §4.13.6 — the HTMLElement constructor, the UPGRADE, and the reaction drain

**Why this section exists.** A custom element's constructor body is code nothing else in the program
calls, and until this section's diff it never ran. The engine had §4.13.4's `define`, §4.13.6's
element queues and §4.13.2's `[HTMLConstructor]` — everything except the one algorithm that joins
them. `upgrade an element` was a PROTOTYPE SWAP: the wrapper was re-pointed at the class's
`prototype` and a `connectedCallback` reaction enqueued, so `el instanceof X` and `el.method()` were
true while `constructor(){ super(); this.routes = … }` had never executed. The construction stack
existed (`ce_define_commit` allocated one per definition, `js_ce_html_ctor` read it) and **nothing
ever pushed onto it**, so §4.13.2's steps 10-15 — the half that returns the node the page already
holds — were unreachable by construction.

**Network was available.** Everything below was read from the live standards on **2026-08-11**:

| Standard | Source | Version read |
| --- | --- | --- |
| WHATWG HTML | `https://html.spec.whatwg.org/` | Living Standard, §4.13.2, §4.13.5, §4.13.6 |
| WHATWG DOM | `https://dom.spec.whatwg.org/` | Living Standard, "create an element", "insert a node" |

Step numbers are the standard's own list numbering as of that date.

### 14.0 Four things the live text says that a reasonable person would remember differently

1. **The already-constructed marker throws a `TypeError`, not an `InvalidStateError`.** §4.13.2 step
   11 says `TypeError` in as many words, and both shapes that reach it are ordinary page bugs — a
   constructor that `new`s its own class before `super()`, and one that calls `super()` twice. This
   engine threw an `InvalidStateError`, which is a `DOMException`: a page's
   `catch (e) { e instanceof TypeError }` answers false for it, and so does the corpus.
2. **The upgrade is ENQUEUED, never performed, by "try to upgrade".** §4.13.5's "try to upgrade an
   element" is two steps — look the definition up, then *enqueue a custom element upgrade reaction* —
   and §4.13.6's reaction queue therefore holds TWO KINDS of entry, which its invoke SWITCHES on.
   This is not bookkeeping: the insertion that triggers an upgrade happens inside a C tree walk, and
   step 8.3 CONSTRUCTS the page's class. An upgrade performed at the insertion point would be a
   `JS_CallConstructor` from C, which is the drive-to-completion this engine aborts on.
3. **The custom element state has FIVE values and three of them coexist with a non-null definition.**
   "undefined", "uncustomized", "failed", "precustomized", "custom". §4.13.5 step 1 returns early for
   the first two and for nothing else, so an element whose upgrade THREW must never be upgraded
   again — and a boolean "does this wrapper carry a definition" cannot express that, because step 2
   sets the definition BEFORE step 8 can fail. DOM's "create an element" gives a fresh element
   "undefined" exactly when its local name is one §4.13.1 would accept, and "uncustomized"
   otherwise, which is why an absent state is DERIVED from the name rather than written at every
   creation site (one of which is the HTML parser).
4. **DOM's insertion steps are a BRANCH, not two calls.** "If inclusiveDescendant is custom, then
   enqueue a `connectedCallback` reaction. **Otherwise**, try to upgrade it." Doing both would fire
   `connectedCallback` twice for a freshly upgraded element, because §4.13.5's own step 5 enqueues
   that same reaction.

### 14.1 §4.13.5 "upgrade an element", given definition and element

1. If element's custom element state is not "undefined" or "uncustomized", then return. —
2. Set element's custom element definition to definition. —
3. Set element's custom element state to "failed". —
4. For each attribute in element's attribute list, in order, **enqueue a custom element callback
   reaction** with element, `"attributeChangedCallback"`, and « attribute's local name, null,
   attribute's value, attribute's namespace ». —
5. If element is **connected**, then enqueue a custom element callback reaction with element,
   `"connectedCallback"`, and « ». —
6. Add element to the end of definition's **construction stack**. —
7. Let C be definition's constructor. — *(7.5-7.6's active custom element constructor map is the
   scoped-registry mechanism; there are no scoped registries, so there is nothing to save or
   restore. It becomes real state in the same diff that makes `customElementRegistry` a creation
   option.)*
8. Run the following steps **while catching any exceptions**:
   1. disable-shadow check. — *not applicable: `attachInternals` is absent*
   2. Set element's custom element state to "precustomized". —
   3. Let constructResult be the result of **constructing C**, with no arguments. — **`[S]`**
   4. If `SameValue(constructResult, element)` is false, then throw a `TypeError`. —
9. Remove the last entry from the end of definition's construction stack. — *(regardless of whether
   the above threw)*
10. Form-associated half. — *not applicable: `formAssociated` is absent*
11. Set element's custom element state to "custom". —

**And if the above threw:** set the definition to null, **empty element's custom element reaction
queue**, and rethrow — which §4.13.6 immediately catches and REPORTS. The state stays "failed" or
"precustomized", which is what makes step 1 refuse the retry.

**Step 8.3 is the only `[S]`, and it is the point of the whole section.** It is a
`step_construct_run` on the drain's own `phase`/`cb`, so the flow parks inside the page's
constructor exactly as it parks inside a lifecycle callback — the constructor may hold a loop, an
`await` or a DOM mutation, and it suspends and resumes at any depth like any other flow.

**The algorithm is a sub-algorithm of the drain, not a machine of its own.** §4.13.6 invokes it and
must inspect its completion (to report it), so a separate `JSTrampStepDef` would need a definition,
a stage list and a park protocol solely to be driven by one caller — which is what a two-value
cursor (`up_stage`) on the drain's state already is. `up_stage` is zero exactly when no upgrade is
in flight, which is what `custom_elements_queue_arm` reads.

### 14.2 §4.13.6 "invoke custom element reactions in an element queue" — the SWITCH

1. While queue is not empty:
   1. Let element be the result of dequeuing from queue. —
   2. Let reactions be element's custom element reaction queue. —
   3. Repeat until reactions is empty: remove the first reaction and **switch on its type**:
      - **upgrade reaction** — **upgrade** element using the reaction's definition. If this throws,
        **catch it and report it** for the definition's constructor's realm's global. — **`[S]`**
        (§4.13.5 step 8.3, and again inside HTML §8.1.4.6's `error` event)
      - **callback reaction** — invoke the reaction's callback function with its arguments and
        `"report"`, `this` = element. — **`[S]`** (the callback, and again inside the report)

So the drain rests at **three** distinct spec steps, and each is a declared stage: the callback, the
construct, and the report. One label for all three would name a resume point that means three
things, which a cold-tier resume cannot report and a `step_stage_check` cannot assert. Those stages
are appended to EVERY declared member's list by `idl_method_id_step` (`IDL_EPILOGUE_STEPS`) and
declared again for the backup queue's own machine, which static-asserts that its three enum values
ARE `CE_ARM_CALLBACK`/`CE_ARM_UPGRADE`/`CE_ARM_REPORT`.

**Both arms CATCH, so every machine that drives this drain must declare `catches_abrupt`** — the
per-member `JSTrampStepDef` the IDL pool builds, and `js_ce_backup_def`. Without it, one throwing
custom element constructor tore down the member that was draining and dropped every reaction queued
behind it. The declaration is made once, for all members, because the epilogue is there for all
members; a per-member opt-in is a line to forget on the member that first needs it. **Nowhere else
does an IDL member catch** — an argument coercion's throw and the member body's own request must
propagate exactly as before — so `js_idl_args_step_inner` re-raises an abrupt delivery at its top
whenever it is not inside the epilogue. Re-entering the conversion loop with it would re-issue the
keyed read the page's getter just threw from, which is an infinite re-ask, not a slow path.

### 14.3 §4.13.6 step 3's POP is observable, and it was only claimed

The `[CEReactions]` wrapper is: push a queue, run the member's steps, **pop**, invoke, rethrow. The
pop is step 3 and the invoke is step 4, in that order — so while the drain runs, no queue is
current, and a reaction enqueued BY the drain (§4.13.5 step 4 and step 5 are exactly that) goes to
the **backup element queue** and its microtask.

`custom_elements_reactions_invoke` carried a comment saying step 3 "already happened: the queue
stopped being current the moment the member's own steps returned". That is true of a member that
PARKS and false of one that does not: `js_idl_args_step` makes the queue current for its whole C
activation, and the epilogue runs inside it. So the enqueues went back onto the queue being drained.
The pop is now performed at the top of the invoke, which is also what lets a custom element
constructor — reached from step 8.3, and itself a declared member that pushes a queue — pass
`custom_elements_reactions_push`'s "no queue is current" assertion.

The element's OWN reaction queue still receives those reactions, and step 1.3's *repeat until
reactions is empty* is what runs them in the same drain; the backup queue's later microtask finds
the element's queue already empty. That is the spec's own arrangement, not a shortcut.

### 14.4 §4.13.2 `[HTMLConstructor]` — what the construction stack makes true

With step 6 of §4.13.5 pushing, §4.13.2's two arms are finally both reachable:

- **empty stack** (`new Router()`, and DOM "create an element" step 5.1.4.1's Construct inside
  `createElement`): steps 7.1-7.9 MAKE the element, and 7.7-7.8 set its state to "custom" and its
  definition. Both, in that order — the state is what DOM's step 5.1.4 assert reads back.
- **non-empty stack** (an upgrade): step 10 takes the last entry, step 11 throws a `TypeError` if it
  is the already-constructed marker, step 12 performs `element.[[SetPrototypeOf]](prototype)` and
  step 13 REPLACES the entry with the marker. This is how `super()` inside an author constructor
  assigns the node the page already holds to `this`, and it is why the prototype swap the old
  upgrade did by hand is deleted rather than kept beside it.

**A constructor that never calls `super()`** leaves the stack entry as the element rather than the
marker and returns abruptly (a derived constructor that does not call `super()` throws a
`ReferenceError` at return), so §4.13.5 step 8.4's SameValue is what reports it and step 9 pops the
entry either way.

### 14.5 What is honestly ABSENT, by name

- `adoptedCallback` — there is no adoption reaction; the callback is collected by §4.13.4 step 14
  and nothing enqueues it.
- **form-associated custom elements** — §4.13.5 step 10, and step 14.12's four-callback map.
- **customized built-ins** (`extends`) — §4.13.4 refuses them with a `NotSupportedError` rather than
  registering them as autonomous. That refusal is load-bearing twice over: it is what lets
  `ce_upgradable_name` answer the insertion-steps branch off the Lexbor local name alone (so
  inserting a `<div>` mints no wrapper), and it is what lets §4.13.2's autonomous/customized split be
  a `DCHECK` instead of a branch. Building customized built-ins widens all three together.
- **scoped registries** (`CustomElementRegistry` as a constructible interface, the active custom
  element constructor map, `Element.customElementRegistry`) — the map's save/restore in §4.13.5
  steps 7.5-7.6 and 8.9 has nothing to save while there is one registry per window.

### 14.6 What running the constructors EXPOSED, by name

Making §4.13.5 construct turns a class of "every subtest fails" files into "the page's own code throws",
because a WPT file's constructor calls the API the test is about. `testharness.js` installs
`window.onerror`, so HTML §8.1.4.6's report — working correctly — is what marks the file ERROR. Each of
these is an ABSENT capability naming itself, not a defect in §4.13:

- **`ElementInternals` / `HTMLElement.attachInternals()`** — 11 files. Every one of them writes
  `constructor(){ super(); this.internals_ = this.attachInternals(); }`, so the constructor throws
  `TypeError: not a function` on the first line the class runs. This is the single largest named gap
  in the directory and it is the whole of `form-associated/` plus the `ElementInternals-*` files.
- **HTML "create an element for the token" with `synchronousCustomElements` true** — the PARSER must
  CONSTRUCT a custom element it parses, not create-then-upgrade it. This engine upgrades, so a
  constructor that `new`s its own class before `super()` finds the upgrade's construction-stack entry
  and §4.13.2 step 11 throws (`parser-uses-constructed-element.html`), and one that never calls
  `super()` reaches `this` uninitialised (`parser-fallsback-to-unknown-element.html`). Both are
  correct behaviour for the algorithm the parser is actually running; the fix is in the parser.
- **`MutationObserver`** — `microtasks-and-constructors.html` uses it to observe the constructor's
  microtask ordering.
- **DOM "create an element" step 5.1.4's REPORT arm IS built** (this diff): a constructor that throws
  inside `document.createElement`, and the 5.1.4.3-8 checks after it, are reported and answered with a
  failed `HTMLUnknownElement` of the requested local name. The declaration that made it possible is
  `IdlStepDecl::catches_abrupt`, and `createElement` is the first member to carry it.
