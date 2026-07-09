/* Self-hosted JS prelude strings the engine evaluates at init — see prelude.c. */
#ifndef ENGINE_HOST_PRELUDE_H
#define ENGINE_HOST_PRELUDE_H
extern const char *ARRAY_PRELUDE_JS;   /* self-hosted Array/String iterators (bytecode, trampolined) */
extern const char *DEDUP_JS;           /* in-engine endpoint identity/dedup run at finalize */
#endif
