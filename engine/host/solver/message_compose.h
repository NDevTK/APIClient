/* The data an attacker document posts, composed from what the running flow's path decided about a message's
   unknown `data`. See message_compose.c. */
#ifndef ENGINE_HOST_SOLVER_MESSAGE_COMPOSE_H
#define ENGINE_HOST_SOLVER_MESSAGE_COMPOSE_H

/* The JSON text of the value the running flow's constraint admits for the declared root `root` (a
   `message.data:<id>` source), with `payload` at the member read `inject` (its `src`, or `root` itself), malloc'd.
   NULL with `*why` (malloc'd) naming what the domain cannot yet compose. A candidate, never a proof: only a fire
   with this value delivered shows a sender reproduces the path. */
char *message_compose(const char *root, const char *inject, const char *payload, char **why);
/* The origins an attacker document may post from, for the message whose `origin` source is `origin_src`: HTML
   §9.3.3 stamps the sender's own origin, so each is one the attacker can own. The constants the page compared
   the origin with (the operands of calls over it and the running flow's pins on it) shape them: a subdomain of
   the attacker's own site carrying every constant as labels, and for each constant that is a domain not
   beginning with a separator, a registrable domain of the attacker's own ending in it. Candidates, which the
   fire decides between; nothing reads which check the page wrote. Answers how many (at least one) and the
   malloc'd list in `*out`, each malloc'd. */
int message_compose_origins(const char *origin_src, char ***out);

#endif
