/* What both Intl hosts share: the op names and the reading of a numeric answer. See intl_host.h. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/intl/intl_host.h"

const char *intl_host_op_name(IntlHostOp op)
{
#define INTL_HOST_OP_NAME(id, name) [id] = name,
    static const char *const NAMES[INTL_OP_N] = { INTL_HOST_OPS(INTL_HOST_OP_NAME) };
#undef INTL_HOST_OP_NAME
    DCHECK((unsigned)op < INTL_OP_N, "an Intl host op outside intl_host.h's list");
    return NAMES[op];
}

double intl_host_answer_number(const char *answer, IntlHostOp op)
{
    static const char OK[] = "{\"ok\":";
    char *end;
    double v;

    CHECKF(strncmp(answer, OK, sizeof OK - 1) == 0, "the Intl host refused %s, whose inputs the engine computed: %s",
           intl_host_op_name(op), answer);
    v = strtod(answer + sizeof OK - 1, &end);
    CHECKF(end != answer + sizeof OK - 1 && strcmp(end, "}") == 0,
           "the Intl host's answer to %s is not one number: %s", intl_host_op_name(op), answer);
    return v;
}

const IntlHostValue *intl_host_field(const IntlHostRequest *req, const char *key)
{
    int i;

    for (i = 0; i < req->nfields; i++)
        if (strcmp(req->fields[i].key, key) == 0) return &req->fields[i].value;
    return NULL;
}
