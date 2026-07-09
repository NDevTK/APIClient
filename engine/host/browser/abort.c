/* AbortSignal — see abort.h. Built from its Web IDL via the idl.h driver, not hand-assembled:
 *
 *   interface AbortSignal : EventTarget {
 *     readonly attribute boolean aborted;      // unknown headless -> forks (never a fixed `false` that
 *     readonly attribute any     reason;       //   silently takes only the not-aborted arm)
 *     undefined throwIfAborted();
 *     // EventTarget: addEventListener / removeEventListener
 *   };
 *
 * The static factories AbortSignal.timeout/any/abort all yield an instance of this interface. */
#include "abort.h"
#include "idl.h"
#include "opaque.h"   /* js_noop */

extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);   /* EventTarget.addEventListener -> driven flow */

static const IDLMember ABORTSIGNAL_IDL[] = {
    { "aborted",             IDL_ATTR_OPAQUE, NULL,            0 },
    { "reason",              IDL_ATTR_OPAQUE, NULL,            0 },
    { "throwIfAborted",      IDL_METHOD,      js_noop,         0 },
    { "addEventListener",    IDL_METHOD,      js_add_listener, 2 },
    { "removeEventListener", IDL_METHOD,      js_noop,         2 },
};

JSValue js_abortsignal_make(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    (void)t; (void)c; (void)v;
    return idl_instance(ctx, ABORTSIGNAL_IDL, sizeof ABORTSIGNAL_IDL / sizeof ABORTSIGNAL_IDL[0]);
}
