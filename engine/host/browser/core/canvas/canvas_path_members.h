/* HTML §4.12.5.1.6 "Building paths" — THE MIXIN'S WEB IDL SURFACE, which is a different contract from the PATH
 * and therefore a different header.
 *
 * IT IS SPLIT FROM canvas_path.h FOR A MEASURED REASON AND NOT FOR TIDINESS. That header is a PATH: a list of
 * subpaths, an opcode vocabulary and the arithmetic that builds one, with no realm and no argument conversion
 * anywhere in it — which is why core/graphics/raster_path.h includes it to READ `canvas_path_op_width` and says
 * of itself that every coordinate in it is a device pixel. Declaring a member needs `IdlArgType`, so putting
 * these three entries beside the path put `core/idl_args.h` into that header's include set, and the graphics
 * layer began pulling in the whole IDL machinery to learn what an opcode is. Measured with a positive control:
 * a translation unit including only `core/graphics/raster_path.h` reaches idl_args.h's own include guard with
 * the declarations there and does not reach it with them here. The two audiences are disjoint — the
 * rasterizer wants the stream and never a declaration, and an INCLUDER wants both — so the split is by
 * CONSUMER and the implementation stays one component in canvas_path.c.
 *
 * THE MEMBER LIST, ITS NAMES, ITS ARITIES, ITS DECLARED TYPES AND ITS DISPATCH BELONG TO THE MIXIN AND NOT TO
 * ITS INCLUDERS, AND A SECOND INCLUDER IS EXACTLY WHEN THAT STOPS BEING A PREFERENCE. Web IDL §3.7.3
 * "Interface prototype object" gives a mixin no prototype of its own, so every includer PLACES these nine on
 * its own prototype — and the enum, the name table, the declared argument counts, the two `IdlArgType` tables
 * and the nine-arm switch were all written once inside the includer that landed first. Copied into the second
 * they become five tables and one body free to disagree about an arity, which is the two-right-answers shape
 * CLAUDE.md's `A FIX OF THE FORM X IS NOT HOW TO ASK Q` rule says to route rather than to duplicate — and the
 * rule's name is BACKTICKED rather than quoted, because a quoted run standing in prose a citation governs is
 * read by the citation auditor as a claim about that section's text, which is a fabrication finding
 * manufactured out of a correct reference to this project's own spec.
 *
 * WHAT IS GENUINELY PER-INCLUDER IS WHICH PATH THE MEMBERS BUILD, and that is the one thing the includer
 * supplies: a `Path2D` holds one behind its class opaque, and §4.12.5.1's rendering context holds its current
 * default path on its state record. So the includer resolves its own path — including the brand check, which
 * is a `TypeError` over a page-supplied receiver rather than an assert — and hands the ARRAY over.
 *
 * `roundRect` IS NOT IN THIS LIST AND ITS ABSENCE IS DELIBERATE. §4.12.5.1.6 declares TEN members and this is
 * nine of them; the tenth is a named residual at each includer's install, because its `radii` position is a
 * three-armed union no position in this platform declares yet. Placing it here would be a member whose
 * argument nothing converts. */
#ifndef ENGINE_HOST_BROWSER_CORE_CANVAS_CANVAS_PATH_MEMBERS_H
#define ENGINE_HOST_BROWSER_CORE_CANVAS_CANVAS_PATH_MEMBERS_H

#include "quickjs.h"
#include "core/canvas/canvas_path.h"
#include "core/idl_args.h"   /* IdlArgType — a declared position's type, which is why this header is not that one */

typedef enum {
    CANVAS_PATH_M_CLOSE_PATH = 0, CANVAS_PATH_M_MOVE_TO, CANVAS_PATH_M_LINE_TO,
    CANVAS_PATH_M_QUADRATIC_CURVE_TO, CANVAS_PATH_M_BEZIER_CURVE_TO, CANVAS_PATH_M_ARC_TO,
    CANVAS_PATH_M_RECT, CANVAS_PATH_M_ARC, CANVAS_PATH_M_ELLIPSE, CANVAS_PATH_M_COUNT
} CanvasPathMember;

/* The IDL's own names, in the IDL's own order — the MAGIC is the member, so the order is the declaration and
   nothing composes a name at a site. */
extern const char *const CANVAS_PATH_M_NAMES[CANVAS_PATH_M_COUNT];

/* The DECLARED argument count of each member, from the harvested IDL, so the arity a member is declared with
   and the arity its body reads are one fact. */
extern const int CANVAS_PATH_M_ARGC[CANVAS_PATH_M_COUNT];

/* The declared TYPES of one member's positions, for `idl_method_id`, or NULL for a member with no arguments.
   Every position is `unrestricted double` except `arc`'s and `ellipse`'s trailing `counterclockwise`, which is
   the one optional position in the mixin and is a `boolean`. */
const IdlArgType *canvas_path_member_types(int magic);

/* The index of the member's first OPTIONAL position for `idl_optional_from`, or -1 where every position is
   required. Derived from the same tables the types are, so an includer cannot declare an arity here and an
   optional cursor that disagrees with it. */
int canvas_path_member_optional_from(int magic);

/* Run member `magic` over the includer's already-resolved `path`, with the arguments the declaration
   converted. Returns 0, or -1 with an exception pending — the three members that throw are the three
   §4.12.5.1.6 says throw, and every one of these is `undefined` in the IDL, so a caller's only answer is
   `JS_UNDEFINED` or `JS_EXCEPTION`.
   NOTHING HERE APPLIES A TRANSFORMATION MATRIX, which is canvas_path.h's own opening argument and is why
   §4.12.5.1.6's "for objects implementing the CanvasDrawPath and CanvasTransform interfaces, the points passed
   to the methods, and the resulting lines added to current default path by these methods, must be transformed
   according to the current transformation matrix before being added to the path" is owed by the INCLUDER at
   its own call site and not here. */
int canvas_path_member(JSContext *ctx, JSValueConst path, int magic, int argc, JSValueConst *argv);

#endif /* ENGINE_HOST_BROWSER_CORE_CANVAS_CANVAS_PATH_MEMBERS_H */
