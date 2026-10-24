/* The environment ECMAScript leaves to the implementation, stated once. The system time zone is ECMAScript
 * §21.4.1.24 SystemTimeZoneIdentifier ( ): a pin rather than the person's setting, so every host and every
 * session answers one zone, Date's local time (core/intl/system_time_zone.c) reads it, and engine/build.mjs
 * reads it from this line into the artifact's stamp beside the host's time zone data version. */
#ifndef ENGINE_HOST_BROWSER_CORE_INTL_INTL_ENVIRONMENT_H
#define ENGINE_HOST_BROWSER_CORE_INTL_INTL_ENVIRONMENT_H

#define INTL_SYSTEM_TIME_ZONE "UTC"

#endif
