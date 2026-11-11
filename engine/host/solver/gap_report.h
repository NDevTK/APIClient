/* gap_report.h — the result document's `_instanceGaps`: the DFAIL sites this engine instance crossed.
 *
 * A release build continues past a DFAIL and records the construct (quickjs-gap.h). This composes that record
 * for the document, so a reader of the surface and the findings also reads which unbuilt code the analysis
 * crossed. The list is the instance's, not a document's: a site does not know which document's flow reached it,
 * and one instance may hold several documents of an agent cluster. Each count is a lifetime counter of the
 * instance, and a session that resumes a parked flow from its delta did not run that flow's prefix, so its
 * list holds what this session ran. A dev build aborts at its first DFAIL, so its documents state an empty list.
 *
 * The engine continued past every site listed, so a site after the first may be a consequence of an earlier
 * one; the list is in first-reach order so the first can be told apart. */
#ifndef ENGINE_HOST_SOLVER_GAP_REPORT_H
#define ENGINE_HOST_SOLVER_GAP_REPORT_H

/* Named residual. Not covered: a release run that aborts at a CHECK composes no document, so the sites it
   crossed before the abort are stated nowhere. Next diff builds: the CHECK emitters write the log as records
   beside their @E line, and bridge.js's crash arm relays them. How its absence would show: a crashed release
   run whose record names an @E and no gap, on a page whose dev run aborts earlier at a DFAIL. */

/* The list as a JSON array in first-reach order, one `{siteFile, siteLine, message, composed, reached}` per
   construct: `siteFile` and `message` are its identity (`message` is a DFAIL's literal or a DFAILF's format),
   `siteLine` locates it, and `reached` counts crossings. `composed` is true where the site handed DFAIL a text it built, and
   `message` is then that text as the first reach wrote it. Caller frees. NULL when the allocator refused the
   composition, which result.c's at-rest loop answers by selling and composing again. */
char *gap_report_json(void);

#endif
