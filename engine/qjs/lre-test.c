#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "libregexp.h"

int lre_want_yield(void *opaque)
{
    return 0;   /* no scheduler here: this harness runs one match straight through */
}

void *lre_realloc(void *opaque, void *ptr, size_t size)
{
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, size);
}

// https://github.com/quickjs-ng/quickjs/issues/1375
static void oob_save_index(void)
{
    // Bytecode with REOP_save_start index=100, but capture_count=1.
    // Without validation this causes a heap-buffer-overflow in lre_exec_backtrack.
    // The header is 16 bytes and a capture index is a u32 — see RE_HEADER_* and libregexp-opcode.h. The
    // point of the fixture is unchanged: a save index the header says does not exist must be REJECTED, not
    // written past the end of the caller's capture array.
    uint8_t bc[] = {
        0x00, 0x00, 0x00, 0x00, // RE_HEADER_FLAGS = 0 (u16, padded)
        0x01, 0x00, 0x00, 0x00, // RE_HEADER_CAPTURE_COUNT = 1
        0x00, 0x00, 0x00, 0x00, // RE_HEADER_REGISTER_COUNT = 0
        0x07, 0x00, 0x00, 0x00, // RE_HEADER_BYTECODE_LEN = 7 (little-endian)
        0x06,                                     // REOP_any
        0x13, 0x64, 0x00, 0x00, 0x00,             // REOP_save_start, index=100
        0x10,                                     // REOP_match
    };

    uint8_t *capture[2] = {NULL, NULL};
    REExecContext ec;
    int ret;
    lre_exec_begin(&ec, capture, bc, (const uint8_t *)"a", 0, 1, 0, NULL);
    do {
        ret = lre_exec_step(&ec);
    } while (ret == LRE_RET_YIELD);
    lre_exec_end(&ec);
    assert(ret < 0);
}

int main(void)
{
    oob_save_index();
    return 0;
}
