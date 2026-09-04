/* SUBRESOURCE INTEGRITY §3.3.2 "Parse metadata" — see subresource_integrity.h for why this is a component of
 * its own and why it is a cursor.
 *
 * THE ALGORITHM IS TRANSCRIBED, NOT RECONSTRUCTED. §3.3.2, whose step 2 is one loop over the items of the
 * string:
 *
 *   step 1     Let result be the empty set.
 *   step 2     For each item returned by splitting metadata on spaces:
 *   step 2.1     Let expression-and-options be the result of splitting item on U+003F (?).
 *   step 2.2     Let algorithm-expression be expression-and-options[0].
 *   step 2.3     Let base64-value be the empty string.
 *   step 2.4     Let algorithm-and-value be the result of splitting algorithm-expression on U+002D (-).
 *   step 2.5     Let algorithm be algorithm-and-value[0].
 *   step 2.6     If algorithm is not a valid SRI hash algorithm token, then continue.
 *   step 2.7     If algorithm-and-value[1] exists, set base64-value to algorithm-and-value[1].
 *   step 2.8     Let metadata be the ordered map with alg mapped to algorithm and val to base64-value.
 *   step 2.9     Append metadata to result.
 *   step 3     Return result.
 *
 * BOTH SPLITS ARE INFRA'S `strictly split`, WHICH IS THE ONE THING HERE A READER MIGHT ASSUME AWAY. §3.3.2's
 * three splits each link to it rather than to `split a string on ASCII whitespace`, so a run of two spaces
 * yields an EMPTY item between them rather than collapsing, a tab is NOT a delimiter, and the token count is
 * always one more than the delimiter count. Nothing has to special-case that: an empty item has an empty
 * algorithm, which step 2.6 refuses, and a tab lands inside an algorithm that is then not a valid token
 * either — so a permissive reading and this one differ in NO answer, which is exactly why writing the
 * permissive one would never have been caught.
 *
 * AND STEP 2.7 TAKES INDEX 1 OF A FULL SPLIT, NOT EVERYTHING AFTER THE FIRST U+002D. Those two readings differ
 * on a real string: CSP §2.3.1's base64-value admits `-` and `_` (it is base64url as well as base64), so
 * `sha256-a-b` splits to three parts and `algorithm-and-value[1]` is `a`, with `-b` discarded. That is the
 * standard's own arithmetic and it is transcribed rather than repaired here, because the alternative is this
 * file quietly holding a different SRI from the one every other implementation reads. Its observable effect is
 * that such a digest matches nothing, which is the safe direction and is visible as a subresource refused. */
#include <string.h>

#include "check.h"
#include "core/fetch/subresource_integrity.h"

/* §2 "Key Concepts and Terminology": "The valid SRI hash algorithm token set is the ordered set" of sha256,
   sha384 and sha512 — and, in the same section, "A string is a valid SRI hash algorithm token if its ASCII
   lowercase is contained in the valid SRI hash algorithm token set", which is what makes the comparison below
   case-insensitive rather than exact.
   IT IS THIS STANDARD'S SET AND NOT CSP'S PRODUCTION, and the two are not shared even though they name the
   same three algorithms today. core/frame/csp_source_list.c transcribes CSP §2.3.1's `hash-algorithm`, which
   is a different document's grammar that this one merely happens to agree with; a table shared between them
   would make each file's answer depend on the other standard's edition, and the day one gains an algorithm the
   shared table would silently give it to both. */
static bool sri_valid_hash_algorithm_token(const char *p, size_t n)
{
    static const char *const TOKENS[] = { "sha256", "sha384", "sha512" };
    size_t t, i;

    for (t = 0; t < sizeof TOKENS / sizeof TOKENS[0]; t++) {
        if (n != strlen(TOKENS[t])) continue;
        for (i = 0; i < n; i++) {
            char c = p[i];

            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');   /* §2's ASCII lowercase */
            if (c != TOKENS[t][i]) break;
        }
        if (i == n) return true;
    }
    return false;
}

void sri_parse_metadata(SriMetadataParse *it, const char *metadata, size_t len)
{
    DCHECK(it != NULL, "SRI §3.3.2 was started with no cursor to write — the cursor is the caller's own "
                       "storage and this component allocates none");
    DCHECK(metadata != NULL || len == 0,
           "SRI §3.3.2 was given a length with no bytes — the string it parses is Fetch §2.2.5's integrity "
           "metadata of some request, whose initial value is the empty string, and a caller that has none "
           "states that with a zero length rather than with a pointer nobody can read");
    /* An absent string is the empty one, so that step 2's arithmetic runs over a readable pointer: the loop
       below forms `p + i` on every turn, and forming that from a null pointer is undefined even where the
       length stops it being read. */
    it->p = metadata ? metadata : "";
    it->n = len;
    it->i = 0;
}

bool sri_parse_metadata_next(SriMetadataParse *it, SriHashExpression *out)
{
    DCHECK(it != NULL && out != NULL,
           "SRI §3.3.2's loop was advanced with no cursor or nowhere to place the expression it yields");
    /* `i > n` is the state step 3 has been reached in. The bound is `<=` and not `<` because strictly split
       yields one more item than there are delimiters, so the empty string is ONE empty item and a string
       ending in a space has an empty item after it. */
    while (it->i <= it->n) {
        const char *item = it->p + it->i;
        size_t end = it->i, item_len, k, alg_len, val_from, val_to;

        /* §3.3.2 STEP 2 — the next item of the strict split on U+0020. */
        while (end < it->n && it->p[end] != ' ') end++;
        item_len = end - it->i;
        it->i = end + 1;

        /* §3.3.2 STEPS 2.1-2.2 — `expression-and-options[0]`, which is everything before the first U+003F. A
           string with no U+003F strictly splits to one part, and that part is the whole item. */
        for (k = 0; k < item_len && item[k] != '?'; k++) { }
        item_len = k;

        /* §3.3.2 STEPS 2.4-2.5 — `algorithm-and-value[0]`, everything before the first U+002D. */
        for (alg_len = 0; alg_len < item_len && item[alg_len] != '-'; alg_len++) { }

        /* §3.3.2 STEP 2.6. */
        if (!sri_valid_hash_algorithm_token(item, alg_len))
            continue;

        /* §3.3.2 STEP 2.3, AND ITS STEP 2.7 — the empty string unless `algorithm-and-value[1]` exists, which
           it does exactly when the split produced a second part, which is exactly when a U+002D was found. Its
           extent runs to the NEXT U+002D, because it is one part of a full split and not the remainder of the
           string. */
        val_from = val_to = item_len;
        if (alg_len < item_len) {
            val_from = alg_len + 1;
            for (val_to = val_from; val_to < item_len && item[val_to] != '-'; val_to++) { }
        }

        /* §3.3.2 STEPS 2.8-2.9 — the map, yielded to the caller in place of an append. */
        out->alg = item;
        out->alg_len = alg_len;
        out->val = item + val_from;
        out->val_len = val_to - val_from;
        return true;
    }
    return false;
}
