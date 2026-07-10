/* Runtime-reasoned @WHY — see why.h. */
#include "why.h"
#include <stdio.h>
#include <stdlib.h>
#include "check.h"   /* APICLIENT_DEV */

void why_add(JSContext *ctx, const char *phase, const char *reason) {
    (void)ctx;
    fflush(stdout);
    fprintf(stderr, "@WHY {\"phase\":\"%s\",\"reason\":\"%s\"}\n", phase ? phase : "why", reason ? reason : "");
    fflush(stderr);
#if APICLIENT_DEV
    abort();   /* DEV: a @WHY is a SHOULD-NEVER-HAPPEN forcing function — crash at the origin, never log-and-continue. */
#endif
    /* RELEASE: the gap is genuinely unsupportable outside development, so the @WHY is surfaced but the USER is not
       crashed — the release exemption, never a dev-mode fallback. */
}
