/* The system time zone Date's local time reads. See system_time_zone.h. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/intl/intl_environment.h"
#include "core/intl/intl_host.h"
#include "core/intl/system_time_zone.h"
#include "core/timing/thread_work.h"

/* Furthest from the epoch the host is asked about, in ms: a time value is within 8.64e15 of it (ECMAScript
   §21.4.1.1 Time Values and Time Range), and V8's DateTimeFormat refuses an instant outside it. MakeDate's
   unclipped local value can overshoot the range by under a day before TimeClip, and is asked at the bound. */
#define LOCAL_TIME_ASK_BOUND_MS 8.64e15

/* ECMAScript §21.4.1.25 LocalTime ( tv )'s offset at `time_ms`, in quickjs's unit: minutes of UTC minus local. */
static int system_time_zone_offset(int64_t time_ms)
{
    static const char ZONE[] = INTL_SYSTEM_TIME_ZONE;
    double t = (double)time_ms;
    IntlHostRequest req;
    ThreadWorkSpan span;
    char *answer;
    double ms;
    size_t bytes;

    if (t > LOCAL_TIME_ASK_BOUND_MS) t = LOCAL_TIME_ASK_BOUND_MS;
    if (t < -LOCAL_TIME_ASK_BOUND_MS) t = -LOCAL_TIME_ASK_BOUND_MS;
    memset(&req, 0, sizeof req);
    req.op = INTL_OP_TIME_ZONE_OFFSET;
    req.args[0] = (IntlHostValue){ .kind = INTL_VALUE_STRING, .str = ZONE, .len = sizeof ZONE - 1 };
    req.args[1] = (IntlHostValue){ .kind = INTL_VALUE_NUMBER, .num = t };
    req.nargs = 2;
    thread_work_begin(&span);
    answer = intl_host_call(&req);
    bytes = sizeof ZONE - 1 + sizeof t + strlen(answer);
    ms = intl_host_answer_number(answer, req.op);
    free(answer);
    thread_work_end(&span, THREAD_WORK_INTL, bytes);
    thread_work_charge(THREAD_WORK_INTL, bytes);
    DCHECKF(ms == (double)(int64_t)(ms / 60000) * 60000,
            "the system time zone %s is %.17g ms from UTC, not a whole minute: Date's local time is kept in minutes, "
            "so a sub-minute zone needs quickjs's offset in ms", ZONE, ms);
    return (int)(-ms / 60000);
}

void system_time_zone_init(JSContext *ctx)
{
    (void)ctx;
    JS_SetLocalTimeZoneOffsetFunc(system_time_zone_offset);
}
