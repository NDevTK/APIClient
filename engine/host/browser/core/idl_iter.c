/* WEB IDL'S `sequence<T>` CONVERSION, as a cursor — see idl_iter.h.
 *
 * It was inside core/fetch/headers.c, which is where it was first needed and not where it belongs: nothing in
 * it is about a header, and the second consumer (URLSearchParams' `sequence<sequence<USVString>>` arm) would
 * otherwise have copied it. Web IDL states this protocol once; so does the engine. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_iter.h"

enum { IT_GET_ITERFN = 0, IT_CALL_ITERFN, IT_GET_NEXT, IT_CALL_NEXT, IT_GET_DONE, IT_GET_VALUE };

void iter_cursor_init(IterCursor *c)
{
    memset(c, 0, sizeof *c);
    c->iterfn = c->iter = c->next_fn = c->res = c->value = JS_UNDEFINED;
    c->cb[0] = c->cb[1] = JS_UNDEFINED;
}

void iter_cursor_visit(JSContext *ctx, IterCursor *c, JSStepVisit *v)
{
    int k;
    v->val(ctx, &c->iterfn); v->val(ctx, &c->iter); v->val(ctx, &c->next_fn);
    v->val(ctx, &c->res);    v->val(ctx, &c->value);
    for (k = 0; k < 2; k++) v->val(ctx, &c->cb[k]);
}

void iter_cursor_release(JSContext *ctx, IterCursor *c)
{
    int k;
    JS_FreeValue(ctx, c->iterfn); JS_FreeValue(ctx, c->iter); JS_FreeValue(ctx, c->next_fn);
    JS_FreeValue(ctx, c->res);    JS_FreeValue(ctx, c->value);
    c->iterfn = c->iter = c->next_fn = c->res = c->value = JS_UNDEFINED;
    for (k = 0; k < 2; k++) { JS_FreeValue(ctx, c->cb[k]); c->cb[k] = JS_UNDEFINED; }
}

int iter_cursor_run(JSContext *ctx, JSStepHdr *h, IterCursor *c, JSValueConst src,
                           JSValue in, JSValue **out_cb, int *out_argc)
{
    int r;

    if (c->phase == IT_GET_ITERFN) {
        r = step_getprop_run(ctx, h, src, JS_WellKnownSymbolAtom(JS_WKS_ITERATOR), in, &c->iterfn,
                             out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        in = JS_UNDEFINED;
        if (!JS_IsFunction(ctx, c->iterfn)) {
            JS_ThrowTypeError(ctx, "the value is not iterable");
            return -1;
        }
        c->phase = IT_CALL_ITERFN;
    }
    if (c->phase == IT_CALL_ITERFN) {
        r = step_call_run(ctx, &c->cphase, c->cb, c->iterfn, src, 0, NULL, in, &c->iter, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        in = JS_UNDEFINED;
        if (!JS_IsObject(c->iter)) {
            JS_ThrowTypeError(ctx, "the iterator is not an object");
            return -1;
        }
        c->phase = IT_GET_NEXT;
    }
    if (c->phase == IT_GET_NEXT) {
        JSAtom a = JS_NewAtom(ctx, "next");
        r = step_getprop_run(ctx, h, c->iter, a, in, &c->next_fn, out_cb, out_argc);
        JS_FreeAtom(ctx, a);
        if (r > 0) return r;
        if (r < 0) return -1;
        in = JS_UNDEFINED;
        c->phase = IT_CALL_NEXT;
    }
    if (c->phase == IT_CALL_NEXT) {
        JS_FreeValue(ctx, c->res);
        c->res = JS_UNDEFINED;
        r = step_call_run(ctx, &c->cphase, c->cb, c->next_fn, c->iter, 0, NULL, in, &c->res, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        in = JS_UNDEFINED;
        if (!JS_IsObject(c->res)) {
            JS_ThrowTypeError(ctx, "an iterator result is not an object");
            return -1;
        }
        c->phase = IT_GET_DONE;
    }
    if (c->phase == IT_GET_DONE) {
        JSValue d;
        JSAtom a = JS_NewAtom(ctx, "done");
        r = step_getprop_run(ctx, h, c->res, a, in, &d, out_cb, out_argc);
        JS_FreeAtom(ctx, a);
        if (r > 0) return r;
        if (r < 0) return -1;
        in = JS_UNDEFINED;
        c->done = JS_ToBool(ctx, d);
        JS_FreeValue(ctx, d);
        if (c->done) { c->phase = IT_CALL_NEXT; return 0; }
        c->phase = IT_GET_VALUE;
    }
    DCHECK(c->phase == IT_GET_VALUE, "the iterable cursor was re-entered at a phase it never parks in");
    {
        JSAtom a = JS_NewAtom(ctx, "value");
        JS_FreeValue(ctx, c->value);
        c->value = JS_UNDEFINED;
        r = step_getprop_run(ctx, h, c->res, a, in, &c->value, out_cb, out_argc);
        JS_FreeAtom(ctx, a);
        if (r > 0) return r;
        if (r < 0) return -1;
    }
    c->phase = IT_CALL_NEXT;   /* the next call resumes the loop, not the setup */
    return 0;
}



/* ---- §3.2.21's record<K, V> ------------------------------------------------------------------------------ */

enum { RC_KEYS_ASKED = 0, RC_DESC_ASKED, RC_VALUE_ASKED, RC_NEXT_KEY };

void record_cursor_init(RecordCursor *c)
{
    memset(c, 0, sizeof *c);
    c->keys = c->name = c->value = JS_UNDEFINED;
}

void record_cursor_visit(JSContext *ctx, RecordCursor *c, JSStepVisit *v)
{
    v->val(ctx, &c->keys); v->val(ctx, &c->name); v->val(ctx, &c->value);
}

void record_cursor_release(JSContext *ctx, RecordCursor *c)
{
    JS_FreeValue(ctx, c->keys); JS_FreeValue(ctx, c->name); JS_FreeValue(ctx, c->value);
    c->keys = c->name = c->value = JS_UNDEFINED;
}

int record_cursor_run(JSContext *ctx, JSStepHdr *h, RecordCursor *c, JSValueConst src, JSValue in,
                      int (*key_ok)(JSContext *ctx, JSValueConst key, void *user), void *user,
                      JSValue **out_cb, int *out_argc)
{
    int r;

    if (c->phase == RC_KEYS_ASKED) {
        JSValue lv;
        uint32_t n = 0;
        r = step_ownkeys_run(ctx, h, src, in, &c->keys, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        in = JS_UNDEFINED;
        lv = JS_GetPropertyStr(ctx, c->keys, "length");   /* the engine's own array; nothing of the page's */
        JS_ToUint32(ctx, &n, lv);
        JS_FreeValue(ctx, lv);
        c->n = (int)n;
        c->i = 0;
        c->phase = RC_NEXT_KEY;
    }

    for (;;) {
        if (c->phase == RC_NEXT_KEY) {
            if (c->i >= c->n) { JS_FreeValue(ctx, in); c->done = 1; return 0; }
            JS_FreeValue(ctx, c->name);
            c->name = JS_GetPropertyUint32(ctx, c->keys, (uint32_t)c->i);
            c->phase = RC_DESC_ASKED;
        }
        if (c->phase == RC_DESC_ASKED) {
            JSValue desc;
            JSAtom a = JS_ValueToAtom(ctx, c->name);
            int keep;
            if (a == JS_ATOM_NULL) { JS_FreeValue(ctx, in); return -1; }
            r = step_getownprop_run(ctx, h, src, a, in, &desc, out_cb, out_argc);
            JS_FreeAtom(ctx, a);
            if (r > 0) return r;
            if (r < 0) return -1;
            in = JS_UNDEFINED;
            /* step 5.2: present AND enumerable, or the key is not part of the record. The descriptor is the
               engine's own object, so reading it reaches nothing of the page's. */
            keep = JS_IsObject(desc);
            if (keep) {
                JSValue en = JS_GetPropertyStr(ctx, desc, "enumerable");
                keep = JS_ToBool(ctx, en);
                JS_FreeValue(ctx, en);
            }
            JS_FreeValue(ctx, desc);
            if (!keep) { c->i++; c->phase = RC_NEXT_KEY; continue; }
            if (key_ok && key_ok(ctx, c->name, user) < 0) return -1;
            c->phase = RC_VALUE_ASKED;
        }
        DCHECK(c->phase == RC_VALUE_ASKED, "the record cursor was re-entered at a phase it never parks in");
        {
            JSAtom a = JS_ValueToAtom(ctx, c->name);
            if (a == JS_ATOM_NULL) { JS_FreeValue(ctx, in); return -1; }
            JS_FreeValue(ctx, c->value);
            c->value = JS_UNDEFINED;
            r = step_getprop_run(ctx, h, src, a, in, &c->value, out_cb, out_argc);
            JS_FreeAtom(ctx, a);
            if (r > 0) return r;            /* parked ON THIS KEY; the resume comes back to it */
            if (r < 0) return -1;
        }
        c->i++;
        c->phase = RC_NEXT_KEY;
        return 0;
    }
}
