/* HTML §4.2.4 "The link element"'s processing model, and §4.6.8.20 Link type "preload" — see html_link.h for
 * what this component exists for, which link types it processes, and what is honestly absent. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "solver/endpoint.h"      /* every request host-edge funnels one endpoint into the @H surface */
#include "core/idl_slots.h"
#include "core/url/url.h"
#include "core/dom/node.h"
#include "core/dom/element.h"
#include "core/dom/document.h"
#include "core/dom/shadow_root.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/fetch/fetch.h"
#include "core/fetch/port_blocking.h"
#include "core/frame/policy_container.h"
#include "core/css/media_query.h"   /* §4.2.4.1's "matches the environment", which is HTML §2.3.10's predicate */
#include "solver/dom_cow.h"      /* the taint an `href` composed out of unknown input carries */
#include "solver/flow.h"         /* §4.2.4.3 ends in a FETCH, and a fetch parks on a flow — see link_trigger */
#include "core/mime/mime_type.h"    /* §4.6.8.20's "a string type matches a preload destination" */
#include "core/html/html_link.h"

static int      g_ready;
static JSValue  g_state_key;
static JSAtom   g_atom_state = JS_ATOM_NULL;
static int      g_task_stepid = -1;

/* ---- the element, and its `rel` ---------------------------------------------------------------------------- */

static bool link_is_node(const lxb_dom_node_t *n)
{
    return n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT && lxb_html_tree_node_is(n, LXB_TAG_LINK);
}

static const char *link_attr(lxb_dom_element_t *el, const char *name, size_t *out_n)
{
    size_t n = 0;
    const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), &n);

    if (out_n) *out_n = v ? n : 0;
    return (const char *)v;
}

/* §4.6.8 "Link types": "the keywords are ASCII case-insensitive" and the attribute is "a set of space-separated
   tokens", so membership is a walk of the value split on ASCII whitespace with an ASCII-case-insensitive
   compare. Written here rather than through core/dom/dom_token_list.h because that component's `relList` is a
   LIVE OBJECT the page owns and this is a question C asks of the attribute, and because a token list would have
   to be minted for every `<link>` a parse produced in order to answer it once. */
static bool rel_has(const char *rel, size_t rel_n, const char *kw)
{
    size_t kw_n = strlen(kw), i = 0;

    if (!rel) return false;
    while (i < rel_n) {
        size_t start;
        while (i < rel_n && (rel[i] == ' ' || rel[i] == '\t' || rel[i] == '\n' ||
                             rel[i] == '\f' || rel[i] == '\r')) i++;
        start = i;
        while (i < rel_n && !(rel[i] == ' ' || rel[i] == '\t' || rel[i] == '\n' ||
                              rel[i] == '\f' || rel[i] == '\r')) i++;
        if (i - start == kw_n) {
            size_t k;
            for (k = 0; k < kw_n; k++) {
                char c = rel[start + k];
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                if (c != kw[k]) break;
            }
            if (k == kw_n) return true;
        }
    }
    return false;
}

/* WHICH OF §4.2.4's STATES THIS ELEMENT IS IN. Four answers and not two, because "there is no external resource
   link here" and "there is one whose steps this engine has not built" are different facts about the document
   and a reader that cannot tell them apart cannot tell an ordinary page from an unreachable capability. See
   html_link.h for why the last two are not fallbacks and not debt against the first. */
typedef enum {
    LINK_EXT_NONE = 0,        /* no keyword in `rel` creates an external resource link */
    LINK_EXT_PRELOAD,         /* §4.6.8.20 — the steps this component runs */
    LINK_EXT_MODULEPRELOAD,   /* §4.6.8.12 — a module GRAPH, core/loader/module_loader.c's algorithm */
    LINK_EXT_UNBUILT,         /* an external resource link whose own steps this engine has no component for */
} LinkExternalType;

/* §4.6.8's external resource link types, as the set this classifier must be able to NAME — the ones that "create
   an external resource link" in their own words. `alternate` is deliberately absent: §4.6.8.1 makes it an
   external resource link ONLY when it also carries `stylesheet`, which the `stylesheet` row already answers. */
static const char *const LINK_EXT_KEYWORDS[] = {
    "stylesheet", "icon", "manifest", "prefetch", "preconnect", "dns-prefetch", "expect",
};

static LinkExternalType link_external_type_of(lxb_dom_element_t *el)
{
    size_t rel_n = 0;
    const char *rel = link_attr(el, "rel", &rel_n);
    size_t i;

    if (!rel) return LINK_EXT_NONE;
    if (rel_has(rel, rel_n, "preload")) return LINK_EXT_PRELOAD;
    if (rel_has(rel, rel_n, "modulepreload")) return LINK_EXT_MODULEPRELOAD;
    for (i = 0; i < sizeof(LINK_EXT_KEYWORDS) / sizeof(LINK_EXT_KEYWORDS[0]); i++)
        if (rel_has(rel, rel_n, LINK_EXT_KEYWORDS[i])) return LINK_EXT_UNBUILT;
    return LINK_EXT_NONE;
}

/* ---- the element's state ---------------------------------------------------------------------------------- */

/* ONE BOOLEAN, AND THE DERIVATION IS WHY IT IS ONE RATHER THAN THREE. §4.6.8.20's last two appropriate times
   are conditional on the PAST — a `type` or `media` change re-triggers only for a link "that was previously not
   obtained DUE TO the type attribute specifying an unsupported type" / "DUE TO the media attribute not matching
   the environment" — so the prose distinguishes the REASON a fetch did not happen. It does not need to be
   stored, because every reason that is not the named one re-declines on its own at the re-run: a link not
   obtained because `as` was absent still translates to a null destination and returns, and one not obtained
   because its media still does not match still fails §4.2.4.1. So triggering on any `type`/`media` change of a
   NOT-YET-OBTAINED link fetches in exactly the cases the three-reason version does, and the single flag is the
   whole state. The one place they could diverge — `as` set, then `type` changed — is covered because the `as`
   change is itself an appropriate time and obtains the link before the `type` change is asked.
   IT IS AN ORDINARY PROPERTY ON A SLOTS OBJECT, so the write is captured by the running flow's COW delta and
   two arms that each reach this element hold their own answer — core/html/html_image.c's rule for a record a
   flow may write, and this is its only creation site, so there is no write site left to miss. */
static JSValue link_state(JSContext *ctx, JSValueConst el)
{
    JSValue st;

    DCHECK(g_ready, "a link element's state was reached before §4.2.4 was declared");
    if (JS_GetOwnSlot(ctx, &st, el, g_atom_state) > 0) return st;

    st = idl_slots_new(ctx);
    CHECK(!JS_IsException(st), "§4.2.4: OOM building a link element's processing state");
    JS_SetPropertyStr(ctx, st, "obtained", JS_FALSE);
    JS_DefinePropertyValue(ctx, (JSValue)el, g_atom_state, JS_DupValue(ctx, st),
                           JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    return st;
}

static bool link_obtained(JSContext *ctx, JSValueConst el)
{
    JSValue st = link_state(ctx, el);
    JSValue v = JS_GetPropertyStr(ctx, st, "obtained");
    bool b;

    DCHECK(JS_IsBool(v), "§4.2.4's link state answered `obtained` with something that is not a boolean — the "
                         "field is written by this file and by nothing else");
    b = JS_ToBool(ctx, v) != 0;
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, st);
    return b;
}

static void link_set_obtained(JSContext *ctx, JSValueConst el)
{
    JSValue st = link_state(ctx, el);

    JS_SetPropertyStr(ctx, st, "obtained", JS_TRUE);
    JS_FreeValue(ctx, st);
}

/* ---- the queued element task: §4.6.8.20's `load` and `error` ---------------------------------------------- */

/* IT IS A TASK FOR THE REASON core/html/html_image.c's IS. The fire runs the PAGE's listener list — the whole
   point of this component is that a `link.onload` handler creates a `<script>` — so it must have a flow base
   under it and it must sit at HTML §8.1.7's task position rather than inside the reply's delivery, where a
   page's `head.appendChild(link)` would see its own `onload` run before the next statement. */
#define LINK_TASK_STAGES(X) \
    X(LINKT_FIRE, "HTML §4.6.8.20 Link type \"preload\" — the queued task's fire of `load` or `error`")
enum { LINK_TASK_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const link_task_steps[] = { LINK_TASK_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr   hdr;       /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t     fphase;    /* the dispatch's own phase, held across a suspension */
    uint8_t     started;
    JSValue     ev;        /* the Event being dispatched (owned) */
    EventFireCb cb;        /* §2.9's dispatch request buffer, whose width travels with its type */
} LinkTask;

static void link_task_visit(JSContext *ctx, void *stp, JSStepVisit *v)
{
    LinkTask *s = stp;
    int k;

    /* NOTHING IS OWNED UNTIL THE FIRST STEP RUNS — the state's block is ZEROED (quickjs-step.h) and a zeroed
       JSValue is not JS_UNDEFINED, so a visit that reached a machine the driver allocated and has not entered
       would hand the collector a value nobody wrote. */
    if (!s->started) return;
    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
}

static int link_task_step(JSContext *ctx, void *stp, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    LinkTask *s = stp;
    JSValueConst element = step_arg(&s->hdr, 0);
    JSValueConst name = step_arg(&s->hdr, 1);
    int r;

    STEP_DISPATCH(LINK_TASK_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(LINKT_FIRE);
        if (!s->started) {
            const char *type;
            int k;

            /* EVERY OWNED FIELD IS PLACED BEFORE THE FIRST THING THAT CAN FAIL — `JS_ToCString` allocates, and
               allocation is a moment the collector can walk this machine through `link_task_visit`. */
            s->started = 1;
            s->fphase = 0;
            s->ev = JS_UNDEFINED;
            STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
            type = JS_ToCString(ctx, name);
            CHECK(type != NULL, "§4.6.8.20: OOM reading a queued link task's event name");
            /* §4.6.8.20 names each fire as "fire an event named load/error at el" and gives no initialiser, so
               neither bubbles and neither is cancelable. */
            s->ev = event_new(ctx, type, /*bubbles*/ false, /*cancelable*/ false);
            JS_FreeCString(ctx, type);
            if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        }
        r = event_target_fire_run(ctx, &s->fphase, STEP_CB(s->cb), element, s->ev, JS_UNDEFINED, cb_result,
                                  NULL, out_cb, out_argc);
        if (r > 0) return r;   /* parked INSIDE the dispatch: a listener is the page's code */
        JS_FreeValue(ctx, s->ev);
        s->ev = JS_UNDEFINED;
        if (r < 0) return JS_STEP_ABRUPT;
        return JS_STEP_DONE;
}

static const JSTrampStepDef link_task_def = {
    sizeof(LinkTask), link_task_step, NULL, 0,
    .visit = link_task_visit,
    .algorithm = "HTML §4.6.8.20 Link type \"preload\" — the task that fires the preload's `load` or `error`",
    .steps = link_task_steps
};

static void link_queue_fire(JSContext *ctx, JSValueConst el, const char *name)
{
    JSValueConst argv[2];
    JSValue fn, nm;

    DCHECK(g_task_stepid >= 0, "a link element task was queued before §4.2.4 registered its machine");
    /* THE CALLEE IS MINTED IN THE ENQUEUING REALM: a C function runs in the realm that DEFINED it (§3.7), and
       this one fires an event at an element of THIS document. */
    fn = JS_NewCFunction2(ctx, NULL, "linkElementTask", 2, JS_CFUNC_step, g_task_stepid);
    CHECK(!JS_IsException(fn), "§4.6.8.20: the queued link task's callee could not be allocated");
    nm = JS_NewString(ctx, name);
    CHECK(!JS_IsException(nm), "§4.6.8.20: OOM allocating a queued link task's event name");
    argv[0] = el;
    argv[1] = nm;
    JS_EnqueueCallTask(ctx, fn, 2, argv);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, nm);
}

/* ---- §4.6.8.20's processResponse -------------------------------------------------------------------------- */

/* "Preload options, with the following steps given a response response: If response is a network error, fire an
   event named error at el. Otherwise, fire an event named load at el."
   A NETWORK ERROR IS THE JSON `null`, which is the same positive answer core/html/html_image.c's delivery
   asserts — and the ONLY thing this arm asks of a reply that is not one, deliberately: §4.6.8.20 branches on
   network-error-or-not and on nothing else, so a 404 is a RESPONSE and fires `load`. That is the standard's own
   answer and it differs from what browsers ship — the note beside the steps says so ("The actual browsers'
   behavior is different from the spec here") — and this engine follows the specification, which is the rule
   §Browser-half states for every such divergence. */
static JSValue link_deliver(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                            int magic, JSValueConst *func_data)
{
    JSValueConst el = func_data[0];

    (void)this_val; (void)magic;
    DCHECK(argc >= 1, "a preload reply was delivered with no response — the host calls this with the reply "
                      "record or with null for a network error, and never with nothing");
    DCHECK(argc < 1 || JS_IsObject(argv[0]) || JS_IsNull(argv[0]),
           "a preload reply arrived as something other than the host's reply record — every host builds one "
           "with fetch_reply_new or parses the trusted zone's JSON into one, and a bare string here is a host "
           "still delivering only bytes");
    link_queue_fire(ctx, el, (argc >= 1 && JS_IsNull(argv[0])) ? "error" : "load");
    return JS_UNDEFINED;
}

/* ---- §4.6.8.20's destination and type checks -------------------------------------------------------------- */

/* "A preload destination is `fetch`, `font`, `image`, `script`, `style`, or `track`." "To translate a preload
   destination given a string destination: If destination is not a preload destination, then return null.
   Return the result of translating destination."
   THE STRING IS BOTH ANSWERS AT ONCE and that is Fetch's doing, not a shortcut: the six preload destinations
   translate to Fetch §2.2.5 destinations spelled identically, and this engine's CSP entry takes a destination
   as that same string (core/frame/policy_container.h: "`destination` IS FETCH §2.2.5's DESTINATION STRING").
   NULL is "the `as` attribute names something that is not a preload destination" — including its ABSENCE, which
   is the `as` attribute in its missing-value-less state, and which is why `<link rel=preload href=x>` with no
   `as` fetches nothing in a browser either. */
static const char *translate_preload_destination(lxb_dom_element_t *el)
{
    static const char *const DESTINATIONS[] = { "fetch", "font", "image", "script", "style", "track" };
    size_t as_n = 0;
    const char *as = link_attr(el, "as", &as_n);
    size_t i;

    if (!as) return NULL;
    for (i = 0; i < sizeof(DESTINATIONS) / sizeof(DESTINATIONS[0]); i++) {
        size_t n = strlen(DESTINATIONS[i]);
        size_t k;
        if (n != as_n) continue;
        /* §2.3's enumerated attributes are ASCII case-insensitive, which is what makes `as="SCRIPT"` the
           script state and not an invalid value. */
        for (k = 0; k < n; k++) {
            char c = as[k];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != DESTINATIONS[i][k]) break;
        }
        if (k == n) return DESTINATIONS[i];
    }
    return NULL;
}

/* §4.6.8.20: "a string type matches a preload destination destination if the following algorithm returns true".
   STEP 4 — "If mimeTypeRecord is not supported by the user agent, then return false" — IS NOT A SEPARATE ARM
   HERE, and that is a derivation rather than an omission: step 5 lists the five (destination, type-group) pairs
   this agent can serve and step 6 returns false for everything else, so a type this agent does not support
   either fails step 5 and reaches step 6, or matches step 5 and is by construction supported. Writing step 4
   would be a second statement of which types are supported, free to disagree with the first. */
static bool type_matches_preload_destination(lxb_dom_element_t *el, const char *destination)
{
    size_t type_n = 0;
    const char *type = link_attr(el, "type", &type_n);
    MimeType m;
    bool ok = false;

    if (!type || type_n == 0) return true;                          /* step 1: the empty string matches */
    if (!strcmp(destination, "fetch")) return true;                 /* step 2 */
    /* Step 3: "Let mimeTypeRecord be the result of parsing type. If mimeTypeRecord is failure, return false."
       §4.4's PARSE and not §4.5's extract: the value is an attribute the page wrote, not a header field. */
    if (!mime_type_parse(&m, type, type_n)) { mime_type_free(&m); return false; }

    /* Step 5's five pairs, in the standard's own order. Each group question is core/mime/mime_type.h's, which
       is MIME Sniffing §4.6 "MIME type groups" — one implementation, so a `type` a preload accepts and a body
       this engine later classifies cannot disagree about what an image is. The two ESSENCE comparisons are the
       two the standard states as essences rather than as groups, and the essence is read only on those arms:
       it is a fresh allocation, and computing it for a `script` destination that never looks at it is a string
       built to be thrown away. */
    if (!strcmp(destination, "script"))     ok = mime_type_is_javascript(&m);
    else if (!strcmp(destination, "image")) ok = mime_type_is_image(&m);
    else if (!strcmp(destination, "font"))  ok = mime_type_is_font(&m);
    else if (!strcmp(destination, "style") || !strcmp(destination, "track")) {
        /* THE EXPECTED ESSENCE IS NAMED BY THE DESTINATION IT BELONGS TO, not derived from a letter of it: a
           `destination[0] == 's'` test reads correctly for exactly the two arms that exist and silently gives
           `text/css` to the next destination whose name begins with an s. */
        const char *want = !strcmp(destination, "style") ? "text/css" : "text/vtt";
        char *essence = mime_type_essence(&m);
        CHECK(essence, "§4.6.8.20: OOM reading a link `type` attribute's essence");
        ok = !strcmp(essence, want);
        free(essence);
    }
    mime_type_free(&m);
    return ok;                                                      /* step 6 */
}

/* §4.2.4.1 "Processing the media attribute": "if the link is an external resource link, then the media
   attribute is prescriptive. The user agent must apply the external resource when the media attribute's value
   MATCHES THE ENVIRONMENT … The default, if the media attribute is omitted, is `all`."
   ONE PREDICATE, THE SAME ONE. HTML §2.3.10 "Media queries" defines "matches the environment", and
   core/css/media_query.h is where it lives — the identical question `<source media>` under a `<picture>` asks
   (core/html/image_source_set.c) and `<source media>` under a media element asks (core/html/media_element.c),
   answered by `media_query_parse` + `media_query_matches_now`. A second spelling here could resolve one query
   differently from the way the page's own `matchMedia` and its cascade resolved it, which is one fact answered
   from two places. The `_now` read is the engine-side policy stated in that header: C cannot fork, so it takes
   the arm this flow has committed to and otherwise the modelled environment, while a PAGE's read of the same
   query stays concolic and still explores both worlds. */
static bool link_media_matches(JSContext *ctx, lxb_dom_element_t *el)
{
    size_t media_n = 0;
    const char *media = link_attr(el, "media", &media_n);
    MediaQuerySet *set;
    char *text;
    bool m;

    if (!media) return true;   /* "The default, if the media attribute is omitted, is `all`" */
    text = malloc(media_n + 1);
    CHECK(text, "§4.2.4.1: OOM copying a link element's `media` attribute");
    memcpy(text, media, media_n);
    text[media_n] = 0;
    set = media_query_parse(text);
    free(text);
    DCHECK(set != NULL,
           "media_query_parse answered NULL for a link element's `media` attribute. MQ4 §3.1 makes an empty "
           "list match everything and an unparseable query `not all`, so every string this attribute can hold "
           "parses to a set — a NULL is the parser refusing input it defines an answer for");
    m = media_query_matches_now(ctx, set);
    media_query_free(set);
    return m;
}

/* ---- §4.2.4.3's "create a link request", and §4.6.8.20's "preload" ---------------------------------------- */

/* "To create a link request given a link processing options options: Assert: options's href is not the empty
   string. Let url be the result of encoding-parsing a URL given options's href, relative to options's BASE
   URL. … If url is failure, then return null."
   THE BASE URL IS THE DOCUMENT'S, which is what "create link options from element" fills it with ("base URL:
   document's document base URL") — so `<base href>` moves a preload's address, and the resolution is the one
   core/dom/document.h owns rather than a second reading of the document's address. Returns a malloc'd absolute
   serialization, or NULL for §4.2.4.3's "url is failure". */
static char *link_url_absolute(JSContext *ctx, const char *href, size_t href_n)
{
    UrlRecord base, rec;
    const char *base_url = document_base_url(ctx);
    bool have_base;
    char *abs_url = NULL;

    url_record_init(&base);
    have_base = base_url && url_parse(&base, base_url, strlen(base_url), NULL);
    url_record_init(&rec);
    if (url_parse(&rec, href, href_n, have_base ? &base : NULL))
        abs_url = url_serialize(&rec, /*exclude_fragment*/ false);
    url_record_free(&rec);
    url_record_free(&base);
    return abs_url;
}

/* §4.6.8.20's "The fetch and process the linked resource steps for this type of linked resource, given a link
 * element el", with its "preload options" inlined at the one call site that has options — there is no second
 * caller for the split to serve, because §4.6.8.20's other entry ("The process a link header step for this type
 * of link … is to preload options") is `Link:` header processing and this engine has no header-link path.
 *
 * STEP 1, "Update the source set for el", IS NOT RUN AND ITS ABSENCE IS ASSERTED rather than assumed: the
 * source set is only non-null for a link carrying `imagesrcset`, which is §4.8.4.3's grammar reached through
 * core/html/image_source_set.h, and the only step that reads it is the `as=image` branch of preload. So the
 * DCHECK below is where that arrives, at the element that would need it, rather than as a silent wrong answer
 * for a responsive preload.
 */
static void link_preload(JSContext *ctx, lxb_dom_element_t *el)
{
    const char *destination;
    size_t href_n = 0;
    const char *href;
    char *abs;
    UrlRecord rec;
    JSValue wrap;

    /* STEP 3: "Let destination be the result of translating the keyword representing the state of el's `as`
       attribute. If destination is null, then return." */
    destination = translate_preload_destination(el);
    if (!destination) return;

    DCHECK(strcmp(destination, "image") != 0 ||
           !lxb_dom_element_has_attribute(el, (const lxb_char_t *)"imagesrcset", 11),
           "§4.6.8.20's preload reached an `as=image` link carrying `imagesrcset` — its step 1 updates the "
           "source set and its step 2 selects an image source out of it, which is §4.8.4.3's grammar in "
           "core/html/image_source_set.h. Run that selection here and set the href from its result, beside the "
           "identical selection core/html/html_image.c already performs");

    /* "To preload given a link processing options options: 1. If options's type doesn't match options's
       destination, then return." */
    if (!type_matches_preload_destination(el, destination)) return;

    /* §4.2.4.1's prescriptive `media`. It is not a numbered step of §4.6.8.20 because §4.2.4.1 states it over
       every external resource link at once — "the user agent must apply the external resource WHEN the media
       attribute's value matches the environment and the other relevant conditions apply, and MUST NOT apply it
       otherwise" — and §4.6.8.20's appropriate-times list names a media change as a re-trigger precisely
       because this check can decline. */
    if (!link_media_matches(ctx, el)) return;

    /* AN `href` COMPOSED OUT OF UNKNOWN EXTERNAL INPUT IS STILL A REQUEST THE PAGE MAKES, and it reaches the
       @H surface as the SHAPE it is rather than disappearing — the same answer core/html/html_script.c gives an
       unknown `<script src>` and core/html/html_image.c gives an undecided image source. It matters more here
       than at either of those: a chunk address is exactly the thing a bundler composes (`"/c/" + hash + ".js"`),
       and Lexbor would have ToString'd the taint away, so reading the raw attribute below is a read of a
       concrete string that is not the address. Nothing is fetched, because there is no address to fetch, and
       nothing is fired, because §4.6.8.20's two events are a RESPONSE's — an element left waiting for a load
       that cannot come is what a browser does with a URL it could not resolve either. The link is NOT marked
       obtained: it did not obtain a resource, so a later `type` or `media` change is still one of §4.6.8.20's
       appropriate times, and the surface dedupes the address if it is recorded twice. */
    {
        /* BORROWED, never freed — solver/dom_cow.h states the contract at the declaration and
           core/html/html_script.c reads it the same way at the same kind of site. */
        JSValueConst t = dom_cow_attr_taint(el, "href");
        if (!JS_IsUndefined(t)) { endpoint_record(ctx, "GET", t, NULL, 0, NULL); return; }
    }

    /* "Assert: options's href is not the empty string, or options's source set is not null." With no source
       set (above), the href carries the whole of it — and a `<link rel=preload as=script>` with no `href` at
       all is the element §4.2.4's own note describes as not representing a link. */
    href = link_attr(el, "href", &href_n);
    if (!href || href_n == 0) return;

    /* "Let url be the result of encoding-parsing a URL given options's href … If url is failure, then return
       null." and "3. Let request be the result of creating a link request given options. If request is null,
       then return." */
    abs = link_url_absolute(ctx, href, href_n);
    if (!abs) return;

    /* THE ELEMENT IS OBTAINED THE MOMENT THE REQUEST EXISTS, which is what makes the conditional appropriate
       times answerable — see `link_obtained`. It is set before the policy check below for the same reason the
       endpoint is recorded before it: a request a policy refuses is a request this link MADE, and a link that
       re-fetched on the next `type` change because a CSP had blocked it would issue one request per attribute
       write. */
    wrap = node_wrap(ctx, lxb_dom_interface_node(el));
    CHECK(!JS_IsException(wrap), "§4.6.8.20: OOM reaching a link element's processing state");
    link_set_obtained(ctx, wrap);

    /* THE ADDRESS AS THE @H SURFACE MUST SEE IT, recorded BEFORE the policy check for the reason
       core/fetch/fetch.c and core/html/html_image.c record theirs before theirs: the endpoint is what the
       page's code COMPOSED, and a request a policy refuses is still a request the bundle can make. This is the
       @H half of why this component exists — a preloaded chunk is an address the bundle NAMED and, until the
       `load` below fired, never asked for. */
    {
        JSValue uv = JS_NewString(ctx, abs);
        CHECK(!JS_IsException(uv), "§4.6.8.20: OOM naming a preload for the endpoint surface");
        /* §4.2.4.3's create-a-link-request sets no method, so it is Fetch §2.2 "Requests"' `GET`. STATED,
           because the reply seam is keyed on the (method, url) pair. */
        endpoint_record(ctx, "GET", uv, NULL, 0, NULL);
        JS_FreeValue(ctx, uv);
    }

    /* Fetch §4.1 "Main fetch" step 7 — "If should request be blocked due to a bad port … or should request be
       blocked by Content Security Policy returns blocked, then set response to a network error". It runs HERE,
       in the engine, because a policy is the DOCUMENT's; the destination computed above is what CSP §6.8.1
       "Get the effective directive for request" switches on, so `as=script` is governed by `script-src` and
       `as=style` by `style-src`. A blocked request is a network error, and §4.6.8.20's processResponse fires
       `error` for one — so the page's handler runs, and the `<script>` it would have injected is not created,
       which is exactly what a browser does under that policy. */
    url_record_init(&rec);
    if (url_parse(&rec, abs, strlen(abs), NULL) &&
        (fetch_block_bad_port(&rec) == FETCH_PORT_BLOCKED ||
         policy_should_block_request(document_policy(ctx), &rec, destination,
                                     /*redirect count*/ 0) == CSP_REQUEST_BLOCKED)) {
        url_record_free(&rec);
        link_queue_fire(ctx, wrap, "error");
        JS_FreeValue(ctx, wrap);
        free(abs);
        return;
    }
    url_record_free(&rec);

    {
        JSValueConst data[1];
        JSValue deliver;
        FetchRequest req;

        data[0] = wrap;
        deliver = JS_NewCFunctionData(ctx, link_deliver, 1, 0, 1, data);
        CHECK(!JS_IsException(deliver), "§4.6.8.20: OOM allocating a preload's processResponse steps");
        req.method = "GET";
        req.url = abs;
        req.headers = NULL;
        req.body = NULL;
        req.body_len = 0;
        fetch_owe(ctx, deliver, &req);
        JS_FreeValue(ctx, deliver);
    }
    JS_FreeValue(ctx, wrap);
    free(abs);
}

/* §4.2.4.3's "fetch and process the linked resource algorithm, which takes a link element el" — the dispatch to
   whichever type's steps this element's `rel` names. Reached only from the appropriate times below, so a type
   arriving here with no steps is not a page doing something unusual, it is THIS FILE having registered a
   trigger it cannot serve — which is why it is an assert and not a silent return. */
static void link_fetch_and_process(JSContext *ctx, lxb_dom_element_t *el)
{
    LinkExternalType t = link_external_type_of(el);

    switch (t) {
    case LINK_EXT_PRELOAD:
        link_preload(ctx, el);
        return;
    case LINK_EXT_MODULEPRELOAD:
        DFAIL("§4.2.4.3's fetch and process the linked resource was entered for a `modulepreload` link, whose "
              "steps this component does not have — §4.6.8.12 ends at `Fetch a modulepreload module script "
              "graph given url, destination, settings object, options`, which is core/loader/module_loader.c's "
              "graph and not a fetch, and then fires `load`/`error` at el exactly as §4.6.8.20 does. Build it "
              "there and route it here; reaching this line means a trigger below was widened without it");
        return;
    case LINK_EXT_UNBUILT:
        DFAIL("§4.2.4.3's fetch and process the linked resource was entered for an external resource link "
              "whose own steps this engine has no component for — every such type defines its own algorithm "
              "and its own appropriate times, so this line is reachable only if a trigger below was registered "
              "for a type whose steps were not. Build the type's component (a `stylesheet` needs the CSS style "
              "sheet core/css/css_rule.h records as absent) and dispatch it above");
        return;
    case LINK_EXT_NONE:
        DFAIL("§4.2.4.3's fetch and process the linked resource was entered for a link element whose `rel` "
              "creates no external resource link — the algorithm is defined only over external resource links, "
              "so the triggers below must ask before they call and one of them did not");
        return;
    }
    DFAIL("§4.2.4's link type classifier answered with a state this dispatch has no arm for");
}

/* ---- §4.6.8.20's "appropriate times to fetch and process the linked resource" ----------------------------- */

/* THE ONE PLACE THAT DECIDES WHETHER A TRIGGER FIRES, so the four entry points below state only WHICH of the
   appropriate times happened. `connected` is §4.6.8.20's own qualifier on every one of its times — each reads
   "a link element that is already browsing-context connected" or "becomes browsing-context connected" — and a
   `<link>` built with `createElement` and configured before insertion must not fetch until it is inserted,
   which is the order the idiom this component exists for actually writes. */
static void link_trigger(JSContext *ctx, lxb_dom_element_t *el)
{
    if (!link_is_node(lxb_dom_interface_node(el))) return;
    if (!node_is_connected(lxb_dom_interface_node(el))) return;
    /* ONLY THE TYPES WHOSE TIMES THIS COMPONENT CARRIES. See html_link.h: §4.6.8 states the appropriate times
       PER LINK TYPE, so a type this file has no steps for is a type whose times it does not register — the
       standard's own structure, and the reason the dispatch's other arms are asserts rather than live paths. */
    if (link_external_type_of(el) != LINK_EXT_PRELOAD) return;
    /* §4.2.4.3's default fetch and process the linked resource ENDS IN A FETCH — "Set request's synchronous
     * flag … Fetch request with processResponseConsumeBody set to …" — with no task and no microtask anywhere
     * in it, so every trigger below reaches a request. THIS ENGINE CANNOT ISSUE ONE OUTSIDE A FLOW: the park
     * pushes onto the flow's own pending register (solver/engine.c), which is per-flow because a global one
     * would resolve one flow's fetch into another flow's COW, so a caller with no flow has nowhere to be owed
     * a reply and nowhere to resume when it arrives.
     *
     * THE THREE LIVE TIMES HAVE A FLOW AND THE PARSED WALK DOES NOT, WHICH IS WHY THE ASSERT IS HERE AND NOT
     * AT THE PARK. `html_link_inserted` and `html_link_attr_changed` are reached from script or from a job,
     * and both of those ARE flows. `html_link_parsed` is reached from core/dom/document.c's parsed-tree walk,
     * which that file states is "still the pre-boot BASELINE" — this engine performs there the steps a browser
     * performs during TREE CONSTRUCTION, because a lexbor parse has no per-token seam. Measured on a real
     * document: gitlab.com/explore ships four `<link rel=preload as=font>` in its head, and a document whose
     * ENTIRE content is one such element and no script at all aborted the engine before a single flow ran —
     * which on a page that also ships scripts means every endpoint that page's bundle names is lost.
     *
     * `<img>` DOES NOT REACH THIS AND THE DIFFERENCE IS NOT A DESIGN. §4.8.4.3.5's "update the image data"
     * queues the SPEC'S OWN microtask before it fetches, and §scheduler makes every enqueued job a flow, so the
     * image half of the same walk hands its fetch to a flow by the standard's structure rather than by anyone
     * having arranged it. Queueing one HERE to match would be inventing a step §4.2.4.3 does not have, and
     * §Browser-half makes ORDER the spec: a preload's request would then be issued after work the parse
     * ordered before it.
     *
     * WHAT TO BUILD, AND THE LINE IT FALLS ON: the baseline walk may RECORD the address — an endpoint is what
     * the page COMPOSED and recording it is not a request, which is why link_preload's endpoint_record stands
     * before this and stays — but it may not ISSUE one. The issue belongs to the BOOT FLOW, beside where that
     * document's parsed scripts are driven (core/loader/document_scripts.h's inventory is the same shape one
     * step earlier: the walk inventories, the flow runs). Until it is there, this crashes rather than letting a
     * document's own preloads go unrequested in silence — a `<link rel=preload as=script>` that never fires
     * `load` is a lazy-chunk graph that never loads, which is the surface this component exists for. */
    DCHECK(flow_running() != NULL,
           "§4.2.4.3's fetch and process the linked resource was entered with no running flow, which means the "
           "PARSED-TREE WALK reached it: core/dom/document.c performs at pre-boot baseline the steps HTML "
           "defines over tree construction, and this one ends in a fetch that has no flow to park on. Drive "
           "the parsed document's preload triggers from the BOOT FLOW instead, where that document's parsed "
           "scripts are driven — not by queueing a task here, which invents a step §4.2.4.3 does not have and "
           "reorders the request against the rest of the parse");
    link_fetch_and_process(ctx, el);
}

void html_link_inserted(JSContext *ctx, lxb_dom_element_t *el)
{
    /* "When the external resource link's link element becomes browsing-context connected." */
    link_trigger(ctx, el);
}

void html_link_attr_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local)
{
    /* A content attribute is the HTML namespace's; a `foo:href` is a different attribute entirely. */
    if (ns && *ns) return;
    DCHECK(local != NULL, "§4.6.8.20 was asked about an attribute change with no attribute name");
    if (!link_is_node(lxb_dom_interface_node(el))) return;

    /* "When the external resource link is CREATED on a link element that is already browsing-context
       connected" — which is what a `rel` gaining the keyword IS — and the `href` and `as` changes beside it.
       Each is unconditional: the link may already have been obtained under a DIFFERENT address or a different
       destination, and re-running is what a browser does. */
    if (!strcmp(local, "rel") || !strcmp(local, "href") || !strcmp(local, "as")) {
        link_trigger(ctx, el);
        return;
    }
    /* …and the two CONDITIONAL times, "but was previously not obtained due to the type attribute specifying an
       unsupported type for the request destination" / "due to the media attribute not matching the
       environment". The condition is read as "not yet obtained" for the reason derived at `link_obtained`. */
    if (!strcmp(local, "type") || !strcmp(local, "media")) {
        JSValue wrap;
        bool done;

        if (!node_is_connected(lxb_dom_interface_node(el))) return;
        wrap = node_wrap(ctx, lxb_dom_interface_node(el));
        CHECK(!JS_IsException(wrap), "§4.6.8.20: OOM reaching a link element's processing state");
        done = link_obtained(ctx, wrap);
        JS_FreeValue(ctx, wrap);
        if (done) return;
        link_trigger(ctx, el);
    }
}

void html_link_parsed(JSContext *ctx, lxb_dom_node_t *root)
{
    lxb_dom_node_t *n;

    DCHECK(root != NULL, "§4.2.4's parsed-tree walk was given no tree to walk");
    DCHECK(g_ready, "a parsed tree reached §4.2.4 before html_link_declare ran");
    /* SHADOW-INCLUDING, for the reason core/html/html_image.c's walk is: a `<template shadowrootmode>` has by
       now become a shadow root whose children are in the tree and are not reachable by the ordinary walk. */
    for (n = root; n; n = shadow_root_next_in_shadow_including(ctx, n, root)) {
        if (!link_is_node(n)) continue;
        link_trigger(ctx, lxb_dom_interface_element(n));
    }
}

/* ---- the agent's declaration ------------------------------------------------------------------------------ */

void html_link_declare(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(!g_ready, "html_link_declare ran twice — §4.2.4's state key and machine are declared once per "
                     "AGENT, and a second Symbol would leave every link element that already carries its "
                     "processing state under the first key answering as though it had none under the second");
    g_state_key = JS_NewSymbol(ctx, "linkProcessing", false);
    CHECK(!JS_IsException(g_state_key), "§4.2.4: the link processing state key allocation failed");
    g_atom_state = JS_ValueToAtom(ctx, g_state_key);
    CHECK(g_atom_state != JS_ATOM_NULL, "§4.2.4: the link processing state key could not be interned");
    g_task_stepid = JS_RegisterStepDef(rt, &link_task_def);
    g_ready = 1;
}

void html_link_free(JSRuntime *rt)
{
    if (!g_ready) return;
    JS_FreeAtomRT(rt, g_atom_state);
    g_atom_state = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_state_key);
    g_state_key = JS_UNDEFINED;
    g_task_stepid = -1;
    g_ready = 0;
}
