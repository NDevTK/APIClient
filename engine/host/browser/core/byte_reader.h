/* READING A BYTE SEQUENCE AS A PROMISE — the machine Fetch §5.2 and File API §3.3 both spell out. See
   byte_reader.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_BYTE_READER_H
#define ENGINE_HOST_BROWSER_CORE_BYTE_READER_H
#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"

/* ONE READER, as its interface declares it: the member's name and what the bytes become.
   `make` returns JS_EXCEPTION with a throw live to REJECT — which is what both specs say to do with an abrupt
   completion inside the read, and it is why `json()`'s SyntaxError arrives at the page's `.catch` rather than
   out of the call itself. */
typedef JSValue (*ByteReaderMake)(JSContext *ctx, JSValueConst recv, const char *bytes, size_t len);
typedef struct { const char *name; ByteReaderMake make; } ByteReader;

/* AN INTERFACE THAT HAS READERS: how to recognise one of its objects, how to get the bytes out, and which
   readers its IDL lists.
   `take` is where the two specs differ and the only place they do: Fetch §5.2's is "consume body", which throws
   on a second read because a body is a stream; File API §3.3's is a plain read, because a Blob is an immutable
   byte sequence that reads as many times as it is asked. Neither rule belongs in the shared machine. Return -1
   with a throw live. */
/* `*pstream` IS THE OTHER ANSWER `take` CAN GIVE. A Fetch body can BE a ReadableStream — `new Response(stream)`
   is §5.1's first union arm — and §5.2's "consume body" then FULLY READS that stream before a reader sees a
   byte. Draining is a loop of reads, each one the page's code, so it cannot happen inside `take`: `take`
   reports the stream (DUP'd) and the reader machine drains it. JS_UNDEFINED means the bytes are here, which is
   every Blob and every body built from anything but a stream. */
typedef struct {
    bool (*is)(JSValueConst v);
    int  (*take)(JSContext *ctx, JSValueConst v, const char **bytes, size_t *len, JSValue *pstream);
    const char      *iface;
    const ByteReader *readers;
    int               nreaders;
} ByteReaderIface;

/* DECLARE an interface's readers. Returns a handle; `d` and its reader table must outlive it. */
int  byte_reader_declare(JSContext *ctx, const ByteReaderIface *d);
/* INSTALL them on the interface's prototype. */
void byte_reader_install(JSContext *ctx, JSValueConst proto, int handle);

/* THE READERS BOTH SPECS DECLARE IN THE SAME WORDS, so that neither writes them out. A spec with a reader of
   its own — Fetch's `formData()`, whose parser is chosen by the Content-Type — supplies its own `make` and
   points its table at that. */
JSValue byte_reader_text(JSContext *ctx, JSValueConst recv, const char *bytes, size_t len);
JSValue byte_reader_json(JSContext *ctx, JSValueConst recv, const char *bytes, size_t len);
JSValue byte_reader_array_buffer(JSContext *ctx, JSValueConst recv, const char *bytes, size_t len);
JSValue byte_reader_bytes(JSContext *ctx, JSValueConst recv, const char *bytes, size_t len);

#endif
