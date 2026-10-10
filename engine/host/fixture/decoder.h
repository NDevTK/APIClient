/* THE TEXT DECODER DOCUMENT: Encoding §10.2's TextDecoder over streamed and BOM-led input.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_DECODER_H
#define ENGINE_HOST_FIXTURE_DECODER_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_DECODER[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    /* ENCODING §7.2's decode() IS A STEP MACHINE, and resting is what this row is about. The page chooses the
       buffer, so a C activation walking it declares no step boundary and holds the thread for the whole decode;
       §7.2's own "While true:" over single ITEMS is the rest point, with no granularity constant to choose.
       `big`: 196,608 bytes of `E4 B8 AD`, every item mid-sequence, so §8.1.1's state (bytes-needed, bytes-seen,
       the code point so far, the next byte's bound) is carried across every rest. A lost step state answers a
       SHORT string, a re-walk a LONG one, a lost decoder record U+FFFD — so the length and BOTH end code points
       are asserted. The buffer is built by sixteen doublings so the cost is the machine's, not the fixture's.
       Its size is a budget choice: raise it against the smoke's measured CPU, not against a feeling.
       `split`: a sequence cut across two `stream: true` calls resumes mid-sequence (§7.2 step 1's "if this's do
       not flush is false"); it reddens if the machine keeps `this's decoder` in its own state.
       `item` and `end` are the two abrupt exits at different stages: a lone `FF` is §4.1's error answered
       mid-queue (step 5.3.3's TypeError), and `E4 B8` with no third byte is the end-of-queue item, the same
       TypeError from decoder_flush. */
    "var tdBig = 'absent', tdSplit = 'absent', tdItem = 'absent', tdEnd = 'absent';"
    "if (typeof TextDecoder !== 'undefined') {"
    " var tdA = new Uint8Array([0xE4, 0xB8, 0xAD]), tdi, tdB;"
    " for (tdi = 0; tdi < 16; tdi++) { tdB = new Uint8Array(tdA.length * 2); tdB.set(tdA, 0);"
    "  tdB.set(tdA, tdA.length); tdA = tdB; }"
    " var tdS = new TextDecoder().decode(tdA);"
    " tdBig = (tdS.length === 65536 && tdS.charCodeAt(0) === 0x4E2D && tdS.charCodeAt(65535) === 0x4E2D)"
    "  ? 'isbig' : ('len' + tdS.length + 'x' + tdS.charCodeAt(0));"
    " var tdc = new TextDecoder();"
    " var tdp1 = tdc.decode(new Uint8Array([0xE4, 0xB8]), { stream: true });"
    " var tdp2 = tdc.decode(new Uint8Array([0xAD]), { stream: true });"
    " var tdp3 = tdc.decode();"
    " tdSplit = (tdp1 === '' && tdp2.length === 1 && tdp2.charCodeAt(0) === 0x4E2D && tdp3 === '')"
    "  ? 'issplit' : 'wrong';"
    " tdItem = 'nothrow';"
    " try { new TextDecoder('utf-8', { fatal: true }).decode(new Uint8Array([0xFF])); }"
    " catch (tde1) { tdItem = (tde1 instanceof TypeError) ? 'isitem' : 'other'; }"
    " tdEnd = 'nothrow';"
    " try { new TextDecoder('utf-8', { fatal: true }).decode(new Uint8Array([0xE4, 0xB8])); }"
    " catch (tde2) { tdEnd = (tde2 instanceof TypeError) ? 'isend' : 'other'; } }"
    "fetch('/api/tdstep?big=' + tdBig + '&split=' + tdSplit + '&item=' + tdItem + '&end=' + tdEnd);"
    /* ENCODING §7.1's SERIALIZE I/O QUEUE, whose BOM strip is restricted to three encodings: "If decoder's
       encoding is UTF-8 or UTF-16BE/LE, and decoder's ignore BOM and BOM seen are false: Set decoder's BOM seen
       to true. If item is U+FEFF BOM, then continue." Three conjuncts, one param each.
       `gb` is the encoding clause and the one a byte test cannot answer: decoded output is UTF-8 whatever the
       input was, and gb18030 spells U+FEFF as `84 31 95 33`, so a strip that asks only the output deletes a
       character the page decoded. §14.2 "Common infrastructure for UTF-16BE/LE": "UTF-16BE/LE is UTF-16BE or
       UTF-16LE".
       `u8`, `le` and `be` are the three admitted encodings: a strip narrowed to `utf-8` passes `u8` and
       reddens the other two; a strip deleted outright reddens all three while `gb` goes green.
       `ig` and `seen` are the other two conjuncts; `seen` needs two calls, because the flag set on the first
       item makes a second chunk's leading U+FEFF a CHARACTER.
       Every clause is a positive token, so a statement that never ran cannot satisfy a param. */
    "var tbGb = 'absent', tbU8 = 'absent', tbLe = 'absent', tbBe = 'absent', tbIg = 'absent', tbSeen = 'absent';"
    "if (typeof TextDecoder !== 'undefined') { var tbS, tbD, tb1, tb2;"
    " tbS = new TextDecoder('gb18030').decode(new Uint8Array([0x84, 0x31, 0x95, 0x33]));"
    " tbGb = (tbS.length === 1 && tbS.charCodeAt(0) === 0xFEFF) ? 'iskept' : ('len' + tbS.length);"
    " tbS = new TextDecoder('utf-8').decode(new Uint8Array([0xEF, 0xBB, 0xBF, 0x41]));"
    " tbU8 = (tbS.length === 1 && tbS.charCodeAt(0) === 0x41) ? 'isu8' : ('len' + tbS.length);"
    " tbS = new TextDecoder('utf-16le').decode(new Uint8Array([0xFF, 0xFE, 0x41, 0x00]));"
    " tbLe = (tbS.length === 1 && tbS.charCodeAt(0) === 0x41) ? 'isle' : ('len' + tbS.length);"
    " tbS = new TextDecoder('utf-16be').decode(new Uint8Array([0xFE, 0xFF, 0x00, 0x41]));"
    " tbBe = (tbS.length === 1 && tbS.charCodeAt(0) === 0x41) ? 'isbe' : ('len' + tbS.length);"
    " tbS = new TextDecoder('utf-8', { ignoreBOM: true }).decode(new Uint8Array([0xEF, 0xBB, 0xBF, 0x41]));"
    " tbIg = (tbS.length === 2 && tbS.charCodeAt(0) === 0xFEFF) ? 'isignore' : ('len' + tbS.length);"
    " tbD = new TextDecoder();"
    " tb1 = tbD.decode(new Uint8Array([0xEF, 0xBB, 0xBF, 0x41]), { stream: true });"
    " tb2 = tbD.decode(new Uint8Array([0xEF, 0xBB, 0xBF, 0x42]), { stream: true });"
    " tbSeen = (tb1 === 'A' && tb2.length === 2 && tb2.charCodeAt(0) === 0xFEFF)"
    "  ? 'isseen' : ('a' + tb1.length + 'b' + tb2.length); }"
    "fetch('/api/tdbom?gb=' + tbGb + '&u8=' + tbU8 + '&le=' + tbLe + '&be=' + tbBe"
    " + '&ig=' + tbIg + '&seen=' + tbSeen);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
