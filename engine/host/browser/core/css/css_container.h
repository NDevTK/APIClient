/* CSS Conditional 5 §5.1 "Creating Query Containers: the container-type property", §5.2 "Naming Query
 * Containers: the container-name property" and §5.3 "Creating Named Containers: the container shorthand": the
 * grammars of the three, which lexbor's registry does not carry. The cascade validates a declaration through
 * them (core/css/css_shorthand.h) and the computed value is read back through them
 * (core/css/css_computed_value.h's `css_computed_container_type`), so the admitted grammar and the set a
 * consumer reads are one. Blink's ContainerType and the container-name list under their own names. */

#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_CONTAINER_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_CONTAINER_H

#include <stdbool.h>

/* §5.1's `normal | [ [ size | inline-size ] || scroll-state ]` as a set; `normal` is the empty set. */
enum {
    CSS_CONTAINER_TYPE_SIZE = 1u << 0,
    CSS_CONTAINER_TYPE_INLINE_SIZE = 1u << 1,
    CSS_CONTAINER_TYPE_SCROLL_STATE = 1u << 2
};

/* `text` against §5.1's `Value:` line. False for a value outside it, with `*bits` unwritten: such a value is
   page input and is refused, never asserted. */
bool css_container_type_parse(const char *text, unsigned *bits);

/* The set as CSSOM §6.7.2 "Serializing CSS Values" writes it, in §5.1's canonical order (per grammar). OWNED. */
char *css_container_type_serialize(unsigned bits);

/* `text` against §5.2's `none | <custom-ident>+`, each name serialized as CSSOM §2.1 serializes an identifier
   and the list joined by single spaces. NULL for a value outside it. OWNED. */
char *css_container_name_specified(const char *text);

/* Whether the computed `container-name` `names` (the serialization above) holds `name`, a `<container-name>` as
   core/css/css_at_rule_prelude.h serializes it. Names compare case-sensitively, as `<custom-ident>`s do. */
bool css_container_name_has(const char *names, const char *name);

/* §5.3's `<'container-name'> [ / <'container-type'> ]?`: the specified value it gives `longhand`, one of the
   two longhands; "if <'container-type'> is omitted, it is reset to its initial value". NULL for a value outside
   the grammar. A CSS-wide keyword is the caller's (css-cascade-5 §3). OWNED. */
char *css_container_shorthand_component(const char *value, const char *longhand);

/* The shorthand's value for the longhands' specified `values` (name, type), CSSOM §6.7.2's shortest form. */
char *css_container_shorthand_serialize(const char *const *values);

#endif
