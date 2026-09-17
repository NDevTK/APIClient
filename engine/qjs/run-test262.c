/*
 * ECMA Test 262 Runner for QuickJS
 *
 * Copyright (c) 2017-2021 Fabrice Bellard
 * Copyright (c) 2017-2021 Charlie Gordon
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
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

#include "cutils.h"
#include "list.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "quickjs-c-atomics.h"
#include "quickjs-libc.h"

#define CMD_NAME "run-test262"

/* Atomics.waitAsync's TIMEOUT IS A HOST JOB — 25.4.3.14 registers the waiter and hands the timeout to the
   embedder, which is why the engine only settles and never sleeps inside the pump. THIS is the embedder's
   event loop, so the waiting is here.
   `may_wait` is the difference between the two places this is called from. On EVERY iteration the loop settles
   what has come due without waiting: atomicsHelper.js implements $262.agent.setTimeout by chaining promise
   reactions, so a test with a pending timer never empties the job queue, and a deadline that is only checked
   when the queue drains is a deadline that never fires. Only when the pump reports empty does the loop have
   nothing better to do than wait for the next one. */
static bool host_pump_atomics_async(JSRuntime *rt, bool may_wait)
{
    if (JS_AtomicsExpireAsync(rt) > 0)
        return true;
    return may_wait && JS_AtomicsAsyncWaitForWork(rt);
}

// not quite correct because in theory someone could compile quickjs.c
// with a different compiler but in practice no one does that, right?
#ifdef __TINYC__
#define CC_IS_TCC 1
#else
#define CC_IS_TCC 0
#endif

typedef struct {
    js_mutex_t agent_mutex;
    js_cond_t agent_cond;
    /* list of Test262Agent.link */
    struct list_head agent_list;
    js_mutex_t report_mutex;
    /* list of AgentReport.link */
    struct list_head report_list;
    int async_done;
    /* $DONE(err)'s message, so a FAILING async test reports what failed. Without it async_done goes to 2 and
       the check below reports "$DONE() not called" — which is not what happened, and sends the reader looking
       for a hang instead of reading the assertion. */
    char async_fail[512];
} ThreadLocalStorage;

typedef struct namelist_t {
    char **array;
    int count;
    int size;
} namelist_t;

long nthreads; // invariant: 0 < nthreads < countof(threads)
js_thread_t threads[32];
js_thread_t progress_thread;
js_cond_t progress_cond;
js_mutex_t progress_mutex;

namelist_t test_list;
namelist_t exclude_list;
namelist_t exclude_dir_list;

FILE *outfile;
enum test_mode_t {
    TEST_DEFAULT_NOSTRICT, /* run tests as nostrict unless test is flagged as strictonly */
    TEST_DEFAULT_STRICT,   /* run tests as strict unless test is flagged as nostrict */
    TEST_NOSTRICT,         /* run tests as nostrict, skip strictonly tests */
    TEST_STRICT,           /* run tests as strict, skip nostrict tests */
    TEST_ALL,              /* run tests in both strict and nostrict, unless restricted by spec */
} test_mode = TEST_DEFAULT_NOSTRICT;
int local;
int skip_async;
int skip_module;
int dump_memory;
int stats_count;
JSMemoryUsage stats_all, stats_avg, stats_min, stats_max;
char *stats_min_filename;
char *stats_max_filename;
js_mutex_t stats_mutex;
int verbose;
char *harness_dir;
char *harness_exclude;
char *harness_features;
char *harness_skip_features;
char *error_filename;
char *error_file;
FILE *error_out;
int update_errors;
int slow_test_threshold;
int start_index, stop_index;
int test_excluded;
_Atomic int test_count, test_failed, test_skipped;
_Atomic int new_errors, changed_errors, fixed_errors;

void warning(const char *, ...) __attribute__((__format__(__printf__, 1, 2)));
void fatal(int, const char *, ...) __attribute__((__format__(__printf__, 2, 3)));

void atomic_inc(volatile _Atomic int *p)
{
    atomic_fetch_add(p, 1);
}

void warning(const char *fmt, ...)
{
    va_list ap;

    fflush(stdout);
    fprintf(stderr, "%s: ", CMD_NAME);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void fatal(int errcode, const char *fmt, ...)
{
    va_list ap;

    fflush(stdout);
    fprintf(stderr, "%s: ", CMD_NAME);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);

    exit(errcode);
}

void perror_exit(int errcode, const char *s)
{
    fflush(stdout);
    fprintf(stderr, "%s: ", CMD_NAME);
    perror(s);
    exit(errcode);
}

char *strdup_len(const char *str, int len)
{
    char *p = malloc(len + 1);
    memcpy(p, str, len);
    p[len] = '\0';
    return p;
}

static inline int str_equal(const char *a, const char *b) {
    return !strcmp(a, b);
}

static inline int str_count(const char *a, const char *b) {
    int count = 0;
    while ((a = strstr(a, b))) {
        a += strlen(b);
        count++;
    }
    return count;
}

char *str_append(char **pp, const char *sep, const char *str) {
    char *res, *p;
    size_t len = 0;
    p = *pp;
    if (p) {
        len = strlen(p) + strlen(sep);
    }
    res = malloc(len + strlen(str) + 1);
    if (p) {
        strcpy(res, p);
        strcat(res, sep);
    }
    strcpy(res + len, str);
    free(p);
    return *pp = res;
}

char *str_strip(char *p)
{
    size_t len = strlen(p);
    while (len > 0 && isspace((unsigned char)p[len - 1]))
        p[--len] = '\0';
    while (isspace((unsigned char)*p))
        p++;
    return p;
}

int has_prefix(const char *str, const char *prefix)
{
    return !strncmp(str, prefix, strlen(prefix));
}

char *skip_prefix(const char *str, const char *prefix)
{
    int i;
    for (i = 0;; i++) {
        if (prefix[i] == '\0') {  /* skip the prefix */
            str += i;
            break;
        }
        if (str[i] != prefix[i])
            break;
    }
    return (char *)str;
}

char *get_basename(const char *filename)
{
    const char *p = strrchr(filename, '/');
    if (!p)
        return NULL;
    return strdup_len(filename, p - filename);
}

char *compose_path(const char *path, const char *name)
{
    int path_len, name_len;
    char *d, *q;

    if (!path || path[0] == '\0' || *name == '/') {
        d = strdup(name);
    } else {
        path_len = strlen(path);
        name_len = strlen(name);
        d = malloc(path_len + 1 + name_len + 1);
        if (d) {
            q = d;
            memcpy(q, path, path_len);
            q += path_len;
            if (path[path_len - 1] != '/')
                *q++ = '/';
            memcpy(q, name, name_len + 1);
        }
    }
    return d;
}

int namelist_cmp(const char *a, const char *b)
{
    /* compare strings in modified lexicographical order */
    for (;;) {
        int ca = (unsigned char)*a++;
        int cb = (unsigned char)*b++;
        if (isdigit(ca) && isdigit(cb)) {
            int na = ca - '0';
            int nb = cb - '0';
            while (isdigit(ca = (unsigned char)*a++))
                na = na * 10 + ca - '0';
            while (isdigit(cb = (unsigned char)*b++))
                nb = nb * 10 + cb - '0';
            if (na < nb)
                return -1;
            if (na > nb)
                return +1;
        }
        if (ca < cb)
            return -1;
        if (ca > cb)
            return +1;
        if (ca == '\0')
            return 0;
    }
}

int namelist_cmp_indirect(const void *a, const void *b)
{
    return namelist_cmp(*(const char **)a, *(const char **)b);
}

void namelist_sort(namelist_t *lp)
{
    int i, count;
    if (lp->count > 1) {
        qsort(lp->array, lp->count, sizeof(*lp->array), namelist_cmp_indirect);
        /* remove duplicates */
        for (count = i = 1; i < lp->count; i++) {
            if (namelist_cmp(lp->array[count - 1], lp->array[i]) == 0) {
                free(lp->array[i]);
            } else {
                lp->array[count++] = lp->array[i];
            }
        }
        lp->count = count;
    }
}

int namelist_find(const namelist_t *lp, const char *name)
{
    int a, b, m, cmp;

    for (a = 0, b = lp->count; a < b;) {
        m = a + (b - a) / 2;
        cmp = namelist_cmp(lp->array[m], name);
        if (cmp < 0)
            a = m + 1;
        else if (cmp > 0)
            b = m;
        else
            return m;
    }
    return -1;
}

void namelist_add(namelist_t *lp, const char *base, const char *name)
{
    char *s;

    s = compose_path(base, name);
    if (!s)
        goto fail;
    if (lp->count == lp->size) {
        size_t newsize = lp->size + (lp->size >> 1) + 4;
        char **a = realloc(lp->array, sizeof(lp->array[0]) * newsize);
        if (!a)
            goto fail;
        lp->array = a;
        lp->size = newsize;
    }
    lp->array[lp->count] = s;
    lp->count++;
    return;
fail:
    fatal(1, "allocation failure\n");
}

void namelist_load(namelist_t *lp, const char *filename)
{
    char buf[1024];
    char *base_name;
    FILE *f;

    f = fopen(filename, "r");
    if (!f) {
        perror_exit(1, filename);
    }
    base_name = get_basename(filename);

    while (fgets(buf, sizeof(buf), f) != NULL) {
        char *p = str_strip(buf);
        if (*p == '#' || *p == ';' || *p == '\0')
            continue;  /* line comment */

        namelist_add(lp, base_name, p);
    }
    free(base_name);
    fclose(f);
}

void namelist_add_from_error_file(namelist_t *lp, const char *file)
{
    const char *p, *p0;
    char *pp;

    for (p = file; (p = strstr(p, ".js:")) != NULL; p++) {
        for (p0 = p; p0 > file && p0[-1] != '\n'; p0--)
            continue;
        pp = strdup_len(p0, p + 3 - p0);
        namelist_add(lp, NULL, pp);
        free(pp);
    }
}

void namelist_free(namelist_t *lp)
{
    while (lp->count > 0) {
        free(lp->array[--lp->count]);
    }
    free(lp->array);
    lp->array = NULL;
    lp->size = 0;
}

static int add_test_file(const char *filename)
{
    namelist_t *lp = &test_list;
    if (js__has_suffix(filename, ".js") && !js__has_suffix(filename, "_FIXTURE.js"))
        namelist_add(lp, NULL, filename);
    return 0;
}

static void find_test_files(const char *path);

static bool ispathsep(int c)
{
    return c == '/' || c == '\\';
}

static void consider_test_file(const char *path, const char *name, int is_dir)
{
    size_t pathlen;
    char s[1024];

    if (str_equal(name, ".") || str_equal(name, ".."))
        return;
    pathlen = strlen(path);
    while (pathlen > 0 && ispathsep(path[pathlen-1]))
        pathlen--;
    snprintf(s, sizeof(s), "%.*s/%s", (int)pathlen, path, name);
#if !defined(_WIN32) && !defined(DT_DIR)
    struct stat st;
    if (is_dir < 0)
        is_dir = !stat(s, &st) && S_ISDIR(st.st_mode);
#endif
    if (is_dir)
        find_test_files(s);
    else
        add_test_file(s);
}

static void find_test_files(const char *path)
{
#ifdef _WIN32
    WIN32_FIND_DATAA d;
    HANDLE h;
    char s[1024];

    snprintf(s, sizeof(s), "%s/*", path);
    h = FindFirstFileA(s, &d);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            consider_test_file(path,
                               d.cFileName,
                               d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        } while (FindNextFileA(h, &d));
        FindClose(h);
    }
#else
    struct dirent *d, **ds = NULL;
    int i, n;

    n = scandir(path, &ds, NULL, alphasort);
    for (i = 0; i < n; i++) {
        d = ds[i];
#ifdef DT_DIR
        consider_test_file(path, d->d_name, d->d_type == DT_DIR);
#else
        consider_test_file(path, d->d_name, -1);
#endif
        free(d);
    }
    free(ds);
#endif
}

/* find js files from the directory tree and sort the list */
static void enumerate_tests(const char *path)
{
    namelist_t *lp = &test_list;
    int start = lp->count;
    find_test_files(path);
    qsort(lp->array + start, lp->count - start, sizeof(*lp->array),
          namelist_cmp_indirect);
}


static JSValue js_print_262(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    ThreadLocalStorage *tls = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    const char *s;
    JSValueConst v;
    int i;

    for (i = 0; i < argc; i++) {
        char *owned = NULL;
        v = argv[i];
        s = JS_DiagCString(ctx, v, &owned);
        if (!s)
            return JS_EXCEPTION;
        if (!strcmp(s, "Test262:AsyncTestComplete")) {
            tls->async_done++;
        } else if (js__strstart(s, "Test262:AsyncTestFailure", NULL)) {
            const char *why = s + sizeof("Test262:AsyncTestFailure:") - 1;
            tls->async_done = 2; /* force an error */
            /* KEEP the message: this is the only place it exists, and the report below is the only place it is
               read. Truncation is fine; losing it entirely is not. */
            snprintf(tls->async_fail, sizeof(tls->async_fail), "%s", why);
        }
        if (outfile) {
            if (i != 0)
                fputc(' ', outfile);
            fputs(s, outfile);
        }
        if (verbose > 1)
            printf("%s%s", &" "[i < 1], s);
        JS_DiagFreeCString(ctx, s, owned);
        if (verbose > 2 && JS_IsError(v)) {
            JSValue stack = JS_GetErrorStackString(ctx, v);
            s = JS_ToCString(ctx, stack);
            JS_FreeValue(ctx, stack);
            if (s) {
                printf("\n%s", s);
                JS_FreeCString(ctx, s);
            }
        }
    }
    if (outfile)
        fputc('\n', outfile);
    if (verbose > 1)
        printf("\n");
    return JS_UNDEFINED;
}

static JSValue js_detachArrayBuffer(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JS_DetachArrayBuffer(ctx, argv[0]);
    return JS_UNDEFINED;
}

static JSValue fork_preempt_eval(JSContext *ctx, const char *buf, size_t buf_len,
                                 const char *filename, int eval_flags);
static JSValue fork_preempt_run_program(JSContext *ctx, const char *buf, size_t buf_len,
                                        const char *filename, int eval_flags);

static JSValue js_evalScript_262(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    const char *str;
    size_t len;
    JSValue ret;
    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    /* THE SAME entry every other evaluation uses. This called plain JS_Eval — the exact fallback eval_buf's own
       comment calls banned ("a fallback to plain JS_Eval tests something that never runs and hides feature
       gaps"), sitting two functions away. The program it evaluates is REAL test code (31 annexB/language files
       run their assertions inside it), so running it off the flow machinery meant its loops preempted in a C
       activation with no flow base and the whole annexB tree aborted. It is a NESTED flow: the outer flow is
       suspended in this C frame while the host pumps the inner one to its completion, which is the host
       boundary a synchronous eval API requires, not a second scheduler inside the engine. */
    ret = fork_preempt_eval(ctx, str, len, "<evalScript>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeCString(ctx, str);
    return ret;
}

static long cpu_count(void)
{
#ifdef _WIN32
    DWORD_PTR procmask, sysmask;
    long count;
    int i;

    count = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &procmask, &sysmask))
        for (i = 0; i < 8 * sizeof(procmask); i++)
            count += 1 & (procmask >> i);
    return count;
#else
    return sysconf(_SC_NPROCESSORS_ONLN);
#endif
}

static void init_thread_local_storage(ThreadLocalStorage *p)
{
    js_mutex_init(&p->agent_mutex);
    js_cond_init(&p->agent_cond);
    init_list_head(&p->agent_list);
    js_mutex_init(&p->report_mutex);
    init_list_head(&p->report_list);
    p->async_done = 0;
    p->async_fail[0] = '\0';
}

typedef struct {
    struct list_head link;
    js_thread_t tid;
    char *script;
    JSValue broadcast_func;
    bool broadcast_pending;
    JSValue broadcast_sab; /* in the main context */
    uint8_t *broadcast_sab_buf;
    size_t broadcast_sab_size;
    int32_t broadcast_val;
    ThreadLocalStorage *tls;
} Test262Agent;

typedef struct {
    struct list_head link;
    char *str;
} AgentReport;

static JSValue add_helpers1(JSContext *ctx);
static void add_helpers(JSContext *ctx);

static void agent_start(void *arg)
{
    ThreadLocalStorage *tls;
    Test262Agent *agent;
    JSRuntime *rt;
    JSContext *ctx;
    JSValue ret_val;
    int ret;

    agent = arg;
    tls = agent->tls; // shares thread-local storage with parent thread
    rt = JS_NewRuntime();
    if (rt == NULL) {
        fatal(1, "JS_NewRuntime failure");
    }
    JS_SetRuntimeOpaque(rt, tls);
    ctx = JS_NewContext(rt);
    if (ctx == NULL) {
        JS_FreeRuntime(rt);
        fatal(1, "JS_NewContext failure");
    }
    JS_SetContextOpaque(ctx, agent);
    JS_SetRuntimeInfo(rt, "agent");
    JS_SetCanBlock(rt, true);

    add_helpers(ctx);
    /* THE AGENT'S SCRIPT IS TEST CODE and runs on the flow machinery like every other program. It ran through
       plain JS_Eval, so an agent body executed in a C activation with no flow base — a loop in it drove to
       completion, and its top level reached JS_EvalFunctionInternal's C call into the interpreter, which is the
       last thing holding JS_CallFree on the interpreter's own cycle.
       The hooks are NOT armed here: agent_start is only ever reached from $262.agent.start inside a running
       test, so the main thread has already armed them, and JSFlowControlHooks is one global. */
    ret_val = fork_preempt_run_program(ctx, agent->script, strlen(agent->script),
                                       "<evalScript>", JS_EVAL_TYPE_GLOBAL);
    free(agent->script);
    agent->script = NULL;
    if (JS_IsException(ret_val))
        js_std_dump_error(ctx);
    JS_FreeValue(ctx, ret_val);

    for(;;) {
        JSContext *ctx1;
        /* THE PUMP, and this is the THIRD one — the two that drive the main thread already drain the parked
           slot before any job and say why. This one did not, and the line above is why that matters: the
           agent's script runs on the flow machinery, so a loop in it PARKS. A parked agent flow with nothing
           to resume it either sits while queued jobs run ahead of it — reordering observable microtasks, which
           the resume razor forbids — or is never resumed at all and the agent silently stops mid-script.
           Three pumps with two of them transparent is the same class of gap as one call spelling behaving
           differently from another: the property has to hold wherever a flow can park, not where we happened
           to look.
           AND THE COMPLETION IS TAKEN, exactly as the job below it is: a parked continuation is the LATE half
           of a job, so an uncaught throw out of one is reported the way an uncaught throw out of a job is.
           Leaving it in the per-runtime slot is what let it reach the next evaluation. */
        for (;;) {
            JSValue pcv;
            if (!JS_ResumeParkedFlow(JS_GetRuntime(ctx), &pcv))
                break;
            if (JS_IsException(pcv))
                js_std_dump_error(ctx);
            JS_FreeValue(ctx, pcv);
        }
        host_pump_atomics_async(JS_GetRuntime(ctx), false);
        ret = JS_ExecutePendingJob(JS_GetRuntime(ctx), &ctx1);
        if (ret < 0) {
            js_std_dump_error(ctx);
            break;
        } else if (ret == 0) {
            if (host_pump_atomics_async(JS_GetRuntime(ctx), true))
                continue;
            if (JS_IsUndefined(agent->broadcast_func)) {
                break;
            } else {
                JSValue args[2];

                js_mutex_lock(&tls->agent_mutex);
                while (!agent->broadcast_pending) {
                    js_cond_wait(&tls->agent_cond, &tls->agent_mutex);
                }

                agent->broadcast_pending = false;
                js_cond_signal(&tls->agent_cond);

                js_mutex_unlock(&tls->agent_mutex);

                args[0] = JS_NewArrayBuffer(ctx, agent->broadcast_sab_buf,
                                            agent->broadcast_sab_size,
                                            NULL, NULL, true);
                args[1] = JS_NewInt32(ctx, agent->broadcast_val);
                ret_val = JS_Call(ctx, agent->broadcast_func, JS_UNDEFINED,
                                  2, (JSValueConst *)args);
                JS_FreeValue(ctx, args[0]);
                JS_FreeValue(ctx, args[1]);
                if (JS_IsException(ret_val))
                    js_std_dump_error(ctx);
                JS_FreeValue(ctx, ret_val);
                JS_FreeValue(ctx, agent->broadcast_func);
                agent->broadcast_func = JS_UNDEFINED;
            }
        }
    }
    JS_FreeValue(ctx, agent->broadcast_func);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static JSValue js_agent_start(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    ThreadLocalStorage *tls = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    const char *script;
    Test262Agent *agent;

    if (JS_GetContextOpaque(ctx) != NULL)
        return JS_ThrowTypeError(ctx, "cannot be called inside an agent");

    script = JS_ToCString(ctx, argv[0]);
    if (!script)
        return JS_EXCEPTION;
    agent = malloc(sizeof(*agent));
    memset(agent, 0, sizeof(*agent));
    agent->broadcast_func = JS_UNDEFINED;
    agent->broadcast_sab = JS_UNDEFINED;
    agent->script = strdup(script);
    agent->tls = tls;
    JS_FreeCString(ctx, script);
    list_add_tail(&agent->link, &tls->agent_list);
    js_thread_create(&agent->tid, agent_start, agent, /*flags*/0);
    return JS_UNDEFINED;
}

static void js_agent_free(JSContext *ctx)
{
    ThreadLocalStorage *tls = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    struct list_head *el, *el1;
    Test262Agent *agent;

    list_for_each_safe(el, el1, &tls->agent_list) {
        agent = list_entry(el, Test262Agent, link);
        js_thread_join(agent->tid);
        JS_FreeValue(ctx, agent->broadcast_sab);
        list_del(&agent->link);
        free(agent);
    }
}

static JSValue js_agent_leaving(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    Test262Agent *agent = JS_GetContextOpaque(ctx);
    if (!agent)
        return JS_ThrowTypeError(ctx, "must be called inside an agent");
    /* nothing to do */
    return JS_UNDEFINED;
}

static bool is_broadcast_pending(ThreadLocalStorage *tls)
{
    struct list_head *el;
    Test262Agent *agent;
    list_for_each(el, &tls->agent_list) {
        agent = list_entry(el, Test262Agent, link);
        if (agent->broadcast_pending)
            return true;
    }
    return false;
}

static JSValue js_agent_broadcast(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    ThreadLocalStorage *tls = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    JSValueConst sab = argv[0];
    struct list_head *el;
    Test262Agent *agent;
    uint8_t *buf;
    size_t buf_size;
    int32_t val;

    if (JS_GetContextOpaque(ctx) != NULL)
        return JS_ThrowTypeError(ctx, "cannot be called inside an agent");

    buf = JS_GetArrayBuffer(ctx, &buf_size, sab);
    if (!buf)
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &val, argv[1]))
        return JS_EXCEPTION;

    /* broadcast the values and wait until all agents have started
       calling their callbacks */
    js_mutex_lock(&tls->agent_mutex);
    list_for_each(el, &tls->agent_list) {
        agent = list_entry(el, Test262Agent, link);
        agent->broadcast_pending = true;
        /* the shared array buffer is used by the thread, so increment
           its refcount */
        agent->broadcast_sab = JS_DupValue(ctx, sab);
        agent->broadcast_sab_buf = buf;
        agent->broadcast_sab_size = buf_size;
        agent->broadcast_val = val;
    }
    js_cond_broadcast(&tls->agent_cond);

    while (is_broadcast_pending(tls)) {
        js_cond_wait(&tls->agent_cond, &tls->agent_mutex);
    }
    js_mutex_unlock(&tls->agent_mutex);
    return JS_UNDEFINED;
}

static JSValue js_agent_receiveBroadcast(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    Test262Agent *agent = JS_GetContextOpaque(ctx);
    if (!agent)
        return JS_ThrowTypeError(ctx, "must be called inside an agent");
    if (!JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "expecting function");
    JS_FreeValue(ctx, agent->broadcast_func);
    agent->broadcast_func = JS_DupValue(ctx, argv[0]);
    return JS_UNDEFINED;
}

static JSValue js_agent_sleep(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    uint32_t duration;
    if (JS_ToUint32(ctx, &duration, argv[0]))
        return JS_EXCEPTION;
#ifdef _WIN32
    Sleep(duration);
#else
    usleep(duration * 1000);
#endif
    return JS_UNDEFINED;
}

static int64_t get_clock_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (ts.tv_nsec / 1000000);
#endif
}

static JSValue js_agent_monotonicNow(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    return JS_NewInt64(ctx, get_clock_ms());
}

static JSValue js_agent_getReport(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    ThreadLocalStorage *tls = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    AgentReport *rep;
    JSValue ret;

    js_mutex_lock(&tls->report_mutex);
    if (list_empty(&tls->report_list)) {
        rep = NULL;
    } else {
        rep = list_entry(tls->report_list.next, AgentReport, link);
        list_del(&rep->link);
    }
    js_mutex_unlock(&tls->report_mutex);
    if (rep) {
        ret = JS_NewString(ctx, rep->str);
        free(rep->str);
        free(rep);
    } else {
        ret = JS_NULL;
    }
    return ret;
}

static JSValue js_agent_report(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    ThreadLocalStorage *tls = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    const char *str;
    AgentReport *rep;

    str = JS_ToCString(ctx, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    rep = malloc(sizeof(*rep));
    rep->str = strdup(str);
    JS_FreeCString(ctx, str);

    js_mutex_lock(&tls->report_mutex);
    list_add_tail(&rep->link, &tls->report_list);
    js_mutex_unlock(&tls->report_mutex);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_agent_funcs[] = {
    /* only in main */
    JS_CFUNC_DEF("start", 1, js_agent_start ),
    JS_CFUNC_DEF("getReport", 0, js_agent_getReport ),
    JS_CFUNC_DEF("broadcast", 2, js_agent_broadcast ),
    /* only in agent */
    JS_CFUNC_DEF("report", 1, js_agent_report ),
    JS_CFUNC_DEF("leaving", 0, js_agent_leaving ),
    JS_CFUNC_DEF("receiveBroadcast", 1, js_agent_receiveBroadcast ),
    /* in both */
    JS_CFUNC_DEF("sleep", 1, js_agent_sleep ),
    JS_CFUNC_DEF("monotonicNow", 0, js_agent_monotonicNow ),
};

static JSValue js_new_agent(JSContext *ctx)
{
    JSValue agent;
    agent = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, agent, js_agent_funcs,
                               countof(js_agent_funcs));
    return agent;
}

static JSValue js_createRealm(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSContext *ctx1;
    JSValue ret;

    ctx1 = JS_NewContext(JS_GetRuntime(ctx));
    if (!ctx1)
        return JS_ThrowOutOfMemory(ctx);
    ret = add_helpers1(ctx1);
    /* ctx1 has a refcount so it stays alive */
    JS_FreeContext(ctx1);
    return ret;
}

static JSValue js_IsHTMLDDA(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    return JS_NULL;
}

static JSValue js_gc(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv)
{
    JS_RunGC(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}

static JSValue add_helpers1(JSContext *ctx)
{
    JSValue global_obj;
    JSValue obj262, is_html_dda;

    global_obj = JS_GetGlobalObject(ctx);

    JS_SetPropertyStr(ctx, global_obj, "print",
                      JS_NewCFunction(ctx, js_print_262, "print", 1));
    JS_SetPropertyStr(ctx, global_obj, "gc",
                      JS_NewCFunction(ctx, js_gc, "gc", 0));

    is_html_dda = JS_NewCFunction(ctx, js_IsHTMLDDA, "IsHTMLDDA", 0);
    JS_SetIsHTMLDDA(ctx, is_html_dda);
    /* `gc` is INTERPRETING.md's host hook — "wraps the host's garbage collection invocation mechanism, if such
       a capability exists", and the tests that need it are the ones whose semantics ARE collection: WeakRef,
       FinalizationRegistry, a WeakMap entry going away. It existed as a bare global and not on $262, so the
       whole host-gc-required feature was skipped even though the capability was already there. */
#define N 8
    static const char *props[N] = {
        "detachArrayBuffer", "evalScript", "codePointRange",
        "agent", "global", "createRealm", "IsHTMLDDA", "gc",
    };
    JSValue values[N] = {
        JS_NewCFunction(ctx, js_detachArrayBuffer, "detachArrayBuffer", 1),
        JS_NewCFunction(ctx, js_evalScript_262, "evalScript", 1),
        JS_NewCFunction(ctx, js_string_codePointRange, "codePointRange", 2),
        js_new_agent(ctx),
        JS_DupValue(ctx, global_obj),
        JS_NewCFunction(ctx, js_createRealm, "createRealm", 0),
        is_html_dda,
        JS_NewCFunction(ctx, js_gc, "gc", 0),
    };
    /* $262 special object used by the tests */
    obj262 = JS_NewObjectFromStr(ctx, N, props, values);
    JS_SetPropertyStr(ctx, global_obj, "$262", JS_DupValue(ctx, obj262));
#undef N

    JS_FreeValue(ctx, global_obj);
    return obj262;
}

static void add_helpers(JSContext *ctx)
{
    JS_FreeValue(ctx, add_helpers1(ctx));
}

static char *load_file(const char *filename, size_t *lenp)
{
    char *buf;
    size_t buf_len;
    buf = (char *)js_load_file(NULL, &buf_len, filename);
    if (!buf)
        perror_exit(1, filename);
    if (lenp)
        *lenp = buf_len;
    return buf;
}

static JSModuleDef *js_module_loader_test(JSContext *ctx,
                                          const char *module_name, void *opaque,
                                          JSValueConst attributes)
{
    char *filename, *slash, path[1024];

    // interpret import("bar.js") from path/to/foo.js as
    // import("path/to/bar.js") but leave import("./bar.js") untouched
    filename = opaque;
    if (!strchr(module_name, '/')) {
        slash = strrchr(filename, '/');
        if (slash) {
            snprintf(path, sizeof(path), "%.*s/%s",
                     (int) (slash - filename), filename, module_name);
            module_name = path;
        }
    }
    return js_module_load(ctx, module_name, opaque, attributes, js_load_file);
}

int is_line_sep(char c)
{
    return (c == '\0' || c == '\n' || c == '\r');
}

char *find_line(const char *str, const char *line)
{
    if (str) {
        const char *p;
        int len = strlen(line);
        for (p = str; (p = strstr(p, line)) != NULL; p += len + 1) {
            if ((p == str || is_line_sep(p[-1])) && is_line_sep(p[len]))
                return (char *)p;
        }
    }
    return NULL;
}

int is_word_sep(char c)
{
    return (c == '\0' || isspace((unsigned char)c) || c == ',');
}

char *find_word(const char *str, const char *word)
{
    const char *p;
    int len = strlen(word);
    if (str && len) {
        for (p = str; (p = strstr(p, word)) != NULL; p += len) {
            if ((p == str || is_word_sep(p[-1])) && is_word_sep(p[len]))
                return (char *)p;
        }
    }
    return NULL;
}

/* handle exclude directories */
void update_exclude_dirs(void)
{
    namelist_t *lp = &test_list;
    namelist_t *ep = &exclude_list;
    namelist_t *dp = &exclude_dir_list;
    char *name, *path;
    int i, j, count;
    size_t include, exclude;

    /* Split the PREFIX-matched entries out of exclude_list, which is otherwise matched by equality. A directory
       (trailing '/') is one; so is every '!' RE-INCLUDE, whatever it names — the marker is what routes it here,
       not the shape of the path. A '!' naming a single FILE is the point: without it the config can only re-admit
       a whole tree, so a spec-true file sitting inside an excluded suite has no spelling at all and stays out of
       the gate with nothing to say so — the excluded-test failure, one granularity down. */
    for (count = i = 0; i < ep->count; i++) {
        name = ep->array[i];
        if (js__has_suffix(name, "/") || *name == '!') {
            namelist_add(dp, NULL, name);
            free(name);
        } else {
            ep->array[count++] = name;
        }
    }
    ep->count = count;

    namelist_sort(dp);

    /* filter out excluded directories */
    for (count = i = 0; i < lp->count; i++) {
        name = lp->array[i];
        include = exclude = 0;
        for (j = 0; j < dp->count; j++) {
            path = dp->array[j];
            if (*path == '!') {
                /* A re-include is matched the way the thing it names is matched on the exclude side: a
                   directory by prefix, a file by equality. Prefix-matching a file name would also re-admit
                   every longer name it happens to start with. */
                const char *incl = &path[1];
                if (js__has_suffix(incl, "/") ? has_prefix(name, incl) : str_equal(name, incl))
                    include = strlen(incl);
            } else if (has_prefix(name, path)) {
                exclude = strlen(path);
            }
        }
        // most specific include/exclude wins
        if (exclude > include) {
            test_excluded++;
            free(name);
        } else {
            lp->array[count++] = name;
        }
    }
    lp->count = count;
}

void load_config(const char *filename, const char *ignore)
{
    char buf[1024];
    FILE *f;
    char *base_name;
    enum {
        SECTION_NONE = 0,
        SECTION_CONFIG,
        SECTION_EXCLUDE,
        SECTION_FEATURES,
        SECTION_TESTS,
    } section = SECTION_NONE;
    int lineno = 0;

    f = fopen(filename, "r");
    if (!f) {
        perror_exit(1, filename);
    }
    base_name = get_basename(filename);

    while (fgets(buf, sizeof(buf), f) != NULL) {
        char *p, *q;
        lineno++;
        p = str_strip(buf);
        if (*p == '#' || *p == ';' || *p == '\0')
            continue;  /* line comment */
        {
            /* A TRAILING comment is a comment too. Only a line STARTING with '#' was one, so
               `Atomics=!tcc # atomics are broken in recent versions of tcc` set the value to
               "!tcc # atomics are broken in recent versions of tcc", which matches neither "yes" nor "!tcc" —
               so the feature was SKIPPED for every compiler and 384 Atomics tests silently never ran. A comment
               must never change a setting's value; that is the whole difference between a config file and a
               trap. Whitespace-then-'#' is the delimiter, so a '#' inside a path is still a path. */
            char *c = p;
            while ((c = strchr(c, '#')) != NULL) {
                if (c > p && (c[-1] == ' ' || c[-1] == '\t')) { *c = '\0'; break; }
                c++;
            }
            p = str_strip(p);
            if (*p == '\0')
                continue;
        }

        if (*p == "[]"[0]) {
            /* new section */
            p++;
            p[strcspn(p, "]")] = '\0';
            if (str_equal(p, "config"))
                section = SECTION_CONFIG;
            else if (str_equal(p, "exclude"))
                section = SECTION_EXCLUDE;
            else if (str_equal(p, "features"))
                section = SECTION_FEATURES;
            else if (str_equal(p, "tests"))
                section = SECTION_TESTS;
            else
                section = SECTION_NONE;
            continue;
        }
        q = strchr(p, '=');
        if (q) {
            /* setting: name=value */
            *q++ = '\0';
            q = str_strip(q);
        }
        switch (section) {
        case SECTION_CONFIG:
            if (!q) {
                printf("%s:%d: syntax error\n", filename, lineno);
                continue;
            }
            if (strstr(ignore, p)) {
                printf("%s:%d: ignoring %s=%s\n", filename, lineno, p, q);
                continue;
            }
            if (str_equal(p, "local")) {
                local = str_equal(q, "yes");
                continue;
            }
            if (str_equal(p, "testdir")) {
                char *testdir = compose_path(base_name, q);
                enumerate_tests(testdir);
                free(testdir);
                continue;
            }
            if (str_equal(p, "harnessdir")) {
                harness_dir = compose_path(base_name, q);
                continue;
            }
            if (str_equal(p, "harnessexclude")) {
                str_append(&harness_exclude, " ", q);
                continue;
            }
            if (str_equal(p, "features")) {
                str_append(&harness_features, " ", q);
                continue;
            }
            if (str_equal(p, "skip-features")) {
                str_append(&harness_skip_features, " ", q);
                continue;
            }
            if (str_equal(p, "mode")) {
                if (str_equal(q, "default") || str_equal(q, "default-nostrict"))
                    test_mode = TEST_DEFAULT_NOSTRICT;
                else if (str_equal(q, "default-strict"))
                    test_mode = TEST_DEFAULT_STRICT;
                else if (str_equal(q, "nostrict"))
                    test_mode = TEST_NOSTRICT;
                else if (str_equal(q, "strict"))
                    test_mode = TEST_STRICT;
                else if (str_equal(q, "all") || str_equal(q, "both"))
                    test_mode = TEST_ALL;
                else
                    fatal(2, "unknown test mode: %s", q);
                continue;
            }
            if (str_equal(p, "strict")) {
                if (str_equal(q, "skip") || str_equal(q, "no"))
                    test_mode = TEST_NOSTRICT;
                continue;
            }
            if (str_equal(p, "nostrict")) {
                if (str_equal(q, "skip") || str_equal(q, "no"))
                    test_mode = TEST_STRICT;
                continue;
            }
            if (str_equal(p, "async")) {
                skip_async = !str_equal(q, "yes");
                continue;
            }
            if (str_equal(p, "module")) {
                skip_module = !str_equal(q, "yes");
                continue;
            }
            if (str_equal(p, "verbose")) {
                int count = str_count(q, "yes");
                verbose = max_int(verbose, count);
                continue;
            }
            if (str_equal(p, "errorfile")) {
                error_filename = compose_path(base_name, q);
                continue;
            }
            if (str_equal(p, "excludefile")) {
                char *path = compose_path(base_name, q);
                namelist_load(&exclude_list, path);
                free(path);
                continue;
            }
        case SECTION_EXCLUDE:
            if (*p == '!') {
                /* The '!' is a marker on the ENTRY, not the first character of the path, so the config file's
                   own directory joins to what FOLLOWS it. Joining the marked string instead buries the '!'
                   mid-path and the re-include silently matches nothing. */
                char *joined = compose_path(base_name, p + 1);
                char *marked = joined ? malloc(strlen(joined) + 2) : NULL;
                if (!marked)
                    fatal(1, "allocation failure\n");
                marked[0] = '!';
                strcpy(marked + 1, joined);
                namelist_add(&exclude_list, NULL, marked);
                free(marked);
                free(joined);
            } else {
                namelist_add(&exclude_list, base_name, p);
            }
            break;
        case SECTION_FEATURES:
            if (!q || str_equal(q, "yes") || (!CC_IS_TCC && str_equal(q, "!tcc")))
                str_append(&harness_features, " ", p);
            else
                str_append(&harness_skip_features, " ", p);
            break;
        case SECTION_TESTS:
            namelist_add(&test_list, base_name, p);
            break;
        default:
            /* ignore settings in other sections */
            break;
        }
    }
    fclose(f);
    free(base_name);
}

char *find_error(const char *filename, int *pline, int is_strict)
{
    if (error_file) {
        size_t len = strlen(filename);
        const char *p, *q, *r;
        int line;

        for (p = error_file; (p = strstr(p, filename)) != NULL; p += len) {
            if ((p == error_file || p[-1] == '\n' || p[-1] == '(') && p[len] == ':') {
                q = p + len;
                line = 1;
                if (*q == ':') {
                    line = strtol(q + 1, (char**)&q, 10);
                    if (*q == ':')
                        q++;
                }
                while (*q == ' ') {
                    q++;
                }
                /* check strict mode indicator */
                if (!js__strstart(q, "strict mode: ", &q) != !is_strict)
                    continue;
                r = q = skip_prefix(q, "unexpected error: ");
                r += strcspn(r, "\n");
                while (r[0] == '\n' && r[1] && strncmp(r + 1, filename, 8)) {
                    r++;
                    r += strcspn(r, "\n");
                }
                if (pline)
                    *pline = line;
                return strdup_len(q, r - q);
            }
        }
    }
    return NULL;
}

int skip_comments(const char *str, int line, int *pline)
{
    const char *p;
    int c;

    p = str;
    while ((c = (unsigned char)*p++) != '\0') {
        if (isspace(c)) {
            if (c == '\n')
                line++;
            continue;
        }
        if (c == '/' && *p == '/') {
            while (*++p && *p != '\n')
                continue;
            continue;
        }
        if (c == '/' && *p == '*') {
            for (p += 1; *p; p++) {
                if (*p == '\n') {
                    line++;
                    continue;
                }
                if (*p == '*' && p[1] == '/') {
                    p += 2;
                    break;
                }
            }
            continue;
        }
        break;
    }
    if (pline)
        *pline = line;

    return p - str;
}

int longest_match(const char *str, const char *find, int pos, int *ppos, int line, int *pline)
{
    int len, maxlen;

    maxlen = 0;

    if (*find) {
        const char *p;
        for (p = str + pos; *p; p++) {
            if (*p == *find) {
                for (len = 1; p[len] && p[len] == find[len]; len++)
                    continue;
                if (len > maxlen) {
                    maxlen = len;
                    if (ppos)
                        *ppos = p - str;
                    if (pline)
                        *pline = line;
                    if (!find[len])
                        break;
                }
            }
            if (*p == '\n')
                line++;
        }
    }
    return maxlen;
}

/* FORK-FEATURE TEST262 HARNESS (env FORK_PREEMPT=1): run each SCRIPT test through the time-travel flow machinery
   under FORCED preemption — suspend + rebuild the heap-frame chain at EVERY loop back-edge (quantum=1). test262's
   own expected result is the oracle: if the ECMAScript outcome diverges from a plain JS_Eval, deep-preempt/resume
   corrupted state — a fork-feature bug conformance testing can never surface. This is the point of test262 here:
   NOT "does QuickJS run JS" (known), but "does our time-travel preserve JS across thousands of suspend/resumes".
   Modules + async ($DONE) fall back to JS_Eval for now (JS_FlowNew is a global program; that surface is next). */
/* EVERY back-edge and every fork, and calls SAMPLED. The interpreter now offers a suspend point at every opcode
   boundary and asks this hook only where something RAISED a yield request, so `kind` is the SOURCE that raised
   it — which is exactly the distinction this policy has always needed.
   Forcing every call too is not a stronger test that was declined — it is one the corpus cannot finish: a call
   is reached orders of magnitude more often than a loop back-edge, and parking + rebuilding the heap-frame chain
   at each one timed built-ins/Array out on its own. The stride keeps call-parking heavily exercised (the corpus
   makes hundreds of millions of calls, so 1-in-64 is still millions of forced call-parks) while leaving
   back-edge coverage exactly as strong. It samples WHEN to force the seam; it is not a fallback to a second
   implementation, because a call the sample skips parks by the same one mechanism the moment the sample selects
   it. JS_PREEMPT_HOST is forced like a back-edge: nothing in this harness raises one, so it costs nothing here,
   and a policy that answered it differently from the source it cannot distinguish would be guessing. */
static unsigned fork_call_stride = 0;
static int fork_preempt_always(int kind) {
    if (kind == JS_PREEMPT_CALL) return (++fork_call_stride & 63) == 0;
    return 1;
}
/* JSFlowControlHooks is {branch, fork, preempt} — THREE fields. A fourth initializer silently left `preempt` NULL,
   so the forced back-edge preemption never armed and the engagement metric read a vacuous 0/0. Designated
   initializers so the field can never drift again. */
static const JSFlowControlHooks fork_hooks_ON  = { .branch = NULL, .fork = NULL, .preempt = fork_preempt_always };
static const JSFlowControlHooks fork_hooks_OFF = { .branch = NULL, .fork = NULL, .preempt = NULL };

/* The feature must stay ON for the WHOLE test, including the JOB DRAIN. A promise reaction, a post-await
   continuation, a .then callback and an async-generator resume are all test code that the project runs under
   forced execution — switching the hooks off when the base flow returns would run them with the feature
   DISABLED, and the engagement metric could not see it (no preempt REQUESTED there means no gap counted). That
   is a fake-green the metric exists to prevent, so the hooks are released only after the jobs are drained
   (fork_preempt_hooks_off, called at the end of eval_buf). */
static void fork_preempt_hooks_off(void) {
    JS_SetFlowControlHooks(&fork_hooks_OFF);
}

/* Run a global PROGRAM as a flow: park and rebuild the whole frame chain at every back-edge. It does NOT touch
   the flow hooks, because the ARMING belongs to whoever owns the run — an agent thread inherits hooks the main
   thread already armed, and JSFlowControlHooks is one global that a second thread has no business writing. */
static JSValue fork_preempt_run_program(JSContext *ctx, const char *buf, size_t buf_len,
                                        const char *filename, int eval_flags) {
    JSValue *flow = JS_FlowNew(ctx, buf, buf_len, filename, eval_flags);   /* STRICTNESS and the real script NAME */
    JSValue res = JS_UNDEFINED;
    if (!flow) return JS_EXCEPTION;   /* compile error: exception already pending */
    while (JS_FlowResume(ctx, flow, &res)) { }   /* park + rebuild the whole frame chain on every back-edge */
    JS_FlowFree(ctx, flow);
    return res;   /* the program's COMPLETION VALUE — what an eval API evaluates to (JS_EXCEPTION on a throw) */
}

static JSValue fork_preempt_eval(JSContext *ctx, const char *buf, size_t buf_len,
                                 const char *filename, int eval_flags) {
    if ((eval_flags & JS_EVAL_TYPE_MASK) == JS_EVAL_TYPE_MODULE) {
        /* A MODULE is a graph, not a program to wrap: link it and evaluate it, and its bodies — which are async
           functions on the same seam a flow's base runs on — park and resume through the pump below. The hooks
           arm BEFORE the call rather than after, because here the evaluation IS the run; arming afterwards would
           execute every module body with the feature off and the engagement metric could not see it. */
        JS_SetFlowControlHooks(&fork_hooks_ON);
        return JS_FlowEvalModule(ctx, buf, buf_len, filename, eval_flags);
    }
    JS_SetFlowControlHooks(&fork_hooks_ON);
    return fork_preempt_run_program(ctx, buf, buf_len, filename, eval_flags);
}

static int eval_buf(JSContext *ctx, const char *buf, size_t buf_len,
                    const char *filename, int is_test, int is_negative,
                    const char *error_type, int eval_flags, int is_async,
                    int *msec)
{
    ThreadLocalStorage *tls = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    JSValue res_val, exception_val;
    int ret, error_line, pos, pos_line;
    bool is_error, has_error_line, ret_promise;
    const char *error_name;
    int start, duration;

    pos = skip_comments(buf, 1, &pos_line);
    error_line = pos_line;
    has_error_line = false;
    exception_val = JS_UNDEFINED;
    error_name = NULL;

    /* a module evaluation returns a promise */
    ret_promise = ((eval_flags & JS_EVAL_TYPE_MODULE) != 0);
    tls->async_done = 0; /* counter of "Test262:AsyncTestComplete" messages */
    tls->async_fail[0] = '\0';   /* per TEST, beside the counter it explains — a stale one would mislabel the next */

    start = get_clock_ms();
    /* FORK-FEATURE run: EVERY test goes through the time-travel machinery — NO fallback. No async carve-out, NO
       module carve-out: the project is never run without its features, so a fallback to plain JS_Eval tests
       something that never runs and hides feature gaps. A construct the feature does not yet support must FAIL
       LOUD here (an honest gap to build at the root), never be papered over by JS_Eval.
       THE ENV SWITCH THAT USED TO GUARD THIS IS GONE. FORK_PREEMPT was read once and branched on in four
       places, and the OFF arm was exactly the banned fallback — dead in practice (engine/test262.mjs always
       sets it) and load-bearing in the worst way: the agent thread's own script eval was written against the
       OFF shape and stayed on plain JS_Eval, which is how a whole thread of test code ran off the flow
       machinery unnoticed. A mode that is never selected is not a safety net, it is a place for the second
       implementation to hide. */
    res_val = fork_preempt_eval(ctx, buf, buf_len, filename, eval_flags);

    if ((is_async || ret_promise) && !JS_IsException(res_val)) {
        JSValue promise = JS_UNDEFINED;
        if (ret_promise) {
            promise = res_val;
        } else {
            JS_FreeValue(ctx, res_val);
        }
        for(;;) {
            JSContext *ctx1;
            /* THE PUMP: a flow parked inside job-driven code resumes BEFORE any queued job — a forced preempt
               must not reorder observable microtasks. With one flow that is immediately, which is exactly what
               the scheduler decides when nothing else is runnable.
               ITS COMPLETION IS THE TEST'S, taken here and reported exactly as the job below reports one: a
               parked continuation is the LATE half of a job, so an uncaught throw out of one is a failed test
               and not a value to leave in the runtime for the promise check below to trip over. */
            /* THE DRAIN IS DRAINED, EVEN AFTER A THROW, and this loop used to stop at the first one. That was
               indistinguishable from correct while at most ONE continuation could be parked — stopping after
               the only one IS draining — and it stopped being so when the pump became a queue: a step reaches
               as many bases as it reaches, so leaving on the first abrupt completion abandons the rest, which
               is a dropped work item and, one line later, JS_FreeRuntime's own "torn down with a flow still
               parked" abort. The FIRST throw is the test's completion and is held aside while the others run;
               a later one is a second failure of an already-failed test and there is one report to make.
               IT IS TAKEN OUT OF THE RUNTIME AT ONCE, not left standing: the exception slot is per-RUNTIME, so
               a throw still live when the next continuation resumes is a completion delivered to an evaluation
               that did not produce it — which is exactly what that resume's own assert refuses. */
            {
                JSValue parked_exc = JS_UNINITIALIZED;
                for (;;) {
                    JSValue pcv;
                    if (!JS_ResumeParkedFlow(JS_GetRuntime(ctx), &pcv))
                        break;
                    if (JS_IsException(pcv)) {
                        JSValue e = JS_GetException(ctx);
                        if (JS_IsUninitialized(parked_exc)) parked_exc = e;
                        else JS_FreeValue(ctx, e);
                    }
                    JS_FreeValue(ctx, pcv);
                }
                if (!JS_IsUninitialized(parked_exc)) {
                    res_val = JS_Throw(ctx, parked_exc);
                    break;
                }
            }
            host_pump_atomics_async(JS_GetRuntime(ctx), false);
            ret = JS_ExecutePendingJob(JS_GetRuntime(ctx), &ctx1);
            if (ret < 0) {
                res_val = JS_EXCEPTION;
                break;
            } else if (ret == 0) {
                if (host_pump_atomics_async(JS_GetRuntime(ctx), true))
                    continue;
                if (is_async) {
                    /* test if the test called $DONE() once */
                    if (tls->async_done != 1) {
                        if (tls->async_fail[0])
                            res_val = JS_ThrowTypeError(ctx, "async test FAILED: %s", tls->async_fail);
                        else
                            res_val = JS_ThrowTypeError(ctx, "$DONE() not called");
                    } else {
                        res_val = JS_UNDEFINED;
                    }
                } else {
                    /* check that the returned promise is fulfilled */
                    JSPromiseStateEnum state = JS_PromiseState(ctx, promise);
                    if (state == JS_PROMISE_FULFILLED)
                        res_val = JS_UNDEFINED;
                    else if (state == JS_PROMISE_REJECTED)
                        res_val = JS_Throw(ctx, JS_PromiseResult(ctx, promise));
                    else
                        res_val = JS_ThrowTypeError(ctx, "promise is pending");
                }
                break;
            }
        }
        JS_FreeValue(ctx, promise);
    }
    fork_preempt_hooks_off();   /* jobs are drained: the whole test ran WITH the feature, reactions included */

    duration = get_clock_ms() - start;
    *msec += duration;

    if (JS_IsException(res_val)) {
        exception_val = JS_GetException(ctx);
        is_error = JS_IsError(exception_val);
        js_print_262(ctx, JS_NULL, 1, (JSValueConst *)&exception_val);
        if (is_error) {
            JSValue name, stack;
            const char *stack_str;

            name = JS_GetPropertyStr(ctx, exception_val, "name");
            error_name = JS_ToCString(ctx, name);
            stack = JS_GetErrorStackString(ctx, exception_val);
            if (!JS_IsUndefined(stack)) {
                stack_str = JS_ToCString(ctx, stack);
                if (stack_str) {
                    const char *p;
                    int len;

                    len = strlen(filename);
                    p = strstr(stack_str, filename);
                    if (p != NULL && p[len] == ':') {
                        error_line = atoi(p + len + 1);
                        has_error_line = true;
                    }
                    JS_FreeCString(ctx, stack_str);
                }
            }
            JS_FreeValue(ctx, stack);
            JS_FreeValue(ctx, name);
        }
        if (is_negative) {
            ret = 0;
            if (error_type) {
                char *cls_owned = NULL;
                const char *cls;

                /* the expected class IS the error's name. Taking it from a ToString of the whole exception and
                   then cutting at the first ':' meant invoking the page's `toString` from a C entry — the state
                   the engine's step-builtin backstop names, and a needless one: the property is the answer. */
                cls = JS_DiagCString(ctx, exception_val, &cls_owned);
                if (cls == NULL) {
                    ret = -1;
                } else {
                    char *error_class = strdup_len(cls, strcspn(cls, ":"));
                    if (!str_equal(error_class, error_type))
                        ret = -1;
                    free(error_class);
                    JS_DiagFreeCString(ctx, cls, cls_owned);
                }
            }
        } else {
            ret = -1;
        }
    } else {
        if (is_negative)
            ret = -1;
        else
            ret = 0;
    }

    if (verbose && is_test) {
        JSValue msg_val = JS_UNDEFINED;
        char *msg_owned = NULL;
        const char *msg = NULL;
        int s_line;
        char *s = find_error(filename, &s_line, eval_flags & JS_EVAL_FLAG_STRICT);
        const char *strict_mode = (eval_flags & JS_EVAL_FLAG_STRICT) ? "strict mode: " : "";
        bool is_unexpected_error = true;

        if (!JS_IsUndefined(exception_val)) {
            msg = JS_DiagCString(ctx, exception_val, &msg_owned);
        }
        if (is_negative) {  // expect error
            if (ret == 0) {
                if (msg && s &&
                    (str_equal(s, "expected error") ||
                     js__strstart(s, "unexpected error type:", NULL) ||
                     str_equal(s, msg))) {     // did not have error yet
                    if (!has_error_line) {
                        longest_match(buf, msg, pos, &pos, pos_line, &error_line);
                    }
                    printf("%s:%d: %sOK, now has error %s\n",
                           filename, error_line, strict_mode, msg);
                    atomic_inc(&fixed_errors);
                    is_unexpected_error = false;
                }
            } else {
                if (!s) {   // not yet reported
                    if (msg) {
                        fprintf(error_out, "%s:%d: %sunexpected error type: %s\n",
                                filename, error_line, strict_mode, msg);
                    } else {
                        fprintf(error_out, "%s:%d: %sexpected error\n",
                                filename, error_line, strict_mode);
                    }
                    atomic_inc(&new_errors);
                }
            }
        } else {            // should not have error
            if (msg) {
                if (!s || !str_equal(s, msg)) {
                    if (!has_error_line) {
                        char *p = skip_prefix(msg, "Test262 Error: ");
                        if (strstr(p, "Test case returned non-true value!")) {
                            longest_match(buf, "runTestCase", pos, &pos, pos_line, &error_line);
                        } else {
                            longest_match(buf, p, pos, &pos, pos_line, &error_line);
                        }
                    }
                    fprintf(error_out, "%s:%d: %s%s%s\n", filename, error_line, strict_mode,
                            error_file ? "unexpected error: " : "", msg);

                    if (s && (!str_equal(s, msg) || error_line != s_line)) {
                        printf("%s:%d: %sprevious error: %s\n", filename, s_line, strict_mode, s);
                        atomic_inc(&changed_errors);
                    } else {
                        atomic_inc(&new_errors);
                    }
                }
            } else {
                if (s) {
                    printf("%s:%d: %sOK, fixed error: %s\n", filename, s_line, strict_mode, s);
                    atomic_inc(&fixed_errors);
                    is_unexpected_error = false;
                }
            }
        }
        if (is_unexpected_error && verbose > 1 && JS_IsException(exception_val)) {
            JSValue val = JS_GetErrorStackString(ctx, exception_val);
            if (!JS_IsException(val) &&
                !JS_IsUndefined(val) &&
                !JS_IsNull(val)) {
                const char *str = JS_ToCString(ctx, val);
                if (str)
                    printf("%s\n", str);
                JS_FreeCString(ctx, str);
                JS_FreeValue(ctx, val);
            }
        }
        JS_FreeValue(ctx, msg_val);
        JS_DiagFreeCString(ctx, msg, msg_owned);
        free(s);
    }

    if (local) {
        ret = js_std_loop(ctx);
        if (ret)
            js_std_dump_error(ctx);
    }

    JS_FreeCString(ctx, error_name);
    JS_FreeValue(ctx, exception_val);
    JS_FreeValue(ctx, res_val);
    return ret;
}

static int eval_file(JSContext *ctx, const char *base, const char *p,
                     int eval_flags)
{
    char *buf;
    size_t buf_len;
    char *filename = compose_path(base, p);
    int msec = 0;

    buf = load_file(filename, &buf_len);
    if (!buf) {
        warning("cannot load %s", filename);
        goto fail;
    }
    if (eval_buf(ctx, buf, buf_len, filename, false, false, NULL,
                 eval_flags, false, &msec)) {
        warning("error evaluating %s", filename);
        goto fail;
    }
    free(buf);
    free(filename);
    return 0;

fail:
    free(buf);
    free(filename);
    return 1;
}

char *extract_desc(const char *buf)
{
    const char *p, *desc_start;
    char *desc;
    int len;

    p = buf;
    while (*p != '\0') {
        if (p[0] == '/' && p[1] == '*' && p[2] == '-' && p[3] != '/') {
            p += 3;
            desc_start = p;
            while (*p != '\0' && (p[0] != '*' || p[1] != '/'))
                p++;
            if (*p == '\0') {
                warning("Expecting end of desc comment");
                return NULL;
            }
            len = p - desc_start;
            desc = malloc(len + 1);
            memcpy(desc, desc_start, len);
            desc[len] = '\0';
            return desc;
        } else {
            p++;
        }
    }
    return NULL;
}

static char *find_tag(char *desc, const char *tag, int *state)
{
    char *p;
    p = strstr(desc, tag);
    if (p) {
        p += strlen(tag);
        *state = 0;
    }
    return p;
}

static char *get_option(char **pp, int *state)
{
    char *p, *p0, *option = NULL;
    if (*pp) {
        for (p = *pp;; p++) {
            switch (*p) {
            case '[':
                *state += 1;
                continue;
            case ']':
                *state -= 1;
                if (*state > 0)
                    continue;
                p = NULL;
                break;
            case ' ':
            case '\t':
            case '\r':
            case ',':
            case '-':
                continue;
            case '\n':
                if (*state > 0 || p[1] == ' ')
                    continue;
                p = NULL;
                break;
            case '\0':
                p = NULL;
                break;
            default:
                p0 = p;
                p += strcspn(p0, " \t\r\n,]");
                option = strdup_len(p0, p - p0);
                break;
            }
            break;
        }
        *pp = p;
    }
    return option;
}

void update_stats(JSRuntime *rt, const char *filename) {
    JSMemoryUsage stats;
    JS_ComputeMemoryUsage(rt, &stats);
    js_mutex_lock(&stats_mutex);
    if (stats_count++ == 0) {
        stats_avg = stats_all = stats_min = stats_max = stats;
        free(stats_min_filename);
        stats_min_filename = strdup(filename);
        free(stats_max_filename);
        stats_max_filename = strdup(filename);
    } else {
        if (stats_max.malloc_size < stats.malloc_size) {
            stats_max = stats;
            free(stats_max_filename);
            stats_max_filename = strdup(filename);
        }
        if (stats_min.malloc_size > stats.malloc_size) {
            stats_min = stats;
            free(stats_min_filename);
            stats_min_filename = strdup(filename);
        }

#define update(f)  stats_avg.f = (stats_all.f += stats.f) / stats_count
        update(malloc_count);
        update(malloc_size);
        update(memory_used_count);
        update(memory_used_size);
        update(atom_count);
        update(atom_size);
        update(str_count);
        update(str_size);
        update(obj_count);
        update(obj_size);
        update(prop_count);
        update(prop_size);
        update(shape_count);
        update(shape_size);
        update(js_func_count);
        update(js_func_size);
        update(js_func_code_size);
        update(js_func_pc2line_count);
        update(js_func_pc2line_size);
        update(c_func_count);
        update(array_count);
        update(fast_array_count);
        update(fast_array_elements);
    }
#undef update
    js_mutex_unlock(&stats_mutex);
}

static JSValue qjs_black_box(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst argv[], int magic)
{
    return JS_NewInt32(ctx, js_std_cmd(magic, ctx, &argv[0]));
}

static const JSCFunctionListEntry qjs_methods[] = {
    JS_CFUNC_MAGIC_DEF("getStringKind", 1, qjs_black_box, /*GetStringKind*/3),
};

static const JSCFunctionListEntry qjs_object =
    JS_OBJECT_DEF("qjs", qjs_methods, countof(qjs_methods), JS_PROP_C_W_E);

JSContext *JS_NewCustomContext(JSRuntime *rt)
{
    JSContext *ctx;
    JSValue obj;

    ctx = JS_NewContext(rt);
    if (ctx && local) {
        js_init_module_std(ctx, "qjs:std");
        js_init_module_os(ctx, "qjs:os");
        js_init_module_bjson(ctx, "qjs:bjson");
        obj = JS_GetGlobalObject(ctx);
        JS_SetPropertyFunctionList(ctx, obj, &qjs_object, 1);
        JS_FreeValue(ctx, obj);
    }
    return ctx;
}

int run_test_buf(ThreadLocalStorage *tls, const char *filename, char *harness,
                 namelist_t *ip, char *buf, size_t buf_len,
                 const char* error_type, int eval_flags, bool is_negative,
                 bool is_async, bool can_block, bool track_promise_rejections,
                 int *msec)
{
    JSRuntime *rt;
    JSContext *ctx;
    int i, ret;

    rt = JS_NewRuntime();
    if (rt == NULL) {
        fatal(1, "JS_NewRuntime failure");
    }
    JS_SetDumpFlags(rt, JS_DUMP_LEAKS);
    JS_SetRuntimeOpaque(rt, tls);
    js_std_init_handlers(rt);
    ctx = JS_NewCustomContext(rt);
    if (ctx == NULL) {
        JS_FreeRuntime(rt);
        fatal(1, "JS_NewContext failure");
    }
    JS_SetRuntimeInfo(rt, filename);

    JS_SetCanBlock(rt, can_block);

    /* loader for ES6 modules */
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader_test, NULL, (void *) filename);

    if (track_promise_rejections)
        JS_SetHostPromiseRejectionTracker(rt, js_std_promise_rejection_tracker, NULL);

    add_helpers(ctx);

    for (i = 0; i < ip->count; i++) {
        if (eval_file(ctx, harness, ip->array[i], JS_EVAL_TYPE_GLOBAL)) {
            fatal(1, "error including %s for %s", ip->array[i], filename);
        }
        // hack to get useful stack traces from Test262Error exceptions
        if (verbose > 1 && str_equal(ip->array[i], "sta.js")) {
            static const char hack[] =
                ";(function(C){"
                "globalThis.Test262Error = class Test262Error extends Error {};"
                "globalThis.Test262Error.thrower = C.thrower;"
                "})(Test262Error)";
            JS_FreeValue(ctx, JS_Eval(ctx, hack, sizeof(hack)-1, "sta.js", JS_EVAL_TYPE_GLOBAL));
        }
    }

    ret = eval_buf(ctx, buf, buf_len, filename, true, is_negative,
                   error_type, eval_flags, is_async, msec);
    ret = (ret != 0);

    if (dump_memory) {
        update_stats(rt, filename);
    }
    js_agent_free(ctx);
    JS_FreeContext(ctx);
    js_std_free_handlers(rt);
    JS_FreeRuntime(rt);

    atomic_inc(&test_count);
    if (ret)
        atomic_inc(&test_failed);
    return ret;
}

int run_test(ThreadLocalStorage *tls, const char *filename, int *msec)
{
    char harnessbuf[1024];
    char *harness;
    char *buf;
    size_t buf_len;
    char *desc, *p;
    char *error_type;
    int ret, eval_flags, use_strict, use_nostrict;
    bool is_negative, is_nostrict, is_onlystrict, is_async, is_module, skip;
    bool detect_module = true;
    bool track_promise_rejections = false;
    bool can_block;
    namelist_t include_list = { 0 }, *ip = &include_list;

    is_nostrict = is_onlystrict = is_negative = is_async = is_module = skip = false;
    can_block = true;
    error_type = NULL;
    buf = load_file(filename, &buf_len);

    harness = harness_dir;

    if (!harness) {
        const char *p1 = strstr(filename, "test/");
        if (p1) {
            snprintf(harnessbuf, sizeof(harnessbuf), "%.*s%s",
                     (int)(p1 - filename), filename, "harness");
        }
        harness = harnessbuf;
    }
    if (!local) {
        namelist_add(ip, NULL, "sta.js");
        namelist_add(ip, NULL, "assert.js");
    }
    /* extract the YAML frontmatter */
    desc = extract_desc(buf);
    if (desc) {
        char *ifile, *option;
        int state;
        p = find_tag(desc, "includes:", &state);
        if (p) {
            while ((ifile = get_option(&p, &state)) != NULL) {
                // skip unsupported harness files
                if (find_word(harness_exclude, ifile)) {
                    skip |= 1;
                } else {
                    namelist_add(ip, NULL, ifile);
                }
                free(ifile);
            }
        }
        p = find_tag(desc, "flags:", &state);
        if (p) {
            while ((option = get_option(&p, &state)) != NULL) {
                if (str_equal(option, "noStrict") ||
                    str_equal(option, "raw")) {
                    is_nostrict = true;
                    skip |= (test_mode == TEST_STRICT);
                }
                else if (str_equal(option, "onlyStrict")) {
                    is_onlystrict = true;
                    skip |= (test_mode == TEST_NOSTRICT);
                }
                else if (str_equal(option, "async")) {
                    is_async = true;
                    skip |= skip_async;
                }
                else if (str_equal(option, "qjs:no-detect-module")) {
                    detect_module = false;
                }
                else if (str_equal(option, "qjs:track-promise-rejections")) {
                    track_promise_rejections = true;
                }
                else if (str_equal(option, "module")) {
                    is_module = true;
                    skip |= skip_module;
                }
                else if (str_equal(option, "CanBlockIsFalse")) {
                    can_block = false;
                }
                free(option);
            }
        }
        p = find_tag(desc, "negative:", &state);
        if (p) {
            /* XXX: should extract the phase */
            char *q = find_tag(p, "type:", &state);
            if (q) {
                while (isspace((unsigned char)*q))
                    q++;
                error_type = strdup_len(q, strcspn(q, " \r\n"));
            }
            is_negative = true;
        }
        p = find_tag(desc, "features:", &state);
        if (p) {
            while ((option = get_option(&p, &state)) != NULL) {
                if (find_word(harness_features, option)) {
                    /* feature is enabled */
                } else if (find_word(harness_skip_features, option)) {
                    /* skip disabled feature */
                    skip |= 1;
                } else {
                    /* feature is not listed: skip and warn */
                    printf("%s:%d: unknown feature: %s\n", filename, 1, option);
                    skip |= 1;
                }
                free(option);
            }
        }
        free(desc);
    }
    if (is_async)
        namelist_add(ip, NULL, "doneprintHandle.js");

    use_strict = use_nostrict = 0;
    /* XXX: should remove 'test_mode' or simplify it just to force
       strict or non strict mode for single file tests */
    switch (test_mode) {
    case TEST_DEFAULT_NOSTRICT:
        if (is_onlystrict)
            use_strict = 1;
        else
            use_nostrict = 1;
        break;
    case TEST_DEFAULT_STRICT:
        if (is_nostrict)
            use_nostrict = 1;
        else
            use_strict = 1;
        break;
    case TEST_NOSTRICT:
        if (!is_onlystrict)
            use_nostrict = 1;
        break;
    case TEST_STRICT:
        if (!is_nostrict)
            use_strict = 1;
        break;
    case TEST_ALL:
        if (is_module) {
            use_nostrict = 1;
        } else {
            if (!is_nostrict)
                use_strict = 1;
            if (!is_onlystrict)
                use_nostrict = 1;
        }
        break;
    }

    if (skip || use_strict + use_nostrict == 0) {
        atomic_inc(&test_skipped);
        ret = -2;
    } else {
        if (local && detect_module) {
            is_module = JS_DetectModule(buf, buf_len);
        }
        if (is_module) {
            eval_flags = JS_EVAL_TYPE_MODULE;
        } else {
            eval_flags = JS_EVAL_TYPE_GLOBAL;
        }
        ret = 0;
        if (use_nostrict) {
            ret = run_test_buf(tls, filename, harness, ip, buf, buf_len,
                               error_type, eval_flags, is_negative, is_async,
                               can_block, track_promise_rejections, msec);
        }
        if (use_strict) {
            ret |= run_test_buf(tls, filename, harness, ip, buf, buf_len,
                                error_type, eval_flags | JS_EVAL_FLAG_STRICT,
                                is_negative, is_async, can_block,
                                track_promise_rejections, msec);
        }
    }
    namelist_free(&include_list);
    free(error_type);
    free(buf);

    return ret;
}

/* run a test when called by test262-harness+eshost */
int run_test262_harness_test(ThreadLocalStorage *tls, const char *filename,
                             bool is_module, bool can_block)
{
    JSRuntime *rt;
    JSContext *ctx;
    char *buf;
    size_t buf_len;
    int eval_flags, ret_code, ret;
    JSValue res_val;

    outfile = stdout; /* for js_print_262 */

    rt = JS_NewRuntime();
    if (rt == NULL) {
        fatal(1, "JS_NewRuntime failure");
    }
    JS_SetDumpFlags(rt, JS_DUMP_LEAKS);
    JS_SetRuntimeOpaque(rt, tls);
    ctx = JS_NewContext(rt);
    if (ctx == NULL) {
        JS_FreeRuntime(rt);
        fatal(1, "JS_NewContext failure");
    }
    JS_SetRuntimeInfo(rt, filename);

    JS_SetCanBlock(rt, can_block);

    /* loader for ES6 modules */
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader_test, NULL, (void *) filename);

    add_helpers(ctx);

    buf = load_file(filename, &buf_len);

    if (is_module) {
      eval_flags = JS_EVAL_TYPE_MODULE;
    } else {
      eval_flags = JS_EVAL_TYPE_GLOBAL;
    }
    res_val = JS_Eval(ctx, buf, buf_len, filename, eval_flags);
    ret_code = 0;
    if (JS_IsException(res_val)) {
       js_std_dump_error(ctx);
       ret_code = 1;
    } else {
        JSValue promise = JS_UNDEFINED;
        if (is_module) {
            promise = res_val;
        } else {
            JS_FreeValue(ctx, res_val);
        }
        for(;;) {
            JSContext *ctx1;
            /* THE PUMP: a flow parked inside job-driven code resumes BEFORE any queued job — a forced preempt
               must not reorder observable microtasks. With one flow that is immediately, which is exactly what
               the scheduler decides when nothing else is runnable.
               ITS COMPLETION IS TAKEN AND REPORTED, exactly as the job below it is — a parked continuation is
               the LATE half of a job, and an uncaught throw out of one used to be left in the per-runtime slot
               with nothing to read it. */
            for (;;) {
                JSValue pcv;
                if (!JS_ResumeParkedFlow(JS_GetRuntime(ctx), &pcv))
                    break;
                if (JS_IsException(pcv)) {
                    js_std_dump_error(ctx);
                    ret_code = 1;
                }
                JS_FreeValue(ctx, pcv);
            }
            host_pump_atomics_async(JS_GetRuntime(ctx), false);
            ret = JS_ExecutePendingJob(JS_GetRuntime(ctx), &ctx1);
            if (ret < 0) {
                js_std_dump_error(ctx1);
                ret_code = 1;
            } else if (ret == 0) {
                if (host_pump_atomics_async(JS_GetRuntime(ctx), true))
                    continue;
                break;
            }
         }
         /* dump the error if the module returned an error. */
         if (is_module) {
             JSPromiseStateEnum state = JS_PromiseState(ctx, promise);
             if (state == JS_PROMISE_REJECTED) {
                 JS_Throw(ctx, JS_PromiseResult(ctx, promise));
                 js_std_dump_error(ctx);
                 ret_code = 1;
             }
         }
         JS_FreeValue(ctx, promise);
    }
    free(buf);
    js_agent_free(ctx);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return ret_code;
}

clock_t last_clock;

void show_progress(void *unused) {
    int interval = 1000*1000*1000 / 4; // 250 ms

    js_mutex_lock(&progress_mutex);
    while (js_cond_timedwait(&progress_cond, &progress_mutex, interval)) {
        /* output progress indicator: erase end of line and return to col 0 */
        fprintf(stderr, "%d/%d/%d        \r",
                atomic_load(&test_failed),
                atomic_load(&test_count),
                atomic_load(&test_skipped));
        fflush(stderr);
    }
    js_mutex_unlock(&progress_mutex);
}

enum { INCLUDE, EXCLUDE, SKIP };

int include_exclude_or_skip(int i) // naming is hard...
{
    if (namelist_find(&exclude_list, test_list.array[i]) >= 0)
        return EXCLUDE;
    if (i < start_index)
        return SKIP;
    if (stop_index >= 0 && i > stop_index)
        return SKIP;
    return INCLUDE;
}

void run_test_dir_list(void *arg)
{
    ThreadLocalStorage tls_s, *tls = &tls_s;
    const char *p;
    int i, msec;

    init_thread_local_storage(tls);

    for (i = (uintptr_t)arg; i < test_list.count; i += nthreads) {
        if (INCLUDE != include_exclude_or_skip(i))
            continue;
        p = test_list.array[i];
        msec = 0;
        run_test(tls, p, &msec);
        if (verbose > 1 || (slow_test_threshold && msec >= slow_test_threshold))
            fprintf(stderr, "%s (%d ms)\n", p, msec);
    }
}

void help(void)
{
    printf("run-test262 version %s\n"
           "usage: run-test262 [options] {-f file ... | [dir_list] [index range]}\n"
           "-h             help\n"
           "-a             run tests in strict and nostrict modes\n"
           "-m             print memory usage summary\n"
           "-N             run test prepared by test262-harness+eshost\n"
           "-s             run tests in strict mode, skip @nostrict tests\n"
           "-E             only run tests from the error file\n"
           "-u             update error file\n"
           "-v             verbose: output error messages\n"
           "-vv            like -v but also print test name and running time\n"
           "-T duration    display tests taking more than 'duration' ms\n"
           "-t threads     number of parallel threads; default: numcpus - 1\n"
           "-c file        read configuration from 'file'\n"
           "-d dir         run all test files in directory tree 'dir'\n"
           "-e file        load the known errors from 'file'\n"
           "-f file        execute single test from 'file'\n"
           "-x file        exclude tests listed in 'file'\n"
           "-I dir         re-include a directory the config excludes (file exclusions still apply)\n"
           "--no-can-block set [[CanBlock]] to false (Atomics.wait will throw)\n",
           JS_GetVersion());
    exit(1);
}

char *get_opt_arg(const char *option, char *arg)
{
    if (!arg) {
        fatal(2, "missing argument for option %s", option);
    }
    return arg;
}

int main(int argc, char **argv)
{
    ThreadLocalStorage tls_s, *tls = &tls_s;
    int i, optind;
    bool is_dir_list;
    bool only_check_errors = false;
    const char *filename;
    const char *ignore = "";
    bool is_test262_harness = false;
    bool is_module = false;
    bool can_block = true;
    bool enable_progress = true;

    js_std_set_worker_new_context_func(JS_NewCustomContext);

    init_thread_local_storage(tls);
    js_mutex_init(&stats_mutex);

#ifndef _WIN32
    /* Date tests assume California local time */
    setenv("TZ", "America/Los_Angeles", 1);
#endif

    // minus one to not (over)commit the system completely
    nthreads = cpu_count() - 1;

    optind = 1;
    while (optind < argc) {
        char *arg = argv[optind];
        if (*arg != '-')
            break;
        optind++;
        if (strstr("-c -d -e -x -I -f -E -T -t", arg))
            optind++;
        if (strstr("-d -f", arg))
            ignore = "testdir"; // run only the tests from -d or -f
    }

    /* cannot use getopt because we want to pass the command line to
       the script */
    optind = 1;
    is_dir_list = true;
    while (optind < argc) {
        char *arg = argv[optind];
        if (*arg != '-')
            break;
        optind++;
        if (str_equal(arg, "-h")) {
            help();
        } else if (str_equal(arg, "-m")) {
            dump_memory++;
        } else if (str_equal(arg, "-s")) {
            test_mode = TEST_STRICT;
        } else if (str_equal(arg, "-a")) {
            test_mode = TEST_ALL;
        } else if (str_equal(arg, "-u")) {
            update_errors++;
        } else if (arg == strstr(arg, "-v")) {
            verbose += str_count(arg, "v");
        } else if (str_equal(arg, "-c")) {
            load_config(get_opt_arg(arg, argv[optind++]), ignore);
        } else if (str_equal(arg, "-d")) {
            enumerate_tests(get_opt_arg(arg, argv[optind++]));
        } else if (str_equal(arg, "-e")) {
            /* error_filename is freed at exit, and the config's `errorfile=` path arrives here malloc'd from
               compose_path — so this one must own its string too, not borrow argv's. */
            free(error_filename);
            error_filename = strdup(get_opt_arg(arg, argv[optind++]));
        } else if (str_equal(arg, "-x")) {
            namelist_load(&exclude_list, get_opt_arg(arg, argv[optind++]));
        } else if (str_equal(arg, "-I")) {
            /* RE-INCLUDE a directory the config excludes, for a run that names it on purpose. The exclude list
               already understands a leading `!` and resolves overlaps by longest prefix, so this needs no new
               matching rule — it writes the entry the config could have written. FILE-level exclusions are
               untouched: those are matched by equality in include_exclude_or_skip, not by prefix here, so a
               single file the config rules out for a stated reason stays out. */
            {
                const char *p = get_opt_arg(arg, argv[optind++]);
                char *bang = malloc(strlen(p) + 3);
                if (!bang)
                    fatal(1, "out of memory");
                snprintf(bang, strlen(p) + 3, "!%s%s", p, js__has_suffix(p, "/") ? "" : "/");
                namelist_add(&exclude_list, NULL, bang);
                free(bang);
            }
        } else if (str_equal(arg, "-f")) {
            is_dir_list = false;
        } else if (str_equal(arg, "-E")) {
            only_check_errors = true;
        } else if (str_equal(arg, "-T")) {
            slow_test_threshold = atoi(get_opt_arg(arg, argv[optind++]));
        } else if (str_equal(arg, "-t")) {
            nthreads = atoi(get_opt_arg(arg, argv[optind++]));
        } else if (str_equal(arg, "-N")) {
            is_test262_harness = true;
        } else if (str_equal(arg, "--module")) {
            is_module = true;
        } else if (str_equal(arg, "--no-can-block")) {
            can_block = false;
        } else {
            fatal(1, "unknown option: %s", arg);
            break;
        }
    }

    if (optind >= argc && !test_list.count)
        help();

    if (is_test262_harness) {
        return run_test262_harness_test(tls, argv[optind], is_module, can_block);
    }

    nthreads = max_int(nthreads, 1);
    nthreads = min_int(nthreads, countof(threads));

    error_out = stdout;
    if (error_filename) {
        error_file = load_file(error_filename, NULL);
        if (only_check_errors && error_file) {
            namelist_free(&test_list);
            namelist_add_from_error_file(&test_list, error_file);
        }
        if (update_errors) {
            free(error_file);
            error_file = NULL;
            error_out = fopen(error_filename, "w");
            if (!error_out) {
                perror_exit(1, error_filename);
            }
        }
    }

    update_exclude_dirs();

#ifndef _WIN32
    if (!isatty(STDOUT_FILENO)) {
        enable_progress = false;
    }
#endif

    if (is_dir_list) {
        if (optind < argc && !isdigit((unsigned char)argv[optind][0])) {
            filename = argv[optind++];
            namelist_load(&test_list, filename);
        }
        start_index = 0;
        stop_index = -1;
        if (optind < argc) {
            start_index = atoi(argv[optind++]);
            if (optind < argc) {
                stop_index = atoi(argv[optind++]);
            }
        }
        // exclude_dir_list has already been sorted by update_exclude_dirs()
        namelist_sort(&test_list);
        namelist_sort(&exclude_list);
        for (i = 0; i < test_list.count; i++) {
            switch (include_exclude_or_skip(i)) {
            case EXCLUDE:
                test_excluded++;
                break;
            case SKIP:
                test_skipped++;
                break;
            }
        }
        js_cond_init(&progress_cond);
        js_mutex_init(&progress_mutex);
        if (enable_progress) {
            js_thread_create(&progress_thread, show_progress, NULL, /*flags*/0);
        }
        for (i = 0; i < nthreads; i++) {
            js_thread_create(&threads[i], run_test_dir_list,
                             (void *)(uintptr_t)i, /*flags*/0);
        }
        for (i = 0; i < nthreads; i++)
            js_thread_join(threads[i]);
        js_mutex_lock(&progress_mutex);
        js_cond_signal(&progress_cond);
        js_mutex_unlock(&progress_mutex);
        if (enable_progress) {
            js_thread_join(progress_thread);
        }
        js_mutex_destroy(&progress_mutex);
        js_cond_destroy(&progress_cond);
    } else {
        while (optind < argc) {
            int msec = 0;
            run_test(tls, argv[optind++], &msec);
        }
    }

    if (dump_memory) {
        if (dump_memory > 1 && stats_count > 1) {
            printf("\nMininum memory statistics for %s:\n\n", stats_min_filename);
            JS_DumpMemoryUsage(stdout, &stats_min, NULL);
            printf("\nMaximum memory statistics for %s:\n\n", stats_max_filename);
            JS_DumpMemoryUsage(stdout, &stats_max, NULL);
        }
        printf("\nAverage memory statistics for %d tests:\n\n", stats_count);
        JS_DumpMemoryUsage(stdout, &stats_avg, NULL);
        printf("\n");
    }

    if (is_dir_list) {
        fprintf(stderr, "Result: %d/%d error%s",
                test_failed, test_count, test_count != 1 ? "s" : "");
        if (test_excluded)
            fprintf(stderr, ", %d excluded", test_excluded);
        if (test_skipped)
            fprintf(stderr, ", %d skipped", test_skipped);
        if (error_file) {
            if (new_errors)
                fprintf(stderr, ", %d new", new_errors);
            if (changed_errors)
                fprintf(stderr, ", %d changed", changed_errors);
            if (fixed_errors)
                fprintf(stderr, ", %d fixed", fixed_errors);
        }
        fprintf(stderr, "\n");
        /* FEATURE ENGAGEMENT (anti-fake-green): a passing result does NOT prove the time-travel feature ran on the
           test's logic. requested = yield polls where this policy said PARK (one per forced back-edge, one per
           sampled call, one per fork, one per step-machine yield — never a count of points OFFERED, which is now
           every opcode boundary); fired = actually parked+rebuilt. They diverge
           EXACTLY where the feature is gated (nested async/generator activations), so <100% engagement means the
           run passed tests the feature silently skipped — a fake green the harness must flag, for ALL categories. */
        {
            uint64_t req = 0, fired = 0;
            JS_FlowPreemptStats(&req, &fired);
            /* ZERO back-edges is NOT 100% engaged — it PROVES NOTHING: no suspend/resume happened at all (an
               unarmed hook, or a corpus with no loop). Reporting 100% for 0/0 is exactly how an unarmed preempt
               hook stayed invisible; engagement only means something once a back-edge actually parked+rebuilt. */
            if (req == 0) {
                fprintf(stderr, "Feature: NOT ENGAGED — 0 back-edge preempts reached; suspend/resume was never "
                                "exercised, so this run proves nothing\n");
            } else {
                fprintf(stderr, "Feature: %llu preempt-requested, %llu fired (%.1f%% engaged), %llu nested-gap%s\n",
                        (unsigned long long)req, (unsigned long long)fired,
                        100.0 * (double)fired / (double)req,
                        (unsigned long long)(req - fired), (req - fired) != 1 ? "s" : "");
            }
            /* DRIVE-TO-COMPLETION detector: any coroutine body run to completion instead of suspend/resume on the
               tramp chain. 0 across the corpus = pure suspend/resume-at-any-depth; >0 names remaining bypasses. */
            fprintf(stderr, "DriveToCompletion: %llu (0 = pure suspend/resume; >0 = tests ran a coroutine to completion off-tramp)\n",
                    (unsigned long long)JS_DriveToCompletionCount());
            fprintf(stderr, "SyncDriveToCompletion: %llu (0 = every bytecode body runs on the tramp; >0 = that many "
                            "ran by C recursion below a live activation, unable to suspend)\n",
                    (unsigned long long)JS_SyncDriveToCompletionCount());
        }
    }

    if (error_out && error_out != stdout) {
        fclose(error_out);
        error_out = NULL;
    }

    namelist_free(&test_list);
    namelist_free(&exclude_list);
    namelist_free(&exclude_dir_list);
    free(harness_dir);
    free(harness_features);
    free(harness_exclude);
    free(harness_skip_features);
    free(error_file);
    free(error_filename);
    free(stats_min_filename);
    free(stats_max_filename);

    /* Signal that the error file is out of date. */
    return new_errors || changed_errors || fixed_errors;
}
