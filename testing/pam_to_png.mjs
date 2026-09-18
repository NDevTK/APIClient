/* A RENDERED DOCUMENT, VIEWABLE — netpbm PAM (`P7`, `RGB_ALPHA`) to PNG, in the one runtime this project
 * already drives everything else with.
 *
 * WHY IT EXISTS AT ALL, WHICH IS A MEASUREMENT AND NOT A PREFERENCE. `engine/host/test_forced.c`'s
 * `--paint-dir` arm writes the engine's own surface as a PAM, because PAM is a CONTAINER the raster needs no
 * transform to enter (§Bind-before-build puts hand-rolling a codec last, and it hand-rolls nothing). The
 * usual readers of that container are netpbm's own `pamtopng` and ImageMagick — and MEASURED on the machine
 * this was written on, `pamtopng`, `pnmtopng`, `magick`, `convert` and Python's PIL are ALL ABSENT, while
 * `node` is what runs the server and the trusted zone in the same three-command sequence. A picture nobody
 * can open is a picture nobody looks at, so the conversion is committed rather than left as a sentence in a
 * report: CLAUDE.md's own rule is that an instrument whose output anyone quotes is COMMITTED, and the tell it
 * names is exactly this one — you can state a result and cannot state the tracked path that produces it.
 *
 * IT IS NOT A CODEC EITHER, WHICH IS WHY IT IS TWENTY LINES. PNG's own container is a signature and three
 * chunks, and `node:zlib` is the compressor — the only arithmetic here is the CRC every chunk carries, which
 * `zlib.crc32` also answers. Nothing about the PIXELS is computed: PNG colour type 6 IS eight-bit
 * NON-PREMULTIPLIED RGBA, which is what core/graphics/raster_surface.h holds and what PAM's `RGB_ALPHA`
 * states, so the default conversion moves the same four bytes per pixel into a different wrapper.
 *
 * THE GROUND IS A FLAG AND HAS NO DEFAULT VALUE, WHICH IS THE WHOLE POINT OF THE PAM BEING RGBA. A page this
 * engine painted has real transparency in it — every pixel CSS 2.1 §E.2 "Painting order" laid nothing on is
 * transparent, and that is a FINDING rather than a blank. Most viewers render transparent and white alike, so
 * `--over <rrggbb>` composites for a preview and SAYS SO in the file it writes; without it nothing is
 * composited and the alpha survives. Either way the choice is made in the command a person runs, which is
 * where it belongs — baking one into the render would make a pixel nothing painted and a pixel painted white
 * the same bytes, which is the absent-and-zero pair with a picture in it.
 *
 * usage:  node testing/pam_to_png.mjs <in.pam> <out.png> [--over rrggbb]
 */
import { readFileSync, writeFileSync } from 'node:fs';
import { deflateSync, crc32 } from 'node:zlib';

function fail(msg) { console.error('@WHY pam_to_png: ' + msg); process.exit(2); }

const argv = process.argv.slice(2);
let over = null;
const positional = [];
for (let i = 0; i < argv.length; i++) {
  if (argv[i] === '--over') {
    const v = argv[++i];
    if (v === undefined || !/^[0-9a-fA-F]{6}$/.test(v))
      fail('`--over` takes six hex digits naming the background a transparent pixel is composited over. ' +
           'It has no default: what a page did NOT paint is a finding, and a ground chosen here rather than ' +
           'at the render is the only place that choice is visible to the person making it.');
    over = [parseInt(v.slice(0, 2), 16), parseInt(v.slice(2, 4), 16), parseInt(v.slice(4, 6), 16)];
    continue;
  }
  positional.push(argv[i]);
}
if (positional.length !== 2) fail('usage: node testing/pam_to_png.mjs <in.pam> <out.png> [--over rrggbb]');

const src = readFileSync(positional[0]);
if (src.subarray(0, 3).toString('latin1') !== 'P7\n')
  fail(positional[0] + ' does not begin with PAM\'s `P7` magic — this reads the container ' +
       'engine/host/test_forced.c writes and nothing else, and guessing at another one would produce a ' +
       'plausible image of the wrong bytes');

/* THE HEADER, BY PAM'S OWN RULES: arbitrary lines, `#` is a comment, `ENDHDR` is the last one, and the raster
   begins at the byte after its newline with no delimiter of any kind. The COMMENTS are carried into the PNG
   below rather than dropped: engine/host/test_forced.c writes what CSS 2.1 §E.2 "Painting order"'s walk
   offered, what it laid and whether it FINISHED into them, and `complete` false means the picture is a
   FRAGMENT of the page — a fact that must not be lost by the step that makes the picture viewable. */
const hdr = Object.create(null);
const comments = [];
let i = 3;
for (;;) {
  const j = src.indexOf(0x0a, i);
  if (j < 0) fail(positional[0] + ' has no ENDHDR — the header ran to the end of the file, so there is no ' +
                  'raster to read and the bytes after it would be read as pixels');
  const line = src.subarray(i, j).toString('latin1');
  i = j + 1;
  if (line.startsWith('#')) { comments.push(line.slice(1).trim()); continue; }
  if (line.trim() === '') continue;
  if (line.trim() === 'ENDHDR') break;
  const sp = line.indexOf(' ');
  if (sp <= 0) fail('a PAM header line carries no value: ' + JSON.stringify(line));
  hdr[line.slice(0, sp)] = line.slice(sp + 1).trim();
}
const w = Number(hdr.WIDTH), h = Number(hdr.HEIGHT), depth = Number(hdr.DEPTH);
if (!Number.isInteger(w) || !Number.isInteger(h) || w < 1 || h < 1)
  fail(`this PAM states WIDTH ${hdr.WIDTH} and HEIGHT ${hdr.HEIGHT}; PAM requires both to be at least 1, so ` +
       'a file naming either as zero was not written by a producer that read its own format');
if (depth !== 4 || hdr.MAXVAL !== '255' || hdr.TUPLTYPE !== 'RGB_ALPHA')
  fail(`this reads DEPTH 4 / MAXVAL 255 / TUPLTYPE RGB_ALPHA and this file states ` +
       `${hdr.DEPTH} / ${hdr.MAXVAL} / ${hdr.TUPLTYPE}. Those three are what make a sample one byte and the ` +
       'fourth plane the OPACITY plane, so converting anything else would move bytes into a PNG that means ' +
       'something different by them');

const raw = src.subarray(i);
/* THE EXTENT IS CHECKED AGAINST THE SHAPE AND NOT ASSUMED FROM IT — a truncated raster under an intact header
   is exactly what a producer killed mid-write leaves, and it is the one corruption a reader can catch for
   free. Without this the last rows would be whatever the file happened to end at. */
if (raw.length !== w * h * 4)
  fail(`this PAM's header states ${w} x ${h} (${w * h * 4} bytes of raster) and the file carries ` +
       `${raw.length} — a truncated raster under an intact header is a write that was interrupted, and ` +
       'converting it would paint the missing rows out of whatever followed');

/* PNG SCANLINES ARE FILTER-BYTE-THEN-ROW, filter 0 meaning `None`; with that the row bytes ARE the PAM's row
   bytes, which is what keeps this a re-wrapping rather than a conversion. */
const composited = over !== null;
const stride = composited ? 3 : 4;
const scan = Buffer.allocUnsafe(h * (1 + w * stride));
let o = 0;
for (let y = 0; y < h; y++) {
  scan[o++] = 0;
  if (!composited) { raw.copy(scan, o, y * w * 4, (y + 1) * w * 4); o += w * 4; }
  else for (let x = 0; x < w; x++) {
    const p = (y * w + x) * 4, a = raw[p + 3] / 255;
    /* SOURCE-OVER ON STRAIGHT ALPHA, which is the arithmetic core/graphics/raster_surface.h composites with
       and HTML §4.12.5.1.17 "Compositing" makes the default operator. The foreground is not premultiplied
       here, so the fraction multiplies the colour rather than being divided back out of it. */
    for (let c = 0; c < 3; c++) scan[o++] = Math.round(raw[p + c] * a + over[c] * (1 - a));
  }
}
const chunk = (tag, payload) => {
  const body = Buffer.concat([Buffer.from(tag, 'latin1'), payload]);
  const len = Buffer.alloc(4); len.writeUInt32BE(payload.length);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(body) >>> 0);
  return Buffer.concat([len, body, crc]);
};
const ihdr = Buffer.alloc(13);
ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4);
ihdr[8] = 8; ihdr[9] = composited ? 2 : 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
const text = [...comments,
              composited ? `composited over #${process.argv[process.argv.indexOf('--over') + 1]} by ` +
                           'testing/pam_to_png.mjs — the source render carries real transparency and this ' +
                           'preview does not'
                         : 'no compositing: transparent pixels are pixels this engine painted nothing on']
  .map((t) => chunk('tEXt', Buffer.from('Comment\0' + t, 'latin1')));
writeFileSync(positional[1], Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
                                            chunk('IHDR', ihdr), ...text,
                                            chunk('IDAT', deflateSync(scan, { level: 9 })),
                                            chunk('IEND', Buffer.alloc(0))]));
console.log(`${positional[1]}: ${w} x ${h}, ${composited ? 'RGB (composited)' : 'RGBA (lossless)'}`);
for (const c of comments) console.log('  # ' + c);
