/* See xml_pseudo_attr.h. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/xml/xml_char.h"
#include "core/xml/xml_name.h"
#include "core/xml/xml_pseudo_attr.h"
#include "core/xml/xml_ref.h"

const char *xml_pseudo_attr_error_message(XmlPseudoAttrError err)
{
    switch (err) {
    case XML_PSEUDO_OK:
        return "the string is matched by [1a] PseudoAtts";
    case XML_PSEUDO_ERR_GRAMMAR:
        return "xml-stylesheet §3 Pseudo-attributes: the string is not matched by the PseudoAtts production";
    case XML_PSEUDO_ERR_LEGAL_CHARACTER:
        return "xml-stylesheet §3 Pseudo-attributes: a CharRef violates the XML Legal Character "
               "well-formedness constraint";
    case XML_PSEUDO_ERR_DUPLICATE_NAME:
        return "xml-stylesheet §3 Pseudo-attributes: two pseudo-attributes have the same name";
    case XML_PSEUDO_ERR_CHARACTER:
        return "the character layer latched an error — ask xml_char_error_message about the reader";
    }
    DFAIL("a pseudo-attribute parse error outside the enumeration was formatted — every value of the enum has "
          "a sentence above, so a value with none is a value nothing in this component produces");
    return "";
}

/* A GROWABLE UTF-8 RUN. Both strings a pseudo-attribute carries are BUILT rather than borrowed (see the
   header), and both are built the same way, so the buffer is one thing rather than two spellings. */
typedef struct { char *p; size_t n, cap; } PaBuf;

/* MAKE ROOM FOR `more` MORE BYTES AND THE TERMINATOR. Called with `more` == 0 by the value parser before it
   reads anything, so that an EMPTY value — `a=""`, which [3] matches with zero repetitions — comes back as a
   pointer to an empty string rather than as NULL. A NULL there would be a producer's field defaulted: the
   caller cannot tell it from "no value was parsed", and the two are different facts. */
static void pa_buf_grow(PaBuf *b, size_t more)
{
    size_t want = b->n + more + 1;
    char *q;

    if (want <= b->cap) return;
    {
        size_t cap = b->cap ? b->cap : 32;
        while (cap < want) {
            CHECK(cap <= (size_t)-1 / 2, "a pseudo-attribute run overflowed the size type while growing — the "
                                         "string being parsed is larger than this address space can describe");
            cap *= 2;
        }
        q = (char *)realloc(b->p, cap);
        CHECK(q != NULL, "OOM building a pseudo-attribute's name or value — a dropped write is a processing "
                         "instruction whose attribute map disagrees with its own data");
        b->p = q;
        b->cap = cap;
    }
    b->p[b->n] = 0;
}

/* APPEND ONE CHARACTER, through core/xml/xml_char.h's encoder for the reason that file states: the characters
   this grammar produces are not the bytes the entity holds, so they have to be spelled back out. Every code
   point that reaches here has been established to be [2] Char — a literal one by the reader that read it, a
   referenced one by xml_ref_scan's [WFC: Legal Character] — which is the encoder's own precondition. */
static void pa_buf_put(PaBuf *b, uint32_t cp)
{
    pa_buf_grow(b, XML_CHAR_ENCODE_MAX);
    b->n += xml_char_encode(cp, b->p + b->n);
    b->p[b->n] = 0;
}

static void pa_buf_free(PaBuf *b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

void xml_pseudo_attrs_free(XmlPseudoAttrs *a)
{
    size_t i;

    if (a == NULL) return;
    for (i = 0; i < a->n; i++) {
        free(a->items[i].name);
        free(a->items[i].value);
    }
    free(a->items);
    a->items = NULL;
    a->n = a->cap = 0;
}

/* THE ERROR ARM, IN ONE PLACE. The header promises `*out` is left valid and empty on every error, and a
   component that promised that at six return sites would eventually not. */
static XmlPseudoAttrError pa_fail(XmlPseudoAttrs *out, XmlPseudoAttrError err)
{
    DCHECK(err != XML_PSEUDO_OK, "the pseudo-attribute parse failed with the success value, so a caller "
                                 "testing `if (err)` would read a discarded result as a parsed one");
    xml_pseudo_attrs_free(out);
    return err;
}

/* READ ONE CHARACTER, ANSWERING WHETHER THE CHARACTER LAYER LATCHED. `*cp` is XML_CHAR_EOF at the end of the
   string, which every caller here tests before anything else — the grammar's productions all end somewhere,
   so running out of string is a GRAMMAR answer at every site but the two where [1a] permits it. */
static bool pa_read(XmlCharReader *r, uint32_t *cp)
{
    return xml_char_read(r, cp) == XML_CHAR_OK;
}

/* TAKE `b`'S BYTES, LEAVING THE BUFFER EMPTY. The run becomes the pseudo-attribute's, so exactly one of the
   two owns it at every instant. */
static void pa_take(PaBuf *b, char **p, size_t *n)
{
    DCHECK(b->p != NULL, "a pseudo-attribute run was taken before it was allocated — every producer of one "
                         "calls pa_buf_grow first so that an empty run is an empty string and never NULL");
    *p = b->p;
    *n = b->n;
    b->p = NULL;
    b->n = b->cap = 0;
}

/* [2] PseudoAtt ::= Name S? "=" S? PseudoAttValue — appended to `out` on success, which is also where the
   duplicate-name sentence is enforced, since that is the first moment a name exists to compare. */
static XmlPseudoAttrError pa_one(XmlCharReader *r, XmlPseudoAttrs *out)
{
    PaBuf name = { NULL, 0, 0 }, value = { NULL, 0, 0 };
    XmlPseudoAttrError err = XML_PSEUDO_ERR_GRAMMAR;
    XmlCharReader save;
    uint32_t cp = XML_CHAR_EOF, quote;
    size_t i;

    /* [5] Name — one NameStartChar then NameChar*, asked of core/xml/xml_name.h so that this grammar and every
       other XML production in this engine decide a name with one implementation. */
    if (!pa_read(r, &cp)) return XML_PSEUDO_ERR_CHARACTER;
    if (cp == XML_CHAR_EOF || !xml_name_is_name_start_char(cp)) return XML_PSEUDO_ERR_GRAMMAR;
    pa_buf_grow(&name, 0);
    pa_buf_put(&name, cp);
    for (;;) {
        save = *r;
        if (!pa_read(r, &cp)) { err = XML_PSEUDO_ERR_CHARACTER; goto done; }
        if (cp != XML_CHAR_EOF && xml_name_is_name_char(cp)) { pa_buf_put(&name, cp); continue; }
        *r = save;
        break;
    }
    /* S? "=" S? — the two optional runs are the same loop written twice because they are two positions in the
       production, and `=` is the only thing between them. */
    for (;;) {
        save = *r;
        if (!pa_read(r, &cp)) { err = XML_PSEUDO_ERR_CHARACTER; goto done; }
        if (cp != XML_CHAR_EOF && xml_char_is_s(cp)) continue;
        *r = save;
        break;
    }
    if (!pa_read(r, &cp)) { err = XML_PSEUDO_ERR_CHARACTER; goto done; }
    if (cp != 0x3D) { err = XML_PSEUDO_ERR_GRAMMAR; goto done; }   /* U+003D EQUALS SIGN */
    for (;;) {
        save = *r;
        if (!pa_read(r, &cp)) { err = XML_PSEUDO_ERR_CHARACTER; goto done; }
        if (cp != XML_CHAR_EOF && xml_char_is_s(cp)) continue;
        *r = save;
        break;
    }
    /* [3] PseudoAttValue. The two alternatives differ only in which quote delimits them and therefore which
       quote the content excludes, so the quote is READ and then carried — writing the alternatives out twice
       would be two places for [^"<&] to lose a character. "The first and last character (the start and end
       quotes) are removed", which is why neither is appended. */
    if (!pa_read(r, &quote)) { err = XML_PSEUDO_ERR_CHARACTER; goto done; }
    if (quote != 0x22 && quote != 0x27) { err = XML_PSEUDO_ERR_GRAMMAR; goto done; }
    pa_buf_grow(&value, 0);
    for (;;) {
        save = *r;
        if (!pa_read(r, &cp)) { err = XML_PSEUDO_ERR_CHARACTER; goto done; }
        if (cp == XML_CHAR_EOF) { err = XML_PSEUDO_ERR_GRAMMAR; goto done; }   /* nobody closed the value */
        if (cp == quote) break;
        /* [^"<&] EXCLUDES U+003C IN BOTH ALTERNATIVES, not only in the one whose quote it is not. A `<` inside
           a pseudo-attribute value is a grammar error however the value is quoted. */
        if (cp == 0x3C) { err = XML_PSEUDO_ERR_GRAMMAR; goto done; }
        if (cp == 0x26) {   /* U+0026 AMPERSAND: [66] CharRef or [4] PredefEntityRef */
            XmlRef ref;
            XmlRefError re;

            /* xml_ref_scan requires the reader to STAND ON the `&`, which is where `save` is. */
            *r = save;
            re = xml_ref_scan(r, &ref);
            if (re == XML_REF_ERR_LEGAL_CHARACTER) { err = XML_PSEUDO_ERR_LEGAL_CHARACTER; goto done; }
            if (re == XML_REF_ERR_CHARACTER) { err = XML_PSEUDO_ERR_CHARACTER; goto done; }
            if (re != XML_REF_OK) { err = XML_PSEUDO_ERR_GRAMMAR; goto done; }
            /* [3] ADMITS EXACTLY TWO OF [67]'S THREE READINGS. A general entity reference is well-formed XML
               and is still not in this production, so `a="&foo;"` is a grammar error here while it is an
               ordinary unresolved entity inside a real attribute value. That difference is why this cannot be
               routed through XML §3.3.3's attribute-value normalization. */
            if (ref.kind == XML_REF_ENTITY) { err = XML_PSEUDO_ERR_GRAMMAR; goto done; }
            pa_buf_put(&value, ref.cp);
            continue;
        }
        pa_buf_put(&value, cp);
    }
    /* "The parsing result is an error if there are more than one pseudo-attribute with the same name." Exact
       bytes, never folded — XML §1.2 Terminology's `match` is "Two strings or names being compared are
       identical", which core/xml/xml_ref.h refuses to fold for the same sentence. */
    for (i = 0; i < out->n; i++)
        if (out->items[i].name_len == name.n && memcmp(out->items[i].name, name.p, name.n) == 0) {
            err = XML_PSEUDO_ERR_DUPLICATE_NAME;
            goto done;
        }
    if (out->n == out->cap) {
        size_t cap = out->cap ? out->cap * 2 : 4;
        XmlPseudoAttr *q = (XmlPseudoAttr *)realloc(out->items, cap * sizeof *q);
        CHECK(q != NULL, "OOM growing a processing instruction's pseudo-attribute list");
        out->items = q;
        out->cap = cap;
    }
    pa_take(&name, &out->items[out->n].name, &out->items[out->n].name_len);
    pa_take(&value, &out->items[out->n].value, &out->items[out->n].value_len);
    out->n++;
    return XML_PSEUDO_OK;

done:
    pa_buf_free(&name);
    pa_buf_free(&value);
    DCHECK(err != XML_PSEUDO_OK, "the single-pseudo-attribute parse left the failure path with the success "
                                 "value, so a half-built pair would be reported as parsed");
    return err;
}

XmlPseudoAttrError xml_pseudo_attr_parse(const char *s, size_t len, XmlPseudoAttrs *out)
{
    XmlCharReader r;
    bool first = true;

    DCHECK(out != NULL, "the pseudo-attribute parse was given nowhere to put its result");
    DCHECK(s != NULL, "the pseudo-attribute parse was given a NULL string — an EMPTY string is a thing [1a] "
                      "answers about (every part of it is optional, so it matches) and is spelled with a valid "
                      "pointer and a zero length, never with NULL");
    out->items = NULL;
    out->n = out->cap = 0;

    xml_char_reader_init(&r, s, len);
    /* [1a] PseudoAtts ::= PseudoAtt? ( S PseudoAtt )* S?
       Read as a loop, the production says three things and this loop is those three: S may PRECEDE the first
       pseudo-attribute only by way of the `( S PseudoAtt )*` arm with `PseudoAtt?` empty, S MUST separate two
       of them, and S may TRAIL the last. The middle one is the whole reason `first` exists — a plain
       skip-whitespace-then-parse loop accepts `a="1"b="2"`, which [1a] does not match. */
    for (;;) {
        size_t ws = 0;
        XmlCharReader save;
        uint32_t cp;

        for (;;) {
            save = r;
            if (!pa_read(&r, &cp)) return pa_fail(out, XML_PSEUDO_ERR_CHARACTER);
            if (cp != XML_CHAR_EOF && xml_char_is_s(cp)) { ws++; continue; }
            r = save;
            break;
        }
        save = r;
        if (!pa_read(&r, &cp)) return pa_fail(out, XML_PSEUDO_ERR_CHARACTER);
        r = save;
        if (cp == XML_CHAR_EOF) return XML_PSEUDO_OK;   /* the trailing `S?`, and the empty string */
        if (!first && ws == 0) return pa_fail(out, XML_PSEUDO_ERR_GRAMMAR);
        {
            XmlPseudoAttrError err = pa_one(&r, out);
            if (err != XML_PSEUDO_OK) return pa_fail(out, err);
        }
        first = false;
    }
}
