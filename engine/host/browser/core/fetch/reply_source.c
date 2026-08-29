/* WHAT A REPLY'S BYTES ARE CALLED.
 *
 * WHY IT IS ONE FUNCTION AND NOT A DECISION EACH INTERFACE MAKES. `fetch()` and XMLHttpRequest are two doors
 * onto ONE fact — a byte sequence the server at an address filled — and a name minted at each door is TWO
 * names for one unknown. That is not a cosmetic split: solver/absent.c states the rule for the mirror case
 * ("`gon.current_user_id` and a bare `current_user_id` are two different unknowns and each must decide only
 * its own predicates"), and it runs in both directions. Two names for one reply means a predicate that
 * narrowed the value read through XHR decides nothing about the value read through fetch, and the @H surface
 * reports one parameter twice under two spellings the reviewer cannot tell apart. So the spelling is decided
 * HERE, once, and each interface supplies only the address it already holds.
 *
 * WHY THE NAME IS THE ADDRESS AND THE WHOLE OF IT. A name that is not unique is one name for several unknowns,
 * and every predicate over any of them then decides all of them — core/loader/data_block.c states that rule
 * for a document's data blocks and it is the same rule here. Two endpoints on one host, and one path on two
 * hosts, are different unknowns; only the whole serialized URL separates all of them. There is nothing shorter
 * that does, so there is no shorter name.
 *
 * WHY IT IS PERCENT-ENCODED, AND WHY THAT IS THE URL STANDARD'S OWN ANSWER RATHER THAN AN ESCAPE INVENTED
 * HERE. The name is spelled into an @H shape between braces, and that shape is read back as a PATH: the
 * consumer substitutes a hole by matching `/\{([^}\/]+)\}/`, and solver/endpoint.c's path scan splits the
 * shape on `/` BEFORE it looks for a brace. An address written raw therefore does not merely fail to
 * substitute — `{https://h/cfg.json}.region` turns one path segment into five, the shape and the concrete
 * example stop having the same segment count, and EVERY path parameter on that request loses the value the run
 * measured, with nothing to say so. core/byte_reader.h states the obligation this discharges: a component
 * naming a byte sequence after an address owes that address a slash-free spelling.
 * URL §1.3 Percent-encoded bytes is where that spelling already exists. Its COMPONENT percent-encode set is
 * defined for exactly this: "could also be used by other standards to percent-encode data that can then be
 * embedded in a URL's path, query, or fragment". It contains `/`, `{` and `}` — and, because it is the
 * userinfo set plus U+0024 ($) to U+0026 (&) inclusive, it contains `%` as well, which is what makes the
 * encoding INJECTIVE: `http://h/a%2Fb` and `http://h/a/b` are two addresses and stay two names, where an
 * escape that left `%` alone would collapse them into one and hand two servers' replies a single identity.
 *
 * WHAT IT IS NOT. It is not a display convenience and it is not decoded anywhere: the name is the value's
 * IDENTITY (solver/concolic.h), so it is compared, keyed and substituted as these bytes. A reviewer reading
 * `{https%3A%2F%2Fapi.example.com%2Fv1%2Fconfig}.region` percent-decodes it the way they would percent-decode
 * any other component of a URL. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/fetch/reply_source.h"
#include "core/url/url.h"

char *reply_source_name(const char *url, size_t len)
{
    char *name;

    /* NO ADDRESS IS A STATEMENT, NOT AN UNKNOWN — see the header. The two callers ask it of a reply that may
       not have one (`new Response("x")`), and the answer is that those bytes are the page's own. */
    if (!url || len == 0)
        return NULL;
    /* url_percent_encode never answers NULL — url.c's ustr_take CHECKs its own allocation and an empty string
       is a string — so a NULL here would be that contract having changed under this file. */
    name = url_percent_encode(url, len, URL_SET_COMPONENT);
    DCHECK(name != NULL, "URL §1.3's percent-encode answered nothing for an address that is not empty");
    DCHECK(!strpbrk(name, "{}/"),
           "a reply's source name still carries a brace or a slash after URL §1.3 Percent-encoded bytes' "
           "component percent-encode set — the @H surface splits a shape on `/` before it looks for a hole, "
           "so such a name does not merely fail to substitute, it shreds the path segment it sits in and takes "
           "that segment's measured value with it");
    return name;
}
