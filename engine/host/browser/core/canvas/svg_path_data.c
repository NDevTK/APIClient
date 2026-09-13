/* SVG 2 §9.3.9 "The grammar for path data". See svg_path_data.h for why the grammar is matched before any
   conversion and why a parse error is a shorter path rather than a throw. */
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/canvas/canvas_path.h"
#include "core/canvas/svg_path_data.h"

typedef struct {
    JSContext *ctx;
    JSValueConst path;
    const char *p;              /* the cursor; the string is NUL-terminated */
    /* SVG 2 §9.3.6's and SVG 2 §9.3.7's reflected control point: "the first control point is assumed to be the reflection
       of the second control point on the previous command relative to the current point. (If there is no
       previous command or if the previous command was not an C, c, S or s, assume the first control point is
       coincident with the current point.)" — the same rule with Q, q, T, t for the quadratic form. This is the
       ONLY state the parse keeps that the path does not; the current point is read back off the path's own
       header so there is no second copy of it to disagree. */
    double ctrl_x, ctrl_y;
    bool cubic_ctrl;            /* the previous command was C, c, S or s */
    bool quad_ctrl;             /* the previous command was Q, q, T or t */
} SvgPathParser;

/* SVG 2 §9.3.9: `wsp::= (#x9 | #x20 | #xA | #xC | #xD)` — five bytes and no others. Not isspace(), whose set is
   locale-dependent and includes #x0B. */
static bool sp_is_wsp(char c)
{
    return c == 0x09 || c == 0x20 || c == 0x0A || c == 0x0C || c == 0x0D;
}

static bool sp_is_digit(char c) { return c >= '0' && c <= '9'; }

static void sp_wsp(SvgPathParser *s)
{
    while (sp_is_wsp(*s->p)) s->p++;
}

/* SVG 2 §9.3.9: `comma_wsp::= (wsp+ ","? wsp*) | ("," wsp*)`. Returns whether one was present, which the elliptical
   arc argument needs: its separator between the x-axis-rotation and the first flag is the one place the
   grammar writes `comma_wsp` without a `?`. */
static bool sp_comma_wsp(SvgPathParser *s)
{
    if (sp_is_wsp(*s->p)) { sp_wsp(s); if (*s->p == ',') s->p++; sp_wsp(s); return true; }
    if (*s->p == ',') { s->p++; sp_wsp(s); return true; }
    return false;
}

/* SVG 2 §9.3.9's `number::= fractional-constant exponent?`, `fractional-constant::= (digit* "." digit+) | digit+`,
   `exponent::= ("e" | "E") sign? digit+`. NO SIGN: `sign?` belongs to `coordinate`, not to `number`, so the
   two are separate productions here as they are in the grammar. Matched first, converted from the matched
   bytes alone. */
static bool sp_number(SvgPathParser *s, double *out)
{
    const char *start = s->p, *q = s->p;
    char stackbuf[128], *buf;
    size_t n;
    bool any = false;

    while (sp_is_digit(*q)) { q++; any = true; }
    if (*q == '.') {
        const char *dot = q;
        q++;
        if (!sp_is_digit(*q)) { q = dot; }      /* a trailing bare point is not a number in SVG 2 */
        else { while (sp_is_digit(*q)) q++; any = true; }
    }
    if (!any) return false;
    if (*q == 'e' || *q == 'E') {
        const char *e = q;
        q++;
        if (*q == '+' || *q == '-') q++;
        if (!sp_is_digit(*q)) q = e;            /* an `e` with no digits is not part of the number */
        else while (sp_is_digit(*q)) q++;
    }

    n = (size_t)(q - start);
    buf = n < sizeof(stackbuf) ? stackbuf : malloc(n + 1);
    if (!buf) return false;
    memcpy(buf, start, n);
    buf[n] = '\0';
    /* The span is now exactly one §9.3.9 `number`, so there is nothing left for strtod to over-consume and
       no end pointer to check. */
    *out = strtod(buf, NULL);
    if (buf != stackbuf) free(buf);
    s->p = q;
    return true;
}

/* SVG 2 §9.3.9's `coordinate::= sign? number`. */
static bool sp_coord(SvgPathParser *s, double *out)
{
    const char *save = s->p;
    double sign = 1;

    if (*s->p == '+') s->p++;
    else if (*s->p == '-') { sign = -1; s->p++; }
    if (!sp_number(s, out)) { s->p = save; return false; }
    *out *= sign;
    return true;
}

/* SVG 2 §9.3.9's `coordinate_pair::= coordinate comma_wsp? coordinate`. */
static bool sp_coord_pair(SvgPathParser *s, double *x, double *y)
{
    const char *save = s->p;

    if (!sp_coord(s, x)) return false;
    sp_comma_wsp(s);
    if (!sp_coord(s, y)) { s->p = save; return false; }
    return true;
}

/* SVG 2 §9.3.9's `flag::= ("0" | "1")` — exactly one character. */
static bool sp_flag(SvgPathParser *s, bool *out)
{
    if (*s->p == '0') { *out = false; s->p++; return true; }
    if (*s->p == '1') { *out = true;  s->p++; return true; }
    return false;
}

static double sp_cur_x(SvgPathParser *s) { return canvas_path_header(s->ctx, s->path, CANVAS_PATH_CUR_X); }
static double sp_cur_y(SvgPathParser *s) { return canvas_path_header(s->ctx, s->path, CANVAS_PATH_CUR_Y); }

/* SVG 2 §B.2.4 "Conversion from endpoint to center parameterization", with SVG 2 §B.2.5 "Correction of
   out-of-range radii" ahead of it and SVG 2 §9.5.1's two degenerate arms ahead of that. Emits the segment onto the path.
   THE ARC BECOMES AN ELLIPSE OP AND NOT A SEGMENT KIND OF ITS OWN, which is what makes canvas_path's `ellipse`
   the one implementation of an elliptical segment in this engine: SVG 2 §B.2.4 produces exactly the centre, the two
   radii, the rotation and the two angles that HTML §4.12.5.1.6's ellipse method steps take. */
static void sp_arc(SvgPathParser *s, double rx, double ry, double phi_deg, bool large, bool sweep,
                   double x2, double y2)
{
    double x1 = sp_cur_x(s), y1 = sp_cur_y(s);
    double phi, cp, sp_, dx2, dy2, x1p, y1p, lam, rx2, ry2, num, den, co, cxp, cyp, cx, cy;
    double ux, uy, vx, vy, theta1, dtheta, sign;

    /* SVG 2 §9.5.1: "If the endpoint (x, y) of the segment is identical to the current point … then this is
       equivalent to omitting the elliptical arc segment entirely." */
    if (x1 == x2 && y1 == y2) return;
    /* SVG 2 §B.2.5 step 2 and SVG 2 §9.5.1: "If either rx or ry have negative signs, these are dropped". Unreachable from
       SVG 2 §9.3.9's grammar, whose `elliptical_arc_argument` spells these three as `number` and therefore admits
       no sign; kept because SVG 2 §9.5.1 states it of the PARAMETERS rather than of the syntax, and because a caller
       that is not this grammar may reach the same conversion. */
    rx = fabs(rx); ry = fabs(ry);
    /* SVG 2 §B.2.5 step 1 and SVG 2 §9.5.1: "If either rx or ry is 0, then this arc is treated as a straight line segment
       (a 'lineto') joining the endpoints." */
    if (rx == 0 || ry == 0) { canvas_path_line_to(s->ctx, s->path, x2, y2); return; }

    phi = phi_deg * M_PI / 180.0;
    cp = cos(phi); sp_ = sin(phi);
    dx2 = (x1 - x2) / 2; dy2 = (y1 - y2) / 2;
    x1p =  cp * dx2 + sp_ * dy2;                                        /* eq. 5.1 */
    y1p = -sp_ * dx2 + cp * dy2;

    rx2 = rx * rx; ry2 = ry * ry;
    lam = (x1p * x1p) / rx2 + (y1p * y1p) / ry2;                        /* eq. 6.2 */
    if (lam > 1) {                                                      /* eq. 6.3 */
        double k = sqrt(lam);
        rx *= k; ry *= k;
        rx2 = rx * rx; ry2 = ry * ry;
    }

    num = rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p;                /* eq. 5.2 */
    den = rx2 * y1p * y1p + ry2 * x1p * x1p;
    /* SVG 2 §B.2.5's own note: "As a consequence of the radii corrections in this section, the equation (5.2) for
       the centre of the ellipse always has at least one solution (i.e. the radicand is never negative)." The
       clamp is for floating-point residue at the exactly-one-solution boundary, not for a case the algorithm
       allows. */
    if (num < 0) num = 0;
    if (den == 0) { canvas_path_line_to(s->ctx, s->path, x2, y2); return; }
    co = sqrt(num / den);
    if (large == sweep) co = -co;                                       /* "+ if fA != fS, − if fA = fS" */
    cxp =  co * (rx * y1p / ry);
    cyp = -co * (ry * x1p / rx);

    cx = cp * cxp - sp_ * cyp + (x1 + x2) / 2;                          /* eq. 5.3 */
    cy = sp_ * cxp + cp * cyp + (y1 + y2) / 2;

    ux = (x1p - cxp) / rx; uy = (y1p - cyp) / ry;                       /* eq. 5.5 and eq. 5.6's operands */
    vx = (-x1p - cxp) / rx; vy = (-y1p - cyp) / ry;

    /* eq. 5.4 — "the angle between two vectors … ± arccos(u·v / (|u||v|)) where the ± sign appearing here is
       the sign of ux·vy − uy·vx". */
    {
        double n1 = hypot(ux, uy), d1 = n1 != 0 ? ux / n1 : 0;
        if (d1 > 1) d1 = 1;
        if (d1 < -1) d1 = -1;
        theta1 = (uy < 0 ? -1 : 1) * acos(d1);                          /* against (1, 0): sign of 1·uy − 0 */
    }
    {
        double n1 = hypot(ux, uy), n2 = hypot(vx, vy);
        double dot = n1 != 0 && n2 != 0 ? (ux * vx + uy * vy) / (n1 * n2) : 1;
        if (dot > 1) dot = 1;
        if (dot < -1) dot = -1;
        sign = ux * vy - uy * vx;
        dtheta = (sign < 0 ? -1 : 1) * acos(dot);
    }
    /* "where Δθ is fixed in the range −360° < Δθ < 360° such that: if fS = 0, then Δθ < 0, else if fS = 1,
       then Δθ > 0. In other words, if fS = 0 and the right side of (eq. 5.6) is greater than 0, then subtract
       360°, whereas if fS = 1 and the right side of (eq. 5.6) is less than 0, then add 360°." */
    if (!sweep && dtheta > 0) dtheta -= 2 * M_PI;
    else if (sweep && dtheta < 0) dtheta += 2 * M_PI;

    /* HTML §4.12.5.1.6's ellipse method steps add a straight line from the last point to the arc's start
       point, which for this conversion IS the current point — so that line is degenerate by construction and
       the two algorithms compose without either knowing about the other. */
    canvas_path_ellipse(s->ctx, s->path, cx, cy, rx, ry, phi, theta1, theta1 + dtheta, dtheta < 0);
}

void svg_path_data_parse(JSContext *ctx, JSValueConst path, const char *d)
{
    SvgPathParser s;
    char cmd = 0;

    DCHECK(d != NULL, "the SVG path data parser was given no string — its caller converts one first");
    s.ctx = ctx; s.path = path; s.p = d;
    s.ctrl_x = s.ctrl_y = 0; s.cubic_ctrl = s.quad_ctrl = false;

    /* SVG 2 §9.3.9's `svg_path::= wsp* (moveto (wsp* drawto_command)*)? wsp*` — a path that does not begin with a moveto
       describes nothing, which is SVG 2 §9.5.4's "up to (but not including) the path command containing the first
       error" with the first command being that error. */
    sp_wsp(&s);
    if (*s.p != 'M' && *s.p != 'm') return;

    for (;;) {
        bool rel;
        double cx0, cy0;

        sp_wsp(&s);
        if (*s.p == '\0') return;

        /* A command letter, or a repetition of the previous command's argument group. SVG 2 §9.3.3: "If a moveto is
           followed by multiple pairs of coordinates, the subsequent pairs are treated as implicit lineto
           commands" — so a repeated M becomes L and a repeated m becomes l, which is the one command whose
           repetition is not itself. */
        if (strchr("MmZzLlHhVvCcSsQqTtAa", *s.p) != NULL) {
            cmd = *s.p;
            s.p++;
            sp_wsp(&s);
        } else if (cmd == 0 || cmd == 'Z' || cmd == 'z') {
            return;                                 /* closepath takes no arguments and repeats as nothing */
        }

        rel = cmd >= 'a' && cmd <= 'z';
        cx0 = sp_cur_x(&s); cy0 = sp_cur_y(&s);

        switch (cmd) {
        case 'M': case 'm': {
            double x, y;
            if (!sp_coord_pair(&s, &x, &y)) return;
            if (rel) { x += cx0; y += cy0; }
            canvas_path_move_to(ctx, path, x, y);
            s.cubic_ctrl = s.quad_ctrl = false;
            cmd = rel ? 'l' : 'L';                  /* SVG 2 §9.3.3's implicit lineto for the following pairs */
            break;
        }
        case 'Z': case 'z':
            /* SVG 2 §9.3.4 and SVG 2 §9.3.4.1. HTML §4.12.5.1.6's closePath also opens the new subpath at the closed
               one's first point, which is SVG 2 §9.3.4's "If a 'closepath' is followed immediately by any other
               command, then the next subpath starts at the same initial point as the current subpath" — so
               the two standards' closepaths are one act here rather than two. */
            canvas_path_close_path(ctx, path);
            s.cubic_ctrl = s.quad_ctrl = false;
            break;
        case 'L': case 'l': {
            double x, y;
            if (!sp_coord_pair(&s, &x, &y)) return;
            if (rel) { x += cx0; y += cy0; }
            canvas_path_line_to(ctx, path, x, y);
            s.cubic_ctrl = s.quad_ctrl = false;
            break;
        }
        case 'H': case 'h': {
            double x;
            if (!sp_coord(&s, &x)) return;
            if (rel) x += cx0;
            canvas_path_line_to(ctx, path, x, cy0);
            s.cubic_ctrl = s.quad_ctrl = false;
            break;
        }
        case 'V': case 'v': {
            double y;
            if (!sp_coord(&s, &y)) return;
            if (rel) y += cy0;
            canvas_path_line_to(ctx, path, cx0, y);
            s.cubic_ctrl = s.quad_ctrl = false;
            break;
        }
        case 'C': case 'c': {
            double x1, y1, x2, y2, x, y;
            if (!sp_coord_pair(&s, &x1, &y1)) return;
            sp_comma_wsp(&s);
            if (!sp_coord_pair(&s, &x2, &y2)) return;
            sp_comma_wsp(&s);
            if (!sp_coord_pair(&s, &x, &y)) return;
            if (rel) { x1 += cx0; y1 += cy0; x2 += cx0; y2 += cy0; x += cx0; y += cy0; }
            canvas_path_bezier_curve_to(ctx, path, x1, y1, x2, y2, x, y);
            s.ctrl_x = x2; s.ctrl_y = y2; s.cubic_ctrl = true; s.quad_ctrl = false;
            break;
        }
        case 'S': case 's': {
            double x1, y1, x2, y2, x, y;
            if (!sp_coord_pair(&s, &x2, &y2)) return;
            sp_comma_wsp(&s);
            if (!sp_coord_pair(&s, &x, &y)) return;
            if (rel) { x2 += cx0; y2 += cy0; x += cx0; y += cy0; }
            /* SVG 2 §9.3.6's reflection, and its parenthesis: with no previous cubic the first control point is
               the current point. */
            if (s.cubic_ctrl) { x1 = 2 * cx0 - s.ctrl_x; y1 = 2 * cy0 - s.ctrl_y; }
            else { x1 = cx0; y1 = cy0; }
            canvas_path_bezier_curve_to(ctx, path, x1, y1, x2, y2, x, y);
            s.ctrl_x = x2; s.ctrl_y = y2; s.cubic_ctrl = true; s.quad_ctrl = false;
            break;
        }
        case 'Q': case 'q': {
            double x1, y1, x, y;
            if (!sp_coord_pair(&s, &x1, &y1)) return;
            sp_comma_wsp(&s);
            if (!sp_coord_pair(&s, &x, &y)) return;
            if (rel) { x1 += cx0; y1 += cy0; x += cx0; y += cy0; }
            canvas_path_quadratic_curve_to(ctx, path, x1, y1, x, y);
            s.ctrl_x = x1; s.ctrl_y = y1; s.quad_ctrl = true; s.cubic_ctrl = false;
            break;
        }
        case 'T': case 't': {
            double x1, y1, x, y;
            if (!sp_coord_pair(&s, &x, &y)) return;
            if (rel) { x += cx0; y += cy0; }
            if (s.quad_ctrl) { x1 = 2 * cx0 - s.ctrl_x; y1 = 2 * cy0 - s.ctrl_y; }
            else { x1 = cx0; y1 = cy0; }
            canvas_path_quadratic_curve_to(ctx, path, x1, y1, x, y);
            s.ctrl_x = x1; s.ctrl_y = y1; s.quad_ctrl = true; s.cubic_ctrl = false;
            break;
        }
        case 'A': case 'a': {
            /* `elliptical_arc_argument::= number comma_wsp? number comma_wsp? number comma_wsp flag
               comma_wsp? flag comma_wsp? coordinate_pair`. The separator before the FIRST FLAG is the one
               `comma_wsp` in the whole grammar written without a `?`, so it is required and a missing one is
               SVG 2 §9.5.4's error rather than a leniency. The three leading values are `number` and not
               `coordinate`, so none of them admits a sign. */
            double rx, ry, rot, x, y;
            bool large, sweep;
            if (!sp_number(&s, &rx)) return;
            sp_comma_wsp(&s);
            if (!sp_number(&s, &ry)) return;
            sp_comma_wsp(&s);
            if (!sp_number(&s, &rot)) return;
            if (!sp_comma_wsp(&s)) return;
            if (!sp_flag(&s, &large)) return;
            sp_comma_wsp(&s);
            if (!sp_flag(&s, &sweep)) return;
            sp_comma_wsp(&s);
            if (!sp_coord_pair(&s, &x, &y)) return;
            if (rel) { x += cx0; y += cy0; }
            sp_arc(&s, rx, ry, rot, large, sweep, x, y);
            s.cubic_ctrl = s.quad_ctrl = false;
            break;
        }
        default:
            /* The letter set is this file's own, one line above, so the arm is unreachable by construction. */
            DFAIL("the SVG path data parser dispatched on a letter its own command set does not contain");
            return;
        }
        sp_comma_wsp(&s);
    }
}
