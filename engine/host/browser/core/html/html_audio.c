/* HTML §4.8.9 "The audio element" — Web IDL §3.7.2 "Legacy factory functions"'s `Audio`. See
 * core/html/html_audio.h for why this is its own component and what the two other legacy factory functions
 * HTML declares are. */
#include <stdbool.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_args.h"
#include "core/dom/element.h"
#include "core/dom/document.h"
#include "core/html/html_audio.h"
/* HTML §4.8.11 "Media elements"'s brand — what step 2 must have produced */
#include "core/html/media_element.h"

static int  g_id_factory = -1;
static bool g_ready;

/* HTML §4.8.9's own five steps, verbatim:
 *   1. "Let document be the current global object's associated Document."
 *   2. "Let audio be the result of creating an element given document, "audio", and the HTML namespace."
 *   3. "Set an attribute value for audio using "preload" and "auto"."
 *   4. "If src is given, then set an attribute value for audio using "src" and src. (This will cause the user
 *      agent to invoke the object's resource selection algorithm before returning.)"
 *   5. "Return audio."
 *
 * STEP 3 IS UNCONDITIONAL AND IT IS THE WHOLE REASON THIS IS NOT createElement. Every element this factory
 * makes carries `preload="auto"`, which HTML §4.8.11 "Media elements"'s attribute change steps read as the
 * strongest hint the element can carry — so `new Audio(u)` and a `document.createElement("audio")` given the
 * same `src` are two different requests, and collapsing the factory into the createElement path would
 * silently make them one.
 *
 * STEP 4 IS "IF GIVEN", AND `argc` DOES NOT ANSWER THAT. `Audio(src)` declares its argument OPTIONAL WITH NO
 * DEFAULT, so Web IDL §3.6 "Overload resolution algorithm" step 15.4.2 appends "the special value 'missing'"
 * for a page that passes `undefined` there, exactly as step 16.2 does for a page that passes nothing:
 * `new Audio()` and `new Audio(undefined)` are THE SAME CALL, and neither sets a `src` attribute.
 *
 * THE PARAGRAPH HERE BEFORE SAID THE OPPOSITE — that the two are different calls and that the second "sets it
 * to the string 'undefined'" — and it named `argc` as what tells them apart. That is what the code did, and
 * §3.6 says it is wrong: an `argc` test is a count of how far the page REACHED, while the standard is asking
 * which of the two kinds of entry §3.6 step 9 puts in `values` sits at that position. The count says "given"
 * for the one call the standard says is not, so `new Audio(undefined)` serialized as
 * `<audio preload="auto" src="undefined">` and HTML §4.8.11.5 "Loading the media resource"'s resource
 * selection algorithm went on to request a URL the page never named. A wrong stated contract outlives the
 * line it describes, which is why the argument is corrected here rather than deleted.
 *
 * WHAT ASKS IT NOW is core/idl_args.h's idl_arg_given, which is that rule as one named question. The same
 * distinction is HTML §4.8.3 "The img element"'s `Image(optional unsigned long width, optional unsigned long
 * height)` — two more positions declared optional with no default, and that component asks the same question.
 *
 * DOM §4.9 "Interface Element"'s CREATE AN ELEMENT is what step 2 names, not DOM §4.5 "Interface Document"'s
 * `createElement`, and the difference is that create-an-element runs no page code: there is no custom element
 * definition for `audio` to look up, and the internal creation is the half that has none. The two attribute
 * writes are the same
 * section's SET AN ATTRIBUTE VALUE, which goes through the mutation chokepoint — so each write is captured
 * into the running flow's COW delta and each runs §4.8.11's own attribute change steps, which is why the
 * `src` write here is one relevant mutation rather than a special case this file has to announce. */
static JSValue js_audio_factory(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue audio, preload;

    (void)magic;
    /* Web IDL §3.7.2 step 1 of the function's steps: "If NewTarget is undefined, then throw a TypeError."
       JS_CFUNC_step_ctor delivers NEW_TARGET in the receiver slot and undefined for a plain call, which is how
       `Audio()` is told apart from `new Audio()`. */
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "Failed to construct 'Audio': please use the 'new' operator");
    audio = document_create_element_internal(ctx, "audio", 5);         /* steps 1-2 */
    if (JS_IsException(audio)) return audio;
    DCHECK(media_element_is(lxb_dom_interface_node(element_of_value(audio))),
           "HTML 4.8.9's legacy factory function created something that is not an audio element — DOM 4.9's "
           "create an element was given the local name audio in the HTML namespace, so a node that is not one "
           "means the element-interface resolution and 4.8.11's brand disagree about what an audio element is");
    preload = JS_NewString(ctx, "auto");
    if (JS_IsException(preload)) { JS_FreeValue(ctx, audio); return preload; }
    element_attr_set_value(ctx, audio, "preload", preload);            /* step 3 */
    JS_FreeValue(ctx, preload);
    /* Step 4. THE VALUE CROSSES AS THE VALUE — `new Audio(cfg.streamUrl)` passes an address the page computed,
       and where that is unknown external input the attribute must keep its provenance rather than be stamped
       with whatever string a coercion invented. element_attr_set_value is the accessor that carries the whole
       triple into the (element, name) shadow, which is what keeps the address solvable once HTML §4.8.11.5
       "Loading the media resource"'s resource selection algorithm turns it into a request. */
    if (idl_arg_given(argc, argv, 0))
        element_attr_set_value(ctx, audio, "src", argv[0]);
    return audio;                                                      /* step 5 */
}

void html_audio_declare(JSContext *ctx)
{
    static const IdlArgType FACTORY_ARGS[1] = { IDL_DOMSTRING };

    DCHECK(!g_ready, "html_audio_declare ran twice — §4.8.9's factory argument list is declared once per AGENT, "
                     "and a second declaration would leave the member id every realm's `Audio` was minted with "
                     "naming a declaration nothing else refers to");
    g_id_factory = idl_method_id(ctx, FACTORY_ARGS, 1, js_audio_factory, 0);
    /* `Audio(optional DOMString src)` — the one argument is OPTIONAL, which is what makes the function
       object's `length` 0 (Web IDL §3.7.2: "Let length be the length of the shortest argument list of the
       entries in S"). NO idl_arg_default FOLLOWS, and that is the declaration matching the IDL rather than an
       omission: §4.8.9 writes `optional DOMString src` with no `= …`, so Web IDL §3.6 step 15.4.2's "missing"
       is the arm an omitted argument takes here, and idl_arg_given above is what reads it. Declaring a default
       would make every call give a `src` — the value the declaration invented — and step 4 would never be
       skipped. */
    idl_optional_from(0);
    g_ready = true;
}

void html_audio_install_global(JSContext *ctx, JSValueConst global, JSValueConst proto)
{
    JSValue factory;

    DCHECK(g_ready, "§4.8.9's legacy factory function was installed before its argument list was declared");
    DCHECK(JS_IsObject(proto),
           "§4.8.9's legacy factory function was installed with no HTMLAudioElement.prototype — Web IDL "
           "§3.7.2 gives F a non-configurable `prototype` naming the interface prototype object of THIS "
           "realm, and a factory whose `prototype` is not that object makes `new Audio() instanceof "
           "HTMLAudioElement` and `Audio.prototype === HTMLAudioElement.prototype` disagree");
    /* Web IDL §3.7.2: "Let F be CreateBuiltinFunction(steps, length, id, « », realm)", with length 0 and id
       `Audio`. It is a step-declared constructor so the `DOMString` argument is converted by the declaration —
       running the page's own `toString` — before the body is entered, and so a call without `new` arrives with
       an undefined receiver. */
    factory = idl_step_constructor(ctx, "Audio", g_id_factory);
    CHECK(!JS_IsException(factory), "§4.8.9: the `Audio` legacy factory function could not be allocated");
    /* "Perform ! DefinePropertyOrThrow(F, "prototype", PropertyDescriptor{[[Value]]: proto, [[Writable]]:
       false, [[Enumerable]]: false, [[Configurable]]: false})." NOT JS_SetConstructor, which would also write
       `HTMLAudioElement.prototype.constructor = Audio` — a legacy factory function is not the interface's
       constructor, and `Audio.prototype.constructor` must stay `HTMLAudioElement`. Flags 0 is exactly
       {[[Writable]]: false, [[Enumerable]]: false, [[Configurable]]: false}. */
    JS_DefinePropertyValueStr(ctx, factory, "prototype", JS_DupValue(ctx, proto), 0);
    idl_define_global_property_reference(ctx, global, "Audio", factory);
}

void html_audio_free(JSRuntime *rt)
{
    (void)rt;
    if (!g_ready) return;
    /* The member id belongs to the argument pool core/idl_args.c releases with the agent; what this component
       owns is the statement that the declaration HAPPENED, so that is what is given back. */
    g_ready = false;
    g_id_factory = -1;
}
