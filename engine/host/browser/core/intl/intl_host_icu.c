/* The native build's Intl host: ICU4C, linked from the system (engine/native_libs.mjs). See intl_host.h.
 *
 * ECMA-402's locale negotiation is implementation-defined data over a fixed algorithm, so this file runs the
 * algorithm (§9.2.7 ResolveLocale, with §9.2.3 LookupMatchingLocaleByPrefix as the best fit too, which §9.2.7
 * permits) over ICU's data, and answers each formatting question with an ICU formatter built from the
 * instance's resolved slots. A formatter depends only on its slots, so it is kept per slot spelling. */
#ifndef __EMSCRIPTEN__
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <unicode/ucal.h>
#include <unicode/ucol.h>
#include <unicode/ucurr.h>
#include <unicode/udat.h>
#include <unicode/udatpg.h>
#include <unicode/uenum.h>
#include <unicode/ufieldpositer.h>
#include <unicode/uformattedvalue.h>
#include <unicode/uloc.h>
#include <unicode/ulocdata.h>
#include <unicode/unumberformatter.h>
#include <unicode/unumsys.h>
#include <unicode/ustring.h>
#include <unicode/uversion.h>

#include "check.h"
#include "core/intl/intl_environment.h"
#include "core/intl/intl_host.h"
#include "core/json_buf.h"
#include <lexbor/core/base.h>   /* lexbor_realloc: the allocator the engine gives its refusal edge */
#include "solver/reclaim.h"

/* ---- requests and answers ------------------------------------------------------------------------------ */

static const IntlHostValue *arg(const IntlHostRequest *req, int i, IntlHostValueKind kind)
{
    DCHECKF(i < req->nargs && req->args[i].kind == kind, "%s's argument %d is missing or of the wrong kind",
            intl_host_op_name(req->op), i);
    return &req->args[i];
}

/* A string field, or NULL when the request does not carry it (or carries null). */
static const char *field_str(const IntlHostRequest *req, const char *key)
{
    const IntlHostValue *v = intl_host_field(req, key);

    if (!v || v->kind == INTL_VALUE_NULL) return NULL;
    DCHECKF(v->kind == INTL_VALUE_STRING, "%s's field %s is not a string", intl_host_op_name(req->op), key);
    return v->str;
}

static int field_bool(const IntlHostRequest *req, const char *key, int dflt)
{
    const IntlHostValue *v = intl_host_field(req, key);

    if (!v) return dflt;
    DCHECKF(v->kind == INTL_VALUE_BOOLEAN, "%s's field %s is not a boolean", intl_host_op_name(req->op), key);
    return v->boolean;
}

static double field_num(const IntlHostRequest *req, const char *key, double dflt)
{
    const IntlHostValue *v = intl_host_field(req, key);

    if (!v) return dflt;
    DCHECKF(v->kind == INTL_VALUE_NUMBER, "%s's field %s is not a number", intl_host_op_name(req->op), key);
    return v->num;
}

static void ok_open(JsonBuf *b)
{
    json_buf_raw(b, "{");
    json_buf_key(b, "ok");
}

static char *ok_close(JsonBuf *b)
{
    json_buf_raw(b, "}");
    return json_buf_take(b);
}

static char *answer_number(double v)
{
    JsonBuf b = { 0 };
    char num[40];

    snprintf(num, sizeof num, "%.17g", v);
    ok_open(&b);
    json_buf_raw(&b, num);
    return ok_close(&b);
}

static char *answer_string(const char *s)
{
    JsonBuf b = { 0 };

    ok_open(&b);
    json_buf_str(&b, s);
    return ok_close(&b);
}

static char *answer_throw(const char *name, const char *message)
{
    JsonBuf b = { 0 };

    json_buf_raw(&b, "{");
    json_buf_key(&b, "throw");
    json_buf_str(&b, name);
    json_buf_raw(&b, ",");
    json_buf_key(&b, "message");
    json_buf_str(&b, message);
    json_buf_raw(&b, "}");
    return json_buf_take(&b);
}

static void member_str(JsonBuf *b, bool *first, const char *key, const char *v)
{
    if (!*first) json_buf_raw(b, ",");
    *first = false;
    json_buf_str(b, key);
    json_buf_raw(b, ":");
    if (v) json_buf_str(b, v); else json_buf_raw(b, "null");
}

static void member_bool(JsonBuf *b, bool *first, const char *key, bool v)
{
    if (!*first) json_buf_raw(b, ",");
    *first = false;
    json_buf_str(b, key);
    json_buf_raw(b, v ? ":true" : ":false");
}

/* ---- UTF-8 and UTF-16 ----------------------------------------------------------------------------------- */

/* `s` of `n` bytes as UTF-16, malloc'd and NUL-terminated; *out_len its length. */
static UChar *to_u16(const char *s, size_t n, int32_t *out_len)
{
    UErrorCode st = U_ZERO_ERROR;
    int32_t len = 0;
    UChar *u;

    u_strFromUTF8(NULL, 0, &len, s, (int32_t)n, &st);
    st = U_ZERO_ERROR;
    u = lexbor_malloc(sizeof(UChar) * ((size_t)len + 1));
    CHECK(u != NULL, "an ICU string could not be allocated");
    u_strFromUTF8(u, len + 1, &len, s, (int32_t)n, &st);
    CHECKF(U_SUCCESS(st), "ICU could not read UTF-8 input: %s", u_errorName(st));
    if (out_len) *out_len = len;
    return u;
}

/* `u` of `n` units as UTF-8, malloc'd and NUL-terminated. */
static char *to_u8(const UChar *u, int32_t n)
{
    UErrorCode st = U_ZERO_ERROR;
    int32_t len = 0;
    char *s;

    u_strToUTF8(NULL, 0, &len, u, n, &st);
    st = U_ZERO_ERROR;
    s = lexbor_malloc((size_t)len + 1);
    CHECK(s != NULL, "an ICU string could not be allocated");
    u_strToUTF8(s, len + 1, &len, u, n, &st);
    CHECKF(U_SUCCESS(st), "ICU could not write UTF-8: %s", u_errorName(st));
    return s;
}

/* ---- locale identifiers ------------------------------------------------------------------------------ */

/* An ICU locale ID for a BCP 47 tag, malloc'd, or NULL when ICU cannot read the tag. */
static char *icu_locale_of(const char *tag)
{
    char buf[ULOC_FULLNAME_CAPACITY * 2];
    UErrorCode st = U_ZERO_ERROR;
    int32_t parsed = 0;

    uloc_forLanguageTag(tag, buf, (int32_t)sizeof buf, &parsed, &st);
    if (U_FAILURE(st) || st == U_STRING_NOT_TERMINATED_WARNING || parsed != (int32_t)strlen(tag)) return NULL;
    return reclaim_strdup(buf);
}

/* A BCP 47 tag for an ICU locale ID, malloc'd, or NULL. */
static char *bcp47_of(const char *icu)
{
    char buf[ULOC_FULLNAME_CAPACITY * 2];
    UErrorCode st = U_ZERO_ERROR;

    uloc_toLanguageTag(icu, buf, (int32_t)sizeof buf, true, &st);
    if (U_FAILURE(st) || st == U_STRING_NOT_TERMINATED_WARNING) return NULL;
    return reclaim_strdup(buf);
}

/* ECMA-402 §6.2.2 CanonicalizeUnicodeLocaleId ( locale ), by UTS #35 Annex C through ICU's canonicalization. */
static char *canonical_tag(const char *tag)
{
    char *icu = icu_locale_of(tag), canon[ULOC_FULLNAME_CAPACITY * 2], *out;
    UErrorCode st = U_ZERO_ERROR;

    if (!icu) return NULL;
    uloc_canonicalize(icu, canon, (int32_t)sizeof canon, &st);
    free(icu);
    if (U_FAILURE(st) || st == U_STRING_NOT_TERMINATED_WARNING) return NULL;
    out = bcp47_of(canon);
    return out;
}

static char *locale_canonicalize(const IntlHostRequest *req)
{
    char *c = canonical_tag(arg(req, 0, INTL_VALUE_STRING)->str), *a;

    if (!c) return answer_throw("RangeError", "Incorrect locale information provided");
    a = answer_string(c);
    free(c);
    return a;
}

/* ---- available locales --------------------------------------------------------------------------------- */

typedef enum { SVC_COLLATOR, SVC_NUMBER, SVC_DATE, SVC_N } Service;

typedef struct { char **tags; int n, cap; } TagList;

static void tags_add(TagList *l, char *t)
{
    int i;

    if (!t) return;
    for (i = 0; i < l->n; i++)
        if (strcmp(l->tags[i], t) == 0) { free(t); return; }
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 256;
        l->tags = lexbor_realloc(l->tags, sizeof(char *) * (size_t)l->cap);
        CHECK(l->tags != NULL, "an available-locale list could not grow");
    }
    l->tags[l->n++] = t;
}

static TagList g_available[SVC_N];

/* The service's available locales as canonical BCP 47 tags: ICU's own list for the service and the locales its
   data has, and the default locale, which §9.1 requires to be available. Built once: ICU's data does not change in
   a process. */
static const TagList *available(Service svc)
{
    TagList *l = &g_available[svc];
    int32_t i, n;

    if (l->n) return l;
    n = svc == SVC_COLLATOR ? ucol_countAvailable() : svc == SVC_NUMBER ? unum_countAvailable() : udat_countAvailable();
    for (i = 0; i < n; i++) {
        const char *id = svc == SVC_COLLATOR ? ucol_getAvailable(i) : svc == SVC_NUMBER ? unum_getAvailable(i)
                                                                                         : udat_getAvailable(i);
        char *tag = bcp47_of(id), *canon;

        if (!tag) continue;
        canon = canonical_tag(tag);
        free(tag);
        tags_add(l, canon);
    }
    tags_add(l, reclaim_strdup(INTL_DEFAULT_LOCALE));
    return l;
}

static bool is_available(Service svc, const char *tag, size_t len)
{
    const TagList *l = available(svc);
    int i;

    for (i = 0; i < l->n; i++)
        if (strlen(l->tags[i]) == len && memcmp(l->tags[i], tag, len) == 0) return true;
    return false;
}

static Service service_of(const char *name)
{
    if (strcmp(name, "Collator") == 0) return SVC_COLLATOR;
    if (strcmp(name, "NumberFormat") == 0) return SVC_NUMBER;
    DCHECKF(strcmp(name, "DateTimeFormat") == 0, "an Intl service this host does not have: %s", name);
    return SVC_DATE;
}

/* The start and length of `tag`'s Unicode locale extension sequence ("-u-…"), or a length of 0. */
static void extension_span(const char *tag, size_t *at, size_t *len)
{
    const char *u = strstr(tag, "-u-"), *x = strstr(tag, "-x-"), *e;

    *at = *len = 0;
    if (!u || (x && x < u)) return;
    for (e = u + 3; *e; e++)
        if (e[0] == '-' && e[1] && (e[2] == '-' || e[2] == '\0')) break;   /* the next singleton */
    *at = (size_t)(u - tag);
    *len = (size_t)(e - u);
}

/* §9.2.3 LookupMatchingLocaleByPrefix ( availableLocales, requestedLocales ): the index of the requested tag that
   matched, with *found the available prefix (malloc'd); -1 when none did. */
static int lookup_match(Service svc, const IntlHostRequest *req, char **found)
{
    int i;

    for (i = 0; i < req->nlocales; i++) {
        const char *tag = req->locales[i];
        size_t at, ext, n = strlen(tag), pos;
        char *base = lexbor_malloc(n + 1);

        CHECK(base != NULL, "a locale match could not be allocated");
        extension_span(tag, &at, &ext);
        if (ext) {
            memcpy(base, tag, at);
            strcpy(base + at, tag + at + ext);
        } else {
            strcpy(base, tag);
        }
        n = strlen(base);
        while (n > 0) {
            if (is_available(svc, base, n)) { base[n] = '\0'; *found = base; return i; }
            pos = n;
            while (pos > 0 && base[pos - 1] != '-') pos--;
            pos = pos > 0 ? pos - 1 : 0;
            while (pos >= 2 && base[pos - 2] == '-') pos -= 2;
            n = pos;
        }
        free(base);
    }
    return -1;
}

/* §9.2.5 UnicodeExtensionComponents: the value of keyword `key` in the extension at [ext, ext + len), or NULL; an
   empty value is returned as "". */
static char *extension_keyword(const char *ext, size_t len, const char *key)
{
    size_t k = 3;
    const char *in_key = NULL;
    char *val = NULL;

    while (k < len) {
        const char *sub = ext + k, *e = memchr(sub, '-', len - k);
        size_t n = e ? (size_t)(e - sub) : len - k;

        if (n == 2) {
            if (in_key) break;                              /* the keyword we were in had an empty value */
            if (memcmp(sub, key, 2) == 0) { in_key = sub; val = reclaim_strdup(""); }
        } else if (in_key) {
            size_t old = strlen(val);

            val = lexbor_realloc(val, old + n + 2);
            CHECK(val != NULL, "an extension keyword could not be read");
            if (old) val[old++] = '-';
            memcpy(val + old, sub, n);
            val[old + n] = '\0';
        }
        k += n + 1;
    }
    return val;
}

/* The locale data for one relevant extension key: a list, its first element the locale's default (NULL for a
   null default). */
typedef struct { const char *items[64]; char *owned[64]; int n; } KeyData;

static void kd_add(KeyData *d, const char *v, bool own)
{
    int i;

    for (i = 0; i < d->n; i++)
        if ((d->items[i] == NULL) == (v == NULL) && (!v || strcmp(d->items[i], v) == 0)) {
            if (own) free((char *)v);
            return;
        }
    if (d->n == 64) { if (own) free((char *)v); return; }
    d->items[d->n] = v;
    d->owned[d->n] = own ? (char *)v : NULL;
    d->n++;
}

static void kd_free(KeyData *d)
{
    int i;

    for (i = 0; i < d->n; i++) free(d->owned[i]);
}

static bool kd_has(const KeyData *d, const char *v)
{
    int i;

    for (i = 0; i < d->n; i++)
        if ((d->items[i] == NULL) == (v == NULL) && (!v || strcmp(d->items[i], v) == 0)) return true;
    return false;
}

/* An ICU keyword enumeration, as BCP 47 type values. */
static void kd_add_enum(KeyData *d, const char *key, UEnumeration *e)
{
    UErrorCode st = U_ZERO_ERROR;
    const char *v;

    if (!e) return;
    while ((v = uenum_next(e, NULL, &st)) != NULL && U_SUCCESS(st)) {
        const char *t = uloc_toUnicodeLocaleType(key, v);

        if (!t) continue;
        if (strcmp(key, "co") == 0 && (strcmp(t, "standard") == 0 || strcmp(t, "search") == 0)) continue;
        kd_add(d, reclaim_strdup(t), true);
    }
    uenum_close(e);
}

static const char *numbering_system_of(const char *icu)
{
    UErrorCode st = U_ZERO_ERROR;
    UNumberingSystem *ns = unumsys_open(icu, &st);
    static char name[16];

    CHECKF(U_SUCCESS(st), "ICU could not name the numbering system of %s: %s", icu, u_errorName(st));
    snprintf(name, sizeof name, "%s", unumsys_getName(ns));
    unumsys_close(ns);
    return name;
}

/* The simple (non-algorithmic) numbering systems ICU has, for §6.7.1 AvailableCanonicalNumberingSystems. */
static void kd_add_numbering_systems(KeyData *d)
{
    UErrorCode st = U_ZERO_ERROR;
    UEnumeration *e = unumsys_openAvailableNames(&st);
    const char *v;

    CHECKF(U_SUCCESS(st), "ICU could not list its numbering systems: %s", u_errorName(st));
    while ((v = uenum_next(e, NULL, &st)) != NULL && U_SUCCESS(st)) {
        UErrorCode s2 = U_ZERO_ERROR;
        UNumberingSystem *ns = unumsys_openByName(v, &s2);

        if (U_SUCCESS(s2) && !unumsys_isAlgorithmic(ns)) kd_add(d, reclaim_strdup(v), true);
        if (ns) unumsys_close(ns);
    }
    uenum_close(e);
}

/* The collator ICU builds for a locale with its locale data's defaults, closed by the caller. */
static UCollator *default_collator(const char *icu)
{
    UErrorCode st = U_ZERO_ERROR;
    UCollator *c = ucol_open(icu, &st);

    CHECKF(U_SUCCESS(st), "ICU could not open a collator for %s: %s", icu, u_errorName(st));
    return c;
}

static void key_data(Service svc, const char *found, const char *key, KeyData *d)
{
    char *icu = icu_locale_of(found);
    UErrorCode st = U_ZERO_ERROR;

    CHECKF(icu != NULL, "the available locale %s is not one ICU can read", found);
    memset(d, 0, sizeof *d);
    if (strcmp(key, "co") == 0) {
        kd_add(d, NULL, false);                                         /* §10.2.3: the first is null */
        kd_add_enum(d, "co", ucol_getKeywordValuesForLocale("collation", icu, false, &st));
    } else if (strcmp(key, "kn") == 0 || strcmp(key, "kf") == 0) {
        UCollator *c = default_collator(icu);
        UColAttributeValue v;

        if (strcmp(key, "kn") == 0) {
            v = ucol_getAttribute(c, UCOL_NUMERIC_COLLATION, &st);
            kd_add(d, v == UCOL_ON ? "true" : "false", false);
            kd_add(d, "false", false);
            kd_add(d, "true", false);
        } else {
            v = ucol_getAttribute(c, UCOL_CASE_FIRST, &st);
            kd_add(d, v == UCOL_UPPER_FIRST ? "upper" : v == UCOL_LOWER_FIRST ? "lower" : "false", false);
            kd_add(d, "upper", false);
            kd_add(d, "lower", false);
            kd_add(d, "false", false);
        }
        ucol_close(c);
    } else if (strcmp(key, "nu") == 0) {
        kd_add(d, reclaim_strdup(numbering_system_of(icu)), true);
        kd_add_numbering_systems(d);
    } else if (strcmp(key, "ca") == 0) {
        kd_add_enum(d, "ca", ucal_getKeywordValuesForLocale("calendar", icu, true, &st));
    } else {
        DCHECKF(strcmp(key, "hc") == 0 && svc == SVC_DATE, "no locale data for the extension key %s", key);
        kd_add(d, NULL, false);                                         /* §11.2.3: the first is null */
        kd_add(d, "h11", false);
        kd_add(d, "h12", false);
        kd_add(d, "h23", false);
        kd_add(d, "h24", false);
    }
    CHECKF(U_SUCCESS(st), "ICU could not state the %s data of %s: %s", key, found, u_errorName(st));
    free(icu);
}

/* §9.2.2 CanonicalizeUValue ( ukey, uvalue ), lowercased and replaced by CLDR's canonical type. */
static char *canonical_uvalue(const char *key, const char *value)
{
    char low[96];
    size_t i, n = strlen(value);
    const char *legacy, *t;

    if (n >= sizeof low) n = sizeof low - 1;
    for (i = 0; i < n; i++) low[i] = (char)(value[i] >= 'A' && value[i] <= 'Z' ? value[i] - 'A' + 'a' : value[i]);
    low[n] = '\0';
    legacy = uloc_toLegacyType(key, low);
    t = legacy ? uloc_toUnicodeLocaleType(key, legacy) : NULL;
    return reclaim_strdup(t ? t : low);
}

typedef struct { char *locale; const char *value[4]; char *owned[4]; char *found; } Resolved;

/* §9.2.7 ResolveLocale ( availableLocales, requestedLocales, options, relevantExtensionKeys, localeData ) over
   the request's locales and its fields named by `keys`, which is also the order of relevantExtensionKeys. */
static void resolve_locale(Service svc, const IntlHostRequest *req, const char *const *keys, int nkeys,
                           Resolved *out)
{
    char *found = NULL, *extension = NULL, ext[256] = "-u";
    int m = lookup_match(svc, req, &found), k;
    size_t at = 0, elen = 0;

    memset(out, 0, sizeof *out);
    if (m < 0) found = reclaim_strdup(INTL_DEFAULT_LOCALE);                     /* step 3 */
    else {
        extension_span(req->locales[m], &at, &elen);
        if (elen) extension = reclaim_strndup(req->locales[m] + at, elen);
    }
    for (k = 0; k < nkeys; k++) {                                       /* step 9 */
        KeyData d;
        const char *value, *opt = NULL;
        char *requested = NULL, *ov = NULL;
        bool supported = false, have_opt = false, empty_keyword = false;
        const IntlHostValue *fv = intl_host_field(req, keys[k]);

        key_data(svc, found, keys[k], &d);
        value = d.n ? d.items[0] : NULL;
        if (extension) requested = extension_keyword(extension, elen, keys[k]);
        if (requested) {
            if (*requested && kd_has(&d, requested)) { value = requested; supported = true; }
            else if (!*requested && kd_has(&d, "true")) { value = "true"; supported = true; empty_keyword = true; }
        }
        if (fv) {                                                       /* steps 9.i-9.k */
            have_opt = true;
            if (fv->kind == INTL_VALUE_STRING) {
                ov = canonical_uvalue(keys[k], fv->str);
                if (!*ov) { free(ov); ov = reclaim_strdup("true"); }
                opt = ov;
            }
        }
        if (have_opt && !((opt == NULL) == (value == NULL) && (!opt || strcmp(opt, value) == 0)) && kd_has(&d, opt)) {
            value = opt;
            supported = false;
        }
        if (supported) {                                                /* supportedKeywords */
            size_t n = strlen(ext);

            snprintf(ext + n, sizeof ext - n, "-%s%s%s", keys[k], empty_keyword ? "" : "-", empty_keyword ? "" : value);
        }
        out->owned[k] = value ? reclaim_strdup(value) : NULL;
        out->value[k] = out->owned[k];
        free(requested);
        free(ov);
        kd_free(&d);
    }
    if (strcmp(ext, "-u") != 0) {                                       /* step 10 */
        char *withext = lexbor_malloc(strlen(found) + strlen(ext) + 1), *canon;

        CHECK(withext != NULL, "a resolved locale could not be allocated");
        strcpy(withext, found);
        strcat(withext, ext);
        canon = canonical_tag(withext);
        free(withext);
        CHECK(canon != NULL, "ResolveLocale built a tag ICU cannot read");
        out->locale = canon;
    } else {
        out->locale = reclaim_strdup(found);
    }
    out->found = found;
    free(extension);
}

static void resolved_free(Resolved *r)
{
    int i;

    for (i = 0; i < 4; i++) free(r->owned[i]);
    free(r->locale);
    free(r->found);
}

/* §9.2.9 FilterLocales ( availableLocales, requestedLocales, options ). */
static char *locale_supported(const IntlHostRequest *req)
{
    Service svc = service_of(field_str(req, "service"));
    JsonBuf b = { 0 };
    int i, n = 0;

    ok_open(&b);
    json_buf_raw(&b, "[");
    for (i = 0; i < req->nlocales; i++) {
        IntlHostRequest one = *req;
        char *found = NULL;

        one.locales = &req->locales[i];
        one.nlocales = 1;
        if (lookup_match(svc, &one, &found) < 0) continue;
        free(found);
        if (n++) json_buf_raw(&b, ",");
        json_buf_str(&b, req->locales[i]);
    }
    json_buf_raw(&b, "]");
    return ok_close(&b);
}

/* ---- Collator -------------------------------------------------------------------------------------------- */

/* The ICU locale a collator for these slots is opened with: the resolved locale, its collation type for search. */
static char *collator_locale(const char *locale, const char *usage, const char *collation)
{
    char *icu = icu_locale_of(locale), buf[ULOC_FULLNAME_CAPACITY * 2];
    UErrorCode st = U_ZERO_ERROR;

    CHECKF(icu != NULL, "a resolved locale %s is not one ICU can read", locale);
    snprintf(buf, sizeof buf, "%s", icu);
    free(icu);
    if (usage && strcmp(usage, "search") == 0) uloc_setKeywordValue("collation", "search", buf, sizeof buf, &st);
    else if (collation && strcmp(collation, "default") != 0)
        uloc_setKeywordValue("collation", uloc_toLegacyType("co", collation), buf, sizeof buf, &st);
    CHECKF(U_SUCCESS(st), "ICU could not set a collation on %s: %s", locale, u_errorName(st));
    return reclaim_strdup(buf);
}

static char *collator_resolve(const IntlHostRequest *req)
{
    static const char *const KEYS[] = { "co", "kn", "kf" };
    const char *usage = field_str(req, "usage");
    Resolved r;
    UCollator *c;
    char *icu;
    UErrorCode st = U_ZERO_ERROR;
    JsonBuf b = { 0 };
    bool first = true, search = usage && strcmp(usage, "search") == 0;

    resolve_locale(SVC_COLLATOR, req, KEYS, 3, &r);
    icu = collator_locale(r.found, usage, r.value[0]);
    c = default_collator(icu);
    free(icu);
    ok_open(&b);
    json_buf_raw(&b, "{");
    member_str(&b, &first, "locale", r.locale);
    member_str(&b, &first, "collation", r.value[0] ? r.value[0] : "default");
    member_bool(&b, &first, "numeric", r.value[1] && strcmp(r.value[1], "true") == 0);
    member_str(&b, &first, "caseFirst", r.value[2]);
    /* The locale data's [[sensitivity]] and [[ignorePunctuation]], from the strength and alternate handling ICU's
       data gives the collator. */
    {
        UColAttributeValue strength = ucol_getAttribute(c, UCOL_STRENGTH, &st);
        UColAttributeValue alt = ucol_getAttribute(c, UCOL_ALTERNATE_HANDLING, &st);

        member_str(&b, &first, "sensitivity", !search ? "variant" : strength == UCOL_PRIMARY ? "base"
                                                     : strength == UCOL_SECONDARY ? "accent" : "variant");
        member_bool(&b, &first, "ignorePunctuation", alt == UCOL_SHIFTED);
    }
    CHECKF(U_SUCCESS(st), "ICU could not read a collator's attributes: %s", u_errorName(st));
    ucol_close(c);
    resolved_free(&r);
    json_buf_raw(&b, "}");
    return ok_close(&b);
}

/* The last collator built, with the slot spelling it was built from. */
static UCollator *g_collator;
static char *g_collator_key;

static char *slot_key(const IntlHostRequest *req)
{
    JsonBuf b = { 0 };
    int i;

    json_buf_str(&b, req->nlocales ? req->locales[0] : "");
    for (i = 0; i < req->nfields; i++) {
        char num[40];

        json_buf_raw(&b, "|");
        json_buf_str(&b, req->fields[i].key);
        switch (req->fields[i].value.kind) {
        case INTL_VALUE_STRING: json_buf_str(&b, req->fields[i].value.str); break;
        case INTL_VALUE_NUMBER: snprintf(num, sizeof num, "%.17g", req->fields[i].value.num); json_buf_raw(&b, num); break;
        case INTL_VALUE_BOOLEAN: json_buf_raw(&b, req->fields[i].value.boolean ? "t" : "f"); break;
        case INTL_VALUE_NULL: json_buf_raw(&b, "null"); break;
        }
    }
    return json_buf_take(&b);
}

static UCollator *collator_for(const IntlHostRequest *req)
{
    char *key = slot_key(req), *icu;
    const char *sens = field_str(req, "sensitivity"), *kf = field_str(req, "caseFirst");
    UErrorCode st = U_ZERO_ERROR;
    UCollator *c;

    if (g_collator && strcmp(key, g_collator_key) == 0) { free(key); return g_collator; }
    DCHECK(req->nlocales == 1, "a collator question carries exactly its resolved locale");
    icu = collator_locale(req->locales[0], field_str(req, "usage"), field_str(req, "collation"));
    c = default_collator(icu);
    free(icu);
    /* §10.3.3.1 CompareStrings: canonically equivalent strings compare equal, so normalization is on. */
    ucol_setAttribute(c, UCOL_NORMALIZATION_MODE, UCOL_ON, &st);
    if (sens) {
        ucol_setStrength(c, strcmp(sens, "base") == 0 || strcmp(sens, "case") == 0 ? UCOL_PRIMARY
                            : strcmp(sens, "accent") == 0 ? UCOL_SECONDARY : UCOL_TERTIARY);
        ucol_setAttribute(c, UCOL_CASE_LEVEL, strcmp(sens, "case") == 0 ? UCOL_ON : UCOL_OFF, &st);
    }
    ucol_setAttribute(c, UCOL_ALTERNATE_HANDLING, field_bool(req, "ignorePunctuation", 0) ? UCOL_SHIFTED
                                                                                          : UCOL_NON_IGNORABLE, &st);
    ucol_setAttribute(c, UCOL_NUMERIC_COLLATION, field_bool(req, "numeric", 0) ? UCOL_ON : UCOL_OFF, &st);
    if (kf)
        ucol_setAttribute(c, UCOL_CASE_FIRST, strcmp(kf, "upper") == 0 ? UCOL_UPPER_FIRST
                                             : strcmp(kf, "lower") == 0 ? UCOL_LOWER_FIRST : UCOL_OFF, &st);
    CHECKF(U_SUCCESS(st), "ICU could not configure a collator: %s", u_errorName(st));
    if (g_collator) ucol_close(g_collator);
    free(g_collator_key);
    g_collator = c;
    g_collator_key = key;
    return c;
}

static char *collator_compare(const IntlHostRequest *req)
{
    UCollator *c = collator_for(req);
    const IntlHostValue *x = arg(req, 0, INTL_VALUE_STRING), *y = arg(req, 1, INTL_VALUE_STRING);
    UErrorCode st = U_ZERO_ERROR;
    UCollationResult r = ucol_strcollUTF8(c, x->str, (int32_t)x->len, y->str, (int32_t)y->len, &st);

    CHECKF(U_SUCCESS(st), "ICU could not compare two strings: %s", u_errorName(st));
    return answer_number(r == UCOL_LESS ? -1 : r == UCOL_GREATER ? 1 : 0);
}

/* ---- NumberFormat --------------------------------------------------------------------------------------- */

static char *number_resolve(const IntlHostRequest *req)
{
    static const char *const KEYS[] = { "nu" };
    Resolved r;
    JsonBuf b = { 0 };
    bool first = true;

    resolve_locale(SVC_NUMBER, req, KEYS, 1, &r);
    ok_open(&b);
    json_buf_raw(&b, "{");
    member_str(&b, &first, "locale", r.locale);
    member_str(&b, &first, "numberingSystem", r.value[0]);
    json_buf_raw(&b, "}");
    resolved_free(&r);
    return ok_close(&b);
}

static char *currency_digits(const IntlHostRequest *req)
{
    const IntlHostValue *c = arg(req, 0, INTL_VALUE_STRING);
    UChar code[4];
    UErrorCode st = U_ZERO_ERROR;
    int32_t d, i;

    DCHECK(c->len == 3, "CurrencyDigits was asked about a currency code that is not three letters");
    for (i = 0; i < 3; i++) code[i] = (UChar)(c->str[i] >= 'a' && c->str[i] <= 'z' ? c->str[i] - 'a' + 'A' : c->str[i]);
    code[3] = 0;
    d = ucurr_getDefaultFractionDigits(code, &st);
    if (U_FAILURE(st)) d = 2;   /* ECMA-402 §16.5.1: "If there is no available information ..., return 2." */
    return answer_number(d);
}

static void skel(char *buf, size_t cap, const char *s)
{
    size_t n = strlen(buf);

    snprintf(buf + n, cap - n, "%s%s", n ? " " : "", s);
}

/* The ICU number skeleton that states an Intl.NumberFormat's resolved slots (§16.2.3's internal slots). */
static void number_skeleton(const IntlHostRequest *req, char *buf, size_t cap)
{
    const char *style = field_str(req, "style"), *notation = field_str(req, "notation");
    const char *sign = field_str(req, "signDisplay"), *grouping = NULL, *mode = field_str(req, "roundingMode");
    const char *rtype = field_str(req, "roundingType"), *tz = field_str(req, "trailingZeroDisplay");
    const IntlHostValue *g = intl_host_field(req, "useGrouping");
    double minI = field_num(req, "minimumIntegerDigits", 1), minF = field_num(req, "minimumFractionDigits", 0);
    double maxF = field_num(req, "maximumFractionDigits", 3), minS = field_num(req, "minimumSignificantDigits", 1);
    double maxS = field_num(req, "maximumSignificantDigits", 21), inc = field_num(req, "roundingIncrement", 1);
    bool accounting = field_str(req, "currencySign") && strcmp(field_str(req, "currencySign"), "accounting") == 0;
    char tmp[128], frac[64], sig[64];
    int i;

    buf[0] = '\0';
    if (style && strcmp(style, "percent") == 0) skel(buf, cap, "percent scale/100");
    if (style && strcmp(style, "currency") == 0) {
        const char *cd = field_str(req, "currencyDisplay");

        snprintf(tmp, sizeof tmp, "currency/%s", field_str(req, "currency"));
        for (i = 9; tmp[i]; i++) if (tmp[i] >= 'a' && tmp[i] <= 'z') tmp[i] = (char)(tmp[i] - 'a' + 'A');
        skel(buf, cap, tmp);
        if (cd && strcmp(cd, "code") == 0) skel(buf, cap, "unit-width-iso-code");
        else if (cd && strcmp(cd, "name") == 0) skel(buf, cap, "unit-width-full-name");
        else if (cd && strcmp(cd, "narrowSymbol") == 0) skel(buf, cap, "unit-width-narrow");
    }
    if (style && strcmp(style, "unit") == 0) {
        const char *ud = field_str(req, "unitDisplay");

        snprintf(tmp, sizeof tmp, "unit/%s", field_str(req, "unit"));   /* a core unit or a -per- compound */
        skel(buf, cap, tmp);
        if (ud && strcmp(ud, "long") == 0) skel(buf, cap, "unit-width-full-name");
        else if (ud && strcmp(ud, "narrow") == 0) skel(buf, cap, "unit-width-narrow");
    }
    if (notation && strcmp(notation, "scientific") == 0) skel(buf, cap, "scientific");
    else if (notation && strcmp(notation, "engineering") == 0) skel(buf, cap, "engineering");
    else if (notation && strcmp(notation, "compact") == 0) {
        const char *cdisp = field_str(req, "compactDisplay");

        skel(buf, cap, cdisp && strcmp(cdisp, "long") == 0 ? "compact-long" : "compact-short");
    }
    /* §16.5.2's rounding: fraction digits, significant digits, or both under a priority. */
    frac[0] = '.';
    for (i = 0; i < (int)minF; i++) frac[1 + i] = '0';
    for (; i < (int)maxF; i++) frac[1 + i] = '#';
    frac[1 + i] = '\0';
    if ((int)maxF == 0) strcpy(frac, "precision-integer");
    for (i = 0; i < (int)minS; i++) sig[i] = '@';
    for (; i < (int)maxS; i++) sig[i] = '#';
    sig[i] = '\0';
    if (rtype && strcmp(rtype, "significantDigits") == 0) {
        snprintf(tmp, sizeof tmp, "%s", sig);
    } else if (rtype && (strcmp(rtype, "morePrecision") == 0 || strcmp(rtype, "lessPrecision") == 0)) {
        snprintf(tmp, sizeof tmp, "%s/%s%s", (int)maxF == 0 ? "." : frac, sig,
                 strcmp(rtype, "morePrecision") == 0 ? "r" : "s");
    } else if (inc != 1) {
        double step = inc / pow(10, maxF);

        snprintf(tmp, sizeof tmp, "precision-increment/%.*f", (int)maxF, step);
    } else {
        snprintf(tmp, sizeof tmp, "%s", frac);
    }
    if (tz && strcmp(tz, "stripIfInteger") == 0) strncat(tmp, "/w", sizeof tmp - strlen(tmp) - 1);
    skel(buf, cap, tmp);
    snprintf(tmp, sizeof tmp, "integer-width/*%.*s", (int)minI, "000000000000000000000");
    skel(buf, cap, tmp);
    if (mode) {
        static const char *const MODES[][2] = {
            { "ceil", "ceiling" }, { "floor", "floor" }, { "expand", "up" }, { "trunc", "down" },
            { "halfCeil", "half-ceiling" }, { "halfFloor", "half-floor" }, { "halfExpand", "half-up" },
            { "halfTrunc", "half-down" }, { "halfEven", "half-even" },
        };
        for (i = 0; i < 9; i++)
            if (strcmp(mode, MODES[i][0]) == 0) { snprintf(tmp, sizeof tmp, "rounding-mode-%s", MODES[i][1]); skel(buf, cap, tmp); }
    }
    if (g && g->kind == INTL_VALUE_BOOLEAN) grouping = g->boolean ? "group-on-aligned" : "group-off";
    else if (g) grouping = strcmp(g->str, "always") == 0 ? "group-on-aligned" : strcmp(g->str, "min2") == 0
                                                                                 ? "group-min2" : "group-auto";
    if (grouping) skel(buf, cap, grouping);
    if (sign) {
        static const char *const SIGNS[][3] = {
            { "auto", "sign-auto", "sign-accounting" }, { "never", "sign-never", "sign-never" },
            { "always", "sign-always", "sign-accounting-always" },
            { "exceptZero", "sign-except-zero", "sign-accounting-except-zero" },
            { "negative", "sign-negative", "sign-accounting-negative" },
        };
        for (i = 0; i < 5; i++)
            if (strcmp(sign, SIGNS[i][0]) == 0) skel(buf, cap, SIGNS[i][accounting ? 2 : 1]);
    }
    if (field_str(req, "numberingSystem")) {
        snprintf(tmp, sizeof tmp, "numbering-system/%s", field_str(req, "numberingSystem"));
        skel(buf, cap, tmp);
    }
}

static UNumberFormatter *g_number;
static char *g_number_key;

static UNumberFormatter *number_formatter(const IntlHostRequest *req)
{
    char *key = slot_key(req), sk[1024], *icu;
    UChar *usk;
    int32_t n;
    UErrorCode st = U_ZERO_ERROR;
    UParseError pe;
    UNumberFormatter *f;

    if (g_number && strcmp(key, g_number_key) == 0) { free(key); return g_number; }
    DCHECK(req->nlocales == 1, "a number format question carries exactly its resolved locale");
    number_skeleton(req, sk, sizeof sk);
    usk = to_u16(sk, strlen(sk), &n);
    icu = icu_locale_of(req->locales[0]);
    CHECKF(icu != NULL, "a resolved locale %s is not one ICU can read", req->locales[0]);
    f = unumf_openForSkeletonAndLocaleWithError(usk, n, icu, &pe, &st);
    CHECKF(U_SUCCESS(st), "ICU refused the number skeleton \"%s\" for %s: %s", sk, icu, u_errorName(st));
    free(usk);
    free(icu);
    if (g_number) unumf_close(g_number);
    free(g_number_key);
    g_number = f;
    g_number_key = key;
    return f;
}

static UFormattedNumber *number_result(const IntlHostRequest *req)
{
    UNumberFormatter *f = number_formatter(req);
    UErrorCode st = U_ZERO_ERROR;
    UFormattedNumber *r = unumf_openResult(&st);
    const IntlHostValue *x = &req->args[0];

    DCHECK(req->nargs == 1, "a number format question carries one value");
    if (x->kind == INTL_VALUE_STRING) unumf_formatDecimal(f, x->str, (int32_t)x->len, r, &st);
    else unumf_formatDouble(f, x->num, r, &st);
    CHECKF(U_SUCCESS(st), "ICU could not format a number: %s", u_errorName(st));
    return r;
}

static char *number_format(const IntlHostRequest *req)
{
    UFormattedNumber *r = number_result(req);
    UErrorCode st = U_ZERO_ERROR;
    int32_t n;
    const UChar *u = ufmtval_getString(unumf_resultAsValue(r, &st), &n, &st);
    char *s, *a;

    CHECKF(U_SUCCESS(st), "ICU could not read a formatted number: %s", u_errorName(st));
    s = to_u8(u, n);
    a = answer_string(s);
    free(s);
    unumf_closeResult(r);
    return a;
}

/* §16.5.6 FormatNumericToParts's part types, by ICU number field. */
static const char *number_part_type(int32_t field, UChar first, const IntlHostValue *x)
{
    switch (field) {
    case UNUM_INTEGER_FIELD:
        if (x->kind == INTL_VALUE_NUMBER && isnan(x->num)) return "nan";
        if (x->kind == INTL_VALUE_NUMBER && isinf(x->num)) return "infinity";
        return "integer";
    case UNUM_FRACTION_FIELD: return "fraction";
    case UNUM_DECIMAL_SEPARATOR_FIELD: return "decimal";
    case UNUM_GROUPING_SEPARATOR_FIELD: return "group";
    case UNUM_CURRENCY_FIELD: return "currency";
    case UNUM_PERCENT_FIELD: return "percentSign";
    case UNUM_SIGN_FIELD: return first == '+' ? "plusSign" : "minusSign";
    case UNUM_EXPONENT_SYMBOL_FIELD: return "exponentSeparator";
    case UNUM_EXPONENT_SIGN_FIELD: return "exponentMinusSign";
    case UNUM_EXPONENT_FIELD: return "exponentInteger";
    case UNUM_COMPACT_FIELD: return "compact";
    case UNUM_MEASURE_UNIT_FIELD: return "unit";
    case UNUM_APPROXIMATELY_SIGN_FIELD: return "approximatelySign";
    default: return NULL;
    }
}

/* Parts from a type per UTF-16 unit: maximal runs of one type, each a {type, value}. */
static char *parts_answer(const UChar *u, int32_t n, const char **types)
{
    JsonBuf b = { 0 };
    int32_t i = 0, k = 0;

    ok_open(&b);
    json_buf_raw(&b, "[");
    while (i < n) {
        int32_t j = i;
        char *v;

        while (j < n && types[j] == types[i]) j++;
        if (k++) json_buf_raw(&b, ",");
        json_buf_raw(&b, "{");
        json_buf_key(&b, "type");
        json_buf_str(&b, types[i] ? types[i] : "literal");
        json_buf_raw(&b, ",");
        json_buf_key(&b, "value");
        v = to_u8(u + i, j - i);
        json_buf_str(&b, v);
        free(v);
        json_buf_raw(&b, "}");
        i = j;
    }
    json_buf_raw(&b, "]");
    return ok_close(&b);
}

static char *number_to_parts(const IntlHostRequest *req)
{
    UFormattedNumber *r = number_result(req);
    UErrorCode st = U_ZERO_ERROR;
    const UFormattedValue *fv = unumf_resultAsValue(r, &st);
    int32_t n, i;
    const UChar *u = ufmtval_getString(fv, &n, &st);
    const char **types = lexbor_calloc((size_t)n + 1, sizeof(char *));
    UConstrainedFieldPosition *pos = ucfpos_open(&st);
    char *a;

    int32_t *span = lexbor_calloc((size_t)n + 1, sizeof(int32_t));

    CHECK(types != NULL && span != NULL, "a parts map could not be allocated");
    ucfpos_constrainCategory(pos, UFIELD_CATEGORY_NUMBER, &st);
    /* A field inside another (a group separator inside the integer) is the narrower span, and the narrower one
       names the unit, whatever order ICU reports them in. */
    while (ufmtval_nextPosition(fv, pos, &st)) {
        int32_t start, limit, field = ucfpos_getField(pos, &st);
        const char *t;

        ucfpos_getIndexes(pos, &start, &limit, &st);
        t = number_part_type(field, start < n ? u[start] : 0, &req->args[0]);
        if (t)
            for (i = start; i < limit; i++)
                if (!types[i] || limit - start < span[i]) { types[i] = t; span[i] = limit - start; }
    }
    free(span);
    CHECKF(U_SUCCESS(st), "ICU could not read a formatted number's fields: %s", u_errorName(st));
    a = parts_answer(u, n, types);
    free(types);
    ucfpos_close(pos);
    unumf_closeResult(r);
    return a;
}

/* ---- time zones ------------------------------------------------------------------------------------------ */

/* §6.5.2 GetAvailableNamedTimeZoneIdentifier ( timeZoneIdentifier )'s [[PrimaryIdentifier]], or NULL. */
static char *primary_time_zone(const char *tz, size_t len)
{
    UErrorCode st = U_ZERO_ERROR;
    UEnumeration *e = ucal_openTimeZones(&st);
    const char *id;
    char *match = NULL;

    CHECKF(U_SUCCESS(st), "ICU could not list its time zones: %s", u_errorName(st));
    while ((id = uenum_next(e, NULL, &st)) != NULL && U_SUCCESS(st))
        if (strlen(id) == len && strncasecmp(id, tz, len) == 0) { match = reclaim_strdup(id); break; }
    uenum_close(e);
    if (!match) {
        if (len == 3 && strncasecmp(tz, "UTC", 3) == 0) return reclaim_strdup("UTC");
        return NULL;
    }
    {
        int32_t un;
        UChar *u = to_u16(match, strlen(match), &un), out[128];
        int32_t n;

        st = U_ZERO_ERROR;
        n = ucal_getIanaTimeZoneID(u, un, out, 128, &st);
        free(u);
        CHECKF(U_SUCCESS(st), "ICU could not name the IANA zone of %s: %s", match, u_errorName(st));
        free(match);
        match = to_u8(out, n);
    }
    /* §6.5.1 step 5.d: "Etc/UTC", "Etc/GMT" and "GMT" are "UTC". */
    if (strcmp(match, "Etc/UTC") == 0 || strcmp(match, "Etc/GMT") == 0 || strcmp(match, "GMT") == 0) {
        free(match);
        match = reclaim_strdup("UTC");
    }
    return match;
}

static char *time_zone_identifier(const IntlHostRequest *req)
{
    const IntlHostValue *z = arg(req, 0, INTL_VALUE_STRING);
    char *p = primary_time_zone(z->str, z->len), *a;

    if (!p) return answer_throw("RangeError", "Invalid time zone specified");
    a = answer_string(p);
    free(p);
    return a;
}

/* The calendar a zone's offsets are read from, kept for the last zone asked. */
static UCalendar *g_zone_cal;
static char g_zone_name[128];

static UCalendar *zone_calendar(const IntlHostValue *zone)
{
    UChar *uzone;
    int32_t n = 0;
    UErrorCode st = U_ZERO_ERROR;

    CHECKF(zone->len < sizeof g_zone_name, "a time zone identifier of %zu bytes reached the host", zone->len);
    if (g_zone_cal && strlen(g_zone_name) == zone->len && memcmp(g_zone_name, zone->str, zone->len) == 0)
        return g_zone_cal;
    uzone = to_u16(zone->str, zone->len, &n);
    if (g_zone_cal) ucal_close(g_zone_cal);
    g_zone_cal = ucal_open(uzone, n, "en", UCAL_GREGORIAN, &st);
    free(uzone);
    CHECKF(U_SUCCESS(st), "ICU could not open a calendar in the time zone %.*s: %s", (int)zone->len, zone->str,
           u_errorName(st));
    memcpy(g_zone_name, zone->str, zone->len);
    g_zone_name[zone->len] = '\0';
    return g_zone_cal;
}

/* The zone's offset from UTC at an instant, standard and daylight parts together, in ms. */
static char *time_zone_offset(const IntlHostRequest *req)
{
    UCalendar *cal = zone_calendar(arg(req, 0, INTL_VALUE_STRING));
    UErrorCode st = U_ZERO_ERROR;
    int32_t zone, dst;

    ucal_setMillis(cal, arg(req, 1, INTL_VALUE_NUMBER)->num, &st);
    zone = ucal_get(cal, UCAL_ZONE_OFFSET, &st);
    dst = ucal_get(cal, UCAL_DST_OFFSET, &st);
    CHECKF(U_SUCCESS(st), "ICU could not read a zone offset at %.17g ms: %s", req->args[1].num, u_errorName(st));
    return answer_number((double)zone + (double)dst);
}

/* ---- DateTimeFormat ----------------------------------------------------------------------------------- */

static UDateTimePatternGenerator *pattern_generator(const char *icu)
{
    UErrorCode st = U_ZERO_ERROR;
    UDateTimePatternGenerator *g = udatpg_open(icu, &st);

    CHECKF(U_SUCCESS(st), "ICU could not open a pattern generator for %s: %s", icu, u_errorName(st));
    return g;
}

static const char *hour_cycle_name(UDateFormatHourCycle hc)
{
    return hc == UDAT_HOUR_CYCLE_11 ? "h11" : hc == UDAT_HOUR_CYCLE_12 ? "h12" : hc == UDAT_HOUR_CYCLE_23 ? "h23"
                                                                                                    : "h24";
}

/* The hour letter of the generator's best pattern for an hour of the skeleton letter `skel` ('h' or 'H'). */
static char hour_letter_of(UDateTimePatternGenerator *g, char skel)
{
    UChar sk[2] = { (UChar)skel, 0 }, pat[64];
    UErrorCode st = U_ZERO_ERROR;
    int32_t n = udatpg_getBestPattern(g, sk, 1, pat, 64, &st), i;
    bool quoted = false;

    CHECKF(U_SUCCESS(st), "ICU could not produce an hour pattern: %s", u_errorName(st));
    for (i = 0; i < n; i++) {
        if (pat[i] == '\'') quoted = !quoted;
        else if (!quoted && (pat[i] == 'h' || pat[i] == 'H' || pat[i] == 'k' || pat[i] == 'K')) return (char)pat[i];
    }
    return skel;
}

static char *date_resolve(const IntlHostRequest *req)
{
    static const char *const KEYS[] = { "ca", "nu", "hc" };
    Resolved r;
    JsonBuf b = { 0 };
    bool first = true;
    char *icu;
    UDateTimePatternGenerator *g;
    UErrorCode st = U_ZERO_ERROR;
    UDateFormatHourCycle hc;
    const char *dflt, *h12, *h24;

    resolve_locale(SVC_DATE, req, KEYS, 3, &r);
    icu = icu_locale_of(r.found);
    CHECKF(icu != NULL, "the available locale %s is not one ICU can read", r.found);
    g = pattern_generator(icu);
    hc = udatpg_getDefaultHourCycle(g, &st);
    CHECKF(U_SUCCESS(st), "ICU could not state %s's hour cycle: %s", r.found, u_errorName(st));
    /* [[hourCycle12]] and [[hourCycle24]]: the hour letter of the locale's own 12-hour and 24-hour patterns. */
    h12 = hour_letter_of(g, 'h') == 'K' ? "h11" : "h12";
    h24 = hour_letter_of(g, 'H') == 'k' ? "h24" : "h23";
    udatpg_close(g);
    free(icu);
    dflt = hour_cycle_name(hc);
    ok_open(&b);
    json_buf_raw(&b, "{");
    member_str(&b, &first, "locale", r.locale);
    member_str(&b, &first, "calendar", r.value[0]);
    member_str(&b, &first, "numberingSystem", r.value[1]);
    member_str(&b, &first, "hc", r.value[2]);
    member_str(&b, &first, "hourCycle", dflt);
    member_str(&b, &first, "hourCycle12", h12);
    member_str(&b, &first, "hourCycle24", h24);
    json_buf_raw(&b, "}");
    resolved_free(&r);
    return ok_close(&b);
}

/* The ICU locale a date formatter is opened with: the resolved locale with its calendar and numbering system. */
static char *date_locale(const IntlHostRequest *req)
{
    char *icu = icu_locale_of(req->locales[0]), buf[ULOC_FULLNAME_CAPACITY * 2];
    const char *ca = field_str(req, "calendar"), *nu = field_str(req, "numberingSystem");
    UErrorCode st = U_ZERO_ERROR;

    CHECKF(icu != NULL, "a resolved locale %s is not one ICU can read", req->locales[0]);
    snprintf(buf, sizeof buf, "%s", icu);
    free(icu);
    if (ca) uloc_setKeywordValue("calendar", uloc_toLegacyType("ca", ca), buf, sizeof buf, &st);
    if (nu) uloc_setKeywordValue("numbers", nu, buf, sizeof buf, &st);
    CHECKF(U_SUCCESS(st), "ICU could not set a date locale's keywords: %s", u_errorName(st));
    return reclaim_strdup(buf);
}

/* Table 16's components, with each value's pattern letters. */
typedef struct { const char *prop; const char *values[6]; const char *letters[6]; } Component;
static const Component COMPONENTS[] = {
    { "weekday", { "narrow", "short", "long" }, { "EEEEE", "EEE", "EEEE" } },
    { "era", { "narrow", "short", "long" }, { "GGGGG", "G", "GGGG" } },
    { "year", { "2-digit", "numeric" }, { "yy", "y" } },
    { "month", { "2-digit", "numeric", "narrow", "short", "long" }, { "MM", "M", "MMMMM", "MMM", "MMMM" } },
    { "day", { "2-digit", "numeric" }, { "dd", "d" } },
    { "dayPeriod", { "narrow", "short", "long" }, { "BBBBB", "B", "BBBB" } },
    { "hour", { "2-digit", "numeric" }, { "jj", "j" } },
    { "minute", { "2-digit", "numeric" }, { "mm", "m" } },
    { "second", { "2-digit", "numeric" }, { "ss", "s" } },
    { "timeZoneName", { "short", "long", "shortOffset", "longOffset", "shortGeneric", "longGeneric" },
      { "z", "zzzz", "O", "OOOO", "v", "vvvv" } },
};
#define N_COMPONENTS ((int)(sizeof COMPONENTS / sizeof COMPONENTS[0]))

static char hour_letter(const char *hc)
{
    return strcmp(hc, "h11") == 0 ? 'K' : strcmp(hc, "h12") == 0 ? 'h' : strcmp(hc, "h23") == 0 ? 'H' : 'k';
}

/* The best pattern ICU's generator gives for the requested components or styles, with every hour letter made the
   requested hour cycle's. */
static char *best_pattern(const IntlHostRequest *req, const char *icu)
{
    const char *ds = field_str(req, "dateStyle"), *ts = field_str(req, "timeStyle"), *hc = field_str(req, "hourCycle");
    UChar pat[512];
    int32_t n, i;
    UErrorCode st = U_ZERO_ERROR;

    if (ds || ts) {
        static const char *const STYLES[] = { "full", "long", "medium", "short" };
        UDateFormatStyle d = UDAT_NONE, t = UDAT_NONE;
        UDateFormat *f;

        for (i = 0; i < 4; i++) {
            if (ds && strcmp(ds, STYLES[i]) == 0) d = (UDateFormatStyle)i;
            if (ts && strcmp(ts, STYLES[i]) == 0) t = (UDateFormatStyle)i;
        }
        static const UChar UTC_ID[] = { 'U', 'T', 'C', 0 };

        f = udat_open(t, d, icu, UTC_ID, -1, NULL, -1, &st);
        CHECKF(U_SUCCESS(st), "ICU could not open a styled date format: %s", u_errorName(st));
        n = udat_toPattern(f, false, pat, 512, &st);
        udat_close(f);
    } else {
        char sk[128] = "";
        UChar *usk;
        int32_t un;
        UDateTimePatternGenerator *g = pattern_generator(icu);
        int32_t fsd = (int32_t)field_num(req, "fractionalSecondDigits", 0);

        for (i = 0; i < N_COMPONENTS; i++) {
            const char *v = field_str(req, COMPONENTS[i].prop);
            int k;

            if (!v) continue;
            for (k = 0; COMPONENTS[i].values[k]; k++)
                if (strcmp(v, COMPONENTS[i].values[k]) == 0) {
                    char letters[8];

                    snprintf(letters, sizeof letters, "%s", COMPONENTS[i].letters[k]);
                    if (strcmp(COMPONENTS[i].prop, "hour") == 0 && hc) {
                        letters[0] = hour_letter(hc);
                        if (letters[1]) letters[1] = letters[0];
                    }
                    strncat(sk, letters, sizeof sk - strlen(sk) - 1);
                }
        }
        for (i = 0; i < fsd; i++) strncat(sk, "S", sizeof sk - strlen(sk) - 1);
        usk = to_u16(sk, strlen(sk), &un);
        n = udatpg_getBestPatternWithOptions(g, usk, un, UDATPG_MATCH_ALL_FIELDS_LENGTH, pat, 512, &st);
        free(usk);
        udatpg_close(g);
    }
    CHECKF(U_SUCCESS(st), "ICU could not produce a date pattern: %s", u_errorName(st));
    if (hc) {                                                          /* the hour cycle the slots resolved */
        bool quoted = false;

        for (i = 0; i < n; i++) {
            if (pat[i] == '\'') quoted = !quoted;
            else if (!quoted && (pat[i] == 'h' || pat[i] == 'H' || pat[i] == 'k' || pat[i] == 'K'))
                pat[i] = (UChar)hour_letter(hc);
        }
    }
    return to_u8(pat, n);
}

/* The components a pattern states, as Table 16's values. */
static void pattern_components(const char *p, JsonBuf *b, bool *first)
{
    static const char LETTERS[] = "EcGyMLdBbahHkKmsSzOvV";
    int counts[128] = { 0 };
    bool quoted = false;
    const char *q;

    for (q = p; *q; q++) {
        if (*q == '\'') { quoted = !quoted; continue; }
        if (!quoted && strchr(LETTERS, *q)) counts[(unsigned char)*q]++;
    }
#define C(ch) counts[(unsigned char)(ch)]
    if (C('E') || C('c')) {
        int n = C('E') + C('c');
        member_str(b, first, "weekday", n >= 5 ? "narrow" : n == 4 ? "long" : "short");
    }
    if (C('G')) member_str(b, first, "era", C('G') >= 5 ? "narrow" : C('G') == 4 ? "long" : "short");
    if (C('y')) member_str(b, first, "year", C('y') == 2 ? "2-digit" : "numeric");
    if (C('M') || C('L')) {
        int n = C('M') + C('L');
        member_str(b, first, "month", n >= 5 ? "narrow" : n == 4 ? "long" : n == 3 ? "short" : n == 2 ? "2-digit"
                                                                                                    : "numeric");
    }
    if (C('d')) member_str(b, first, "day", C('d') == 2 ? "2-digit" : "numeric");
    if (C('B')) member_str(b, first, "dayPeriod", C('B') >= 5 ? "narrow" : C('B') == 4 ? "long" : "short");
    member_bool(b, first, "hasHour", C('h') || C('H') || C('k') || C('K'));
    if (C('h') || C('H') || C('k') || C('K')) {
        int n = C('h') + C('H') + C('k') + C('K');
        member_str(b, first, "hour", n == 2 ? "2-digit" : "numeric");
    }
    if (C('m')) member_str(b, first, "minute", C('m') == 2 ? "2-digit" : "numeric");
    if (C('s')) member_str(b, first, "second", C('s') == 2 ? "2-digit" : "numeric");
    if (C('S')) {
        char num[8];

        snprintf(num, sizeof num, "%d", C('S'));
        if (!*first) json_buf_raw(b, ",");
        *first = false;
        json_buf_key(b, "fractionalSecondDigits");
        json_buf_raw(b, num);
    }
    if (C('z')) member_str(b, first, "timeZoneName", C('z') >= 4 ? "long" : "short");
    if (C('O')) member_str(b, first, "timeZoneName", C('O') >= 4 ? "longOffset" : "shortOffset");
    if (C('v') || C('V')) member_str(b, first, "timeZoneName", C('v') >= 4 ? "longGeneric" : "shortGeneric");
#undef C
}

static char *date_pattern(const IntlHostRequest *req)
{
    char *icu = date_locale(req), *p;
    JsonBuf b = { 0 };
    bool first = true;

    p = best_pattern(req, icu);
    free(icu);
    ok_open(&b);
    json_buf_raw(&b, "{");
    pattern_components(p, &b, &first);
    json_buf_raw(&b, "}");
    free(p);
    return ok_close(&b);
}

static UDateFormat *g_date;
static char *g_date_key;

static UDateFormat *date_formatter(const IntlHostRequest *req)
{
    char *key = slot_key(req), *icu, *pattern;
    const char *tz = field_str(req, "timeZone");
    UChar *upat, *utz;
    int32_t pn, tn;
    UErrorCode st = U_ZERO_ERROR;
    UDateFormat *f;

    if (g_date && strcmp(key, g_date_key) == 0) { free(key); return g_date; }
    DCHECK(tz != NULL, "a date format question carries its time zone");
    icu = date_locale(req);
    /* The slots state the best format by its components or styles, so the pattern is re-derived from them. */
    pattern = best_pattern(req, icu);
    upat = to_u16(pattern, strlen(pattern), &pn);
    free(pattern);
    /* An offset zone (§11.1.3's "+HH:MM") is ICU's "GMT+HH:MM". */
    if (tz[0] == '+' || tz[0] == '-') {
        char g[16];

        snprintf(g, sizeof g, "GMT%s", tz);
        utz = to_u16(g, strlen(g), &tn);
    } else {
        utz = to_u16(tz, strlen(tz), &tn);
    }
    f = udat_open(UDAT_PATTERN, UDAT_PATTERN, icu, utz, tn, upat, pn, &st);
    CHECKF(U_SUCCESS(st), "ICU could not open a date format for %s: %s", icu, u_errorName(st));
    /* §11.5.15 ToLocalTime: the proleptic Gregorian calendar, with no Julian switch. */
    {
        UCalendar *cal = (UCalendar *)udat_getCalendar(f);

        if (strcmp(ucal_getType(cal, &st), "gregorian") == 0) {
            UCalendar *c2 = ucal_clone(cal, &st);

            ucal_setGregorianChange(c2, -8.64e15, &st);
            udat_setCalendar(f, c2);
            ucal_close(c2);
        }
    }
    CHECKF(U_SUCCESS(st), "ICU could not make a date format proleptic: %s", u_errorName(st));
    free(icu);
    free(upat);
    free(utz);
    if (g_date) udat_close(g_date);
    free(g_date_key);
    g_date = f;
    g_date_key = key;
    return f;
}

static char *date_format(const IntlHostRequest *req)
{
    UDateFormat *f = date_formatter(req);
    UChar out[512];
    UErrorCode st = U_ZERO_ERROR;
    int32_t n = udat_format(f, arg(req, 0, INTL_VALUE_NUMBER)->num, out, 512, NULL, &st);
    char *s, *a;

    CHECKF(U_SUCCESS(st), "ICU could not format a date: %s", u_errorName(st));
    s = to_u8(out, n);
    a = answer_string(s);
    free(s);
    return a;
}

/* §11.5.7 FormatDateTimeToParts's part types, by ICU date field. */
static const char *date_part_type(int32_t field)
{
    switch (field) {
    case UDAT_ERA_FIELD: return "era";
    case UDAT_YEAR_FIELD: case UDAT_EXTENDED_YEAR_FIELD: case UDAT_YEAR_WOY_FIELD: return "year";
    case UDAT_YEAR_NAME_FIELD: return "yearName";
    case UDAT_RELATED_YEAR_FIELD: return "relatedYear";
    case UDAT_MONTH_FIELD: case UDAT_STANDALONE_MONTH_FIELD: return "month";
    case UDAT_DATE_FIELD: return "day";
    case UDAT_HOUR_OF_DAY1_FIELD: case UDAT_HOUR_OF_DAY0_FIELD: case UDAT_HOUR1_FIELD: case UDAT_HOUR0_FIELD:
        return "hour";
    case UDAT_MINUTE_FIELD: return "minute";
    case UDAT_SECOND_FIELD: return "second";
    case UDAT_FRACTIONAL_SECOND_FIELD: return "fractionalSecond";
    case UDAT_DAY_OF_WEEK_FIELD: case UDAT_STANDALONE_DAY_FIELD: case UDAT_DOW_LOCAL_FIELD: return "weekday";
    case UDAT_AM_PM_FIELD: case UDAT_AM_PM_MIDNIGHT_NOON_FIELD: case UDAT_FLEXIBLE_DAY_PERIOD_FIELD:
        return "dayPeriod";
    case UDAT_TIMEZONE_FIELD: case UDAT_TIMEZONE_RFC_FIELD: case UDAT_TIMEZONE_GENERIC_FIELD:
    case UDAT_TIMEZONE_SPECIAL_FIELD: case UDAT_TIMEZONE_LOCALIZED_GMT_OFFSET_FIELD:
    case UDAT_TIMEZONE_ISO_FIELD: case UDAT_TIMEZONE_ISO_LOCAL_FIELD:
        return "timeZoneName";
    default: return NULL;
    }
}

static char *date_to_parts(const IntlHostRequest *req)
{
    UDateFormat *f = date_formatter(req);
    UChar out[512];
    UErrorCode st = U_ZERO_ERROR;
    UFieldPositionIterator *it = ufieldpositer_open(&st);
    int32_t n = udat_formatForFields(f, arg(req, 0, INTL_VALUE_NUMBER)->num, out, 512, it, &st), i, field, b, e;
    const char *types[512] = { 0 };
    char *a;

    CHECKF(U_SUCCESS(st), "ICU could not format a date's fields: %s", u_errorName(st));
    while ((field = ufieldpositer_next(it, &b, &e)) >= 0) {
        const char *t = date_part_type(field);

        if (t)
            for (i = b; i < e && i < 512; i++) types[i] = t;
    }
    ufieldpositer_close(it);
    a = parts_answer(out, n, types);
    return a;
}

/* ---- §8.3.2 Intl.supportedValuesOf ----------------------------------------------------------------------- */

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static char *sorted_answer(TagList *l)
{
    JsonBuf b = { 0 };
    int i;

    qsort(l->tags, (size_t)l->n, sizeof(char *), cmp_str);
    ok_open(&b);
    json_buf_raw(&b, "[");
    for (i = 0; i < l->n; i++) {
        if (i) json_buf_raw(&b, ",");
        json_buf_str(&b, l->tags[i]);
        free(l->tags[i]);
    }
    free(l->tags);
    json_buf_raw(&b, "]");
    return ok_close(&b);
}

static void list_enum(TagList *l, const char *key, UEnumeration *e)
{
    UErrorCode st = U_ZERO_ERROR;
    const char *v;

    while ((v = uenum_next(e, NULL, &st)) != NULL && U_SUCCESS(st)) {
        const char *t = key ? uloc_toUnicodeLocaleType(key, v) : v;

        if (!t) continue;
        if (key && strcmp(key, "co") == 0 && (strcmp(t, "standard") == 0 || strcmp(t, "search") == 0)) continue;
        tags_add(l, reclaim_strdup(t));
    }
    uenum_close(e);
}

static char *supported_values(const IntlHostRequest *req)
{
    const char *key = arg(req, 0, INTL_VALUE_STRING)->str;
    TagList l = { 0 };
    UErrorCode st = U_ZERO_ERROR;

    if (strcmp(key, "calendar") == 0) {
        list_enum(&l, "ca", ucal_getKeywordValuesForLocale("calendar", "", false, &st));
    } else if (strcmp(key, "collation") == 0) {
        list_enum(&l, "co", ucol_getKeywordValues("collation", &st));
    } else if (strcmp(key, "currency") == 0) {
        list_enum(&l, NULL, ucurr_openISOCurrencies(UCURR_COMMON | UCURR_NON_DEPRECATED, &st));
    } else if (strcmp(key, "numberingSystem") == 0) {
        KeyData d;
        int i;

        memset(&d, 0, sizeof d);
        kd_add_numbering_systems(&d);
        for (i = 0; i < d.n; i++) tags_add(&l, reclaim_strdup(d.items[i]));
        kd_free(&d);
    } else {
        UEnumeration *e;
        const char *id;

        DCHECKF(strcmp(key, "timeZone") == 0, "Intl.supportedValuesOf reached the host with the key %s", key);
        e = ucal_openTimeZoneIDEnumeration(UCAL_ZONE_TYPE_CANONICAL_LOCATION, NULL, NULL, &st);
        CHECKF(U_SUCCESS(st), "ICU could not list its zones: %s", u_errorName(st));
        while ((id = uenum_next(e, NULL, &st)) != NULL && U_SUCCESS(st))
            tags_add(&l, primary_time_zone(id, strlen(id)));
        uenum_close(e);
        tags_add(&l, reclaim_strdup("UTC"));
    }
    CHECKF(U_SUCCESS(st), "ICU could not list the supported values of %s: %s", key, u_errorName(st));
    return sorted_answer(&l);
}

/* ---- the data's identity --------------------------------------------------------------------------------- */

static char *data_version(void)
{
    UVersionInfo v;
    char icu[U_MAX_VERSION_STRING_LENGTH], cldr[U_MAX_VERSION_STRING_LENGTH];
    UErrorCode st = U_ZERO_ERROR;
    const char *tz;
    JsonBuf b = { 0 };
    bool first = true;

    u_getVersion(v);
    u_versionToString(v, icu);
    ulocdata_getCLDRVersion(v, &st);
    u_versionToString(v, cldr);
    tz = ucal_getTZDataVersion(&st);
    CHECKF(U_SUCCESS(st), "ICU could not state its data versions: %s", u_errorName(st));
    ok_open(&b);
    json_buf_raw(&b, "{");
    member_str(&b, &first, "host", "icu4c");
    member_str(&b, &first, "icu", icu);
    member_str(&b, &first, "cldr", cldr);
    member_str(&b, &first, "tz", tz);
    json_buf_raw(&b, "}");
    return ok_close(&b);
}

char *intl_host_call(const IntlHostRequest *req)
{
    switch (req->op) {
    case INTL_OP_LOCALE_CANONICALIZE: return locale_canonicalize(req);
    case INTL_OP_LOCALE_SUPPORTED: return locale_supported(req);
    case INTL_OP_COLLATOR_RESOLVE: return collator_resolve(req);
    case INTL_OP_COLLATOR_COMPARE: return collator_compare(req);
    case INTL_OP_NUMBER_RESOLVE: return number_resolve(req);
    case INTL_OP_CURRENCY_DIGITS: return currency_digits(req);
    case INTL_OP_NUMBER_FORMAT: return number_format(req);
    case INTL_OP_NUMBER_TO_PARTS: return number_to_parts(req);
    case INTL_OP_TIME_ZONE_IDENTIFIER: return time_zone_identifier(req);
    case INTL_OP_TIME_ZONE_OFFSET: return time_zone_offset(req);
    case INTL_OP_DATE_RESOLVE: return date_resolve(req);
    case INTL_OP_DATE_PATTERN: return date_pattern(req);
    case INTL_OP_DATE_FORMAT: return date_format(req);
    case INTL_OP_DATE_TO_PARTS: return date_to_parts(req);
    case INTL_OP_SUPPORTED_VALUES: return supported_values(req);
    case INTL_OP_DATA_VERSION: return data_version();
    case INTL_OP_N: break;
    }
    CHECK_FAIL("an Intl host op outside intl_host.h's list");
}

#endif
