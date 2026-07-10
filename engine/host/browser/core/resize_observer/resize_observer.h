#ifndef ENGINE_HOST_BROWSER_CORE_RESIZE_OBSERVER_H
#define ENGINE_HOST_BROWSER_CORE_RESIZE_OBSERVER_H
#include "quickjs.h"
/* ResizeObserver — Blink core/resize_observer. Its callback never fires headless, so the ctor registers it as a driven scheduler flow;
 * the instance SHAPE is generated from canonical ResizeObserver IDL, observe/disconnect/... declared deliberate noops. */
JSValue js_resize_observer_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv);
#endif
