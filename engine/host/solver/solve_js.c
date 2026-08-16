/* THE @S JS-CONTEXT BREAKOUT, CONSTRUCTED — see solve_js.h for why there is no payload list here.
   Every production named below is ECMAScript §12, the Lexical Grammar
   (https://tc39.es/ecma262/multipage/ecmascript-language-lexical-grammar.html); the escape for each state is
   that state's own exit, not a vector chosen beside it. */
#include "solver/solve_js.h"
#include "check.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* THE §12 STATE the attacker's bytes are in. Named after the grammar's own productions because the escape is
   that production's exit — the names ARE the derivation, not documentation of it.
   The second group is states this file can NAME but has no escape rule for, and the third is a scan that could
   not reach the hole at all. Both are returned rather than crashed on HERE, so that the whole work queue is one
   switch in one place (`construct`) — and so a release build, where a DFAIL compiles out, emits no breakout for
   them instead of falling through to another state's escape. */
typedef enum {
    JS_SOURCE = 0,      /* §12.6 between input elements — the bytes ARE source */
    JS_STR_SINGLE,      /* §12.9.4 SingleStringCharacters */
    JS_STR_DOUBLE,      /* §12.9.4 DoubleStringCharacters */
    JS_TEMPLATE,        /* §12.9.6 TemplateCharacters */
    JS_COMMENT_LINE,    /* §12.4 SingleLineComment, §12.5 HashbangComment, Annex B.1.1's two HTML-like ones */
    JS_COMMENT_BLOCK,   /* §12.4 MultiLineComment */
    JS_RE_BODY,         /* §12.9.5 RegularExpressionChars */
    JS_RE_BODY_FIRST,   /* §12.9.5 RegularExpressionFirstChar — the body is still EMPTY at the hole */
    JS_RE_CLASS,        /* §12.9.5 RegularExpressionClassChars */
    JS_RE_FLAGS,        /* §12.9.5 RegularExpressionFlags */

    JS_IN_IDENT,        /* §12.7 inside an IdentifierName token */
    JS_IN_NUMBER,       /* §12.9.3 inside a NumericLiteral token */
    JS_IN_PRIVATE,      /* §12.6 inside a PrivateIdentifier token */
    JS_IN_ESCAPE_DEEP,  /* §12.9.4 inside the digits of a Hex/UnicodeEscapeSequence, past its first character */

    JS_GOAL_AMBIGUOUS,  /* §12.1 the goal symbol at a `/` is a fact about the SYNTACTIC grammar */
    JS_NOT_A_SCRIPT     /* the sink output is not a parseable Script prefix, so §12 defines no state at the hole */
} HoleState;

/* WHERE THE HOLE IS, and one orthogonal bit: whether its FIRST byte is the character an unfinished escape
   sequence consumes. That bit is orthogonal because §12.9.4/.6/.9.5 each admit a backslash inside them, so it
   multiplies the state rather than being one of them — and it changes the escape by exactly one filler byte. */
typedef struct { HoleState st; int esc; } Hole;

/* WHAT THE PREVIOUS INPUT ELEMENT DECIDES ABOUT A `/`. §12 opens with the reason this exists: "There are
   several situations where the identification of lexical input elements is sensitive to the syntactic grammar
   context that is consuming the input elements. This requires multiple goal symbols for the lexical grammar."
   So a scanner cannot always answer it — and where it cannot, this file CRASHES rather than guessing, because
   guessing wrong reports a state the page is not in and the search then never says what it failed to escape.
   (quickjs's own `is_regexp_allowed` guesses `}` and carries an `XXX: regexp may occur after` at that line; it
   is a lookahead heuristic for `js_parse_skip_parens_token`, not the real parse, which asks the parser.) */
enum { PREV_NONE = 0, PREV_OPERAND, PREV_OPERATOR, PREV_AMBIG };

typedef struct {
    const char *s;
    size_t      n, at;
    int         prev;
    int         sol;      /* no token yet on this line — Annex B.1.1's `-->` is a comment only there */
    int         brace;    /* §12.8 `{` depth inside the innermost template substitution (or at top level) */
    int        *tpl;      /* the brace depth each open §12.9.6 substitution suspends — GROWABLE, never a fixed
                             stack: a template's nesting depth is the PAGE's data, so a fixed one is a bound */
    int         ntpl, tplcap;
} Scan;

static void tpl_push(Scan *z, int brace) {
    if (z->ntpl >= z->tplcap) {
        z->tplcap = z->tplcap ? z->tplcap * 2 : 8;
        z->tpl = realloc(z->tpl, (size_t)z->tplcap * sizeof(int));
        CHECK(z->tpl, "solve_js: OOM recording a template substitution's suspended brace depth");
    }
    z->tpl[z->ntpl++] = brace;
}
static int tpl_pop(Scan *z) {
    DCHECK(z->ntpl > 0, "a §12.9.6 TemplateSubstitutionTail was closed with no substitution open — the caller "
                        "tests the same stack before deciding this `}` is a tail, so an empty pop means the two "
                        "tests disagree");
    return z->tpl[--z->ntpl];
}

/* §12.2 Table 31 (WhiteSpace) and §12.3 (LineTerminator), DECODED rather than assumed. U+2028 and U+2029 end a
   §12.4 SingleLineComment, so a scanner that treated every non-ASCII byte as ordinary would run a `//` comment
   past its real end and then report the wrong state for everything after it. */
static uint32_t cp_at(const char *s, size_t n, size_t i, size_t *len) {
    const unsigned char *u = (const unsigned char *)s + i;
    size_t avail = n - i;

    if (u[0] < 0x80)                          { *len = 1; return u[0]; }
    if ((u[0] & 0xE0) == 0xC0 && avail >= 2)  { *len = 2; return ((uint32_t)(u[0] & 0x1F) << 6) | (u[1] & 0x3F); }
    if ((u[0] & 0xF0) == 0xE0 && avail >= 3)  { *len = 3; return ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) | (u[2] & 0x3F); }
    if ((u[0] & 0xF8) == 0xF0 && avail >= 4)  { *len = 4; return ((uint32_t)(u[0] & 0x07) << 18) | ((uint32_t)(u[1] & 0x3F) << 12) | ((uint32_t)(u[2] & 0x3F) << 6) | (u[3] & 0x3F); }
    *len = 1; return 0xFFFD;                  /* an ill-formed byte is one SourceCharacter's worth of nothing */
}
static int is_lt(uint32_t c) { return c == 0x0A || c == 0x0D || c == 0x2028 || c == 0x2029; }
static int is_ws(uint32_t c) {
    return c == 0x09 || c == 0x0B || c == 0x0C || c == 0x20 || c == 0xA0 || c == 0xFEFF ||
           c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F || c == 0x3000;
}
/* §12.7 IdentifierStart/IdentifierPart are UAX #31's ID_Start/ID_Continue, and this file does not carry that
   table: every state it NAMES is opened and closed by ASCII (`'`, `"`, a backtick, `/`, `\`, `#!`, `<!--`), so
   whether a non-ASCII code point is an identifier character or an illegal SourceCharacter cannot move the hole
   from one named state into another — it can only end a token one code point early or late, and the hole's own
   first byte is ASCII alphanumeric by SOLVE_JS_LOCATOR's construction (asserted at the entry point). */
static int is_id_part(uint32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '$' || c == '_' || (c >= 0x80 && !is_ws(c) && !is_lt(c));
}
static int is_id_start(uint32_t c) { return is_id_part(c) && !(c >= '0' && c <= '9'); }

/* §12.9.4 EscapeSequence / LineContinuation, and §12.9.6 TemplateEscapeSequence — the span the `\` at `k`
   consumes. This is what decides where the LITERAL ends, which is the only thing the scan needs from it. */
static size_t esc_span(const char *s, size_t n, size_t k) {
    size_t j = k + 1, l = 0;
    uint32_t c;

    if (j >= n) return n;
    if (s[j] == 'u') {                                     /* UnicodeEscapeSequence */
        j++;
        if (j < n && s[j] == '{') { while (j < n && s[j] != '}') j++; return j < n ? j + 1 : n; }
        return j + 4 <= n ? j + 4 : n;
    }
    if (s[j] == 'x') { j++; return j + 2 <= n ? j + 2 : n; }   /* HexEscapeSequence */
    c = cp_at(s, n, j, &l);
    if (c == 0x0D && j + l < n && s[j + l] == 0x0A) return j + l + 1;   /* <CR><LF> is ONE LineTerminatorSequence */
    return j + l;
}

/* §12.9.4 StringLiteral opened by `q` at `i`. Returns the offset past the closing quote; sets *hit when the
   hole is inside. */
static size_t str_end(Scan *z, size_t i, char q, Hole *h, int *hit) {
    const char *s = z->s;
    size_t n = z->n, k = i + 1;
    HoleState st = q == '\'' ? JS_STR_SINGLE : JS_STR_DOUBLE;

    *hit = 1;
    while (k < n) {
        size_t l = 0;
        uint32_t c;

        if (k == z->at) { h->st = st; h->esc = 0; return k; }
        if (s[k] == '\\') {
            size_t e = esc_span(s, n, k);
            if (z->at == k + 1) { h->st = st; h->esc = 1; return e; }
            if (z->at > k + 1 && z->at < e) { h->st = JS_IN_ESCAPE_DEEP; h->esc = 0; return e; }
            k = e;
            continue;
        }
        if (s[k] == q) { *hit = 0; return k + 1; }
        c = cp_at(s, n, k, &l);
        /* §12.9.4: SingleStringCharacter admits <LS>/<PS> but NOT <LF>/<CR>. One here means the literal is
           unterminated, so the parser fails before the hole and there is no §12 state to report. */
        if (c == 0x0A || c == 0x0D) { h->st = JS_NOT_A_SCRIPT; h->esc = 0; return k; }
        k += l;
    }
    h->st = JS_NOT_A_SCRIPT; h->esc = 0;
    return n;
}

/* §12.9.6 TemplateCharacters from `k` (just past a backtick, or past a substitution's `}`). Returns the offset
   past whichever terminator it found, and sets *sub when that terminator was `${`. */
static size_t tmpl_chars(Scan *z, size_t k, int *sub, Hole *h, int *hit) {
    const char *s = z->s;
    size_t n = z->n;

    *sub = 0; *hit = 1;
    while (k < n) {
        size_t l = 0;

        if (k == z->at) { h->st = JS_TEMPLATE; h->esc = 0; return k; }
        if (s[k] == '\\') {
            size_t e = esc_span(s, n, k);
            if (z->at == k + 1) { h->st = JS_TEMPLATE; h->esc = 1; return e; }
            if (z->at > k + 1 && z->at < e) { h->st = JS_IN_ESCAPE_DEEP; h->esc = 0; return e; }
            k = e;
            continue;
        }
        if (s[k] == '`') { *hit = 0; return k + 1; }
        if (s[k] == '$' && k + 1 < n && s[k + 1] == '{') { *sub = 1; *hit = 0; return k + 2; }
        cp_at(s, n, k, &l);
        k += l;
    }
    h->st = JS_NOT_A_SCRIPT; h->esc = 0;
    return n;
}

/* §12.9.5 RegularExpressionLiteral opened at `i`. The caller has already taken both comment openers, and that
   is a §12.9.5 fact rather than an ordering convenience: RegularExpressionFirstChar admits neither `/` nor `*`,
   so a slash followed by either can only be a §12.4 Comment under EVERY goal symbol — which is why the goal
   ambiguity is only ever asked about a slash followed by a third byte. */
static size_t regexp_end(Scan *z, size_t i, Hole *h, int *hit) {
    const char *s = z->s;
    size_t n = z->n, k = i + 1;
    int cls = 0;

    *hit = 1;
    while (k < n) {
        size_t l = 0;
        uint32_t c;

        /* §12.9.5 splits RegularExpressionFirstChar from RegularExpressionChar, and Note 2 says why it matters
           here: "regular expression literals may not be empty; instead […] the code unit sequence // starts a
           single-line comment". So a hole sitting at the FIRST body character has a different exit from one
           with characters already behind it — closing there would write `//` and comment the fire out. */
        if (k == z->at) {
            h->st = cls ? JS_RE_CLASS : (k == i + 1 ? JS_RE_BODY_FIRST : JS_RE_BODY);
            h->esc = 0;
            return k;
        }
        if (s[k] == '\\') {
            /* §12.9.5 RegularExpressionBackslashSequence is `\` and EXACTLY ONE RegularExpressionNonTerminator
               — there is no \u/\x span here to be deeper inside, unlike §12.9.4's EscapeSequence. */
            if (z->at == k + 1) { h->st = cls ? JS_RE_CLASS : JS_RE_BODY; h->esc = 1; return k + 2; }
            k++;
            if (k >= n) break;
            c = cp_at(s, n, k, &l);
            if (is_lt(c)) { h->st = JS_NOT_A_SCRIPT; h->esc = 0; return k; }
            k += l;
            continue;
        }
        c = cp_at(s, n, k, &l);
        /* §12.9.5 RegularExpressionNonTerminator :: SourceCharacter but not LineTerminator. */
        if (is_lt(c)) { h->st = JS_NOT_A_SCRIPT; h->esc = 0; return k; }
        if (!cls && c == '[') cls = 1;                       /* §12.9.5 RegularExpressionClass */
        else if (cls && c == ']') cls = 0;
        else if (!cls && c == '/') { k += l; break; }
        k += l;
    }
    /* §12.9.5 RegularExpressionFlags :: RegularExpressionFlags IdentifierPartChar */
    while (k < n) {
        size_t l = 0;
        uint32_t c = cp_at(s, n, k, &l);

        if (!is_id_part(c)) break;
        if (k == z->at) { h->st = JS_RE_FLAGS; h->esc = 0; return k; }
        k += l;
    }
    *hit = 0;
    return k;
}

/* §12.4 MultiLineComment. *lf reports whether it contained a LineTerminator, because §12.4 then makes the whole
   comment count as one — which is what Annex B.1.1's `-->` reads to decide it starts a line. */
static size_t block_comment_end(Scan *z, size_t i, int *lf, Hole *h, int *hit) {
    const char *s = z->s;
    size_t n = z->n, k = i + 2;

    *lf = 0; *hit = 1;
    while (k < n) {
        size_t l = 0;
        uint32_t c;

        if (k == z->at) { h->st = JS_COMMENT_BLOCK; h->esc = 0; return k; }
        if (s[k] == '*' && k + 1 < n && s[k + 1] == '/') { *hit = 0; return k + 2; }
        c = cp_at(s, n, k, &l);
        if (is_lt(c)) *lf = 1;
        k += l;
    }
    h->st = JS_NOT_A_SCRIPT; h->esc = 0;   /* §12.4 has no production for an unterminated MultiLineComment */
    return n;
}

/* §12.4 SingleLineComment / §12.5 HashbangComment / Annex B.1.1's two HTML-like ones — one production tail,
   SingleLineCommentChars, so one scanner. `skip` is the marker's length. §12.4: "the LineTerminator at the end
   of the line is not considered to be part of the single-line comment". */
static size_t line_comment_end(Scan *z, size_t i, size_t skip, Hole *h, int *hit) {
    const char *s = z->s;
    size_t n = z->n, k = i + skip;

    *hit = 1;
    while (k < n) {
        size_t l = 0;
        uint32_t c;

        if (k == z->at) { h->st = JS_COMMENT_LINE; h->esc = 0; return k; }
        c = cp_at(s, n, k, &l);
        if (is_lt(c)) break;
        k += l;
    }
    *hit = 0;
    return k;
}

/* §12.7 IdentifierName — IdentifierPart runs, plus the `\ UnicodeEscapeSequence` spelling §12.7 Note 1 admits. */
static size_t ident_end(const char *s, size_t n, size_t i) {
    size_t k = i;

    while (k < n) {
        size_t l = 0;
        uint32_t c;

        /* §12.7 Note 1: an IdentifierName may contain `\ UnicodeEscapeSequence` and nothing else — a backslash
           followed by anything else is not part of the name, and treating it as one would run the token past
           its real end. The caller turns a name of length zero into JS_NOT_A_SCRIPT. */
        if (s[k] == '\\' && k + 1 < n && s[k + 1] == 'u') { k = esc_span(s, n, k); continue; }
        c = cp_at(s, n, k, &l);
        if (!is_id_part(c)) break;
        k += l;
    }
    return k;
}

/* §12.9.3 NumericLiteral, including the NonDecimalIntegerLiteral radix prefixes (whose digit sets swallow the
   locator's leading letters — `0x` followed by it is a hole INSIDE the literal, not one after it), the §12.9.3
   NumericLiteralSeparator, an exponent, and the BigIntLiteralSuffix. */
static size_t num_end(const char *s, size_t n, size_t i) {
    size_t k = i;

    if (s[k] == '0' && k + 1 < n && (s[k + 1] == 'x' || s[k + 1] == 'X' || s[k + 1] == 'o' || s[k + 1] == 'O' ||
                                     s[k + 1] == 'b' || s[k + 1] == 'B')) {
        int hex = s[k + 1] == 'x' || s[k + 1] == 'X';
        k += 2;
        while (k < n && (s[k] == '_' || (s[k] >= '0' && s[k] <= '9') ||
                         (hex && ((s[k] >= 'a' && s[k] <= 'f') || (s[k] >= 'A' && s[k] <= 'F'))))) k++;
    } else {
        while (k < n && ((s[k] >= '0' && s[k] <= '9') || s[k] == '_' || s[k] == '.')) k++;
        if (k < n && (s[k] == 'e' || s[k] == 'E')) {
            size_t j = k + 1;
            if (j < n && (s[j] == '+' || s[j] == '-')) j++;
            /* ExponentPart needs at least one SignedInteger digit; without one the `e` starts an
               IdentifierName instead and the literal ended at `k`. */
            if (j < n && s[j] >= '0' && s[j] <= '9') {
                k = j;
                while (k < n && ((s[k] >= '0' && s[k] <= '9') || s[k] == '_')) k++;
            }
        }
    }
    if (k < n && s[k] == 'n') k++;   /* BigIntLiteralSuffix */
    return k;
}

/* §12.8 Punctuator / DivPunctuator / RightBracePunctuator, longest match — §12 scans "repeatedly taking the
   longest possible sequence of code points as the next input element". `/`, `{` and `}` are handled by the
   caller (a comment, a regular expression literal, and §12.9.6's substitution tail all begin with one). */
static const char *const PUNCT[] = {
    ">>>=", "...", "===", "!==", "**=", "<<=", ">>=", ">>>", "&&=", "||=", "??=",
    "=>", "==", "!=", "<=", ">=", "&&", "||", "??", "++", "--", "+=", "-=", "*=", "%=", "**", "<<", ">>",
    "&=", "|=", "^=",
    "(", ")", "[", "]", ".", ";", ",", "<", ">", "+", "-", "*", "%", "&", "|", "^", "!", "~", "?", ":", "="
};
#define PUNCT_N ((int)(sizeof PUNCT / sizeof PUNCT[0]))

static size_t punct_len(const char *s, size_t n, size_t i) {
    int p;

    /* §12.8 OptionalChainingPunctuator :: `?.` [lookahead ∉ DecimalDigit] — `x?.5:y` is `?` then `.5`. */
    if (s[i] == '?' && i + 1 < n && s[i + 1] == '.' &&
        !(i + 2 < n && s[i + 2] >= '0' && s[i + 2] <= '9')) return 2;
    for (p = 0; p < PUNCT_N; p++) {
        size_t l = strlen(PUNCT[p]);
        if (i + l <= n && !memcmp(s + i, PUNCT[p], l)) return l;
    }
    return 0;
}

/* WHAT A PUNCTUATOR LEAVES BEHIND for the next `/`. `)` is the classic undecidable one — `if (x) /re/.test(y)`
   against `(a)/b/c` — so it is AMBIGUOUS and crashes rather than guessing. `]` and the update operators are
   not: a RegularExpressionLiteral is not a valid assignment target, so `++` and `--` here can only be postfix
   and a `/` after either can only be division. */
static int punct_prev(const char *p, size_t l) {
    if (l == 1 && *p == ')') return PREV_AMBIG;
    if (l == 1 && *p == ']') return PREV_OPERAND;
    if (l == 2 && (!memcmp(p, "++", 2) || !memcmp(p, "--", 2))) return PREV_OPERAND;
    return PREV_OPERATOR;
}

/* §12.7.2 ReservedWord, split by what each leaves behind for a following `/`. A reserved word is an operator
   position (`return /re/`, `typeof /re/`, `case /re/`), EXCEPT the five that are themselves primary
   expressions. `of`, `yield` and `await` are the words whose reading depends on the [Yield]/[Await] grammar
   parameters and on `for`'s own production, which is the syntactic grammar — so they are AMBIGUOUS. */
static const char *const KW_OPERAND[] = { "this", "super", "null", "true", "false" };
static const char *const KW_AMBIG[]   = { "of", "yield", "await" };
static const char *const RESERVED[]   = {
    "await", "break", "case", "catch", "class", "const", "continue", "debugger", "default", "delete", "do",
    "else", "enum", "export", "extends", "false", "finally", "for", "function", "if", "import", "in",
    "instanceof", "new", "null", "return", "super", "switch", "this", "throw", "true", "try", "typeof",
    "var", "void", "while", "with", "yield"
};
static int in_list(const char *const *list, int n, const char *p, size_t l) {
    int i;
    for (i = 0; i < n; i++) if (strlen(list[i]) == l && !memcmp(list[i], p, l)) return 1;
    return 0;
}
static int word_prev(const char *p, size_t l) {
    if (in_list(KW_AMBIG,   (int)(sizeof KW_AMBIG   / sizeof KW_AMBIG[0]),   p, l)) return PREV_AMBIG;
    if (in_list(KW_OPERAND, (int)(sizeof KW_OPERAND / sizeof KW_OPERAND[0]), p, l)) return PREV_OPERAND;
    if (in_list(RESERVED,   (int)(sizeof RESERVED   / sizeof RESERVED[0]),   p, l)) return PREV_OPERATOR;
    return PREV_OPERAND;   /* an IdentifierReference; a `/` after one is §12.8's DivPunctuator */
}

/* THE SCAN. §12: "The source text […] is first converted into a sequence of input elements […] scanned from
   left to right, repeatedly taking the longest possible sequence of code points as the next input element."
   So this consumes ONE input element per turn and answers with the state the moment the hole is inside one —
   or with JS_SOURCE the moment an element BEGINS exactly at the hole, which is what a hole between tokens is. */
static Hole scan(Scan *z) {
    const char *s = z->s;
    size_t n = z->n, i = 0, e;
    Hole h;
    int hit = 0, sub = 0, lf = 0;

    h.st = JS_NOT_A_SCRIPT; h.esc = 0;
    while (i < n) {
        size_t l = 0;
        uint32_t c;

        DCHECK(i <= z->at,
               "a §12 input element was consumed past the @S hole without reporting the state it was in — every "
               "scanner here must answer when the hole falls inside its element, so an overshoot means one "
               "production consumed bytes it never examined and the state reported next belongs to the wrong "
               "element entirely");
        if (i == z->at) { h.st = JS_SOURCE; h.esc = 0; return h; }

        c = cp_at(s, n, i, &l);
        if (is_ws(c)) { i += l; continue; }
        if (is_lt(c)) { i += l; z->sol = 1; continue; }

        if (c == '/') {
            if (i + 1 < n && s[i + 1] == '*') {
                e = block_comment_end(z, i, &lf, &h, &hit);
                if (hit) return h;
                if (lf) z->sol = 1;
                i = e; continue;
            }
            if (i + 1 < n && s[i + 1] == '/') {
                e = line_comment_end(z, i, 2, &h, &hit);
                if (hit) return h;
                i = e; continue;
            }
            if (z->prev == PREV_AMBIG) { h.st = JS_GOAL_AMBIGUOUS; h.esc = 0; return h; }
            if (z->prev == PREV_OPERAND) {                       /* §12.8 DivPunctuator :: `/` | `/=` */
                i += (i + 1 < n && s[i + 1] == '=') ? 2 : 1;
                z->prev = PREV_OPERATOR; z->sol = 0; continue;
            }
            e = regexp_end(z, i, &h, &hit);
            if (hit) return h;
            i = e; z->prev = PREV_OPERAND; z->sol = 0; continue;
        }
        if (c == '\'' || c == '"') {
            e = str_end(z, i, (char)c, &h, &hit);
            if (hit) return h;
            i = e; z->prev = PREV_OPERAND; z->sol = 0; continue;
        }
        if (c == '`') {
            e = tmpl_chars(z, i + 1, &sub, &h, &hit);
            if (hit) return h;
            if (sub) { tpl_push(z, z->brace); z->brace = 0; z->prev = PREV_OPERATOR; }
            else       z->prev = PREV_OPERAND;
            i = e; z->sol = 0; continue;
        }
        if (c == '}') {
            /* §12.9.6: a `}` closing the innermost substitution is scanned under the InputElementTemplateTail
               goal and is a TemplateMiddle/TemplateTail, not §12.8's RightBracePunctuator. The template stack is
               what tells the two apart, and it is exact — a `{` opened inside the substitution raises the depth
               this compares against. */
            if (z->ntpl > 0 && z->brace == 0) {
                z->brace = tpl_pop(z);
                e = tmpl_chars(z, i + 1, &sub, &h, &hit);
                if (hit) return h;
                if (sub) { tpl_push(z, z->brace); z->brace = 0; z->prev = PREV_OPERATOR; }
                else       z->prev = PREV_OPERAND;
                i = e; z->sol = 0; continue;
            }
            if (z->brace == 0) { h.st = JS_NOT_A_SCRIPT; h.esc = 0; return h; }
            z->brace--; z->prev = PREV_AMBIG; i++; z->sol = 0; continue;
        }
        if (c == '{') { z->brace++; z->prev = PREV_OPERATOR; i++; z->sol = 0; continue; }
        if (c == '#') {
            if (i == 0 && n > 1 && s[1] == '!') {                /* §12.5 HashbangComment */
                e = line_comment_end(z, i, 2, &h, &hit);
                if (hit) return h;
                i = e; continue;
            }
            e = ident_end(s, n, i + 1);                          /* §12.6 PrivateIdentifier */
            if (z->at < e) { h.st = JS_IN_PRIVATE; h.esc = 0; return h; }
            i = e; z->prev = PREV_OPERAND; z->sol = 0; continue;
        }
        /* Annex B.1.1, which this engine's own tokenizer implements (`allow_html_comments`), so the derivation
           must agree with the parser that will actually run the payload: `<!--` opens a single-line comment
           anywhere, and `-->` does so only where nothing but white space and comments precede it on the line. */
        if (c == '<' && i + 4 <= n && !memcmp(s + i, "<!--", 4)) {
            e = line_comment_end(z, i, 4, &h, &hit);
            if (hit) return h;
            i = e; continue;
        }
        if (c == '-' && z->sol && i + 3 <= n && !memcmp(s + i, "-->", 3)) {
            e = line_comment_end(z, i, 3, &h, &hit);
            if (hit) return h;
            i = e; continue;
        }
        if (c >= '0' && c <= '9') {
            e = num_end(s, n, i);
            if (z->at < e) { h.st = JS_IN_NUMBER; h.esc = 0; return h; }
            i = e; z->prev = PREV_OPERAND; z->sol = 0; continue;
        }
        if (c == '.' && i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '9') {
            e = num_end(s, n, i);
            if (z->at < e) { h.st = JS_IN_NUMBER; h.esc = 0; return h; }
            i = e; z->prev = PREV_OPERAND; z->sol = 0; continue;
        }
        if (is_id_start(c) || c == '\\') {
            e = ident_end(s, n, i);
            /* A BACKSLASH THAT OPENS NO §12.7 UnicodeEscapeSequence IS NOT AN IdentifierStart, and the guard is
               not defensive: without it the scan would consume nothing and turn on the spot, so an eval sink
               handed a stray backslash would HANG the derivation instead of reporting an unparseable Script. */
            if (e == i) { h.st = JS_NOT_A_SCRIPT; h.esc = 0; return h; }
            if (z->at < e) { h.st = JS_IN_IDENT; h.esc = 0; return h; }
            z->prev = word_prev(s + i, e - i);
            i = e; z->sol = 0; continue;
        }
        l = punct_len(s, n, i);
        if (l == 0) { h.st = JS_NOT_A_SCRIPT; h.esc = 0; return h; }
        z->prev = punct_prev(s + i, l);
        i += l; z->sol = 0;
    }
    DFAIL("the §12 scan reached the end of an eval sink's argument without ever covering the hole — the caller "
          "found the locator inside that same string, so the offset it asked about is one the scan skipped");
    h.st = JS_NOT_A_SCRIPT; h.esc = 0;
    return h;
}

static void emit_one(SolveJsEmit emit, void *user, int *n, const char *fmt, ...) {
    char b[256];
    va_list ap;
    int k;

    va_start(ap, fmt);
    k = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    CHECK(k > 0 && (size_t)k < sizeof b, "solve_js: a constructed breakout did not fit its buffer");
    emit(user, b);
    (*n)++;
}

/* THE ESCAPE IS THE STATE'S OWN EXIT. Nothing below is chosen; each is the byte sequence §12 defines as leaving
   that state, then the fire, then whatever §12 itself makes necessary for the REST of the string to still be a
   Script — because a Script that does not parse cannot fire, and §@S accepts nothing but firing as proof.
   Three §12 rules do all of that work and no fourth is invented:
     - §12.10 rule 1 inserts a semicolon before an offending token only when a LineTerminator separates it from
       the previous one. So an exit that resumes on the SAME line writes its own `;`, and the one that exits by
       ending a line does not — that asymmetry is the spec's, not a style.
     - §12.4 makes a SingleLineComment run to the end of the line, which is exactly where a literal's ORIGINAL
       terminator sits (§12.9.4 forbids a LineTerminator in a string, §12.9.5 in a regular expression), so `//`
       discards precisely the orphaned terminator and nothing else.
     - §12.4 also makes MultiLineComment non-nesting, so the page's own terminator, still ahead in the source,
       would be an offending token: the exit re-opens a MultiLineComment and that same terminator closes it. */
static int construct(Hole h, SolveJsEmit emit, void *user) {
    const char *escape = NULL;
    int n = 0;

    switch (h.st) {
    case JS_SOURCE:
        /* §12.6: the bytes are already input elements of the Script, so there is no state to leave and the
           escape IS the fire — the same answer §13.2.5.1's data state gets in the markup half. */
        escape = "X9()";
        break;
    case JS_STR_SINGLE:  escape = "';X9()//";  break;   /* §12.9.4 SingleStringCharacters end at `'` */
    case JS_STR_DOUBLE:  escape = "\";X9()//"; break;   /* §12.9.4 DoubleStringCharacters end at `"` */
    case JS_TEMPLATE:
        /* §12.9.6 TemplateCharacters end at `${` into a substitution whose expression is EVALUATED, and `}`
           returns to a TemplateMiddle/TemplateTail — so the template's own backtick still closes it and this
           exit is strictly shorter than one that ends the literal. The deleted CANDS_JS could not express it:
           its backtick closed a literal whose terminator was still ahead, which parses as a second template. */
        escape = "${X9()}";
        break;
    case JS_COMMENT_LINE:
        /* §12.4: "the LineTerminator at the end of the line is not considered to be part of the single-line
           comment" — so one byte leaves the state, and §12.10 rule 1 supplies the semicolon across it. */
        escape = "\nX9()";
        break;
    case JS_COMMENT_BLOCK: escape = "*/;X9();/*";  break;   /* §12.4 MultiLineComment, left then re-opened */
    case JS_RE_BODY:       escape = "/;X9()//";    break;   /* §12.9.5 RegularExpressionBody ends at `/` */
    case JS_RE_BODY_FIRST:
        /* THE SAME EXIT, PLUS THE ONE CHARACTER §12.9.5 REQUIRES IT TO HAVE. Nothing of the body precedes the
           hole, so closing immediately would write `//`, which Note 2 makes a §12.4 SingleLineComment and not
           an empty literal — the fire would be inside the comment it wrote. `a` is a RegularExpressionChar and
           a RegularExpressionFirstChar (that production excludes only `*`, `\`, `/` and `[`), so one character
           is the whole of the difference. */
        escape = "a/;X9()//";
        break;
    case JS_RE_CLASS:      escape = "]/;X9()//";   break;   /* §12.9.5 RegularExpressionClass ends at `]` */
    case JS_RE_FLAGS:
        /* §12.9.5 RegularExpressionFlags is an IdentifierPartChar run; `;` is not one, so the literal ends
           there and there is no exit character to write at all. */
        escape = ";X9()//";
        break;

    case JS_IN_IDENT:
        DFAIL("the attacker bytes land INSIDE a §12.7 IdentifierName, and no §12 exit reaches out of a token — "
              "ending the name leaves the page's own leading characters as an IdentifierReference that is "
              "EVALUATED before the injected call, so a ReferenceError there means X9 never runs and the escape "
              "has to be built out of what the probe run learned about that prefix, not out of the grammar");
        return 0;
    case JS_IN_NUMBER:
        DFAIL("the attacker bytes land INSIDE a §12.9.3 NumericLiteral — a radix prefix swallowed them into the "
              "literal's own digit set, so the escape is a digit that terminates the literal followed by a "
              "resume, and which digit is legal depends on the radix; build it");
        return 0;
    case JS_IN_PRIVATE:
        DFAIL("the attacker bytes land INSIDE a §12.6 PrivateIdentifier — a private name is meaningful only "
              "inside the class body that declares it, so the escape is not a token exit but a choice of a "
              "declared name, and it has to be built from what the probe run saw of that class");
        return 0;
    case JS_IN_ESCAPE_DEEP:
        DFAIL("the attacker bytes land inside the DIGITS of a §12.9.4 HexEscapeSequence or "
              "UnicodeEscapeSequence, past the character the backslash consumes — the exit is the remaining hex "
              "digits that sequence still owes, and how many are owed depends on which of `\\x`, `\\uXXXX` and "
              "`\\u{…}` opened it; build that count into the escape");
        return 0;
    case JS_GOAL_AMBIGUOUS:
        DFAIL("a `/` before the hole follows a `)`, a `}`, or one of `of`/`yield`/`await`, and §12 states that "
              "which goal symbol is in force there is decided by the SYNTACTIC grammar — this component has "
              "only the lexical one, so it cannot tell a §12.9.5 RegularExpressionLiteral from §12.8's "
              "DivPunctuator and every state after that `/` would be a guess. Route the question to the real "
              "parser, which already answers it (quickjs calls js_parse_regexp from js_parse_primary_expr, not "
              "from next_token)");
        return 0;
    case JS_NOT_A_SCRIPT:
        DFAIL("the string an eval sink was handed is not a parseable Script prefix before the hole — an "
              "unterminated §12.9.4 string literal, an unterminated §12.4 MultiLineComment, a `}` closing a "
              "brace nothing opened, or a code point §12 admits nowhere. §12 defines no state at the hole, so "
              "the page built this string out of parts this derivation has not been shown how to join");
        return 0;
    }
    /* A PENDING BACKSLASH IS SATISFIED BY EXACTLY ONE CHARACTER, and `n` is the one that consumes nothing
       beyond itself: §12.9.4 SingleEscapeCharacter lists it, and it is neither `x`, `u`, nor a DecimalDigit, so
       no Hex/Unicode/LegacyOctal sequence starts. Inside a §12.9.5 literal the same byte is §22.2.1's
       ControlEscape, so one filler serves every state that admits a backslash. */
    DCHECK(!h.esc || h.st == JS_STR_SINGLE || h.st == JS_STR_DOUBLE || h.st == JS_TEMPLATE ||
           h.st == JS_RE_BODY || h.st == JS_RE_CLASS,   /* never _FIRST: a backslash before it IS a first char */
           "a pending escape sequence was reported for a §12 state that has no backslash production — only "
           "12.9.4's string characters, 12.9.6's template characters and 12.9.5's body and class admit one, so "
           "a comment or a bare source position claiming one means a scanner set the bit on the wrong hole");
    emit_one(emit, user, &n, "%s%s", h.esc ? "n" : "", escape);
    return n;
}

int solve_js_breakouts(const char *output, SolveJsEmit emit, void *user) {
    size_t loclen = sizeof SOLVE_JS_LOCATOR - 1, olen;
    const char *p;
    int n = 0;

    DCHECK(output != NULL && emit != NULL,
           "the JS breakout derivation was asked for the context of nothing, or with nowhere to put what it "
           "derives");
    DCHECK(strstr(output, SOLVE_JS_LOCATOR) != NULL,
           "the JS breakout derivation was handed an eval sink's argument that does not carry the context "
           "locator — the probe candidate substitutes it AT THE SOURCE, so an argument without it is a call the "
           "probe never reached and there is no context in it to read");
    olen = strlen(output);
    for (p = output; (p = strstr(p, SOLVE_JS_LOCATOR)) != NULL; p += loclen) {
        Scan z;
        Hole h;

        /* EACH OCCURRENCE IS SCANNED IN ITS OWN STATE, from the start of the string every time — a page that
           writes the source's value twice puts it in two §12 states, and one payload has to break out of
           whichever one it is read in, so each contributes its own candidate and the ONE that fires is the
           verified PoC. Nothing is renamed to do it (see solve_js.h): the scan is addressed by OFFSET. */
        z.s = output; z.n = olen; z.at = (size_t)(p - output);
        z.prev = PREV_NONE; z.sol = 1; z.brace = 0; z.tpl = NULL; z.ntpl = z.tplcap = 0;
        DCHECK((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'),
               "the @S JS context locator's first byte is not ASCII alphanumeric — every scanner here reads "
               "that byte as an ordinary character of whatever state it is in, so a locator that could open a "
               "string, a comment or an escape would change the very scan it is measuring");
        h = scan(&z);
        free(z.tpl);
        n += construct(h, emit, user);
    }
    return n;
}
