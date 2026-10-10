/* lib/dom-builder.js — the popup's DOM builder. Elements come from createElement, text from createTextNode,
   attributes from setAttribute over an allowlist; no markup string is ever parsed, so a value a page or a
   server chose can only ever become a text node or an attribute value.
   The allowlists are about this codebase's own calls: a tag or attribute name outside them is a caller of
   ours asking for an execution sink, so it is a DCHECK. Attribute VALUES may be stranger text and are not
   asserted on; that is safe because no allowed attribute is a URL, a style or an event handler. */
(function (g) {
  const TAGS = new Set([
    "b", "br", "button", "caption", "code", "datalist", "details", "div", "em", "h2", "h3", "h4", "hr", "i",
    "input", "label", "li", "ol", "optgroup", "option", "p", "pre", "section", "select", "small", "span",
    "strong", "summary", "table", "tbody", "td", "textarea", "tfoot", "th", "thead", "tr", "ul",
  ]);
  /* No URL-valued attribute (href, src, action, formaction, srcdoc) is listed, which is how a `javascript:`
     URL is refused: there is no attribute it could be written into. A caller that needs a link builds a URL
     arm here that parses the value and admits only http: and https:. */
  const ATTRS = new Set([
    "class", "colspan", "disabled", "for", "id", "list", "max", "min", "name", "open", "placeholder",
    "role", "rows", "rowspan", "scope", "step", "title", "type", "value",
  ]);

  function attrAllowed(name) {
    return ATTRS.has(name) || /^data-[a-z0-9-]+$/.test(name) || /^aria-[a-z]+$/.test(name);
  }

  // Appends one child: a Node as itself, a string as a text node. Anything else is a caller's bug.
  function appendChild(parent, child) {
    if (typeof child === "string") { parent.appendChild(document.createTextNode(child)); return; }
    DCHECK(child instanceof Node,
           "domElement was handed a child that is neither a Node nor a string (" + typeof child + ") — " +
           "stringify a number at the call site, and pass no entry rather than null for an absent child");
    parent.appendChild(child);
  }

  /* domElement(tag, attrs, children?) — `attrs` maps an allowed attribute name to a string; `children` is an
     array of Nodes and strings, appended in order. */
  g.domElement = function domElement(tag, attrs, children) {
    DCHECK(TAGS.has(tag), "domElement was asked for <" + tag + ">, which is not in lib/dom-builder.js's tag " +
                          "allowlist — add it there only if it can neither run script nor load a resource");
    DCHECK(!!attrs && typeof attrs === "object" && !Array.isArray(attrs),
           "domElement's second argument is not an attribute record — pass {} for an element with none");
    const node = document.createElement(tag);
    for (const name of Object.keys(attrs)) {
      DCHECK(!/^on/i.test(name), "domElement was asked to set the event-handler attribute `" + name +
                                  "` — attach behaviour with addEventListener on the returned node");
      DCHECK(attrAllowed(name), "domElement was asked to set `" + name + "`, which is not in lib/dom-" +
                                "builder.js's attribute allowlist; a URL, style or handler attribute is " +
                                "refused there by design");
      DCHECK(typeof attrs[name] === "string",
             "domElement's attribute `" + name + "` is not a string (" + typeof attrs[name] + ") — " +
             "stringify at the call site, and omit the key for an attribute the element does not carry");
      node.setAttribute(name, attrs[name]);
    }
    if (children !== undefined) {
      DCHECK(Array.isArray(children), "domElement's children are not an array");
      for (const c of children) appendChild(node, c);
    }
    return node;
  };

  // Replaces every child of `parent` with `children`, through the same append as domElement.
  g.domReplaceChildren = function domReplaceChildren(parent, children) {
    DCHECK(parent instanceof Element, "domReplaceChildren was handed a parent that is not an Element");
    DCHECK(Array.isArray(children), "domReplaceChildren's children are not an array");
    parent.replaceChildren();
    for (const c of children) appendChild(parent, c);
  };
})(self);
