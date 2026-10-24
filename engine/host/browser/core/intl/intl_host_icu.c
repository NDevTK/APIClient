/* The native build's Intl host: ICU4C, linked from the system (engine/native_libs.mjs). See intl_host.h. */
#ifndef __EMSCRIPTEN__
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unicode/ucal.h>
#include <unicode/uloc.h>
#include <unicode/ulocdata.h>
#include <unicode/ustring.h>
#include <unicode/uversion.h>

#include "check.h"
#include "core/intl/intl_host.h"
#include "core/json_buf.h"

/* ---- answers --------------------------------------------------------------------------------------------- */

static char *answer_number(double v)
{
    JsonBuf b = { 0 };
    char num[40];

    snprintf(num, sizeof num, "%.17g", v);
    json_buf_raw(&b, "{");
    json_buf_key(&b, "ok");
    json_buf_raw(&b, num);
    json_buf_raw(&b, "}");
    return json_buf_take(&b);
}

static const IntlHostValue *arg(const IntlHostRequest *req, int i, IntlHostValueKind kind)
{
    DCHECKF(i < req->nargs && req->args[i].kind == kind, "%s's argument %d is missing or of the wrong kind",
            intl_host_op_name(req->op), i);
    return &req->args[i];
}

/* ---- time zones ------------------------------------------------------------------------------------------ */

/* The calendar a zone's offsets are read from, kept for the last zone asked: ICU's zone data does not change in
   a process, so nothing a flow writes can make the cached calendar wrong. */
static UCalendar *g_zone_cal;
static char g_zone_name[128];

static UCalendar *zone_calendar(const IntlHostValue *zone)
{
    UChar uzone[128];
    int32_t n = 0;
    UErrorCode st = U_ZERO_ERROR;

    CHECKF(zone->len < sizeof g_zone_name, "a time zone identifier of %zu bytes reached the host", zone->len);
    if (g_zone_cal && strlen(g_zone_name) == zone->len && memcmp(g_zone_name, zone->str, zone->len) == 0)
        return g_zone_cal;
    u_strFromUTF8(uzone, 128, &n, zone->str, (int32_t)zone->len, &st);
    CHECKF(U_SUCCESS(st), "ICU could not read the time zone identifier %.*s", (int)zone->len, zone->str);
    if (g_zone_cal) ucal_close(g_zone_cal);
    g_zone_cal = ucal_open(uzone, n, "en", UCAL_GREGORIAN, &st);
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

/* ---- the data's identity --------------------------------------------------------------------------------- */

static char *data_version(void)
{
    UVersionInfo v;
    char icu[U_MAX_VERSION_STRING_LENGTH], cldr[U_MAX_VERSION_STRING_LENGTH];
    UErrorCode st = U_ZERO_ERROR;
    const char *tz;
    JsonBuf b = { 0 };

    u_getVersion(v);
    u_versionToString(v, icu);
    ulocdata_getCLDRVersion(v, &st);
    u_versionToString(v, cldr);
    tz = ucal_getTZDataVersion(&st);
    CHECKF(U_SUCCESS(st), "ICU could not state its data versions: %s", u_errorName(st));
    json_buf_raw(&b, "{");
    json_buf_key(&b, "ok");
    json_buf_raw(&b, "{");
    json_buf_key(&b, "host");
    json_buf_str(&b, "icu4c");
    json_buf_raw(&b, ",");
    json_buf_key(&b, "icu");
    json_buf_str(&b, icu);
    json_buf_raw(&b, ",");
    json_buf_key(&b, "cldr");
    json_buf_str(&b, cldr);
    json_buf_raw(&b, ",");
    json_buf_key(&b, "tz");
    json_buf_str(&b, tz);
    json_buf_raw(&b, "}}");
    return json_buf_take(&b);
}

char *intl_host_call(const IntlHostRequest *req)
{
    switch (req->op) {
    case INTL_OP_TIME_ZONE_OFFSET: return time_zone_offset(req);
    case INTL_OP_DATA_VERSION: return data_version();
    case INTL_OP_N: break;
    }
    CHECK_FAIL("an Intl host op outside intl_host.h's list");
}

#endif
