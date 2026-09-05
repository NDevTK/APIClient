/* HTML §9.2.5 Parsing an event stream and §9.2.6 Interpreting an event stream — the format, and nothing that
 * knows what an EventSource is. See event_source_parser.c for the algorithm; this header is the contract.
 *
 * WHY THE PARSER IS A COMPONENT AND NOT A PRIVATE FUNCTION OF THE INTERFACE. §9.2.2 The EventSource interface's
 * constructor reaches the format at exactly one place — its step 15 fetch, whose success arm is "announce the
 * connection and interpret res's body line by line" — so the format is a SUBPROBLEM of the interface and not a
 * detail of it, and it is the half that depends on nothing: it needs no realm, no request, no readyState, no
 * navigable and no network. That makes it the one piece of §9.2 with a single assertable contract you can
 * exercise with one fixture, which is what core/realm.h's neighbours mean by a component. Blink draws the line
 * in the same place and by the same name — `EventSourceParser` beside `EventSource` under modules/eventsource —
 * and its client interface is the seam this one is.
 *
 * WHAT IT DOES NOT DECIDE, AND WHY THAT IS THE SPLIT RATHER THAN AN OMISSION. §9.2.6's dispatch the event is
 * eight steps and three of them are about an object this file has never heard of: step 4 creates the event "in
 * the relevant realm of the EventSource object", step 5 takes its `origin` from "the origin of the event
 * stream's final URL (i.e., the URL after redirects)" and its `lastEventId` from the event source's own field,
 * and step 8 queues a task that runs only "if the readyState attribute is set to a value other than CLOSED".
 * A parser that answered any of those would be holding a second copy of state the interface owns. So the three
 * SINK entries below are exactly the steps that need the object, and everything else — the decode, the line
 * split, the four line arms, the five field arms, and dispatch steps 1, 2, 3, 6 and 7 — is decided here.
 *
 * IT IS `Sink` AND NOT `Client`, WHICH IS BLINK'S NAME, FOR A REASON LOCAL TO THIS TREE. Fetch §2.2.5 Requests
 * already gives "client" a meaning this component's own caller uses one step earlier: §9.2.2's constructor step
 * 9 is "Set request's client to settings". Two meanings for one word inside one algorithm is the hazard
 * core/fetch/reply_source.c states from the other side — one name for two things makes every reader's
 * predicate about it decide both — so the seam is named for what it is.
 *
 * THE BYTES ARE A STRANGER'S AND NOTHING HERE ASSERTS ANYTHING ABOUT THEM. Every byte this component reads was
 * written by whichever server the page addressed, so a malformed stream is INPUT and not a violated invariant:
 * §9.2.6 answers a line it cannot use with "Ignore the line" and a field it cannot use with "the field is
 * ignored", and those arms are the whole of the error handling. A DCHECK over a field value would hand any
 * server on the internet an abort switch for this engine, which is the failure CLAUDE.md names at
 * §WHOSE-BYTES-STATE-THE-VALUE. What IS asserted here is what this codebase computed: that the caller supplied
 * a sink with all three entries, that the decoder's output is well-formed UTF-8, and that a sink did not
 * re-enter the parse.
 *
 * THE CALLBACKS ARE ORDERED AND THE ORDER IS §9.2.6'S. `set_last_event_id` is dispatch step 1 and runs on EVERY
 * invocation of dispatch the event, INCLUDING the one that returns at step 2 with an empty data buffer — so a
 * stream carrying `id: 7` and a blank line updates the event source's field and fires no event, and a sink that
 * only implemented `dispatch_event` would silently lose that. `dispatch_event` is step 8, and by the time it
 * runs the event source's last event ID string is already the value step 5 reads. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTSOURCE_EVENT_SOURCE_PARSER_H
#define ENGINE_HOST_BROWSER_CORE_EVENTSOURCE_EVENT_SOURCE_PARSER_H
#include <stddef.h>
#include <stdint.h>

/* THE THREE STEPS THAT NEED THE EventSource OBJECT. `user` is the caller's, passed back unchanged.
 *
 * EVERY BYTE SPAN IS BORROWED FOR THE DURATION OF THE CALL AND IS NOT NUL-TERMINATED-BY-CONTRACT. A field
 * value may legitimately contain U+0000 — §9.2.5's `any-char` production excludes only LF and CR, and §9.2.6's
 * `id` arm exists precisely to test for a NULL — so a sink that reaches for strlen measures a prefix. The
 * length is the fact; the pointer is never NULL and is `""` when the length is zero. */
typedef struct {
    /* §9.2.6 dispatch the event step 1: "Set the last event ID string of the event source to the value of the
       last event ID buffer." Its own note is why this cannot be folded into the call below — "The buffer does
       not get reset, so the last event ID string of the event source remains set to this value until the next
       time it is set by the server" — and step 2's early return is why it cannot be folded into it either. */
    void (*set_last_event_id)(void *user, const char *id, size_t id_n);
    /* §9.2.6 dispatch the event step 8, with steps 4-6 left to the caller: `type` is already the answer steps 5
       and 6 compute together ("message", or the event type buffer where it "has a value other than the empty
       string"), and `data` is the data buffer after step 3. */
    void (*dispatch_event)(void *user, const char *type, size_t type_n, const char *data, size_t data_n);
    /* §9.2.6's `retry` field arm: "If the field value consists of only ASCII digits, then interpret the field
       value as an integer in base ten, and set the event stream's reconnection time to that integer." Called at
       the field, not at a dispatch, because that is where the standard sets it. */
    void (*set_reconnection_time)(void *user, uint64_t millis);
} EventSourceParserSink;

/* INTERPRET A COMPLETE EVENT STREAM BODY — §9.2.6, from its "Streams must be decoded using the UTF-8 decode
 * algorithm" through the last line the bytes terminate.
 *
 * IT TAKES THE WHOLE BODY IN ONE CALL BECAUSE THAT IS WHAT THE TRANSPORT DELIVERS, AND THAT IS A STATEMENT
 * ABOUT THE TRANSPORT RATHER THAN ABOUT THE FORMAT. SECURITY.md makes `extension/lib/safe-fetch.js` the only
 * door onto the network, and it reads a reply with one `await resp.arrayBuffer()`, so a reply reaches this
 * engine whole or not at all; core/fetch/fetch.h's `fetch_owe` hands a component one reply record with one
 * body. A one-shot entry is therefore the honest shape today, and it is also what makes this component's
 * buffers correct as plain C rather than as the JS values CLAUDE.md requires of anything a flow QUEUES: they
 * are created, filled and released inside this one call, they cross no suspend point, and there is no park for
 * them to survive. The moment a chunked delivery exists they would have to survive one, and that is the
 * residual event_source_parser.c names.
 *
 * `bytes`/`n` are the reply's BODY BYTES, undecoded — this runs §9.2.6's decode itself, because "The UTF-8
 * decode algorithm strips one leading UTF-8 Byte Order Mark (BOM), if any" is a step of THIS algorithm and a
 * caller that decoded first would strip the BOM twice or not at all. `n` may be zero; `bytes` may be NULL only
 * then. */
void event_source_parser_interpret(const char *bytes, size_t n,
                                   const EventSourceParserSink *sink, void *user);

#endif
