/* solver/fire_target.c — what a candidate run's fire proves (see fire_target.h).
 *
 * One candidate flow serves both views: it runs the page with the candidate delivered at its sources and
 * performs the acts its path recorded, and the target says which observation settles it. The two kinds never
 * mix on one flow, and the fields each kind owns are asserted present at every reader. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "solver/fire_target.h"

/* The one shape check every accessor and transition runs: each kind holds exactly its own field. */
static void fire_target_check(const FireTarget *t)
{
    DCHECK(t != NULL, "a fire target was asked of nothing");
    DCHECK(t->kind == FIRE_NONE ? !t->sink && !t->key
           : t->kind == FIRE_SINK ? t->sink && *t->sink && !t->key
           : t->kind == FIRE_REQUEST && t->key && *t->key && !t->sink,
           "a fire target holds fields of a kind it is not — a sink target owns its class, a request its "
           "verification key, and an exploring flow neither");
}

void fire_target_sink_set(FireTarget *t, const char *sink)
{
    fire_target_check(t);
    DCHECK(t->kind == FIRE_NONE && sink && *sink, "a sink target was set over another target or with no class");
    t->kind = FIRE_SINK;
    t->sink = sink;
}

void fire_target_request(FireTarget *t, const char *key)
{
    fire_target_check(t);
    DCHECK(t->kind == FIRE_NONE && key && *key, "a request target was set over another target or with no key");
    t->key = strdup(key);
    CHECK(t->key, "fire target: OOM copying a verification key");
    t->kind = FIRE_REQUEST;
}

void fire_target_copy(FireTarget *dst, const FireTarget *src)
{
    fire_target_check(src);
    fire_target_check(dst);
    DCHECK(dst->kind == FIRE_NONE, "a fire target was copied over another target");
    if (src->kind == FIRE_SINK) fire_target_sink_set(dst, src->sink);
    else if (src->kind == FIRE_REQUEST) fire_target_request(dst, src->key);
}

void fire_target_clear(FireTarget *t)
{
    fire_target_check(t);
    free(t->key);
    t->kind = FIRE_NONE;
    t->sink = NULL;
    t->key = NULL;
}

const char *fire_target_sink(const FireTarget *t)
{
    fire_target_check(t);
    DCHECK(t->kind == FIRE_SINK, "a sink class was asked of a fire target that is not a sink's");
    return t->sink;
}

const char *fire_target_key(const FireTarget *t)
{
    fire_target_check(t);
    DCHECK(t->kind == FIRE_REQUEST, "a verification key was asked of a fire target that is not a request's");
    return t->key;
}
