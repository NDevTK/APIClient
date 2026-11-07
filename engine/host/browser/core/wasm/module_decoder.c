/* WebAssembly Core Specification (Release 3.0) §5.5 "Modules": preamble and section framing. See module_decoder.h. */
#include "core/wasm/module_decoder.h"

#include <stdlib.h>
#include <string.h>

#include <lexbor/encoding/decode.h>

#include "check.h"

enum {
    PH_PREAMBLE = 0,   /* §5.5.17's magic and version */
    PH_SECTION,        /* §5.5.2's next section header, or the end of the module */
    PH_NAME,           /* §5.5.3's custom name, one code point per step */
    PH_FINISHED
};

/* §5.5.17's module production: custom sections anywhere, every other section at most once in this order. The
   order is not the id order: the tag section (13) stands between memory and global, data count (12) between
   element and code. */
static const int SECTION_RANK[] = {
    [WASM_SECTION_CUSTOM] = 0,    [WASM_SECTION_TYPE] = 1,    [WASM_SECTION_IMPORT] = 2,
    [WASM_SECTION_FUNCTION] = 3,  [WASM_SECTION_TABLE] = 4,   [WASM_SECTION_MEMORY] = 5,
    [WASM_SECTION_TAG] = 6,       [WASM_SECTION_GLOBAL] = 7,  [WASM_SECTION_EXPORT] = 8,
    [WASM_SECTION_START] = 9,     [WASM_SECTION_ELEMENT] = 10, [WASM_SECTION_DATA_COUNT] = 11,
    [WASM_SECTION_CODE] = 12,     [WASM_SECTION_DATA] = 13,
};
#define SECTION_ID_LAST WASM_SECTION_TAG

void wasm_module_decoder_init(WasmModuleDecoder *d, const uint8_t *bytes, size_t n)
{
    DCHECK(d != NULL, "a WebAssembly decode was begun with no decoder");
    DCHECK(bytes != NULL || n == 0, "a WebAssembly decode was handed a length with no bytes");
    memset(d, 0, sizeof *d);
    d->in = bytes;
    d->in_len = n;
    d->phase = PH_PREAMBLE;
}

/* §5.2.2's u32: unsigned LEB128 in at most ceil(32/7) = 5 bytes, whose fifth byte sets no bit beyond the 32nd.
   The checks run in the reference interpreter's order: the length before the read, the range after it. */
static WasmDecodeStatus read_u32(WasmModuleDecoder *d, uint32_t *out)
{
    uint32_t v = 0;
    unsigned i;

    for (i = 0;; i++) {
        uint8_t b;

        if (i == 5) return WASM_DECODE_INTEGER_TOO_LONG;
        if (d->pos >= d->in_len) return WASM_DECODE_UNEXPECTED_END;
        b = d->in[d->pos++];
        if (i == 4 && (b & 0x7f) >= 0x10) return WASM_DECODE_INTEGER_TOO_LARGE;
        v |= (uint32_t)(b & 0x7f) << (7 * i);
        if ((b & 0x80) == 0) {
            *out = v;
            return WASM_DECODE_DONE;
        }
    }
}

/* §5.5.17's preamble: two four-byte words, each read whole before it is compared. */
static WasmDecodeStatus read_preamble(WasmModuleDecoder *d)
{
    static const uint8_t MAGIC[4] = { 0x00, 0x61, 0x73, 0x6D };
    static const uint8_t VERSION[4] = { 0x01, 0x00, 0x00, 0x00 };

    if (d->in_len < 4) return WASM_DECODE_UNEXPECTED_END;
    if (memcmp(d->in, MAGIC, 4) != 0) return WASM_DECODE_MAGIC;
    if (d->in_len < 8) return WASM_DECODE_UNEXPECTED_END;
    if (memcmp(d->in + 4, VERSION, 4) != 0) return WASM_DECODE_VERSION;
    d->pos = 8;
    d->phase = PH_SECTION;
    return WASM_DECODE_MORE;
}

static WasmDecodeStatus push_section(WasmModuleDecoder *d, WasmSectionId id, size_t offset, size_t size)
{
    WasmSection *s;

    if (d->n_sections == d->cap_sections) {
        size_t cap = d->cap_sections ? d->cap_sections * 2 : 8;
        WasmSection *grown;

        if (cap > SIZE_MAX / sizeof *grown) return WASM_DECODE_NO_ROOM;
        grown = realloc(d->sections, cap * sizeof *grown);
        if (grown == NULL) return WASM_DECODE_NO_ROOM;
        d->sections = grown;
        d->cap_sections = cap;
    }
    s = &d->sections[d->n_sections++];
    s->id = id;
    s->offset = offset;
    s->size = size;
    s->name_offset = 0;
    s->name_size = 0;
    return WASM_DECODE_MORE;
}

/* §5.5.2: an id byte, then the contents' u32 length. A section's order is judged on its id alone, as the
   reference interpreter does by never reading past an id it did not expect. */
static WasmDecodeStatus read_section(WasmModuleDecoder *d)
{
    WasmDecodeStatus st;
    uint32_t size, name_size;
    uint8_t id;

    if (d->pos == d->in_len) return WASM_DECODE_DONE;
    id = d->in[d->pos];
    if (id > SECTION_ID_LAST) return WASM_DECODE_SECTION_ID;
    if (id != WASM_SECTION_CUSTOM) {
        if (SECTION_RANK[id] <= d->last_rank) return WASM_DECODE_SECTION_ORDER;
        d->last_rank = SECTION_RANK[id];
    }
    d->pos++;
    st = read_u32(d, &size);
    if (st != WASM_DECODE_DONE) return st;
    if (size > d->in_len - d->pos) return WASM_DECODE_LENGTH_OUT_OF_BOUNDS;
    st = push_section(d, (WasmSectionId)id, d->pos, size);
    if (st != WASM_DECODE_MORE) return st;
    if (id != WASM_SECTION_CUSTOM) {
        d->pos += size;
        return WASM_DECODE_MORE;
    }
    /* §5.5.3's custom: a name, then bytes to the section's end. The name's length is bounded by the bytes, its
       encoding is read next, and only then is it held to the section, which is the reference order. */
    d->section_end = d->pos + size;
    st = read_u32(d, &name_size);
    if (st != WASM_DECODE_DONE) return st;
    if (name_size > d->in_len - d->pos) return WASM_DECODE_LENGTH_OUT_OF_BOUNDS;
    d->sections[d->n_sections - 1].name_offset = d->pos;
    d->sections[d->n_sections - 1].name_size = name_size;
    d->name_end = d->pos + name_size;
    d->phase = PH_NAME;
    return WASM_DECODE_MORE;
}

/* §5.2.4's name: one character of utf8 per step, through lexbor's UTF-8 decoder, which refuses exactly the byte
   sequences §5.2.4's utf8 does not produce (overlongs, surrogates, above U+10FFFF). A fresh decoder state is
   the whole of one: its `need` and `lower` are the only fields the single-character entry reads. */
static WasmDecodeStatus read_name(WasmModuleDecoder *d)
{
    lxb_encoding_decode_t dec;
    const lxb_char_t *q;
    lxb_codepoint_t cp;

    if (d->pos == d->name_end) {
        if (d->name_end > d->section_end) return WASM_DECODE_UNEXPECTED_END;
        d->pos = d->section_end;
        d->phase = PH_SECTION;
        return WASM_DECODE_MORE;
    }
    dec.u.utf_8.need = 0;
    dec.u.utf_8.lower = 0x00;
    q = d->in + d->pos;
    cp = lxb_encoding_decode_utf_8_single(&dec, &q, d->in + d->name_end);
    if (cp == LXB_ENCODING_DECODE_ERROR || cp == LXB_ENCODING_DECODE_CONTINUE) return WASM_DECODE_UTF8;
    DCHECK(cp <= LXB_ENCODING_DECODE_MAX_CODEPOINT && q > d->in + d->pos && q <= d->in + d->name_end,
           "lexbor's UTF-8 decoder answered a character without consuming bytes inside the name");
    d->pos = (size_t)(q - d->in);
    return WASM_DECODE_MORE;
}

WasmDecodeStatus wasm_module_decoder_step(WasmModuleDecoder *d)
{
    WasmDecodeStatus st;

    DCHECK(d != NULL, "a WebAssembly decode step was handed no decoder");
    DCHECK(d->phase != PH_FINISHED, "a WebAssembly decode was stepped after it answered");
    DCHECK(d->pos <= d->in_len, "a WebAssembly decode stands past the end of its bytes");
    switch (d->phase) {
    case PH_PREAMBLE: st = read_preamble(d); break;
    case PH_SECTION:  st = read_section(d); break;
    case PH_NAME:     st = read_name(d); break;
    default:
        CHECK_FAIL("a WebAssembly decode stands in a phase this file does not define");
    }
    if (st != WASM_DECODE_MORE) d->phase = PH_FINISHED;
    return st;
}

WasmDecodeStatus wasm_module_decoder_run(WasmModuleDecoder *d)
{
    WasmDecodeStatus st;

    do st = wasm_module_decoder_step(d);
    while (st == WASM_DECODE_MORE);
    return st;
}

size_t wasm_module_decoder_section_count(const WasmModuleDecoder *d)
{
    DCHECK(d != NULL, "a WebAssembly decode's sections were asked of no decoder");
    return d->n_sections;
}

const WasmSection *wasm_module_decoder_section(const WasmModuleDecoder *d, size_t i)
{
    DCHECK(d != NULL && i < d->n_sections, "a WebAssembly section was asked for past the sections read");
    return &d->sections[i];
}

void wasm_module_decoder_free(WasmModuleDecoder *d)
{
    DCHECK(d != NULL, "a WebAssembly decode was freed with no decoder");
    free(d->sections);
    d->sections = NULL;
    d->n_sections = 0;
    d->cap_sections = 0;
}

const char *wasm_decode_status_message(WasmDecodeStatus st)
{
    switch (st) {
    case WASM_DECODE_DONE:
    case WASM_DECODE_MORE:                 return NULL;
    case WASM_DECODE_UNEXPECTED_END:       return "unexpected end of section or function";
    case WASM_DECODE_MAGIC:                return "magic header not detected";
    case WASM_DECODE_VERSION:              return "unknown binary version";
    case WASM_DECODE_SECTION_ID:           return "malformed section id";
    case WASM_DECODE_SECTION_ORDER:        return "unexpected content after last section";
    case WASM_DECODE_INTEGER_TOO_LONG:     return "integer representation too long";
    case WASM_DECODE_INTEGER_TOO_LARGE:    return "integer too large";
    case WASM_DECODE_LENGTH_OUT_OF_BOUNDS: return "length out of bounds";
    case WASM_DECODE_UTF8:                 return "malformed UTF-8 encoding";
    case WASM_DECODE_NO_ROOM:              return "out of memory";
    }
    CHECK_FAIL("a WebAssembly decode status this file does not define");
}
