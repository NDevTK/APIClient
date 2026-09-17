/*
 * Regular Expression Engine
 *
 * Copyright (c) 2017-2018 Fabrice Bellard
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
#ifndef LIBREGEXP_H
#define LIBREGEXP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libunicode.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LRE_FLAG_GLOBAL     (1 << 0)
#define LRE_FLAG_IGNORECASE (1 << 1)
#define LRE_FLAG_MULTILINE  (1 << 2)
#define LRE_FLAG_DOTALL     (1 << 3)
#define LRE_FLAG_UNICODE    (1 << 4)
#define LRE_FLAG_STICKY     (1 << 5)
#define LRE_FLAG_INDICES    (1 << 6) /* Unused by libregexp, just recorded. */
#define LRE_FLAG_NAMED_GROUPS (1 << 7) /* named groups are present in the regexp */
#define LRE_FLAG_UNICODE_SETS (1 << 8)
/* INTERNAL, like LRE_FLAG_NAMED_GROUPS: the compiled program contains a character or a range that needs a
   code unit above 0xFF. It is the gate on the one-byte reachability analysis — without it that analysis would
   run for every match of every pattern, and it can only ever change the answer for a pattern that has such a
   character in it. */
#define LRE_FLAG_NON_LATIN1 (1 << 9)

#define LRE_RET_MEMORY_ERROR   (-1)
#define LRE_RET_YIELD          (-2)
#define LRE_RET_BYTECODE_ERROR (-3)

/* trailer length after the group name including the trailing '\0'. It was 2: the NUL plus a per-group
   "scope" byte that only the parser read, and which is gone now that duplicate group names are decided by an
   alternative TREE the parser keeps on the side. A byte the compiled byte code carries and nobody reads is a
   lie about the format. */
#define LRE_GROUP_NAME_TRAILER_LEN 1

uint8_t *lre_compile(int *plen, char *error_msg, int error_msg_size,
                     const char *buf, size_t buf_len, int re_flags,
                     void *opaque);
int lre_get_alloc_count(const uint8_t *bc_buf);
int lre_check_bytecode(const uint8_t *bc_buf, int bc_buf_len);
int lre_get_capture_count(const uint8_t *bc_buf);
int lre_get_flags(const uint8_t *bc_buf);
const char *lre_get_groupnames(const uint8_t *bc_buf);
/* ---- One MATCH, suspendably. ------------------------------------------------------------------------
   lre_exec_backtrack's interpreter loop never recursed — it has always been an explicit backtracking stack —
   but "does not recurse" is not "can be interrupted". Its position (the regexp program counter, the input
   cursor, and the two offsets into the backtracking stack) lived in C locals, so a catastrophically
   backtracking pattern — the ReDoS shape, `/(a+)+b/.exec("a".repeat(40))` — held the thread from the first
   opcode to the last. Those four live on the CONTEXT, which the CALLER owns, and the loop's back-edges
   (REOP_goto, the loop opcodes, and the backtrack pop) are suspension points.

   lre_exec_begin seeds a match, lre_exec_step runs it until it finishes or the host asks for the thread back
   (LRE_RET_YIELD — call again and it continues at the exact opcode, never re-matching), lre_exec_end releases
   the stack. There is deliberately NO run-to-completion entry: a caller that cannot park is a caller that has
   not been routed yet, and one would be the second driver that hides that. */
/* The context is DEFINED here rather than inside libregexp.c because the caller now owns it: a suspended
   match's state has to outlive the frame that began it, and by value is the arrangement with no second
   allocation to lose — static_stack_buf exists precisely so an ordinary match allocates nothing. */
typedef enum {
    RE_EXEC_STATE_SPLIT,
    RE_EXEC_STATE_LOOKAHEAD,
    RE_EXEC_STATE_NEGATIVE_LOOKAHEAD,
} REExecStateEnum;

#if INTPTR_MAX >= INT64_MAX
#define BP_TYPE_BITS 3
#else
#define BP_TYPE_BITS 2
#endif

typedef union {
    uint8_t *ptr;
    intptr_t val; /* for bp, the low BP_SHIFT bits store REExecStateEnum */
    struct {
        uintptr_t val : sizeof(uintptr_t) * 8 - BP_TYPE_BITS;
        uintptr_t type : BP_TYPE_BITS;
    } bp;
} StackElem;

typedef struct REExecContext {
    const uint8_t *cbuf;
    const uint8_t *cbuf_end;
    /* 0 = 8 bit chars, 1 = 16 bit chars, 2 = 16 bit chars, UTF-16 */
    int cbuf_type;
    int capture_count;
    bool is_unicode;
    void *opaque; /* used for the stack-overflow check and the yield question */

    /* THE SUSPENSION POINT: everything else in the interpreter loop is per-opcode. */
    const uint8_t *pc;          /* the regexp program counter */
    const uint8_t *cptr;        /* the input cursor */
    size_t sp_off, bp_off;      /* the two cursors into stack_buf, as OFFSETS: stack_realloc moves the buffer */
    uint8_t **capture;          /* the caller's capture/register array, live for the whole match */

    /* The one-byte filter's answer for THIS match: one byte per program offset, saying whether success is
       still reachable from there. NULL when the filter does not apply — an ordinary pattern, or a two-byte
       subject — and `impossible` is the whole-program verdict the entry point reads. */
    uint8_t *reach;
    /* `reach`'s LENGTH, which is the program's bytecode length. It is stated rather than re-derived from
       bc_body, because a match that SUSPENDS can be forked, and the fork has to copy this buffer — an owner
       that cannot say how big the thing it owns is cannot hand it over. 0 exactly when reach is NULL. */
    size_t reach_size;
    const uint8_t *bc_body;   /* the program's first opcode: `reach` is indexed by the offset from here */
    bool impossible;

    StackElem *stack_buf;
    size_t stack_size;
    /* WHERE the stack lives, stated rather than inferred from `stack_buf == static_stack_buf`. The inference is
       right for a teardown, which sees the context it is destroying; it is WRONG for a fork, which sees a byte
       copy whose stack_buf still holds the ORIGINAL's inline address and cannot compute what that was. */
    bool stack_is_static;
    StackElem static_stack_buf[32]; /* static stack to avoid allocation in most cases */
} REExecContext;

int lre_exec_begin(REExecContext *s, uint8_t **capture,
                   const uint8_t *bc_buf, const uint8_t *cbuf, int cindex, int clen,
                   int cbuf_type, void *opaque);
int lre_exec_step(REExecContext *s);
void lre_exec_end(REExecContext *s);

int lre_parse_escape(const uint8_t **pp, int allow_utf16);
/* lre_is_space() is provided as an inline in libunicode.h */

/* DELETED: lre_check_stack_overflow. It was asked at the top of re_parse_disjunction and
   re_parse_nested_class, and its answer was "stack overflow" — a BOUND in an error's clothing, since both
   grammars have an answer at every depth. Both are frame stacks now and nothing in this engine asks the C
   stack how deep it may go. */
/* must be provided by the user: non-zero when a running match should give the thread back at its next
   back-edge. It REPLACES lre_check_timeout, which was a watchdog — a bound that truncated a match instead of
   parking it. The host answers from its scheduler, and the match resumes exactly where it stopped. */
int lre_want_yield(void *opaque);
void *lre_realloc(void *opaque, void *ptr, size_t size);

/* JS identifier test */
extern uint32_t const lre_id_start_table_ascii[4];
extern uint32_t const lre_id_continue_table_ascii[4];

static inline int lre_js_is_ident_first(int c)
{
    if ((uint32_t)c < 128) {
        return (lre_id_start_table_ascii[c >> 5] >> (c & 31)) & 1;
    } else {
        return lre_is_id_start(c);
    }
}

static inline int lre_js_is_ident_next(int c)
{
    if ((uint32_t)c < 128) {
        return (lre_id_continue_table_ascii[c >> 5] >> (c & 31)) & 1;
    } else {
        /* ZWNJ and ZWJ are accepted in identifiers */
        return lre_is_id_continue(c) || c == 0x200C || c == 0x200D;
    }
}

#ifdef __cplusplus
} /* extern "C" { */
#endif

#endif /* LIBREGEXP_H */
