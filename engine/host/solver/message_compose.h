/* The data an attacker document posts, composed from what the running flow's path decided about a message's
   unknown `data`. See message_compose.c. */
#ifndef ENGINE_HOST_SOLVER_MESSAGE_COMPOSE_H
#define ENGINE_HOST_SOLVER_MESSAGE_COMPOSE_H

/* The JSON text of the value the running flow's constraint admits for the declared root `root` (a
   `message.data:<id>` source), with `payload` at the member read `inject` (its `src`, or `root` itself), malloc'd.
   NULL with `*why` (malloc'd) naming what the domain cannot yet compose. A candidate, never a proof: only a fire
   with this value delivered shows a sender reproduces the path. */
char *message_compose(const char *root, const char *inject, const char *payload, char **why);
/* The origin an attacker document posts from, for the message whose `origin` source is `origin_src`
   (malloc'd): HTML §9.3.3 stamps the sender's own origin, so it is an origin the attacker can register, a
   subdomain of its own site, whose host carries every constant the page compared the origin with (the
   operands of calls over it and the running flow's pins on it) as labels. A candidate: only a fire with it
   delivered shows a check passes. */
char *message_compose_origin(const char *origin_src);

#endif
