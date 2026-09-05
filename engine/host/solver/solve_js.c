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
   THE SECOND GROUP IS THE STATES WITH NO ESCAPE, AND THAT IS A FIRST-CLASS ANSWER RATHER THAN A GAP. Each is a
   state §12 defines and out of which §12 defines no exit that could carry a firing call, so `construct` returns
   ZERO for it — the same answer, for the same reason, that solve_html.c's §13.2.5.5 PLAINTEXT arm returns:
   "not one this file has not built yet, one that does not exist — so the search is honestly parked with
   nothing tried". CLAUDE.md §Offensive programming names an unsolved @S sink as explicitly NOT a `@WHY`, and
   an abort here would cost the WHOLE DOCUMENT — every other finding of the run — to report one sink's
   unsolvability. What each of the three states is unable to do differs and is written at its own arm. */
typedef enum {
    JS_SOURCE = 0,      /* §12 Lexical Grammar, between input elements — the bytes ARE source (NOT §12.6
                           Tokens, which is what stood here: an input element is §12's own unit, and the
                           difference is the §12.10.1 boundary construct now has to answer) */
    JS_STR_SINGLE,      /* §12.9.4 SingleStringCharacters */
    JS_STR_DOUBLE,      /* §12.9.4 DoubleStringCharacters */
    JS_TEMPLATE,        /* §12.9.6 TemplateCharacters */
    JS_COMMENT_LINE,    /* §12.4 SingleLineComment, §12.5 HashbangComment, Annex B.1.1's two HTML-like ones */
    JS_COMMENT_BLOCK,   /* §12.4 MultiLineComment */
    JS_RE_BODY,         /* §12.9.5 RegularExpressionChars */
    JS_RE_BODY_FIRST,   /* §12.9.5 RegularExpressionFirstChar — the body is still EMPTY at the hole */
    JS_RE_CLASS,        /* §12.9.5 RegularExpressionClassChars */
    JS_RE_FLAGS,        /* §12.9.5 RegularExpressionFlags */

    JS_IN_IDENT,        /* §12.7 Names and Keywords, inside an IdentifierName token */
    JS_IN_NUMBER,       /* §12.9.3 Numeric Literals, inside a NumericLiteral token */
    JS_IN_PRIVATE,      /* §12.7 Names and Keywords, inside a PrivateIdentifier whose name is not yet empty
                           (§12.6 Tokens, which is what stood here, only LISTS PrivateIdentifier as a
                           CommonToken alternative — `PrivateIdentifier :: # IdentifierName` is §12.7's) */

    JS_IN_PRIVATE_FIRST,/* §12.7, and the name is still EMPTY at the hole — no escape, see construct */
    JS_GOAL_AMBIGUOUS,  /* §12 the goal symbol at a `/` is a fact about the SYNTACTIC grammar. NOT §12.1, which
                           is "Unicode Format-Control Characters"; the multiple-goal-symbols paragraph is §12
                           ECMAScript Language: Lexical Grammar's own opening */
    JS_NOT_A_SCRIPT     /* the sink output is not a parseable Script prefix, so §12 defines no state at the hole */
} HoleState;

/* WHERE THE HOLE IS, and the DEBT an unfinished escape sequence has left it holding. That debt is orthogonal
   to the state because §12.9.4/.6/.9.5 each admit a backslash inside them, so it multiplies the state rather
   than being one of them — and it changes the escape only by the bytes the sequence still owes.
   `esc` IS THE ONE-CHARACTER CASE AND `owed`/`close` ARE THE REST OF THE SAME FACT. The hole's first byte is
   either the single character a `\` consumes (§12.9.4 SingleEscapeCharacter and its neighbours — `esc`), or it
   is inside the DIGITS of a §12.9.4 HexEscapeSequence or UnicodeEscapeSequence, past that character. The
   second used to be its own HoleState and it should never have been one: a hole inside `\x4` is still inside
   the STRING, left by the string's own quote, and the sequence is a prefix the escape pays off — so the state
   is the literal's and the debt rides beside it, which is what makes every literal arm below work unchanged. */
/* …AND, FOR A HOLE BETWEEN TOKENS, THE TWO FACTS §12.10.1 DECIDES ON. A hole in a STATE is left by that
   state's own exit and nothing else matters; a hole at a token BOUNDARY has no state to leave, and what can
   still go wrong there is the boundary itself — whether the grammar accepts the injected call juxtaposed
   against the token before it, which §12.10.1 answers from the previous token and from whether a
   LineTerminator separates them. Carried on the hole because the scan is the only thing that knows them and
   construct is the only thing that needs them. */
typedef struct { HoleState st; int esc; int owed; int close; int prev; int sol; } Hole;

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

/* …AND WHAT THAT SEQUENCE STILL OWES when the hole sits at `at`, past the character the backslash consumes.
   DERIVED FROM esc_span RATHER THAN FROM A SECOND COPY OF §12.9.4's ARITIES, which is the whole reason it is
   spelled this way: `HexEscapeSequence :: x HexDigit HexDigit` and `UnicodeEscapeSequence :: u Hex4Digits`
   are fixed widths, so every byte from the first digit to the span's END is a HexDigit and the debt is simply
   the distance to that end — a second function counting 2 and 4 again would be a second copy of one fact,
   free to disagree with the one the scan is steering by.
   `u{` IS THE ONE THAT IS NOT A WIDTH. §12.9.4 spells it `u{ CodePoint }` and §12.9.6 defines
   `CodePoint :: HexDigits[~Sep] but only if the MV of HexDigits ≤ 0x10FFFF`, so what it owes is the `}` —
   plus ONE HexDigit when the hole stands exactly where the first one would, because CodePoint derives
   HexDigits and HexDigits is not empty. Where digits are ALREADY written the debt is the `}` alone, and that
   is not thrift: appending a digit to a code point the page began could push its MV over 0x10FFFF, which
   §12.9.4.1 Static Semantics: Early Errors makes a SyntaxError, so paying only what is owed is what keeps the
   escape parseable. Returns the HexDigit count; sets *close when a `}` is owed too. */
static int esc_owed(const char *s, size_t n, size_t k, size_t at, int *close) {
    size_t e;

    if (k + 2 < n && s[k + 1] == 'u' && s[k + 2] == '{') { *close = 1; return at > k + 3 ? 0 : 1; }
    *close = 0;
    e = esc_span(s, n, k);
    return at < e ? (int)(e - at) : 0;
}

/* THE DEEP-ESCAPE INVARIANT, EXPANDED AT EACH SITE RATHER THAN CALLED. §12.9.4's sequence is part of a
   Single/DoubleStringCharacter and §12.9.6's of a TemplateCharacter, so a hole inside its digits is still
   inside the LITERAL and is left by the literal's own terminator — the debt is a prefix the escape pays, not
   a state of its own. Two scanners reach that conclusion and a shared FUNCTION would stamp ITS line on both
   crashes; a macro stamps the caller's, which is the one a reader has to open. */
#define SOLVE_JS_DEEP_DCHECK                                                                            \
    DCHECK(k + 1 < n && (s[k + 1] == 'x' || s[k + 1] == 'u'),                                            \
           "a §12.9.4 escape sequence was reported as holding the hole inside its DIGITS while its "     \
           "second character is neither `x` nor `u` — every other EscapeSequence spans ONE code point "  \
           "past the backslash, and the locator's first byte is ASCII alphanumeric (asserted at both "   \
           "entry points), so it can be neither a continuation byte of a multi-byte SourceCharacter nor " \
           "the <LF> of a LineContinuation's <CR><LF>. A hole deep inside any other sequence therefore " \
           "means the span and the hole were measured against different strings")

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
            if (z->at > k + 1 && z->at < e) {
                SOLVE_JS_DEEP_DCHECK;
                h->st = st; h->esc = 0; h->owed = esc_owed(s, n, k, z->at, &h->close);
                return e;
            }
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
            if (z->at > k + 1 && z->at < e) {
                SOLVE_JS_DEEP_DCHECK;
                h->st = JS_TEMPLATE; h->esc = 0; h->owed = esc_owed(s, n, k, z->at, &h->close);
                return e;
            }
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
    /* `?\?=` NOT `??=`: the latter is C's trigraph for `#`, so a compiler in a mode that honours trigraphs
       reads this entry as the one-character string "#" and §12.8's longest match then never sees the nullish
       assignment operator — the hole would land in `a ??= tainted` looking like an unrecognised punctuator.
       GNU mode ignores trigraphs and warns, which is why this is a latent semantic change rather than a live
       defect; the escape makes the bytes say what they mean under either mode. */
    ">>>=", "...", "===", "!==", "**=", "<<=", ">>=", ">>>", "&&=", "||=", "?\?=",
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

    /* EVERY FIELD, at the one declaration, because this function returns from a dozen places and a state that
       does not care about the boundary fields would otherwise return whatever the stack held — which
       construct's JS_SOURCE arm would then read as a real answer. */
    h.st = JS_NOT_A_SCRIPT; h.esc = 0; h.owed = 0; h.close = 0; h.prev = PREV_NONE; h.sol = 1;
    while (i < n) {
        size_t l = 0;
        uint32_t c;

        DCHECK(i <= z->at,
               "a §12 input element was consumed past the @S hole without reporting the state it was in — every "
               "scanner here must answer when the hole falls inside its element, so an overshoot means one "
               "production consumed bytes it never examined and the state reported next belongs to the wrong "
               "element entirely");
        if (i == z->at) { h.st = JS_SOURCE; h.esc = 0; h.prev = z->prev; h.sol = z->sol; return h; }

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
            /* §12.7 `PrivateIdentifier :: # IdentifierName` — the `#` alone is not the token, so WHETHER ANY
               NAME PRECEDES THE HOLE decides whether there is an escape at all, exactly as §12.9.5's empty
               body does one production over. `construct` answers the two differently and says why. */
            e = ident_end(s, n, i + 1);
            if (z->at < e) {
                h.st = z->at == i + 1 ? JS_IN_PRIVATE_FIRST : JS_IN_PRIVATE;
                h.esc = 0; h.sol = 0;
                return h;
            }
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
            if (z->at < e) {
                /* THE EXIT ENDS THE TOKEN, SO THE HOLE IS AT A TOKEN BOUNDARY AND `prev` IS THE SAME FACT IT
                   IS AT JS_SOURCE — what the injected call is juxtaposed against. The token it is juxtaposed
                   against is the page's PREFIX ALONE (`retur` of `return`, `typeof` of `typeofX`), so the
                   classification is asked of `at - i` bytes and not of the whole name the page would have
                   had: which of those two the exit leaves behind is the entire difference between `;X9()`
                   and ` X9()`, and §12.7.2's ReservedWord list is what tells them apart.
                   `sol` IS 0 BY CONSTRUCTION and is written rather than left: a token began before the hole
                   and §12.3's LineTerminator would have ended it, so nothing separates the call from it and
                   §12.10.1 supplies no semicolon. */
                h.st = JS_IN_IDENT; h.esc = 0;
                h.prev = word_prev(s + i, z->at - i);
                h.sol = 0;
                return h;
            }
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

/* AN ESCAPE THE SOURCE CANNOT CARRY IS NOT EMITTED — the same gate solve_html.c states at its own emitter,
   and the one place this file decides it. Declining is not a swallowed condition: the candidate would
   re-run the whole document to arrive percent-encoded at its own sink, which is a search state solve.h
   already reports (`survivedBy` against `sourceEncodes`) and never a fire. */
static int emit_one(SolveJsEmit emit, void *user, int *n, const SolveDelivered *d, const char *fmt, ...) {
    char b[256];
    va_list ap;
    int k;

    va_start(ap, fmt);
    k = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    CHECK(k > 0 && (size_t)k < sizeof b, "solve_js: a constructed breakout did not fit its buffer");
    if (!solve_delivered_ok(d, b)) return 0;
    emit(user, b);
    (*n)++;
    return 1;
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
static int construct(Hole h, const SolveDelivered *d, SolveJsEmit emit, void *user) {
    /* TWO SLOTS, AND THE SECOND IS NOT A SECOND GUESS — it is the one place where §12 hands the question to
       the SYNTACTIC grammar and this component still has an answer worth firing. `if (a)` and `f()` end in
       the SAME `)`, and the first accepts `X9()` juxtaposed against it while the second requires `;X9()`; no
       classification recorded beside `prev` can separate them, because what differs is which production the
       `)` closes. CLAUDE.md §@S already decides what to do with a question re-execution can settle: it says @S
       searches freely for a firing input because emission is working-PoC-only and fire-verified, so a wrong
       solve simply never fires and is discarded, and completeness beats purity. (Stated rather than quoted: a
       quotation of THIS PROJECT's own prose standing under an ECMAScript citation is one engine/citegen.mjs
       reads as a claim about ECMAScript and reports as fabricated, which is a wrong finding at a site nobody
       can repair.) So both spellings are pushed as two candidates of ONE search and the fire picks. Every
       other state fills one slot. */
    const char *escape[2] = { NULL, NULL };
    char pend[8];
    int n = 0, i, owed;

    switch (h.st) {
    case JS_SOURCE:
        /* §12 ECMAScript Language: Lexical Grammar — the bytes BEGIN an input element, so there is no state to
           leave, which is the same answer §13.2.5.1 data state gets in the markup half. What is NOT the same
           is that a markup hole in the data state has no boundary to satisfy and this one does.
           §12.10.1 Rules of Automatic Semicolon Insertion inserts a semicolon before an offending token only
           when a LineTerminator separates it from the previous token, when the offending token is `}`, or at a
           do-while's `)`. So a call juxtaposed against a preceding token that ENDS an expression, on the same
           line, is not an ASI site at all — it is a SyntaxError. `eval("f()" + x)` with the derived escape was
           `f()X9()`, which does not parse, so the candidate never compiled, never fired, and the search parked
           reporting a breakout it had supposedly tried. The separator is written exactly where §12.10.1 does
           not supply one, and nowhere else: after an operator (`cfg=`) a `;` would itself be the SyntaxError.
           (This is the citation that was wrong rather than merely imprecise — it read §12.6 Tokens, and input
            elements are §12. Checking it is what found the missing separator.) */
        if (h.sol || h.prev == PREV_NONE || h.prev == PREV_OPERATOR) escape[0] = "X9()";
        else if (h.prev == PREV_OPERAND) escape[0] = ";X9()";
        else {
            /* THE CRASH THAT STOOD HERE PRESCRIBED A REMEDY THAT CANNOT BE BUILT, AND THAT IS THE FINDING.
               It said the five PREV_AMBIG tokens "split differently and each is decidable", and asked for "a
               second classification recorded beside `prev` at each of the sites that sets it — one word of
               spec per site". Its own worked example refutes it: `f()X9()` does not parse and `if (a)X9()`
               does, and BOTH sites write a `)` — so the fact that decides the boundary is which production
               that `)` closes, which is the syntactic grammar and not something the site setting `prev` can
               know. Its reading of the other four was wrong in the same direction rather than merely
               incomplete: `await` and `of` were said to FORBID the separator, and each is a plain
               IdentifierReference outside an async body or a for-of head, where `await;X9()` parses and
               `await X9()` does not.
               WHAT IS TRUE OF ALL FIVE IS THAT EACH SPELLING PARSES IN SOME READING AND FIRES WHEN IT DOES:
               `if(a)X9()` / `f();X9()`, `{}X9()` / `x={};X9()`, `for(x of X9())`, `yield X9()` (which CALLS
               X9 and yields its result) / `yield;X9()`, `await X9()` / `await;X9()`. So both are emitted and
               re-execution decides, which is §@S's own rule for exactly this shape. */
            escape[0] = ";X9()";
            escape[1] = "X9()";
        }
        break;
    case JS_STR_SINGLE:  escape[0] = "';X9()//";  break;   /* §12.9.4 SingleStringCharacters end at `'` */
    case JS_STR_DOUBLE:  escape[0] = "\";X9()//"; break;   /* §12.9.4 DoubleStringCharacters end at `"` */
    case JS_TEMPLATE:
        /* §12.9.6 TemplateCharacters end at `${` into a substitution whose expression is EVALUATED, and `}`
           returns to a TemplateMiddle/TemplateTail — so the template's own backtick still closes it and this
           exit is strictly shorter than one that ends the literal. The deleted CANDS_JS could not express it:
           its backtick closed a literal whose terminator was still ahead, which parses as a second template. */
        escape[0] = "${X9()}";
        break;
    case JS_COMMENT_LINE:
        /* §12.4: "the LineTerminator at the end of the line is not considered to be part of the single-line
           comment" — so one byte leaves the state, and §12.10 rule 1 supplies the semicolon across it. */
        escape[0] = "\nX9()";
        break;
    case JS_COMMENT_BLOCK: escape[0] = "*/;X9();/*";  break;   /* §12.4 MultiLineComment, left then re-opened */
    case JS_RE_BODY:       escape[0] = "/;X9()//";    break;   /* §12.9.5 RegularExpressionBody ends at `/` */
    case JS_RE_BODY_FIRST:
        /* THE SAME EXIT, PLUS THE ONE CHARACTER §12.9.5 REQUIRES IT TO HAVE. Nothing of the body precedes the
           hole, so closing immediately would write `//`, which Note 2 makes a §12.4 SingleLineComment and not
           an empty literal — the fire would be inside the comment it wrote. `a` is a RegularExpressionChar and
           a RegularExpressionFirstChar (that production excludes only `*`, `\`, `/` and `[`), so one character
           is the whole of the difference. */
        escape[0] = "a/;X9()//";
        break;
    case JS_RE_CLASS:      escape[0] = "]/;X9()//";   break;   /* §12.9.5 RegularExpressionClass ends at `]` */
    case JS_RE_FLAGS:
        /* §12.9.5 RegularExpressionFlags is an IdentifierPartChar run; `;` is not one, so the literal ends
           there and there is no exit character to write at all. */
        escape[0] = ";X9()//";
        break;

    case JS_IN_IDENT:
        /* A TOKEN DOES HAVE AN EXIT, AND THE CRASH THAT STOOD HERE DENIED IT. It said "no §12 exit reaches out
           of a token", and §12's own opening says the source text "is scanned from left to right, repeatedly
           taking the longest possible sequence of code points as the next input element" — so a §12.7
           IdentifierName ends at the first code point that is not an IdentifierPart, and writing one IS the
           exit. What the crash was actually describing is the second half of its own sentence: the truncated
           prefix is left standing where the syntactic grammar will read it, and if it is an undefined
           IdentifierReference the program throws a ReferenceError before X9 runs. That is an ordinary parked
           @S search — the candidate does not fire and is discarded — and never a reason to construct nothing,
           because the prefix a page concatenates onto is usually a name that page has.
           WHICH exit byte is §12.10.1's question asked of the token the truncation leaves BEHIND, which is why
           `scan` classifies the prefix rather than the whole name. After an IdentifierReference, or one of the
           five §12.7.2 words that are themselves primary expressions, the call stands against an operand on
           the same line, §12.10.1 supplies no semicolon, and the exit writes one. After a ReservedWord in
           operator position (`typeof`, `new`, `delete`) a `;` is itself the SyntaxError and the exit is §12.2
           WhiteSpace, which ends the name just as well and leaves the word its operand. `of`, `yield` and
           `await` are each of those in some reading and take both spellings, exactly as the boundary above.
           NO TRAILING `//`, AND THAT IS THE DIFFERENCE BETWEEN A TOKEN INTERIOR AND A LITERAL. Every literal
           arm writes one because §12 leaves it an ORPHANED TERMINATOR — the quote, the backtick, the closing
           `/` the page still has ahead of it — and §12.4 makes a SingleLineComment run to the end of the line,
           which is exactly where that terminator sits. A token has no terminator: it ends where a
           non-IdentifierPart begins, so truncating it orphans NOTHING and a `//` here would discard the page's
           own remaining bytes for no §12 reason. MEASURED, in a real engine over this file's own fixtures:
           carrying one turned `class C{#p=1;m(){this.#p<HOLE>}}new C().m()` and
           `function*g(){yield<HOLE>}g().next()` from FIRING into SyntaxErrors, because the `}` each needs was
           on the same line as the hole. Where the page's suffix does not compose with the truncated token the
           candidate simply does not fire, which is an ordinary parked @S search and is the syntactic grammar's
           half of the question, not §12's.
           NAMED RESIDUAL — a name spelled with §12.7's `\ UnicodeEscapeSequence` (the second alternative of
           both IdentifierStart and IdentifierPart) whose hole lands inside that escape's digits is NOT
           covered: ending the token there leaves a truncated `\u00` and the candidate cannot compile. The next
           diff carries the debt the way the literals now do — ask esc_owed at the identifier branch of `scan`,
           as str_end and tmpl_chars already do, and let `owed` reach this arm. Its absence shows as an eval
           sink whose witness holds a backslash inside the identifier ahead of the locator and whose candidates
           never once fire. */
        if (h.prev == PREV_OPERATOR)     escape[0] = " X9()";
        else if (h.prev == PREV_OPERAND) escape[0] = ";X9()";
        else { escape[0] = ";X9()"; escape[1] = " X9()"; }
        break;
    case JS_IN_NUMBER:
        /* NO DIGIT TERMINATES A NumericLiteral — a digit CONTINUES one, which is the opposite of what the
           crash that stood here said ("the escape is a digit that terminates the literal … which digit is
           legal depends on the radix"). §12.9.3 Numeric Literals ends with the rule that does terminate one:
           "The SourceCharacter immediately following a NumericLiteral must not be an IdentifierStart or
           DecimalDigit". A `;` ends the literal and satisfies that constraint in the same byte, and a literal
           is an operand, so §12.10.1 supplies no semicolon on this line and the exit writes the one it needs.
           THE DIGIT IS OWED FOR THE OPPOSITE REASON, and it is the JS_RE_BODY_FIRST shape one production over:
           the hole is reachable with the literal still EMPTY, because `HexIntegerLiteral :: 0x HexDigits` and
           its binary and octal siblings each need at least one digit of their own set — `0x;` is a
           SyntaxError, and `0x` followed by the locator is exactly how this state is reached, since the
           locator's first byte is ASCII alphanumeric and `HexDigit :: one of 0 1 2 3 4 5 6 7 8 9 a b c d e f
           A B C D E F` swallows it.
           `0` IS THE ONE FILLER THAT SERVES EVERY RADIX, which is why no radix branch is needed and no state
           has to record whether the run was empty: `BinaryDigit :: one of 0 1`, `OctalDigit :: one of 0 1 2 3
           4 5 6 7`, DecimalDigit and HexDigit all derive it, and where digits already stand it merely extends
           a run every one of those productions is left-recursive in. It is the same argument the pending-`n`
           filler below makes for §12.9.4's SingleEscapeCharacter — one byte, legal in every state that can
           owe it. */
        escape[0] = "0;X9()";
        break;
    case JS_IN_PRIVATE:
        /* §12.7 `PrivateIdentifier :: # IdentifierName`, so the exit is the IdentifierName's exit and nothing
           more — the same one JS_IN_IDENT takes. The crash that stood here said, in its own words and no
           standard's, that `the escape is not a token exit but a choice of a declared name`: it IS a token
           exit, and no name has to be chosen, because
           the name the truncation leaves standing is the PAGE'S OWN and the page is what declared it. What
           that crash was right about is confined to the case where no such prefix exists, which is now
           JS_IN_PRIVATE_FIRST and is answered there.
           `;` AND NOT A SPACE, and the split JS_IN_IDENT makes cannot arise: §12.7 derives PrivateIdentifier
           through IdentifierName, and §12.7.2's ReservedWord is a subset of IdentifierName that a leading `#`
           puts out of reach — so the token left behind is an operand in every reading a class body admits
           (`this.#pre`), and §12.10.1 supplies no semicolon against it. NO TRAILING `//`, for the reason
           JS_IN_IDENT gives above — a token orphans no terminator, and a real engine measured the comment
           turning this very state's fixture into a SyntaxError by eating the `}` that closes the method. */
        escape[0] = ";X9()";
        break;
    case JS_IN_PRIVATE_FIRST:
        /* NO ESCAPE, AND THAT IS THE ANSWER RATHER THAN A GAP. §12.7 gives `#` no meaning alone —
           `PrivateIdentifier :: # IdentifierName` — so an exit here would have to supply the whole name, and
           a name this run never observed is one CLAUDE.md §RUN-DON'T-MATCH forbids inventing AND one the
           grammar rejects outright: a PrivateIdentifier must be declared by the class body enclosing it, so
           every name that could be written is an early error and no search over them can converge. The two
           prohibitions agree, and that agreement is what makes this FINAL rather than merely unbuilt.
           Zero, never a crash — the same answer solve_html.c's §13.2.5.5 PLAINTEXT arm gives, for the same
           reason: the search is honestly parked with nothing tried. */
        return 0;
    case JS_GOAL_AMBIGUOUS:
        /* THE LEXICAL GRAMMAR CANNOT DECIDE THIS AND SAYS SO ITSELF, so answering ZERO is the sound answer and
           not a missing one. §12 opens: "There are several situations where the identification of lexical
           input elements is sensitive to the syntactic grammar context that is consuming the input elements.
           This requires multiple goal symbols for the lexical grammar." A `/` after `)`, `}`, `of`, `yield` or
           `await` is exactly such a situation — it is §12.9.5's RegularExpressionLiteral under one goal symbol
           and §12.8's DivPunctuator under another — so every state AFTER it, the hole's included, would be a
           guess, and solve_js.h states the direction that must never be got wrong: an escape is never CLAIMED
           on a scan that could not be made.
           THIS USED TO ABORT, WHICH WAS THE WRONG COST. A page holding `(a+b)/2` anywhere ahead of an eval
           hole is ordinary minified code, and the crash spent the WHOLE DOCUMENT — every finding of the run,
           in both of the views §What-the-tool-produces names — to report that one sink was undecidable.
           CLAUDE.md §Offensive programming lists an unsolved @S sink among the states that are explicitly NOT
           a `@WHY`: it is the exploration surface, not a broken invariant of this file's own logic.
           NAMED RESIDUAL — WHAT IS NOT COVERED is a witness carrying such a `/` before the locator; this file
           constructs nothing for it, while an escape may well exist. WHAT THE NEXT DIFF BUILDS is the seam to
           the parser that already answers it: `js_parse_regexp` in engine/qjs's quickjs.c, whose callers are
           `js_parse_drive` (at the `parse_regexp` label its expression entry reaches from a `/` or a
           `TOK_DIV_ASSIGN` token) and `js_parse_skip_parens_token` — TWO of them, and `next_token` is not one,
           which is the whole reason a lexer cannot answer this. Both names were grepped in that file at the
           revision this landed and both are present; `js_parse_primary_expr`, which an older spelling of this
           site named and solve_js.h named until now, occurs there ZERO times, so grep them again before
           building against them — nothing here is notified when the submodule moves. HOW ITS ABSENCE SHOWS is
           a search parked at `probes == payloads`, which solve.h currently reads as the single positive
           statement that the source's percent-encode set makes the escape unsatisfiable — the one record
           spelling this answer and the JS_NOT_A_SCRIPT one below now also land in. */
        return 0;
    case JS_NOT_A_SCRIPT:
        /* NO ESCAPE, BECAUSE THE PROGRAM NEVER RUNS. The witness is the whole text a code-execution sink was
           handed — quickjs's js_eval_program_source announces it for §19.2.1.1 PerformEval's direct and
           indirect spellings alike, and for §20.2.1.1.1 CreateDynamicFunction it announces the SYNTHESIZED
           `(function anonymous(…){…})` source rather than the body, so every route delivers a complete
           program text. A text that is not one — an unterminated §12.9.4 string literal, an unterminated
           §12.4 MultiLineComment, a `}` closing a brace nothing opened, a code point §12 admits nowhere — is
           a SyntaxError the sink throws BEFORE evaluating anything, so no bytes written at the hole can fire
           and there is nothing for a search to converge on. §@S accepts nothing but firing as proof, and here
           nothing can fire.
           THE CRASH THAT STOOD HERE READ THIS AS A JOINING PROBLEM — its own closing words, no standard's,
           were `the page built this string out of parts this derivation has not been shown how to join` —
           which is a claim about a FRAGMENT. The witness is never a fragment, for the reason above, so the
           sentence described a case that does not arrive.
           SOUND IN THE DIRECTION THAT COSTS A CANDIDATE AND NEVER IN THE ONE THAT INVENTS ONE, which is what
           makes zero safe here: where this scan reaches JS_NOT_A_SCRIPT on a text that IS a Script, the
           search is left ordered where it was, and where it under-reads nothing is claimed. */
        return 0;
    }
    /* A PENDING ESCAPE SEQUENCE IS PAID OFF BEFORE THE STATE IS LEFT, and the two spellings of that debt are
       one fact — see the Hole declaration. A BACKSLASH the hole stands directly after is satisfied by exactly
       one character, and `n` is the one that consumes nothing beyond itself: §12.9.4 `SingleEscapeCharacter ::
       one of ' " \ b f n r t v` lists it, and it is neither `x`, `u`, nor a DecimalDigit, so no
       Hex/Unicode/LegacyOctal sequence starts. Inside a §12.9.5 literal the same byte is §22.2.1's
       ControlEscape, so one filler serves every state that admits a backslash. DIGITS the hole stands inside
       are paid in `0`, a HexDigit under §12.9.3's own definition, and the `}` §12.9.4's `u{ CodePoint }` still
       owes closes the sequence after them. */
    DCHECK(!h.esc || h.st == JS_STR_SINGLE || h.st == JS_STR_DOUBLE || h.st == JS_TEMPLATE ||
           h.st == JS_RE_BODY || h.st == JS_RE_CLASS,   /* never _FIRST: a backslash before it IS a first char */
           "a pending escape sequence was reported for a §12 state that has no backslash production — only "
           "12.9.4's string characters, 12.9.6's template characters and 12.9.5's body and class admit one, so "
           "a comment or a bare source position claiming one means a scanner set the bit on the wrong hole");
    DCHECK((!h.owed && !h.close) || h.st == JS_STR_SINGLE || h.st == JS_STR_DOUBLE || h.st == JS_TEMPLATE,
           "a MULTI-character escape debt was reported for a §12 state that has no multi-character escape "
           "sequence — §12.9.4's Hex/UnicodeEscapeSequence reaches only a StringLiteral and, through "
           "§12.9.6's TemplateEscapeSequence, a Template, while §12.9.5's `RegularExpressionBackslashSequence "
           ":: \\ RegularExpressionNonTerminator` is one code point and can hold no hole inside it");
    DCHECK(h.owed >= 0 && h.owed <= 4,
           "a §12.9.4 escape sequence was reported owing more HexDigits than any of them has — `Hex4Digits :: "
           "HexDigit HexDigit HexDigit HexDigit` is the widest, `HexEscapeSequence :: x HexDigit HexDigit` is "
           "two and `u{ CodePoint }` is asked for one, so a larger debt means esc_owed measured the span "
           "against a different string than the hole was found in");
    {
        int k = 0;

        if (h.esc) pend[k++] = 'n';
        for (owed = h.owed; owed > 0; owed--) pend[k++] = '0';
        if (h.close) pend[k++] = '}';
        pend[k] = '\0';
    }
    DCHECK(escape[0] != NULL,
           "a §12 state named by the scan reached the emitter with neither an escape nor a refusal — every arm "
           "above either fills the first slot or returns, so an empty one means a state was added to HoleState "
           "and given no answer, and the breakout emitted would be whatever the stack held");
    for (i = 0; i < 2; i++)
        if (escape[i]) emit_one(emit, user, &n, d, "%s%s", pend, escape[i]);
    return n;
}

int solve_js_breakouts(const char *output, const SolveDelivered *d, SolveJsEmit emit, void *user) {
    size_t loclen = sizeof SOLVE_JS_LOCATOR - 1, olen;
    const char *p;
    int n = 0;

    DCHECK(output != NULL && emit != NULL && d != NULL,
           "the JS breakout derivation was asked for the context of nothing, with nowhere to put what it "
           "derives, or with no statement of which bytes this source can carry — the constraint is half the "
           "solve (see solve_js.h), so a derivation without one constructs escapes that cannot arrive");
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
        n += construct(h, d, emit, user);
    }
    return n;
}

/* §@S's CONTEXT-ESCAPED RUNG — see solve_js.h. The scan is the derivation's, unchanged; only the question is
   different, and it is the one JS_SOURCE already answers. `construct` is deliberately not reached: its DFAILs
   name the states this file cannot build an escape OUT of, and being unable to build one is not a reason to
   crash while merely asking whether a candidate already got out. */
int solve_js_at_source(const char *output, size_t at) {
    Scan z;
    Hole h;

    DCHECK(output != NULL, "the @S context-escape rung was asked about the lexical state of nothing");
    z.s = output; z.n = strlen(output); z.at = at;
    z.prev = PREV_NONE; z.sol = 1; z.brace = 0; z.tpl = NULL; z.ntpl = z.tplcap = 0;
    DCHECK(at < z.n,
           "the @S context-escape rung was asked about an offset past the end of the sink's own argument — the "
           "caller found its marker INSIDE that string, so an offset outside it is one computed against a "
           "different string entirely");
    DCHECK((output[at] >= 'a' && output[at] <= 'z') || (output[at] >= 'A' && output[at] <= 'Z') ||
           (output[at] >= '0' && output[at] <= '9'),
           "the @S context-escape rung was asked about an offset whose byte is not ASCII alphanumeric — every "
           "scanner here reads that byte as an ordinary character of whatever state it is in, and a byte that "
           "could open a string, a comment or an escape would change the very scan being used to measure it. "
           "It is the same requirement SOLVE_JS_LOCATOR is built around; the fire marker satisfies it, and a "
           "caller passing anything else is addressing the scan with something that is not a marker");
    h = scan(&z);
    free(z.tpl);
    return h.st == JS_SOURCE;
}
