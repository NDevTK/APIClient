/* ECMAScript §21.4.1.24 SystemTimeZoneIdentifier ( ) and §21.4.1.25 LocalTime ( tv ): the zone Date's local time
 * reads, answered by the Intl host over the pin core/intl/intl_environment.h states. */
#ifndef ENGINE_HOST_BROWSER_CORE_INTL_SYSTEM_TIME_ZONE_H
#define ENGINE_HOST_BROWSER_CORE_INTL_SYSTEM_TIME_ZONE_H

#include "quickjs.h"

/* Once per agent: Date's local-time offset becomes the system time zone's, through the host. */
void system_time_zone_init(JSContext *ctx);

#endif
