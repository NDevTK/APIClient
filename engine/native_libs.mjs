/* The system libraries a native link of the engine's host takes, in link order: ICU4C for the Intl host
 * (engine/host/browser/core/intl/intl_host_icu.c), then libm and pthreads. One list, so every native link of the
 * host (build.mjs, wpt.mjs) links the same program. ICU is linked by its unversioned names, as its development
 * package installs them; its headers come from the system include path. */
export const NATIVE_HOST_LIBS = ["-licui18n", "-licuuc", "-licudata", "-lm", "-lpthread"];
