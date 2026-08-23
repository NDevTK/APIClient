/* CSS Values and Units 4 §7 "Other Quantities". See css_dimension.h for why the three families that need no
 * realm live apart from §6's `<length>`, and why every entry takes a span. */
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "check.h"
#include "core/css/css_dimension.h"

/* The longest unit identifier §7 defines is four bytes (`grad`, `turn`, `dppx`, `dpcm`), so a unit that does
   not fit is not one of them and the comparison below answers false without a copy. The buffer is one larger
   than it has to be so the NUL is never the thing that decides. */
#define CSS_DIM_UNIT_MAX 8

/* THE UNIT, LOWERCASED — CSS Syntax §4 makes a dimension token's unit an ident sequence and CSS compares unit
   identifiers ASCII case-insensitively, so `10DEG` and `10deg` are one value. False when it cannot be one of
   §7's identifiers at all, which is what keeps the caller's own tables spelled in lower case only. */
static bool css_dim_unit_of(const char *unit, size_t unit_len, char out[CSS_DIM_UNIT_MAX])
{
    size_t i;

    DCHECK(unit != NULL || unit_len == 0,
           "a CSS dimension's unit was asked about through a NULL span with a non-zero length — a dimension "
           "token names its unit inside the buffer it was tokenized from, so an absent pointer is a caller "
           "that lost the buffer");
    if (unit_len == 0 || unit_len >= CSS_DIM_UNIT_MAX) return false;
    for (i = 0; i < unit_len; i++) out[i] = (char)tolower((unsigned char)unit[i]);
    out[unit_len] = '\0';
    return true;
}

static bool css_dim_in(const char *const *set, unsigned n, const char *unit)
{
    unsigned i;

    for (i = 0; i < n; i++)
        if (strcmp(set[i], unit) == 0) return true;
    return false;
}

#define CSS_DIM_N(set) (sizeof(set) / sizeof((set)[0]))

/* §7.1's four. */
static const char *const CSS_ANGLE_UNITS[] = { "deg", "grad", "rad", "turn" };
/* §7.2's two. */
static const char *const CSS_TIME_UNITS[] = { "s", "ms" };
/* §7.4's four identifiers, which name three quantities — `x` is defined beside `dppx` with the same words. */
static const char *const CSS_RESOLUTION_UNITS[] = { "dpi", "dpcm", "dppx", "x" };

bool css_angle_unit(const char *unit, size_t unit_len)
{
    char u[CSS_DIM_UNIT_MAX];

    if (!css_dim_unit_of(unit, unit_len, u)) return false;
    return css_dim_in(CSS_ANGLE_UNITS, CSS_DIM_N(CSS_ANGLE_UNITS), u);
}

bool css_time_unit(const char *unit, size_t unit_len)
{
    char u[CSS_DIM_UNIT_MAX];

    if (!css_dim_unit_of(unit, unit_len, u)) return false;
    return css_dim_in(CSS_TIME_UNITS, CSS_DIM_N(CSS_TIME_UNITS), u);
}

bool css_resolution_unit(const char *unit, size_t unit_len)
{
    char u[CSS_DIM_UNIT_MAX];

    if (!css_dim_unit_of(unit, unit_len, u)) return false;
    return css_dim_in(CSS_RESOLUTION_UNITS, CSS_DIM_N(CSS_RESOLUTION_UNITS), u);
}

bool css_resolution_dppx(const char *unit, size_t unit_len, double n, double *dppx)
{
    char u[CSS_DIM_UNIT_MAX];

    DCHECK(dppx != NULL, "§7.4's canonical resolution was asked for with nowhere to put it");
    if (!css_dim_unit_of(unit, unit_len, u)) return false;
    if (strcmp(u, "dppx") == 0 || strcmp(u, "x") == 0) { *dppx = n; return true; }
    if (strcmp(u, "dpi") == 0) { *dppx = n / 96.0; return true; }
    if (strcmp(u, "dpcm") == 0) { *dppx = n * 2.54 / 96.0; return true; }
    DCHECK(!css_resolution_unit(unit, unit_len),
           "§7.4 names a resolution unit this conversion has no ratio for — the membership test and the "
           "conversion are two readings of ONE list, and a unit in one and not the other is that list having "
           "grown in one place");
    return false;
}
