/* XML 1.0 (Fifth Edition) §2.2 Characters, §2.3 Common Syntactic Constructs' [3] `S`, and §2.11 End-of-Line
 * Handling — THE LAYER EVERY OTHER XML PRODUCTION READS ITS INPUT THROUGH.
 *
 * WHAT IT IS. Three sentences of the standard that together decide what a character of an XML entity IS before
 * any grammar looks at one: §2.2's [2] `Char ::= #x9 | #xA | #xD | [#x20-#xD7FF] | [#xE000-#xFFFD] |
 * [#x10000-#x10FFFF]`, §2.3's [3] `S ::= (#x20 | #x9 | #xD | #xA)+`, and §2.11's requirement that a processor
 * "MUST behave as if it normalized all line breaks in external parsed entities (including the document entity)
 * on input, before parsing, by translating both the two-character sequence #xD #xA and any #xD that is not
 * followed by #xA to a single #xA character". It sits BELOW core/xml/xml_name.h: a `Name`'s character classes
 * are a subset of [2] Char, and both files transcribe normative range tables, which is why the range type and
 * the walk that holds a table to the standard's own order are declared HERE and used there — one assert, one
 * transcription discipline, for both productions.
 *
 * WHY A READER AND NOT A NORMALIZING COPY, because §2.11 says "as if" and the two readings are not equivalent
 * in this engine. A pass that rewrote the entity into a second buffer would (a) allocate a whole second copy of
 * every document, and (b) — the half that actually matters — make every position a well-formedness report
 * carries name an offset into a buffer no caller owns, so a `parsererror` would point at a document the author
 * never wrote. A reader normalizes at the one place a character is produced and hands back the position IN THE
 * ENTITY, which is the position a report has to quote.
 *   IT IS ALSO WHY THE READER IS A POD POSITION AND NOTHING ELSE. CLAUDE.md §scheduler requires that a parse
 * over attacker-length input suspend and resume at any point, and core/xml/xml_ns.h states the same obligation
 * from the scope stack's side. A struct of a cursor, two counters and a latch is copied to PEEK (save it, read
 * ahead, assign it back) and copied to PARK, with no allocation, no interior pointers of its own and no arena —
 * so there is nothing here for a forked arm to share and nothing to re-point. The decoder's own context is a
 * per-read LOCAL for exactly that reason: this reader is handed a whole entity, so no character is ever split
 * across a call and there is no mid-sequence state for the position to carry.
 *
 * TWO FATAL ERRORS, AND THEY ARE RETURNED RATHER THAN ASSERTED. §1.2 Terminology defines a fatal error as "an
 * error which a conforming XML processor MUST detect and report to the application", after which it "MUST NOT
 * continue normal processing". A malformed document is a page's INPUT — CLAUDE.md §Offensive-programming's own
 * carve-out — so a violation is a value this returns, exactly as core/xml/xml_ns.h returns its namespace
 * constraints. The DIFFERENCE from that file is the consequence and it is asserted here rather than described:
 * a namespace constraint is REPORTED and resolution continues, while a fatal error ENDS the parse, so this
 * reader latches its error and a read after one is a DCHECK — a caller that reads on has not stopped, and §1.2
 * says it must.
 *
 * ILL-FORMED UTF-8 IS AN INPUT HERE AND AN ENGINE BUG IN xml_name.c, AND THE OPPOSITE VERDICT IS THE DEFECT IN
 * EITHER DIRECTION. That file DCHECKs on a bad sequence because its bytes are the JS string encoder's output,
 * so a broken one is this engine's own mistake. These bytes came off the NETWORK, and §4.3.3 Character Encoding
 * in Entities says "it is a fatal error if an entity encoded in UTF-8 contains any ill-formed code unit
 * sequences, as defined in section 3.9 of Unicode". Same byte pattern, opposite answer, decided by provenance:
 * a DCHECK on network bytes would crash the engine on a page's input, and a returned error on engine bytes
 * would swallow an engine bug.
 *
 * WHAT IT DOES NOT DO: DECIDE THE ENCODING. Its input is UTF-8. §4.3.3's determination — the higher-level
 * protocol's charset, the Byte Order Mark, the XMLDecl's `encoding=` — happens before a character is read, and
 * the BOM, being "an encoding signature, not part of either the markup or the character data of the XML
 * document", is consumed by that step. That is a PRECONDITION and it is asserted at init rather than assumed,
 * because a BOM left in front of the document entity would otherwise be read as a character of the prolog and
 * the failure would surface as a grammar error about a document that has none.
 *
 * WHY #xD IS IN [3] `S` EVEN THOUGH §2.11 REMOVES EVERY LITERAL ONE. §2.3's own note answers it: "all #xD
 * characters literally present in an XML document are either removed or replaced by #xA characters before any
 * other processing is done. The only way to get a #xD character to match this production is to use a character
 * reference in an entity value literal." So a `S` predicate that dropped #xD would be right about everything
 * this reader produces and wrong about the one case §4.1's references produce — and the reference layer is
 * above this one, which is exactly why the predicate must be complete here.
 *
 * THE LINE AND COLUMN ARE THIS COMPONENT'S BECAUSE §2.11 IS WHAT DEFINES A LINE BREAK. A CRLF is ONE break and
 * a bare CR is one, so a counter above the normalization would answer differently from the document the parser
 * actually read, and a counter below it would be counting bytes. Both are 1-based, which is what a `parsererror`
 * quotes. */
#ifndef APICLIENT_XML_CHAR_H
#define APICLIENT_XML_CHAR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* WHICH FATAL ERROR THIS LAYER DETECTED. One value per sentence of the standard, for xml_ns.h's reason: an
   ill-formed byte sequence and a legal sequence encoding an illegal character are different things an author
   has to be told apart. Zero is OK so a caller may write `if (err)`. */
typedef enum {
    XML_CHAR_OK = 0,
    XML_CHAR_ERR_ILL_FORMED_UTF8,   /* §4.3.3: an entity encoded in UTF-8 holding an ill-formed code unit sequence */
    XML_CHAR_ERR_NOT_A_CHAR         /* §2.2: a code point outside [2] Char */
} XmlCharError;

/* The sentence violated, for the well-formedness error record to report. Never NULL — XML_CHAR_OK has a message
   too, and a caller that formats it has asked the wrong question. */
const char *xml_char_error_message(XmlCharError err);

/* THE END OF THE ENTITY, as a character. A tokenizer's states are written over "the next character" and one of
   the answers is that there is not one — HTML §13.2.5's tokenizer spells that the same way, with an EOF that
   states reason about. It is above [2] Char's ceiling of #x10FFFF, so no predicate here can confuse the two,
   and reading at the end yields it again without moving. */
#define XML_CHAR_EOF ((uint32_t)0xFFFFFFFFu)

/* A CODE-POINT RANGE OF A NORMATIVE PRODUCTION, and the walk that reads one. Declared here rather than in
   whichever file needs a table first, because §2.2's [2] Char and §2.3's [4]/[4a] name classes are transcribed
   by hand from the standard's own text and share one discipline: strictly ascending, non-overlapping, in the
   order printed. `xml_char_in_ranges` asserts exactly that on every lookup, so a transposed or duplicated row —
   which no compiler can catch and whose only symptom would be a code point silently changing class — fires at
   the first read instead. The walk is LINEAR and not a binary search for the same reason: it depends on no
   ordering, so the assert is the only thing that has an opinion about order. */
typedef struct { uint32_t lo, hi; } XmlCpRange;
#define XML_CP_RANGE_N(t) (sizeof(t) / sizeof((t)[0]))
bool xml_char_in_ranges(const XmlCpRange *r, size_t n, uint32_t cp);

/* §2.2 [2] `Char`. Note what it EXCLUDES and why the exclusions matter one layer up: the surrogate blocks
   (D800-DFFF), #xFFFE and #xFFFF, and every C0 control except tab, line feed and carriage return — so NUL is
   not a character an XML document may contain, and a document holding one is fatally in error rather than
   truncated at it. */
bool xml_char_is_char(uint32_t cp);

/* ONE CHARACTER OF §2.3 [3] `S`. The production is `(...)+`, one or more; whether a RUN of them is required or
   optional is each grammar rule's own business, so this answers about a single character and the callers that
   need the `+` write the loop. See the head comment for why #xD is in it. */
bool xml_char_is_s(uint32_t cp);

/* THE READER. Treat it as a VALUE: the only ways to move it are the calls below, and the only supported way to
   rewind it is to assign back a copy this component produced. It is exposed rather than opaque because peeking
   and parking ARE copies — an accessor pair would buy nothing and cost the caller a heap allocation.
     `line` and `column` are 1-based and count NORMALIZED characters, so `column` advances by one per character
   however many bytes encoded it. `fatal` is the §1.2 latch: once non-OK the parse is over. */
typedef struct {
    const char  *start;   /* the entity's first byte, so a position can be turned back into an offset */
    const char  *p;       /* the next byte to read */
    const char  *end;     /* one past the entity's last byte */
    size_t       line;
    size_t       column;
    XmlCharError fatal;
} XmlCharReader;

/* `s` is `len` bytes of the entity, ALREADY in UTF-8 and with §4.3.3's encoding signature removed — see the
   head comment. `s` must be a valid pointer even when `len` is zero: an empty entity is a document that ends
   immediately, which is a thing this reader answers about, and not the absence of one. */
void xml_char_reader_init(XmlCharReader *r, const char *s, size_t len);

/* READ ONE CHARACTER, with §2.11 already applied and §2.2 and §4.3.3 already decided.
   `*out` is written ONLY when XML_CHAR_OK is returned — a half-filled out-parameter is a producer's field
   defaulted, which is worse than no answer. At the end of the entity the answer is XML_CHAR_OK with `*out`
   XML_CHAR_EOF.
   ON A FATAL ERROR THE READER DOES NOT ADVANCE, which is what makes `line`/`column` name the offending
   character rather than the one after it, and the error is latched: reading again is a DCHECK, because §1.2
   requires the processor to stop. */
XmlCharError xml_char_read(XmlCharReader *r, uint32_t *out);

#endif
