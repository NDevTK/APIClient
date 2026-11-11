/* Web IDL §3.2 JavaScript type mapping per member, read from the table engine/idlgen.mjs generates from the
   IDL corpus. See core/idl_lang_types.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_lang_types.h"
#include "idl_member_types.h"   /* generated */

static int imt_cmp(const void *key, const void *row)
{
    return strcmp(key, ((const IdlMemberTypes *)row)->member);
}

unsigned idl_member_types_at(const char *iface, const char *member, const char *file, int line)
{
    char key[128];
    const IdlMemberTypes *row;
    int n;

    DCHECK(iface != NULL && member != NULL, "a member's types were asked with no interface or no member name");
    n = snprintf(key, sizeof key, "%s.%s", iface, member);
    CHECK(n > 0 && (size_t)n < sizeof key, "an interface member's name does not fit the lookup key");
    row = bsearch(key, IDL_MEMBER_TYPES, sizeof IDL_MEMBER_TYPES / sizeof IDL_MEMBER_TYPES[0],
                  sizeof IDL_MEMBER_TYPES[0], imt_cmp);
    DCHECKF(row != NULL,
            "%s:%d asked the types of `%s`, which the IDL corpus declares as no attribute or value-returning "
            "operation: the name is misspelled, or it is a mixin's member asked under the mixin's own name",
            file, line, key);
    (void)file; (void)line;
    /* A release build keeps every type for a name the table lacks, so each type test over the value forks. */
    if (row == NULL) return JS_LANG_TYPE_ALL;
    return row->types;
}
