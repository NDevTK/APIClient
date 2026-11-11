/* The reply record store: one record per answered send, owned by the instance, and the trail of records a
   flow's path has taken. Two readers: a replay delivers a returning member's replies from its trail rows
   instead of sending again, and the egress policy asks by key whether an observed send already has its answer.
   The store is host-side state like the outstanding set (solver/pending_index.h): no COW delta captures it,
   and the reply values it names are the records the registers share. */
#ifndef ENGINE_HOST_SOLVER_REPLY_RECORD_H
#define ENGINE_HOST_SOLVER_REPLY_RECORD_H

#include <stdint.h>

#include "quickjs.h"

/* How the zone produced an answer, stated by the zone on every provide. `sent`: the request left for another
   party and this is its outcome. `answered-locally`: the zone answered without a send. `unknown`: the zone
   cannot say which, and no reader may treat it as either. */
typedef enum {
    REPLY_HOW_SENT = 1,
    REPLY_HOW_ANSWERED_LOCALLY,
    REPLY_HOW_UNKNOWN,
} ReplyHow;

#define REPLY_HOW_TOKEN_SENT             "sent"
#define REPLY_HOW_TOKEN_ANSWERED_LOCALLY "answered-locally"
#define REPLY_HOW_TOKEN_UNKNOWN          "unknown"

/* The one mapping between the enum and the seam's words. Both are fatal on a value outside the vocabulary. */
const char *reply_how_token(ReplyHow how);
ReplyHow reply_how_of_token(const char *token);

#define REPLY_BODY_DIGEST_LEN 32

/* A send's identity: Fetch §2.2.5 Requests' method, mode, referrer policy, referrer and URL as the pending line
   states them, the credentials mode the line states, and the SHA-256 of the request body. `has_body` is 0 for
   a null body, whose digest is all zero. Borrowed by every function taking one; the store copies. */
typedef struct ReplyRecordKey {
    const char *method;
    const char *mode;
    const char *referrer_policy;
    const char *referrer;
    const char *url;
    const char *credentials;
    int has_body;
    uint8_t body_digest[REPLY_BODY_DIGEST_LEN];
} ReplyRecordKey;

/* Writes the SHA-256 of `body` (`n` bytes, NULL for a null body) into `k`. */
void reply_record_key_body(ReplyRecordKey *k, const uint8_t *body, size_t n);

typedef struct ReplyRecord ReplyRecord;

/* Records one answered send and returns its id: ids are 1-based and issued in arrival order, so the next id is
   reply_record_last_id() + 1. `askers` is the line's comma-joined asking documents, `reach` the asking
   document's reach and `prov` the reply's grade (both solver/pending.h's PROV_ values), `driven` the reply's
   drive mark. `reply` is the host's record whole, or JS_NULL for a network error; the store takes its own
   reference. Everything borrowed is copied. */
uint64_t reply_record_add(JSContext *ctx, const ReplyRecordKey *k, const char *askers, int reach, int prov,
                          int driven, ReplyHow how, JSValueConst reply);

/* The id of the newest record, 0 before the first. Also the lifetime count of records. */
uint64_t reply_record_last_id(void);
/* The record `id` names. Fatal on an id the store never issued. */
const ReplyRecord *reply_record_by_id(uint64_t id);
/* How many answers `k` has, and its `seq`-th (0-based, arrival order), NULL past the last. */
long reply_record_key_count(const ReplyRecordKey *k);
const ReplyRecord *reply_record_by_key(const ReplyRecordKey *k, long seq);

uint64_t    reply_record_id(const ReplyRecord *r);
long        reply_record_seq(const ReplyRecord *r);       /* the n-th answer of its key, 0-based */
const char *reply_record_askers(const ReplyRecord *r);
int         reply_record_reach(const ReplyRecord *r);
int         reply_record_prov(const ReplyRecord *r);
int         reply_record_driven(const ReplyRecord *r);
ReplyHow    reply_record_how(const ReplyRecord *r);
/* The recorded reply, owned by the caller. */
JSValue     reply_record_reply(JSContext *ctx, const ReplyRecord *r);

/* A flow's reply trail: the records its path has taken, newest first, as a chain of refcounted rows so a fork
   shares its parent's rows. A row is the park's per-path ordinal (`slot`), the record's id and the flow's
   path work at the arrival. */
typedef struct ReplyTrailRow ReplyTrailRow;

/* Appends a row to the chain `head` (NULL for an empty trail) and returns the new head. The caller's
   reference to `head` moves into the new row. Along a chain ids and coordinates
   do not fall; rows of one id are the parks one record answered on the path, in rising slot order. */
ReplyTrailRow *reply_trail_push(ReplyTrailRow *head, int64_t slot, uint64_t id, int64_t path_work);
/* Another holder of the chain `head`; returns `head`. NULL is the empty trail. */
ReplyTrailRow *reply_trail_share(ReplyTrailRow *head);
/* One holder fewer. Rows no chain reaches are freed. */
void reply_trail_release(ReplyTrailRow *head);

/* Whether the chain `head` holds the row (`slot`, `id`). Ids do not fall along a chain, so the walk ends at the
   first older row. */
int reply_trail_holds(const ReplyTrailRow *head, int64_t slot, uint64_t id);
const ReplyTrailRow *reply_trail_up(const ReplyTrailRow *row);
int64_t  reply_trail_slot(const ReplyTrailRow *row);
uint64_t reply_trail_id(const ReplyTrailRow *row);
int64_t  reply_trail_path_work(const ReplyTrailRow *row);

/* Rows appended since the last reset, and the rows standing now. */
long reply_trail_rows_total(void);
long reply_trail_rows_now(void);

/* Releases every record. Called at the frontier's teardown, after every flow has released its trail. */
void reply_record_reset(JSContext *ctx);

#endif
