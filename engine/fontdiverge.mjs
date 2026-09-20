/* WHICH FONT CAPABILITY THE CORPUS ACTUALLY NEEDS, MEASURED RATHER THAN RANKED BY WHAT A SHAPING ENGINE
 * USUALLY HAS.
 *
 *   node engine/fontdiverge.mjs [mirror-dir]      (default testing/corpus/mirror)
 *
 * THE QUESTION. core/fonts/open_type_metrics.c measures text off ONE face — the metrics-only DejaVu subset
 * core/fonts/default_font_data.c ships — because css-fonts-4 §5.2 "Matching font styles" ends its first
 * available font definition with "or a user agent's default font if none are available" and this user agent
 * installs no faces and loads none. That is CORRECT and it is NARROWER than what the mirrored pages ask for,
 * and the gap between those two is a NUMBER rather than an opinion. This prints it.
 *
 * WHY IT EXISTS AT ALL, rather than as a paragraph somebody wrote once. CLAUDE.md: an instrument whose output
 * anyone quotes is COMMITTED, in the same diff as the first quotation of its number — a measurement that
 * outlives its instrument is quotable and unreproducible at the same time, which is the one shape of stale
 * claim no grep can ever catch. Every figure this file prints is re-derivable by running it.
 *
 * IT PRINTS NO EXPECTED TOTAL AND NO EXPECTED COUNT. Both rot, and a count of what is missing shrinks as the
 * work is done and has exactly one reader: the person about to invalidate it. The face list is DERIVED by
 * walking the mirror, so the day a page is added the census knows.
 *
 * WHAT CALIBRATES IT. core/fonts/default_font_data.c PUBLISHES three totals in its own generated header —
 * unitsPerEm, numGlyphs, numberOfHMetrics. This reproduces all three FROM THE BYTES and throws unless they
 * agree, before printing any breakdown of anything. A probe that cannot reproduce the subject's own published
 * figures is a second implementation of the subject's selector wearing a plausible larger number, and the
 * disagreement is then the finding rather than the breakdown. Neither number is hardcoded here: both sides
 * move together when the generator is re-run, so this check cannot drift into a calibration against a
 * constant somebody typed.
 *
 * THE TWO KINDS OF INPUT, AND WHY THEY FAIL DIFFERENTLY. The corpus faces are BYTES A PAGE SERVED — attacker
 * input in the engine, and no better here — so a malformed one is REPORTED and the census continues; it may
 * not take the run down, exactly as core/fonts/open_type_metrics.c rejects rather than asserts. The SHIPPED
 * face is this repository's own generated artifact, so its failure is the data-integrity arm and THROWS: a
 * calibration that silently measured a subset would report a smaller divergence and read as progress.
 *
 * PROVENANCE OF THE FORMAT CONSTANTS. The WOFF2 known-table-tag indices below are the "Known Table Tags"
 * table of the W3C WOFF2 Recommendation (https://www.w3.org/TR/WOFF2/), and the GPOS structures are the
 * OpenType GPOS chapter (https://learn.microsoft.com/en-us/typography/opentype/spec/gpos) — PairPosFormat1's
 * and PairPosFormat2's field orders and the ValueFormat flag whose mask is 0x0004 X_ADVANCE. Both were
 * FETCHED rather than recalled; a format table written from memory is a claim no reader can check.
 *
 * WHAT IT DOES NOT DO. It decodes no 'glyf' and rasterizes nothing — it reads advances, which is the quantity
 * layout reports through getBoundingClientRect and therefore the one a render differential compares. It reads
 * no variation data either: a face with 'fvar' is measured at its DEFAULT instance, so its row understates a
 * page that sets a weight axis, and the census says how many such faces there are rather than pretending the
 * number covers them. */
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join, basename, sep } from 'node:path';
import { brotliDecompressSync } from 'node:zlib';

/* W3C WOFF2, "Known Table Tags". Index 63 means an arbitrary 4-byte tag follows the flags byte instead. */
const WOFF2_KNOWN_TAGS = (
    'cmap head hhea hmtx maxp name OS/2 post cvt fpgm glyf loca prep CFF VORG EBDT ' +
    'EBLC gasp hdmx kern LTSH PCLT VDMX vhea vmtx BASE GDEF GPOS GSUB EBSC JSTF MATH ' +
    'CBDT CBLC COLR CPAL SVG sbix acnt avar bdat bloc bsln cvar fdsc feat fmtx fvar ' +
    'gvar hsty just lcar mort morx opbd prop trak Zapf Silf Glat Gloc Feat Sill').split(' ');

/* A face is REJECTED, never asserted about: these bytes came off somebody else's server. Every thrower below
   this line is reached only for the shipped face, which is this repository's own artifact. */
class FaceRejected extends Error {}
const reject = (why) => { throw new FaceRejected(why); };

const u16 = (b, o) => { if (o + 2 > b.length) reject('a 16-bit field was read outside the face'); return b[o] * 256 + b[o + 1]; };
const i16 = (b, o) => { const v = u16(b, o); return v >= 0x8000 ? v - 0x10000 : v; };
const u32 = (b, o) => { if (o + 4 > b.length) reject('a 32-bit field was read outside the face'); return ((b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]) >>> 0); };

/* UIntBase128, WOFF2 §Data Types. Five bytes is the maximum a uint32 can need. */
function uintBase128(b, p) {
    let v = 0;
    for (let i = 0; i < 5; i++) {
        if (p >= b.length) reject('a UIntBase128 ran off the end of the face');
        const c = b[p++];
        v = ((v << 7) | (c & 0x7f)) >>> 0;
        if (!(c & 0x80)) return [v, p];
    }
    reject('a UIntBase128 is longer than the five bytes a uint32 can need');
}

/* ---- the sfnt this engine ships ------------------------------------------------------------------------- */

/* The table directory of an uncompressed sfnt: OpenType "The OpenType Font File" §Organization of an OpenType
   Font puts a Table Directory at the head of every one. */
function sfntTables(b) {
    const numTables = u16(b, 4), t = Object.create(null);
    for (let i = 0; i < numTables; i++) {
        const r = 12 + 16 * i;
        if (r + 16 > b.length) reject('the table directory declares more records than the file can hold');
        const tag = String.fromCharCode(b[r], b[r + 1], b[r + 2], b[r + 3]);
        const off = u32(b, r + 8), len = u32(b, r + 12);
        if (off + len > b.length) reject(`table '${tag}' runs past the end of the file`);
        t[tag] = { off, len };
    }
    return t;
}

/* ---- WOFF2 --------------------------------------------------------------------------------------------- */

/* The table directory alone — enough to say WHICH tables a face carries without paying for Brotli. */
function woff2Directory(b) {
    if (b.length < 48 || u32(b, 0) !== 0x774f4632) reject('not a WOFF2 file (signature is not wOF2)');
    const numTables = u16(b, 12), totalCompressed = u32(b, 20);
    let p = 48;
    const entries = [];
    for (let i = 0; i < numTables; i++) {
        if (p >= b.length) reject('the WOFF2 table directory runs past the end of the file');
        const flags = b[p++], idx = flags & 0x3f, version = (flags >> 6) & 3;
        let tag;
        if (idx === 63) { if (p + 4 > b.length) reject('an arbitrary tag ran off the end'); tag = String.fromCharCode(b[p], b[p + 1], b[p + 2], b[p + 3]); p += 4; }
        else tag = WOFF2_KNOWN_TAGS[idx];
        let origLength; [origLength, p] = uintBase128(b, p);
        /* WOFF2 §Transformed tables: 'glyf' and 'loca' are transformed unless the version is 3; every other
           table is transformed only when the version is NOT 0. transformLength is present exactly then. */
        const glyfLoca = tag === 'glyf' || tag === 'loca';
        const transformed = glyfLoca ? version !== 3 : version !== 0;
        let transformLength = origLength;
        if (transformed) [transformLength, p] = uintBase128(b, p);
        entries.push({ tag, transformed, transformLength });
    }
    return { entries, tags: entries.map((e) => e.tag), compressedAt: p, totalCompressed };
}

/* Decompress and split. The reconciliation at the end is the conservation identity that makes the split
   checkable: the table lengths must account for every byte Brotli produced, exactly. */
function woff2Tables(b) {
    const dir = woff2Directory(b);
    let raw;
    try { raw = brotliDecompressSync(b.subarray(dir.compressedAt, dir.compressedAt + dir.totalCompressed)); }
    catch (e) { reject('the WOFF2 compressed stream did not decompress: ' + e.message); }
    const t = Object.create(null);
    let off = 0;
    for (const e of dir.entries) { t[e.tag] = { ...e, data: raw.subarray(off, off + e.transformLength) }; off += e.transformLength; }
    if (off !== raw.length)
        reject(`the WOFF2 table lengths sum to ${off} and the decompressed stream is ${raw.length} bytes — ` +
               'the directory and the stream disagree about where one table ends and the next begins');
    return { tables: t, tags: dir.tags };
}

/* ---- the three questions a face is asked ---------------------------------------------------------------- */

/* cmap Format 4 / Format 12 lookup and the 'hmtx' advance, over tables already located. */
function faceReader(get) {
    for (const need of ['head', 'hhea', 'hmtx', 'maxp', 'cmap']) if (!get(need)) reject(`the face carries no '${need}' table`);
    const head = get('head'), hhea = get('hhea'), hmtx = get('hmtx'), maxp = get('maxp'), cmap = get('cmap');
    const unitsPerEm = u16(head, 18), numberOfHMetrics = u16(hhea, 34), numGlyphs = u16(maxp, 4);
    if (unitsPerEm < 16 || unitsPerEm > 16384) reject("'head'.unitsPerEm is outside the 16..16384 OpenType fixes for it");
    if (numberOfHMetrics < 1 || numberOfHMetrics > numGlyphs) reject("'hhea'.numberOfHMetrics is not in 1..numGlyphs");

    /* Prefer the full-repertoire subtable, then the BMP one, on either platform — the same preference order
       core/fonts/open_type_metrics.c's OT_CMAP_PREFERENCE states. */
    const n = u16(cmap, 2);
    let best = null;
    for (let i = 0; i < n; i++) {
        const r = 4 + 8 * i, pid = u16(cmap, r), eid = u16(cmap, r + 2), sub = u32(cmap, r + 4);
        if (sub + 2 > cmap.length) continue;
        const fmt = u16(cmap, sub);
        const rank = pid === 3 && eid === 10 && fmt === 12 ? 4 : pid === 3 && eid === 1 && fmt === 4 ? 3
                   : pid === 0 && fmt === 12 ? 2 : pid === 0 && fmt === 4 ? 1 : 0;
        if (rank && (!best || rank > best.rank)) best = { sub, fmt, rank };
    }
    if (!best) reject("the face has no 'cmap' subtable in Format 4 or Format 12");
    const { sub, fmt } = best;

    function glyph(cp) {
        if (fmt === 4) {
            if (cp > 0xffff) return 0;                       /* Format 4 covers the BMP only — a miss, not an error */
            const segCount = u16(cmap, sub + 6) / 2;
            const endA = sub + 14, startA = endA + 2 * segCount + 2, deltaA = startA + 2 * segCount, rangeA = deltaA + 2 * segCount;
            for (let i = 0; i < segCount; i++) {
                const end = u16(cmap, endA + 2 * i);
                if (end < cp) continue;
                const start = u16(cmap, startA + 2 * i);
                if (start > cp) return 0;
                const delta = u16(cmap, deltaA + 2 * i), rangeOff = u16(cmap, rangeA + 2 * i);
                if (rangeOff === 0) return (cp + delta) & 0xffff;
                const at = rangeA + 2 * i + rangeOff + 2 * (cp - start);
                if (at + 2 > cmap.length) reject("a 'cmap' Format 4 idRangeOffset indexes outside the table");
                const g = u16(cmap, at);
                return g === 0 ? 0 : (g + delta) & 0xffff;
            }
            return 0;
        }
        const groups = u32(cmap, sub + 12);
        for (let i = 0; i < groups; i++) {
            const g = sub + 16 + 12 * i, start = u32(cmap, g), end = u32(cmap, g + 4);
            if (cp < start) return 0;
            if (cp <= end) return u32(cmap, g + 8) + (cp - start);
        }
        return 0;
    }
    /* 'hmtx' — "the advance width value of the last record applies to all remaining glyph IDs". */
    const advance = (g) => (g >= numGlyphs ? 0 : u16(hmtx, 4 * Math.min(g, numberOfHMetrics - 1)));
    return {
        unitsPerEm, numGlyphs, numberOfHMetrics, glyph, advance,
        measureEm: (s) => [...s].reduce((a, ch) => a + advance(glyph(ch.codePointAt(0))) / unitsPerEm, 0),
    };
}

/* ---- GPOS pair adjustment under the 'kern' feature ------------------------------------------------------- */

const VALUE_FORMAT_BITS = [0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080];
const valueRecordSize = (vf) => VALUE_FORMAT_BITS.reduce((n, b) => n + (vf & b ? 2 : 0), 0);
/* Where xAdvance sits inside a ValueRecord: the fields appear in the ValueRecord definition's own order, so
   only the two placements can precede it. 0x0004 is X_ADVANCE; -1 means this record carries none. */
const xAdvanceOffset = (vf) => (vf & 0x0004 ? (vf & 0x0001 ? 2 : 0) + (vf & 0x0002 ? 2 : 0) : -1);

function gposKern(G) {
    const featureList = u16(G, 6), lookupList = u16(G, 8);
    const featureCount = u16(G, featureList);
    const kernLookups = new Set();
    const featureTags = new Set();
    for (let i = 0; i < featureCount; i++) {
        const r = featureList + 2 + 6 * i;
        const tag = String.fromCharCode(G[r], G[r + 1], G[r + 2], G[r + 3]);
        featureTags.add(tag);
        if (tag !== 'kern') continue;
        const f = featureList + u16(G, r + 4), count = u16(G, f + 2);
        for (let k = 0; k < count; k++) kernLookups.add(u16(G, f + 4 + 2 * k));
    }
    const lookupCount = u16(G, lookupList);
    const subtables = [];
    for (const li of kernLookups) {
        if (li >= lookupCount) continue;
        const lo = lookupList + u16(G, lookupList + 2 + 2 * li);
        const type = u16(G, lo), subCount = u16(G, lo + 4);
        for (let s = 0; s < subCount; s++) {
            let st = lo + u16(G, lo + 6 + 2 * s), t = type;
            /* Lookup type 9 is Extension positioning: the real type and a 32-bit offset sit in the stub. */
            if (t === 9) { t = u16(G, st + 2); st += u32(G, st + 4); }
            if (t === 2) subtables.push(st);                 /* type 2 is Pair adjustment */
        }
    }
    const coverageIndex = (co, g) => {
        if (u16(G, co) === 1) { const n = u16(G, co + 2); for (let i = 0; i < n; i++) if (u16(G, co + 4 + 2 * i) === g) return i; return -1; }
        const n = u16(G, co + 2);
        for (let i = 0; i < n; i++) { const r = co + 4 + 6 * i, s = u16(G, r), e = u16(G, r + 2); if (g >= s && g <= e) return u16(G, r + 4) + (g - s); }
        return -1;
    };
    const classOf = (cd, g) => {
        if (u16(G, cd) === 1) { const st = u16(G, cd + 2), n = u16(G, cd + 4); return g >= st && g < st + n ? u16(G, cd + 6 + 2 * (g - st)) : 0; }
        const n = u16(G, cd + 2);
        for (let i = 0; i < n; i++) { const r = cd + 4 + 6 * i, s = u16(G, r), e = u16(G, r + 2); if (g >= s && g <= e) return u16(G, r + 4); }
        return 0;
    };
    /* The adjustment GPOS makes to the FIRST glyph's advance, in font design units. */
    function pairAdvanceAdjustment(g1, g2) {
        for (const st of subtables) {
            const format = u16(G, st), coverage = st + u16(G, st + 2), vf1 = u16(G, st + 4), vf2 = u16(G, st + 6);
            if (coverageIndex(coverage, g1) < 0) continue;
            const xo = xAdvanceOffset(vf1);
            if (format === 1) {
                const ci = coverageIndex(coverage, g1), pairSetCount = u16(G, st + 8);
                if (ci >= pairSetCount) continue;
                const ps = st + u16(G, st + 10 + 2 * ci), count = u16(G, ps);
                const stride = 2 + valueRecordSize(vf1) + valueRecordSize(vf2);
                for (let i = 0; i < count; i++) { const r = ps + 2 + stride * i; if (u16(G, r) === g2) return xo < 0 ? 0 : i16(G, r + 2 + xo); }
            } else if (format === 2) {
                const c1 = classOf(st + u16(G, st + 8), g1), c2 = classOf(st + u16(G, st + 10), g2);
                const class1Count = u16(G, st + 12), class2Count = u16(G, st + 14);
                if (c1 >= class1Count || c2 >= class2Count) continue;
                const stride = valueRecordSize(vf1) + valueRecordSize(vf2);
                return xo < 0 ? 0 : i16(G, st + 16 + (c1 * class2Count + c2) * stride + xo);
            }
        }
        return 0;
    }
    return { pairAdvanceAdjustment, featureTags: [...featureTags].sort(), pairSubtables: subtables.length };
}

/* ---- the shipped face, and the calibration against its own published totals ------------------------------- */

const DEFAULT_FONT_C = 'engine/host/browser/core/fonts/default_font_data.c';

function shippedFace(path) {
    const src = readFileSync(path, 'utf8');
    const decl = src.match(/DEFAULT_FONT_SFNT\[(\d+)\]\s*=\s*\{/);
    if (!decl) throw new Error(`${path} does not declare DEFAULT_FONT_SFNT[] — this is the generated artifact of engine/fontsubset.mjs; re-run the generator rather than editing it`);
    const open = src.indexOf('{', src.indexOf('DEFAULT_FONT_SFNT['));
    const bytes = Uint8Array.from(src.slice(open + 1, src.indexOf('};', open)).match(/0x[0-9a-fA-F]{2}/g).map((h) => parseInt(h, 16)));
    if (bytes.length !== Number(decl[1]))
        throw new Error(`${path} declares DEFAULT_FONT_SFNT[${decl[1]}] and holds ${bytes.length} bytes — the generated array and its own length have come apart`);

    /* The generated header states these three in its own words. Reproducing them FROM THE BYTES is what makes
       every breakdown below worth reading; neither side is a constant typed here, so the check follows the
       generator instead of drifting from it. */
    const published = {};
    for (const [field, re] of [['unitsPerEm', /unitsPerEm\s+(\d+)/], ['numGlyphs', /numGlyphs\s+(\d+)/], ['numberOfHMetrics', /numberOfHMetrics\s+(\d+)/]]) {
        const m = src.match(re);
        if (!m) throw new Error(`${path}'s generated header no longer publishes ${field} — this file's calibration reads all three from it, so a probe that skipped the missing one would be measuring a subset and reporting a total`);
        published[field] = Number(m[1]);
    }
    const tables = sfntTables(bytes);
    const face = faceReader((tag) => (tables[tag] ? bytes.subarray(tables[tag].off, tables[tag].off + tables[tag].len) : null));
    for (const field of Object.keys(published))
        if (face[field] !== published[field])
            throw new Error(`the shipped face's ${field} is ${face[field]} in the bytes and ${published[field]} in ${path}'s own generated header — the header and the array disagree, so nothing below could be attributed to either`);
    return { face, tags: Object.keys(tables), published };
}

/* ---- the corpus ------------------------------------------------------------------------------------------ */

function walk(dir, out = []) {
    let entries;
    try { entries = readdirSync(dir); } catch { return out; }
    for (const e of entries) {
        const p = join(dir, e);
        let st; try { st = statSync(p); } catch { continue; }
        if (st.isDirectory()) walk(p, out); else out.push(p);
    }
    return out;
}

/* The text a page presents, with the scripts and styles that are not text taken out. It is a coarse reading
   and it is the SAME reading on both sides of every comparison below, so it cancels: what is being measured
   is the ratio of two faces over one string, never the string. */
function visibleText(htmlPath) {
    return readFileSync(htmlPath, 'utf8')
        .replace(/<script[\s\S]*?<\/script>/gi, ' ').replace(/<style[\s\S]*?<\/style>/gi, ' ')
        .replace(/<!--[\s\S]*?-->/g, ' ').replace(/<[^>]+>/g, ' ')
        .replace(/&nbsp;/g, ' ').replace(/&amp;/g, '&').replace(/&lt;/g, '<').replace(/&gt;/g, '>')
        .replace(/&quot;/g, '"').replace(/&#x27;|&apos;/g, "'").replace(/&#(\d+);/g, (_, d) => String.fromCodePoint(Number(d)))
        .replace(/\s+/g, ' ').trim();
}

const SAMPLE_CHARS = 6000;   /* one sample size for every row, so no row is a different measurement */

function main() {
    /* THE CORPUS DIRECTORY IS REQUIRED AND HAS NO DEFAULT. The default used to name a committed copy of
       other people's sites, which is deleted: a page's fonts, like its scripts and styles, are FETCHED AT
       RUNTIME and are not an artifact this tree carries. An absent path throws on the walk, but a path that
       EXISTS and holds no face answers an EMPTY LIST, and the line below used to print one sentence and
       RETURN — a comparison that was never made, rendered identically to a comparison that found no
       divergence. */
    const mirror = process.argv[2];
    if (!mirror)
        throw new Error('fontdiverge: a corpus directory is REQUIRED as argv[2]. It names faces a '
                      + 'real-network drive fetched and saved. There is no default.');
    const { face: shipped, tags: shippedTags, published } = shippedFace(DEFAULT_FONT_C);

    console.log(`SHIPPED FACE  ${DEFAULT_FONT_C}`);
    console.log(`  tables: ${shippedTags.join(' ')}`);
    console.log(`  reproduced from the bytes, against that file's own published header: ` +
                Object.entries(published).map(([k, v]) => `${k}=${v}`).join(' '));

    const faces = walk(mirror).filter((p) => p.endsWith('.woff2')).sort();
    /* AN EMPTY CORPUS IS A FAILED MEASUREMENT AND SAYS SO. It used to print and return, which is the
       absent-versus-zero conflation this project refuses everywhere else: nothing to compare against and
       nothing found to diverge are different facts and only one of them is a result. */
    if (faces.length === 0)
        throw new Error(`fontdiverge: no .woff2 under ${mirror}. That is a corpus that was never fetched, `
                      + `not a comparison that found no divergence — the two are different facts and this `
                      + `tool may only report the second.`);

    console.log(`\nWHAT THE CORPUS'S OWN FACES CARRY  (${mirror})`);
    const census = Object.create(null);
    const readable = [];
    for (const p of faces) {
        let dir;
        try { dir = woff2Directory(readFileSync(p)); }
        catch (e) { console.log(`  ${basename(p).slice(0, 44).padEnd(46)} REJECTED — ${e.message}`); continue; }
        for (const t of ['GSUB', 'GPOS', 'GDEF', 'kern', 'fvar']) if (dir.tags.includes(t)) census[t] = (census[t] || 0) + 1;
        readable.push({ path: p, tags: dir.tags });
        const carries = ['GSUB', 'GPOS', 'GDEF', 'kern', 'fvar'].map((t) => (dir.tags.includes(t) ? t : '·'.repeat(t.length))).join(' ');
        console.log(`  ${basename(p).slice(0, 44).padEnd(46)} ${carries}`);
    }
    console.log(`  faces read: ${readable.length} of ${faces.length}`);
    for (const t of ['GSUB', 'GPOS', 'GDEF', 'kern', 'fvar']) console.log(`    carry '${t}': ${census[t] || 0}/${readable.length}`);

    /* A page's own face against the face this engine actually measures with, over that page's own text. The
       kern column is what GPOS would move the same sum by, so the two are directly comparable and the
       ordering question stops being a matter of which feature a shaping engine usually has. */
    console.log('\nTHE SAME TEXT, MEASURED BOTH WAYS  (one page, its own face, its own words)');
    console.log(`  ${'page'.padEnd(14)}${'face'.padEnd(34)}${'chars'.padStart(6)}${'face err'.padStart(10)}${'GPOS kern'.padStart(11)}`);
    const faceErr = [], kernErr = [];
    for (const { path: p, tags } of readable) {
        const parts = p.split(sep);
        const page = parts[parts.indexOf(basename(mirror)) + 1];
        let text;
        try { text = visibleText(join(mirror, page, 'index.html')); } catch { continue; }
        if (text.length < 200) continue;
        let reader, kern = null;
        try {
            const { tables } = woff2Tables(readFileSync(p));
            if (tables.hmtx?.transformed) { console.log(`  ${page.padEnd(14)}${basename(p).slice(0, 32).padEnd(34)} SKIPPED — 'hmtx' is transformed and this file does not undo that transform`); continue; }
            reader = faceReader((tag) => tables[tag]?.data ?? null);
            if (tables.GPOS) kern = gposKern(tables.GPOS.data);
        } catch (e) { console.log(`  ${page.padEnd(14)}${basename(p).slice(0, 32).padEnd(34)} REJECTED — ${e.message}`); continue; }

        const s = [...text.slice(0, SAMPLE_CHARS)];
        let own = 0, kerned = 0;
        for (let i = 0; i < s.length; i++) {
            const g = reader.glyph(s[i].codePointAt(0));
            own += reader.advance(g) / reader.unitsPerEm;
            if (kern && i + 1 < s.length) kerned += kern.pairAdvanceAdjustment(g, reader.glyph(s[i + 1].codePointAt(0))) / reader.unitsPerEm;
        }
        if (own <= 0) continue;
        const fe = (shipped.measureEm(s.join('')) / own - 1) * 100, ke = (-kerned / own) * 100;
        faceErr.push(Math.abs(fe)); if (kern) kernErr.push(Math.abs(ke));
        console.log(`  ${page.padEnd(14)}${basename(p).slice(0, 32).padEnd(34)}${String(s.length).padStart(6)}` +
                    `${((fe >= 0 ? '+' : '') + fe.toFixed(1) + '%').padStart(10)}${(kern ? ke.toFixed(2) + '%' : 'no GPOS').padStart(11)}`);
    }

    const median = (a) => { const t = [...a].sort((x, y) => x - y); return t.length ? t[t.length >> 1] : NaN; };
    if (faceErr.length) {
        console.log(`\n  pages compared: ${faceErr.length}`);
        console.log(`  median |this engine's face vs the page's own|: ${median(faceErr).toFixed(1)}%`);
        if (kernErr.length) {
            console.log(`  median |what GPOS kerning moves the same sum by|: ${median(kernErr).toFixed(2)}%`);
            console.log(`  the first is ${(median(faceErr) / median(kernErr)).toFixed(0)}x the second`);
        }
        console.log('\n  Both columns are advance sums over the same string, so they are the same kind of number.\n' +
                    '  WHICH FACE is selected and WHETHER IT IS KERNED are two capabilities, and this says which\n' +
                    '  one the corpus is actually waiting on. Neither is built: core/css/css_computed_value.c\n' +
                    "  models no `font-family` at all, so nothing in the engine can say which family an element\n" +
                    '  asks for, and no component decodes WOFF2 — so every face above is unreachable and the\n' +
                    '  one face that is reachable carries none of the tables the left column would need.');
    }
}

main();
