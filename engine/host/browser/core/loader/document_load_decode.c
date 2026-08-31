/* HTML §13.2.3.2 "Determining the character encoding" over a RESPONSE, and the Encoding §6.1 "Legacy hooks for
 * standards" decode its answer names. Nothing else.
 *
 * WHY THIS IS A COMPONENT AND NOT TWO CALLS AT EACH LOADER. It is the same rule
 * core/loader/document_load_type.c is written under, one question over: a dispatch deciding WHAT A SET OF
 * BYTES IS must be asked at EVERY entry that builds the thing out of them, and an entry that skips it does not
 * report an absent capability — it produces a plausible wrong answer. §7.4.5's WHICH-DOCUMENT dispatch and
 * §13.2.3.2's WHICH-DECODER dispatch are two halves of one act and they were reachable from different sets of
 * entries: every entry asked the first, one entry asked the second.
 *
 * WHAT THE ENTRIES THAT DID NOT ASK PRODUCED. The response's own bytes went to a tokenizer that reads UTF-8,
 * so a document served `windows-1252`, or one carrying `<meta charset=shift_jis>`, was replaced with U+FFFD
 * above 0x7F before HTML §13.2.6 "Tree construction" saw a token — and its Document kept DOM §4.5 "Interface
 * Document"'s default,
 * so `document.characterSet` answered UTF-8. Nothing crashed and nothing logged, which is what makes it the
 * defect shape rather than a gap: the tree is REAL, the string is WRONG, and no reader of either can tell.
 *
 * AND THE HALF THAT IS NOT HERE IS HERE FOR A REASON. §13.2.3.2's own steps are
 * core/html/html_encoding_sniff.c's and the decode is core/encoding/encoding.c's; this file holds only the
 * ORDER of them and the one input neither of them may compute for itself — which `Content-Type` value the
 * algorithm reads. That input is the whole reason a component exists rather than a shared helper: MIME
 * Sniffing §5.1 "Interpreting the resource metadata" and §13.2.3.2 read the SAME header by different rules,
 * so a caller that read it once and handed the value to both would be answering one of the two algorithms
 * with the other one's operand.
 */
#include <stdlib.h>

#include "core/loader/document_load_decode.h"
#include "core/encoding/encoding.h"
#include "core/fetch/headers.h"
#include "core/html/html_encoding_sniff.h"
#include "check.h"

int document_load_decode(char **out, size_t *out_len,
                         const HeaderList *response_headers,
                         const void *body, size_t body_len,
                         int parent_encoding)
{
    char  *content_type;
    char  *decoded;
    size_t decoded_len = 0;
    int    encoding;

    DCHECK(out != NULL && out_len != NULL && response_headers != NULL && body != NULL,
           "HTML §13.2.3.2 Determining the character encoding was asked without a response — the algorithm "
           "takes the out-of-band metadata AND all the bytes available so far, so half of it is an encoding "
           "determined for some other response, and no bytes at all is a caller with no response that is not "
           "asking this question: HTML §7.4 Navigation and session history's initial about:blank keeps the "
           "utf-8 default DOM §4.5 Interface Document gives every Document");
    /* §13.2.3.2's OUT-OF-BAND METADATA, IN THE FORM THAT ALGORITHM DEFINES. Fetch §2.2.2 Headers get JOINS
       duplicate values with a comma and a space, and Fetch §3.5 legacy extract an encoding runs over the
       record that joined value extracts to. The UNJOINED last value belongs to MIME Sniffing §5.1 and is read
       inside the component that runs it — see the header on why one read cannot serve both. NULL is a
       response that carried no Content-Type at all, which is a positive statement §13.2.3.2 branches on. */
    content_type = header_list_get(response_headers, "content-type");
    encoding = html_encoding_sniff((const char *)body, body_len, content_type, parent_encoding);
    free(content_type);
    /* §13.2.3.2's LAST STEP IS A DEFAULT, so the algorithm cannot fail to answer, and an id that names no
       encoding is a component that stopped running the algorithm rather than an input this one may carry. */
    DCHECK(encoding >= 0,
           "HTML §13.2.3.2 Determining the character encoding answered nothing — its last step is the default "
           "encoding, so the algorithm always answers, and a caller of this component has no arm for an "
           "absent one to take");
    /* Encoding §6.1's decode, which is what §13.2.3.1 Parsing with a known character encoding then makes the
       parser operate on. THE BOM OVERRULES THE LABEL and that is the standard's own deliberate violation of
       the Content-Type semantics, so the WHOLE byte sequence goes over rather than a suffix this file trimmed
       past a mark it recognised. */
    decoded = encoding_decode((const char *)body, body_len, encoding, &decoded_len);
    CHECK(decoded != NULL,
          "OOM decoding a response into the UTF-8 the HTML tokenizer takes — the DOM this parse produces is "
          "what every flow of this instance reads, so a decode that cannot allocate is not a document with no "
          "endpoints");
    *out     = decoded;
    *out_len = decoded_len;
    return encoding;
}
