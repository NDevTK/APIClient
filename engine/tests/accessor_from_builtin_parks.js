/*---
description: >
  A page accessor reached through a C builtin is driven on the trampoline, not called from C.

  A getter or setter is the page's code and can contain a loop. If a builtin invokes one by C-recursing into
  JS_CallInternal, that body cannot suspend: the scheduler has no way to park it, so it runs to completion and
  holds the whole frontier. quickjs's own JS_GetPropertyInternal / JS_SetPropertyInternal still have such a
  call — it is the correct primitive for an engine-internal read — and any bytecode body entered that way with
  a live flow now CRASHES at its origin rather than being counted.

  This file is the standing evidence that the builtins which read page-visible properties request them instead:
  each one below invokes an accessor and must produce the right answer without tripping that crash. It exists
  because the reachability was established by measurement — reading the call sites suggested these paths ran
  the accessor from C, and running them showed the requests were already in place.
---*/
var log = [];
function withGetter(name, value) {
    var o = {};
    Object.defineProperty(o, name, {
        get: function () { log.push(name); return value; },
        enumerable: true, configurable: true,
    });
    return o;
}

assert.sameValue(JSON.stringify(withGetter("a", 1)), '{"a":1}', "JSON.stringify reads through a getter");
assert.sameValue(Object.keys(withGetter("k", 1)).join(""), "k", "Object.keys does not need to read it");
assert.sameValue(JSON.stringify(Object.assign({}, withGetter("z", 9))), '{"z":9}', "Object.assign reads it");
assert.sameValue(JSON.stringify(Object.entries(withGetter("e", 5))), '[["e",5]]', "Object.entries reads it");
assert.sameValue(JSON.stringify({ ...withGetter("s", 7) }), '{"s":7}', "object spread reads it");
assert.sameValue(String(withGetter("toString", function () { return "TS"; })), "TS",
                 "ToPrimitive reads the method through a getter and then calls it");
assert.sameValue(Array.from(withGetter("length", 0)).length, 0, "Array.from reads length through a getter");

/* A SETTER on the receiving side of a copy is the same question with the page's code on the write. */
var sink = {};
Object.defineProperty(sink, "w", {
    set: function (v) { log.push("set:" + v); },
    enumerable: true, configurable: true,
});
Object.assign(sink, { w: 3 });

assert.sameValue(log.join(","), "a,z,e,s,toString,length,set:3",
                 "every accessor really ran, in order — the answers above are not defaults: " + log.join(","));
