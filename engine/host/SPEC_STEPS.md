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
