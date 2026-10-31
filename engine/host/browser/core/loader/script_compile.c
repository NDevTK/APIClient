/* HTML §8.1.4.3 "Creating scripts"' compile, held across rests — see script_compile.h. */
#include <stdlib.h>

#include "check.h"
#include "core/loader/script_compile.h"

/* The parse and the UTF-8 it borrows for its whole length (JS_FlowCompileStep's contract). */
typedef struct {
    const char *src, *base;
    size_t      n;
    void       *carrier;
} ScriptCompileRec;

static void script_compile_rec_destroy(JSContext *ctx, void *p)
{
    ScriptCompileRec *r = p;

    JS_FlowCompileDrop(ctx, &r->carrier);
    JS_FreeCString(ctx, r->src);
    JS_FreeCString(ctx, r->base);
    free(r);
}

void script_compile_init(ScriptCompile *c)
{
    c->src = JS_UNDEFINED;
    c->base = JS_UNDEFINED;
    c->rec = NULL;
}

bool script_compile_pending(const ScriptCompile *c)
{
    return !JS_IsUndefined(c->src);
}

void script_compile_begin(JSContext *ctx, ScriptCompile *c, const char *src, size_t n, const char *base)
{
    DCHECK(!script_compile_pending(c) && c->rec == NULL, "a script compile began over one still in flight");
    DCHECK(src != NULL && base != NULL, "a script compile began with no source or no base URL");
    c->src = JS_NewStringLen(ctx, src, n);
    c->base = JS_NewString(ctx, base);
    CHECK(!JS_IsException(c->src) && !JS_IsException(c->base), "script compile: OOM holding a script's source");
}

int script_compile_run(JSContext *ctx, ScriptCompile *c, int eval_flags, JSValue *prog)
{
    ScriptCompileRec *r = c->rec;
    int cr;

    DCHECK(script_compile_pending(c), "a script compile was stepped with nothing begun");
    DCHECK(!(eval_flags & ~(JS_EVAL_FLAG_STRICT | JS_EVAL_FLAG_INLINE_SCRIPT | JS_EVAL_FLAG_FUNCTION_CTOR)),
           "a script compile was asked for flags JS_FlowCompileStep does not carry");
    if (!r) {
        r = malloc(sizeof *r);
        CHECK(r != NULL, "script compile: OOM allocating a compile record");
        r->src = JS_ToCStringLen(ctx, &r->n, c->src);
        r->base = JS_ToCString(ctx, c->base);
        r->carrier = NULL;
        CHECK(r->src != NULL && r->base != NULL, "script compile: OOM reading a script's source back");
        c->rec = r;
    }
    *prog = JS_UNDEFINED;
    cr = JS_FlowCompileStep(ctx, r->src, r->n, r->base, eval_flags, prog, &r->carrier);
    if (cr == 0)
        return JS_STEP_YIELD;
    script_compile_rec_destroy(ctx, r);
    c->rec = NULL;
    JS_FreeValue(ctx, c->src);
    JS_FreeValue(ctx, c->base);
    c->src = c->base = JS_UNDEFINED;
    if (cr < 0)
        *prog = JS_EXCEPTION;
    return 0;
}

void script_compile_visit(JSContext *ctx, ScriptCompile *c, JSStepVisit *v)
{
    v->val(ctx, &c->src);
    v->val(ctx, &c->base);
    v->cache(ctx, &c->rec, script_compile_rec_destroy);
}
