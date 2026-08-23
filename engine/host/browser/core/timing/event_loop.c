/* THE EVENT LOOP'S OWN STATE — HTML §8.1.7. See event_loop.h for why it is a heap record and not three
   statics, and why it is per-agent while §8.7 Timers's map of active timers is per-global. */
#include "check.h"
#include "quickjs.h"
#include "core/timing/event_loop.h"

/* THE RECORD, held for the AGENT. A module static is the CORRECT scope here and the wrong one for a prototype:
   §8.1.7 gives one event loop to a similar-origin window agent, so one record answering every realm IS the
   fact, where one prototype answering every realm is a defect. What must not be shared is the TIMELINE, and
   that is answered by the object's properties riding the per-flow COW delta rather than by a second object. */
static JSValue g_rec = JS_UNDEFINED;
static JSAtom  g_atom_now = JS_ATOM_NULL, g_atom_render = JS_ATOM_NULL, g_atom_seq = JS_ATOM_NULL;

static double el_get(JSContext *ctx, JSAtom key)
{
    JSValue v;
    double d = 0;

    DCHECK(JS_IsObject(g_rec),
           "the event loop's state was read before event_loop_init built it — the record has to be in the "
           "PRE-BOOT baseline, or the first flow to touch it creates it inside its own delta and every "
           "sibling then reads a clock that is not there");
    v = JS_GetProperty(ctx, g_rec, key);
    DCHECK(JS_IsNumber(v), "the event loop's state holds a value that is not a number — every field of it is "
                           "a moment or a counter, and every writer here writes one");
    JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    return d;
}

static void el_set(JSContext *ctx, JSAtom key, double d)
{
    DCHECK(JS_IsObject(g_rec), "the event loop's state was written before event_loop_init built it");
    JS_SetProperty(ctx, g_rec, key, JS_NewFloat64(ctx, d));
}

void event_loop_init(JSContext *ctx)
{
    DCHECK(JS_IsUndefined(g_rec), "event_loop_init ran twice — §8.1.7 gives one event loop to an agent");
    g_atom_now = JS_NewAtom(ctx, "eventLoopNow");
    g_atom_render = JS_NewAtom(ctx, "lastRenderOpportunityTime");
    g_atom_seq = JS_NewAtom(ctx, "taskInsertionOrder");
    CHECK(g_atom_now != JS_ATOM_NULL && g_atom_render != JS_ATOM_NULL && g_atom_seq != JS_ATOM_NULL,
          "event loop: the record's own keys could not be interned");
    /* NULL-PROTOTYPED, like §8.12 Animation frames's map: this is the loop's own storage and never the page's object, so it
       carries no members a page could have replaced. */
    g_rec = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(g_rec), "event loop: the loop's own state could not be allocated");
    JS_SetProperty(ctx, g_rec, g_atom_now, JS_NewFloat64(ctx, 0));
    JS_SetProperty(ctx, g_rec, g_atom_render, JS_NewFloat64(ctx, 0));
    JS_SetProperty(ctx, g_rec, g_atom_seq, JS_NewFloat64(ctx, 0));
}

void event_loop_free(JSContext *ctx)
{
    if (JS_IsUndefined(g_rec))
        return;
    JS_FreeValue(ctx, g_rec);
    g_rec = JS_UNDEFINED;
    JS_FreeAtom(ctx, g_atom_now);
    JS_FreeAtom(ctx, g_atom_render);
    JS_FreeAtom(ctx, g_atom_seq);
    g_atom_now = g_atom_render = g_atom_seq = JS_ATOM_NULL;
}

double event_loop_now(JSContext *ctx) { return el_get(ctx, g_atom_now); }

void event_loop_advance_to(JSContext *ctx, double when, double due)
{
    double now = el_get(ctx, g_atom_now);

    DCHECK(when >= now, "the event loop's clock was asked to move BACKWARDS — a moment already passed is a "
                        "task that should have run before whatever is asking, and on a PER-FLOW clock it is "
                        "also how a flow reading another timeline's clock announces itself");
    DCHECK(due < 0 || when <= due,
           "the event loop's clock was moved PAST a task source that is already due — a source that steps over "
           "an earlier one runs the page's work in an order the page did not write, which is exactly what "
           "virtual time makes easy to get wrong (a long timeout landing in the middle of the work it was set "
           "to outlast)");
    el_set(ctx, g_atom_now, when);
}

double event_loop_last_render(JSContext *ctx) { return el_get(ctx, g_atom_render); }

void event_loop_set_last_render(JSContext *ctx, double when)
{
    DCHECK(when <= el_get(ctx, g_atom_now),
           "§8.1.7.1's last render opportunity time was set to a moment the clock has not reached — the "
           "rendering task source becomes due AT a moment on this clock, so the clock moves there first");
    el_set(ctx, g_atom_render, when);
}

double event_loop_task_seq_peek(JSContext *ctx) { return el_get(ctx, g_atom_seq); }

double event_loop_task_seq(JSContext *ctx)
{
    double seq = el_get(ctx, g_atom_seq);

    el_set(ctx, g_atom_seq, seq + 1);
    return seq;
}
