/* IDNA — UTS-46 domain-to-ASCII over RFC 3492 Punycode. See idna.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_URL_IDNA_H
#define ENGINE_HOST_BROWSER_CORE_URL_IDNA_H
#include <stddef.h>
#include <stdint.h>

/* WHATWG URL §4.2's "domain to ASCII". `domain` is the percent-decoded UTF-8 bytes the host parser produced.
   Returns 0 with `*out` a malloc'd NUL-terminated A-label domain, or -1 for the spec's FAILURE. */
int  idna_domain_to_ascii(const char *domain, size_t len, char **out, size_t *out_len);
void idna_free(char *s);

/* RFC 3492, both directions, exported because they are exact and testable on their own. */
int  idna_punycode_encode(const uint32_t *in, int in_len, char *out, int out_cap);
int  idna_punycode_decode(const char *in, size_t in_len, uint32_t *out, int out_cap);

#endif
