/* FETCH §2.9 PORT BLOCKING — the whole of it: the normative "bad port" table and the three-step algorithm that
 * reads it. Blink's net/base/port_util.cc is the same component under the same name.
 *
 * WHY THIS IS ITS OWN TRANSLATION UNIT, ON THE EVIDENCE. The table is 83 rows of NORMATIVE data — larger than
 * every algorithm in core/fetch except the request build itself — and it is the only reason this file will ever
 * change: the standard has added rows to it (554, 4190, 6697, 10080 each arrived on their own), and each
 * addition is a data edit with no logic in it. That is precisely why core/url keeps idna_table.h and
 * public_suffix_table.h apart from idna.c and public_suffix.c. What is NOT split here is the table from its
 * three-step algorithm: those two files would only ever change together, the algorithm is meaningless without
 * the table and the table is unreadable without the sentence that says what a row MEANS, so §2.9 is one problem
 * and gets one unit. The tables core/url separates are 9251 and 2596 lines of machine-derived Unicode and
 * registry data with no algorithm attached at all — a different situation, not a smaller one.
 *
 * WHAT IT MEASURES. `fetch/api/request/request-bad-port.any.js` is 83 subtests, one per row, each asserting that
 * `fetch(new URL(location.protocol + "//" + location.hostname + ":" + port))` rejects with a TypeError. Before
 * this file the engine had no notion of a blocked port at all — `grep -rn bad_port engine/host` answered
 * nothing — so every one of those 83 was red, and each one was red by going to the trusted host and getting a
 * reply back.
 *
 * WHERE IT IS INVOKED AND WHAT "BLOCKED" IS. §4.1 main fetch step 7: "If should request be blocked due to a bad
 * port, should fetching request be blocked as mixed content, should request be blocked by Content Security
 * Policy, or should request be blocked by Integrity Policy Policy returns blocked, then set response to a
 * network error." A network error is what §5.6 `fetch(input, init)` step 12's processResponse step 3 turns into
 * the page's answer — "If response is a network error, then reject p with a TypeError and abort these steps" —
 * so the observable is a rejected TypeError and nothing else. fetch.c produces it through the same
 * fetch_settle_local path §4.3's two locally-answered schemes already reject through, and it does so BEFORE the
 * host is owed anything: main fetch step 7 runs before step 12's scheme fetch, so a blocked request never
 * becomes a pending host entry and no flow is left parked on a reply that will never be sent.
 *
 * THE SCHEME CONJUNCT IS NOT DECORATION. §2.9 step 2 blocks only where the URL's scheme is an HTTP(S) scheme —
 * §2.5, "An HTTP(S) scheme is 'http' or 'https'" — so `data:`, `blob:`, `file:` and every non-special scheme
 * carry no port restriction whatever. A `ftp://x:21/` is NOT blocked by this algorithm. */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "core/fetch/port_blocking.h"

/* §2.9's TABLE, TRANSCRIBED FROM THE STANDARD IN ITS OWN ORDER — first column the port, second column the
   standard's "Typical service", which is informative and is kept because it is what makes a transcription of a
   normative table AUDITABLE: a reader diffing this against the spec is diffing two labelled columns rather than
   83 bare integers. Three rows name no service; the standard prints an em dash and so does this list, spelled
   `(none)` because a source file is not the place to depend on one.
   IT IS SPEC ORDER, WHICH IS ASCENDING, AND THE SEARCH BELOW ASSERTS THAT ON EVERY LOOKUP. */
static const uint16_t k_bad_ports[] = {
    0,      /* (none)       */
    1,      /* tcpmux       */
    7,      /* echo         */
    9,      /* discard      */
    11,     /* systat       */
    13,     /* daytime      */
    15,     /* netstat      */
    17,     /* qotd         */
    19,     /* chargen      */
    20,     /* ftp-data     */
    21,     /* ftp          */
    22,     /* ssh          */
    23,     /* telnet       */
    25,     /* smtp         */
    37,     /* time         */
    42,     /* name         */
    43,     /* nicname      */
    53,     /* domain       */
    69,     /* tftp         */
    77,     /* (none)       */
    79,     /* finger       */
    87,     /* (none)       */
    95,     /* supdup       */
    101,    /* hostname     */
    102,    /* iso-tsap     */
    103,    /* gppitnp      */
    104,    /* acr-nema     */
    109,    /* pop2         */
    110,    /* pop3         */
    111,    /* sunrpc       */
    113,    /* auth         */
    115,    /* sftp         */
    117,    /* uucp-path    */
    119,    /* nntp         */
    123,    /* ntp          */
    135,    /* epmap        */
    137,    /* netbios-ns   */
    139,    /* netbios-ssn  */
    143,    /* imap         */
    161,    /* snmp         */
    179,    /* bgp          */
    389,    /* ldap         */
    427,    /* svrloc       */
    465,    /* submissions  */
    512,    /* exec         */
    513,    /* login        */
    514,    /* shell        */
    515,    /* printer      */
    526,    /* tempo        */
    530,    /* courier      */
    531,    /* chat         */
    532,    /* netnews      */
    540,    /* uucp         */
    548,    /* afp          */
    554,    /* rtsp         */
    556,    /* remotefs     */
    563,    /* nntps        */
    587,    /* submission   */
    601,    /* syslog-conn  */
    636,    /* ldaps        */
    989,    /* ftps-data    */
    990,    /* ftps         */
    993,    /* imaps        */
    995,    /* pop3s        */
    1719,   /* h323gatestat */
    1720,   /* h323hostcall */
    1723,   /* pptp         */
    2049,   /* nfs          */
    3659,   /* apple-sasl   */
    4045,   /* npp          */
    4190,   /* sieve        */
    5060,   /* sip          */
    5061,   /* sips         */
    6000,   /* x11          */
    6566,   /* sane-port    */
    6665,   /* ircu         */
    6666,   /* ircu         */
    6667,   /* ircu         */
    6668,   /* ircu         */
    6669,   /* ircu         */
    6679,   /* osaut        */
    6697,   /* ircs-u       */
    10080,  /* amanda       */
};

FetchPortVerdict fetch_block_bad_port(const UrlRecord *url)
{
    size_t i;

    DCHECK(url != NULL,
           "§2.9 was asked about no URL — step 1 is `let url be request's current URL`, and a request that has "
           "reached main fetch has one");
    DCHECK(url->scheme != NULL,
           "§2.9 was handed a URL record with no scheme — a record the basic URL parser produced always has "
           "one, so this came from a parse that FAILED and whose failure the caller read as success");
    /* §4.1's port is null or a 16-bit unsigned integer, and url.h spells the null as -1. Anything else is a
       record no parser wrote, and it would compare against this table as a number that means nothing. */
    DCHECK(url->port >= -1 && url->port <= 65535,
           "§2.9 was handed a URL record whose port is outside §4.1's range — a URL's port is null or a 16-bit "
           "unsigned integer, so this record was written by something that is not the URL parser");

    /* STEP 2's FIRST CONJUNCT, and with it the whole of step 3 for every other scheme: §2.5's "An HTTP(S)
       scheme is 'http' or 'https'". */
    if (strcmp(url->scheme, "http") && strcmp(url->scheme, "https"))
        return FETCH_PORT_ALLOWED;

    /* STEP 2's SECOND CONJUNCT: "url's port is a bad port", which is membership of the table above and nothing
       else. The walk is LINEAR and exhaustive rather than a binary search, and that is the design: a linear
       walk depends on no ordering, so a transposed row cannot make a port silently stop being bad — while the
       DCHECK still holds the table to the spec's own ascending order on every lookup, which is what catches a
       duplicated or misplaced row in a transcription no compiler can check. 83 comparisons in front of a
       network request is not a cost worth trading a silent hole for.
       A NULL PORT (-1) MATCHES NOTHING, which is the right answer and not an accident: `https://x/` and
       `https://x:443/` both parse to the null port, and 443 is not a bad port anyway. */
    for (i = 0; i < sizeof k_bad_ports / sizeof k_bad_ports[0]; i++) {
        DCHECK(i == 0 || k_bad_ports[i] > k_bad_ports[i - 1],
               "the §2.9 bad port table is not strictly ascending — it is transcribed row by row in the "
               "standard's own order, so a duplicate or a transposition here is a mis-transcription of a "
               "normative table and the row it displaced is a port this engine would fetch");
        if (url->port == (int)k_bad_ports[i])
            return FETCH_PORT_BLOCKED;
    }
    /* STEP 3. */
    return FETCH_PORT_ALLOWED;
}
