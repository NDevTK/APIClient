/* The CompressionStream document: the Compression Standard §4 "Interface CompressionStream". The first shape
   is an upload helper's: a string through `new Blob([s]).stream().pipeThrough(new CompressionStream("gzip"))`,
   read whole by `new Response(stream).arrayBuffer()`. Each witness checks what the format fixes whatever blocks
   the compressor chose: RFC 1952's header bytes, CRC32 and ISIZE for "gzip", RFC 1950's CMF/FLG check and
   ADLER32 for "deflate". An unlisted format is Web IDL's TypeError, a chunk that is not a BufferSource errors
   the stream, and unknown bytes written to the writable side compress to an unknown image whose example is the
   real output.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_COMPRESS_H
#define ENGINE_HOST_FIXTURE_COMPRESS_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_COMPRESS[] =
    "<!doctype html><html><body>"
    "<script>"
    "function csRun(f, s) {"
    "  var t = new Blob([s]).stream().pipeThrough(new CompressionStream(f));"
    "  return new Response(t).arrayBuffer().then(function (a) { return new Uint8Array(a); });"
    "}"
    "function csTail(b, k) { var r = []; for (var i = b.length - k; i < b.length; i++) r.push(b[i]); return r.join('.'); }"
    "try {"
    "  csRun('gzip', 'hello').then(function (b) {"
    "    var w = (b[0] === 31 && b[1] === 139 && b[2] === 8 && csTail(b, 8) === '134.166.16.54.5.0.0.0') ? 'csGZ' : 'csBAD';"
    "    fetch('/api/cs-gzip?w=' + w);"
    "  }, function (e) { fetch('/api/cs-gzip?w=csREJ'); });"
    "  csRun('deflate', 'hello').then(function (b) {"
    "    var w = ((b[0] & 15) === 8 && ((b[0] << 8) | b[1]) % 31 === 0 && csTail(b, 4) === '6.44.2.21') ? 'csZL' : 'csBAD';"
    "    fetch('/api/cs-deflate?w=' + w);"
    "  }, function (e) { fetch('/api/cs-deflate?w=csREJ'); });"
    "} catch (e) { fetch('/api/cs-gzip?w=' + (e instanceof ReferenceError ? 'csREF' : 'csTHREW')); }"
    "var csF = 'csNONE';"
    "try { new CompressionStream('zip'); csF = 'csMADE'; } catch (e) { csF = e instanceof TypeError ? 'csTE' : 'csOTHER'; }"
    "fetch('/api/cs-format?w=' + csF);"
    "try {"
    "  var csW = new CompressionStream('gzip').writable.getWriter();"
    "  csW.write('not bytes').then(function () { fetch('/api/cs-chunk?w=csOK'); },"
    "    function (e) { fetch('/api/cs-chunk?w=' + (e instanceof TypeError ? 'csTE' : 'csOTHER')); });"
    "} catch (e) { fetch('/api/cs-chunk?w=csTHREW'); }"
    "try {"
    "  var csU = new TextEncoder().encode(location.hash);"
    "  var csS = new CompressionStream('gzip'), csV = csS.writable.getWriter();"
    "  csV.write(csU); csV.close();"
    "  new Response(csS.readable).arrayBuffer().then(function (a) {"
    "    var b = new Uint8Array(a);"
    "    fetch('/api/cs-unk?w=' + (b[0] === 31 && b[1] === 139 ? 'csUNK' : 'csBAD'));"
    "  }, function (e) { fetch('/api/cs-unk?w=csREJ'); });"
    "} catch (e) { fetch('/api/cs-unk?w=csTHREW'); }"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
