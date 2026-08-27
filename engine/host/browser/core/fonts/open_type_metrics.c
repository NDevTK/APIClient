/* The OpenType metrics tables. See open_type_metrics.h for the three questions this answers, and for why a
   claim the BYTES make is an `if` that rejects while an invariant THIS FILE established is a DCHECK. */
#include "core/fonts/open_type_metrics.h"

#include <string.h>

#include "check.h"

/* A four-character Tag as the uint32 OpenType "The OpenType Font File" §Data Types defines it — "array of four
   uint8s ... used to identify a table", compared as a number so that "the records in the array must be sorted
   in ascending order by tag" is one integer comparison and not a memcmp with an ordering nobody stated. */
#define OT_TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* THE ONE PLACE A REJECTION IS WRITTEN. `reject` is a positive statement of which rule the bytes broke, so
   nothing downstream has to infer it from a bare false — the same reason a consumer never defaults a
   producer's field. Every literal handed to it names the SECTION as well as the symptom, because the reader of
   a rejected font is deciding whether the font is hostile or this parser is wrong. */
#define OT_REJECT(m, why) do { (m)->reject = (why); return false; } while (0)

/* DOES `n` BYTES AT `off` LIE INSIDE THE FACE — written as a subtraction rather than as `off + n <= len` so
   that a hostile `off` near SIZE_MAX cannot wrap the addition into a pass. Every caller below either rejects
   on a false or has already proved it and reads through the accessors, which assert it again. */
static bool ot_fits(const OpenTypeMetrics *m, size_t off, size_t n)
{
    return n <= m->len && off <= m->len - n;
}

static uint16_t ot_u16(const OpenTypeMetrics *m, size_t off)
{
    DCHECK(ot_fits(m, off, 2),
           "a 16-bit OpenType field was read outside the face. Every read below open_type_metrics_read's "
           "validation is inside an extent that validation already proved fits, so this is not a hostile font "
           "— it is a missing or wrong extent check in this file, and the offset in the frame above names "
           "which table's validation is short");
    return (uint16_t)(((uint16_t)m->sfnt[off] << 8) | (uint16_t)m->sfnt[off + 1]);
}

static uint32_t ot_u32(const OpenTypeMetrics *m, size_t off)
{
    DCHECK(ot_fits(m, off, 4),
           "a 32-bit OpenType field was read outside the face — as for the 16-bit reader above, that is this "
           "file's own extent bookkeeping and not the font's");
    return ((uint32_t)m->sfnt[off] << 24) | ((uint32_t)m->sfnt[off + 1] << 16) |
           ((uint32_t)m->sfnt[off + 2] << 8) | (uint32_t)m->sfnt[off + 3];
}

/* ---- the table directory ---------------------------------------------------------------------------------- */

/* ONE TABLE RECORD, AS FOUND. `len` is the record's own `length` and NOT the padded extent: OpenType says
   "the length of each table should be recorded in the table record with the actual length of data, not the
   padded length", so a validation done against the padded value would accept a table whose last bytes are
   another table's. */
typedef struct { size_t off, len; bool found; } OtTable;

static OtTable ot_find(const OpenTypeMetrics *m, uint16_t num_tables, uint32_t tag)
{
    OtTable t = { 0, 0, false };
    uint16_t i;

    for (i = 0; i < num_tables; i++) {
        size_t rec = 12 + (size_t)16 * i;
        if (ot_u32(m, rec) != tag) continue;
        t.off = ot_u32(m, rec + 8);
        t.len = ot_u32(m, rec + 12);
        t.found = true;
        return t;
    }
    return t;
}

/* ---- 'cmap' subtable validation --------------------------------------------------------------------------- */

/* WHY THE WHOLE SUBTABLE IS WALKED HERE RATHER THAN CHECKED AT EACH LOOKUP. A 'cmap' maps a character code to a
   glyph ID by ARITHMETIC — Format 4 adds a per-segment `idDelta` modulo 65536, Format 12 adds an offset into a
   `startGlyphID` — so a hostile (or merely broken) subtable can name a glyph this face does not have, and it
   can do it for one codepoint out of a million. Checking at the lookup would leave two bad choices at the site:
   clamp (a wrong advance with nothing able to see it) or crash on a page's font (a denial of service handed to
   whoever serves the font). Walking the coverage ONCE at read time removes the choice: after this returns
   true, every glyph ID the subtable can produce is below `num_glyphs`, so the lookup is total and its bound is
   a DCHECK about this file rather than an `if` about the font. The walk is O(coverage) and coverage is at most
   the Unicode scalar space, once, at startup or at the end of a font fetch. */

static bool ot_cmap4_validate(OpenTypeMetrics *m, size_t sub, size_t sub_len)
{
    size_t end_a, start_a, delta_a, range_a, gia, gia_end;
    uint32_t seg_count_x2, seg_count, i;
    uint32_t prev_end = 0;
    bool first = true;

    /* 14 bytes of header, then endCode[segCount], reservedPad, startCode[segCount], idDelta[segCount],
       idRangeOffset[segCount], then glyphIdArray[] to the end of the subtable. */
    if (sub_len < 16) OT_REJECT(m, "a 'cmap' Format 4 (Segment mapping to delta values) subtable is shorter "
                                   "than its own fixed header plus the reservedPad the format places after "
                                   "endCode — OpenType 'cmap' — Character to Glyph Index Mapping Table");
    seg_count_x2 = ot_u16(m, sub + 6);
    if ((seg_count_x2 & 1) != 0)
        OT_REJECT(m, "a 'cmap' Format 4 subtable's segCountX2 is ODD. The field is defined as \"2 x segCount\" "
                     "by OpenType 'cmap' §Format 4: Segment mapping to delta values, so an odd value names no "
                     "segment count at all and the four parallel arrays it sizes cannot be laid out");
    seg_count = seg_count_x2 / 2;
    if (seg_count == 0)
        OT_REJECT(m, "a 'cmap' Format 4 subtable has ZERO segments. §Format 4 requires a final segment mapping "
                     "0xFFFF — \"For the search to terminate, the final startCode and endCode values must be "
                     "0xFFFF ... However, the segment must be present\" — so a segment count of zero is a "
                     "search with no terminator");
    if (sub_len < 16 + (size_t)8 * seg_count)
        OT_REJECT(m, "a 'cmap' Format 4 subtable's own `length` is too small for the four parallel arrays its "
                     "segCountX2 declares. §Format 4 lays out endCode[segCount], reservedPad, "
                     "startCode[segCount], idDelta[segCount] and idRangeOffset[segCount] after a 14-byte "
                     "header, so the arrays alone need 16 + 8*segCount bytes");

    end_a = sub + 14;
    start_a = end_a + 2 * (size_t)seg_count + 2;   /* the reservedPad §Format 4 places between the two arrays */
    delta_a = start_a + 2 * (size_t)seg_count;
    range_a = delta_a + 2 * (size_t)seg_count;
    gia = range_a + 2 * (size_t)seg_count;
    gia_end = sub + sub_len;

    for (i = 0; i < seg_count; i++) {
        uint32_t end = ot_u16(m, end_a + 2 * (size_t)i);
        uint32_t start = ot_u16(m, start_a + 2 * (size_t)i);
        uint16_t delta = ot_u16(m, delta_a + 2 * (size_t)i);
        uint32_t range_off = ot_u16(m, range_a + 2 * (size_t)i);
        uint32_t c;

        /* "The segments are sorted in order of increasing endCode values" (§Format 4), and a character code
           belongs to the FIRST segment whose endCode reaches it — so two segments that overlap would make the
           mapping depend on the search's implementation rather than on the font. Strictly increasing endCodes
           and a startCode past the previous endCode is that sentence made checkable. */
        if (!first && end <= prev_end)
            OT_REJECT(m, "a 'cmap' Format 4 subtable's segments are not in increasing endCode order. OpenType "
                         "§Format 4: Segment mapping to delta values says \"the segments are sorted in order "
                         "of increasing endCode values\" and the lookup is \"the first endCode that is greater "
                         "than or equal to the character code\" — so an out-of-order or repeated endCode makes "
                         "which glyph a codepoint maps to a property of the search rather than of the font");
        if (!first && start <= prev_end)
            OT_REJECT(m, "a 'cmap' Format 4 subtable has OVERLAPPING segments — a startCode at or below the "
                         "previous segment's endCode. §Format 4's four parallel arrays describe \"one segment "
                         "for each contiguous range of codes\", and a code inside two of them has two glyph "
                         "IDs with nothing in the format to choose between them");
        if (start > end)
            OT_REJECT(m, "a 'cmap' Format 4 subtable has a segment whose startCode is ABOVE its endCode. "
                         "§Format 4 describes each segment as a range \"described by a startCode and endCode\", "
                         "and an empty-by-inversion range is not one");
        first = false;
        prev_end = end;

        for (c = start; c <= end; c++) {
            uint32_t glyph;
            if (range_off == 0) {
                /* "If the idRangeOffset is 0, the idDelta value is added directly to the character code offset
                   (i.e. idDelta[i] + c) to get the corresponding glyph index. Again, the idDelta arithmetic is
                   modulo 65536." (§Format 4) */
                glyph = (uint32_t)(uint16_t)(c + delta);
            } else {
                /* §Format 4's own expression: glyphId = *(idRangeOffset[i]/2 + (c - startCode[i]) +
                   &idRangeOffset[i]). The addend is a BYTE offset from the address of idRangeOffset[i] itself,
                   which is the "obscure indexing trick" the section names — and it is exactly the shape a
                   hostile font aims at, so the resulting address is checked against the glyphIdArray extent
                   the subtable's own `length` fixes rather than against the whole face. */
                size_t at = range_a + 2 * (size_t)i + range_off + 2 * (size_t)(c - start);
                if (at < gia || at + 2 > gia_end)
                    OT_REJECT(m, "a 'cmap' Format 4 subtable's idRangeOffset indexes OUTSIDE its own "
                                 "glyphIdArray. §Format 4 defines the index as an offset from the address of "
                                 "idRangeOffset[i], and says the trick \"works because glyphIdArray "
                                 "immediately follows idRangeOffset in the font file\" — an index that leaves "
                                 "the array is a read of whatever the file holds next");
                glyph = ot_u16(m, at);
                /* "If the value obtained from the indexing operation is not 0 (which indicates missingGlyph),
                   idDelta[i] is added to it" — so a zero stays zero and does NOT take the delta. */
                if (glyph != 0) glyph = (uint32_t)(uint16_t)(glyph + delta);
            }
            if (glyph >= m->num_glyphs)
                OT_REJECT(m, "a 'cmap' Format 4 subtable maps a character code to a glyph ID this face does "
                             "not have — at or above 'maxp' — Maximum Profile's numGlyphs. Every table indexed "
                             "by glyph ID is sized by that count, so the mapping names a record outside 'hmtx' "
                             "and outside every other per-glyph array in the file");
        }
    }
    if (prev_end != 0xFFFF)
        OT_REJECT(m, "a 'cmap' Format 4 subtable's final segment does not end at 0xFFFF. OpenType §Format 4: "
                     "Segment mapping to delta values requires it in as many words — \"For the search to "
                     "terminate, the final startCode and endCode values must be 0xFFFF. This segment need not "
                     "contain any valid mappings ... However, the segment must be present\" — and a search "
                     "that runs off the end of the array is what its absence produces");
    if (ot_u16(m, start_a + 2 * (size_t)(seg_count - 1)) != 0xFFFF)
        OT_REJECT(m, "a 'cmap' Format 4 subtable's final segment ends at 0xFFFF but does not START there. "
                     "§Format 4 states both halves — \"the final startCode and endCode values must be 0xFFFF\" "
                     "— and a final segment starting lower silently swallows every code above it into one "
                     "delta");
    return true;
}

static bool ot_cmap12_validate(OpenTypeMetrics *m, size_t sub, size_t sub_len)
{
    uint32_t num_groups, i;
    uint32_t prev_end = 0;
    bool first = true;

    /* format(2) reserved(2) length(4) language(4) numGroups(4), then SequentialMapGroup[numGroups] of 12. */
    if (sub_len < 16)
        OT_REJECT(m, "a 'cmap' Format 12 (Segmented coverage) subtable is shorter than its own 16-byte header "
                     "— OpenType 'cmap' §Format 12: Segmented coverage");
    num_groups = ot_u32(m, sub + 12);
    if (num_groups > (sub_len - 16) / 12)
        OT_REJECT(m, "a 'cmap' Format 12 subtable declares more SequentialMapGroup records than its own "
                     "`length` can hold. §Format 12's `length` is the \"byte length of this subtable "
                     "(including the header)\", so the groups it counts must fit inside it");

    for (i = 0; i < num_groups; i++) {
        size_t g = sub + 16 + (size_t)12 * i;
        uint32_t start = ot_u32(m, g);
        uint32_t end = ot_u32(m, g + 4);
        uint32_t first_glyph = ot_u32(m, g + 8);

        if (start > end)
            OT_REJECT(m, "a 'cmap' Format 12 group's endCharCode is BELOW its startCharCode. §Format 12's "
                         "SequentialMapGroup Record names them \"First character code in this group\" and "
                         "\"Last character code in this group\", which an inverted pair is not");
        if (!first && start <= prev_end)
            OT_REJECT(m, "a 'cmap' Format 12 subtable's groups are not disjoint and increasing. §Format 12 "
                         "states both requirements — \"Groups must be sorted by increasing startCharCode. A "
                         "group's endCharCode must be less than the startCharCode of the following group, if "
                         "any\" — and a code inside two groups has two glyph IDs");
        first = false;
        prev_end = end;

        if (end > 0x10FFFF)
            OT_REJECT(m, "a 'cmap' Format 12 group covers a code above U+10FFFF. §Format 12: Segmented "
                         "coverage is \"the standard character-to-glyph-index mapping subtable for fonts "
                         "supporting Unicode character repertoires that include supplementary-plane "
                         "characters (U+10000 to U+10FFFF)\", so a code outside Unicode's scalar space is not "
                         "a character this subtable can be asked about");
        /* The group maps start..end onto first_glyph..first_glyph+(end-start) contiguously, so the LAST glyph
           is the one to bound — computed in 64 bits because both terms are 32-bit and their sum is not. */
        if ((uint64_t)first_glyph + (uint64_t)(end - start) >= (uint64_t)m->num_glyphs)
            OT_REJECT(m, "a 'cmap' Format 12 group runs off the end of the face's glyphs — its startGlyphID "
                         "plus its own span reaches at or above 'maxp' — Maximum Profile's numGlyphs. "
                         "§Format 12's startGlyphID is the \"glyph index corresponding to the starting "
                         "character code\" and the group is contiguous from there, so the whole run must be "
                         "glyphs this face has");
    }
    return true;
}

/* WHICH SUBTABLE IS THE FACE'S UNICODE MAPPING, in one fixed order, and why this order. OpenType 'cmap'
   §Encoding records and encodings makes two statements that decide it: "If a font includes Unicode subtables
   for both 16-bit encoding (typically, format 4) and also 32-bit encoding (formats 10 or 12), then the
   characters supported by the subtable for 32-bit encoding should be a superset of the characters supported by
   the subtable for 16-bit encoding, and the 32-bit encoding should be used by applications" — so a full
   repertoire subtable outranks a BMP one; and "If a font includes encoding records for Unicode subtables of the
   same format but with different platform IDs, an application may choose which to select, but should make this
   selection consistently each time the font is used" — so the platform tie is broken by this table and by
   nothing else. §Windows platform (platform ID = 3) names encodings 1 (Unicode BMP) and 10 (Unicode full
   repertoire); §Unicode platform (platform ID = 0) names 3 (BMP) and 4 (full repertoire).
   THE FORMAT IS CHECKED AGAINST THE RECORD RATHER THAN TAKEN FROM IT. The pairing above is a "should" in the
   spec's own words, so a record may promise one format and hold another; selecting on the SUBTABLE'S OWN
   `format` field means a mislabelled record is simply not selected instead of being trusted into a wrong
   parse. */
static const struct { uint16_t platform, encoding, format; } OT_CMAP_PREFERENCE[] = {
    { 3, 10, 12 },  /* Windows, Unicode full repertoire */
    { 0,  4, 12 },  /* Unicode, Unicode 2.0 and onwards semantics, Unicode full repertoire */
    { 3,  1,  4 },  /* Windows, Unicode BMP */
    { 0,  3,  4 },  /* Unicode, Unicode 2.0 and onwards semantics, Unicode BMP only */
};

static bool ot_cmap_select(OpenTypeMetrics *m, size_t cmap, size_t cmap_len)
{
    uint16_t n, i;
    unsigned pref;
    uint32_t prev_key = 0;
    bool first = true;

    if (cmap_len < 4)
        OT_REJECT(m, "the 'cmap' — Character to Glyph Index Mapping Table is shorter than its own header "
                     "(`version`, `numTables`)");
    if (ot_u16(m, cmap) != 0)
        OT_REJECT(m, "the 'cmap' table's `version` is not 0. OpenType 'cmap' §'cmap' Header defines the field "
                     "as \"Table version number (0)\" and defines no other value, so a face carrying one is "
                     "making a claim about a format that does not exist");
    n = ot_u16(m, cmap + 2);
    if ((size_t)4 + (size_t)8 * n > cmap_len)
        OT_REJECT(m, "the 'cmap' table declares more EncodingRecords than its own length can hold — §'cmap' "
                     "Header's encodingRecords[numTables] of eight bytes each follows the four-byte header");

    /* "The encoding record entries in the 'cmap' header must be sorted first by platform ID, then by
       platform-specific encoding ID, and then by the language field in the corresponding subtable." The pair
       may REPEAT (the language is the third key and lives in the subtable), so the check is non-decreasing on
       the pair rather than strictly increasing. */
    for (i = 0; i < n; i++) {
        size_t rec = cmap + 4 + (size_t)8 * i;
        uint32_t key = ((uint32_t)ot_u16(m, rec) << 16) | ot_u16(m, rec + 2);
        if (!first && key < prev_key)
            OT_REJECT(m, "the 'cmap' table's EncodingRecords are not sorted. OpenType §Encoding records and "
                         "encodings requires that they \"must be sorted first by platform ID, then by "
                         "platform-specific encoding ID, and then by the language field in the corresponding "
                         "subtable\"");
        first = false;
        prev_key = key;
    }

    for (pref = 0; pref < sizeof(OT_CMAP_PREFERENCE) / sizeof(OT_CMAP_PREFERENCE[0]); pref++) {
        for (i = 0; i < n; i++) {
            size_t rec = cmap + 4 + (size_t)8 * i;
            uint32_t sub_off;
            size_t sub, sub_len;

            if (ot_u16(m, rec) != OT_CMAP_PREFERENCE[pref].platform) continue;
            if (ot_u16(m, rec + 2) != OT_CMAP_PREFERENCE[pref].encoding) continue;
            sub_off = ot_u32(m, rec + 4);
            /* `subtableOffset` is a "byte offset from beginning of table", so it is bounded by the 'cmap'
               extent and not by the file — a subtable reaching outside its own table would make the table's
               `length` in the directory a statement about nothing. */
            if (sub_off > cmap_len || cmap_len - sub_off < 4)
                OT_REJECT(m, "a 'cmap' EncodingRecord's subtableOffset points outside the 'cmap' table. "
                             "§Encoding records and encodings defines it as a \"byte offset from beginning of "
                             "table to the subtable for this encoding\", and OpenType §Organization of an "
                             "OpenType Font requires that a table's recorded length \"measure a contiguous "
                             "range of bytes that encompasses all of the data for a table. This applies to "
                             "any subtables as well as to top-level tables\"");
            sub = cmap + sub_off;
            if (ot_u16(m, sub) != OT_CMAP_PREFERENCE[pref].format) continue;

            /* Format 4's `length` is a uint16 at +2; Format 12's is a uint32 at +4. */
            if (OT_CMAP_PREFERENCE[pref].format == 4) {
                sub_len = ot_u16(m, sub + 2);
            } else {
                if (cmap_len - sub_off < 8)
                    OT_REJECT(m, "a 'cmap' subtable declares Format 12 (Segmented coverage) and then ends "
                                 "inside its own header, before the uint32 `length` at offset 4 that fixes "
                                 "its extent");
                sub_len = ot_u32(m, sub + 4);
            }
            if (sub_len < 4 || sub_len > cmap_len - sub_off)
                OT_REJECT(m, "a 'cmap' subtable's own `length` runs past the end of the 'cmap' table. The "
                             "field is the subtable's whole extent — every offset inside it, including "
                             "Format 4's glyphIdArray, is bounded by it — so a length that leaves the table "
                             "makes every one of those bounds a read of the next table's bytes");

            if (OT_CMAP_PREFERENCE[pref].format == 4) {
                if (!ot_cmap4_validate(m, sub, sub_len)) return false;
            } else {
                if (!ot_cmap12_validate(m, sub, sub_len)) return false;
            }
            m->cmap_format = OT_CMAP_PREFERENCE[pref].format;
            m->cmap_sub = sub;
            m->cmap_sub_len = sub_len;
            return true;
        }
    }
    OT_REJECT(m, "the face has no 'cmap' subtable in a format this engine maps Unicode through. OpenType "
                 "'cmap' §Encoding records and encodings names the Unicode pairs — Windows platform 3 with "
                 "encoding 1 (Unicode BMP, Format 4: Segment mapping to delta values) or encoding 10 (Unicode "
                 "full repertoire, Format 12: Segmented coverage), and Unicode platform 0 with encodings 3 and "
                 "4 — and a face carrying only a Format 0, 2, 6, 10 or 13 subtable maps its characters through "
                 "a format that is not built here. That is a capability to add to OT_CMAP_PREFERENCE and a "
                 "validator beside ot_cmap4_validate, not a face to guess the coverage of");
}

/* ---- read ------------------------------------------------------------------------------------------------- */

bool open_type_metrics_read(OpenTypeMetrics *m, const unsigned char *sfnt, size_t len)
{
    uint32_t sfnt_version;
    uint16_t num_tables, i;
    uint32_t prev_tag = 0;
    bool first_tag = true;
    OtTable head, maxp, hhea, hmtx, cmap, vhea, vmtx, os2;

    DCHECK(m != NULL && sfnt != NULL,
           "a face was read out of a NULL pointer. The bytes are BORROWED by OpenTypeMetrics and must outlive "
           "it, so a NULL here is a caller that has not decided where its font lives");
    memset(m, 0, sizeof(*m));
    m->sfnt = sfnt;
    m->len = len;

    if (len < 12)
        OT_REJECT(m, "the bytes are shorter than an sfnt Table Directory. OpenType \"The OpenType Font File\" "
                     "§Organization of an OpenType Font puts `sfntVersion`, `numTables`, `searchRange`, "
                     "`entrySelector` and `rangeShift` in the first twelve bytes of every font file");
    sfnt_version = ot_u32(m, 0);
    if (sfnt_version != 0x00010000u && sfnt_version != OT_TAG('O', 'T', 'T', 'O'))
        OT_REJECT(m, "the bytes do not begin with an sfnt version this engine reads. OpenType §Organization of "
                     "an OpenType Font defines two — \"OpenType fonts that contain TrueType outlines should "
                     "use the value of 0x00010000 for sfntVersion. OpenType fonts containing CFF data (version "
                     "1 or 2) should use 0x4F54544F ('OTTO', when re-interpreted as a Tag)\" — and says of the "
                     "others that \"Apple's TrueType Reference Manual allows for 'true' and 'typ1' for "
                     "sfntVersion. These version tags should not be used for OpenType fonts\". A 'ttcf' "
                     "collection is a DIFFERENT top-level structure (§Font Collections' TTCHeader names a "
                     "table directory per font resource) and reaching this with one is a caller that has not "
                     "chosen which font of the collection it means");

    num_tables = ot_u16(m, 4);
    if (num_tables == 0)
        OT_REJECT(m, "the sfnt Table Directory declares no tables at all");
    if ((size_t)16 * num_tables > len - 12)
        OT_REJECT(m, "the sfnt Table Directory declares more TableRecords than the file can hold. Each is "
                     "sixteen bytes (`tableTag`, `checksum`, `offset`, `length`) and they follow the "
                     "twelve-byte header — the count is the first number a hostile file inflates, which is "
                     "why OpenType §Organization of an OpenType Font tells parsers to derive searchRange, "
                     "entrySelector and rangeShift from it rather than trust them: \"incorrect values could "
                     "potentially be used as an attack vector against some implementations\"");

    for (i = 0; i < num_tables; i++) {
        size_t rec = 12 + (size_t)16 * i;
        uint32_t tag = ot_u32(m, rec);
        uint32_t off = ot_u32(m, rec + 8);
        uint32_t tlen = ot_u32(m, rec + 12);

        if (!first_tag && tag <= prev_tag)
            OT_REJECT(m, "the sfnt Table Directory's records are not in ascending tag order. OpenType "
                         "§Organization of an OpenType Font requires it — \"the records in the array must be "
                         "sorted in ascending order by tag\" — and the same section adds that \"a font "
                         "resource should have at most one table record using a given tag. If a font resource "
                         "does contain more than one table of a given type, behaviour is unpredictable: apps "
                         "or platforms may select one of the tables arbitrarily, or may reject the font as "
                         "invalid\". A strictly increasing order is both statements at once");
        first_tag = false;
        prev_tag = tag;

        if ((off & 3) != 0)
            OT_REJECT(m, "an sfnt table does not begin on a four-byte boundary. OpenType §Organization of an "
                         "OpenType Font requires that \"all tables must begin on four-byte boundaries, and any "
                         "remaining space between tables must be padded with zeros\", and its own checksum "
                         "routine assumes it");
        if (!ot_fits(m, off, tlen))
            OT_REJECT(m, "an sfnt TableRecord's offset and length run past the end of the file. Every offset "
                         "inside a table is bounded by the table's extent and the extent is bounded here, so "
                         "this is the check every other one in this file stands on");
    }

    head = ot_find(m, num_tables, OT_TAG('h', 'e', 'a', 'd'));
    maxp = ot_find(m, num_tables, OT_TAG('m', 'a', 'x', 'p'));
    hhea = ot_find(m, num_tables, OT_TAG('h', 'h', 'e', 'a'));
    hmtx = ot_find(m, num_tables, OT_TAG('h', 'm', 't', 'x'));
    cmap = ot_find(m, num_tables, OT_TAG('c', 'm', 'a', 'p'));
    vhea = ot_find(m, num_tables, OT_TAG('v', 'h', 'e', 'a'));
    vmtx = ot_find(m, num_tables, OT_TAG('v', 'm', 't', 'x'));
    os2 = ot_find(m, num_tables, OT_TAG('O', 'S', '/', '2'));
    m->has_os2 = os2.found;

    if (!head.found || !maxp.found || !hhea.found || !hmtx.found || !cmap.found)
        OT_REJECT(m, "the face is missing a table the measurement needs. 'head' — Font Header Table carries "
                     "unitsPerEm, 'maxp' — Maximum Profile carries numGlyphs, 'hhea' — Horizontal Header Table "
                     "carries numberOfHMetrics, 'hmtx' — Horizontal Metrics Table carries the advances "
                     "themselves and 'cmap' — Character to Glyph Index Mapping Table is how a character "
                     "reaches one. None of the five is optional in OpenType and none of them is substitutable");

    /* 'head' — Font Header Table. 54 bytes: two version uint16s, `fontRevision`, `checksumAdjustment`,
       `magicNumber`, `flags`, `unitsPerEm`, two LONGDATETIMEs, the bounding box, `macStyle`, `lowestRecPPEM`,
       `fontDirectionHint`, `indexToLocFormat`, `glyphDataFormat`. */
    if (head.len < 54)
        OT_REJECT(m, "the 'head' — Font Header Table is shorter than the 54 bytes its field list occupies");
    if (ot_u16(m, head.off) != 1)
        OT_REJECT(m, "the 'head' table's majorVersion is not 1. OpenType 'head' — Font Header Table defines it "
                     "as \"Major version number of the font header table — set to 1\" and describes no other "
                     "layout, so the field offsets below would be a guess");
    if (ot_u32(m, head.off + 12) != 0x5F0F3CF5u)
        OT_REJECT(m, "the 'head' table's magicNumber is not 0x5F0F3CF5. OpenType 'head' — Font Header Table "
                     "states the value outright (\"Set to 0x5F0F3CF5\"), which makes it the one field that "
                     "confirms the table is a 'head' at all rather than whatever a hostile directory offset "
                     "pointed at");
    m->units_per_em = ot_u16(m, head.off + 18);
    if (m->units_per_em < 16 || m->units_per_em > 16384)
        OT_REJECT(m, "the 'head' table's unitsPerEm is outside the range OpenType fixes for it — \"Set to a "
                     "value from 16 to 16384. Any value in this range is valid\". It is the DIVISOR that turns "
                     "a design-unit advance into a multiple of the em, so a zero divides and every other value "
                     "outside the range scales every length on every page by a factor the face never meant");

    /* 'maxp' — Maximum Profile. Version 0.5 (0x00005000, CFF outlines) carries only `numGlyphs`; version 1.0
       (0x00010000, TrueType outlines) carries the rest. Both put `numGlyphs` at offset 4, which is all that is
       read here. */
    if (maxp.len < 6)
        OT_REJECT(m, "the 'maxp' — Maximum Profile table is shorter than its version and numGlyphs fields");
    {
        uint32_t maxp_version = ot_u32(m, maxp.off);
        if (maxp_version != 0x00005000u && maxp_version != 0x00010000u)
            OT_REJECT(m, "the 'maxp' — Maximum Profile table's version is neither of the two OpenType defines "
                         "— \"Fonts with CFF or CFF2 outlines must use Version 0.5 of this table, specifying "
                         "only the numGlyphs field. Fonts with TrueType outlines must use Version 1.0\"");
    }
    m->num_glyphs = ot_u16(m, maxp.off + 4);
    if (m->num_glyphs == 0)
        OT_REJECT(m, "the face declares ZERO glyphs. OpenType 'cmap' §Table overview requires a glyph at index "
                     "0 of every font — \"The glyph at this location must be a special glyph representing a "
                     "missing character, commonly known as .notdef\" — so a face with no glyphs cannot even "
                     "answer for a character it does not cover");

    /* 'hhea' — Horizontal Header Table: 36 bytes, `numberOfHMetrics` the last uint16 of them. */
    if (hhea.len < 36)
        OT_REJECT(m, "the 'hhea' — Horizontal Header Table is shorter than the 36 bytes its field list "
                     "occupies");
    if (ot_u16(m, hhea.off) != 1)
        OT_REJECT(m, "the 'hhea' table's majorVersion is not 1 — OpenType 'hhea' — Horizontal Header Table "
                     "defines it as \"set to 1\" and describes no other layout");
    /* FWORDs — signed, and the descender is negative for every ordinary face. They are read as unsigned and
       cast because that is what a big-endian FWORD is: the two's-complement bit pattern in the file. */
    m->hhea_ascender = (int16_t)ot_u16(m, hhea.off + 4);
    m->hhea_descender = (int16_t)ot_u16(m, hhea.off + 6);
    m->number_of_h_metrics = ot_u16(m, hhea.off + 34);
    if (m->number_of_h_metrics == 0)
        OT_REJECT(m, "the 'hhea' table's numberOfHMetrics is ZERO. OpenType 'hmtx' — Horizontal Metrics Table "
                     "makes \"the advance width value of the last record\" the advance of every glyph past the "
                     "array, so an empty array leaves every glyph in the face with no advance at all");
    if (m->number_of_h_metrics > m->num_glyphs)
        OT_REJECT(m, "the 'hhea' table's numberOfHMetrics is ABOVE 'maxp' — Maximum Profile's numGlyphs. "
                     "OpenType 'hmtx' — Horizontal Metrics Table sizes the trailing leftSideBearings array as "
                     "\"numGlyphs - numberOfHMetrics\", which is not a count when the subtraction is negative, "
                     "and the hMetrics array would then hold records for glyphs the face does not have");

    /* 'hmtx' — Horizontal Metrics Table: hMetrics[numberOfHMetrics] of LongHorMetric{advanceWidth, lsb} then
       leftSideBearings[numGlyphs - numberOfHMetrics]. The whole layout is required to fit even though only the
       advances are read: a table truncated after the advances is a table whose recorded length is false, and
       accepting it would mean this file's idea of the layout and the font's had silently diverged. */
    if (hmtx.len < (size_t)4 * m->number_of_h_metrics +
                       (size_t)2 * ((size_t)m->num_glyphs - m->number_of_h_metrics))
        OT_REJECT(m, "the 'hmtx' — Horizontal Metrics Table is shorter than the arrays 'hhea' and 'maxp' "
                     "declare for it — hMetrics[numberOfHMetrics] of four bytes each followed by "
                     "leftSideBearings[numGlyphs - numberOfHMetrics] of two");
    m->hmtx = hmtx.off;

    /* 'vhea'/'vmtx' — OPTIONAL, and optional TOGETHER: OpenType 'vmtx' — Vertical Metrics Table says
       "OpenType vertical fonts require both a vertical header table ('vhea') and the vertical metrics table",
       so one without the other is a face that declares vertical layout and then does not describe it. */
    if (vhea.found != vmtx.found)
        OT_REJECT(m, "the face has exactly one of 'vhea' — Vertical Header Table and 'vmtx' — Vertical Metrics "
                     "Table. OpenType 'vmtx' requires both together — \"OpenType vertical fonts require both a "
                     "vertical header table ('vhea') and the vertical metrics table\" — because 'vmtx' \"does "
                     "not have a header\" and is sized entirely by 'vhea'.numOfLongVerMetrics and "
                     "'maxp'.numGlyphs");
    if (vhea.found) {
        uint32_t vhea_version;
        if (vhea.len < 36)
            OT_REJECT(m, "the 'vhea' — Vertical Header Table is shorter than the 36 bytes both of its versions "
                         "occupy");
        vhea_version = ot_u32(m, vhea.off);
        if (vhea_version != 0x00010000u && vhea_version != 0x00011000u)
            OT_REJECT(m, "the 'vhea' — Vertical Header Table's version is neither 1.0 (0x00010000) nor 1.1 "
                         "(0x00011000). OpenType 'vhea' §Table formats defines exactly those two and says the "
                         "difference is \"the name and definition of\" three fields this file does not read, "
                         "so an unknown version is a layout nobody has described");
        m->num_of_long_ver_metrics = ot_u16(m, vhea.off + 34);
        if (m->num_of_long_ver_metrics == 0 || m->num_of_long_ver_metrics > m->num_glyphs)
            OT_REJECT(m, "the 'vhea' table's numOfLongVerMetrics is zero or above numGlyphs. OpenType 'vmtx' "
                         "— Vertical Metrics Table sizes its second array by \"subtracting the value of "
                         "numOfLongVerMetrics from the number of glyphs in the font\" and requires that \"the "
                         "sum of glyphs represented in the first array plus the glyphs represented in the "
                         "second array therefore equals the number of glyphs in the font\", and it says of the "
                         "first array that \"only one entry need be in the first array, but that one entry is "
                         "required\"");
        if (vmtx.len < (size_t)4 * m->num_of_long_ver_metrics +
                           (size_t)2 * ((size_t)m->num_glyphs - m->num_of_long_ver_metrics))
            OT_REJECT(m, "the 'vmtx' — Vertical Metrics Table is shorter than the arrays 'vhea' and 'maxp' "
                         "declare for it — vMetrics[numOfLongVerMetrics] of four bytes each followed by "
                         "topSideBearing[numGlyphs - numOfLongVerMetrics] of two");
        m->vmtx = vmtx.off;
        m->has_vertical = true;
    }

    if (!ot_cmap_select(m, cmap.off, cmap.len)) return false;

    DCHECK(m->reject == NULL,
           "open_type_metrics_read is about to report a readable face while holding a rejection reason. The "
           "two are one answer and a face that carries both has a path that set the reason and then fell "
           "through instead of returning");
    return true;
}

/* ---- lookup ----------------------------------------------------------------------------------------------- */

/* THE ASSERT EVERY ACCESSOR OPENS WITH. A face that failed to read is zeroed, so reading one would hand back
   zeros — a plausible datum indistinguishable from a measurement, which is the exact defect a default hides.
   The read's answer is therefore not advisory: taking the face without it is a crash. */
#define OT_READY(m)                                                                                          \
    DCHECK((m) != NULL && (m)->reject == NULL && (m)->sfnt != NULL,                                          \
           "a face that open_type_metrics_read REJECTED (or never read at all) was measured. The struct is "  \
           "zeroed on failure precisely so that this cannot return a plausible number: unitsPerEm 0 and "     \
           "numGlyphs 0 are not a face. Whoever ignored the false is one frame up")

uint16_t open_type_metrics_glyph_id(const OpenTypeMetrics *m, uint32_t codepoint)
{
    uint32_t glyph = 0;

    OT_READY(m);
    DCHECK(codepoint <= 0x10FFFF && !(codepoint >= 0xD800 && codepoint <= 0xDFFF),
           "a glyph was asked for something that is not a Unicode scalar value — a surrogate code point or one "
           "past U+10FFFF. OpenType 'cmap' — Character to Glyph Index Mapping Table maps nothing else, so this "
           "is a caller that split a string between the two halves of a surrogate pair rather than a face that "
           "is missing a mapping");

    if (m->cmap_format == 4) {
        /* Format 4 covers the Basic Multilingual Plane only — "This is the standard character-to-glyph-index
           mapping subtable for fonts that support only Unicode Basic Multilingual Plane characters (U+0000 to
           U+FFFF)". A supplementary-plane codepoint is therefore not in it, which is a MISS and not an error:
           the face has no glyph for it and OpenType's answer for that is glyph 0. */
        uint32_t seg_count = (uint32_t)ot_u16(m, m->cmap_sub + 6) / 2;
        size_t end_a = m->cmap_sub + 14;
        size_t start_a = end_a + 2 * (size_t)seg_count + 2;
        size_t delta_a = start_a + 2 * (size_t)seg_count;
        size_t range_a = delta_a + 2 * (size_t)seg_count;
        uint32_t i;

        if (codepoint > 0xFFFF) return 0;
        /* "You search for the first endCode that is greater than or equal to the character code you want to
           map. If the corresponding startCode is less than or equal to the character code, then you use the
           corresponding idDelta and idRangeOffset to map the character code to a glyph index (otherwise, the
           missingGlyph is returned)." The search terminates because the read proved the final endCode is
           0xFFFF; a linear scan and the binary search the header's searchRange fields describe answer the same
           question, and this engine derives nothing from those fields for the reason OpenType gives. */
        for (i = 0; i < seg_count; i++) {
            uint32_t end = ot_u16(m, end_a + 2 * (size_t)i);
            uint32_t start, range_off;
            uint16_t delta;

            if (end < codepoint) continue;
            start = ot_u16(m, start_a + 2 * (size_t)i);
            if (start > codepoint) return 0;
            delta = ot_u16(m, delta_a + 2 * (size_t)i);
            range_off = ot_u16(m, range_a + 2 * (size_t)i);
            if (range_off == 0) {
                glyph = (uint32_t)(uint16_t)(codepoint + delta);
            } else {
                size_t at = range_a + 2 * (size_t)i + range_off + 2 * (size_t)(codepoint - start);
                glyph = ot_u16(m, at);
                if (glyph != 0) glyph = (uint32_t)(uint16_t)(glyph + delta);
            }
            break;
        }
        DCHECK(i < seg_count,
               "a 'cmap' Format 4 search ran off the end of the segment array. open_type_metrics_read proved "
               "the final endCode is 0xFFFF, which is the termination OpenType §Format 4: Segment mapping to "
               "delta values requires (\"For the search to terminate, the final startCode and endCode values "
               "must be 0xFFFF\") — so the face changed under this reader or the validation and this search "
               "disagree about the array's address");
    } else {
        /* Format 12: Segmented coverage — SequentialMapGroup{startCharCode, endCharCode, startGlyphID}, sorted
           and disjoint, so the first group whose endCharCode reaches the code decides. */
        uint32_t num_groups = ot_u32(m, m->cmap_sub + 12);
        uint32_t i;

        DCHECK(m->cmap_format == 12,
               "the face's selected 'cmap' subtable is in neither of the two formats this engine reads. "
               "open_type_metrics_read selects only Format 4 and Format 12 and rejects a face with neither, so "
               "a third value here is a field written outside that selection");
        for (i = 0; i < num_groups; i++) {
            size_t g = m->cmap_sub + 16 + (size_t)12 * i;
            uint32_t start = ot_u32(m, g);
            uint32_t end = ot_u32(m, g + 4);

            if (end < codepoint) continue;
            if (start > codepoint) break;
            glyph = ot_u32(m, g + 8) + (codepoint - start);
            break;
        }
    }

    DCHECK(glyph < m->num_glyphs,
           "a 'cmap' lookup produced a glyph ID this face does not have. open_type_metrics_read WALKS the "
           "whole selected subtable and rejects the face unless every code it can be asked about maps below "
           "'maxp' — Maximum Profile's numGlyphs, precisely so that this is an assert about this file rather "
           "than a clamp over a hostile font. So the validation walk and this lookup are reading the subtable "
           "differently — compare the two address computations, not the font");
    return (uint16_t)glyph;
}

bool open_type_metrics_covers(const OpenTypeMetrics *m, uint32_t codepoint)
{
    OT_READY(m);
    /* GLYPH 0 IS THE WHOLE ANSWER, because OpenType reserves it: 'cmap' §Table overview says "character codes
       that do not correspond to any glyph in the font should be mapped to glyph index 0", and the glyph there
       "must be a special glyph representing a missing character, commonly known as .notdef". So a face never
       maps a character it HAS onto glyph 0, and a lookup that lands there is coverage's negative answer. */
    return open_type_metrics_glyph_id(m, codepoint) != 0;
}

/* THE LAST-RECORD RULE, IN ONE PLACE FOR BOTH AXES. OpenType 'hmtx' — Horizontal Metrics Table: "As an
   optimization, the number of records can be less than the number of glyphs, in which case the advance width
   value of the last record applies to all remaining glyph IDs." 'vmtx' — Vertical Metrics Table states the same
   rule from the other side: the trailing top-sidebearing array's glyphs "must have the same advance height as
   the last entry in the vMetrics array".
   GETTING THIS WRONG IS INVISIBLE, which is why it is one function. A reader that indexed past the array would
   read a left (or top) side bearing — a plausible small number, of the wrong glyph, in the right units — and
   every advance from that glyph on would be wrong with nothing to say so. The clamp below is not a clamp past a
   broken invariant: `numberOfHMetrics <= numGlyphs` and `glyph_id < numGlyphs` are both proved, and the
   substitution IS the format's rule. */
static uint16_t ot_advance(const OpenTypeMetrics *m, size_t table, uint16_t long_metrics, uint16_t glyph_id)
{
    uint16_t record = glyph_id < long_metrics ? glyph_id : (uint16_t)(long_metrics - 1);

    DCHECK(glyph_id < m->num_glyphs,
           "an advance was asked for a glyph ID at or above 'maxp' — Maximum Profile's numGlyphs. Every glyph "
           "ID in this engine comes out of open_type_metrics_glyph_id, which asserts the same bound after a "
           "read that proved it over the whole 'cmap' — so this is a caller that invented a glyph ID rather "
           "than mapping a character to one");
    DCHECK(long_metrics >= 1 && long_metrics <= m->num_glyphs,
           "the face's long-metric count is outside 1..numGlyphs at the moment an advance is taken, though "
           "open_type_metrics_read rejects a face where it is. The record index below is derived from it, so a "
           "zero count would index the array at -1");
    return ot_u16(m, table + 4 * (size_t)record);
}

uint16_t open_type_metrics_advance_width(const OpenTypeMetrics *m, uint16_t glyph_id)
{
    OT_READY(m);
    return ot_advance(m, m->hmtx, m->number_of_h_metrics, glyph_id);
}

uint16_t open_type_metrics_advance_height(const OpenTypeMetrics *m, uint16_t glyph_id)
{
    OT_READY(m);
    if (!m->has_vertical)
        DFAIL("a VERTICAL advance was asked of a face with no 'vhea' — Vertical Header Table and 'vmtx' — "
              "Vertical Metrics Table. Those two are OPTIONAL in OpenType and most Latin faces omit them, so "
              "this is the expected state of a real font and not a corrupt one — but the answer is not the "
              "horizontal advance, which is a different measurement of a different axis that no spec asked "
              "for. css-writing-modes-4 §5.1.1 \"Vertical Typesetting and Font Features\" names what to build "
              "and admits it hands over no recipe: upright typographic character units are \"typeset upright "
              "in vertical lines with vertical font metrics. The UA MUST SYNTHESIZE vertical font metrics for "
              "fonts that lack them. (This specification does not define heuristics for synthesizing such "
              "metrics.)\" So the work is a synthesis component beside this one, reached from here, deriving "
              "an advance height from the metrics the face DOES carry — and until it exists the honest state "
              "is this crash. A CALLER THAT ONLY WANTS A CSS UNIT'S FALLBACK MUST NOT COME HERE AT ALL: "
              "css-values-4 §6.1.1 fixes assumed values for `ch` and `ic` \"in the cases where it is "
              "impossible or impractical to determine\" the measure, and core/css/font_metrics.c reads "
              "`has_vertical` to take that branch before asking");
    return ot_advance(m, m->vmtx, m->num_of_long_ver_metrics, glyph_id);
}
