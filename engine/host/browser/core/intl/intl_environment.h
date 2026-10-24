/* The environment ECMAScript and ECMA-402 leave to the implementation, stated once. Both are pins rather than
 * the person's settings, so every host and every session answers one value, and engine/build.mjs reads both
 * from these lines into the artifact's stamp beside the host's data versions.
 *   INTL_DEFAULT_LOCALE is ECMA-402 §6.2.3 DefaultLocale ( ), and navigator.language's example.
 *   INTL_SYSTEM_TIME_ZONE is ECMAScript §21.4.1.24 SystemTimeZoneIdentifier ( ), which Date's local time
 *   (core/intl/system_time_zone.c) and DateTimeFormat's default time zone both read.
 * Named residual. Not covered: DefaultLocale is concrete while navigator.language is an environment unknown
 * whose example is this locale, so a service built with no locales does not derive from the page's language.
 * Next diff builds: DefaultLocale answered from navigator.language's own value, so a service resolved over it
 * carries it as an operand. Absence shows as: `new Intl.NumberFormat().format(x)` reported concrete where
 * `new Intl.NumberFormat(navigator.language).format(x)` is derived. */
#ifndef ENGINE_HOST_BROWSER_CORE_INTL_INTL_ENVIRONMENT_H
#define ENGINE_HOST_BROWSER_CORE_INTL_INTL_ENVIRONMENT_H

#define INTL_DEFAULT_LOCALE "en-US"
#define INTL_SYSTEM_TIME_ZONE "UTC"

#endif
