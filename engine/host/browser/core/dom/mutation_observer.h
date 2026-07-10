#ifndef ENGINE_HOST_BROWSER_CORE_DOM_MUTATION_OBSERVER_H
#define ENGINE_HOST_BROWSER_CORE_DOM_MUTATION_OBSERVER_H
#include "quickjs.h"
/* MutationObserver — Blink core/dom. Its callback never fires headless, so the ctor registers it as a driven scheduler flow;
 * the instance SHAPE is generated from canonical MutationObserver IDL, observe/disconnect/... declared deliberate noops. */
JSValue js_mutation_observer_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv);
#endif
