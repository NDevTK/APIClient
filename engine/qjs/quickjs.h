/*
 * QuickJS Javascript Engine
 *
 * Copyright (c) 2017-2026 Fabrice Bellard
 * Copyright (c) 2017-2024 Charlie Gordon
 * Copyright (c) 2023-2026 Ben Noordhuis
 * Copyright (c) 2023-2026 Saúl Ibarra Corretgé
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef QUICKJS_H
#define QUICKJS_H

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QUICKJS_NG 1

/* Helpers. */
#if defined(_WIN32) || defined(__CYGWIN__)
# define QUICKJS_NG_PLAT_WIN32 1
#endif /* defined(_WIN32) || defined(__CYGWIN__) */

#if defined(__GNUC__) || defined(__clang__)
# define QUICKJS_NG_CC_GNULIKE 1
#endif /* defined(__GNUC__) || defined(__clang__) */

/*
 * `JS_EXTERN` -- helper macro that must be used to mark the external
 * interfaces of libqjs.
 *
 * Define BUILDING_QJS_SHARED when building and USING_QJS_SHARED when using
 * shared libqjs.
 *
 * Windows note: The `__declspec` syntax is supported by both Clang and GCC.
 * If building qjs, the BUILDING_QJS_SHARED macro must be defined for libqjs
 * (and only for it) to properly export symbols.
 */
#ifdef QUICKJS_NG_PLAT_WIN32
# if defined(BUILDING_QJS_SHARED)
#  define JS_EXTERN __declspec(dllexport)
# elif defined(USING_QJS_SHARED)
#  define JS_EXTERN __declspec(dllimport)
# else
#  define JS_EXTERN /* nothing */
# endif
#else
# if defined(BUILDING_QJS_SHARED) && defined(QUICKJS_NG_CC_GNULIKE)
#  define JS_EXTERN __attribute__((visibility("default")))
# else
#  define JS_EXTERN /* nothing */
# endif
#endif /* QUICKJS_NG_PLAT_WIN32 */

/*
 * `JS_LIBC_EXTERN` -- helper macro that must be used to mark the extern
 * interfaces of quickjs-libc specifically.
 */
#if defined(QUICKJS_NG_BUILD) && !defined(QJS_BUILD_LIBC) && defined(QUICKJS_NG_PLAT_WIN32)
/*
 * We are building QuickJS-NG, quickjs-libc is a static library and we are on
 * Windows. Then, make sure to not export any interfaces.
 */
# define JS_LIBC_EXTERN /* nothing */
#else
/*
 * Otherwise, if we are either (1) not building QuickJS-NG, (2) libc is built as
 * a part of libqjs, or (3) we are not on Windows, define JS_LIBC_EXTERN to
 * JS_EXTERN.
 */
# define JS_LIBC_EXTERN JS_EXTERN
#endif

/*
 * `JS_MODULE_EXTERN` -- helper macro that must be used to mark `js_init_module`
 * and other public functions of the binary modules. See examples/ for examples
 * of the usage.
 *
 * Windows note: -DQUICKJS_NG_MODULE_BUILD must be set when building a binary
 * module to properly set __declspec.
 */
#ifdef QUICKJS_NG_PLAT_WIN32
# ifdef QUICKJS_NG_MODULE_BUILD
#  define JS_MODULE_EXTERN __declspec(dllexport)
# else
#  define JS_MODULE_EXTERN __declspec(dllimport)
# endif
#else
# if defined(QUICKJS_NG_MODULE_BUILD) && defined(QUICKJS_NG_CC_GNULIKE)
#  define JS_MODULE_EXTERN __attribute__((visibility("default")))
# else
#  define JS_MODULE_EXTERN /* nothing */
# endif
#endif /* QUICKJS_NG_PLAT_WIN32 */

/* Borrowed from Folly */
#ifndef JS_PRINTF_FORMAT
/* Clang on Windows doesn't seem to support _Printf_format_string_ */
#if defined(_MSC_VER) && !defined(__clang__)
#include <sal.h>
#define JS_PRINTF_FORMAT _Printf_format_string_
#define JS_PRINTF_FORMAT_ATTR(format_param, dots_param)
#else
#define JS_PRINTF_FORMAT
#if !defined(__clang__) && defined(__GNUC__)
#define JS_PRINTF_FORMAT_ATTR(format_param, dots_param) \
  __attribute__((format(gnu_printf, format_param, dots_param)))
#else
#define JS_PRINTF_FORMAT_ATTR(format_param, dots_param) \
  __attribute__((format(printf, format_param, dots_param)))
#endif
#endif
#endif

/*
 * `JS_CALL_X` -- macro tree for calling a function with a variable number of arguments.
 * This is used for bulk operations such as freeing values or atoms.
 */
#define JS_COUNT_ARGS_(_0, _8, _7, _6, _5, _4, _3, _2, _1, N, ...) N
#define JS_COUNT_ARGS(...) JS_COUNT_ARGS_(0, __VA_ARGS__, 8, 7, 6, 5, 4, 3, 2, 1, 0)

#define JS_CALLX1(F,X,a)               do { F(X,a); } while (0)
#define JS_CALLX2(F,X,a,b)             do { F(X,a); F(X,b); } while (0)
#define JS_CALLX3(F,X,a,b,c)           do { F(X,a); F(X,b); F(X,c); } while (0)
#define JS_CALLX4(F,X,a,b,c,d)         do { F(X,a); F(X,b); F(X,c); F(X,d); } while (0)
#define JS_CALLX5(F,X,a,b,c,d,e)       do { F(X,a); F(X,b); F(X,c); F(X,d); F(X,e); } while (0)
#define JS_CALLX6(F,X,a,b,c,d,e,f)     do { F(X,a); F(X,b); F(X,c); F(X,d); F(X,e); F(X,f); } while (0)
#define JS_CALLX7(F,X,a,b,c,d,e,f,g)   do { F(X,a); F(X,b); F(X,c); F(X,d); F(X,e); F(X,f); F(X,g); } while (0)
#define JS_CALLX8(F,X,a,b,c,d,e,f,g,h) do { F(X,a); F(X,b); F(X,c); F(X,d); F(X,e); F(X,f); F(X,g); F(X,h); } while (0)

#define JS_CALLX__(F, X, N, ...) JS_CALLX##N(F, X, __VA_ARGS__)
#define JS_CALLX_(F, X, N, ...) JS_CALLX__(F, X, N, __VA_ARGS__)

#undef QUICKJS_NG_CC_GNULIKE
#undef QUICKJS_NG_PLAT_WIN32

typedef struct JSRuntime JSRuntime;
typedef struct JSContext JSContext;
typedef struct JSObject JSObject;
typedef struct JSClass JSClass;
typedef uint32_t JSClassID;
typedef uint32_t JSAtom;

/* THE BUILT-IN CLASSES, IN THE SAME ID SPACE A HOST REGISTERS ITS OWN INTO. They are declared here rather than
   inside quickjs.c because JS_GetClassProto and JS_GetClassCtor are the public way to reach a realm's
   intrinsics, and a host that cannot NAME %DataView% cannot ask either of them for it — which left a browser
   component reproducing an intrinsic constructor in C rather than constructing it. The comment beside an entry
   is which arm of JSObject's union that class uses; that is the engine's own note and is why these are one
   list rather than a second copy of the interesting few. */
enum {
    /* classid tag        */    /* union usage   | properties */
    JS_CLASS_OBJECT = 1,        /* must be first */
    JS_CLASS_ARRAY,             /* u.array       | length */
    JS_CLASS_ERROR,
    JS_CLASS_NUMBER,            /* u.object_data */
    JS_CLASS_STRING,            /* u.object_data */
    JS_CLASS_BOOLEAN,           /* u.object_data */
    JS_CLASS_SYMBOL,            /* u.object_data */
    JS_CLASS_ARGUMENTS,         /* u.array       | length */
    JS_CLASS_MAPPED_ARGUMENTS,  /*               | length */
    JS_CLASS_DATE,              /* u.object_data */
    JS_CLASS_MODULE_NS,
    JS_CLASS_C_FUNCTION,        /* u.cfunc */
    JS_CLASS_BYTECODE_FUNCTION, /* u.func */
    JS_CLASS_BOUND_FUNCTION,    /* u.bound_function */
    JS_CLASS_C_FUNCTION_DATA,   /* u.c_function_data_record */
    JS_CLASS_C_CLOSURE,         /* u.c_closure_record */
    JS_CLASS_GENERATOR_FUNCTION, /* u.func */
    JS_CLASS_FOR_IN_ITERATOR,   /* u.for_in_iterator */
    JS_CLASS_REGEXP,            /* u.regexp */
    JS_CLASS_ARRAY_BUFFER,      /* u.array_buffer */
    JS_CLASS_SHARED_ARRAY_BUFFER, /* u.array_buffer */
    JS_CLASS_UINT8C_ARRAY,      /* u.array (typed_array) */
    JS_CLASS_INT8_ARRAY,        /* u.array (typed_array) */
    JS_CLASS_UINT8_ARRAY,       /* u.array (typed_array) */
    JS_CLASS_INT16_ARRAY,       /* u.array (typed_array) */
    JS_CLASS_UINT16_ARRAY,      /* u.array (typed_array) */
    JS_CLASS_INT32_ARRAY,       /* u.array (typed_array) */
    JS_CLASS_UINT32_ARRAY,      /* u.array (typed_array) */
    JS_CLASS_BIG_INT64_ARRAY,   /* u.array (typed_array) */
    JS_CLASS_BIG_UINT64_ARRAY,  /* u.array (typed_array) */
    JS_CLASS_FLOAT16_ARRAY,     /* u.array (typed_array) */
    JS_CLASS_FLOAT32_ARRAY,     /* u.array (typed_array) */
    JS_CLASS_FLOAT64_ARRAY,     /* u.array (typed_array) */
    JS_CLASS_DATAVIEW,          /* u.typed_array */
    JS_CLASS_BIG_INT,           /* u.object_data */
    JS_CLASS_MAP,               /* u.map_state */
    JS_CLASS_SET,               /* u.map_state */
    JS_CLASS_WEAKMAP,           /* u.map_state */
    JS_CLASS_WEAKSET,           /* u.map_state */
    JS_CLASS_ITERATOR,
    JS_CLASS_ITERATOR_CONCAT,   /* u.iterator_concat_data */
    JS_CLASS_ITERATOR_ZIP,      /* u.iterator_zip_data */
    JS_CLASS_ITERATOR_HELPER,   /* u.iterator_helper_data */
    JS_CLASS_ITERATOR_WRAP,     /* u.iterator_wrap_data */
    JS_CLASS_MAP_ITERATOR,      /* u.map_iterator_data */
    JS_CLASS_SET_ITERATOR,      /* u.map_iterator_data */
    JS_CLASS_ARRAY_ITERATOR,    /* u.array_iterator_data */
    JS_CLASS_STRING_ITERATOR,   /* u.array_iterator_data */
    JS_CLASS_REGEXP_STRING_ITERATOR,   /* u.regexp_string_iterator_data */
    JS_CLASS_GENERATOR,         /* u.generator_data */
    JS_CLASS_DISPOSABLE_STACK,
    JS_CLASS_PROXY,             /* u.proxy_data */
    JS_CLASS_PROMISE,           /* u.promise_data */
    JS_CLASS_PROMISE_RESOLVE_FUNCTION,  /* u.promise_function_data */
    JS_CLASS_PROMISE_REJECT_FUNCTION,   /* u.promise_function_data */
    JS_CLASS_ASYNC_FUNCTION,            /* u.func */
    JS_CLASS_ASYNC_FUNCTION_RESOLVE,    /* u.async_function_data */
    JS_CLASS_ASYNC_FUNCTION_REJECT,     /* u.async_function_data */
    JS_CLASS_ASYNC_FROM_SYNC_ITERATOR,  /* u.async_from_sync_iterator_data */
    JS_CLASS_ASYNC_GENERATOR_FUNCTION,  /* u.func */
    JS_CLASS_ASYNC_GENERATOR,   /* u.async_generator_data */
    JS_CLASS_ASYNC_DISPOSABLE_STACK,
    JS_CLASS_WEAK_REF,
    JS_CLASS_FINALIZATION_REGISTRY,
    JS_CLASS_DOM_EXCEPTION,
    JS_CLASS_CALL_SITE,
    JS_CLASS_RAWJSON,
    JS_CLASS_SHADOW_REALM,      /* u.shadow_realm_data */
    JS_CLASS_WRAPPED_FUNCTION,  /* u.wrapped_function_data */

    JS_CLASS_INIT_COUNT, /* last entry for predefined classes */
};

/* Unless documented otherwise, C string pointers (`char *` or `const char *`)
   are assumed to verify these constraints:
   - unless a length is passed separately, the string has a null terminator
   - string contents is either pure ASCII or is UTF-8 encoded.
 */

/* Overridable purely for testing purposes; don't touch. */
#ifndef JS_NAN_BOXING
#if INTPTR_MAX < INT64_MAX
#define JS_NAN_BOXING 1 /* Use NAN boxing for 32bit builds. */
#endif
#endif

enum {
    /* all tags with a reference count are negative */
    JS_TAG_FIRST       = -9, /* first negative tag */
    JS_TAG_BIG_INT     = -9,
    JS_TAG_SYMBOL      = -8,
    JS_TAG_STRING      = -7,
    JS_TAG_STRING_ROPE = -6,
    JS_TAG_MODULE      = -3, /* used internally */
    JS_TAG_FUNCTION_BYTECODE = -2, /* used internally */
    JS_TAG_OBJECT      = -1,

    JS_TAG_INT         = 0,
    JS_TAG_BOOL        = 1,
    JS_TAG_NULL        = 2,
    JS_TAG_UNDEFINED   = 3,
    JS_TAG_UNINITIALIZED = 4,
    JS_TAG_CATCH_OFFSET = 5,
    JS_TAG_EXCEPTION   = 6,
    JS_TAG_SHORT_BIG_INT = 7,
    JS_TAG_FLOAT64     = 8,
    /* any larger tag is FLOAT64 if JS_NAN_BOXING */
};

#if !defined(JS_CHECK_JSVALUE)
#define JSValueConst JSValue
#endif

// JS_CHECK_JSVALUE build mode does not produce working code but is here to
// help catch reference counting bugs at compile time, by making it harder
// to mix up JSValue and JSValueConst
//
// rules:
//
// - a function with a JSValue parameter takes ownership;
//   caller must *not* call JS_FreeValue
//
// - a function with a JSValueConst parameter does not take ownership;
//   caller *must* call JS_FreeValue
//
// - a function returning a JSValue transfers ownership to caller;
//   caller *must* call JS_FreeValue
//
// - a function returning a JSValueConst does *not* transfer ownership;
//   caller must *not* call JS_FreeValue
#if defined(JS_CHECK_JSVALUE)

typedef struct JSValue *JSValue;
typedef const struct JSValue *JSValueConst;

#define JS_MKVAL(tag, val)       ((JSValue)((tag) | (intptr_t)(val) << 4))
#define JS_MKPTR(tag, ptr)       ((JSValue)((tag) | (intptr_t)(ptr)))
#define JS_VALUE_GET_NORM_TAG(v) ((int)((intptr_t)(v) & 15))
#define JS_VALUE_GET_TAG(v)      ((int)((intptr_t)(v) & 15))
#define JS_VALUE_GET_SHORT_BIG_INT(v) JS_VALUE_GET_INT(v)
#define JS_VALUE_GET_PTR(v)      ((void *)((intptr_t)(v) & ~15))
#define JS_VALUE_GET_INT(v)      ((int)((intptr_t)(v) >> 4))
#define JS_VALUE_GET_BOOL(v)     ((int)((intptr_t)(v) >> 4))
#define JS_VALUE_GET_FLOAT64(v)  ((double)((intptr_t)(v) >> 4))
#define JS_TAG_IS_FLOAT64(tag)   ((int)(tag) == JS_TAG_FLOAT64)
#define JS_NAN                   JS_MKVAL(JS_TAG_FLOAT64, 0)

static inline JSValue __JS_NewFloat64(double d)
{
    return JS_MKVAL(JS_TAG_FLOAT64, (int)d);
}

static inline bool JS_VALUE_IS_NAN(JSValue v)
{
    (void)&v;
    return false;
}

#elif defined(JS_NAN_BOXING) && JS_NAN_BOXING

typedef uint64_t JSValue;

#define JS_VALUE_GET_TAG(v) (int)((v) >> 32)
#define JS_VALUE_GET_INT(v) (int)(v)
#define JS_VALUE_GET_BOOL(v) (int)(v)
#define JS_VALUE_GET_SHORT_BIG_INT(v) (int)(v)
#define JS_VALUE_GET_PTR(v) (void *)(intptr_t)(v)

#define JS_MKVAL(tag, val) (((uint64_t)(tag) << 32) | (uint32_t)(val))
#define JS_MKPTR(tag, ptr) (((uint64_t)(tag) << 32) | (uintptr_t)(ptr))

#define JS_FLOAT64_TAG_ADDEND (0x7ff80000 - JS_TAG_FIRST + 1) /* quiet NaN encoding */

static inline double JS_VALUE_GET_FLOAT64(JSValue v)
{
    union {
        JSValue v;
        double d;
    } u;
    u.v = v;
    u.v += (uint64_t)JS_FLOAT64_TAG_ADDEND << 32;
    return u.d;
}

#define JS_NAN (0x7ff8000000000000 - ((uint64_t)JS_FLOAT64_TAG_ADDEND << 32))

static inline JSValue __JS_NewFloat64(double d)
{
    union {
        double d;
        uint64_t u64;
    } u;
    JSValue v;
    u.d = d;
    /* normalize NaN */
    if ((u.u64 & 0x7fffffffffffffff) > 0x7ff0000000000000)
        v = JS_NAN;
    else
        v = u.u64 - ((uint64_t)JS_FLOAT64_TAG_ADDEND << 32);
    return v;
}

#define JS_TAG_IS_FLOAT64(tag) ((unsigned)((tag) - JS_TAG_FIRST) >= (JS_TAG_FLOAT64 - JS_TAG_FIRST))

/* same as JS_VALUE_GET_TAG, but return JS_TAG_FLOAT64 with NaN boxing */
static inline int JS_VALUE_GET_NORM_TAG(JSValue v)
{
    uint32_t tag;
    tag = JS_VALUE_GET_TAG(v);
    if (JS_TAG_IS_FLOAT64(tag))
        return JS_TAG_FLOAT64;
    else
        return tag;
}

static inline bool JS_VALUE_IS_NAN(JSValue v)
{
    uint32_t tag;
    tag = JS_VALUE_GET_TAG(v);
    return tag == (JS_NAN >> 32);
}

#else /* !JS_NAN_BOXING */

typedef union JSValueUnion {
    int32_t int32;
    double float64;
    void *ptr;
    int32_t short_big_int;
} JSValueUnion;

typedef struct JSValue {
    JSValueUnion u;
    int64_t tag;
} JSValue;

#define JS_VALUE_GET_TAG(v) ((int32_t)(v).tag)
/* same as JS_VALUE_GET_TAG, but return JS_TAG_FLOAT64 with NaN boxing */
#define JS_VALUE_GET_NORM_TAG(v) JS_VALUE_GET_TAG(v)
#define JS_VALUE_GET_INT(v) ((v).u.int32)
#define JS_VALUE_GET_BOOL(v) ((v).u.int32)
#define JS_VALUE_GET_FLOAT64(v) ((v).u.float64)
#define JS_VALUE_GET_SHORT_BIG_INT(v) ((v).u.short_big_int)
#define JS_VALUE_GET_PTR(v) ((v).u.ptr)

/* msvc doesn't understand designated initializers without /std:c++20 */
#ifdef __cplusplus
static inline JSValue JS_MKPTR(int64_t tag, void *ptr)
{
    JSValue v;
    v.u.ptr = ptr;
    v.tag = tag;
    return v;
}
static inline JSValue JS_MKVAL(int64_t tag, int32_t int32)
{
    JSValue v;
    v.u.int32 = int32;
    v.tag = tag;
    return v;
}
static inline JSValue JS_MKNAN(void)
{
    JSValue v;
    v.u.float64 = NAN;
    v.tag = JS_TAG_FLOAT64;
    return v;
}
/* provide as macros for consistency and backward compat reasons */
#define JS_MKPTR(tag, ptr) JS_MKPTR(tag, ptr)
#define JS_MKVAL(tag, val) JS_MKVAL(tag, val)
#define JS_NAN             JS_MKNAN() /* alas, not a constant expression */
#else
#define JS_MKPTR(tag, p)   (JSValue){ (JSValueUnion){ .ptr = p }, tag }
#define JS_MKVAL(tag, val) (JSValue){ (JSValueUnion){ .int32 = val }, tag }
#define JS_NAN             (JSValue){ (JSValueUnion){ .float64 = NAN }, JS_TAG_FLOAT64 }
#endif

#define JS_TAG_IS_FLOAT64(tag) ((unsigned)(tag) == JS_TAG_FLOAT64)

static inline JSValue __JS_NewFloat64(double d)
{
    JSValue v;
    v.tag = JS_TAG_FLOAT64;
    v.u.float64 = d;
    return v;
}

static inline bool JS_VALUE_IS_NAN(JSValue v)
{
    union {
        double d;
        uint64_t u64;
    } u;
    if (v.tag != JS_TAG_FLOAT64)
        return 0;
    u.d = v.u.float64;
    return (u.u64 & 0x7fffffffffffffff) > 0x7ff0000000000000;
}

#endif /* !JS_NAN_BOXING */

#define JS_VALUE_IS_BOTH_INT(v1, v2) ((JS_VALUE_GET_TAG(v1) | JS_VALUE_GET_TAG(v2)) == 0)
#define JS_VALUE_IS_BOTH_FLOAT(v1, v2) (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(v1)) && JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(v2)))

#define JS_VALUE_HAS_REF_COUNT(v) ((unsigned)JS_VALUE_GET_TAG(v) >= (unsigned)JS_TAG_FIRST)

/* special values */
#define JS_NULL      JS_MKVAL(JS_TAG_NULL, 0)
#define JS_UNDEFINED JS_MKVAL(JS_TAG_UNDEFINED, 0)
#define JS_FALSE     JS_MKVAL(JS_TAG_BOOL, 0)
#define JS_TRUE      JS_MKVAL(JS_TAG_BOOL, 1)
#define JS_EXCEPTION JS_MKVAL(JS_TAG_EXCEPTION, 0)
#define JS_UNINITIALIZED JS_MKVAL(JS_TAG_UNINITIALIZED, 0)

/* flags for object properties */
#define JS_PROP_CONFIGURABLE  (1 << 0)
#define JS_PROP_WRITABLE      (1 << 1)
#define JS_PROP_ENUMERABLE    (1 << 2)
#define JS_PROP_C_W_E         (JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE | JS_PROP_ENUMERABLE)
#define JS_PROP_LENGTH        (1 << 3) /* used internally in Arrays */
#define JS_PROP_TMASK         (3 << 4) /* mask for NORMAL, GETSET, VARREF, AUTOINIT */
#define JS_PROP_NORMAL         (0 << 4)
#define JS_PROP_GETSET         (1 << 4)
#define JS_PROP_VARREF         (2 << 4) /* used internally */
#define JS_PROP_AUTOINIT       (3 << 4) /* used internally */

/* flags for JS_DefineProperty */
#define JS_PROP_HAS_SHIFT        8
#define JS_PROP_HAS_CONFIGURABLE (1 << 8)
#define JS_PROP_HAS_WRITABLE     (1 << 9)
#define JS_PROP_HAS_ENUMERABLE   (1 << 10)
#define JS_PROP_HAS_GET          (1 << 11)
#define JS_PROP_HAS_SET          (1 << 12)
#define JS_PROP_HAS_VALUE        (1 << 13)

/* throw an exception if false would be returned
   (JS_DefineProperty/JS_SetProperty) */
#define JS_PROP_THROW            (1 << 14)
/* throw an exception if false would be returned in strict mode
   (JS_SetProperty) */
#define JS_PROP_THROW_STRICT     (1 << 15)

#define JS_PROP_NO_ADD           (1 << 16) /* internal use */
#define JS_PROP_NO_EXOTIC        (1 << 17) /* internal use */
#define JS_PROP_DEFINE_PROPERTY  (1 << 18) /* internal use */
#define JS_PROP_REFLECT_DEFINE_PROPERTY (1 << 19) /* internal use */

#ifndef JS_DEFAULT_STACK_SIZE
#define JS_DEFAULT_STACK_SIZE (1024 * 1024)
#endif

/* JS_Eval() flags */
#define JS_EVAL_TYPE_GLOBAL   (0 << 0) /* global code (default) */
#define JS_EVAL_TYPE_MODULE   (1 << 0) /* module code */
#define JS_EVAL_TYPE_DIRECT   (2 << 0) /* direct call (internal use) */
#define JS_EVAL_TYPE_INDIRECT (3 << 0) /* indirect call (internal use) */
#define JS_EVAL_TYPE_MASK     (3 << 0)

#define JS_EVAL_FLAG_STRICT   (1 << 3) /* force 'strict' mode */
/* INTERNAL: PARSE THE WHOLE SOURCE AS ONE PARENTHESISED FunctionExpression, PINNED TO END OF INPUT — see
   js_parse_fn_ctor_source. It has exactly two consumers and they are different questions, so read the one you
   are asking for:
     (1) THE PARSE. `s->fn_ctor_toplevel` routes the top-level parse to that entry, which is what §20.2.1.1.1
   CreateDynamicFunction step 23's goal-symbol parse needs and what HTML §8.1.8.1 "Event handlers" step 3.7
   needs to decide whether a handler body is parsable as a FunctionBody alone. Without it a Script parse
   accepts a body that CLOSES the wrapper and the engine runs what follows.
     (2) EVAL ORIGIN. `fd->from_eval` is `(eval_type == DIRECT || INDIRECT) && !FUNCTION_CTOR`: the source is
   the FUNCTION CONSTRUCTOR's body, not an eval — `new Function("...")` creates a function whose code has no
   eval origin, and CallSite#isEval must say so. The bit was `JS_EVAL_FLAG_UNUSED` and got its name from this
   half, which is why the name says nothing about (1).
     AT JS_EVAL_TYPE_GLOBAL, (2) IS ALREADY DECIDED BY THE TYPE — from_eval is false there whatever this flag
   says — so a GLOBAL compile carrying this flag is asking for the pin and for nothing else. That is what makes
   it usable by an algorithm that is neither an eval nor the Function constructor, without claiming to be one.
   THIS COMMENT USED TO OPEN "the source is the FUNCTION CONSTRUCTOR's body, not an eval" and stop there, which
   was true of the only caller there was and hid (1) entirely — an under-claim, so the reader who needed the
   pin was told there was nothing here for them. */
#define JS_EVAL_FLAG_FUNCTION_CTOR (1 << 4)
/* compile but do not run. The result is an object with a
   JS_TAG_FUNCTION_BYTECODE or JS_TAG_MODULE tag. It can be executed
   with JS_EvalFunction(). */
#define JS_EVAL_FLAG_COMPILE_ONLY (1 << 5)
/* don't include the stack frames before this eval in the Error() backtraces */
#define JS_EVAL_FLAG_BACKTRACE_BARRIER (1 << 6)
/* allow top-level await in normal script. JS_Eval() returns a
   promise. Only allowed with JS_EVAL_TYPE_GLOBAL */
#define JS_EVAL_FLAG_ASYNC (1 << 7)
/* Return the compiled program as a CLOSURE instead of calling it, so the caller can dispatch it on the trampoline
   chain. Without this the program body runs in its own activation off the chain, and a loop inside it cannot park
   for the scheduler (gen_state != flow base) — which is why JS_EvalFunctionInternal DFAILs rather than running one.
   IT IS A STATEMENT ABOUT EXECUTION MECHANICS AND NOTHING ELSE — who runs the body — and in particular it is NOT a
   claim that the source is page-supplied text an eval is turning into code. That question is asked of the eval TYPE
   (see js_eval_is_string_to_code_sink), because ECMAScript §19.2.1.2 HostEnsureCanCompileStrings is performed by
   exactly two clauses, §19.2.1.1 PerformEval step 5 and §20.2.1.1.1 CreateDynamicFunction step 11, and those are
   JS_EVAL_TYPE_DIRECT and JS_EVAL_TYPE_INDIRECT. The two were conflated once, and the cost was that HTML §8.1.4.4
   "Calling scripts"'s run a classic script — a JS_EVAL_TYPE_GLOBAL program that announces no sink at all — could
   not ask for a trampolinable closure without asserting it was an eval. So this flag is available to an embedder
   compiling a `<script>` element's program, and JS_FlowNew is the same request made for a whole flow. */
#define JS_EVAL_FLAG_TRAMP_CLOSURE (1 << 8)
/* THIS PROGRAM'S SOURCE TEXT ARRIVED IN THE DOCUMENT'S OWN RESPONSE — an INLINE `<script>`. HTML §4.12.1 "The
   script element" splits a document's programs by exactly one attribute: `src` "denotes that instead of using
   the element's child text content as the script content, the script will be fetched from the specified URL",
   and the section's own conformance table names the halves "Inline classic scripts" and "External classic
   scripts". The split matters because the two halves have different SESSION-VARIANCE: the document is rendered
   per request, against this visitor's credentials, while a subresource bundle at a content-addressed URL is
   byte-identical for every visitor — which is the premise this whole engine rests on, that a logged-out visit
   is served the same bundle containing the auth and admin code that never runs.
   It rides down the whole nest exactly as eval-ness does, because it is a property of the SCRIPT and not of a
   function: a function declared inside an inline script is inline-script code too, and a DIRECT eval inherits
   it from its caller. Its ONE consumer is the object allocator, which stamps the records this half of the
   document builds so that JSConcolicHooks.publish can tell them from the bundle's own. */
#define JS_EVAL_FLAG_INLINE_SCRIPT (1 << 9)

typedef JSValue JSCFunction(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
typedef JSValue JSCFunctionMagic(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic);
typedef JSValue JSCFunctionData(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValueConst *func_data);
typedef JSValue JSCClosure(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, void *opaque);

typedef struct JSMallocFunctions {
    void *(*js_calloc)(void *opaque, size_t count, size_t size);
    void *(*js_malloc)(void *opaque, size_t size);
    void (*js_free)(void *opaque, void *ptr);
    void *(*js_realloc)(void *opaque, void *ptr, size_t size);
    size_t (*js_malloc_usable_size)(const void *ptr);
} JSMallocFunctions;

// Debug trace system: the debug output will be produced to the dump stream (currently
// stdout) if dumps are enabled and JS_SetDumpFlags is invoked with the corresponding
// bit set.
#define JS_DUMP_BYTECODE_FINAL   0x01  /* dump pass 3 final byte code */
#define JS_DUMP_BYTECODE_PASS2   0x02  /* dump pass 2 code */
#define JS_DUMP_BYTECODE_PASS1   0x04  /* dump pass 1 code */
#define JS_DUMP_BYTECODE_HEX     0x10  /* dump bytecode in hex */
#define JS_DUMP_BYTECODE_PC2LINE 0x20  /* dump line number table */
#define JS_DUMP_BYTECODE_STACK   0x40  /* dump compute_stack_size */
#define JS_DUMP_BYTECODE_STEP    0x80  /* dump executed bytecode */
#define JS_DUMP_READ_OBJECT     0x100  /* dump the marshalled objects at load time */
#define JS_DUMP_FREE            0x200  /* dump every object free */
#define JS_DUMP_GC              0x400  /* dump the occurrence of the automatic GC */
#define JS_DUMP_GC_FREE         0x800  /* dump objects freed by the GC */
#define JS_DUMP_MODULE_RESOLVE 0x1000  /* dump module resolution steps */
#define JS_DUMP_PROMISE        0x2000  /* dump promise steps */
#define JS_DUMP_LEAKS          0x4000  /* dump leaked objects and strings in JS_FreeRuntime */
/* 0x8000 WAS JS_DUMP_ATOM_LEAKS, and it is gone rather than kept as a name. The atom walk it gated is
   unconditional now, for the reason stated at the gc_obj_list census it stands beside: a leak report that
   exists only in some builds reports only some leaks, and no host in this tree ever set this bit — so every
   leaked interned string and private Symbol was invisible in every run. A flag whose only reader has been
   deleted is a setting a caller can turn on to no effect, which is the same concealment as a default over a
   field nobody writes. JS_ABORT_ON_LEAKS drops the bit with it. */
#define JS_DUMP_MEM           0x10000  /* dump memory usage in JS_FreeRuntime */
#define JS_DUMP_OBJECTS       0x20000  /* dump objects in JS_FreeRuntime */
#define JS_DUMP_ATOMS         0x40000  /* dump atoms in JS_FreeRuntime */
#define JS_DUMP_SHAPES        0x80000  /* dump shapes in JS_FreeRuntime */
/* Abort on what JS_FreeRuntime's leak reports found. This is the RELEASE-build verdict: in dev the object,
   realm, step-machine and atom censuses each carry their own DCHECK, which aborts at the report naming what
   leaked, and those are compiled out at APICLIENT_DEV=0. check_dump_flag tests ALL the bits it is given, so
   this asks for the abort bit and JS_DUMP_LEAKS together. */
#define JS_ABORT_ON_LEAKS    0x104000

// Finalizers run in LIFO order at the very end of JS_FreeRuntime.
// Intended for cleanup of associated resources; the runtime itself
// is no longer usable.
typedef void JSRuntimeFinalizer(JSRuntime *rt, void *arg);

typedef struct JSGCObjectHeader JSGCObjectHeader;

JS_EXTERN JSRuntime *JS_NewRuntime(void);
/* info lifetime must exceed that of rt */
JS_EXTERN void JS_SetRuntimeInfo(JSRuntime *rt, const char *info);
/* use 0 to disable memory limit */
JS_EXTERN void JS_SetMemoryLimit(JSRuntime *rt, size_t limit);
JS_EXTERN void JS_SetDumpFlags(JSRuntime *rt, uint64_t flags);
JS_EXTERN uint64_t JS_GetDumpFlags(JSRuntime *rt);
JS_EXTERN size_t JS_GetGCThreshold(JSRuntime *rt);
JS_EXTERN void JS_SetGCThreshold(JSRuntime *rt, size_t gc_threshold);
/* use 0 to disable maximum stack size check */
JS_EXTERN void JS_SetMaxStackSize(JSRuntime *rt, size_t stack_size);
/* should be called when changing thread to update the stack top value
   used to check stack overflow. */
JS_EXTERN void JS_UpdateStackTop(JSRuntime *rt);
JS_EXTERN JSRuntime *JS_NewRuntime2(const JSMallocFunctions *mf, void *opaque);
JS_EXTERN void JS_FreeRuntime(JSRuntime *rt);
JS_EXTERN void *JS_GetRuntimeOpaque(JSRuntime *rt);
JS_EXTERN void JS_SetRuntimeOpaque(JSRuntime *rt, void *opaque);
JS_EXTERN int JS_AddRuntimeFinalizer(JSRuntime *rt,
                                     JSRuntimeFinalizer *finalizer, void *arg);
typedef void JS_MarkFunc(JSRuntime *rt, JSGCObjectHeader *gp);
JS_EXTERN void JS_MarkValue(JSRuntime *rt, JSValueConst val,
                            JS_MarkFunc *mark_func);
JS_EXTERN void JS_RunGC(JSRuntime *rt);
JS_EXTERN bool JS_IsLiveObject(JSRuntime *rt, JSValueConst obj);

JS_EXTERN JSContext *JS_NewContext(JSRuntime *rt);
JS_EXTERN void JS_FreeContext(JSContext *s);
JS_EXTERN JSContext *JS_DupContext(JSContext *ctx);
/* IS THIS EXCEPTION THE RUNTIME'S OWN OUT OF MEMORY? A pointer comparison against the singleton the throw
   hands out — it runs none of the page's code, which is what lets a C activation ask at all. */
JS_EXTERN bool JS_IsOutOfMemoryError(JSContext *ctx, JSValueConst v);

/* A REALM IS BEING TORN DOWN — the host's chance to release what it hung off that realm, called for EVERY
 * realm of the runtime once its last reference is gone and before the realm releases any of its own.
 *
 * IT IS WHERE PER-REALM HOST STATE DIES because it is the only place that can be: a realm's last reference is
 * normally released by the COLLECTOR (every C function object in a realm holds a counted reference to it, so a
 * realm nothing points at is a cycle), which means no host call ever precedes it and no host list can be sure
 * it still names a live realm. The hook may release JSValues and free its own records; it must not assume the
 * collector is idle, since it can be running inside one.
 * A realm that never held any host state is handed over too — a hook that had to be told which realms matter
 * would be that list of them, kept in the place this exists to make unnecessary. */
typedef void JSContextTeardownFunc(JSRuntime *rt, JSContext *ctx);
JS_EXTERN void JS_SetContextTeardownHook(JSRuntime *rt, JSContextTeardownFunc *cb);

/* THE COUNTED REFERENCES A HOST HUNG OFF A REALM, DECLARED TO THE COLLECTOR — the other half of the hook above,
 * and the half without which that one can never fire.
 *
 * A host record reached through JS_GetContextOpaque is not a GC object, so JS_MarkContext walks a realm without
 * ever naming what the record holds. Every reference in it is then one gc_decref cannot subtract, so each object
 * it names keeps a refcount nothing in the heap accounts for, reads as EXTERNALLY ROOTED, and gc_scan revives
 * the whole graph behind it. When one of those references points back INTO the realm — a Window, its
 * WindowProxy, its `document` — the realm is a cycle with one edge the collector cannot see and is uncollectable
 * for the life of the runtime: the teardown hook that would release the record fires only when the realm dies,
 * and the record is exactly what stops it dying. That is not a slow leak, it is a permanent one, and it is
 * unbounded in a host that mints a realm per navigable.
 *
 * MEASURED, WHICH IS WHY THIS EXISTS RATHER THAN A NOTE SAYING IT SHOULD: every child navigable's realm in
 * web-platform-tests' html/browsers survived to JS_FreeRuntime, whose gc_obj_list walk then reported the entire
 * page — Window, document, every function object — as a leak with nothing in the report naming an owner. It was
 * the single largest abort cause in that directory, above every spec failure in it.
 *
 * THE HOOK REPORTS EXACTLY WHAT THE RECORD OWNS, ONCE EACH: the same list its teardown releases, which is why a
 * host writes the two as ONE list with two consumers. Marking a reference twice, or marking one the record does
 * not own, makes gc_decref over-subtract and frees an object that is still live — the failure mode is a
 * use-after-free rather than a leak, so the list is not a place to be generous. It is called during a collection
 * (from gc_decref and again from gc_scan) and must do nothing but report. */
typedef void JSContextMarkFunc(JSRuntime *rt, JSContext *ctx, JS_MarkFunc *mark_func);
JS_EXTERN void JS_SetContextMarkHook(JSRuntime *rt, JSContextMarkFunc *cb);

/* THE DECLARED HOOK, so a host can state at the birth of a record that the collector will read it — the same
 * reason JS_ContextRefCount is exposed, and not a decision input either. It exists because this file CANNOT
 * make that assertion itself: whether a record holds counted references OF THIS RUNTIME is known only to the
 * host that shaped it (run-test262's agent record holds the parent runtime's values and correctly declares no
 * hook), so an assert at JS_SetContextOpaque would fire on a record that is perfectly well formed. */
JS_EXTERN JSContextMarkFunc *JS_GetContextMarkHook(JSRuntime *rt);

/* A REALM'S REFERENCE COUNT, for stating an ownership invariant where one is being handed over. Not a decision
 * input — a host that branches on this is guessing at a lifetime the collector owns. */
JS_EXTERN int JS_ContextRefCount(JSContext *ctx);

/* WHICH EDGES THAT COUNT IS MADE OF — the same reading broken down by the FUNCTION that took each reference,
 * under the same restriction: it STATES who holds a realm and decides nothing.
 *
 * IT EXISTS BECAUSE THE COUNT ALONE NAMES NOBODY. A host census can report that every live child realm is held
 * by some thousands of references and cannot name one of them, so a reader who wants to give a realm back has
 * nothing to aim at and no way to score a repair: an edge removed and an edge never taken read alike in a
 * total. These three make that total an addition of parts a reader can enumerate.
 *
 * THE ORIGIN IS A FUNCTION NAME AND IT IS DERIVED, not a list anyone keeps: every increment of a realm's count
 * goes through one function, which inside quickjs.c is spelled so that the CALLER's `__func__` comes with it.
 * A take site added later rows up under its own name with nothing to edit; the exported JS_DupContext is
 * outside that spelling and rows up as `JS_DupContext_exported`, which is how a caller in another translation
 * unit makes itself visible instead of hiding inside somebody else's row.
 *
 * `taken` IS GROSS AND JS_ContextRefReleased IS NOT ATTRIBUTED. A release is charged to no origin, because
 * charging one means storing the origin WITH each reference and a realm's references live in places that have
 * no room for it. So the three reconcile as `1 + the sum of taken - released == JS_ContextRefCount(ctx)` — an
 * identity rather than an expectation, asserted at every take and every release — and a caller that publishes
 * the breakdown publishes `released` beside it so a reader can perform the same addition. Where `released` is
 * 0 the breakdown IS the live attribution; where it is not, it is the gross takes and says so.
 *
 * A BUILD WITH NO ATTRIBUTION ANSWERS -1, from the count and from `taken`. Zero is a population a realm can
 * genuinely have, so "this build does not watch references" would be indistinguishable from "nothing holds
 * this realm"; -1 is a value neither can take, which makes the absence a positive statement. In such a build
 * JS_ContextRefSite answers NULL and must not be called at all — the count of -1 is what says so.
 *
 * `i` IS IN [0, JS_ContextRefSiteCount(ctx)) AND THE ORDER IS ARRIVAL: the row a realm opened first comes
 * first, which is stable within one realm and is NOT comparable between two. The name returned is a literal of
 * the engine's own image and outlives the realm; `taken` may be NULL. */
JS_EXTERN int JS_ContextRefSiteCount(JSContext *ctx);
JS_EXTERN const char *JS_ContextRefSite(JSContext *ctx, int i, int *taken);
JS_EXTERN int JS_ContextRefReleased(JSContext *ctx);

/* A REF-COUNTED VALUE'S REFERENCE COUNT, for the same purpose and under the same restriction as the realm's
 * above: STATING an ownership invariant, never deciding on one. A host that branches on this is guessing at a
 * lifetime the collector owns; a host that ASSERTS on it is saying, at the one moment the answer is decidable,
 * exactly who is allowed to be holding a value.
 *
 * MEASURED, like its sibling. An accessor that hands out an owned reference to internal state — IndexedDB
 * §5.5's list of database changes — had three callers and one of them dropped its reference without giving it
 * back. The value was an empty Array, so the entire consequence was invisible in every census except one: an
 * Array holds Array.prototype, Array.prototype holds the realm's function objects, and each of those holds the
 * REALM, so one leaked reference to an empty Array made a whole browser immortal (2612 Functions, 408 shapes,
 * a JSContext at refcount 3108) and the leak walk named nothing but three anonymous Arrays. The assert this
 * exists for fires at the transaction that dropped it instead.
 *
 * The value must be one that HAS a reference count — a number, a boolean and a short string are copied rather
 * than shared, so an invariant stated over one is a statement about nothing, and asking is asserted against. */
JS_EXTERN int JS_ValueRefCount(JSValueConst v);

JS_EXTERN void *JS_GetContextOpaque(JSContext *ctx);
JS_EXTERN void JS_SetContextOpaque(JSContext *ctx, void *opaque);
JS_EXTERN JSRuntime *JS_GetRuntime(JSContext *ctx);
JS_EXTERN void JS_SetClassProto(JSContext *ctx, JSClassID class_id, JSValue obj);
JS_EXTERN JSValue JS_GetClassProto(JSContext *ctx, JSClassID class_id);
/* THIS REALM'S INTRINSIC CONSTRUCTOR FOR A CLASS — the mirror of the two above, and the only sound way for C
   to reach the object a spec step names as `%DataView%`, `%Map%` or `%URL%`. It is not `JS_GetClassProto(id)`'s
   `constructor` property: that property is WRITABLE, so a page can redirect an internal `! Construct(…)` at
   anything it likes, and an engine that read it would be running the page's code where the standard says the
   step is infallible. And it is PER REALM for the same reason the prototype is — a C member runs in the realm
   that defined it (js_call_c_function takes `ctx` from the function object), so one remembered constructor
   would answer every document with the first realm's.
   The engine populates a class's slot wherever the class's constructor is DEFINED (JS_SetConstructor and every
   spelling that reaches it), so a host whose interface object goes through JS_SetConstructor has nothing to
   declare. JS_SetClassCtor is for a host that mints an interface object some other way; `obj` is CONSUMED.
   JS_GetClassCtor's answer is OWNED, and reading a slot this realm never populated is fatal in dev — a missing
   intrinsic is a realm that was built without it, never a null to test for. */
JS_EXTERN void JS_SetClassCtor(JSContext *ctx, JSClassID class_id, JSValue obj);
JS_EXTERN JSValue JS_GetClassCtor(JSContext *ctx, JSClassID class_id);
JS_EXTERN JSValue JS_GetFunctionProto(JSContext *ctx);

/* THE REALM A FUNCTION OBJECT ANSWERS FOR — ECMAScript §7.3.24 "GetFunctionRealm ( func )", the operation a
   spec step spells `? GetFunctionRealm(f)`. Step 1 answers a function that HAS a [[Realm]] slot with it; steps
   2 and 3 forward a bound function to its [[BoundTargetFunction]] and a Proxy to its [[ProxyTarget]], so
   `f.bind().bind()` and `new Proxy(new Proxy(f, h), h)` answer for `f`; step 4 falls back to `ctx`.
   IT IS EXPORTED BECAUSE A HOST STEP ASKS FOR IT BY NAME. HTML §3.2.3 "HTML element constructors" step 11.1 is
   `Let realm be ? GetFunctionRealm(NewTarget)`, and NewTarget is whatever the page passed to `Reflect.construct`
   or wrote after `new` — so the realm a custom element's fallback prototype comes from is the PAGE's choice and
   cannot be derived from the running one.
   THE ANSWER IS BORROWED, NOT OWNED: no reference is taken, exactly as `JS_GetRuntime` takes none. It is valid
   while the caller holds `func_obj`, which is what keeps the realm reachable — a function object references the
   realm it was minted in, so the realm cannot die under a live reference to a function that names it.
   NULL IS A THROW AND NOT AN ABSENCE — it is the `?` in the spec's own spelling. Step 3.a is
   `Perform ? ValidateNonRevokedProxy(func)` (ECMAScript §10.5.14), so a REVOKED Proxy anywhere along the chain
   raises a TypeError on `ctx` and answers NULL; every other input answers a realm. A caller propagates it.
   IT IS NOT WEB IDL'S "associated realm", and the difference is exactly steps 2 and 3. Web IDL §3.1
   "JavaScript environment" defines that term only "for non-exotic function objects (i.e. not callable proxies,
   and not bound functions)", where it is "the value of the function object's [[Realm]] internal slot" — this
   operation's step 1, and the same answer. For the two exotic kinds the standard defines NO association, so a
   caller reaching for Web IDL's term gets a defined answer here where its own standard gives none; that is a
   choice the caller is making and states, never a step it is performing. */
JS_EXTERN JSContext *JS_GetFunctionRealm(JSContext *ctx, JSValueConst func_obj);

/* the following functions are used to select the intrinsic object to
   save memory */
JS_EXTERN JSContext *JS_NewContextRaw(JSRuntime *rt);
JS_EXTERN int JS_AddIntrinsicBaseObjects(JSContext *ctx);
JS_EXTERN int JS_AddIntrinsicDate(JSContext *ctx);
JS_EXTERN int JS_AddIntrinsicEval(JSContext *ctx);
JS_EXTERN void JS_AddIntrinsicRegExpCompiler(JSContext *ctx);
JS_EXTERN int JS_AddIntrinsicRegExp(JSContext *ctx);
JS_EXTERN int JS_AddIntrinsicJSON(JSContext *ctx);
JS_EXTERN int JS_AddIntrinsicProxy(JSContext *ctx);
JS_EXTERN int JS_AddIntrinsicMapSet(JSContext *ctx);
JS_EXTERN int JS_AddIntrinsicTypedArrays(JSContext *ctx);
JS_EXTERN int JS_AddIntrinsicPromise(JSContext *ctx);
JS_EXTERN int JS_AddIntrinsicBigInt(JSContext *ctx);
JS_EXTERN int JS_AddIntrinsicWeakRef(JSContext *ctx);
JS_EXTERN int JS_AddIntrinsicShadowRealm(JSContext *ctx);
JS_EXTERN int JS_AddPerformance(JSContext *ctx);
JS_EXTERN int JS_AddIntrinsicDOMException(JSContext *ctx);
JS_EXTERN int JS_AddIntrinsicAToB(JSContext *ctx);

/* for equality comparisons and sameness */
JS_EXTERN int JS_IsEqual(JSContext *ctx, JSValueConst op1, JSValueConst op2);
JS_EXTERN bool JS_IsStrictEqual(JSContext *ctx, JSValueConst op1, JSValueConst op2);
JS_EXTERN bool JS_IsSameValue(JSContext *ctx, JSValueConst op1, JSValueConst op2);
/* Similar to same-value equality, but +0 and -0 are considered equal. */
JS_EXTERN bool JS_IsSameValueZero(JSContext *ctx, JSValueConst op1, JSValueConst op2);

/* Only used for running 262 tests. TODO(saghul) add build time flag. */
JS_EXTERN JSValue js_string_codePointRange(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv);

JS_EXTERN void *js_calloc_rt(JSRuntime *rt, size_t count, size_t size);
JS_EXTERN void *js_malloc_rt(JSRuntime *rt, size_t size);
JS_EXTERN void js_free_rt(JSRuntime *rt, void *ptr);
JS_EXTERN void *js_realloc_rt(JSRuntime *rt, void *ptr, size_t size);
/* WHATWG base64, the encoder half — the engine's own, so a host that has to put bytes on a text channel binds
   this rather than growing a second implementation of a codec the spec already made this engine implement. */
JS_EXTERN size_t JS_Base64EncodedSize(size_t len);
JS_EXTERN size_t JS_Base64Encode(char *dst, size_t dst_size, const uint8_t *src, size_t len);
JS_EXTERN size_t JS_Base64DecodedMax(size_t len);
JS_EXTERN size_t JS_Base64Decode(uint8_t *dst, size_t dst_size, const char *src, size_t len, int *err);

JS_EXTERN size_t js_malloc_usable_size_rt(JSRuntime *rt, const void *ptr);
JS_EXTERN void *js_mallocz_rt(JSRuntime *rt, size_t size);

JS_EXTERN void *js_calloc(JSContext *ctx, size_t count, size_t size);
JS_EXTERN void *js_malloc(JSContext *ctx, size_t size);
JS_EXTERN void js_free(JSContext *ctx, void *ptr);
JS_EXTERN void *js_realloc(JSContext *ctx, void *ptr, size_t size);
JS_EXTERN size_t js_malloc_usable_size(JSContext *ctx, const void *ptr);
JS_EXTERN void *js_realloc2(JSContext *ctx, void *ptr, size_t size, size_t *pslack);
JS_EXTERN void *js_mallocz(JSContext *ctx, size_t size);
JS_EXTERN char *js_strdup(JSContext *ctx, const char *str);
JS_EXTERN char *js_strndup(JSContext *ctx, const char *s, size_t n);

typedef struct JSMemoryUsage {
    int64_t malloc_size, malloc_limit, memory_used_size;
    int64_t malloc_count;
    int64_t memory_used_count;
    int64_t atom_count, atom_size;
    int64_t str_count, str_size;
    int64_t obj_count, obj_size;
    int64_t prop_count, prop_size;
    int64_t shape_count, shape_size;
    int64_t js_func_count, js_func_size, js_func_code_size;
    int64_t js_func_pc2line_count, js_func_pc2line_size;
    int64_t c_func_count, array_count;
    int64_t fast_array_count, fast_array_elements;
    int64_t binary_object_count, binary_object_size;
} JSMemoryUsage;

JS_EXTERN void JS_ComputeMemoryUsage(JSRuntime *rt, JSMemoryUsage *s);
JS_EXTERN void JS_DumpMemoryUsage(FILE *fp, const JSMemoryUsage *s, JSRuntime *rt);

/* atom support */
#define JS_ATOM_NULL 0

JS_EXTERN JSAtom JS_NewAtomLen(JSContext *ctx, const char *str, size_t len);
JS_EXTERN JSAtom JS_NewAtom(JSContext *ctx, const char *str);
JS_EXTERN JSAtom JS_NewAtomUInt32(JSContext *ctx, uint32_t n);
JS_EXTERN JSAtom JS_DupAtom(JSContext *ctx, JSAtom v);
JS_EXTERN JSAtom JS_DupAtomRT(JSRuntime *rt, JSAtom v);
JS_EXTERN void JS_FreeAtom(JSContext *ctx, JSAtom v);
JS_EXTERN void JS_FreeAtomRT(JSRuntime *rt, JSAtom v);
#define JS_FreeAtoms(ctx, ...) JS_CALLX_(JS_FreeAtom, ctx, JS_COUNT_ARGS(__VA_ARGS__), __VA_ARGS__)
#define JS_FreeAtomsRT(rt, ...) JS_CALLX_(JS_FreeAtomRT, rt, JS_COUNT_ARGS(__VA_ARGS__), __VA_ARGS__)
JS_EXTERN JSValue JS_AtomToValue(JSContext *ctx, JSAtom atom);
JS_EXTERN JSValue JS_AtomToString(JSContext *ctx, JSAtom atom);
JS_EXTERN const char *JS_AtomToCStringLen(JSContext *ctx, size_t *plen, JSAtom atom);
static inline const char *JS_AtomToCString(JSContext *ctx, JSAtom atom)
{
    return JS_AtomToCStringLen(ctx, NULL, atom);
}
/* §7.1.21 ToPropertyKey ( arg ) folded into an atom — the CONVERSION FAMILY, so it carries its call site for
   the reason stated in full at JS_ToStringAt below. */
JS_EXTERN JSAtom JS_ValueToAtomAt(JSContext *ctx, JSValueConst val, const char *file, int line);
#define JS_ValueToAtom(ctx, val) \
    JS_ValueToAtomAt((ctx), (val), __FILE__, __LINE__)

/* object class support */

typedef struct JSPropertyEnum {
    bool is_enumerable;
    JSAtom atom;
} JSPropertyEnum;

typedef struct JSPropertyDescriptor {
    int flags;
    JSValue value;
    JSValue getter;
    JSValue setter;
} JSPropertyDescriptor;

typedef struct JSClassExoticMethods {
    /* Return -1 if exception (can only happen in case of Proxy object),
       false if the property does not exists, true if it exists. If 1 is
       returned, the property descriptor 'desc' is filled if != NULL. */
    int (*get_own_property)(JSContext *ctx, JSPropertyDescriptor *desc,
                             JSValueConst obj, JSAtom prop);
    /* '*ptab' should hold the '*plen' property keys. Return 0 if OK,
       -1 if exception. The 'is_enumerable' field is ignored.
    */
    int (*get_own_property_names)(JSContext *ctx, JSPropertyEnum **ptab,
                                  uint32_t *plen, JSValueConst obj);
    /* return < 0 if exception, or true/false */
    int (*delete_property)(JSContext *ctx, JSValueConst obj, JSAtom prop);
    /* return < 0 if exception or true/false */
    int (*define_own_property)(JSContext *ctx, JSValueConst this_obj,
                               JSAtom prop, JSValueConst val,
                               JSValueConst getter, JSValueConst setter,
                               int flags);
    /* THE CLASS'S OWN [[GetPrototypeOf]] — for a class whose prototype is COMPUTED and therefore cannot be the
       link stored in the object's shape. Returns an OWNED Object, JS_NULL, or JS_EXCEPTION.
       WHAT FORCED IT: HTML §7.2.3.1 [[GetPrototypeOf]] ( ) answers `OrdinaryGetPrototypeOf(W)` where W is the
       WindowProxy's [[Window]] — the navigable's CURRENT active Window, which a navigation replaces — and null
       for a Window the reader may not see. A stored link cannot express that in this engine for a reason
       stronger than staleness: the binding is PER FLOW (it rides the host's copy-on-write delta), while a
       shape's proto is not captured by that delta, so one forked arm's navigation would rewrite the prototype
       every sibling arm reads.
       IT RUNS NONE OF THE PAGE'S CODE, and that is a CONTRACT rather than a hint: it is consulted from
       JS_GetPrototype, which C callers reach with no flow base under them to suspend into. A class whose
       prototype answer could reach a getter belongs on the keyed entry's GP_GETPROTO — the route a Proxy's
       `getPrototypeOf` trap takes — and not here.
       A CLASS THAT DECLARES IT HAS NO PROTOTYPE SLOT TO WRITE, so its [[SetPrototypeOf]] is ECMAScript
       §10.4.7.2 SetImmutablePrototype ( obj, proto ): JS_SetPrototypeInternal accepts the value this hook
       already answers and refuses every other. That is HTML §7.2.3.2 [[SetPrototypeOf]] ( V )'s own answer, and
       it FALLS OUT rather than being chosen — there is nowhere for a set to land. */
    JSValue (*get_prototype)(JSContext *ctx, JSValueConst obj);
    /* The following methods can be emulated with the previous ones,
       so they are usually not needed */
    /* return < 0 if exception or true/false */
    int (*has_property)(JSContext *ctx, JSValueConst obj, JSAtom atom);
    JSValue (*get_property)(JSContext *ctx, JSValueConst obj, JSAtom atom,
                            JSValueConst receiver);
    /* return < 0 if exception or true/false */
    int (*set_property)(JSContext *ctx, JSValueConst obj, JSAtom atom,
                        JSValueConst value, JSValueConst receiver, int flags);
    /* THE [[GetOwnProperty]] ABOVE CONSULTS ONLY THIS CLASS'S OWN STATE and can never reach the page's code.
       The accessor walk needs to be told, because it runs the hook FROM C with no flow base to park into: one
       that can reach the page must be routed onto the trampoline instead, and there is no way to tell the two
       apart by looking at the pointer. A class that says nothing is assumed to be the second kind, which is
       the safe direction — it asserts rather than runs. Web IDL's indexed property getter is the first kind
       by construction: an index lookup has no accessors in it. */
    bool get_own_property_no_user_code;
    /* THE OBJECT THIS ONE'S KEYED OPERATIONS ARE PERFORMED ON — a class that STANDS IN for another object,
       which is what HTML §7.2.3's same-origin branch makes a WindowProxy: every one of its internal methods
       is `W.[[X]](…)`, so `frame.contentWindow.onunload = f` writes the OTHER document's Window and not the
       stand-in.
       IT EXISTS FOR THE COW DELTA AND FOR NOTHING ELSE, which is why it is one question and not a second
       property protocol beside the four hooks above. Those hooks forward the OPERATION and the delta cannot
       see that they did: cow_capture runs at the head of the write, names the object the write was ADDRESSED
       to, and would record a baseline for the stand-in while the write lands on the object behind it — an
       entry whose unapply then puts that baseline back as a REAL own property of the stand-in, which from
       then on shadows the object it was standing in for, for every flow. So the capture asks the class where
       the write is going, once, at the one place every capture converges on.
       BORROWED (the stand-in holds it, and the delta's entry holds only the object it names). JS_UNINITIALIZED
       = this object stands for itself, which is every class that does not declare this.
       IT IS ASKED PER KEY, because a stand-in need not forward every one: HTML §7.2.3 "The WindowProxy exotic object"'s own surface is
       answered by the WindowProxy itself and only the rest is the Window's, and a capture that forwarded the
       first group would name the wrong object as surely as one that forwarded none. It runs NO PAGE CODE —
       every caller is a capture at the head of a write, so a class that reached the page here would run it
       with no flow base and in the middle of recording a baseline. */
    JSValueConst (*forwarded_object)(JSContext *ctx, JSValueConst obj, JSAtom prop);
    /* THE CLASS'S OWN [[PreventExtensions]] — the one internal method a class could not previously override at
       all, because JS_PreventExtensions consulted the class for nothing and simply cleared `extensible`.
       WHAT NEEDS IT: Web IDL §3.9.5 [[PreventExtensions]] is "return false", full stop — its own note says
       "this keeps legacy platform objects extensible by making [[PreventExtensions]] fail for them" — so
       `Object.freeze(el.classList)` is 7.3.15 SetIntegrityLevel step 2 failing, which Object.freeze and
       Object.seal turn into a TypeError and Reflect.preventExtensions reports. Without the hook the freeze
       SUCCEEDED and the object silently stopped accepting the properties the spec says it always accepts.
       IT IS PER OBJECT AND NOT A FLAG ON THE CLASS, because a class need not be a legacy platform object for
       all of its instances: a CSSRule is one only when it is the `@keyframes` rule that carries the indexed
       getter, and a boolean beside the hooks could not say so.
       Returns 0 to REFUSE (the caller's false), 1 to let the ordinary prevent-extensions proceed, or -1 with an
       exception pending. It runs NO PAGE CODE — every caller reaches it from C. */
    int (*prevent_extensions)(JSContext *ctx, JSValueConst obj);
} JSClassExoticMethods;

typedef void JSClassFinalizer(JSRuntime *rt, JSValueConst val);
typedef void JSClassGCMark(JSRuntime *rt, JSValueConst val,
                           JS_MarkFunc *mark_func);
#define JS_CALL_FLAG_CONSTRUCTOR (1 << 0)
typedef JSValue JSClassCall(JSContext *ctx, JSValueConst func_obj,
                            JSValueConst this_val, int argc,
                            JSValueConst *argv, int flags);

typedef struct JSClassDef {
    const char *class_name; /* pure ASCII only! */
    JSClassFinalizer *finalizer;
    JSClassGCMark *gc_mark;
    /* if call != NULL, the object is a function. If (flags &
       JS_CALL_FLAG_CONSTRUCTOR) != 0, the function is called as a
       constructor. In this case, 'this_val' is new.target. A
       constructor call only happens if the object constructor bit is
       set (see JS_SetConstructorBit()). */
    JSClassCall *call;
    /* XXX: suppress this indirection ? It is here only to save memory
       because only a few classes need these methods */
    /* CONST, because the runtime's own copy of it is (JSClass.exotic) and every read is through a
       `const JSClassExoticMethods *`. Without it a host declaring its table `static const` — which is what a
       table of function pointers that nothing writes should be — discards the qualifier at the JSClassDef,
       and the only ways out are dropping the const or casting it away at every class. */
    const JSClassExoticMethods *exotic;
} JSClassDef;

#define JS_EVAL_OPTIONS_VERSION 1

typedef struct JSEvalOptions {
  int version;
  int eval_flags;
  const char *filename;
  int line_num;
  // can add new fields in ABI-compatible manner by incrementing JS_EVAL_OPTIONS_VERSION
} JSEvalOptions;

#define JS_INVALID_CLASS_ID 0
JS_EXTERN JSClassID JS_NewClassID(JSRuntime *rt, JSClassID *pclass_id);
/* Returns the class ID if `v` is an object, otherwise returns JS_INVALID_CLASS_ID. */
JS_EXTERN JSClassID JS_GetClassID(JSValueConst v);
JS_EXTERN int JS_NewClass(JSRuntime *rt, JSClassID class_id, const JSClassDef *class_def);

/* GIVE THE GLOBAL OBJECT A CLASS, so it can have EXOTIC own-property behaviour.
 *
 * HTML §7.2.2.2 "Indexed access on the Window object" makes the global a legacy platform object: `window[0]` names a child navigable without being an
 * own property, and `Object.defineProperty(window, 0, …)` must fail. In this engine exotic behaviour comes from
 * an object's CLASS, and the global is created by JS_NewContext long before any host class is registered — so
 * a host that needs it hands the already-created global the class it registered.
 *
 * THE CLASS MUST OWN NO PER-OBJECT DATA. A finalizer or a gc_mark reads the object's class-specific union,
 * which the global has never filled, so one that owns anything would free or trace garbage; both are rejected
 * here rather than left to crash later. Returns 0, or -1 for an unknown or unsuitable class. */
JS_EXTERN int JS_SetGlobalClass(JSContext *ctx, JSClassID class_id);

/* IS THIS ATOM AN ARRAY INDEX PROPERTY NAME? — ECMAScript's canonical numeric string in 0..2^32-2, which is the
   same thing Web IDL calls a "supported property index" and HTML §7.2.2.2 "Indexed access on the Window object" branches on. An exotic
   [[GetOwnProperty]]/[[DefineOwnProperty]]/[[Delete]] written outside this file has to answer it, and doing so
   from the atom's TEXT would both allocate on a lookup-miss path and re-derive a rule the engine already owns
   ("01" and "1e2" are not indices). Writes *pval on true. */
JS_EXTERN bool JS_AtomIsIndex(JSContext *ctx, uint32_t *pval, JSAtom atom);
/* AN INTERNAL METHOD'S REFUSAL, IN THE CALLER'S OWN MODE — the answer an exotic [[DefineOwnProperty]] or
   [[Set]] must give when the spec says "return false", which is NOT the same observable in every caller.
   Object.defineProperty turns a false into 7.3.8 DefinePropertyOrThrow's TypeError, an assignment turns one
   into 13.15 Assignment Operators' TypeError ONLY IN STRICT CODE, and Reflect.defineProperty /
   Reflect.set / a sloppy assignment report the false as a value. quickjs carries that distinction in `flags`
   (JS_PROP_THROW, JS_PROP_THROW_STRICT) and resolves it against the running frame's strictness — both of
   which are internal, so a hook in another translation unit can see neither and has no way to spell the
   spec's one-word answer. Returns false, or -1 with a TypeError pending. */
JS_EXTERN int JS_RefuseOrThrowTypeError(JSContext *ctx, int flags, const char *msg);
JS_EXTERN bool JS_IsRegisteredClass(JSRuntime *rt, JSClassID class_id);
/* Returns the class name or JS_ATOM_NULL if `id` is not a registered class. Must be freed with JS_FreeAtom. */
JS_EXTERN JSAtom JS_GetClassName(JSRuntime *rt, JSClassID class_id);
/* HOW MANY CLASS IDS THIS RUNTIME HAS HANDED OUT, past the predefined ones — `rt->js_class_id_alloc` less
   JS_CLASS_INIT_COUNT. That counter is incremented at ONE line, inside JS_NewClassID, so it is the one number
   a mint cannot evade however the mint is spelled: a host that keeps its own census of the class ids it was
   told about can bracket a window, difference this across it, and find a mint that skipped the census.
   IT IS NOT JS_IsRegisteredClass, WHICH ANSWERS A DIFFERENT QUESTION. An id minted and never handed to
   JS_NewClass is UNREGISTERED, so probing registration would miss precisely the case a census exists to
   catch, and would also count the predefined classes, which nobody minted.
   IT IS A LIFETIME COUNT AND NEVER A GAUGE: a class id cannot be given back, so this only ever rises and two
   readings of it may be differenced. The bias is removed here rather than left to the caller because
   `rt->js_class_id_alloc` is a biased counter and a caller that forgot the bias would report every runtime as
   having minted the whole predefined table. */
JS_EXTERN uint32_t JS_ClassIDsMinted(JSRuntime *rt);

/* value handling */

static inline JSValue JS_NewBool(JSContext *ctx, bool val)
{
    (void)&ctx;
    return JS_MKVAL(JS_TAG_BOOL, (val != 0));
}

static inline JSValue JS_NewInt32(JSContext *ctx, int32_t val)
{
    (void)&ctx;
    return JS_MKVAL(JS_TAG_INT, val);
}

static inline JSValue JS_NewFloat64(JSContext *ctx, double val)
{
    (void)&ctx;
    return __JS_NewFloat64(val);
}

static inline JSValue JS_NewCatchOffset(JSContext *ctx, int32_t val)
{
    (void)&ctx;
    return JS_MKVAL(JS_TAG_CATCH_OFFSET, val);
}

static inline JSValue JS_NewInt64(JSContext *ctx, int64_t val)
{
    JSValue v;
    if (val >= INT32_MIN && val <= INT32_MAX) {
        v = JS_NewInt32(ctx, (int32_t)val);
    } else {
        v = JS_NewFloat64(ctx, (double)val);
    }
    return v;
}

static inline JSValue JS_NewUint32(JSContext *ctx, uint32_t val)
{
    JSValue v;
    if (val <= INT32_MAX) {
        v = JS_NewInt32(ctx, (int32_t)val);
    } else {
        v = JS_NewFloat64(ctx, (double)val);
    }
    return v;
}

static inline JSValue JS_NewUint64(JSContext *ctx, uint64_t val)
{
    JSValue v;
    if (val <= INT32_MAX) {
        v = JS_NewInt32(ctx, (int32_t)val);
    } else {
        v = JS_NewFloat64(ctx, (double)val);
    }
    return v;
}

JS_EXTERN JSValue JS_NewNumber(JSContext *ctx, double d);
JS_EXTERN JSValue JS_NewBigInt64(JSContext *ctx, int64_t v);
JS_EXTERN JSValue JS_NewBigUint64(JSContext *ctx, uint64_t v);

static inline bool JS_IsNumber(JSValueConst v)
{
    int tag = JS_VALUE_GET_TAG(v);
    return tag == JS_TAG_INT || JS_TAG_IS_FLOAT64(tag);
}

static inline bool JS_IsBigInt(JSValueConst v)
{
    int tag = JS_VALUE_GET_TAG(v);
    return tag == JS_TAG_BIG_INT || tag == JS_TAG_SHORT_BIG_INT;
}

static inline bool JS_IsBool(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_BOOL;
}

static inline bool JS_IsNull(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_NULL;
}

static inline bool JS_IsUndefined(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_UNDEFINED;
}

static inline bool JS_IsException(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_EXCEPTION;
}

static inline bool JS_IsUninitialized(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_UNINITIALIZED;
}

static inline bool JS_IsString(JSValueConst v)
{
    int tag = JS_VALUE_GET_TAG(v);
    return tag == JS_TAG_STRING || tag == JS_TAG_STRING_ROPE;
}

static inline bool JS_IsSymbol(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_SYMBOL;
}

static inline bool JS_IsObject(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT;
}

static inline bool JS_IsModule(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_MODULE;
}

JS_EXTERN JSValue JS_Throw(JSContext *ctx, JSValue obj);
JS_EXTERN JSValue JS_GetException(JSContext *ctx);
/* An Error's recorded stack as a string, or undefined. Never invokes an accessor, so it never runs the
   page's Error.prepareStackTrace — use it wherever a HOST prints a diagnostic. */
JS_EXTERN JSValue JS_GetErrorStackString(JSContext *ctx, JSValueConst error);
/* A DOMException's name and message, read from the [[Name]]/[[Message]] internal slots, or undefined for any
   other value. The SAME reason as the stack above: they live behind accessors on DOMException.prototype, so a
   host diagnostic that looks for own properties finds nothing and reports a thrown DOMException — the single
   most common throw in a DOM engine — as an anonymous object. */
JS_EXTERN JSValue JS_GetDOMExceptionName(JSContext *ctx, JSValueConst val);
JS_EXTERN JSValue JS_GetDOMExceptionMessage(JSContext *ctx, JSValueConst val);
JS_EXTERN bool JS_HasException(JSContext *ctx);
JS_EXTERN bool JS_IsError(JSValueConst val);
JS_EXTERN bool JS_IsUncatchableError(JSValueConst val);
JS_EXTERN void JS_SetUncatchableError(JSContext *ctx, JSValueConst val);
JS_EXTERN void JS_ClearUncatchableError(JSContext *ctx, JSValueConst val);
// Shorthand for:
//  JSValue exc = JS_GetException(ctx);
//  JS_ClearUncatchableError(ctx, exc);
//  JS_Throw(ctx, exc);
JS_EXTERN void JS_ResetUncatchableError(JSContext *ctx);
JS_EXTERN JSValue JS_NewError(JSContext *ctx);
JS_EXTERN JSValue JS_PRINTF_FORMAT_ATTR(2, 3) JS_NewInternalError(JSContext *ctx, JS_PRINTF_FORMAT const char *fmt, ...);
JS_EXTERN JSValue JS_PRINTF_FORMAT_ATTR(2, 3) JS_NewPlainError(JSContext *ctx, JS_PRINTF_FORMAT const char *fmt, ...);
JS_EXTERN JSValue JS_PRINTF_FORMAT_ATTR(2, 3) JS_NewRangeError(JSContext *ctx, JS_PRINTF_FORMAT const char *fmt, ...);
JS_EXTERN JSValue JS_PRINTF_FORMAT_ATTR(2, 3) JS_NewReferenceError(JSContext *ctx, JS_PRINTF_FORMAT const char *fmt, ...);
JS_EXTERN JSValue JS_PRINTF_FORMAT_ATTR(2, 3) JS_NewSyntaxError(JSContext *ctx, JS_PRINTF_FORMAT const char *fmt, ...);
JS_EXTERN JSValue JS_PRINTF_FORMAT_ATTR(2, 3) JS_NewTypeError(JSContext *ctx, JS_PRINTF_FORMAT const char *fmt, ...);
JS_EXTERN JSValue JS_PRINTF_FORMAT_ATTR(2, 3) JS_ThrowInternalError(JSContext *ctx, JS_PRINTF_FORMAT const char *fmt, ...);
JS_EXTERN JSValue JS_PRINTF_FORMAT_ATTR(2, 3) JS_ThrowPlainError(JSContext *ctx, JS_PRINTF_FORMAT const char *fmt, ...);
JS_EXTERN JSValue JS_PRINTF_FORMAT_ATTR(2, 3) JS_ThrowRangeError(JSContext *ctx, JS_PRINTF_FORMAT const char *fmt, ...);
JS_EXTERN JSValue JS_PRINTF_FORMAT_ATTR(2, 3) JS_ThrowReferenceError(JSContext *ctx, JS_PRINTF_FORMAT const char *fmt, ...);
JS_EXTERN JSValue JS_PRINTF_FORMAT_ATTR(2, 3) JS_ThrowSyntaxError(JSContext *ctx, JS_PRINTF_FORMAT const char *fmt, ...);
JS_EXTERN JSValue JS_PRINTF_FORMAT_ATTR(2, 3) JS_ThrowTypeError(JSContext *ctx, JS_PRINTF_FORMAT const char *fmt, ...);
JS_EXTERN JSValue JS_PRINTF_FORMAT_ATTR(3, 4) JS_ThrowDOMException(JSContext *ctx, const char *name, JS_PRINTF_FORMAT const char *fmt, ...);
JS_EXTERN JSValue JS_ThrowOutOfMemory(JSContext *ctx);
JS_EXTERN void JS_FreeValue(JSContext *ctx, JSValue v);
JS_EXTERN void JS_FreeValueRT(JSRuntime *rt, JSValue v);
#define JS_FreeValues(ctx, ...) JS_CALLX_(JS_FreeValue, ctx, JS_COUNT_ARGS(__VA_ARGS__), __VA_ARGS__)
#define JS_FreeValuesRT(rt, ...) JS_CALLX_(JS_FreeValueRT, rt, JS_COUNT_ARGS(__VA_ARGS__), __VA_ARGS__)
JS_EXTERN JSValue JS_DupValue(JSContext *ctx, JSValueConst v);
JS_EXTERN JSValue JS_DupValueRT(JSRuntime *rt, JSValueConst v);
JS_EXTERN int JS_ToBool(JSContext *ctx, JSValueConst val); /* return -1 for JS_EXCEPTION */
static inline JSValue JS_ToBoolean(JSContext *ctx, JSValueConst val)
{
    return JS_NewBool(ctx, JS_ToBool(ctx, val));
}
/* THE COERCING SITE'S OWN ADDRESS TRAVELS WITH THE REQUEST, for the SAME reason the converter's and the byte
   consumer's do below and against an abort of the same shape. ECMAScript §7.1.4 ToNumber ( arg ) hands the
   remaining Object case to §7.1.1 ToPrimitive ( input [ , preferredType ] ), and §7.1.1 over UNKNOWN EXTERNAL
   INPUT is the IDENTITY — so an unknown arriving in this family is not a value to convert but a C body
   performing its own §7.1.4 / §7.1.3 ToNumeric ( arg ) where the OPERATOR should have answered at its own
   site, and converting it is an infinite coercion rather than a wrong number.
   The engine asserts that where every spelling converges, in ONE function, which is what makes the assert
   possible at all — but is also exactly why its remedy could not name the SITE it was instructing anyone to
   fix. It fires one frame below every numeric coercion in the engine and the host, so "fix it at that site"
   named an action with no object, over a call-site population far larger than anyone reads by hand. The site
   is known at the one place that knows it, so it is passed from there and the abort names the file:line.
   ONE ABI IN BOTH BUILDS and MACROS RATHER THAN INLINE WRAPPERS, both for the reasons stated in full above
   JS_ToStringAt: a translation unit built with a different APICLIENT_DEV cannot disagree with this one about
   the argument list, and __FILE__/__LINE__ inside a static inline expands at the HEADER, which is the one site
   that is never the answer. A release build simply never reads the two words. */
JS_EXTERN JSValue JS_ToNumberAt(JSContext *ctx, JSValueConst val, const char *file, int line);
#define JS_ToNumber(ctx, val) \
    JS_ToNumberAt((ctx), (val), __FILE__, __LINE__)
JS_EXTERN int JS_ToInt32At(JSContext *ctx, int32_t *pres, JSValueConst val,
                           const char *file, int line);
#define JS_ToInt32(ctx, pres, val) \
    JS_ToInt32At((ctx), (pres), (val), __FILE__, __LINE__)
/* §7.1.9 ToUint32 ( arg ) is §7.1.8 ToInt32 ( arg )'s bit pattern read unsigned, so this is the ONE member of
   the family that stays a FUNCTION. It FORWARDS its caller's site rather than capturing one — this wrapper is
   never the answer — and it is a function so `uint32_t *` keeps being TYPE-CHECKED: the macro form would have
   to spell the cast itself, and a cast in a macro accepts a wrong pointer type silently at every call site. */
static inline int JS_ToUint32At(JSContext *ctx, uint32_t *pres, JSValueConst val,
                                const char *file, int line)
{
    return JS_ToInt32At(ctx, (int32_t*)pres, val, file, line);
}
#define JS_ToUint32(ctx, pres, val) \
    JS_ToUint32At((ctx), (pres), (val), __FILE__, __LINE__)
JS_EXTERN int JS_ToInt64At(JSContext *ctx, int64_t *pres, JSValueConst val,
                           const char *file, int line);
#define JS_ToInt64(ctx, pres, val) \
    JS_ToInt64At((ctx), (pres), (val), __FILE__, __LINE__)
JS_EXTERN int JS_ToIndexAt(JSContext *ctx, uint64_t *plen, JSValueConst val,
                           const char *file, int line);
#define JS_ToIndex(ctx, plen, val) \
    JS_ToIndexAt((ctx), (plen), (val), __FILE__, __LINE__)
JS_EXTERN int JS_ToFloat64At(JSContext *ctx, double *pres, JSValueConst val,
                             const char *file, int line);
#define JS_ToFloat64(ctx, pres, val) \
    JS_ToFloat64At((ctx), (pres), (val), __FILE__, __LINE__)
/* return an exception if 'val' is a Number.
   §7.1.17 ToBigInt64 ( arg ) and §7.1.18 ToBigUint64 ( arg ), whose step 1 is `? ToBigInt(arg)`. They do NOT
   feed the assertion the family above feeds: §7.1.15 ToBigInt ( arg ) reaches §7.1.1 ToPrimitive
   ( input [ , preferredType ] ) directly rather than through §7.1.4 ToNumber ( arg ), so JS_ToNumberHintFree's
   object arm never sees them. They carry a site anyway, against a SECOND assertion of their own, in
   JS_ToBigIntFreeAt's object arm — and it guards a HARDER failure than the numeric one. §7.1.1 over UNKNOWN
   EXTERNAL INPUT is the IDENTITY and §7.1.15's switch re-dispatches on the tag it gets back, so a boundary
   with no assertion here does not return a wrong BigInt: it DOES NOT RETURN, in one C activation, with no
   preempt point and no way for the scheduler to take the thread back. That is why "a site threaded here would
   be read by nothing" — the argument this comment used to make, correctly, while no such assertion existed —
   no longer holds, and it is also why this pair is worth threading before anything else in the family.
   ONE ABI IN BOTH BUILDS and MACROS RATHER THAN INLINE WRAPPERS, for the reasons stated in full above
   JS_ToNumberAt: a translation unit built with a different APICLIENT_DEV cannot disagree with this one about
   the argument list, and __FILE__/__LINE__ inside a static inline expands at the HEADER, which is the one site
   that is never the answer. A release build simply never reads the two words. */
JS_EXTERN int JS_ToBigInt64At(JSContext *ctx, int64_t *pres, JSValueConst val,
                              const char *file, int line);
#define JS_ToBigInt64(ctx, pres, val) \
    JS_ToBigInt64At((ctx), (pres), (val), __FILE__, __LINE__)
/* §7.1.18 is §7.1.17's bit pattern read unsigned, so this is the ONE member of the family that stays a
   FUNCTION, exactly as §7.1.9 ToUint32 ( arg ) is in the numeric family and for the identical reason. It
   FORWARDS its caller's site rather than capturing one — this wrapper is never the answer — and it is a
   function so `uint64_t *` keeps being TYPE-CHECKED: the macro form would have to spell the cast itself, and a
   cast in a macro accepts a wrong pointer type silently at every call site. */
static inline int JS_ToBigUint64At(JSContext *ctx, uint64_t *pres, JSValueConst val,
                                   const char *file, int line)
{
    return JS_ToBigInt64At(ctx, (int64_t *)pres, val, file, line);
}
#define JS_ToBigUint64(ctx, pres, val) \
    JS_ToBigUint64At((ctx), (pres), (val), __FILE__, __LINE__)
/* same as JS_ToInt64() but allow BigInt */
JS_EXTERN int JS_ToInt64ExtAt(JSContext *ctx, int64_t *pres, JSValueConst val,
                              const char *file, int line);
#define JS_ToInt64Ext(ctx, pres, val) \
    JS_ToInt64ExtAt((ctx), (pres), (val), __FILE__, __LINE__)

JS_EXTERN JSValue JS_NewStringLen(JSContext *ctx, const char *str1, size_t len1);
static inline JSValue JS_NewString(JSContext *ctx, const char *str) {
    return JS_NewStringLen(ctx, str, strlen(str));
}
// makes a copy of the input; does not check if the input is valid UTF-16,
// that is the responsibility of the caller
JS_EXTERN JSValue JS_NewStringUTF16(JSContext *ctx, const uint16_t *buf,
                                    size_t len);
JS_EXTERN JSValue JS_NewAtomString(JSContext *ctx, const char *str);
/* THE CONVERTER'S OWN CALL SITE TRAVELS WITH THE REQUEST, for the SAME reason the byte consumer's does below
   and against the SAME abort. §7.1.19 ToString ( arg ) step 9 asserts the remaining case is an Object and step
   10 hands it to §7.1.1 ToPrimitive ( input [ , preferredType ] ), whose step 1.a is GetMethod(input,
   %Symbol.toPrimitive%) and whose §7.1.1.1 OrdinaryToPrimitive ( obj, hint ) step 3 CALLS the page's
   valueOf/toString — page code from a C activation with no flow base, which is a capability this engine does
   not have. The crash that says so used to name only the value's CLASS and the page's innermost frame, and its
   own instruction was "this is a JS_ToString, a JS_ToPropertyKey or a JS_ValueToAtom called straight from C" —
   an instruction with no address in it, over roughly a hundred spellings, which is unfollowable exactly as the
   byte consumer's was before its site travelled. The site is known at the one place that knows it, so it is
   passed from there and the abort names the file:line to route.
   ONE ABI IN BOTH BUILDS and MACROS RATHER THAN INLINE WRAPPERS, both for the reasons the block below states:
   a translation unit built with a different APICLIENT_DEV cannot disagree about the argument list, and
   __FILE__/__LINE__ inside a static inline expands at the HEADER, which is the one site that is never the
   answer. */
JS_EXTERN JSValue JS_ToStringAt(JSContext *ctx, JSValueConst val, const char *file, int line);
#define JS_ToString(ctx, val) \
    JS_ToStringAt((ctx), (val), __FILE__, __LINE__)
JS_EXTERN JSValue JS_ToPropertyKeyAt(JSContext *ctx, JSValueConst val, const char *file, int line);
#define JS_ToPropertyKey(ctx, val) \
    JS_ToPropertyKeyAt((ctx), (val), __FILE__, __LINE__)
/* THE BYTE CONSUMER'S OWN CALL SITE TRAVELS WITH THE REQUEST, because the assertion that fires underneath it
   is about the CALLER and the caller is the one thing the operand cannot name. These entries are ToString
   (ECMAScript §7.1.19 ToString ( arg )) followed by an encoder, and §7.1.19 step 9 asserts the remaining case
   is an Object, step 10 sends it to §7.1.1 ToPrimitive ( input [ , preferredType ] ), and §7.1.1 over UNKNOWN
   EXTERNAL INPUT is the identity — the value is primitive in the page and wears an Object only as a carrier.
   A `const char *` cannot carry a concolic, so there is nothing for this boundary to derive into and no
   coercion it can perform; the consumer has to ask for the unknown's own display shape at ITS site. The abort
   that says so used to name only WHICH value died, and that instruction is unfollowable on its own: an orphan
   drive's argument reads `{orphan<locator>.arg<n>}` at every edge that could have taken it, so the reader was
   sent to search every byte consumer in the tree by hand. The site is known at the one place that knows it, so
   it is passed from there and the abort names the file:line to fix.
   ONE ABI IN BOTH BUILDS: the site is carried whether or not APICLIENT_DEV is set. A translation unit built
   with a different value of it can then never disagree with this one about the argument list — the skew that
   makes two objects read one struct at two sizes and fault in a place neither names. A release build simply
   never reads the two words.
   MACROS, NOT INLINE WRAPPERS: __FILE__/__LINE__ inside a static inline expand at the HEADER, which is the one
   site that is never the answer. */
JS_EXTERN const char *JS_ToCStringLen2At(JSContext *ctx, size_t *plen, JSValueConst val1, bool cesu8,
                                         const char *file, int line);
#define JS_ToCStringLen2(ctx, plen, val1, cesu8) \
    JS_ToCStringLen2At((ctx), (plen), (val1), (cesu8), __FILE__, __LINE__)
#define JS_ToCStringLen(ctx, plen, val1) \
    JS_ToCStringLen2At((ctx), (plen), (val1), 0, __FILE__, __LINE__)
#define JS_ToCString(ctx, val1) \
    JS_ToCStringLen2At((ctx), NULL, (val1), 0, __FILE__, __LINE__)
// returns a utf-16 version of the string in native endianness; the
// string is not nul terminated and can contain unmatched surrogates
// |*plen| is in uint16s, not code points; a surrogate pair such as
// U+D834 U+DF06 has len=2; an unmatched surrogate has len=1
JS_EXTERN const uint16_t *JS_ToCStringLenUTF16At(JSContext *ctx, size_t *plen, JSValueConst val1,
                                                 const char *file, int line);
#define JS_ToCStringLenUTF16(ctx, plen, val1) \
    JS_ToCStringLenUTF16At((ctx), (plen), (val1), __FILE__, __LINE__)
#define JS_ToCStringUTF16(ctx, val1) \
    JS_ToCStringLenUTF16At((ctx), NULL, (val1), __FILE__, __LINE__)
JS_EXTERN void JS_FreeCString(JSContext *ctx, const char *ptr);
JS_EXTERN void JS_FreeCStringRT(JSRuntime *rt, const char *ptr);
JS_EXTERN void JS_FreeCStringUTF16(JSContext *ctx, const uint16_t *ptr);
JS_EXTERN void JS_FreeCStringRT_UTF16(JSRuntime *rt, const uint16_t *ptr);

JS_EXTERN JSValue JS_NewObjectProtoClass(JSContext *ctx, JSValueConst proto,
                                         JSClassID class_id);
JS_EXTERN JSValue JS_NewObjectClass(JSContext *ctx, JSClassID class_id);
JS_EXTERN JSValue JS_NewObjectProto(JSContext *ctx, JSValueConst proto);
JS_EXTERN JSValue JS_NewObject(JSContext *ctx);
// takes ownership of the values
JS_EXTERN JSValue JS_NewObjectFrom(JSContext *ctx, int count,
                                   const JSAtom *props,
                                   const JSValue *values);
// takes ownership of the values
JS_EXTERN JSValue JS_NewObjectFromStr(JSContext *ctx, int count,
                                      const char **props,
                                      const JSValue *values);
JS_EXTERN JSValue JS_ToObject(JSContext *ctx, JSValueConst val);
/* The HOST printer's tag for a value: 20.1.3.6 without its @@toStringTag Get. Not the spec algorithm — that is
   Object.prototype.toString, which script reaches and which runs the page's code. */
JS_EXTERN JSValue JS_ToObjectString(JSContext *ctx, JSValueConst val);

JS_EXTERN bool JS_IsFunction(JSContext* ctx, JSValueConst val);
JS_EXTERN bool JS_IsAsyncFunction(JSValueConst val);
JS_EXTERN bool JS_IsConstructor(JSContext* ctx, JSValueConst val);
JS_EXTERN bool JS_SetConstructorBit(JSContext *ctx, JSValueConst func_obj, bool val);

JS_EXTERN bool JS_IsRegExp(JSValueConst val);
JS_EXTERN bool JS_IsMap(JSValueConst val);
JS_EXTERN bool JS_IsSet(JSValueConst val);
JS_EXTERN bool JS_IsWeakRef(JSValueConst val);
JS_EXTERN bool JS_IsWeakSet(JSValueConst val);
JS_EXTERN bool JS_IsWeakMap(JSValueConst val);
JS_EXTERN bool JS_IsDataView(JSValueConst val);

JS_EXTERN JSValue JS_NewArray(JSContext *ctx);
// takes ownership of the values
JS_EXTERN JSValue JS_NewArrayFrom(JSContext *ctx, int count,
                                  const JSValue *values);
// reader beware: JS_IsArray used to "punch" through proxies and check
// if the target object is an array but it no longer does; use JS_IsProxy
// and JS_GetProxyTarget instead, and remember that the target itself can
// also be a proxy, ad infinitum
JS_EXTERN bool JS_IsArray(JSValueConst val);

JS_EXTERN bool JS_IsProxy(JSValueConst val);
JS_EXTERN JSValue JS_GetProxyTarget(JSContext *ctx, JSValueConst proxy);
JS_EXTERN JSValue JS_GetProxyHandler(JSContext *ctx, JSValueConst proxy);
JS_EXTERN JSValue JS_NewProxy(JSContext *ctx, JSValueConst target,
                              JSValueConst handler);

JS_EXTERN JSValue JS_NewDate(JSContext *ctx, double epoch_ms);
JS_EXTERN bool JS_IsDate(JSValueConst v);
/* A Date's [[DateValue]] — the READ half of JS_NewDate, and an INTERNAL SLOT rather than a method call. The
   only other route to it from outside is `getTime`/`valueOf`, which are the PAGE's to replace and which are
   step machines here precisely so that no C entry calls one; an algorithm defined over "the time represented by
   input" (HTML §4.10.5.1.7's convert a Date object to a string is the first) means the slot and not the method.
   NaN is a real answer — it is what an invalid Date holds — so the caller tests for it rather than for failure;
   asking this of anything that is not a Date is fatal in dev, since JS_IsDate is how you find out. */
JS_EXTERN double JS_GetDateValue(JSValueConst v);

/* THE READER'S OWN CALL SITE TRAVELS WITH THE REQUEST, for the SAME reason the converter's and the byte
   consumer's do above and against the SAME abort. §10.1.8.1 OrdinaryGet ( obj, propertyKey, receiver ) step 7
   is `Return ? Call(getter, receiver)` — the page's function, from a C activation with no flow base under it,
   which is a capability this engine does not have. The read OPCODES route it (tramp_accessor_getter hands the
   getter to the tramp chain as a 0-arg method call); a C caller cannot, so the crash that says so is the only
   thing standing between a getter and a drive-to-completion.
   That crash could name the PROPERTY, the RECEIVER's class and the HOLDER's class, and no address — and these
   four spellings have hundreds of call sites between them, so "route this read" was an instruction with
   nowhere to go, exactly as "route the JS_ToString that did this" was before the converter's site travelled.
   THE SITE IS KNOWN AT THE ONE PLACE THAT KNOWS IT, so it is passed from there.
   ONE ABI IN BOTH BUILDS and MACROS RATHER THAN INLINE WRAPPERS, both for the reasons the converter's block
   states: a translation unit built with a different APICLIENT_DEV cannot disagree about the argument list, and
   __FILE__/__LINE__ inside a static inline expands at the HEADER, which is the one site that is never the
   answer. Each spelling carries its OWN site rather than forwarding through the next: the three below all end
   in JS_GetPropertyAt, and a site captured there would name quickjs.c for every one of them. */
JS_EXTERN JSValue JS_GetPropertyAt(JSContext *ctx, JSValueConst this_obj, JSAtom prop,
                                   const char *file, int line);
#define JS_GetProperty(ctx, this_obj, prop) \
    JS_GetPropertyAt((ctx), (this_obj), (prop), __FILE__, __LINE__)
JS_EXTERN JSValue JS_GetPropertyUint32At(JSContext *ctx, JSValueConst this_obj,
                                         uint32_t idx, const char *file, int line);
#define JS_GetPropertyUint32(ctx, this_obj, idx) \
    JS_GetPropertyUint32At((ctx), (this_obj), (idx), __FILE__, __LINE__)
JS_EXTERN JSValue JS_GetPropertyInt64At(JSContext *ctx, JSValueConst this_obj,
                                        int64_t idx, const char *file, int line);
#define JS_GetPropertyInt64(ctx, this_obj, idx) \
    JS_GetPropertyInt64At((ctx), (this_obj), (idx), __FILE__, __LINE__)
JS_EXTERN JSValue JS_GetPropertyStrAt(JSContext *ctx, JSValueConst this_obj,
                                      const char *prop, const char *file, int line);
#define JS_GetPropertyStr(ctx, this_obj, prop) \
    JS_GetPropertyStrAt((ctx), (this_obj), (prop), __FILE__, __LINE__)

JS_EXTERN int JS_SetProperty(JSContext *ctx, JSValueConst this_obj,
                             JSAtom prop, JSValue val);
JS_EXTERN int JS_SetPropertyUint32(JSContext *ctx, JSValueConst this_obj,
                                   uint32_t idx, JSValue val);
JS_EXTERN int JS_SetPropertyInt64(JSContext *ctx, JSValueConst this_obj,
                                  int64_t idx, JSValue val);
JS_EXTERN int JS_SetPropertyStr(JSContext *ctx, JSValueConst this_obj,
                                const char *prop, JSValue val);
JS_EXTERN int JS_HasProperty(JSContext *ctx, JSValueConst this_obj, JSAtom prop);
JS_EXTERN int JS_IsExtensible(JSContext *ctx, JSValueConst obj);
JS_EXTERN int JS_PreventExtensions(JSContext *ctx, JSValueConst obj);
JS_EXTERN int JS_DeleteProperty(JSContext *ctx, JSValueConst obj, JSAtom prop, int flags);
JS_EXTERN int JS_SetPrototype(JSContext *ctx, JSValueConst obj, JSValueConst proto_val);
JS_EXTERN JSValue JS_GetPrototype(JSContext *ctx, JSValueConst val);
JS_EXTERN int JS_GetLength(JSContext *ctx, JSValueConst obj, int64_t *pres);
JS_EXTERN int JS_SetLength(JSContext *ctx, JSValueConst obj, int64_t len);

#define JS_GPN_STRING_MASK  (1 << 0)
#define JS_GPN_SYMBOL_MASK  (1 << 1)
#define JS_GPN_PRIVATE_MASK (1 << 2)
/* only include the enumerable properties */
#define JS_GPN_ENUM_ONLY    (1 << 4)
/* set theJSPropertyEnum.is_enumerable field */
#define JS_GPN_SET_ENUM     (1 << 5)

JS_EXTERN int JS_GetOwnPropertyNames(JSContext *ctx, JSPropertyEnum **ptab,
                                     uint32_t *plen, JSValueConst obj,
                                     int flags);
/* The COW delta's own-property read: the SLOT's whole state, as a §6.2.6 PROPERTY DESCRIPTOR — {[[Value]],
   [[Writable]]} XOR {[[Get]],[[Set]]}, both carrying [[Enumerable]] and [[Configurable]]. It is one record and
   not a value plus a side-flag because the spec makes it one: the accessor bit in `flags` IS the kind field, so
   there is no second spelling of "which shape is this slot in" to drift from the first.
   NOT [[GetOwnProperty]] — it asserts that no Proxy is reachable, because a delta is swapped by the scheduler
   and a trap would run the page's code mid-context-switch. An ACCESSOR is answered rather than refused: the
   getter and setter are handed over as function OBJECTS, exactly as they are handed to
   Object.getOwnPropertyDescriptor, and neither is ever called by the read or by the restore.
   1 = own property (*pd owned), 0 = absent, -1 = threw. */
JS_EXTERN int JS_GetOwnSlotDesc(JSContext *ctx, JSPropertyDescriptor *pd, JSValueConst obj, JSAtom prop);
/* Its VALUE projection, for a component reading a slot IT created (an idl_slots record, a listener map, a key
   path's next step). An accessor is refused on this side — a component's own internal slot is one nothing else
   can define, so an accessor there says the read landed on an object that is not the one it named.
   1 = own data property (*pval owned), 0 = absent, -1 = threw. */
JS_EXTERN int JS_GetOwnSlot(JSContext *ctx, JSValue *pval, JSValueConst obj, JSAtom prop);
/* [[GetOwnProperty]] — the INTERNAL METHOD, performed from C by a class that stands in for another object.
   HTML §7.2.3.5 step 3's `OrdinaryGetOwnProperty(W, P)` is the caller it exists for: a WindowProxy answering
   its own [[GetOwnProperty]] hook has to perform the Window's, and the Window is a legacy platform object
   whose own hook answers `window[0]`.
   IT IS NOT JS_GetOwnSlotDesc, and the difference is not a nicety: that one reads STORAGE, so a `let` binding
   in TDZ answers with an uninitialised value where the internal method throws its ReferenceError — right for
   a delta putting a slot back, wrong for a forward that owes the operation.
   IT ASSERTS THAT IT RUNS NONE OF THE PAGE'S CODE, at the CLASS rather than at the caller: an object with no
   exotic hook answers out of its own shape, one whose class declares get_own_property_no_user_code answers out
   of its own state, and a Proxy — whose every own-property query is the `getOwnPropertyDescriptor` trap —
   aborts here rather than running a trap in a C activation with no flow base to park it on.
   1 = own property (*desc filled and OWNED when desc != NULL), 0 = absent, -1 = threw. */
JS_EXTERN int JS_GetOwnPropertyNoUserCode(JSContext *ctx, JSPropertyDescriptor *desc,
                                          JSValueConst obj, JSAtom prop);
/* Its WRITE twin: the SLOT's whole state put back, attributes included. NOT [[Set]] — no prototype walk, no
   setter, no `writable` and no `extensible`; and NOT [[DefineOwnProperty]] either, which is the trap this
   signature exists to avoid: 10.1.6.3 step 4 refuses every change to a NON-CONFIGURABLE slot, and a slot the
   flow made non-configurable is exactly the slot a restore has to widen back. Every one of those refuses by
   THROWING with no flow base to run the exception on, so this writes STORAGE — the shape's flag word and the
   property's union. Mirrors every kind the read answers (data, accessor and the conversion in either direction,
   VARREF, AUTOINIT, dense element, the array `length` pair) and CANNOT FAIL — a typed-array element and an
   allocation failure both abort at the origin, so there is no status a caller could default.
   Consumes the descriptor's values (and clears them). */
JS_EXTERN void JS_SetOwnSlotDesc(JSContext *ctx, JSValueConst obj, JSAtom prop, JSPropertyDescriptor *pd);
/* And its REMOVAL: the slot taken out, whatever its attributes. 10.1.10.1 OrdinaryDelete returns false for a
   non-configurable slot and hands an exotic object its own handler; a swap asks neither, because the slot it is
   removing is one the running flow CREATED and `Object.defineProperty(o,"x",{value:1})` — whose C/W/E all
   default to false — is the commonest way a flow creates one. CANNOT FAIL, for the same reason the write
   cannot. */
JS_EXTERN void JS_DeleteOwnSlot(JSContext *ctx, JSValueConst obj, JSAtom prop);
/* THE BUFFER'S BYTES, READ — the byte twin of the slot read above, and the reason it is a separate call rather
   than one more kind of slot is JSTimeTravelHooks.buf_state's: a view's element is not what the page wrote, the
   storage is. `obj` is the ArrayBuffer/SharedArrayBuffer OBJECT.
   It takes no JSContext and cannot throw, for the same reason JS_GetOwnSlotDesc cannot: its callers run where
   there is no flow base to receive an exception. It answers NULL with *plen 0 for a DETACHED buffer — a positive
   statement, not a failure: a detached buffer holds no bytes, so there is nothing a flow could have changed.
   THERE IS NO WRITE TWIN, and that is the shape of the fact rather than an omission: putting bytes back is only
   half of putting a buffer back, because a flow that RESIZED, TRANSFERRED or DETACHED it moved the storage those
   bytes live in. Both halves are one blob — JS_BufferStateSave below. */
JS_EXTERN const uint8_t *JS_GetBufferBytes(JSValueConst obj, uint32_t *plen);
JS_EXTERN void JS_FreePropertyEnum(JSContext *ctx, JSPropertyEnum *tab,
                                   uint32_t len);

JS_EXTERN JSValue JS_Call(JSContext *ctx, JSValueConst func_obj,
                          JSValueConst this_obj, int argc, JSValueConst *argv);
JS_EXTERN JSValue JS_Invoke(JSContext *ctx, JSValueConst this_val, JSAtom atom,
                            int argc, JSValueConst *argv);
JS_EXTERN JSValue JS_CallConstructor(JSContext *ctx, JSValueConst func_obj,
                                     int argc, JSValueConst *argv);
JS_EXTERN JSValue JS_CallConstructor2(JSContext *ctx, JSValueConst func_obj,
                                      JSValueConst new_target,
                                      int argc, JSValueConst *argv);
/* Try to detect if the input is a module. Returns true if parsing the input
 * as a module produces no syntax errors. It's a naive approach that is not
 * wholly infallible: non-strict classic scripts may _parse_ okay as a module
 * but not _execute_ as one (different runtime semantics.) Use with caution.
 * |input| can be either ASCII or UTF-8 encoded source code.
 * Returns false if QuickJS was built with -DQJS_DISABLE_PARSER.
 */
JS_EXTERN bool JS_DetectModule(const char *input, size_t input_len);
/* 'input' must be zero terminated i.e. input[input_len] = '\0'. */
JS_EXTERN JSValue JS_Eval(JSContext *ctx, const char *input, size_t input_len,
                          const char *filename, int eval_flags);
JS_EXTERN JSValue JS_Eval2(JSContext *ctx, const char *input, size_t input_len,
                           JSEvalOptions *options);
JS_EXTERN JSValue JS_EvalThis(JSContext *ctx, JSValueConst this_obj,
                              const char *input, size_t input_len,
                              const char *filename, int eval_flags);
JS_EXTERN JSValue JS_EvalThis2(JSContext *ctx, JSValueConst this_obj,
                              const char *input, size_t input_len,
                              JSEvalOptions *options);
JS_EXTERN JSValue JS_GetGlobalObject(JSContext *ctx);
JS_EXTERN int JS_DefineProperty(JSContext *ctx, JSValueConst this_obj,
                                JSAtom prop, JSValueConst val,
                                JSValueConst getter, JSValueConst setter,
                                int flags);
JS_EXTERN int JS_DefinePropertyValue(JSContext *ctx, JSValueConst this_obj,
                                     JSAtom prop, JSValue val, int flags);
JS_EXTERN int JS_DefinePropertyValueUint32(JSContext *ctx, JSValueConst this_obj,
                                           uint32_t idx, JSValue val, int flags);
JS_EXTERN int JS_DefinePropertyValueStr(JSContext *ctx, JSValueConst this_obj,
                                        const char *prop, JSValue val, int flags);
JS_EXTERN int JS_DefinePropertyGetSet(JSContext *ctx, JSValueConst this_obj,
                                      JSAtom prop, JSValue getter, JSValue setter,
                                      int flags);
/* Only supported for custom classes, returns 0 on success < 0 otherwise. */
JS_EXTERN int JS_SetOpaque(JSValueConst obj, void *opaque);
JS_EXTERN void *JS_GetOpaque(JSValueConst obj, JSClassID class_id);
JS_EXTERN void *JS_GetOpaque2(JSContext *ctx, JSValueConst obj, JSClassID class_id);
JS_EXTERN void *JS_GetAnyOpaque(JSValueConst obj, JSClassID *class_id);

/* 'buf' must be zero terminated i.e. buf[buf_len] = '\0'. */
JS_EXTERN JSValue JS_ParseJSON(JSContext *ctx, const char *buf, size_t buf_len,
                               const char *filename);
/* JS_JSONStringify is DELETED. Serialization runs the page's code — toJSON, the replacer, every element and
   member read, and a Proxy's ownKeys/getOwnPropertyDescriptor traps — so it is a step machine reached through
   the flow machinery, and a C entry beside it would be a second implementation of the same algorithm. */

typedef void JSFreeArrayBufferDataFunc(JSRuntime *rt, void *opaque, void *ptr);
JS_EXTERN JSValue JS_NewArrayBuffer(JSContext *ctx, uint8_t *buf, size_t len,
                                    JSFreeArrayBufferDataFunc *free_func, void *opaque,
                                    bool is_shared);
JS_EXTERN JSValue JS_NewArrayBufferCopy(JSContext *ctx, const uint8_t *buf, size_t len);
JS_EXTERN void JS_DetachArrayBuffer(JSContext *ctx, JSValueConst obj);
JS_EXTERN uint8_t *JS_GetArrayBuffer(JSContext *ctx, size_t *psize, JSValueConst obj);
JS_EXTERN bool JS_IsArrayBuffer(JSValueConst obj);
// returns true or false if obj is an ArrayBuffer, -1 otherwise
JS_EXTERN int JS_IsImmutableArrayBuffer(JSValueConst obj);
// returns 0 if obj is an ArrayBuffer, -1 otherwise
JS_EXTERN int JS_SetImmutableArrayBuffer(JSValueConst obj, bool immutable);
JS_EXTERN uint8_t *JS_GetUint8Array(JSContext *ctx, size_t *psize, JSValueConst obj);

typedef enum JSTypedArrayEnum {
    JS_TYPED_ARRAY_UINT8C = 0,
    JS_TYPED_ARRAY_INT8,
    JS_TYPED_ARRAY_UINT8,
    JS_TYPED_ARRAY_INT16,
    JS_TYPED_ARRAY_UINT16,
    JS_TYPED_ARRAY_INT32,
    JS_TYPED_ARRAY_UINT32,
    JS_TYPED_ARRAY_BIG_INT64,
    JS_TYPED_ARRAY_BIG_UINT64,
    JS_TYPED_ARRAY_FLOAT16,
    JS_TYPED_ARRAY_FLOAT32,
    JS_TYPED_ARRAY_FLOAT64,
} JSTypedArrayEnum;

JS_EXTERN JSValue JS_NewTypedArray(JSContext *ctx, int argc, JSValueConst *argv,
                                   JSTypedArrayEnum array_type);
JS_EXTERN JSValue JS_GetTypedArrayBuffer(JSContext *ctx, JSValueConst obj,
                                         size_t *pbyte_offset,
                                         size_t *pbyte_length,
                                         size_t *pbytes_per_element);
JS_EXTERN JSValue JS_NewUint8Array(JSContext *ctx, uint8_t *buf, size_t len,
                                   JSFreeArrayBufferDataFunc *free_func, void *opaque,
                                   bool is_shared);
/* returns -1 if not a typed array otherwise return a JSTypedArrayEnum value */
JS_EXTERN int JS_GetTypedArrayType(JSValueConst obj);
JS_EXTERN JSValue JS_NewUint8ArrayCopy(JSContext *ctx, const uint8_t *buf, size_t len);
typedef struct {
    void *(*sab_alloc)(void *opaque, size_t size);
    void (*sab_free)(void *opaque, void *ptr);
    void (*sab_dup)(void *opaque, void *ptr);
    void *sab_opaque;
} JSSharedArrayBufferFunctions;
JS_EXTERN void JS_SetSharedArrayBufferFunctions(JSRuntime *rt, const JSSharedArrayBufferFunctions *sf);

typedef enum JSPromiseStateEnum {
    // argument to JS_PromiseState() was not in fact a promise
    JS_PROMISE_NOT_A_PROMISE = -1,
    JS_PROMISE_PENDING       =  0,
    JS_PROMISE_FULFILLED,
    JS_PROMISE_REJECTED,
} JSPromiseStateEnum;

JS_EXTERN JSValue JS_NewPromiseCapability(JSContext *ctx, JSValue *resolving_funcs);

/* SET [[PromiseIsHandled]] WITHOUT ATTACHING A REACTION — a spec step several web specs perform verbatim.
   Streams §5.3's EnsureReadyPromiseRejected replaces an already-settled `ready` with a promise REJECTED at
   birth and immediately marks it handled, because the writer may never be read again and the rejection is the
   stream's own bookkeeping rather than an error the page failed to catch. Without this the only way to set the
   flag is to attach a reaction, which is a different, observable operation: it queues a job. */
JS_EXTERN void JS_MarkPromiseHandled(JSContext *ctx, JSValueConst promise);

/* READ [[PromiseIsHandled]]. The write above has no reader, and the spec steps that need one read the flag at a
   DIFFERENT TIME from the one that set it, which is why attaching a reaction cannot answer the question.
   HTML §8.1.4.7 Unhandled promise rejections step 4.1.1 re-checks it inside the task its checkpoint queued, so
   a promise that got a handler between the two is not reported; step 4.1.4 checks it AGAIN after the
   `unhandledrejection` listeners have run, to decide whether the promise joins that global's outstanding
   rejected promises weak set.
   NOT THE TRACKER'S `is_handled` ARGUMENT. That one names the EDGE the host is being told about, and it arrives
   BEFORE the flag is written — a host reading this inside the tracker's handle arm reads `false`. Do not use
   one to stand in for the other.
   ITS ARGUMENT IS A PROMISE: [[PromiseIsHandled]] is a slot only a promise has, so a non-promise is a caller
   error rather than a value with a false answer, and it aborts rather than answering. */
JS_EXTERN bool JS_IsPromiseHandled(JSContext *ctx, JSValueConst promise);

/* CALL `func(value)` AS A CALL-ROOT FLOW — the base a promise reaction runs on — rather than as a C activation.
   A trusted-host reply is delivered by calling something, and JS_Call from the host's pump has no flow base, so
   a page's `then` getter or a loop inside its handler drives to completion there. Every host delivery uses this.
   0 = done, -1 = it threw (the exception is live). */
JS_EXTERN int JS_CallAsFlow(JSContext *ctx, JSValueConst func, JSValueConst value);

/* ATTACH REACTIONS THE WAY THE SPEC DOES — PerformPromiseThen, which does NOT read `then` off the promise. A
   host component reacting to a promise it did not create had only `.then`, and a page that replaces
   Promise.prototype.then must not thereby change what a spec algorithm does. Returns the reactions' capability
   promise; either handler may be JS_UNDEFINED. */
JS_EXTERN JSValue JS_PerformPromiseThen(JSContext *ctx, JSValueConst promise, JSValueConst on_fulfilled,
                                        JSValueConst on_rejected);
/* Atomics.waitAsync timeouts are the HOST's to wait for — 25.4.3.14 enqueues a host timeout job. Ask how long
   until the earliest deadline (-1 = none), wait for it wherever the host already waits, then settle what is
   due. The engine never blocks on this itself. */
/* Atomics.waitAsync's timeout is a HOST job (25.4.3.14). JS_AtomicsExpireAsync settles every waiter this
   runtime owns that is due — notified or past its deadline — and reports how many; call it from the event loop
   alongside the job pump. JS_AtomicsAsyncWaitForWork blocks until there is one, returning false when the
   runtime owns no outstanding waiter and the loop may treat its queue as drained. */
JS_EXTERN int JS_AtomicsExpireAsync(JSRuntime *rt);
JS_EXTERN bool JS_AtomicsAsyncWaitForWork(JSRuntime *rt);
JS_EXTERN JSPromiseStateEnum JS_PromiseState(JSContext *ctx,
                                             JSValueConst promise);
JS_EXTERN JSValue JS_PromiseResult(JSContext *ctx, JSValueConst promise);
JS_EXTERN bool JS_IsPromise(JSValueConst val);
/* A promise already settled with `value`, for an embedder with a completion and no capability to hand it to.
   NARROWED IN THIS FORK, AND THE NARROWING IS THE WHOLE OF WHAT AN EMBEDDER MUST KNOW: a rejection takes any
   value, and a resolution takes any value that is NOT an Object. A resolution WITH one is refused, because
   27.5.4.7.1 PromiseResolve step 1.a reads `constructor` off a promise and 27.5.1.3 step 2.f reads `then` off a
   thenable — the page's code either way, and a C activation has no flow base to run it on. An embedder holding
   an object settles it from a step machine through the flow instead; there is no C route and refusing is how
   that stays true. */
JS_EXTERN JSValue JS_NewSettledPromise(JSContext *ctx, bool is_reject, JSValueConst value);

JS_EXTERN JSValue JS_NewSymbol(JSContext *ctx, const char *description, bool is_global);

typedef enum JSPromiseHookType {
    JS_PROMISE_HOOK_INIT,     // emitted when a new promise is created
    JS_PROMISE_HOOK_BEFORE,   // runs right before promise.then is invoked
    JS_PROMISE_HOOK_AFTER,    // runs right after promise.then is invoked
    JS_PROMISE_HOOK_RESOLVE,  // not emitted for rejected promises
} JSPromiseHookType;

// parent_promise is only passed in when type == JS_PROMISE_HOOK_INIT and
// is then either a promise object or JS_UNDEFINED if the new promise does
// not have a parent promise; only promises created with promise.then have
// a parent promise
typedef void JSPromiseHook(JSContext *ctx, JSPromiseHookType type,
                           JSValueConst promise, JSValueConst parent_promise,
                           void *opaque);
/* THE NAME OF ONE QUEUED CALLBACK, and the whole reason the two entries below answer rather than return void.
   HTML has TASK TRACKERS — §4.11.1's details, §4.11.4's dialog and §6.12's popover each hold one, and each
   says "remove element's <x> toggle task tracker's task from its task queue" so that N transitions in one turn
   COALESCE into ONE event carrying the ORIGINAL old state. A queue whose entries have no identity cannot obey
   that sentence, and this is the identity: MONOTONE, allocated at the enqueue, never reused within a runtime.
   MONOTONE RATHER THAN A POINTER because a handle OUTLIVES what it names — the tracker slot holds it across
   scheduler steps and the task may have run, been dropped with its document, or gone with a paged-out flow in
   between. A pointer would then be a use-after-free with no way to tell; an integer nobody re-issues names
   nothing at all, which is exactly what "remove an already-run task" must mean. JS_TASK_HANDLE_NONE is the
   never-issued value, so a zeroed field is honestly "no task". */
typedef uint64_t JSTaskHandle;
#define JS_TASK_HANDLE_NONE ((JSTaskHandle)0)

/* WHICH TASK SOURCE QUEUED ONE TASK — HTML §8.1.7.1 "Definitions"' `source` field, CARRIED BY THIS FORK AND
   NEVER INTERPRETED BY IT. §8.1.7.1 requires that "For each event loop, every task source must be associated
   with a specific task queue", and a host whose work is split across several carriers cannot check that unless
   the source travels with the work item — so it travels here, beside `is_task` and `handle`, for the same
   reason those do: a task that lands on `baseline_call_list` is handed to the enqueue hook LATER, and a fact
   the entry does not hold is a fact the handover loses.
   IT IS AN OPAQUE SCALAR AND NOT AN ENUM, WHICH IS A DECISION ABOUT THE DEPENDENCY AND NOT ABOUT TASTE. The
   values are the BROWSER's — one per task source some standard defines — and this fork includes no host header
   (quickjs.c's ECMA-402 banner states that direction and why keeping it is the SMALLER delta), so naming them
   here would be a second copy of a list the host already owns and the copy that drifts. Nothing in this file
   reads the value; it is stored, carried and handed back.
   THE ONE VALUE THIS FORK DOES STATE IS THE ABSENCE OF ONE, because a MICROTASK has no task source at all
   (§8.1.7.1 gives a source to a TASK) and every enqueue this file makes on its own account is a microtask: a
   promise reaction, a thenable job, a FinalizationRegistry cleanup. Zero, so that a zeroed field is honestly
   "no source stated" rather than silently claiming whatever the host's list happens to put first — the same
   reason the host's own sentinel is zero, and the one value the two ends have to agree on. */
typedef int JSTaskSource;
#define JS_TASK_SOURCE_UNSTATED ((JSTaskSource)0)

/* Enqueue `func(arg)` as a JOB that runs as a CALL-ROOT FLOW — the platform's route from a host edge to a page
   callback (an event listener, a timer). Not a JS_Call: the callback is the page's code and must be able to
   loop, await and fork, which a C activation cannot host. Answers the queued callback's HANDLE.
   IT MAY BE CALLED BEFORE THERE IS A FRONTIER, exactly as the TASK entry below may. An earlier version of this
   comment said otherwise — that a microtask is always queued by RUNNING script and so always has a flow to own
   its callback — and HTML §8.1.7.3 is the counterexample in one sentence: "when an algorithm running in
   PARALLEL is to await a stable state, the user agent must QUEUE A MICROTASK". §4.8.11.5 step 4 is such an
   await and §4.8.11.2 invokes it IMMEDIATELY for a `<video src>` in a document's initial markup, so the user
   agent queues that microtask while it is still creating the Document. Such a callback is BASELINE work and
   waits on the runtime for the first flow whose host OWNS the queues to adopt it, like the task below. */
JS_EXTERN JSTaskHandle JS_EnqueueCallJob(JSContext *ctx, JSValueConst func, int argc, JSValueConst *argv);
/* THE SAME, ON A TASK SOURCE rather than the microtask queue — HTML 8.1.7's other half. A platform edge that
   the spec words as "queue a task" (8.7 "Timers"'s timer task source, a queued event fire, a delivered reply) uses this
   one, and the event loop will not begin it until every microtask outstanding has run. Choosing the wrong one
   is not a performance detail: it reorders what the page observes.
   IT MAY ALSO BE CALLED BEFORE THERE IS A FRONTIER: the user agent queues a task whenever it likes, including
   while it is CREATING a Document — HTML §4.8.5's insertion steps for an `<iframe src>` in a page's initial
   markup queue §7.4 step 14's navigation, and in this engine that happens at qjs_init, before the scheduler is
   seeded. Such a task is BASELINE work and waits on the runtime for the first flow WHOSE HOST OWNS THE
   QUEUES (JS_SetJobEnqueueHook) to adopt it (`baseline_call_list` in quickjs.c). A flow driven by a host that
   installs no hook asks and is told `not yet` — that is ORDER and not loss, which is why the adoption point
   reports nothing and JS_FreeRuntime aborts on a runtime freed still holding one. It is never dropped
   silently, and no embedder pump ever runs it — the callback belongs to a flow's timeline.
   ANSWERS THE TASK'S HANDLE, which is what a §4.11.4-shaped task tracker stores: the caller keeps it, and hands
   it to JS_RemoveQueuedTask when the next transition must take this task back off the queue.
   `source` IS §8.1.7.1's OWN FIELD AND IS THE CALLER'S TO STATE — see JSTaskSource. It is a PARAMETER and not
   a bracket the caller sets around this call, because a parameter is a compile error for a producer that has
   not thought about the question and a bracket is a thing a producer can forget; and it is on THIS entry
   rather than on JS_EnqueueCallJob because a microtask has no source to state. */
JS_EXTERN JSTaskHandle JS_EnqueueCallTask(JSContext *ctx, JSValueConst func, int argc, JSValueConst *argv,
                                          JSTaskSource source);
JS_EXTERN void JS_SetPromiseHook(JSRuntime *rt, JSPromiseHook promise_hook,
                                 void *opaque);

/* is_handled = true means that the rejection is handled */
typedef void JSHostPromiseRejectionTracker(JSContext *ctx, JSValueConst promise,
                                           JSValueConst reason,
                                           bool is_handled, void *opaque);
JS_EXTERN void JS_SetHostPromiseRejectionTracker(JSRuntime *rt, JSHostPromiseRejectionTracker *cb, void *opaque);

/* return != 0 if the JS code needs to be interrupted */
typedef int JSInterruptHandler(JSRuntime *rt, void *opaque);
JS_EXTERN void JS_SetInterruptHandler(JSRuntime *rt, JSInterruptHandler *cb, void *opaque);

/* THE ALLOCATOR'S REFUSAL EDGE — the embedder's last chance to give memory back before an allocation fails.
 *
 * WHY IT IS HERE AND NOT AT THE CALL SITES. An embedder that keeps a releasable working set (APIClient's
 * frontier of parked flows, each a COW delta plus a suspended frame chain) has exactly one honest moment to
 * page part of it out: the moment the runtime cannot satisfy a request. Anything earlier is a watermark — a
 * number someone picked, which truncates work while memory remains — and anything later is a failed
 * allocation the embedder never got to prevent. Every refusal inside js_malloc_rt / js_calloc_rt /
 * js_realloc_rt asks this hook, so an embedder cannot be surprised by WHICH allocation happened to be the one
 * that hit the floor; there is no list of sites to keep up to date.
 *
 * THE CONTRACT. `wanted` is the size the runtime is trying to obtain. Return non-zero if the callback FREED
 * something, and the runtime retries the allocation — repeatedly, for as long as the callback keeps saying it
 * freed something, so the loop's termination is the embedder's own "there is nothing left to give" and never a
 * count of attempts. Return 0 for that answer, and the allocation fails exactly as it would with no hook
 * installed. That is the physical floor.
 *
 * WHAT THE CALLBACK MAY DO. It runs INSIDE an allocation, so it may FREE (js_free_rt, JS_FreeValue) and it may
 * allocate through the SYSTEM allocator, but it may not re-enter the runtime to run JS: a nested refusal is
 * answered 0 without consulting the callback again, and a refusal raised while the collector owns the object
 * graph (rt->gc_phase) is never offered to it at all. Both are enforced here rather than left to the embedder,
 * because both are properties of the runtime's state and not of the callback's. */
typedef int JSMemoryReclaimFunc(JSRuntime *rt, void *opaque, size_t wanted);
JS_EXTERN void JS_SetMemoryReclaimHook(JSRuntime *rt, JSMemoryReclaimFunc *cb, void *opaque);

/* ASK THE EDGE DIRECTLY — for the OTHER allocators an embedder has. The runtime's own is not the only one that
   can hit the floor: a host that parses HTML with lexbor, or links any library whose allocator it can replace,
   reaches the wall in whichever allocator happens to ask first, and a refusal there is exactly as physical as a
   refusal here. Routing it through this entry rather than calling the embedder's callback directly is what
   keeps ONE mechanism: the same one-deep latch (so a free that allocates cannot recurse into the reclaim), the
   same refusal while the collector owns the graph, and the same installed hook. Answers non-zero if the
   callback freed something, i.e. whether the caller should retry its own allocation. */
JS_EXTERN int JS_ReclaimMemory(JSRuntime *rt, size_t wanted);

/* APIClient forced-execution FLOW-CONTROL hooks — the scheduler's control over interpreter execution: the
   points at which the forced-exec engine steers a running flow, plus the one OBSERVATION of it whose lifetime
   is the same (`work`, below, and its own note says why it lives here). Each member is documented beside its
   declaration; a COUNT of them stated here would be one more thing to keep true, and it was already false.
   One concern, one owner (the scheduler), one registration (JS_SetFlowControlHooks); not optional, a NULL
   argument crashes.
     - branch:  the frame-agnostic branch decision. Returns the arm (0/1) to take when branching on a concolic
                value (the hook forks the OTHER arm as a sibling flow; arm | 0x100 additionally requests a
                frame-snapshot fork), or -1 if the value is not concolic (fall through to the normal ToBool).
     - fork:    the frame-snapshot fork. When branch returned arm | 0x100, the interpreter hands the host a
                CLONE of the flow frame taken at the forking branch; the host builds the hot sibling from it.
     - preempt: the preemption yield, and the ONE policy behind it. A flow runs as a preemptible (heap-resident)
                async-function frame; the interpreter polls a yield request at EVERY opcode boundary
                (JS_RequestFlowYield) and asks this hook there — nowhere else. Returning 1 parks the flow, which
                resumes later at that exact opcode. Where a flow MAY park is the engine's business and is
                universal; WHEN it should is this hook's, and nothing about the page's bytecode shape gates
                either. The substrate for the WFQ to interleave flows by value instead of running each to
                completion. */
/* "THIS ASK MAKES NO CLAIM ABOUT WHICH COMPLETION A REAL SESSION REACHES" — the one value outside 0..n-1 that
   an outcome fork's `real` may take, and a POSITIVE STATEMENT rather than the absence of one.
   `real` is the completion this machine's operation reaches when RUN ON THE OPERAND'S EXAMPLE. A branch has
   that answer for free: the condition carries a concrete example and coercing it to a boolean IS the arm a
   session carrying real values takes, which is what makes one arm PRIMARY and the other FORCED. An outcome has
   no condition and its arms are not booleans — they are numbered COMPLETIONS of a real operation — so nothing
   at the seam can derive it: the seam holds the operand and the operation's NAME, never its semantics, and
   deciding from the name would be a recognizer. Only the MACHINE can say, at its own definition, and this is
   what it says when it cannot: `JSON.parse` over an operand carrying no example has no completion to report,
   and an ask whose operand is example-free has none by construction.
   IT IS NOT "OUTCOME 0". Outcome 0 is what a run with NO FORKING POLICY takes, which is a statement about this
   engine's numbering; `real` is a statement about a session carrying real values, and answering the second
   with the first would invent which completion such a session reaches — the same invention as picking an arm
   for an example-free branch. A machine that cannot say passes this and behaves exactly as one that could not
   be asked: the fork happens, both arms run, neither is marked forced. */
#define JS_OUTCOME_REAL_UNSTATED (-1)
typedef struct JSFlowControlHooks {
    int  (*branch)(JSContext *ctx, JSValueConst cond);
    /* A NATIVE OPERATION WHOSE COMPLETION IS ONE OF N FEASIBLE OUTCOMES — the same decision `branch` makes,
       asked from where there is no OP_if to make it at.
       `branch` needs a bytecode branch: a condition on the operand stack, an `if_pc` to resume a sibling at, a
       frame to snapshot. A C builtin has none of those, and its completion over UNKNOWN input is still a
       decision the flow must not take on its own: `JSON.parse(x)` on unknown text either yields a value or
       throws a SyntaxError, both feasible, and picking one DELETES the other arm. That is why this engine
       ABORTED at those builtins rather than answer — an abort is the honest state for a missing fork, a
       fabricated value is not.
       `over` is the unknown operand the outcome depends on and `op` names the operation ("JSON.parse"), which
       is everything the constraint key needs: the solver builds it from the operand's own source identity
       exactly as it builds a comparison's, so no grammar and no policy crosses into the engine. `n` is how
       many completions the machine declares feasible.
       `real` is the ASKING MACHINE'S OWN DECLARATION of the completion its operation reaches when run on the
       operand's example, or JS_OUTCOME_REAL_UNSTATED (above) when it cannot say. It is a parameter for the
       same reason `branch`'s arm is computed by the seam and this one is not: the answer belongs to the
       operation, and no two operations answer it alike. It decides which arm the parent KEEPS and whether the
       arm a flow ends on is FORCED, so a machine that states it makes an outcome fork report its provenance
       exactly as a branch does.
       Returns the outcome THIS flow takes (0..n-1), ORed with 0x100 when a sibling was prepared for another —
       the same protocol `branch` uses, so the fork is one mechanism and not two. -1 means there is no decision
       to make (no forking policy installed: the @S candidate re-fire runs ONE concrete path), which the caller
       reads as outcome 0, the same way a declined `branch` falls through to the ordinary ToBool. */
    int  (*outcome)(JSContext *ctx, JSValueConst over, const char *op, int n, int real);
    void (*fork)(JSContext *ctx, JSValue *clone);       /* BASE-activation fork: build the hot sibling from a frame clone.
                                                           (The deleted `replay` hook re-ran a nested/deep flow from its
                                                           start — BANNED, not byte-identical; that fork now DFAILs until a
                                                           sound async-frame snapshot is built.) */
    /* WHAT RAISED the yield request this point is answering, so a policy can answer per source. Parking is ONE
       mechanism and there is now exactly ONE place that asks this question — the per-opcode poll in the
       interpreter's dispatch (see JS_RequestFlowYield). The kind is not where the flow may park (every opcode
       boundary is), it is who wanted the thread, which is a real difference to a policy: "force every request"
       means wildly different amounts of work when the source is a loop back-edge (once per iteration) and when
       it is a call (millions of times), and a policy that cannot tell them apart must answer the same for both.
       run-test262 forces every back-edge and samples calls for exactly that reason; the solver's own policy is
       rank + wall-clock and does not care which source asked. */
#define JS_PREEMPT_BACKEDGE 0
#define JS_PREEMPT_FORK     1
#define JS_PREEMPT_CALL     2
/* THE HOST ASKED FOR THE THREAD — JS_RequestFlowYield, from outside the running flow's own code. This is the
   source that has no bytecode shape behind it at all: the cooperative quantum expiring, a rank change on the
   frontier (an emit, a sibling appearing, a flow blocking on the host), RAM pressure wanting the cold tail, a
   cross-instance read needing the flow suspended. */
#define JS_PREEMPT_HOST     3
    int  (*preempt)(int kind);
    /* IS THE RUNNING FLOW'S COOPERATIVE SLICE SPENT — the same question `preempt`'s own policy answers on its
       quantum clause, asked from the interpreter's DISPATCH so that the budget can expire inside a stretch of
       bytecode that raises nothing. It is a SECOND QUESTION AND NOT A SECOND POLICY: `preempt` answers "should
       this flow park", which is rank AND budget, and this answers the budget alone — because the only thing a
       dispatch does with the answer is RAISE, and the raise is then answered by the poll through `preempt`
       exactly as a back-edge's is. One park decision, still in one place.
       WHY IT EXISTS: solver/quantum.h's raise sources were a loop back-edge, a call and a fork, and all three
       are occasions THE PAGE'S OWN CODE SHAPE supplies or withholds, so a straight-line call-free stretch could
       not expire the slice it was supposed to be bounded by. A count of the interpreter's own dispatches is
       UNEVADABLE — a page cannot write bytecode that dispatches without dispatching — which is the property
       that header states and which is not restated here.
       `budget_period` IS A GRANULARITY AND NOT A BUDGET. It says how often the question is ASKED; the answer is
       the hook's, and the hook's is a CLOCK. It buys the one thing a period can buy: on a host whose clock read
       is a call into JS, asking at every opcode is unaffordable, and asking every N amortises it while bounding
       the OVERSHOOT at N dispatches. The raise is a CONJUNCTION with the hook's answer, so the raises taken with
       the period are a SUBSET of those taken without it — which is the test g_flow_work_retired's declaration in
       quickjs.c states in full, applied at the countdown that gates this call.
       A DISPATCH-DENOMINATED BUDGET WOULD BE A DIFFERENT THING AND IS BANNED, and the reason bounds what this
       can reach too: the dispatch count has ONE increment site, inside DISPATCH, so a C activation that declares
       no step boundary retires zero of it however long it runs. That is the same population no raise source
       reaches, and it is why solver/engine.c charges its aging term in thread time and not in work.
       INSTALLED AND REMOVED WITH `preempt`, by one owner in one registration, for `work`'s reason below: a
       budget is a fact about a scheduler session and about nothing else. A NULL `budget` is a host that declines
       this edge — wpt_runner.c drives flows under its own policy with no frontier to be fair between — and it
       costs that host one predicted-not-taken branch per dispatch and nothing else. */
    int      (*budget)(void);
    uint32_t budget_period;
    /* THE WORK THE RUNNING FLOW HAS RETIRED since this hook last saw it, in OPCODES — reported at the yield
     * poll, immediately BEFORE `preempt` is asked, so a flow that parks there has already banked the work it
     * did to reach that point. `units` is never 0 — a poll at which nothing was retired reports nothing rather
     * than an advance of zero, which is a real shape and not a corner (a flow resumed with a host request
     * already standing reaches its first dispatch having retired nothing). Nothing in the interpreter reads
     * the number back.
     *
     * IT IS AN OBSERVATION AND NOT A CONTROL, which is the one thing that could be misread from its home in
     * this struct. It is HERE because its correctness is a lifetime property it shares with `preempt` and with
     * nothing else: the count is only attributable while a scheduler is running flows, so the hook must be
     * installed and removed with the policy, by the same owner, in the same registration. Two registrations
     * would be two things that have to agree, and the day they disagreed the symptom would be one flow's work
     * on another flow's clock — a wrong number, silently.
     *
     * WHY THE UNIT IS OPCODES RETIRED AND NOT ONE OF THE TWO QUANTITIES BESIDE IT. An agent-global work total
     * (forks + flows + jobs + switches) moves because a SIBLING forked, so a flow's own reading would depend
     * on what else the frontier was doing; the count of times this poll was REACHED is a fact about the
     * machine, since the cooperative quantum raises the request asynchronously and "last raise wins" means a
     * raise landing on an already-raised byte adds no poll while one landing on a clear byte adds one.
     * Opcodes retired is neither: it is what this flow DID, in the order its own bytecode says, identical on
     * every schedule and across a park.
     *
     * WHICH IS WHAT MAKES IT A CLOCK. This engine has no wall clock to wait on and must not acquire one — a
     * real clock is a function of the machine and the load average, so every timestamp under it would be a
     * disagreement the solver differential reports as a scheduling bug, and a flow parked to the cold tier
     * would resume into a clock that moved without it. HIGH RESOLUTION TIME Level 3 §2.1 Clocks asks of the
     * monotonic clock only that it never decrease and that it exist within one execution of the user agent;
     * matching real-world time is what all clocks "attempt", explicitly not what they achieve.
     *
     * THE CONSUMER MUST ACCUMULATE IT EXACTLY and divide once at the read, never fold each batch into a
     * floating-point moment as it arrives: addition is not associative, so a folded moment would depend on how
     * the batches were SPLIT, and the split is the schedule-dependent poll count above. */
    void (*work)(JSContext *ctx, uint64_t units);
} JSFlowControlHooks;
JS_EXTERN void JS_SetFlowControlHooks(const JSFlowControlHooks *hooks);
/* THROW AWAY the retired-work count standing in the interpreter, attributing it to nobody — called by the
   scheduler where it switches a flow IN, which is the last instant at which anything standing there is by
   construction NOT the incoming flow's. Two things can be standing and neither is: work the HOST retired
   between two flows, and the residue of a flow that FINISHED without parking (one that parks banks at the poll
   first, so its residue is zero). A finished flow's clock is never read again and host time is no flow's time,
   so both are discarded rather than banked — which is what makes "the count `work` receives is the running
   flow's own" a property of the mechanism instead of a hope about call order. */
JS_EXTERN void JS_FlowDiscardRetiredWork(void);

/* APIClient TIME-TRAVEL (record/replay) hooks — the RECORD boundary of the COW time-travel executor.
   A flow's live state is (shared baseline ∘ its per-flow COW delta); rewinding a flow reverses that delta,
   replaying re-applies it. For that to be EXACT the delta must observe every mutation of SHARED baseline state
   at the instant it happens, and these callbacks ARE that observation boundary: the interpreter invokes them
   immediately BEFORE it mutates shared heap state, so the host records the pre-write value into the running
   flow's delta. Two mutation classes cover the JS heap: a normal property write (obj[atom]) and a closure CELL
   write (a captured local's V8-Context cell — a JSVarRef, not a property, so it needs its own edge). Object
   CREATION needs no separate hook: prop_write also fires before a creating write, and the host records the slot
   as "absent" so a rewind DELETES it. A flow-LOCAL object (one stamped with a generation ABOVE the delta's
   fork generation — see JS_SetFlowGen) is never shared and is skipped in prop_write's gate, so a delta stays
   O(shared state actually written). The converse is what makes the skip sound and is the reason that stamp is
   the scheduler's to bracket: an object created while NO flow is running is baseline, is shared by
   construction, and must never be skipped by anybody. Install the whole boundary ONCE with JS_SetTimeTravelHooks at engine setup — it is not
   optional and a NULL argument crashes. Baseline setup runs uncaptured NOT by leaving the hooks unset but
   because there is no CURRENT flow to capture into yet (the host routes captures to the running flow's delta). */
/* The generator-fork hook's type, named because the HOST's COW layer takes it as a parameter: the primitive
   that installs the table does not own this one entry — whoever assembles the sibling flow does. */
typedef void (*JSTimeTravelGenFork)(JSContext *ctx, JSValueConst genobj, void *base_gd, void *cur_gd);

typedef struct JSTimeTravelHooks {
    void (*prop_write)(JSContext *ctx, JSValueConst obj, JSAtom atom);  /* before writing a shared obj[atom] */
    void (*cell_write)(JSContext *ctx, void *cell);                     /* before writing a shared closure cell */
    /* Before a fast-array APPEND creates obj[idx] (idx == the dense count). The slot's BASELINE is absent, so
       the host needs no baseline lookup for it (existed=0 always) — that, and not the absence of a dedup, is
       what makes this the accumulator's hot path. It still dedups (O(1), on the hash index): a define on the
       same index captures the slot first, and a push/pop loop would otherwise add an entry per iteration for
       one slot, which is the O(shared-state-touched) invariant broken over a single element.
       The array's `length` is captured through prop_write immediately before this fires — an append writes the
       length too, and only sometimes, so it is a slot with its own entry rather than a number derived from
       these. See cow_capture_append. */
    void (*arr_append)(JSContext *ctx, JSValueConst obj, JSAtom atom);
    /* A concolic branch inside a synchronously-driven GENERATOR body forked the flow: clone_deep_flow built a
       fresh per-flow gen_data CLONE (cur_gd) of the shared generator object's execution state (base_gd is its
       current, object-owned state). The host records a per-flow gendata swap on the forking sibling's delta so
       genobj->[[GeneratorState]] resolves to cur_gd while the sibling runs and to base_gd otherwise; the sibling
       delta OWNS cur_gd (JS_GenDataRef/Unref). Only fires for a direct .next() drive (genobj is a generator). */
    JSTimeTravelGenFork gen_fork;
    /* Before a NEW record (key not already present) is added to a shared Set/Map (Set.add / Map.set of a fresh
       key). The host records the KNOWN-NEW add on the current flow's delta so a snapshot-forked sibling stays
       isolated: unapply deletes the flow's added record, apply re-adds it (JS_MapAddRecord / JS_MapDeleteRecord).
       O(1), the accumulator hot path for `new Set(gen)` / `[...set]`-building — mirrors arr_append for arrays.
       An add APPENDS, so its replay appends and it carries no position. The overwrite and delete of a baseline
       record are map_mutate's, below — a sentence here used to say they "are not yet captured", which stopped
       being true when that hook was added and then read as an instruction to build it again. */
    void (*map_add)(JSContext *ctx, JSValueConst obj, JSValueConst key, JSValueConst val);
    /* Before an OVERWRITE (Map.set of an existing key: op=1, old=the current value, val=the new value) or a DELETE
       (Set.delete / Map.delete / Set.clear / Map.clear of a present record: op=2, old=the current value, val
       ignored) on a shared Set/Map. The host records a reversible undo-log entry so a snapshot-forked sibling
       stays isolated: apply replays the op, unapply inverts it (overwrite restores old; delete re-adds old).
       Completes map_add's add-only capture.
       `pos` is WHERE the inverse must put the record back: the number of LIVE records preceding it for a DELETE,
       and JS_MAP_POS_TAIL for an OVERWRITE, whose inverse creates nothing and therefore places nothing. */
    void (*map_mutate)(JSContext *ctx, JSValueConst obj, JSValueConst key, JSValueConst old_val, JSValueConst val, int op, int pos);
    /* Before a flow changes a shared object's OWN STATE — the state that is neither a property slot nor the
       class's opaque record: its EXTENSIBLE bit, its PROTOTYPE, and its [[PrimitiveValue]]/[[DateValue]]/
       [[ErrorData]] internal slot. No property hook and no engine hook could see any of them, so
       `Object.freeze(sharedCfg)`, `Object.setPrototypeOf(shared, x)` and `d.setHours(0)` on a baseline Date
       were each permanent for every sibling. One hook for the three because they are one object; the host
       captures per object and puts back what it saved (JS_ObjStateSave / Restore / Free). */
    void (*obj_state)(JSContext *ctx, JSValueConst obj);
    /* Before a flow ADVANCES a shared ITERATION RECORD — the engine's own class opaque behind an iterator
       object (`p->u.for_in_iterator`, `p->u.array_iterator_data`, and the siblings that have no arm yet),
       whose cursor says how far through the walk this object is. `obj` is the ITERATOR object.
       IT IS NOT obj_state's UNIT AND NOT prop_write's, which is why it is a hook and not a widening: an
       iteration cursor is neither a property slot nor one of the three fields every object has, and
       cow_capture_obj draws that boundary itself, in the last sentence of its own comment:
       `What is NOT here is the class's own record ... those have their own units.`
       This is that unit for the records the ENGINE owns, as cow_capture_host_record is for the ones a browser
       component owns; the difference is only who can see the layout, which is why the blob is the engine's
       (JS_IterStateSave / Restore / Free) rather than a caller-supplied offset list.
       WHY IT IS NEEDED AT ALL, since an iterator looks like the most flow-private object there is: a snapshot
       fork COPIES THE FRAME and clone_susp_frame dups each operand stack slot, so a JSValue is a REFERENCE and
       both arms walk ONE record. The cursor is then shared, each arm's advance moves the other's, and the two
       arms SPLIT one enumeration — each reporting a proper subset, with no abort and nothing to say so. That
       is a wrong answer rather than a crash, and §Solver-half makes it the common case rather than an edge
       one: an iteration over unknown input forks each iteration as its own parkable flow, so a `for...in` over
       a server-injected record is precisely the shape that forks inside the loop.
       WHAT IS CAPTURED IS THE MUTABLE HALF AND THE IMMUTABLE HALF IS ASSERTED, not saved-and-restored: a
       for-in iterator's receiver, its array-ness and its array length are written once at construction and
       never again, so a blob that put them back would be describing a fact rather than restoring a write. The
       save holds them anyway and the restore compares them, which is what makes a future write to any of them
       fail loudly here instead of being dropped on every context switch. */
    void (*iter_state)(JSContext *ctx, JSValueConst obj);
    /* Before a flow changes the ASYNC STATE of a shared object: a promise leaving PENDING, a REACTION being
       attached to one that is still pending, or a resolving-function pair latching already_resolved. The
       reaction list belongs here for the same reason the settlement does — `if (flag) p.then(h1); else
       p.then(h2);` attaches h1 in one arm only, and without capturing the attach the other arm ran h1 too. A promise created before a fork is shared baseline
       state like any other object, and settling it is a WRITE — but one no property hook can see, because it
       lives in the promise's internal slots and its reaction list. Without this the first arm to resolve wins
       and every sibling's resolve is a silent no-op, so a value that differs per arm is simply lost.
       The host captures the object's whole settlement with JS_AsyncStateSave and puts it on the running flow's
       delta; the swap restores it. Two classes, one hook, because "the settlement state of this shared async
       object" is one concept and which internal slots that means is the engine's business. */
    void (*async_state)(JSContext *ctx, JSValueConst obj);
    /* Before a flow changes a MODULE's evaluation state (16.2.1.5.3's status/capability/cycle fields). A
       module's BINDINGS are closure cells and already ride the delta through cell_write, but its evaluation
       state did not — so the first flow to evaluate a module left the record EVALUATED for every sibling, and a
       sibling that forked before the import skipped evaluation and read the exports it never wrote: TDZ
       (`region is not initialized`) instead of its own values. Under forced multi-path execution each flow is a
       possible world and each world evaluates the module once, so the state is per-flow like every other write.
       `m` is an opaque JSModuleDef*; JS_ModuleEvalStateSave/Restore is what "that state" means. */
    void (*module_eval)(JSContext *ctx, void *m);
    /* Before a flow RESUMES a shared suspended async activation — which consumes it. `base_data` is the
       activation every other arm still finds, `cur_data` a clone this flow owns; the host installs the clone on
       `closure` for the duration of this flow and records the swap on its delta, exactly as gen_fork does for a
       generator's execution state. The delta OWNS cur_data (JS_AsyncDataRef/Unref). */
    void (*async_fork)(JSContext *ctx, JSValueConst closure, void *base_data, void *cur_data);
    /* Before a flow changes a shared ARRAY BUFFER's STORAGE STATE — any of its BYTES, or the EXTENT those bytes
       live in (`resize`/`grow`, `transfer`, a detach). `abuf` is the ArrayBuffer/SharedArrayBuffer OBJECT —
       never a view and never a raw pointer — because the buffer is what OWNS the storage and is the only name
       every writer of it agrees on.
       THE UNIT IS THE BUFFER'S BYTES AND NOT A VIEW'S ELEMENT, for three reasons the code decides rather than
       taste. (1) A DataView write has NO element: `dv.setFloat64(3, x)` writes bytes 3..10 of the buffer, a
       DataView exposes no indexed properties at all, and those bytes cross the element boundary of every view
       over that buffer — so an (object, index) slot entry cannot even name the write. (2) A Float64Array
       element cannot round-trip through a JSValue: under JS_NAN_BOXING — which is every 32-bit build, and the
       wasm one is 32-bit — __JS_NewFloat64 NORMALISES a NaN, so a payload the page stored comes back canonical
       and §Time-travel's byte-identical resume is not. (3) `fill`/`set`/`copyWithin`/`sort`/`reverse`/Atomics
       write through memmove/memset over the raw storage, where there is no per-element hook to hang a capture
       on and one entry per element would cost sizeof(CowEntry) per BYTE.
       Aliasing follows from the unit rather than being handled by it: a Float64Array and a Uint8Array over one
       buffer are one entry, because they are one storage. The host captures the buffer's state ONCE per flow
       (JS_BufferStateSave/Restore are the read/write twins the swap uses), so a loop overwriting one element a
       million times costs one entry — the delta stays O(shared state TOUCHED), which an undo log of ranges
       would not.
       CONTENTS AND EXTENT ARE ONE HOOK BECAUSE THEY ARE ONE STORAGE, and this was two. `resize`/`transfer`/
       detach had a hook of their own whose host arm could only ABORT, because an entry holding `a_len` bytes
       read off the buffer describes a length a resize takes away and names storage a detach frees. Splitting
       them cannot be repaired by giving the second one an entry too: two entries over one buffer have an ORDER,
       and a replay that puts the flow's bytes back before its length has an extent the bytes do not fit. So the
       blob is the buffer's WHOLE storage state and a flow that reaches any part of it may write any other —
       exactly the argument obj_state's one kind for three fields makes.
       WHY THE SITE IS THE MUTATION AND NOT THE SAVE, for the extent half. A save-side check can only fire for a
       buffer this flow had ALREADY captured, which is one of the two orderings. Resize FIRST and write SECOND
       and the entry is created after the fact, over the post-resize bytes: no length ever disagrees, nothing
       fires, and the sibling silently inherits both the new size and the write. A zero-length resizable buffer
       is only the extreme of that ordering (there is no byte to write before the resize, so it is ALWAYS the
       silent one) and not a separate case. Raised here, the ordering stops mattering, because the mutation
       cannot happen without passing this. */
    void (*buf_state)(JSContext *ctx, JSValueConst abuf);
} JSTimeTravelHooks;
/* A BUFFER'S WHOLE STORAGE STATE, as an opaque owned blob — the buffer twin of JS_ObjStateSave, and engine-owned
   for the same reason: two of the things it holds are engine-internal and one of them is a REFERENCE.
 *
 * WHAT IT HOLDS is what the three operations of §25.1.6.6 ArrayBuffer.prototype.resize, §25.1.3.5
 * DetachArrayBuffer and §25.2.5.3 SharedArrayBuffer.prototype.grow between them change: the [[ArrayBufferData]]
 * block (its BYTES, copied — a restore must be able to hand storage back that a detach freed), the
 * [[ArrayBufferByteLength]], and whether the buffer is detached at all. §25.1.6.6 steps 14 and 15 are "Set
 * obj.[[ArrayBufferData]] to newBlock." and "Set obj.[[ArrayBufferByteLength]] to newByteLength."; §25.1.3.5
 * steps 4 and 5 are "Set arrayBuffer.[[ArrayBufferData]] to null." and "Set
 * arrayBuffer.[[ArrayBufferByteLength]] to 0."; and §25.1.6.8 ArrayBuffer.prototype.transfer step 2 is "Return
 * ? ArrayBufferCopyAndDetach(obj, newLength, preserve-resizability)", whose §25.1.3.3 step 15 is "Perform !
 * DetachArrayBuffer(arrayBuffer)." — so a transfer is a detach of THIS buffer plus a new one, and only the
 * detach half is this buffer's state.
 *
 * AND THE VIEWS, WHICH ARE NOT A SECOND ENTRY. Per the specification a view carries no state a resize changes:
 * §25.3.1.3 GetViewByteLength step 3 is "If view.[[ByteLength]] is not auto, return view.[[ByteLength]]." and
 * step 8 "Return byteLength - byteOffset.", so a length-tracking view's extent is DERIVED at every read from
 * the buffer's current one. This engine CACHES that derivation — `p->u.array.count`/`.u.ptr` on every typed
 * array and `ta->length` on a tracking DataView — and js_array_buffer_update_typed_arrays re-derives them, so
 * the restore re-derives rather than storing a second copy of a computed fact. ONE cached field does not
 * re-derive: that update leaves a tracking DataView's `ta->length` ALONE when the view's offset lands at or
 * past the new length, so the value it keeps is history rather than a function of the buffer, and it is read
 * back by js_dataview_getValue's bounds test. That one is recorded, for THOSE views only, and recording it is
 * why the blob holds a counted reference on each of them rather than a bare pointer — a delta outlives a flow,
 * so a view the page dropped would otherwise be a dangling name in it. Every other view costs the blob nothing,
 * which is what keeps the hot byte-write path free of per-view work. */
JS_EXTERN void *JS_BufferStateSave(JSContext *ctx, JSValueConst obj);
JS_EXTERN void  JS_BufferStateRestore(JSContext *ctx, JSValueConst obj, void *blob);
JS_EXTERN void  JS_BufferStateFree(JSRuntime *rt, void *blob);
/* The evaluation state of a module record, as an opaque owned blob — the module twin of JS_AsyncStateSave. */
JS_EXTERN void *JS_ModuleEvalStateSave(JSContext *ctx, void *m);
JS_EXTERN void  JS_ModuleEvalStateRestore(JSContext *ctx, void *m, void *blob);
JS_EXTERN void *JS_ModuleEvalStateClone(JSContext *ctx, void *blob);
JS_EXTERN void  JS_ModuleEvalStateFree(JSRuntime *rt, void *blob);
/* The settlement state of a promise (state + result + the pending reaction records) or of a resolving-function
   pair (its already_resolved latch), as an opaque owned blob. NULL for an object that has neither. Restore
   puts it back exactly, so a flow's timeline is reproduced rather than replayed. */
JS_EXTERN void *JS_AsyncStateSave(JSContext *ctx, JSValueConst obj);
JS_EXTERN void  JS_AsyncStateRestore(JSContext *ctx, JSValueConst obj, void *blob);
JS_EXTERN void *JS_AsyncStateClone(JSContext *ctx, void *blob);
JS_EXTERN void  JS_AsyncStateFree(JSRuntime *rt, void *blob);
#define JS_MAP_MUTATE_OVERWRITE 1
#define JS_MAP_MUTATE_DELETE    2
/* The FORK GENERATION stamped onto every object created from now on: 0 = no flow is running (baseline), else
   the running flow's generation. The scheduler owns it at ONE bracket — it opens a slice with the generation
   the previous slice left and suspends it to 0 before returning to the host, so an object the host creates
   between two slices is baseline and its writes are captured by every flow that goes on to share it. It takes
   a NUMBER and not a flag precisely so that bracket is expressible: the counter is monotonic across slices
   (JS_FlowBumpGen moves it at each fork) and restarting it would make a later object compare as older than an
   earlier fork. Replaced JS_SetFlowLocalMark(int), whose boolean nothing ever read. */
JS_EXTERN void JS_SetFlowGen(uint32_t gen);
/* Whether obj was created flow-local. The COW hook consults this: a never-forked flow skips its flow_local
   writes (truly private), but AFTER a fork those objects are shared with the snapshot sibling and must be captured. */
JS_EXTERN int  JS_IsFlowLocal(JSValueConst obj);
/* The running flow's FORK GENERATION, set by the scheduler beside its delta, and the test built on it: is this
   object shared with a sibling (created at or before the last fork) rather than private to this flow? It is the
   same question the delta asks before capturing a write, asked by the engine where a shared ACTIVATION is about
   to be consumed. */
JS_EXTERN void JS_SetFlowForkGen(uint32_t gen);
JS_EXTERN int  JS_IsFlowShared(JSValueConst obj);
/* forced-exec generational flow-local marking: JS_ObjFlowGen returns the fork generation an object was created at
   (0 = baseline); JS_FlowGen the current generation; JS_FlowBumpGen increments it (called by the host at a fork).
   An object is flow-PRIVATE to a delta iff JS_ObjFlowGen(obj) > delta's fork generation. */
JS_EXTERN uint32_t JS_ObjFlowGen(JSValueConst obj);
JS_EXTERN uint32_t JS_FlowGen(void);
JS_EXTERN uint32_t JS_FlowBumpGen(void);
/* Is this captured slot's storage a DENSE array element (a fast Array, index inside its dense count)? Only then
   is removing the slot a shrink of the dense part; every other integer-indexed slot on an Array is an ordinary
   property JS_DeleteOwnSlot removes. Pairs with the shrink below — the delta's element half. */
JS_EXTERN int  JS_IsArrayDenseSlot(JSValueConst obj, JSAtom atom, uint32_t *idx);
/* Remove the dense elements from `count` up — the COUNT and never the `length`, which is a slot with its own
   entry (arr_append captures it). A length write is a different operation and it removes these elements too. */
JS_EXTERN void JS_ArrayTruncateDense(JSContext *ctx, JSValueConst obj, uint32_t count);
JS_EXTERN void JS_SetTimeTravelHooks(const JSTimeTravelHooks *hooks);
/* Per-flow generator-state COW: swap a shared generator object's execution-state pointer and own clones by
   refcount (see JSTimeTravelHooks.gen_fork). The clone is opaque to the host (JSGeneratorData is engine-internal). */
JS_EXTERN void  JS_SetObjAsyncData(JSValueConst closure, void *ad);
JS_EXTERN void  JS_AsyncDataRef(void *ad);
JS_EXTERN void  JS_AsyncDataUnref(JSContext *ctx, void *ad);
JS_EXTERN void  JS_SetObjGenData(JSValueConst genobj, void *gd);
JS_EXTERN void *JS_GetObjGenData(JSValueConst genobj);
JS_EXTERN void  JS_GenDataRef(void *gd);
JS_EXTERN void  JS_GenDataUnref(JSContext *ctx, void *gd);
/* A closure cell is opaque here (JSVarRef is engine-internal); the host reads/writes its value via these. */
/* A captured closure cell is OWNED by whoever holds its pointer across time — a COW delta restores cells on
   every context switch, and the cell's own frames may be long gone by then. */
JS_EXTERN void    JS_VarRefRef(void *cell);
JS_EXTERN void    JS_VarRefUnref(JSContext *ctx, void *cell);
JS_EXTERN JSValue JS_VarRefGetValue(void *cell);
JS_EXTERN void    JS_VarRefSetValue(JSContext *ctx, void *cell, JSValue val);
/* Per-flow Set/Map COW: manipulate a Set/Map's internal record directly (bypassing any JS-level method override),
   so the host's cow apply/unapply can re-add / remove the per-flow record for JSTimeTravelHooks.map_add. `obj`
   must be a Set or Map. Add sets/overwrites the record for `key` (val ignored for a Set); Delete removes it.
   A RECORD'S POSITION IS PART OF ITS STATE, because a Set/Map iterates in INSERTION order and that order is
   observable: `pos` is the number of LIVE records that must precede the re-added one, or JS_MAP_POS_TAIL to
   append — which is what every ordinary add does and what replaying one must do. It is consulted only when the
   record has to be CREATED; an overwrite writes a value into a record already in its place.
   NEITHER RETURNS A STATUS, because the two things the old -1 conflated are answered where they arise: the
   "obj is a Set/Map with live state" invariant is a DCHECK inside, and the re-add's allocation is a CHECK (an
   OOM here loses a baseline record inside a context switch and leaves an InternalError belonging to no flow). */
#define JS_MAP_POS_TAIL (-1)
JS_EXTERN void JS_MapAddRecord(JSContext *ctx, JSValueConst obj, JSValueConst key, JSValueConst val, int pos);
JS_EXTERN void JS_MapDeleteRecord(JSContext *ctx, JSValueConst obj, JSValueConst key);

/* A WEAK COLLECTION'S RECORD, HELD BY THE DELTA, WITHOUT KEEPING ITS KEY ALIVE.
 *
 * The four functions above serve a WeakMap/WeakSet exactly as they serve a Map/Set — `pos` is the one thing
 * that differs, and a weak restore must carry JS_MAP_POS_TAIL because a weak collection is not iterable, so
 * the order of its records is unobservable and there is nowhere to put one back. What a weak entry may NOT do
 * is hold its key the way a strong entry does.
 *
 * A strong entry dups its key, which is right because the collection holds a counted reference too. A weak
 * collection holds its key UNCOUNTED, so a delta that dup'd one would be the only counted reference in the
 * system and the key could never die — and flows are never terminated, so that is for the session, once per
 * flow, for every object an identity map has ever seen. The VALUE is the other half: a weak record's value is
 * reachable only through its key (the ephemeron rule the engine's own mark walk implements), so holding it
 * unconditionally makes any value that can reach its key immortal.
 *
 * So a weak entry holds THIS instead. It is registered on the key exactly as a record is, and it dies by the
 * same walk: when the key goes, the cell is NEUTRALISED IN PLACE rather than freed, because the delta still
 * names it. `JS_CowWeakRefLive` is then false and every operation over the entry is a no-op — which is exact
 * and not lossy, since a dead weak key is unreachable and neither restoring its record nor removing it can be
 * observed by anything left that could ask. It is held exactly ONCE: a fork hands the entry array to a
 * segment and counts the segment, so an entry — and the cell it names — exists once however many arms reach
 * it. The free unlinks the key's record where the key is alive and finds nothing to unlink where it is not. */
typedef struct JSCowWeakRef JSCowWeakRef;
JS_EXTERN JSCowWeakRef *JS_NewCowWeakRef(JSContext *ctx, JSValueConst key,
                                         JSValueConst v_base, JSValueConst v_cur);
JS_EXTERN void          JS_FreeCowWeakRef(JSRuntime *rt, JSCowWeakRef *w);
JS_EXTERN int           JS_CowWeakRefLive(const JSCowWeakRef *w);
JS_EXTERN JSValueConst  JS_CowWeakRefKey(const JSCowWeakRef *w);
/* The two values the entry is made of: unapply writes the base, apply writes the cur. An operation that
   creates nothing on its side leaves that side UNDEFINED — an ADD has no before, a DELETE has no after. */
JS_EXTERN JSValueConst  JS_CowWeakRefBase(const JSCowWeakRef *w);
JS_EXTERN JSValueConst  JS_CowWeakRefCur(const JSCowWeakRef *w);
/* Is this collection weak? The capture side must know, because the two kinds of entry hold their key
   differently and the hooks carry the collection rather than its state. */
JS_EXTERN int           JS_IsWeakCollection(JSValueConst obj);

/* A shared object's OWN state (JSTimeTravelHooks.obj_state), saved into an engine-owned blob and put back on a
   context switch. The blob holds a reference on each owned half, so the same one restores any number of times.
   The restore is a SLOT WRITE and never [[SetPrototypeOf]]: its immutable-prototype refusal, its extensible
   test and its cycle check would each refuse inside a context switch with no flow to throw on. */
JS_EXTERN void *JS_ObjStateSave(JSContext *ctx, JSValueConst obj);
JS_EXTERN void  JS_ObjStateRestore(JSContext *ctx, JSValueConst obj, void *blob);
JS_EXTERN void  JS_ObjStateFree(JSRuntime *rt, void *blob);

/* A shared ITERATION RECORD's state (JSTimeTravelHooks.iter_state), saved into an engine-owned blob and put
   back on a context switch — the iterator twin of JS_ObjStateSave, engine-owned for the reason that covers
   every arm: THE RECORD IS THE ENGINE'S. Each of these structs is private to quickjs.c, so there is no caller
   anywhere who could hand the host's cow_capture_host_record an offset list for one, and that is true whatever
   a particular arm happens to own. The for-in arm has the sharper half of the same argument and it is kept
   because it is the one that would still hold if the layouts were public: what THAT record owns is an ATOM,
   a counted reference `val_off` cannot name at all, since it names JSValues. The array/string arm owns a
   JSValue, which `val_off` COULD have named — so for that one the private struct is the whole reason, which is
   why the general argument is stated first and the atom second.
   THE CLASS DECIDES THE ARMS AND AN UNKNOWN ONE CRASHES IN ALL THREE. Save's default is the gate — a class
   whose opaque record grew a cursor and reached this hook has no arm here yet, and it says so by name — and
   Restore's and Free's are then guards over a value this engine enumerates: Save accepted the class, so a blob
   that exists names a class all three know, and a default reached in either of them means the three lists have
   drifted apart. That is the assert that makes forgetting an arm impossible, and it is why there is no fourth
   place to keep in step.
   NAMED RESIDUAL — WHAT IS NOT COVERED: the for-in iterator and the array/string one have arms. Every other
   engine class whose opaque record carries a cursor its own `.next()` ADVANCES is still shared whole across a
   snapshot fork, and that is a PROPERTY rather than a list — derive today's set with
       grep -nE '^ +struct JS[A-Za-z]+ \*[a-z_]*iterator[a-z_]*;' engine/qjs/quickjs.c
   and read each record for a field its next() WRITES. That second step is the whole of the derivation and not
   a formality: run on this tree it returns records with NO cursor at all — a wrapper holding two JSValues
   written once at construction, which delegates, so the cursor it walks belongs to the wrapped iterator and is
   that object's own arm to have. A record is a member of this population because its next() writes it, never
   because its name says iterator.
   THE STRUCTURAL FACT, WHICH IS WHAT PREDICTS THE NEXT MEMBER AND WHAT DECIDES WHETHER AN EXISTING ARM IS ITS
   TEMPLATE: these records differ in WHAT THE CURSOR OWNS, and there are three shapes, of which the two built
   here are the easy ones. (a) A cursor that owns nothing beside itself — for-in's `idx` — is a scalar the
   restore puts back. (b) A cursor whose advance RELEASES AN OWNED VALUE — the array/string record dropping its
   source on exhaustion — needs the blob to hold a counted reference, which is the arm below. (c) A cursor that
   is A POSITION IN A STRUCTURE A SIBLING CAN MUTATE OR FREE, or one that DECIDES WHAT THE RECORD OWNS, is
   neither: putting back a refcounted link into a list another arm has deleted from is not restoring a value,
   and putting back an index over a half-consumed owned array without putting back the ownership double-frees
   or leaks. Nothing in (a) or (b) is a template for (c), and reading a record for its cursor's SHAPE is what
   says which one a class is before any of it is written.
   WHAT THE NEXT DIFF BUILDS: that class's arm in all three functions below, plus the capture at every place
   its advance reaches the record — through ONE accessor where the sites converge, as for_in_reach and
   array_iter_reach are, so a write site cannot be forgotten. Where the class is a shape (c), the arm is a
   subproblem of its own and the ownership question is answered before the cursor is.
   HOW ITS ABSENCE WOULD SHOW: no abort at all — a loop body that ran fewer times than its source has elements,
   with nothing in any log to say so, and an arm that runs the iterator to exhaustion ending its sibling's loop
   as well. What separates that from a loop that simply was not reached is the swap census: `cowStateAsks` and
   `cowStateMade` carry the `iter` unit per kind, so a class with no capture contributes to NEITHER, while one
   whose capture is reached and refuses contributes to the asks alone. */
JS_EXTERN void *JS_IterStateSave(JSContext *ctx, JSValueConst obj);
JS_EXTERN void  JS_IterStateRestore(JSContext *ctx, JSValueConst obj, void *blob);
JS_EXTERN void  JS_IterStateFree(JSRuntime *rt, void *blob);

/* APIClient forced-execution CONCOLIC-VALUE hooks — how a concolic (symbolic + carried example) value
   PROPAGATES through the two interpreter operators that must carry it. One concern, one owner (the concolic
   component), one registration (JS_SetConcolicHooks); not optional, a NULL argument crashes. Each returns 1 when
   it handled a concolic operand (result already placed in sp[-2]) and 0 to let the normal operator run.
     - add: propagation through a CONCATENATION. A concolic operand yields the concolic result (derived shape +
            example) in sp[-2] — this is how `'/api/' + id` carries the URL shape. WHICH concatenation is the
            caller's to state (JSConcolicAddOp below); the hook never guesses it.
     - cmp: propagation through == / === . A concolic operand yields a concolic BOOL carrying the {src,op,tok}
            constraint, so `if (x === 'admin')` FORKS instead of collapsing to a concrete false. is_neq flips
            the recorded op. WHICH EQUALITY is the caller's to state (JSConcolicEqOp below) for the SAME reason
            `add` states its concatenation, and the reason is the same shape: 7.2.13 IsLooselyEqual and 7.2.14
            IsStrictlyEqual DISAGREE — `"1" == 1` is true and `"1" === 1` is false — so a hook that must run
            the comparison on the operands' concrete examples cannot do it without being told, and a hook that
            merely RECORDS the predicate was recording two different predicates under one identity.
     - is:  the domain-carrying PREDICATE, and the reason the other two are ever reached. A concolic value is a
            real JSObject of a host-registered class, so an operator that asks the raw tag sees an ordinary
            object and coerces it: 7.1.1 then reads @@toPrimitive/valueOf off the concolic, whose exotic [[Get]]
            answers with another concolic, and the walk CALLS it. `+` and == must ask this before deciding a
            coercion applies — the operand is not an ordinary object and its coercion is the solver's, not the
            page's. Never a binary know-nothing test: it names the solver's value class, nothing more.
     - absent: a read that fell off the END OF THE PROTOTYPE CHAIN — §10.1.8.1 OrdinaryGet ( obj, propertyKey,
            receiver ) step 2.b, "If parent is null, return undefined" — where that `undefined` would be a
            FABRICATION. Two different things wear that shape and must not be conflated. A missing WEB API is
            honestly absent — the page's own ReferenceError is the forcing function naming the component to
            build — while server-injected APP STATE is unknown INPUT: the server writes it for a logged-in
            visitor and did not for this one, so the read is SYMBOLIC and the auth gate FORKS to the logged-in
            arm. Collapsing it to `undefined` throws on the first field access and buries every endpoint behind
            it, which is the surface this engine exists to reach.
            IT IS ASKED FOR TWO BASES AND `obj` IS WHICH. The GLOBAL OBJECT, because a bare unresolved name and
            `window.X` are one read spelled two ways; and a RECORD THE DOCUMENT PUBLISHED into the global
            namespace (see .publish), because a server does not only write `window.__FLAGS` — it writes
            `window.gon={}` and then two of the twenty-three fields its bundle reads, and every one of the
            other twenty-one is the same unknown wearing a present parent. The engine has already established
            that `obj` is one of the two before it asks; a host that cannot NAME the base it was handed is a
            host whose registry disagrees with the engine's mark, which is a defect rather than a miss.
            Returns the concolic value, or JS_UNINITIALIZED to leave the read exactly as it was.
     - present: THE OTHER HALF OF THE SAME QUESTION, and the half without which .absent's own rule is false of
            the case it names. .absent is asked at the END of the walk, so it can only ever see a member the
            record does NOT hold — and a server that sends `window.__FLAGS={admin:false}` holds it. That read
            hits §10.1.8.1 OrdinaryGet ( obj, propertyKey, receiver ) step 3, "If IsDataDescriptor(propertyDesc)
            is true, return propertyDesc.[[Value]]", and answering `false` concretely decides `if (__FLAGS.admin)`
            for the whole program: the admin arm is never forked and every endpoint behind it is lost, which is
            EXACTLY the loss .absent exists to prevent, reached through a present slot instead of a missing one.
            The extent of that record was the server's choice against THIS visitor's credentials, so what it
            holds is a LOADED CONFIG and not a program constant: opaque for control flow, carrying the bytes the
            server sent as its EXAMPLE. Which is why it is a hook and not a value the engine invents — the
            engine has the slot, the host has the path the member is read by and the value class that carries
            both facts at once.
            ASKED FOR ONE BASE ONLY: a record the document PUBLISHED (.doc_namespace). NOT the global object,
            whose HIT is normally a builtin — `parseInt`, `Object`, `document` — so .absent's own miss predicate
            (`base == global || published`) is a WRONG predicate for a hit and is deliberately not reused: under
            it every intrinsic the page touched would become symbolic. `holder` is the record the slot was found
            ON, which is what the path names, so a record read through an inheriting receiver still reports
            under the name it was published at. `value` is BORROWED. Returns the concolic value — and the caller
            then drops its own reference to the held value — or JS_UNINITIALIZED to leave the read exactly as it
            was, which the host answers for a member the SERVER'S CHANNEL does not carry (an object-valued one:
            a child record is published in its own right and its own members are asked, and minting a fresh
            value per read would answer `a.b === a.b` false and hand the path registry an address it never
            filed).
     - publish: the document's INLINE half handing a RECORD to its EXTERNAL half, which is the one channel a
            server has for injecting per-visitor state into a bundle it ships unchanged to everybody. Called
            once per record, with the PARENT it is being published under (the global object, or a record
            already published) and the NAME, so the host can compose the path the record's members are read
            by: `gon.current_user_id`, never a bare `current_user_id` that two namespaces would collide on.
            Called for the value's own object-valued members too, deepest-last, so a server's JSON dump is
            published whole rather than one level of it.
     - rel: propagation through < <= > >= , the same shape as cmp and for the same reason. These coerce with
            ToPrimitive, which a concolic cannot satisfy — it is an object whose @@toPrimitive answers with
            another concolic — so the operator threw TypeError and took the program with it: a session check
            like `cookie.indexOf("role=admin") >= 0` explored NEITHER arm. §solver requires opacity to survive
            coercion precisely so control flow keeps forking, and equality already had a hook while ordering
            did not. `op` is the OP_lt/lte/gt/gte opcode. */
/* The arithmetic operator a concolic result is derived for — quickjs speaks these to the host so the solver
   never has to know opcode numbers.
   THE UNARY ONES COME FIRST AND THE HOST READS THE ARITY FROM THAT ORDER (op <= JS_CARITH_DEC), so a new
   operator is APPENDED, never inserted.
   The set is 13.15.3 ApplyStringOrNumericBinaryOperator's `opText` minus `+`, plus the three unary operators
   13.5.4/13.5.5/13.5.6 and the two update operators. `+` is deliberately absent: 13.15.3 step 1 ToPrimitives
   BOTH operands and string-concatenates when either primitive is a String, so it is a different algorithm and
   has its own hook (.add) — routing it here would turn `"/api/" + cfg.region` into arithmetic.
   The BITWISE and SHIFT six are 13.12 Binary Bitwise Operators and 13.9 Bitwise Shift Operators, whose numeric
   results are 6.1.6.1.16 NumberBitwiseOp ( op, x, y ), 6.1.6.1.9 Number::leftShift ( x, y ),
   6.1.6.1.10 Number::signedRightShift ( x, y ) and 6.1.6.1.11 Number::unsignedRightShift ( x, y ). Each of
   those begins with ToInt32/ToUint32 rather than plain arithmetic, which is why the host runs the ENGINE'S own
   7.1.8 ToInt32 ( arg ) / 7.1.9 ToUint32 ( arg ) on the operands' examples instead of casting a double. */
enum { JS_CARITH_NEG = 0, JS_CARITH_PLUS, JS_CARITH_NOT, JS_CARITH_INC, JS_CARITH_DEC,
       JS_CARITH_SUB, JS_CARITH_MUL, JS_CARITH_DIV, JS_CARITH_MOD, JS_CARITH_POW,
       JS_CARITH_AND, JS_CARITH_OR, JS_CARITH_XOR, JS_CARITH_SHL, JS_CARITH_SAR, JS_CARITH_SHR };

/* WHICH ALGORITHM IS CONCATENATING. Two spec operations share the .add derivation and they do NOT share its
   TYPE TEST. 13.15.3 ApplyStringOrNumericBinaryOperator step 1.c tests its own operands — "If leftPrimitive is
   a String or rightPrimitive is a String" — and takes the NUMERIC path (step 3's 7.1.3 ToNumeric, then
   6.1.6.1.7 Number::add) when neither is. 22.1.3.5 String.prototype.concat has ALREADY performed 7.1.19
   ToString on every piece at its steps 3 and 5.a, so its answer is a String always and there is no arm to pick.
   The two shared one entry for as long as the hook was string-only, and the moment its `+` arm learned 13.15.3
   that sharing became a WRONG ANSWER for concat: `x.concat(y)` over two numeric examples would have produced
   their SUM. So the CALLER states which operation it is performing — it is the only party that knows — and the
   hook never infers it from the operands. */
typedef enum JSConcolicAddOp {
    JS_CONCOLIC_ADD_PLUS,     /* 13.15.3's `+` (js_add_slow): step 1.c decides the arm */
    JS_CONCOLIC_ADD_CONCAT,   /* 22.1.3.5's string-concatenation: the string arm, unconditionally */
} JSConcolicAddOp;

/* WHICH EQUALITY THE PROGRAM WROTE — the twin of JSConcolicAddOp, and it exists for the twin reason. One hook
   serves both `==` (js_eq_slow) and `===` (js_strict_eq_slow) and was told only whether the result is negated,
   so it could neither run the comparison on concrete examples nor keep the two predicates apart in its own
   constraint key. The two algorithms genuinely differ at step 1: 7.2.14 IsStrictlyEqual returns false the
   moment SameType(x, y) is false, and 7.2.13 IsLooselyEqual coerces instead. */
typedef enum JSConcolicEqOp {
    JS_CONCOLIC_EQ_LOOSE,     /* 7.2.13 IsLooselyEqual ( x, y ): the `==` and `!=` operators */
    JS_CONCOLIC_EQ_STRICT,    /* 7.2.14 IsStrictlyEqual ( x, y ): the `===` and `!==` operators */
} JSConcolicEqOp;

/* WHICH OPERATOR ASKED A GLOBAL NAME AND ANSWERED WITHOUT PERFORMING A [[Get]] — the third of these enums and
   it exists for the first one's reason exactly: the CALLER states which operation it is performing, because it
   is the only party that knows, and the hook never infers it from the operands. Each member settles a name it
   cannot resolve WITHOUT READING IT, so every one of them is a RECORDING and the hook returns void for all of
   them; what a host does with them differs, because a census that cannot say which operation asked has a
   number about which SPELLING the bundle happened to use.
   THIS SAID `TWO OPERATORS ... AT THE OPCODE` AND IS REWRITTEN RATHER THAN DELETED, because the clause that
   went wrong is the one a reader re-derives from the two members that were here. `Reflect.has(window, "X")` is
   the same question answered by a CALL — a step machine's request, not an operator's placement — so the enum's
   subject is an OPERATION and never an opcode, and a member reached from a builtin is not an exception to a
   rule about operators but the rule stated at the width it always had.
   IT IS A FACT AND NOT A QUESTION, which is why it is an enum rather than an `is_in` flag on the request. A bit
   meaning "was it the one I happen to care about" is a question frozen into storage, and the day a third
   operation arrives it is a second bit beside the first that can disagree with it — which is exactly what the
   third member below would have been. */
typedef enum JSConcolicAbsentOp {
    /* ECMAScript §13.5.3 "The typeof Operator"'s §13.5.3.1 "Runtime Semantics: Evaluation" step 2.a, "If
       IsUnresolvableReference(value) is true, return "undefined"." — which settles the operator BEFORE step
       2.b's GetValue. The key is an IDENTIFIER's atom, so it is a string atom by §12.7 "Names and Keywords". */
    JS_CONCOLIC_ABSENT_TYPEOF,
    /* ECMAScript §13.10.1 "Runtime Semantics: Evaluation"'s `RelationalExpression : RelationalExpression in
       ShiftExpression`, whose last step is "Return ? HasProperty(rightValue, ? ToPropertyKey(leftValue))" —
       §7.3.11 "HasProperty ( obj, propertyKey )" and never a [[Get]], so there is nothing here for a host to
       answer either. THE KEY IS THE PAGE'S and not an identifier: `ToPropertyKey` runs the page's own
       @@toPrimitive on the left operand and may yield a SYMBOL, so the engine FILTERS this arm's key where the
       `typeof` arm asserts its own. The two are not one rule with two spellings — one is a fact about the
       grammar and the other is a fact about a value a page computed. */
    JS_CONCOLIC_ABSENT_IN,
    /* THE SAME §7.3.11 "HasProperty ( obj, propertyKey )" REACHED BY A CALL RATHER THAN BY AN OPERATOR —
       `Reflect.has(window, "X")`, whose §28.1.8 "Reflect.has ( target, key )" step 3 is "Return ?
       target.[[HasProperty]](propertyKey)" and contains no [[Get]] either. It is a SEPARATE member and not a
       second spelling of the one above because the two reach this hook by routes that cannot be shared: `in`
       is a bytecode operator and is recorded at its placement, and this is a step machine's request, which
       the placement is structurally unable to see (their requests carry different outer kinds). A bundle
       writes one of the two, so a census that summed them would answer a question about which the authors
       happened to prefer.
       THE KEY IS THE PAGE'S, exactly as `in`'s is: §28.1.8 step 2 is "Let propertyKey be ?
       ToPropertyKey(key).", so `Reflect.has(window, Symbol.iterator)` is a line any bundle may write and the
       engine FILTERS this arm's key rather than asserting it. Only JS_CONCOLIC_ABSENT_TYPEOF's key is the
       grammar's. */
    JS_CONCOLIC_ABSENT_HAS_CALL,
} JSConcolicAbsentOp;

typedef struct JSConcolicHooks {
    int (*add)(JSContext *ctx, JSValue *sp, JSConcolicAddOp op);
    int (*cmp)(JSContext *ctx, JSValue *sp, int is_neq, JSConcolicEqOp op);
    int (*is)(JSValueConst v);
    JSValue (*absent)(JSContext *ctx, JSValueConst obj, JSAtom name);
    /* THE SAME MISS ON THE GLOBAL, REACHED BY AN OPERATION THAT PERFORMS NO [[Get]] AT ALL. RECORDING ONLY,
       AND THE VOID RETURN IS THE CONTRACT rather than a convenience — for EVERY member of JSConcolicAbsentOp,
       and for one reason in three spellings: no member's algorithm contains a read, so a value handed back
       here would make a step run that the standard does not have. `typeof X` answers at §13.5.3.1 step 2.a,
       BEFORE step 2.b's GetValue, so a value would make it say "object" where every browser says "undefined";
       `"X" in window` ends at §13.10.1's "Return ? HasProperty(rightValue, ? ToPropertyKey(leftValue))", which
       is §7.3.11 and never §7.3.2 Get; `Reflect.has(window, "X")` ends at §28.1.8 step 3's "Return ?
       target.[[HasProperty]](propertyKey)", which is the same §7.3.11 one call further out. `.absent` may
       decide a read; this may only watch one.
       WHY IT EXISTS AT ALL, since the decision is unchanged: the suppression `.absent` performs for a name a
       standard owns is CORRECT and SILENT, and these are the spellings that never reach it. A bundle whose
       whole feature detection is written `typeof X !== "undefined"`, `"X" in window` or `Reflect.has(window,
       "X")` — between them the dominant shape in transpiled and in hand-written code — takes the false arm on
       every guard while the host's census of unanswered names reads clean, so what is lost is not the line but
       every endpoint and every sink behind the guard. The host counts it; nothing forks and nothing is minted.
       THE KEY RULE IS NOT ONE RULE, WHICH IS THE ONE THING A CALLER MAY NOT GENERALISE ACROSS THE MEMBERS, AND
       IT IS KEYED ON THE ONE MEMBER THAT HAS THE PROPERTY RATHER THAN ON THE ONES THAT DO NOT. For `typeof`,
       `name` is the identifier's atom and §12.7 Names and Keywords makes it a string atom, so
       JS_AtomIsPublishedName holds BY CONSTRUCTION and the engine asserts it. EVERY OTHER MEMBER TAKES A KEY
       THE PAGE COMPUTED — `in` is `ToPropertyKey(leftValue)` and `Reflect.has` is §28.1.8 step 2's
       `ToPropertyKey(key)` — so the same predicate is a statement about a value a PAGE computed and
       the engine FILTERS on it before speaking to this hook. Asserting there would hand any page an abort
       switch for one line of `Symbol.iterator in window`, which CLAUDE.md §WHOSE-BYTES-STATE-THE-VALUE forbids
       outright. THE POLARITY IS THE LOAD-BEARING HALF: a member added to this enum joins the FILTER, which is
       the arm that records less and asserts nothing, rather than the assert, which is the arm that would be a
       page-held abort switch — so the safe arm is the default and the unsafe one is the one a member has to be
       named into. A host may therefore rely on the atom being nameable, and may not rely on the reason being
       the same one at every arm.
       Installed with `.absent` or not at all: a host that takes one spelling of one miss and not the others has
       a census whose zero is a fact about which spelling the page happened to use. */
    void (*absent_unresolved)(JSContext *ctx, JSAtom name, JSConcolicAbsentOp op);
    /* THE HIT ON A PUBLISHED RECORD — see the paragraph above. `holder` is the record the own data slot was
       found on and `value` is what it holds, BORROWED. */
    JSValue (*present)(JSContext *ctx, JSValueConst holder, JSAtom name, JSValueConst value);
    /* THE DOCUMENT PUBLISHING A RECORD INTO THE GLOBAL NAMESPACE — see the paragraph above. `parent` is the
       global object or a record already published under it; both are BORROWED. Installing this is what makes
       the engine mark records at all, so a host that wants neither half installs neither. */
    void (*publish)(JSContext *ctx, JSValueConst parent, JSAtom name, JSValueConst value);
    int (*rel)(JSContext *ctx, JSValue *sp, int op);
    /* `typeof v`. Returns the type STRING to use, or JS_UNINITIALIZED to run the real js_operator_typeof. An
       unknown value's type is unknown, and the engine must not answer it from the host object's REPRESENTATION
       (a callable placeholder would say "function" and decide `typeof x === "function"` for the program). */
    JSValue (*type_of)(JSContext *ctx, JSValueConst v);
    /* 7.1.4 ToNumber OVER UNKNOWN INPUT, answered where the SPEC says the operator computes — never at the
       conversion boundary, which owes C a real number. `-x`, `~x`, `x*2`, `x**2`, `++x` yield a DERIVED
       concolic that keeps the source's identity, so a later branch still forks and a later sink still solves
       for the original source. The EXAMPLE propagates by RUNNING THE REAL OP on the operands' examples, which
       is what makes this the concolic triple rather than a taint label. Operands are at sp[-nops..sp[-1]] and
       the result is written to sp[-nops], the shape .add and .cmp already use. Returns 1 when it answered. */
    int (*arith)(JSContext *ctx, JSValue *sp, int op, int nops);
    /* §7.1.19 ToString ( arg ) over unknown input, same rule: `String(x)` is unknown, with the source kept.
       Returns JS_UNINITIALIZED when the operand is not concolic.
       IT IS THE ANSWER FOR THE ONE ALGORITHM WHOSE RESULT *IS* THE COERCION — 22.1.1.1 String ( value ) — and
       not for ToString wherever else it appears: a builtin that merely CONSUMES a string derives its own
       result from the operand, named by its own operation, which is what step_tostring_run's JS_STEP_UNKNOWN
       does at the one sub-sequence all of them share. Two hooks would be two names for one derivation; these
       are two derivations, and their shapes say so (`String(x)` versus `x.<operation>()`). */
    JSValue (*to_str)(JSContext *ctx, JSValueConst v);
    /* §7.1.2 ToBoolean ( arg ) OVER UNKNOWN INPUT, answered by the OPERATOR for the same reason .arith and
       .to_str are: the conversion boundary underneath owes C a real boolean and has only one to give. §7.1.2
       has no ToPrimitive step, so the C answer for an unknown is whatever its REPRESENTATION is — and a
       concolic is an ordinary JS_TAG_OBJECT, which JS_ToBoolFree answers `true` for. That is not a coarse
       answer, it is a DECIDED one: it deletes an arm before any OP_if can fork it, and the arm it deletes is
       the one behind every `if (!user.isAdmin) return;` a bundle writes.
       `negate` IS A PARAMETER AND NOT A SECOND HOOK, because §13.5.7 Logical NOT Operator ( ! ) is §7.1.2
       with the answer complemented and nothing else — its §13.5.7.1 Runtime Semantics: Evaluation is four
       steps of which one is the coercion and two are the complement, so there is one derivation with a
       polarity and never two derivations. So `!x` and
       `Boolean(x)` reach one implementation and `!!x` composes back to `Boolean(x)`'s own predicate rather
       than to a third one. Passing 1 asks for the complement.
       Returns JS_UNINITIALIZED when the operand is not concolic. */
    JSValue (*to_bool)(JSContext *ctx, JSValueConst v, int negate);
    /* `obj[x]` with an UNKNOWN key. Not a coercion of the operand — a lookup that names no particular slot, so
       the read yields a concolic derived from the base and the key's source. JS_UNINITIALIZED = not concolic. */
    JSValue (*key_read)(JSContext *ctx, JSValueConst obj, JSValueConst key);
    /* THE NAME an unknown key denotes, as a REAL string — the concolic's own shape. A property key must be an
       atom, which a concolic cannot be, but the shape IS a string and it is stable per source: two writes
       through the same unknown source land in the SAME slot, two different sources in different ones, and a
       read through that source finds what was written. That is a sound model of "unknown but consistent", and
       it is what lets `obj[x] = v`, `delete obj[x]`, `x in obj` and every key-taking builtin work at all.
       JS_UNINITIALIZED = not concolic. */
    JSValue (*key_name)(JSContext *ctx, JSValueConst key);
    /* THE INVERSE, ASKED WHERE A PROPERTY NAME IS HANDED BACK TO THE PROGRAM AS A VALUE. `key_name` above is
       a ONE-WAY DOOR: it spends a concolic's PROVENANCE and its DOMAIN to buy an atom, and that is the honest
       price of the sentence it cites — a key is bytes, and an unknown cannot be bytes. The price is CORRECT at
       every site that wanted bytes: a lookup, an atom a builtin is about to consume, a `@WHY` message's
       display form. It is a LOSS at exactly one shape of site, the one that wanted a VALUE.
       An ENUMERATION is that site. §14.7.5.10.2.1 %ForInIteratorPrototype%.next ( ) binds the name to a
       variable the page then computes with, so a slot named through `key_name` — `o[x] = 1`, or a record's own
       member materialised by `own_key_mint` below — reaches the page as an ordinary string: `k === "admin"` is
       then decided by comparing two concrete strings and PRUNES an arm nothing contradicts, and
       `fetch("/api/" + k)` composes a concrete path segment out of an unknown, which forks no branch and
       reports no domain. Neither of those is a smaller answer; they are a wrong one.
       So the name->value direction asks the host to answer the unknown AGAIN, and the two hooks compose:
       `o[k]` over the restored value asks `key_name` and lands back in the same slot.
       ASKED ONLY WHERE THE NAME BECOMES A VALUE THE PROGRAM OBSERVES — never at JS_AtomToString, whose callers
       want bytes by construction, and never at an atom a lookup is about to consume.
       JS_UNINITIALIZED = this string is not a name this host minted for an unknown key, which is the ordinary
       case and covers every atom the parser, the bytecode and the engine's own vocabulary produce. */
    JSValue (*key_value)(JSContext *ctx, JSValueConst name);
    /* A BUILTIN WHOSE OPERAND IS UNKNOWN produces an unknown DERIVED FROM IT, labelled with the operation the
       spec was performing ("RegExp.exec"). The same rule as .arith and .to_str, for the operations that are
       neither arithmetic nor a coercion: the operator answers, because the conversion boundary underneath owes
       C a real string and can only crash. Without it `/token=(\w+)/.exec(document.cookie)` — which is what
       reading a cookie LOOKS like in a real bundle — aborted the whole document.
       The result keeps the source's identity, so a later branch still forks and a later sink still solves for
       the original source. JS_UNINITIALIZED when the operand is not concolic. */
    JSValue (*builtin)(JSContext *ctx, JSValueConst v, const char *op, JSValue example);
    /* THE OPERAND'S CONCRETE EXAMPLE, so the operator can RUN THE REAL OPERATION on it and hand the answer
       back as the derived value's example. §solver is explicit that this is the only sound way an example
       propagates — the engine runs the real op on the concrete, never a rule that predicts what it would have
       produced. Returns JS_UNDEFINED when the operand carries no example yet. */
    JSValue (*example)(JSContext *ctx, JSValueConst v);
    /* THE FIRST CHARACTER THE BROWSER'S DELIVERY OF THIS SOURCE GUARANTEES, or 0 when it guarantees none.
       This is DOMAIN, not example: `location.hash` is either the empty string or `#` followed by the fragment
       — the component that owns the source declares that prefix — so a builtin can rule an outcome INFEASIBLE
       without knowing the value. §solver states the case it exists for: the leading `#` makes
       `JSON.parse(location.hash)`'s success arm infeasible so it throws exactly as V8 does, and forking a
       success arm there would fabricate a completion no value of the source can produce. It is a
       feasible-refinement, which is sound-only: it prunes an arm the DOMAIN already contradicts, never one the
       domain permits. A DERIVED value (`location.hash.slice(1)`) has its own identity and no declared
       delivery, so it answers 0 and both arms stay — which is the correct answer for it. */
    int (*lead)(JSValueConst v);
    /* THE PREDICATE AN ENUMERATION OF AN UNKNOWN KEY SET FORKS ON — the question §10.1.11 [[OwnPropertyKeys]]
       ( ) cannot ask and cannot answer, handed to the seam that has a resume point.
       WHY THE ENGINE HAS TO ASK. An unknown that carries no EXAMPLE has an unknown key set: the empty List is
       a real world (the payload was `{}`, and it is the world a program branching on
       `Object.keys(x).length === 0` needs to have run) and it is a WRONG one as a FACT, because it states that
       the record holds nothing — which no run observed. So it is an ARM, and the fork needs a resume point.
       The record's own internal method has no resume point: §10.1.11 "takes no arguments and returns a normal
       completion containing a List of property keys", and it is reached from inside a C activation with no flow
       base under it, so it cannot snapshot a sibling. That, and only that, is why the ask is at the CONSUMER,
       where the enumeration is a step machine's request that can park.
       IT IS NOT THAT THE TRUE ARM CANNOT BE EXPRESSED — an earlier statement here said so and it was wrong.
       §6.1.7 The Object Type says "A property key is either a String or a Symbol." and §6.1.7.3 Invariants of
       the Essential Internal Methods requires that "Each element of the returned List must be a property key",
       and an unknown key in this engine ALREADY denotes a real String, its own shape (`key_name`), so the
       member the flow decides on is materialised under that name by `own_key_mint` below and the ordinary key
       walk lists it like any other. The List needs nothing it cannot hold; the FORK needed somewhere to stand.
       THE HOST OWNS THE KEY AND THE ENGINE NEVER SPELLS ONE. The returned value's identity is composed over the
       record's, so two consumers asking about ONE record ask ONE question and the host reads that same answer
       back when its internal method runs. An engine that built the key itself would file the question under a
       second name and no consumer's fork could answer it.
       Returns JS_UNINITIALIZED when this enumeration carries no question — an ordinary object, or a record
       whose EXAMPLE holds the answer, whose key set is an OBSERVATION rather than an unknown. That is the same
       positive statement `type_of` and `to_bool` make, and it is why this is not a boolean.
       A HOST THAT INSTALLS THE VALUE CLASS AND NOT THIS MEMBER is not exempted from the question, it merely
       leaves it unasked — and its own record class is then reached without a decided arm, which is the state
       that class must refuse rather than answer. Install the two together. */
    JSValue (*own_keys_pred)(JSContext *ctx, JSValueConst record);
    /* MATERIALISE THE RECORD'S n-TH UNKNOWN OWN MEMBER — the arm the predicate above answers TRUE, PERFORMED.
       WHY IT IS A MATERIALISATION AND NOT A KEY HANDED BACK. A member whose name is unknown looks unable to
       reach §10.1.11 [[OwnPropertyKeys]] ( )'s answer at all, because §6.1.7.3 Invariants of the Essential
       Internal Methods requires that "Each element of the returned List must be a property key" and §6.1.7 The
       Object Type says "A property key is either a String or a Symbol." — so the arm reads as one the internal
       method cannot express. It can: an unknown key ALREADY denotes a real String in this engine, its own
       SHAPE, which is what .key_name states and what makes `o[x] = 1` then `o[x]` find one slot. So the host
       gives the member that name and an ordinary slot on the record, and the ordinary key walk underneath
       [[OwnPropertyKeys]] lists it like any other — one List, one vocabulary, and every consumer (the
       enumerable-key cursor's [[GetOwnProperty]] re-check, §20.1.2.11.1 GetOwnPropertyKeys ( value, type )'s
       String/Symbol filter, §14.7.5.10.2.1 %ForInIteratorPrototype%.next ( )) works on it unchanged.
       A SLOT IS ALSO WHAT MAKES IT PER-FLOW: the write is an ordinary property creation, so the COW delta
       captures it and the sibling that took the empty arm never sees the member.
       THE NAME IS DERIVED, NEVER INVENTED (§@H): it is composed over the record's own identity and n through
       the host's one derivation speller, so it is the same string every time this flow re-asks and a different
       one for every other record. STRING rather than Symbol is not a coin toss either — the channel these
       records stand for is a server writing a record of fields into the page, which JS_AtomIsPublishedName
       states carries no Symbol.
       `n` counts from 0 and rises only while the enumeration's chain keeps answering "another". Returns 1
       when the member is on the record, 0 when this host models no such record, -1 having thrown.
       INSTALLED WITH own_keys_pred OR NEITHER: a host that asks the question and cannot perform its true arm
       has an enumeration that decided a member exists and then enumerated nothing, which is the empty-List
       fabrication the predicate exists to prevent, arriving one step later. */
    int (*own_key_mint)(JSContext *ctx, JSValueConst record, int n);
    /* THE RUNNING FLOW'S OWN COUNT OF PRIOR CREATION-NAME MINTS, asked ONCE per page-created value the engine
       can name and never again — the half of a creation name this engine cannot compose for itself.
       WHY IT IS A HOOK AT ALL. A creation name is (SITE, ORDINAL): the site is a program fact the engine owns
       and composes per class (a closure's body locator, a RegExp's own source text and flags — see
       JS_CreationName), and the ordinal is a fact about THE EXECUTED PREFIX OF ONE FLOW, which
       only the host has — it must be inherited by a fork exactly as every other prefix quantity is, parked
       with a suspended flow, and restored at zero for a flow the cold tier resumes, because such a flow
       REPLAYS the document from the baseline and re-mints the same sequence. Nothing in this engine knows
       what a flow is.
       WHY IT IS HERE AND NOT IN JSTimeTravelHooks. That table is "before this write mutates shared heap
       state, capture it" — every member of it is a CAPTURE and its installer is the COW layer. This is not a
       capture and nothing about it rewinds: it is the concolic layer's identity channel, beside `key_name`
       and `example`, and its installer is the component that owns the constraint chain the counter rides.
       WHAT IT MUST RETURN: a NONZERO value, distinct from every other value this hook has returned to the
       same flow since that flow's prefix began. Zero is this engine's "no name was minted", so a hook that
       returns it silently unnames the value; a REPEAT is worse and is the failure the whole mechanism exists
       to prevent, because two objects under one name make one flow's constraint refine the other's branch and
       the ARM IS LOST rather than duplicated (concolic.c states that trade at the field itself: "Absence
       costs forks; a wrong identity costs the arm").
       NULL IS A HOST DECLINING THIS EDGE, which is JSFlowControlHooks.budget's own arrangement: the engine
       then mints nothing, JS_CreationName answers absent for every value, and the behaviour is byte-identical
       to a build that never had this member — one predictable branch on a thread-local pointer at each of the
       engine's creation paths, and no per-object cost anywhere, because the ordinal a value carries is a field
       it has either way. */
    uint32_t (*mint_ordinal)(JSContext *ctx);
} JSConcolicHooks;
JS_EXTERN void JS_SetConcolicHooks(const JSConcolicHooks *hooks);

/* CAN THE INJECTED-STATE CHANNEL NAME THIS KEY? The channel is a server writing a RECORD OF FIELDS into the
   page (`window.gon={current_user_id:7}`), so its keys are strings and array indices; a SYMBOL is not on it.
   The ENGINE gates on this before it asks `.absent`/`.present` at all, and it is EXPORTED because the host is
   the channel's other end and composes a PATH out of the key — so the host ASSERTS the rule rather than
   trusting it. Both halves are the mechanism: a gate with nothing checking it is a gate a later route goes
   around in silence, and this one already was gone around once. §7.1.1 ToPrimitive ( input [ , preferredType ] )
   step 1.a's `? GetMethod(input, %Symbol.toPrimitive%)` misses on EVERY object, so while the miss arm asked no
   key rule, every object reachable through a global coerced to a callable unknown and step 1.b.vi threw
   `TypeError` — a whole document's worth of statements lost to a read that looked like an operator bug. */
JS_EXTERN bool JS_AtomIsPublishedName(JSRuntime *rt, JSAtom atom);

/* …AND IS IT AN INDEX RATHER THAN A NAME? The two keys the answer above admits are not spelled the same way,
   and a provenance is the EXPRESSION THE RUN BUILT (§@H) — `__STATE__.users[0].role` is one a reader can
   paste and `__STATE__.users.0.role` is a syntax error wearing a path. That distinction used to cost nothing
   because an index key reached the channel only when a server wrote one (`{"0":…}`); it is now the ordinary
   case, because the walk descends through the LISTS a server's tree is made of and every element is reached
   by its index.
   ASKED OF THE ATOM AND NOT OF ITS SPELLING, which is the whole reason it is exported rather than re-derived:
   a digit test on the C string cannot tell `x[1]` from `x["01"]` — ECMAScript §6.1.7 The Object Type makes an
   array index a canonical numeric string, so "01" is a NAME — and a host that guessed would compose one path
   for two different keys, which is the collision the symbol rule already refuses one key kind for. */
JS_EXTERN bool JS_AtomIsIndexName(JSAtom atom);

/* APIClient @S — THE JS-CONTEXT CODE-EXECUTION SINK, ANNOUNCED BY THE ENGINE THAT OWNS IT.
 *
 * The other two sink classes belong to the HOST: a markup sink is `element.innerHTML` and a URL sink is a
 * navigation, and both are members of browser components, so the detector is called from the component that
 * performs the operation. `eval` is not: 19.2.1 "eval ( source )" and 20.2.1.1.1 CreateDynamicFunction are
 * ECMAScript intrinsics and live in this file, so this file is the only place that can say a value was offered
 * to a program evaluation. Without this seam the whole JS-context derivation was reachable only from a
 * fixture, and a real page's `eval(prefix + attackerInput)` was detected by nothing at all.
 *
 * IT IS A HOOK AND NOT A SELECTOR, and the test is structural rather than a claim: it has NO return value, so
 * there is nothing for the algorithm to route on, and 19.2.1.1 proceeds byte-identically whether a host
 * registered one or not. A host that registers none (the qjs shell, a conformance host) is told nothing and
 * behaves exactly as upstream does.
 *
 * `src` is the value 19.2.1.1 step 1 was handed, announced BEFORE step 2 ("If source is not a String, return
 * source") tests it, and BOTH arms are announced because those two arms are the sink's two halves:
 *   - the NON-STRING arm is where the sink is DETECTED. Unknown external input is an object of the solver's
 *     value class, so `eval(location.hash)` takes step 2 and returns unevaluated — this engine can compile no
 *     program because the unknown names none, exactly as HTML 8.7 "Timers"'s string-handler arm cannot.
 *   - the STRING arm is where an @S CANDIDATE is read. A candidate re-run substitutes a concrete payload at
 *     the source, so the page's own concatenations and filters build a real string, and the detector must see
 *     the very bytes that are about to be compiled — to scan them per ECMAScript 12 for the lexical state the
 *     attacker's bytes sit in, and to observe a breakout arriving at its own sink.
 * The bytes are then COMPILED AND RUN by this engine, and that is what proves a PoC: the sink's own evaluation
 * IS the fire oracle, so no host may model a second one beside it.
 *
 * BORROWED: the hook must not free `src` and must not retain it past the call. */
typedef void JSEvalSinkFunc(JSContext *ctx, JSValueConst src);
/* NULL is a legitimate argument and means "no host is listening" — which is what a conformance host is, and
   what a solver host becomes once it has torn down the store the detector writes into. */
JS_EXTERN void JS_SetEvalSinkHook(JSEvalSinkFunc *cb);

/* Forced-execution FLOW API — the host CALLS these to create / drive / snapshot / free flows. (The preempt
   CALLBACK that parks a running flow lives in JSFlowControlHooks above; these are the host-driven counterpart.)
   A flow runs as a preemptible heap-resident async-function frame, so it interleaves under the WFQ. */
JS_EXTERN JSValue *JS_FlowNew(JSContext *ctx, const char *src, size_t len, const char *filename, int eval_flags);
/* …AND THE SAME THING WITH THE COMPILE'S OWN SUSPEND POINT ARMED. The PARSE is the one span in this engine
   that is O(a length the PAGE chose) and could not rest at any input size: the three interpreter raise kinds
   above are raised from DISPATCH, which a parse never reaches, so a compile polled nothing for its whole
   length. It polls now, from its own production dispatch, through the SAME budget hook and the SAME preempt
   policy as the interpreter — one park decision, still in one place.
     Answers 1 (the flow is built and *pframe holds it), 0 (the compile HANDED THE THREAD BACK — *pcompile
   holds it and the host calls again, with the same src/len/filename/eval_flags, when it next wants to spend
   time here) or -1 (the compile failed; the exception is pending). A `pcompile` of NULL is a host that
   DECLINES this edge, exactly as a NULL `budget` above is, and it gets the parse it always got: JS_FlowNew is
   that spelling and is a wrapper over this entry rather than a second implementation.
     THE RESUME IS BYTE-IDENTICAL AND NOT A RE-PARSE. What is suspended is the descent's explicit frame stack,
   whose chunks are allocated once and never moved, so the frames, the JSFunctionDef chain and every
   break/continue linkage stay at the addresses they already hold; nothing is serialised and nothing is
   rebuilt. That is also why a parked compile is an IN-RAM park only: it cannot be serialised, and it does not
   need to be, because a compile is RE-DERIVABLE from the source the host is holding — a flow paged out
   mid-parse drops the parse through JS_FlowCompileDrop and re-compiles when its recipe replays the document.
     THE SOURCE AND THE FILENAME ARE BORROWED FOR THE WHOLE COMPILE and not merely for one call — the parse
   reads them across every stint — so a host that offers `pcompile` is promising those bytes outlive it. */
JS_EXTERN int JS_FlowNewStep(JSContext *ctx, const char *src, size_t len, const char *filename, int eval_flags,
                             JSValue **pframe, void **pcompile);
/* Release a compile the host is never going to finish — the flow it belonged to is being freed with its
   program still half-parsed. It is a no-op on a NULL slot, so a teardown may call it unconditionally. */
JS_EXTERN void JS_FlowCompileDrop(JSContext *ctx, void **pcompile);
/* A FLOW WHOSE WHOLE PROGRAM IS ONE CALL — `func(argv…)` with `this_val` as the receiver. The third way to make
   a flow, beside a SOURCE and a CLONE, and the one ORPHAN-INVOKE needs: a function nothing has called is
   reachable only by calling it, and a host that JS_Calls it from C runs it in an activation with no flow base
   under it, which is the drive-to-completion this engine aborts on. The base built here is the one a promise
   reaction runs on, so `func` may be of any kind and a loop or an await inside it PARKS like any other flow's.
   A throw is the flow's completion and arrives through JS_FlowResume's `pres` exactly as a program's does.
   WHETHER THE CALL'S RESULT ARRIVES THERE TOO IS THE CALLER'S TO STATE, and it is a required argument rather
   than a default because the two answers are not interchangeable and neither is safe to assume. A host that
   drives a call for its SIDE EFFECTS (an orphan invoked to see what it reaches, a report of an exception) has
   no use for the value and must not be handed one: `pres` is read by arms that dispatch on its TYPE, so a
   driven function returning a string would arrive at a reader asking whether a javascript: URL evaluated to
   one. A host that drives a call for its ANSWER — an algorithm queued as a task whose completion value IS the
   fact the caller needs, which is what HTML §6.10.1 "Close requests"' step 9 is — must be handed it, and a
   `false` here is then a reader whose producer produces nothing, with `JS_UNDEFINED` standing in for the
   answer and no way to tell that apart from a real one.
   take_result FALSE: the result is discarded here and `pres` is JS_UNDEFINED on a normal completion.
   take_result TRUE:  the result is handed to `pres` and the CALLER OWNS IT (it frees it, exactly as it frees a
   program flow's completion value).
   Resumed, cloned and freed by the three calls below; a CLONE keeps this answer, because a forked arm of a
   call flow completes the same call and owes its completion to the same host.
   NULL if the allocation failed (the exception is live). */
JS_EXTERN JSValue *JS_FlowNewCall(JSContext *ctx, JSValueConst func, JSValueConst this_val,
                                  int argc, JSValueConst *argv, bool take_result);
/* IS THIS HANDLE A CALL FLOW (JS_FlowNewCall's) rather than a program flow (JS_FlowNew's)? A program's
   completion advances a document's script sequence and its throw is the page's; a call's completion advances
   nothing and its throw belongs to whoever made the call. A host running both must be able to tell them apart,
   and a position in a script sequence cannot say: a program the running call queues moves the cursor back
   inside the sequence while the call frame is still live. */
JS_EXTERN int      JS_FlowIsCall(const JSValue *flow);
/* IS THIS HANDLE A PROGRAM FLOW (JS_FlowNew's)? — AND IT IS NOT `!JS_FlowIsCall`, WHICH IS THE WHOLE REASON IT
   EXISTS. There are THREE kinds of base, not two: a PROGRAM this host compiled (JS_FlowNew), a CALL this host
   built (JS_FlowNewCall), and an ACTIVATION THIS HOST NEVER CREATED — the clone JS_FlowClone takes at a
   concolic branch inside a body the scheduler drives directly, which is a module body or any async function
   entered from C. That third kind answers FALSE to both of these questions, and it must: it completes by
   SETTLING A PROMISE rather than by handing a value back (JS_FlowResume's FLOW_BASE_ASYNC_CALL arm), its row
   was left the instant HTML §8.1.4.4 "Calling scripts"' run a module script Evaluate returned, and nothing it
   does advances a script sequence.
   A HOST THAT ASKED `!JS_FlowIsCall` WAS ASKING "IS THIS A PROGRAM" AND GETTING "IS THIS NOT A CALL", and the
   two answers differ for exactly that third kind — so an arm forked inside a module body read as the program of
   a row its parent had already left, left that row a SECOND time, and where its cursor was not already at the
   end SKIPPED the next row in silence. Ask the question you mean; both are one read of the same field, so they
   cannot come apart. */
JS_EXTERN int      JS_FlowIsProgram(const JSValue *flow);
/* ONE ORPHAN — the next function object on this runtime's heap whose BODY no call frame has ever begun, handed
   over with the callee's own declared formal parameter count. See JS_OrphanTakeOne in quickjs.c for what an
   orphan is, why the heap is where they are enumerated from, why taking one is not a seen-set, and why this
   hands over ONE rather than the set: its consumer turns each into a FLOW, so a take of N mints N flows inside
   one C loop that has no back-edge and therefore no suspend point, and the scheduler cannot re-rank, return to
   its host or page its tail across the burst. One per call makes the scheduler's own loop the only loop over
   orphans, and needs no queue anywhere — between two calls the remaining orphans are the heap.
   `visit` is called from inside the walk of the object list and MUST NOT ALLOCATE A GC OBJECT — it records (a
   JS_DupValue costs no allocation) and acts once the take has returned; a dev build asserts it. `fn` is
   BORROWED. Returns 1 if one was handed over, 0 if this heap holds none. */
typedef void JSOrphanVisitFn(JSContext *ctx, JSValueConst fn, int arg_count, void *opaque);
JS_EXTERN int      JS_OrphanTakeOne(JSContext *ctx, JSOrphanVisitFn *visit, void *opaque);
/* COULD THE ORPHAN SET HAVE GROWN — the count of function objects this runtime has ever made. Creating one is
   the only event that can add to the set (running a body only marks it entered, taking one only marks it
   taken, collecting one only removes it), so a host that took the orphans at generation G may skip the heap
   walk until this differs from G. Compared for INEQUALITY only, so its wrap costs one redundant walk. */
JS_EXTERN uint32_t JS_OrphanGen(JSRuntime *rt);
/* THE CROSS-SESSION NAME OF ONE ORPHAN'S BODY — what a host writes down so a LATER session can drive the same
   function again. Neither of the two names a session already has survives it: a function object is a live heap
   reference, and a position in the heap walk above is a fact about one heap at one instant. So the locator is
   composed of what the BUNDLE determines and the session does not — the script the body was compiled from, WHERE
   IN THAT SCRIPT the body begins, and the body's own source text — and it is stable across sessions for the
   simple reason that the same bytes compile the same way.
   ALL THREE ARE LOAD-BEARING. Source text alone does not identify a body in a minified bundle: `function(e){
   return e.default}` occurs dozens of times in one webpack output, so a hash of the text alone would name a set
   and the resume would drive whichever member it met first. Position alone changes with every byte the server
   prepends, so a hash of the position alone silently stops matching the day a banner is added. Together, a
   mismatch means the file genuinely changed, which is the honest answer.
   IT NAMES THE BYTECODE, NOT THE CLOSURE, for the same reason the take does: `entered` is a bit on the
   JSFunctionBytecode, so one body is one orphan however many closures of it a factory has made, and two
   closures of one body must therefore hash the same.
   ALLOCATION-FREE — the interned filename's characters and the source buffer are read in place — so it may be
   called from inside the object-list walk above. `fn` must be an object with a bytecode body; a C function, a
   bound function and a Proxy have no body to name and are never orphans. */
JS_EXTERN uint64_t JS_OrphanHash(JSContext *ctx, JSValueConst fn);
/* WHERE THE PAGE'S OWN CODE IS STANDING, as a CALL SITE rather than as a body — the locator above folded over
   the nearest bytecode frame's body PLUS the byte offset of the opcode that frame is standing at, so two calls
   in one minified function are two names where JS_OrphanHash gives them one. Answers about the nearest frame
   with a bytecode body, walking outward past call roots, because a C function this engine runs on the page's
   behalf is doing it FOR the page code beneath it — a DOM mutator, a coercion, a codec.
   IT RETURNS WHETHER A PAGE FRAME IS STANDING AND WRITES WHICH SITE, and those are two questions on purpose.
   "No page frame at all" is the whole of HOST TIME and of the initial parse — a large population and a
   meaningful one — so a reserved hash for it would seat that population on a value a real site can also fold
   to, and nothing downstream could separate them. A caller that ignores the return cannot read `*out`.
   ALLOCATION-FREE and O(1) in the body, like the locator it is built on: the position is the frame's own byte
   offset, never a pc2line resolution, which makes it reproducible within a BUILD and not across two of them.
   See the definition's named residual for what that costs and what replaces it. */
JS_EXTERN int JS_RunningSiteHash(JSContext *ctx, uint64_t *out);
/* THE NAME OF AN INTRINSIC OF THIS REALM — the OTHER name source a value can have, for the creator kind
   JS_OrphanHash's composition cannot reach. A locator built from a script, a position and a body's text names
   nothing for `Array.prototype`: no page created it. An intrinsic is a SINGLETON of its realm, so the SLOT it
   occupies is its whole name, and a slot may name it because the class registry is fixed at each class's own
   definition rather than being an ordinal over a set the page mutates. Answered by walking the per-realm
   class_proto/class_ctor arrays — the ONE registry every realm goes through — plus the global object and the
   well-known symbols, which live below JS_ATOM_END and are therefore the same kind of compile-time registry.
   TEXT AND NOT A HASH, because its consumer spends the name twice: once as an operand's IDENTITY and once as
   its DISPLAY SHAPE, under the rule that a shape must separate every pair of operands its identity separates.
   A hash separates the first and collapses the second. The spelling is ECMAScript's own §6.1.7.4 Well-Known
   Intrinsic Objects notation — `%Array.prototype%`, `%Array%`, `%Symbol.iterator%`, `%globalThis%`.
   ALLOCATION-FREE and side-effect-free: no property is read, so no accessor and no Proxy trap runs, which is
   what lets a caller with no flow base under it ask at all. `v` is BORROWED and may be any value.
   RETURNS snprintf's length — the length that WOULD have been written, so a caller checks it against its own
   buffer rather than trusting a silent truncation — or -1 where this value is not an intrinsic of this realm,
   which is the "no name" answer every value gave before and is never an error. */
JS_EXTERN int      JS_IntrinsicName(JSContext *ctx, JSValueConst v, char *buf, size_t buf_size);
/* THE LONGEST NAME THE ABOVE CAN COMPOSE, so a caller's buffer makes truncation IMPOSSIBLE rather than
   asserted: every name is `%` + one registry name + at most `.prototype` + `%`, and the registry name is
   bounded by the atom-spelling buffer quickjs.c sizes for it. A _Static_assert there ties the two, so widening
   one without the other stops the build instead of silently shortening a name — and a shortened intrinsic name
   is two intrinsics under one key. */
#define JS_INTRINSIC_NAME_MAX 96
/* THE NAME OF A SYMBOL THE PAGE REGISTERED — the THIRD name source, for the one value JS_IntrinsicName above
   refuses by name: a `Symbol.for` symbol belongs to a registry that is not this realm's, and its name is the
   key the PAGE registered it under.
   IT IS PAGE-CREATED AND NEEDS NO ORDINAL BESIDE IT, which is what separates it from a creation SITE. A site
   is 1:N with the values made there, so a site alone names a SET and a constraint about one member would
   refine a branch over another; §20.4.2.4 "Symbol.for ( key )" step 4's loop over the GlobalSymbolRegistry
   returns the EXISTING symbol for a key already in it and only step 7 appends a new record, so a key denotes
   exactly one symbol and is its whole name. In this engine that List is the ATOM TABLE, so the 1:1 is a fact
   about this heap and not only about the standard's model.
   REPRODUCIBLE BY THE REPLAY A RESUMED FLOW PERFORMS, which is the requirement rather than
   uniqueness-in-a-heap: the key is text the page's own bytes produced, so a replay registers under the same
   key and reaches the same symbol. Not an address, which is reused, and not a position in the atom table.
   ALLOCATION-FREE and side-effect-free: the atom struct is the key's string, so the answer is a refcount bump,
   no property is read and no trap runs — which is what lets a caller with no flow base under it ask at all.
   Returns the key as a String, OWNED by the caller, or JS_UNDEFINED where `v` is not a registered symbol —
   the same "no name" answer JS_IntrinsicName's -1 is, and never an error. It is the value §20.4.2.8
   "Symbol.keyFor ( symbol )" returns, reached without running a builtin. `v` is BORROWED and may be any
   value. */
JS_EXTERN JSValue  JS_SymbolRegistryKey(JSContext *ctx, JSValueConst v);

/* A PAGE-CREATED VALUE'S CREATION NAME — the FOURTH name source, and the first one that is 1:N with its own
   site and therefore needs an ordinal beside it.
 *
 * IT IS ONE NAME SOURCE OVER SEVERAL CLASSES, AND THE SITE IS THE PART THAT DIFFERS. Every class it names is
 * named (SITE, ORDINAL) out of one ordinal mechanism; what a value is 1:N with is decided by what CREATED it,
 * so the site composer is per class and the class namespace rides the name — `fn@` for a closure, `re@` for a
 * RegExp — which is what keeps two classes clear of one constraint key under one caller-side tag. The class
 * question is THIS function's: a caller that had to ask it would be keeping a list of classes in a file that
 * cannot tell a bound function from a Proxy, which is the whole reason this exists beside JS_OrphanHash.
 *
 * A CLOSURE the page's own code created. Its SITE is JS_OrphanHash's body locator — (script, line, column,
 * body text), already composed and already this engine's one spelling of "which body" — and that alone is NOT
 * a name, because a body is 1:1 with its position while a CLOSURE is not: a factory called three times is one
 * locator and three functions, and a name shared by three closures would have one call's constraint refine
 * another's branch.
 *
 * A REGEXP the page created. Its SITE is the TEXT THE PAGE WROTE — the pattern's code units and the flag word
 * — which is reproducible by the replay a resumed flow performs for the reason a registered symbol's key is.
 * That alone is not a name either, and §22.2.8.1 "lastIndex" is why: the property it defines on every instance
 * "shall have the attributes { [[Writable]]: true, [[Enumerable]]: false, [[Configurable]]: false }", so
 * `r.lastIndex = 5` is observable on one of a pair of identically-spelled RegExps and not on the other
 * whatever their flags are — and a RegExp is an ordinary object besides, on which a page may set own
 * properties and for which `r1 === r2` is false. Source-and-flags names a SET.
 *
 * SO THE ORDINAL IS THE OTHER HALF AND IT COMES FROM THE HOST (JSConcolicHooks.mint_ordinal), minted ONCE, AT
 * CREATION, and carried on the object for the rest of its life. AT CREATION is not an implementation detail —
 * it is what makes the name sound across a fork. A value is FLOW-PRIVATE at the instant it is created (that is
 * the same fact JS_SetFlowGen's generation records), so the ordinal a value carries came from the counter of
 * the one flow that could have been running, and two siblings that each mint ordinal k mint it for two values
 * neither of which the other flow can see. A value created BEFORE a fork was stamped out of the common prefix,
 * so both arms read one name for it. Naming at the ASK instead would break exactly that: two arms would stamp
 * one shared value from two counters, and the loser would go on minting a name the winner had already spent.
 *
 * WHAT IT REFUSES is what JS_OrphanHash and JS_IntrinsicName refuse, unchanged — the live address, which
 * js_malloc REUSES and which no park carries, and any index into a set allocation order decides. What is
 * required is not uniqueness-in-a-heap but REPRODUCIBILITY BY THE REPLAY a resumed flow performs, and both
 * halves have it: the locator is a fact about the bundle, and the ordinal is a fact about the executed prefix,
 * which a replay reproduces by reproducing the prefix.
 *
 * IT ANSWERS ABSENT RATHER THAN ASSERTING, WHICH IS THE WHOLE DIFFERENCE BETWEEN IT AND JS_OrphanHash. That
 * function is handed only values JS_OrphanTakeOne chose and DCHECKs on anything else; this one is handed every
 * operand of every comparison a page makes, so a C function, a bound function and a Proxy reach it as ordinary
 * input — `arr.some(f.bind(this))` is not a defect and an assert here would be a page-held abort switch on it.
 * -1 is "this engine has no creation name for this value", the same answer JS_IntrinsicName's -1 is, and it is
 * also the answer when no host installed mint_ordinal at all.
 *
 * IT IS TEXT AND NOT A HASH for JS_IntrinsicName's reason: the solver spends a name TWICE, once as an
 * operand's IDENTITY and once as its DISPLAY SHAPE, and a shape must separate every pair of operands the
 * identity separates. `buf`/`buf_size` are the caller's and the return is snprintf's — the length that WOULD
 * have been written, so a caller checks it against its own buffer rather than trusting a silent truncation. */
JS_EXTERN int      JS_CreationName(JSContext *ctx, JSValueConst v, char *buf, size_t buf_size);
/* A 3-character class tag (`fn@`, `re@`) + 16 hex digits of site + `#` + a 32-bit decimal ordinal + NUL = 31;
   rounded up, and asserted at the one join every composition passes through, so a caller sized by it cannot
   truncate and a class added with a longer tag fires rather than sharing a prefix with its neighbour. A
   TRUNCATED creation name is two values under one constraint key, which is the exact collision the ordinal
   exists to prevent. */
#define JS_CREATION_NAME_MAX 40
/* MODULE sources: a graph to link and evaluate, not a program to wrap. Returns the evaluation PROMISE. */
JS_EXTERN JSValue  JS_FlowEvalModule(JSContext *ctx, const char *src, size_t len, const char *filename, int eval_flags);   /* eval_flags: JS_EVAL_FLAG_STRICT threaded through; opaque flow handle (NULL on error) */
/* 1 = suspended (preempted), 0 = completed. *pres receives the program's COMPLETION VALUE (or JS_EXCEPTION) on
   completion, JS_UNDEFINED while suspended — a script's completion value is part of its completion, so it is not
   optional: pres==NULL is a DCHECK, never a licence to discard the value the program produced. */
/* JS_FlowResume's third answer. 0 = completed (the caller frees the flow), 1 = suspended (the caller resumes it
   later), and this = the flow's base has been HANDED OFF and is no longer the caller's to free. It happens when
   a base registers itself as a continuation somewhere else — a module body reaching a top-level await moves its
   state onto the awaited promise's reaction — so the scheduler must forget the flow WITHOUT tearing it down. */
#define JS_FLOW_DETACHED 2
JS_EXTERN int      JS_FlowResume(JSContext *ctx, JSValue *flow, JSValue *pres);
JS_EXTERN void     JS_FlowFree(JSContext *ctx, JSValue *flow);
/* ASK THE RUNNING FLOW FOR THE THREAD. Raises the YIELD REQUEST the interpreter polls at EVERY opcode boundary,
   so the flow reaches a suspend point at the very next opcode it executes — in straight-line code, inside a
   getter, inside a callback, inside a direct `eval`, in a body with no loop and no branch. WHERE a flow may
   suspend is a property of the engine and nothing about the page's bytecode shape gates it; this is the call the
   scheduler makes when the answer to "should this flow still hold the thread" has changed and only the scheduler
   knows it: the cooperative quantum expiring, a rank change (an emit, a fork landing on the frontier, a flow
   blocking on a host request), RAM pressure, a cross-instance read.
   It does NOT decide to park — it makes the question askable. The poll clears the request and asks
   JSFlowControlHooks.preempt (kind JS_PREEMPT_HOST), which is where the ONE policy lives.
   Idempotent, cheap, and LOSSLESS: a request nothing consumes (raised while no flow is running, or while a C
   builtin holds the thread) is answered at the next opcode whenever that is, and answering it late costs one
   declined hook call. Per-thread, like the flow base it is about. */
JS_EXTERN void     JS_RequestFlowYield(void);
/* Feature-engagement counters (anti-fake-green): requested = yield polls where the policy asked to park; fired =
   actually parked+rebuilt. requested>fired iff the feature is gated somewhere (nested async/generator activation). */
JS_EXTERN void     JS_FlowPreemptStats(uint64_t *requested, uint64_t *fired);
/* LIVE STEP MACHINES — the runtime's census (see JSStepHdr.census_prev). Every continuation-holding builtin a
   flow is currently suspended inside has one, and a machine nothing finishes is memory JS_ComputeMemoryUsage
   cannot name: a step state is plain js_malloc'd bytes, so it lands in the residual the @HEAP line calls
   `unattributed` with nothing to say what it is. Reported beside that residual for exactly that reason. */
JS_EXTERN int      JS_StepMachineCount(JSRuntime *rt);
/* LIVE HEAP CALL FRAMES — the trampoline's own stack (TrampFrame + its frame buffer), which is the OTHER thing
   JS_ComputeMemoryUsage cannot name and by far the larger of the two: a parked flow IS a suspended chain, so a
   frontier of parked flows holds one of these per suspended call. Reported beside `unattributed` so a run can
   tell a deep-but-live frontier from a chain nothing unwound. */
JS_EXTERN int      JS_TrampFrameCount(JSRuntime *rt);


/* DRIVE-TO-COMPLETION detector: total sites where a coroutine body ran to completion instead of suspend/resume on
   the tramp chain (a generator/async body driven off-tramp). 0 across a test262 corpus = pure suspend/resume. */
JS_EXTERN uint64_t  JS_DriveToCompletionCount(void);
JS_EXTERN uint64_t  JS_SyncDriveToCompletionCount(void);

/* THE PUMP. A flow that preempts inside job-driven code (an async-generator body) parks instead of re-queuing
   behind the job FIFO — re-queuing lets other microtasks run first and CHANGES observable interleaving. The
   host must resume parked flows BEFORE draining a job, so a forced preempt stays transparent to ordering:
   while (JS_ResumeParkedFlow(rt, &completion)) ;  then run one job.
   HOW MANY MAY BE PARKED AT ONCE IS NOT A NUMBER THE ENGINE PICKS. The record used to be a single struct on the
   JSRuntime and a second park aborted; a step reaches as many bases as it reaches, both in a ROW (a host drain
   settling several fetch replies, each its own call root) and NESTED (an async completion settling its promise
   while the reaction that resumed it is still on the C stack). The record now lives on the base it suspends and
   the runtime holds only their ORDER, oldest first — which is the transparency contract as a data structure,
   since continuations then resume in the order they would have completed had no preempt fired. The `while` above
   is unchanged: it always was the drain, and it now has more than one thing to drain. */
typedef void JSFlowParkFn(JSContext *ctx, void *opaque);
/* …AND HOW TO RELEASE THE SAME REFERENCE WITHOUT RUNNING THE BODY. The park OWNS its continuation: every park
   site takes a reference and only the resume discharges it, so an embedder that tears a flow down — or pages it
   out to a cold tier, where a recipe replays the WORK and frees none of the MEMORY — had no way to give it back
   and dropped the last pointer to an activation and its whole heap call chain. Recorded BESIDE the resume so
   each site states how its own reference is released next to where it takes it; the alternative is one disposer
   that compares `fn` against the known park functions, which is a table that must be edited every time a fourth
   site parks and is silently wrong until someone notices. */
typedef void JSFlowParkFreeFn(JSContext *ctx, void *opaque);
/* Parking is the ENGINE's, never an embedder's: a park is a property of a suspended flow BASE, and a base is
   not a thing outside quickjs.c can hold. There is no JS_ParkFlow — what a host has is the drain below. */
JS_EXTERN bool     JS_HasParkedFlow(JSRuntime *rt);
/* IS ANY JS ACTIVATION ON THIS THREAD RIGHT NOW — the runtime's current stack frame, exported because nothing
   outside the interpreter can otherwise tell engine code running BETWEEN a flow's tasks from engine code the
   page called into. A flow's own suspended-frame handle answers neither question: it is NULL while a queued
   job's callback runs, so a consumer that read it would call a builtin invoked from a promise reaction
   "between tasks" — and a sibling built on that belief resumes at a scheduler step that will never re-reach
   the ask. */
JS_EXTERN bool     JS_HasActivation(JSRuntime *rt);
/* RESUME THE PARKED CONTINUATION, AND HAND BACK ITS COMPLETION — `*pres` is JS_UNDEFINED for a normal one and
   JS_EXCEPTION with the throw still live in the context for an abrupt one, which is JS_FlowResume's `pres`
   contract and deliberately the same one: a parked continuation IS a piece of a flow, so it completes the way a
   flow completes, and one contract read at two entries is what stops a host from handling only the entry it
   happened to think of. Returns false (and writes JS_UNDEFINED) when nothing was parked.
   IT IS A PARAMETER BECAUSE THE COMPLETION USED TO HAVE NOWHERE TO GO. `JSFlowParkFn` returns void, so every
   park site left its abrupt completion standing in `rt->current_exception` and relied on prose — "the pump's
   caller reads it from the context" — to name a reader that one host did not have. That slot is per-RUNTIME
   while a completion is per-EVALUATION (ECMA-262 §6.2.4 The Completion Record Specification Type; §5.2.4.3
   Shorthands for Unwrapping Completion Records: "?" propagates an abrupt completion TO THE CALLER), so a throw
   left in it outlives the evaluation that produced it and is found by whatever the scheduler runs next — under
   an interleaving host, a different flow's timeline entirely. Taking it here is what makes the slot empty
   again at the one boundary where it can be.
   The read is done ONCE, by the pump, rather than by each park site: a fourth site that forgot would report a
   normal completion over a live throw, which is the exact defect this parameter exists to make impossible. */
JS_EXTERN bool     JS_ResumeParkedFlow(JSRuntime *rt, JSValue *pres);
/* A PARKED FLOW IS A PIECE OF ONE FLOW'S TIMELINE, so a host that INTERLEAVES flows must carry them with the
   flow rather than leave them in the runtime. Each continuation owns a suspended async activation and resumes
   under that flow's COW delta; a scheduler that switched to a sibling and resumed one there would run it
   against the wrong heap, and one that simply left it behind would DROP it — the one thing the frontier may
   never do. So the switch takes them out and the switch back puts them in, exactly as the delta and the
   decision cursor swap.
   THE UNIT IS THE SET, and it is the set for the same reason the record moved onto the base: a flow may have
   parked several within one step, and a take that moved only the first would leave the rest in the runtime for
   whichever flow ran next — both failure modes above, applied to all but one. The handle is opaque and carries
   its own order; NULL means "no parks", which is what an empty set and an absent one both are. A host that runs
   ONE flow (run-test262) needs neither: it drains before anything else runs. */
JS_EXTERN void    *JS_TakeParkedFlows(JSRuntime *rt);
JS_EXTERN void     JS_PutParkedFlows(JSRuntime *rt, void *parked);
/* …AND THE SET THAT WILL NEVER RUN, released through each member's own disposer — a flow the host tears down or
   pages out to the cold tier. It is the ONLY way a park ends without being resumed: the base teardowns assert
   that no park still names them, so a flow released around this leaves the pump's order pointing into freed
   memory. Takes the handle a JS_TakeParkedFlows returned and empties it. */
JS_EXTERN void     JS_FreeParkedFlows(void *parked);
/* …AND THE SET AN ARM GETS, which is the OTHER half of the frame snapshot below. A flow suspended inside a
   PROGRAM forks by cloning its frame (JS_FlowClone); a flow suspended inside a JOB has no frame at all — a job
   runs only between programs — and its suspension IS its parked continuations, so an arm of it is a clone of
   THOSE. Returns a detached ring in the same currency the take does, for the flow that is switched IN (its
   parks are in the runtime, not on it): put it on the arm, and the arm's switch-in hands it to the pump.
   ONE PARK PER ARM: each member is a clone parked on its own record, and the source set is left untouched —
   a park is one activation's one resume point, so two records naming one activation resume it twice and the
   second rebuilds a frame the first already freed. NULL when the runtime holds no parks. */
JS_EXTERN void    *JS_CloneParkedFlows(JSContext *ctx);
/* Frame-snapshot fork: deep-copy a SUSPENDED flow into an INDEPENDENT clone that resumes from the same point.
   No fallback — an un-built frame shape (deep tramp chain / live closures) crashes loud. */
JS_EXTERN JSValue *JS_FlowClone(JSContext *ctx, JSValue *flow);
/* if can_block is true, Atomics.wait() can be used */
JS_EXTERN void JS_SetCanBlock(JSRuntime *rt, bool can_block);
/* set the [IsHTMLDDA] internal slot */
JS_EXTERN void JS_SetIsHTMLDDA(JSContext *ctx, JSValueConst obj);

typedef struct JSModuleDef JSModuleDef;

/* return the module specifier (allocated with js_malloc()) or NULL if
   exception */
typedef char *JSModuleNormalizeFunc(JSContext *ctx,
                                    const char *module_base_name,
                                    const char *module_name, void *opaque);
typedef char *JSModuleNormalizeFunc2(JSContext *ctx,
                                     const char *module_base_name,
                                     const char *module_name,
                                     JSValueConst attributes,
                                     void *opaque);
typedef JSModuleDef *JSModuleLoaderFunc(JSContext *ctx,
                                        const char *module_name, void *opaque);

/* module loader with import attributes support */
typedef JSModuleDef *JSModuleLoaderFunc2(JSContext *ctx,
                                         const char *module_name, void *opaque,
                                         JSValueConst attributes);

/* return -1 if exception, 0 if OK */
typedef int JSModuleCheckSupportedImportAttributes(JSContext *ctx, void *opaque,
                                                   JSValueConst attributes);

/* module_normalize = NULL is allowed and invokes the default module
   filename normalizer */
JS_EXTERN void JS_SetModuleLoaderFunc(JSRuntime *rt,
                                      JSModuleNormalizeFunc *module_normalize,
                                      JSModuleLoaderFunc *module_loader, void *opaque);

/* same as JS_SetModuleLoaderFunc but with import attributes support */
JS_EXTERN void JS_SetModuleLoaderFunc2(JSRuntime *rt,
                                       JSModuleNormalizeFunc *module_normalize,
                                       JSModuleLoaderFunc2 *module_loader,
                                       JSModuleCheckSupportedImportAttributes *module_check_attrs,
                                       void *opaque);

/* HostLoadImportedModule (16.2.1.10) is permitted to complete ASYNCHRONOUSLY, and a browser's always does — a
   module's source comes off the network. A loader that has the bytes returns the JSModuleDef as before; one
   that must fetch them calls this with a promise that settles with the module's SOURCE TEXT and then returns
   NULL WITHOUT throwing. The load finishes on that promise's reaction: the source is compiled under the
   specifier (so a second import of it finds the record loaded), linked, evaluated, and the import's promise
   settles with the namespace. The importing flow stays suspended on its own `await` throughout — the scope that
   ran the import is never re-run, which would be a replay. */
JS_EXTERN void JS_ModuleLoadPending(JSContext *ctx, JSValue source_promise);

/* Set an attributes-aware module normalizer. Call after JS_SetModuleLoaderFunc2. */
JS_EXTERN void JS_SetModuleNormalizeFunc2(JSRuntime *rt,
                                          JSModuleNormalizeFunc2 *module_normalize);

/* return the import.meta object of a module */
JS_EXTERN JSValue JS_GetImportMeta(JSContext *ctx, JSModuleDef *m);
JS_EXTERN JSAtom JS_GetModuleName(JSContext *ctx, JSModuleDef *m);
JS_EXTERN JSValue JS_GetModuleNamespace(JSContext *ctx, JSModuleDef *m);

/* associate a JSValue to a C module */
JS_EXTERN int JS_SetModulePrivateValue(JSContext *ctx, JSModuleDef *m, JSValue val);
JS_EXTERN JSValue JS_GetModulePrivateValue(JSContext *ctx, JSModuleDef *m);

/* JS Job support */

typedef JSValue JSJobFunc(JSContext *ctx, int argc, JSValueConst *argv);
JS_EXTERN int JS_EnqueueJob(JSContext *ctx, JSJobFunc *job_func,
                            int argc, JSValueConst *argv);
/* …onto a TASK SOURCE instead of the microtask queue. See JS_EnqueueCallTask, whose `source` this takes for
   the same reason and under the same contract. */
JS_EXTERN int JS_EnqueueTaskJob(JSContext *ctx, JSJobFunc *job_func,
                                int argc, JSValueConst *argv, JSTaskSource source);

/* forced-exec ASYNC-AS-FLOW: every enqueued job (a promise .then/.catch/.finally reaction, queueMicrotask, a
   thenable-resolve, a dynamic-import continuation) is routed to the SCHEDULER as a first-class flow instead of a
   global drain loop. This hook is called at JS_EnqueueJob time; it returns 1 if the host took OWNERSHIP of the
   job (the fork then does NOT add it to the global job list), or 0 to fall back to the default global enqueue.
   The host dups argv and later invokes job_func(ctx, argc, argv) under the enqueuing flow's per-flow COW — so a
   reaction runs in its flow's timeline (correct microtask ordering) and is isolated from other flows' reactions.
   job_func is an opaque JSJobFunc pointer the host calls back through; it never needs the quickjs-internal symbol.
   `is_task` says WHICH of HTML 8.1.7's two queues this belongs to — false for a microtask, true for a task
   source. A host that takes ownership takes the ordering rule with it: it may not begin a task while any
   microtask it holds is still outstanding. It is a parameter rather than a second hook because a host that
   registers one and forgets the other silently loses every job of the kind it forgot.
   `handle` IS THE JOB'S IDENTITY and the host must record it beside the job, because the host is then the only
   thing that can find it again: the removal hook below names a job by this and by nothing else. It is allocated
   HERE rather than by the host so that one runtime issues every handle — a host-side counter would collide with
   the runtime's own queues the moment a callback is queued with no flow to own it. It is carried across the
   baseline handover too, so a task the user agent queued before the frontier existed keeps the name its tracker
   already holds.
   `source` IS §8.1.7.1's TASK SOURCE and travels for the same reason `is_task` does: the host owns the queues,
   so the host is the only thing that can answer whether one source has landed in two of them, and it cannot
   answer that from a value it was never handed. JS_TASK_SOURCE_UNSTATED whenever `is_task` is false — a microtask has no
   source — which makes the pair a two-sided statement the host can assert rather than a field it must trust. */
typedef int (*JSJobEnqueueHook)(JSContext *ctx, JSJobFunc *job_func, int argc, JSValueConst *argv,
                                bool is_task, JSTaskSource source, JSTaskHandle handle);
JS_EXTERN void JS_SetJobEnqueueHook(JSJobEnqueueHook h);

/* THE OTHER HALF OF OWNERSHIP. A host that TOOK a job is the only thing that can give it back, so the drop
   below asks it — HTML §7.5.10 step 7 removes every task whose document is a destroyed Document "without
   running those tasks", and a task that ran anyway would script a document whose browsing context is null.
   The hook answers how many it dropped, and is asked for a REALM because that is what a queued job records:
   the enqueue hook is handed `ctx` and a document is one realm. A host that registers the enqueue hook and
   not this one silently keeps every task of every destroyed document, which is the same failure as
   registering one queue's hook and forgetting the other's. */
typedef int (*JSJobDropHook)(JSContext *ctx);
JS_EXTERN void JS_SetJobDropHook(JSJobDropHook h);

/* HTML §7.5.10 step 7 — remove every queued job belonging to `ctx` WITHOUT running it, from the runtime's own
   two queues and from whatever the enqueue hook's owner is holding. Answers the number removed, so a caller
   can assert that a second call finds none. */
JS_EXTERN int JS_DropJobsForContext(JSContext *ctx);

/* THE SAME OWNERSHIP SEAM, BY NAME INSTEAD OF BY REALM — the hook a host that TOOK a job answers when ONE job
   is to be taken back off its queue. It never fights JS_DropJobsForContext: that one removes by document and
   this one by identity, so a job the destroy already removed is simply not found here (and the reverse).
   IT IS ASKED OF THE HOST'S RUNNING FLOW ALONE, and that is a statement about the design rather than a
   convenience. A fork COPIES the parent's queued jobs, so after a branch two flows hold two jobs bearing one
   handle — two timelines' copies of the same queued task — and each arm's tracker names its OWN. A hook that
   swept every flow would delete the sibling's task from the sibling's timeline, which is the shared-state bug
   this engine's per-flow queues exist to make impossible. Answers the number removed: 0 or 1. */
typedef int (*JSJobRemoveHook)(JSTaskHandle handle);
JS_EXTERN void JS_SetJobRemoveHook(JSJobRemoveHook h);

/* REMOVE ONE QUEUED CALLBACK BY ITS HANDLE, WITHOUT RUNNING IT — HTML's "remove <task> from its task queue",
   the step every toggle task tracker rests on (§4.11.1 details, §4.11.4 dialog, §6.12 popover).
   ANSWERS 0 OR 1, AND 0 IS ORDINARY. A handle outlives the task it names: the task may already have run, may
   be running RIGHT NOW (a `toggle` listener that closes the dialog again asks for the removal of the very task
   dispatching it — the spec's own no-op, because a running task has already left its queue), or may have gone
   with a destroyed document or a paged-out flow. So this is a lookup that answers honestly, never a use-after-
   free and never an assert. What it DOES assert is that a handle names at most one queued callback anywhere,
   which is the property the monotone allocation is for. */
JS_EXTERN int JS_RemoveQueuedTask(JSRuntime *rt, JSTaskHandle handle);

JS_EXTERN bool JS_IsJobPending(JSRuntime *rt);
JS_EXTERN JSContext *JS_GetPendingJobContext(JSRuntime *rt);
JS_EXTERN int JS_ExecutePendingJob(JSRuntime *rt, JSContext **pctx);

/* Structure to retrieve (de)serialized SharedArrayBuffer objects. */
typedef struct JSSABTab {
    uint8_t **tab;
    size_t len;
} JSSABTab;

/* HTML §2.7.7's `memory`, SEEDED WITH THE TRANSFER LIST — the one fact a serializer cannot derive from the
 * graph it is walking, because it is a fact about the CALL and not about the value.
 *
 * HTML §2.7.7 step 1 puts every entry of transferList into `memory` before StructuredSerializeInternal runs,
 * and HTML §2.7.3's step 2 is "If memory[value] exists, then return memory[value]" — so a transferable REACHED
 * FROM INSIDE the message body serializes as its data holder rather than being cloned or refused. HTML §2.7.8
 * is the mirror: it fills `memory` with the objects its transfer-receiving steps built and only then
 * deserializes, and HTML §2.7.6's step 2 reads that map, so such a reference comes back as THE SAME OBJECT as
 * the corresponding entry of [[TransferredValues]] — not a copy of it.
 * THOSE TWO NUMBERS WERE §2.7.1 AND §2.7.2 AND WERE A RETIRED EDITION'S — `memory` belongs to
 * StructuredSerializeInternal, which is §2.7.3, and §2.7.1 is "Serializable objects". They were invisible while
 * this banner named no standard, and the diff that added HTML anchors here is what moved them into the judged
 * population: writing a standard's name at ONE site re-decides every bare number in the file. That identity is the whole point: it is what makes
 * `port.postMessage({p: other}, [other])` deliver a message whose `p` IS the moved port.
 *
 * THE MAP IS A PARAMETER, NOT A SCOPE. It belongs to ONE serialization: a writer and a reader running in the
 * same turn (a same-agent delivery) each carry their own, and neither can see the other's. A hook installed on
 * the runtime would instead be one map answering for every serialization the agent ever performs.
 *
 * THE TWO HALVES SHARE ONE NUMBERING, and it is the transfer list's. §2.7.7 appends one data holder per list
 * entry in list order; §2.7.8 appends one value per data holder in holder order. So the writer answers an index
 * into the transfer list and the reader resolves that same index against [[TransferredValues]]. */
typedef struct JSTransferWriteHook {
    /* The index of `obj` in the transfer list, or -1 when it is not in it. Called for objects only. */
    int (*index_of)(JSContext *ctx, void *opaque, JSValueConst obj);
    void *opaque;
} JSTransferWriteHook;

typedef struct JSTransferReadHook {
    /* [[TransferredValues]][index], owned by the caller. `count` is that list's length, and the reader refuses
       a stream naming an index outside it rather than asking — an index it cannot resolve is a corrupt stream,
       not a value. A stream carrying such a reference is likewise refused outright by a read given no hook:
       the tag is readable only where the host supplied the map that gives it a meaning. */
    JSValue (*value_at)(JSContext *ctx, void *opaque, uint32_t index);
    uint32_t count;
    void *opaque;
} JSTransferReadHook;

/* HTML §2.7.1's SERIALIZABLE OBJECTS — the host's answer for a platform object, which this writer cannot
 * derive and must not guess. HTML §2.7.1 "Serializable objects" makes the property one of an INTERFACE —
 * "Platform objects can be serializable objects if their primary interface is decorated with the
 * [Serializable] IDL extended attribute" — and the writer below knows only CLASSES. Without these two hooks
 * its class dispatch falls through to a refusal, which is HTML §2.7.3 step 20's answer for a platform object
 * that is NOT serializable and a wrong answer for one that is.
 *
 * THE WIRE CARRIES AN IDENTIFIER AND THEN ONE SUB-VALUE. HTML §2.7.3 StructuredSerializeInternal ( value,
 * forStorage [ , memory ] ) step 19 is "Otherwise, if value is a platform object that is a serializable
 * object:", whose sub-steps are "Let typeString be the identifier of the primary interface of value", "Set
 * serialized to { [[Type]]: typeString }" and "Set deep to true"; its step 26's THIRD arm then performs "the
 * serialization steps for value's primary interface". Those steps "may need to perform a sub-serialization",
 * which the same step defines as "an operation which takes as input a value subValue, and returns
 * StructuredSerializeInternal(subValue, forStorage, memory)" — the SAME walk under the SAME `memory`. So the
 * one value `serialize` answers is pushed on this writer's own work stack rather than handed to a nested
 * JS_WriteObject, which would open a second object_list and neither terminate a cycle nor preserve `===`.
 *
 * THE READER CREATES BEFORE IT FILLS, AND THAT IS HTML §2.7.6's OWN ORDER RATHER THAN A PREFERENCE. Step 22
 * is "Otherwise:" — "Let interfaceName be serialized.[[Type]]", "If the interface identified by interfaceName
 * is not exposed in targetRealm, then throw a \"DataCloneError\" DOMException", "Set value to a new instance
 * of the interface identified by interfaceName, created in targetRealm", "Set deep to true". Step 23 is "Set
 * memory[serialized] to value". Step 24's last arm performs the deserialization steps. So the instance is in
 * the object-reference list BEFORE any sub-value is read, which is what makes a graph reaching one platform
 * object twice come back as ONE object.
 * BC_TAG_TYPED_ARRAY's reserve-and-patch is the wrong idiom to copy and it is the nearest one: it reserves
 * because a typed array cannot exist before its buffer, and HTML §2.7.1 says of these steps that value "will
 * be a newly-created instance of the platform object type in question, with none of its internal data set up;
 * setting that up is the job of these steps". A serializable CAN exist first, so a row is TWO operations. */
typedef struct JSSerializableWriteHook {
    /* HTML §2.7.3 step 19's "the identifier of the primary interface of value", or NULL when `obj` is not a
       serializable platform object — in which case step 20's refusal stands. Called for objects no JavaScript
       arm of the walk took. */
    const char *(*interface_of)(JSContext *ctx, void *opaque, JSValueConst obj);
    /* HTML §2.7.3 step 26's third arm, as ONE owned value the writer then sub-serializes, or JS_EXCEPTION
       with a throw live. ONE value rather than a Record of fields is this embedding's choice and not the
       standard's count: a host that has several fields to write puts them in a holder the same walk reaches. */
    JSValue (*serialize)(JSContext *ctx, void *opaque, JSValueConst obj);
    void *opaque;
} JSSerializableWriteHook;

typedef struct JSSerializableReadHook {
    /* HTML §2.7.6 step 22's "a new instance of the interface identified by interfaceName, created in
       targetRealm", with none of its internal data set up — or JS_EXCEPTION, which is that step's own
       refusal for an interface the target realm does not expose. */
    JSValue (*create)(JSContext *ctx, void *opaque, const char *name);
    /* HTML §2.7.6 step 24's last arm, given the instance `create` answered and the sub-deserialization of the
       one value the write produced. 0, or -1 with a throw live. */
    int (*deserialize)(JSContext *ctx, void *opaque, JSValueConst value, JSValueConst sub);
    void *opaque;
} JSSerializableReadHook;

/* Object Writer/Reader (currently only used to handle precompiled code) */
#define JS_WRITE_OBJ_BYTECODE  (1 << 0) /* allow function/module */
#define JS_WRITE_OBJ_BSWAP     (0)      /* byte swapped output (obsolete, handled transparently) */
#define JS_WRITE_OBJ_SAB       (1 << 2) /* allow SharedArrayBuffer */
#define JS_WRITE_OBJ_REFERENCE (1 << 3) /* allow object references to encode arbitrary object graph */
#define JS_WRITE_OBJ_STRIP_SOURCE  (1 << 4) /* do not write source code information */
#define JS_WRITE_OBJ_STRIP_DEBUG   (1 << 5) /* do not write debug information */
JS_EXTERN uint8_t *JS_WriteObject(JSContext *ctx, size_t *psize, JSValueConst obj, int flags);
JS_EXTERN uint8_t *JS_WriteObject2(JSContext *ctx, size_t *psize, JSValueConst obj,
                                   int flags, JSSABTab *psab_tab);
/* ... and with §2.7.7's seeded `memory`. NULL is an unseeded one, which is every write that is not a
   StructuredSerializeWithTransfer. */
JS_EXTERN uint8_t *JS_WriteObject3(JSContext *ctx, size_t *psize, JSValueConst obj,
                                   int flags, JSSABTab *psab_tab,
                                   const JSTransferWriteHook *transfer);
/* ... and with HTML §2.7.1's serializable seam beside it. NULL is a write with no seam, which refuses every
   platform object exactly as step 20 does. The two maps are separate parameters because they answer two
   questions: one is about the CALL (which objects did the caller name in its transfer list), the other about
   the INTERFACE (which platform objects does the host serialize at all). */
JS_EXTERN uint8_t *JS_WriteObject4(JSContext *ctx, size_t *psize, JSValueConst obj,
                                   int flags, JSSABTab *psab_tab,
                                   const JSTransferWriteHook *transfer,
                                   const JSSerializableWriteHook *serializable);

/* WARNING: only enable JS_READ_OBJ_BYTECODE on input from a trusted
   writer. The bytecode format is not designed to resist a hostile
   producer; loading adversarial bytecode can lead to memory corruption. */
#define JS_READ_OBJ_BYTECODE  (1 << 0) /* allow function/module */
#define JS_READ_OBJ_ROM_DATA  (0)      /* avoid duplicating 'buf' data (obsolete, broken by ICs) */
/* WARNING: serialized SharedArrayBuffers carry a literal host pointer in
   the blob; only enable JS_READ_OBJ_SAB on input produced by a trusted
   writer in the same process (e.g. another Worker on the same runtime). */
#define JS_READ_OBJ_SAB       (1 << 2) /* allow SharedArrayBuffer */
#define JS_READ_OBJ_REFERENCE (1 << 3) /* allow object references */
JS_EXTERN JSValue JS_ReadObject(JSContext *ctx, const uint8_t *buf, size_t buf_len, int flags);
JS_EXTERN JSValue JS_ReadObject2(JSContext *ctx, const uint8_t *buf, size_t buf_len,
                                 int flags, JSSABTab *psab_tab);
/* ... and with §2.7.8's `memory`, the [[TransferredValues]] its transfer-receiving steps have already built. */
JS_EXTERN JSValue JS_ReadObject3(JSContext *ctx, const uint8_t *buf, size_t buf_len,
                                 int flags, JSSABTab *psab_tab,
                                 const JSTransferReadHook *transfer);
/* ... and with HTML §2.7.1's serializable seam. A stream naming a serialized platform object is refused
   outright by a read given none: the tag has no meaning a reader may invent. */
JS_EXTERN JSValue JS_ReadObject4(JSContext *ctx, const uint8_t *buf, size_t buf_len,
                                 int flags, JSSABTab *psab_tab,
                                 const JSTransferReadHook *transfer,
                                 const JSSerializableReadHook *serializable);
/* Instantiate and evaluate a bytecode function. Only used when reading a script or module with JS_ReadObject().
   FOR A MODULE THIS IS THE WHOLE OF HTML §8.1.4.4 Calling scripts' "run a module script": it LOADS the graph
   (16.2.1.6.1.1 LoadRequestedModules), and only upon that load's fulfilment LINKS and EVALUATES it — so it
   returns a PROMISE that settles as the module's own evaluation promise does, and the embedder must pump jobs
   for it to progress. A JS_ResolveModule stood beside it to load the graph up front; loading cannot be
   synchronous (a host answers 16.2.1.10 whenever the network does) so there was nothing left for it to do that
   this call does not, and a caller that "resolved" first was asserting a phase order it could not enforce. */
JS_EXTERN JSValue JS_EvalFunction(JSContext *ctx, JSValue fun_obj);

/* only exported for os.Worker() */
JS_EXTERN JSAtom JS_GetScriptOrModuleName(JSContext *ctx, int n_stack_levels);
/* only exported for os.Worker() */
JS_EXTERN JSValue JS_LoadModule(JSContext *ctx, const char *basename,
                                const char *filename);

/* C function definition */
typedef enum JSCFunctionEnum {  /* XXX: should rename for namespace isolation */
    JS_CFUNC_generic,
    JS_CFUNC_generic_magic,
    JS_CFUNC_constructor,
    JS_CFUNC_constructor_magic,
    JS_CFUNC_constructor_or_func,
    JS_CFUNC_constructor_or_func_magic,
    JS_CFUNC_f_f,
    JS_CFUNC_f_f_f,
    JS_CFUNC_getter,
    JS_CFUNC_setter,
    JS_CFUNC_getter_magic,
    JS_CFUNC_setter_magic,
    JS_CFUNC_iterator_next,
    /* A CONTINUATION-HOLDING builtin: it re-enters JS from a loop whose state lives in C locals, and a C frame
       cannot be parked. So the builtin IS a step machine — there is no JS_Call loop version of it to fall back
       to — and `magic` indexes its row in the step table. */
    JS_CFUNC_step,
    JS_CFUNC_consume,     /* a builtin with NO C BODY whose machine is named by an ITERCONS_* id in magic, so the
                             callee CARRIES which machine it wants and the interpreter asks ONE question instead
                             of a per-builtin identity test. Same shape as JS_CFUNC_step: the function pointer is
                             unused, because there is no C body left to call.
                             Named for what nearly all of them do — CONSUME an iterator in a loop — but the id
                             namespace answers WHICH MACHINE, and several of its members are not walks over an
                             iterator at all: Array.from over an array-like (length + indices, no @@iterator),
                             Iterator.from, the keyed promise combinators (own keys), and Promise.try (a
                             callback). Those are different ALGORITHMS reached through one declaration, never
                             fallbacks for the walk. */
    JS_CFUNC_iterdrive,   /* a lazy Iterator Helper's own .next(): the DRIVE is a coroutine on the tramp
                             (do_iter_helper_step), and there is no C body left to call. It needs a cproto of its
                             own rather than JS_CFUNC_step because its DELIVERY has modes — the same drive
                             finishes into a direct call, a for-of, an OP_iterator_next, or a consuming machine —
                             which a step machine's push-the-result cannot express. The function pointer is
                             unused, as for consume and step. */
    JS_CFUNC_step_ctor,   /* a step machine that is ALSO a constructor (String): `new C(x)` and `C(x)` are two
                             spellings of one builtin, and which one ran is already carried by the receiver slot
                             (new_target for a construct, undefined for a call). Separate from JS_CFUNC_step only
                             so that a step METHOD stays a non-constructor — `new String.prototype.concat()` must
                             throw — and so IsConstructor answers true for the ones that are. */
} JSCFunctionEnum;

typedef union JSCFunctionType {
    JSCFunction *generic;
    JSValue (*generic_magic)(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic);
    JSCFunction *constructor;
    JSValue (*constructor_magic)(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv, int magic);
    JSCFunction *constructor_or_func;
    double (*f_f)(double);
    double (*f_f_f)(double, double);
    JSValue (*getter)(JSContext *ctx, JSValueConst this_val);
    JSValue (*setter)(JSContext *ctx, JSValueConst this_val, JSValueConst val);
    JSValue (*getter_magic)(JSContext *ctx, JSValueConst this_val, int magic);
    JSValue (*setter_magic)(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic);
    JSValue (*iterator_next)(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int *pdone, int magic);
} JSCFunctionType;

JS_EXTERN JSValue JS_NewCFunction2(JSContext *ctx, JSCFunction *func,
                                   const char *name,
                                   int length, JSCFunctionEnum cproto, int magic);
JS_EXTERN JSValue JS_NewCFunction3(JSContext *ctx, JSCFunction *func,
                                   const char *name,
                                   int length, JSCFunctionEnum cproto, int magic,
                                   JSValueConst proto_val, int n_fields);
JS_EXTERN JSValue JS_NewCFunctionData(JSContext *ctx, JSCFunctionData *func,
                                      int length, int magic, int data_len,
                                      JSValueConst *data);
JS_EXTERN JSValue JS_NewCFunctionData2(JSContext *ctx, JSCFunctionData *func,
                                       const char *name,
                                       int length, int magic, int data_len,
                                       JSValueConst *data);
/* DECLARE THAT AN ATTRIBUTE GETTER'S C BODY RUNS NONE OF THE PAGE'S CODE — the C-function twin of
   JSClassExoticMethods' get_own_property_no_user_code below, made at the point the member is installed.
 *
 * WHAT IT BUYS. A property read that lands on an accessor may not invoke it from C: §10.1.8.1 OrdinaryGet
 * ( obj, propertyKey, receiver ) step 7 is `Return ? Call(getter, receiver)`, the getter is normally the page's
 * function, and a C activation has no flow base under it — a loop in that body would drive to completion
 * instead of parking, so the read opcodes route it onto the trampoline and a C reader that reaches one aborts
 * naming the site. That reasoning is entirely about a body that RUNS THE PAGE. It is vacuous for a C getter
 * that runs none: there is no continuation to hold, nothing to preempt, and the routed path reaches the very
 * same C function through the very same dispatch. A member that says so is answered where it is asked.
 *
 * WHAT IT DOES NOT BUY, AND THIS IS THE HALF THAT MATTERS. The default is UNDECLARED and stays undeclared:
 * saying nothing means "this needs routing, and it crashes". The declaration is not a way to quiet an abort —
 * it is a CLAIM, and a false one FIRES: while a declared getter runs, entry into any bytecode body aborts and
 * names this member, because bytecode is the one door the page's code comes through. A getter that later gains
 * a route to the page — a [[Get]] on an object whose prototype the page can extend, a coercion of a value the
 * page can make an object, a callback — therefore crashes rather than silently keeping an exemption it has
 * stopped deserving. Declare a member only where you would be content for the engine to prove you wrong.
 *
 * It takes the minted FUNCTION OBJECT, so the claim is per member and never per installer, per file or per
 * function pointer: two members sharing one C body with different magics are two objects and two decisions. */
JS_EXTERN void JS_DeclareCFunctionNoUserCode(JSValueConst func_obj);

typedef void JSCClosureFinalizerFunc(void*);
JS_EXTERN JSValue JS_NewCClosure(JSContext *ctx, JSCClosure *func,
                                 const char *name,
                                 JSCClosureFinalizerFunc *opaque_finalize,
                                 int length, int magic, void *opaque);

static inline JSValue JS_NewCFunction(JSContext *ctx, JSCFunction *func,
                                      const char *name, int length)
{
    return JS_NewCFunction2(ctx, func, name, length, JS_CFUNC_generic, 0);
}

static inline JSValue JS_NewCFunctionMagic(JSContext *ctx, JSCFunctionMagic *func,
                                           const char *name, int length,
                                           JSCFunctionEnum cproto, int magic)
{
    /* Used to squelch a -Wcast-function-type warning. */
    JSCFunctionType ft;
    ft.generic_magic = func;
    return JS_NewCFunction2(ctx, ft.generic, name, length, cproto, magic);
}
JS_EXTERN int JS_SetConstructor(JSContext *ctx, JSValueConst func_obj,
                                JSValueConst proto);

/* C property definition */

typedef struct JSCFunctionListEntry {
    const char *name;       /* pure ASCII or UTF-8 encoded */
    uint8_t prop_flags;
    uint8_t def_type;
    int16_t magic;
    union {
        struct {
            uint8_t length; /* XXX: should move outside union */
            uint8_t cproto; /* XXX: should move outside union */
            JSCFunctionType cfunc;
        } func;
        struct {
            JSCFunctionType get;
            JSCFunctionType set;
        } getset;
        struct {
            int16_t get_id;     /* step-definition id of the getter */
            int16_t set_id;     /* step-definition id of the setter, or -1 for a getter-only accessor */
        } getset_step;
        struct {
            const char *name;
            int base;
        } alias;
        struct {
            const struct JSCFunctionListEntry *tab;
            int len;
        } prop_list;
        const char *str;    /* pure ASCII or UTF-8 encoded */
        int32_t i32;
        int64_t i64;
        uint64_t u64;
        double f64;
    } u;
} JSCFunctionListEntry;

#define JS_DEF_CFUNC          0
#define JS_DEF_CGETSET        1
#define JS_DEF_CGETSET_MAGIC  2
#define JS_DEF_PROP_STRING    3
#define JS_DEF_PROP_INT32     4
#define JS_DEF_PROP_INT64     5
#define JS_DEF_PROP_DOUBLE    6
#define JS_DEF_PROP_UNDEFINED 7
#define JS_DEF_OBJECT         8
#define JS_DEF_ALIAS          9
#define JS_DEF_PROP_SYMBOL   10
#define JS_DEF_PROP_BOOL     11
#define JS_DEF_CGETSET_STEP  12
#define JS_DEF_CGETSET_STEP_BOTH 13

/* Note: c++ does not like nested designators */
#define JS_CFUNC_DEF(name, length, func1) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, 0, { .func = { length, JS_CFUNC_generic, { .generic = func1 } } } }
#define JS_CFUNC_DEF2(name, length, func1, prop_flags) { name, prop_flags, JS_DEF_CFUNC, 0, { .func = { length, JS_CFUNC_generic, { .generic = func1 } } } }
/* `stepid` is a NAMED STEPDEF_* constant, so the registration line says which machine runs — the same readability
   as JS_CFUNC_MAGIC_DEF naming its C function — and a designated-initializer table makes inserting a row
   incapable of silently repointing an existing one. The step def is NOT stored in JSCFunctionType: that union
   holds function pointers, and putting a data pointer in it is a strict-aliasing violation that survives -O0 and
   segfaults at -O1 (measured: -fno-strict-aliasing passed, plain -O1 crashed). */
#define JS_CFUNC_CONSUME_DEF(name, length, sink) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, sink, { .func = { length, JS_CFUNC_consume, { .generic = NULL } } } }
#define JS_CFUNC_ITERDRIVE_DEF(name, length) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, 0, { .func = { length, JS_CFUNC_iterdrive, { .generic = NULL } } } }
#define JS_CFUNC_STEP_DEF(name, length, stepid) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, stepid, { .func = { length, JS_CFUNC_step, { .generic = NULL } } } }
#define JS_CFUNC_STEP_CTOR_DEF(name, length, stepid) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, stepid, { .func = { length, JS_CFUNC_step_ctor, { .generic = NULL } } } }
#define JS_CFUNC_MAGIC_DEF(name, length, func1, magic) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, magic, { .func = { length, JS_CFUNC_generic_magic, { .generic_magic = func1 } } } }
#define JS_CFUNC_SPECIAL_DEF(name, length, cproto, func1) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, 0, { .func = { length, JS_CFUNC_ ## cproto, { .cproto = func1 } } } }
#define JS_ITERATOR_NEXT_DEF(name, length, func1, magic) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_CFUNC, magic, { .func = { length, JS_CFUNC_iterator_next, { .iterator_next = func1 } } } }
#define JS_CGETSET_DEF(name, fgetter, fsetter) { name, JS_PROP_CONFIGURABLE, JS_DEF_CGETSET, 0, { .getset = { .get = { .getter = fgetter }, .set = { .setter = fsetter } } } }
#define JS_CGETSET_DEF2(name, fgetter, fsetter, prop_flags) { name, prop_flags, JS_DEF_CGETSET, 0, { .getset = { .get = { .getter = fgetter }, .set = { .setter = fsetter } } } }
/* An ACCESSOR whose SETTER is a step machine. A setter is as much the page's entry point as a method is — the
   value it is handed is the page's, and what it does with it (a [[GetOwnProperty]], a Set, a define) is the
   page's code the moment the receiver is a Proxy. The getter stays an ordinary C function; `stepid` names the
   machine the setter runs, exactly as JS_CFUNC_STEP_DEF does. */
#define JS_CGETSET_STEP_DEF(name, fgetter, stepid) { name, JS_PROP_CONFIGURABLE, JS_DEF_CGETSET_STEP, stepid, { .getset = { .get = { .getter = fgetter }, .set = { .setter = NULL } } } }
/* BOTH halves of an accessor are step machines. The ids go in a data member of the entry union, never in
   JSCFunctionType — that union holds FUNCTION pointers, and a data pointer in it is the strict-aliasing trap
   this file learned the hard way. A set_id of -1 declares a getter-only accessor. */
#define JS_CGETSET_STEP_BOTH_DEF(name, get_stepid, set_stepid) { name, JS_PROP_CONFIGURABLE, JS_DEF_CGETSET_STEP_BOTH, 0, { .getset_step = { get_stepid, set_stepid } } }
#define JS_CGETSET_MAGIC_DEF(name, fgetter, fsetter, magic) { name, JS_PROP_CONFIGURABLE, JS_DEF_CGETSET_MAGIC, magic, { .getset = { .get = { .getter_magic = fgetter }, .set = { .setter_magic = fsetter } } } }
/* The same with EXPLICIT property attributes, for a WEB IDL interface rather than an ECMAScript builtin. The
   two disagree: §3.7.6 makes an interface's attributes { enumerable: true, configurable: true } where an
   ECMAScript accessor is configurable-only, and the difference is observable — a non-enumerable `name` on
   DOMException.prototype is one Web IDL's own record conversion SKIPS, so the brand check that should have
   thrown never ran. */
#define JS_CGETSET_MAGIC_DEF2(name, fgetter, fsetter, magic, prop_flags) { name, prop_flags, JS_DEF_CGETSET_MAGIC, magic, { .getset = { .get = { .getter_magic = fgetter }, .set = { .setter_magic = fsetter } } } }
#define JS_PROP_STRING_DEF(name, cstr, prop_flags) { name, prop_flags, JS_DEF_PROP_STRING, 0, { .str = cstr } }
#define JS_PROP_INT32_DEF(name, val, prop_flags) { name, prop_flags, JS_DEF_PROP_INT32, 0, { .i32 = val } }
#define JS_PROP_INT64_DEF(name, val, prop_flags) { name, prop_flags, JS_DEF_PROP_INT64, 0, { .i64 = val } }
#define JS_PROP_DOUBLE_DEF(name, val, prop_flags) { name, prop_flags, JS_DEF_PROP_DOUBLE, 0, { .f64 = val } }
#define JS_PROP_U2D_DEF(name, val, prop_flags) { name, prop_flags, JS_DEF_PROP_DOUBLE, 0, { .u64 = val } }
#define JS_PROP_UNDEFINED_DEF(name, prop_flags) { name, prop_flags, JS_DEF_PROP_UNDEFINED, 0, { .i32 = 0 } }
#define JS_PROP_SYMBOL_DEF(name, val, prop_flags) { name, prop_flags, JS_DEF_PROP_SYMBOL, 0, { .i32 = val } }
#define JS_PROP_BOOL_DEF(name, val, prop_flags) { name, prop_flags, JS_DEF_PROP_BOOL, 0, { .i32 = val } }
#define JS_OBJECT_DEF(name, tab, len, prop_flags) { name, prop_flags, JS_DEF_OBJECT, 0, { .prop_list = { tab, len } } }
#define JS_ALIAS_DEF(name, from) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_ALIAS, 0, { .alias = { from, -1 } } }
#define JS_ALIAS_BASE_DEF(name, from, base) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE, JS_DEF_ALIAS, 0, { .alias = { from, base } } }

JS_EXTERN int JS_SetPropertyFunctionList(JSContext *ctx, JSValueConst obj,
                                          const JSCFunctionListEntry *tab,
                                          int len);

/* C module definition */

typedef int JSModuleInitFunc(JSContext *ctx, JSModuleDef *m);

JS_EXTERN JSModuleDef *JS_NewCModule(JSContext *ctx, const char *name_str,
                                     JSModuleInitFunc *func);
/* can only be called before the module is instantiated */
JS_EXTERN int JS_AddModuleExport(JSContext *ctx, JSModuleDef *m, const char *name_str);
JS_EXTERN int JS_AddModuleExportList(JSContext *ctx, JSModuleDef *m,
                                      const JSCFunctionListEntry *tab, int len);
/* can only be called after the module is instantiated */
JS_EXTERN int JS_SetModuleExport(JSContext *ctx, JSModuleDef *m, const char *export_name,
                                 JSValue val);
JS_EXTERN int JS_SetModuleExportList(JSContext *ctx, JSModuleDef *m,
                                     const JSCFunctionListEntry *tab, int len);

/* Version */

#define QJS_VERSION_MAJOR 0
#define QJS_VERSION_MINOR 15
#define QJS_VERSION_PATCH 1
#define QJS_VERSION_SUFFIX ""

JS_EXTERN const char* JS_GetVersion(void);

/* Integration point for quickjs-libc.c, not for public use. */
JS_EXTERN uintptr_t js_std_cmd(int cmd, ...);

#ifdef __cplusplus
} /* extern "C" { */
#endif

#endif /* QUICKJS_H */
