/* quickjs-gap.h — the DFAIL sites a release build crossed.
 *
 * A dev build aborts at a DFAIL. A release build continues on the code beneath it, so it records the construct
 * here and the result document states it (engine/host/solver/gap_report.h).
 *
 * Each construct owns a static record, linked into one list at its first reach, so recording allocates nothing
 * and is safe wherever a DFAIL stands: in the interpreter, under a COW delta, inside a collection. The records
 * are plain C statics: diagnostic state of the engine instance (one address space) and never flow state, so no
 * delta captures them and a fork, a switch or a resume leaves them as they are. Every count is a lifetime
 * counter of the instance, never reset, so it is not differenced against a gauge.
 *
 * It lives beside quickjs-check.h because the fork's translation units build without the host's include roots.
 * The log is a weak definition, so every program that links a DFAIL holds exactly one, whichever source list
 * it was built from. */
#ifndef QUICKJS_GAP_H
#define QUICKJS_GAP_H

#include <stddef.h>
#include <string.h>

/* The room a site that composes its message keeps for the text of its first reach. */
#define APICLIENT_GAP_TEXT_CAP 512

/* One DFAIL construct. `message` is the construct's string literal (a DFAILF's format), which with `file` is its
   identity; `line` only locates it. A site handed a text it composed has no literal: `message` is NULL and `text`
   holds what its first reach was handed, cut to its room, with `text_full` the uncut length.
   Named residual. Not covered: a composed site's text carries that reach's values, so it is not an identity
   across runs. Next diff builds: DFAILF in quickjs-check.h, and the sites that compose into a buffer of their
   own moved onto it, so every site is stated by a literal. How its absence would show: one construct listed
   under two messages by two runs, each marked composed. */
struct apiclient_gap_site {
    const char *file;
    const char *message;
    char *text;
    size_t text_cap;
    size_t text_full;
    int line;
    unsigned long long reached;        /* crossings, a lifetime counter of the instance */
    unsigned long long order;          /* 1 for the site this instance reached first; 0 until reached */
    struct apiclient_gap_site *next;   /* the site linked before this one */
};

/* Every site this instance has reached, newest first, and how many were numbered. */
struct apiclient_gap_log {
    struct apiclient_gap_site *last;
    unsigned long long sites;
};

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak)) struct apiclient_gap_log apiclient_gap_log;
#else
#error "quickjs-gap.h needs a weak definition and __builtin_constant_p; this compiler states neither"
#endif

/* Count one crossing of `site`; its first is numbered and linked. `text` is what the site handed DFAIL, read
   only where the site has no literal. The operations are atomic because run-test262 runs one runtime per
   thread over these statics; a WebAssembly instance without threads lowers them to plain loads and stores. */
static inline void apiclient_gap_reach(struct apiclient_gap_site *site, const char *text)
{
    if (__atomic_fetch_add(&site->reached, 1, __ATOMIC_RELAXED) != 0) return;
    if (site->message == NULL) {
        size_t n = strlen(text), k = n < site->text_cap ? n : site->text_cap - 1;

        /* Unicode 16.0 §3.9 "Unicode Encoding Forms", Table 3-6: a cut lands on a character boundary. */
        while (k < n && k > 0 && ((unsigned char) text[k] & 0xC0) == 0x80) k--;
        memcpy(site->text, text, k);
        site->text[k] = '\0';
        site->text_full = n;
    }
    site->order = __atomic_add_fetch(&apiclient_gap_log.sites, 1, __ATOMIC_RELAXED);
    site->next = __atomic_load_n(&apiclient_gap_log.last, __ATOMIC_RELAXED);
    while (!__atomic_compare_exchange_n(&apiclient_gap_log.last, &site->next, site, 1,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED)) { }
}

/* The release form of DFAIL(msg): the construct's static record, and one crossing of it. `msg` is evaluated
   once, as the dev form evaluates it. A string literal is a constant expression and becomes the record's
   identity; anything else leaves `message` NULL and gives the site room for its text. */
#define APICLIENT_GAP_REACH(msg) do { \
    static char apiclient_gap_text_[__builtin_constant_p(msg) ? 1 : APICLIENT_GAP_TEXT_CAP]; \
    static struct apiclient_gap_site apiclient_gap_site_ = { \
        __FILE__, __builtin_constant_p(msg) ? (msg) : (const char *) 0, \
        apiclient_gap_text_, sizeof apiclient_gap_text_, 0, __LINE__, 0, 0, (struct apiclient_gap_site *) 0 }; \
    apiclient_gap_reach(&apiclient_gap_site_, (msg)); \
  } while (0)

/* A DFAILF's format, the first of its arguments. */
#define APICLIENT_GAP_FORMAT(fmt, ...) fmt

#endif /* QUICKJS_GAP_H */
