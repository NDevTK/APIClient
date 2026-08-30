/* READING A BYTE SEQUENCE AS A PROMISE — the machine Fetch §5.3 "Body mixin" and File API §3.3 "Methods
   and Parameters" both spell out. See
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
   `take` is where the two specs differ and the only place they do: Fetch §5.3's is "consume body", which throws
   on a second read because a body is a stream; File API §3.3's is a plain read, because a Blob is an immutable
   byte sequence that reads as many times as it is asked. Neither rule belongs in the shared machine. Return -1
   with a throw live. */
/* `*pstream` IS THE OTHER ANSWER `take` CAN GIVE. A Fetch body can BE a ReadableStream — `new Response(stream)`
   is §5.2 "BodyInit unions"' first arm — and §5.3's "consume body" then FULLY READS it before a reader sees a
   byte. Draining is a loop of reads, each one the page's code, so it cannot happen inside `take`: `take`
   reports the stream (DUP'd) and the reader machine drains it. JS_UNDEFINED means the bytes are here, which is
   every Blob and every body built from anything but a stream. */
/* `source` IS WHERE THESE BYTES CAME FROM, and only the including interface knows — the same rule, and the
   same reason, as the Body mixin's `mime`: the machine reads a byte sequence and has no way to ask who handed
   it one. It matters because the two readers below whose value IS the content — `text()` and `json()` — hand
   the page a value it computes with, and §solver's trust boundary decides what that value must be: a byte
   sequence a SERVER filled is unknown per-visitor input, so the value carries those bytes as its EXAMPLE while
   staying opaque for control flow, and the gate over a member the payload does not hold FORKS instead of
   answering `undefined`. A byte sequence the PAGE built (`new Response(JSON.stringify(x))`, `new Blob(["x"])`)
   is none of that, and NULL is the positive statement that it is not — never a hole a default fills.
   `*attacker` splits the two KINDS of unknown, because they are counted differently and only the interface can
   say which it holds: true for a DECLARED attacker delivery (solver/concolic.h's source registry — a file the
   user chose), which is one of the values `concolic_source_reads` exists to count, and false for
   server-injected state, which is unknown input the attacker does not author and must not be counted as one.
   The name is MALLOC'D and the caller frees it, and it is SPELLED SO THE @H SURFACE CAN PRINT IT: a hole is
   written between braces and the consumer reads one back with `/\{([^}\/]+)\}/`, so a name carrying `/` or `}`
   is a hole nothing can substitute — and solver/endpoint.c's path scan splits a shape on `/` before it looks
   for braces, so such a name is not merely unsubstitutable, it SHREDS the segment it sits in and takes the
   value the run measured with it. A component naming a byte sequence after an address owes that address a
   slash-free spelling, and core/fetch/reply_source.h is where an interface whose bytes came off the network
   discharges that obligation — one spelling for every such interface, because two doors onto one reply that
   name it twice split every predicate over its bytes in two. `script#__NEXT_DATA__` and `gon.current_user_id`
   are what a name minted from something other than an address looks like. */
typedef struct {
    bool (*is)(JSValueConst v);
    int  (*take)(JSContext *ctx, JSValueConst v, const char **bytes, size_t *len, JSValue *pstream);
    const char      *iface;
    const ByteReader *readers;
    int               nreaders;
    char *(*source)(JSContext *ctx, JSValueConst v, bool *attacker);
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
