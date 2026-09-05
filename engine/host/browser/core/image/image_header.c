/* css-images-3 §4.1 "Object-Sizing Terminology"'s natural width and height, read from an encoded image's own
 * header. See image_header.h for why this is a header read and not a decoder, and for why nothing a server
 * sent is ever asserted here. */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "check.h"
#include "core/image/image_header.h"
#include "core/mime/mime_sniff.h"

/* THE TWO BYTE ORDERS, EACH NAMED BY THE STANDARD THAT IMPOSES IT, because the formats below disagree and a
   single "read a 16-bit number" helper would make the difference invisible at the call site.
   PNG §7.1 "Integers and byte order": "All integers that require more than one byte shall be in network byte
   order … the most significant byte comes first". ITU-T T.81 §B.1.1.1 "Parameters": "For parameters which are
   2 bytes (16 bits) in length, the most significant byte shall come first". GIF §4 "About the Document":
   "Multi-byte numeric fields are ordered Least Significant Byte first."
   THE RESULT IS `unsigned long` SO THE FOUR-BYTE READ CANNOT OVERFLOW IT: PNG §7.1 limits a PNG four-byte
   unsigned integer to 0..2^31-1, and `unsigned long` is at least 32 bits, so every value a conforming
   datastream can state is representable and a non-conforming one is still read without undefined behaviour. */
static unsigned img_be16(const unsigned char *p)
{
    return ((unsigned)p[0] << 8) | (unsigned)p[1];
}

static unsigned long img_be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8)  | (unsigned long)p[3];
}

static unsigned img_le16(const unsigned char *p)
{
    return ((unsigned)p[1] << 8) | (unsigned)p[0];
}

/* ---- PNG ---------------------------------------------------------------------------------------------- */

/* PNG §5.2 "PNG signature" is the eight bytes MIME Sniffing §6.1 has already matched. §5.3 "Chunk layout"
   gives every chunk a four-byte Length and a four-byte Chunk Type ahead of its data, and §11.2.1 "IHDR Image
   header" states both that "The IHDR chunk shall be the first chunk in the PNG datastream" and its field
   order: "Width 4 bytes", then "Height 4 bytes". So the width is the four bytes at 8 + 4 + 4 = 16 and the
   height the four at 20, and no chunk walk is needed to reach them — the standard fixes the position.
   THE CHUNK TYPE IS CHECKED RATHER THAN ASSUMED. "Shall be the first chunk" is a conformance requirement on
   the ENCODER, so a datastream that breaks it is a stranger's malformed bytes and not this engine's broken
   invariant: it is REFUSED (which §4.8.4.3.5 has an arm for), never asserted.
   ZERO IS REFUSED BECAUSE §11.2.1 SAYS SO — "Width and height give the image dimensions in pixels. They are
   PNG four-byte unsigned integers. Zero is an invalid value." A zero-by-zero image is not a small image, it
   is a corrupt datastream, and reporting it as a natural dimension would hand css-images-3 §4.1 a degenerate
   object that every arm of CSS 2.1 §10.3.2 would then size from. */
static void img_png(ImageHeader *out, const unsigned char *b, size_t n)
{
    unsigned long w, h;

    if (n < 24) return;                                  /* the header is truncated: dimensions unobtainable */
    if (memcmp(b + 12, "IHDR", 4) != 0) return;
    w = img_be32(b + 16);
    h = img_be32(b + 20);
    if (w == 0 || h == 0) return;
    out->have_dims = true;
    out->width  = (double)w;
    out->height = (double)h;
}

/* ---- GIF ---------------------------------------------------------------------------------------------- */

/* GIF §17 "Header" is six bytes — a three-byte Signature and a three-byte Version — and "must appear at the
   beginning of every Data Stream". §18 "Logical Screen Descriptor" "must appear immediately after the Header"
   and opens with "Logical Screen Width" (two bytes, Unsigned) then "Logical Screen Height". So the width is
   at 6 and the height at 8, Least Significant Byte first per §4 "About the Document".
   THE LOGICAL SCREEN IS THE IMAGE'S NATURAL SIZE, and that is §18's own definition rather than an
   approximation of one: "Logical Screen Width — Width, in pixels, of the Logical Screen where the images will
   be rendered in the displaying device." A GIF's frames are placed INTO that screen by §20 "Image
   Descriptor"'s own offsets, so the screen is what the resource as a whole is, which is what a natural
   dimension names.
   ZERO IS NOT REFUSED HERE, and the asymmetry with PNG is the standards' rather than this file's: §18 states
   no lower bound on either field, so a zero is a value the format allows and css-images-3 §4.1 has a reading
   for — an object with a natural width and height of zero and, being degenerate, no natural aspect ratio. */
static void img_gif(ImageHeader *out, const unsigned char *b, size_t n)
{
    if (n < 10) return;
    out->have_dims = true;
    out->width  = (double)img_le16(b + 6);
    out->height = (double)img_le16(b + 8);
}

/* ---- JPEG --------------------------------------------------------------------------------------------- */

/* ITU-T T.81 §B.1.1.3 "Marker assignments" Table B.1: the Start Of Frame markers are XFFC0-XFFC3 (non
   differential, Huffman), XFFC5-XFFC7 (differential, Huffman), XFFC9-XFFCB (non-differential, arithmetic) and
   XFFCD-XFFCF (differential, arithmetic). THE THREE HOLES ARE NOT SOF AND THAT IS THE WHOLE OF THIS FUNCTION:
   XFFC4 is DHT "Define Huffman table(s)", XFFC8 is JPG "Reserved for JPEG extensions" and XFFCC is DAC
   "Define arithmetic coding conditioning(s)". Reading a frame header out of one of those would take a Huffman
   table's counts as an image's dimensions. */
static bool img_jpeg_is_sof(unsigned char m)
{
    if (m == 0xC4 || m == 0xC8 || m == 0xCC) return false;
    return (m >= 0xC0 && m <= 0xCF);
}

/* T.81 §B.1.1.3's asterisked markers — "An asterisk (*) indicates a marker which stands alone, that is, which
   is not the start of a marker segment" — so these carry NO length parameter and advancing by one would be
   reading their successor's first byte as a length. Table B.1 asterisks TEM (XFF01), RSTm (XFFD0 through
   XFFD7), SOI (XFFD8) and EOI (XFFD9). */
static bool img_jpeg_standalone(unsigned char m)
{
    return m == 0x01 || m == 0xD8 || m == 0xD9 || (m >= 0xD0 && m <= 0xD7);
}

/* WALK THE MARKER SEGMENTS TO THE FIRST FRAME HEADER. T.81 §B.2.2 "Frame header syntax": the frame header is
   "SOFn Lf P Y X Nf …", where "Lf: Frame header length", "P: Sample precision" is one byte, "Y: Number of
   lines" and "X: Number of samples per line". SO THE HEIGHT PRECEDES THE WIDTH — Y before X — which is the one
   thing about this format that is easy to write backwards and impossible to notice afterwards, because the
   two are equal for every square test image.
   §B.1.1.4 "Marker segments": "The first parameter in a marker segment is the two-byte length parameter. This
   length parameter encodes the number of bytes in the marker segment, INCLUDING the length parameter and
   EXCLUDING the two-byte marker." So the next marker sits at the length field plus that length.
   §B.1.1.2 "Markers": "Any marker may optionally be preceded by any number of fill bytes, which are bytes
   assigned code XFF" — hence the skip loop rather than a fixed two-byte step.
   THE WALK STOPS AT SOS AND DOES NOT ENTER IT. XFFDA is Start Of Scan, after whose header comes §B.1.1.5's
   entropy-coded data, in which an XFF is a stuffed byte rather than a marker; a walk that continued would be
   reading compressed samples as segment lengths. A baseline JPEG states its frame header before its first
   scan, so stopping there loses nothing this component is asked for — and a file that reaches SOS with no SOF
   is refused, which is exactly "corrupted in some fatal way such that the image dimensions cannot be
   obtained".
   EVERY BOUND IS CHECKED AGAINST `n` BEFORE THE READ, and `n` is the length of a stranger's response body, so
   a truncated or hostile file leaves through the same refusal as any other malformed one. */
static void img_jpeg(ImageHeader *out, const unsigned char *b, size_t n)
{
    size_t p = 2;   /* past the SOI (XFFD8) MIME Sniffing §6.1 matched */

    while (p + 1 < n) {
        unsigned char m;
        size_t seg;

        if (b[p] != 0xFF) return;              /* not a marker where one must be: refuse */
        while (p < n && b[p] == 0xFF) p++;     /* §B.1.1.2's fill bytes */
        if (p >= n) return;
        m = b[p++];
        if (m == 0x00) return;                 /* §B.1.1.3: a marker's second byte is never 0 */
        if (img_jpeg_standalone(m)) continue;
        if (m == 0xDA) return;                 /* SOS: entropy-coded data follows — see above */
        if (p + 1 >= n) return;
        seg = img_be16(b + p);                 /* §B.1.1.4's length, counting itself */
        if (seg < 2) return;
        if (img_jpeg_is_sof(m)) {
            /* §B.2.2: Lf(2) P(1) Y(2) X(2) — so Y is at p+3 and X at p+5, and the segment must be long
               enough to hold all of them (Lf + P + Y + X + at least one component is 8 or more). */
            if (seg < 8 || p + 7 > n) return;
            out->have_dims = true;
            out->height = (double)img_be16(b + p + 3);
            out->width  = (double)img_be16(b + p + 5);
            return;
        }
        if (p + seg > n) return;
        p += seg;
    }
}

/* ---- the one entry ------------------------------------------------------------------------------------ */

ImageHeader image_header_read(const unsigned char *bytes, size_t n)
{
    ImageHeader out = { false, 0.0, 0.0 };
    const char *type;

    /* THE ONE ASSERT IN THIS FILE, AND IT IS ABOUT THE CALLER RATHER THAN ABOUT THE BYTES. A non-empty body
       with no pointer is this engine composing a call wrongly, which is an invariant; every property of the
       CONTENT belongs to whoever served it and is refused below instead. */
    DCHECK(n == 0 || bytes != NULL,
           "an image header read was handed a byte count with no bytes — the body of a reply is a pointer and "
           "a length taken together from core/fetch/fetch.h's fetch_body_bytes, and a length without its "
           "pointer is a caller that split them");
    if (bytes == NULL || n == 0) return out;

    /* MIME Sniffing §6.1 "Matching an image type pattern", asked of core/mime rather than restated — see
       image_header.h. It answers the IMAGE MIME TYPE of a byte sequence, or NULL for bytes that match no image
       pattern at all. */
    type = mime_sniff_image_pattern(bytes, n);
    if (type == NULL) return out;

    /* WHICH FORMATS THIS AGENT READS DIMENSIONS FOR, WHICH IS WHAT HTML §4.8.4.3.5 MEANS BY "a supported image
       format" FOR THIS BUILD. §6.1's table has three more rows — `image/bmp`, `image/x-icon` and
       `image/webp` — and they are absent from this switch rather than answered badly, so a reply carrying one
       takes §4.8.4.3.5's "Otherwise" arm and reports `broken`, which is what a user agent that does not
       support a format is required to do.
       NAMED RESIDUAL. WHAT IS NOT COVERED: BMP, ICO and WebP, and every format §6.1 has no row for (AVIF,
       HEIC, SVG — SVG is not a §6.1 image pattern at all and its natural dimensions are its `width`/`height`
       or `viewBox` attributes, so it is a document parse rather than a header read). WHAT THE NEXT DIFF
       BUILDS: an `img_bmp` over BITMAPINFOHEADER's `biWidth`/`biHeight`, and an `img_webp` over the RIFF
       container's three chunk kinds (`VP8 `, `VP8L`, `VP8X`), each added as one arm here with the same shape
       as the three above and no other file touched. HOW ITS ABSENCE SHOWS: a page whose image is a WebP fires
       `error` where a browser fires `load`, and its `naturalWidth` answers 0 — which is exactly the symptom
       every image had before this component existed, now narrowed to the formats no arm reads. */
    if (strcmp(type, "image/png") == 0)       img_png(&out, bytes, n);
    else if (strcmp(type, "image/gif") == 0)  img_gif(&out, bytes, n);
    else if (strcmp(type, "image/jpeg") == 0) img_jpeg(&out, bytes, n);

    DCHECK(!out.have_dims || (out.width >= 0.0 && out.height >= 0.0),
           "an image header read reported a NEGATIVE natural dimension — every field above is read out of an "
           "unsigned integer of a fixed width, so a negative here is this file's own arithmetic and not "
           "anything a resource could state");
    return out;
}
