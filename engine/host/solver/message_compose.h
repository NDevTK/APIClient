/* The data an attacker document posts, composed from what the running flow's path decided about a message's
   unknown `data`. See message_compose.c. */
#ifndef ENGINE_HOST_SOLVER_MESSAGE_COMPOSE_H
#define ENGINE_HOST_SOLVER_MESSAGE_COMPOSE_H

/* The JSON text of the value the running flow's constraint admits for the declared root `root` (a
   `message.data:<id>` source), with `payload` at the member read `inject` (its `src`, or `root` itself), malloc'd.
   NULL with `*why` (malloc'd) naming what the domain cannot yet compose. A candidate, never a proof: only a fire
   with this value delivered shows a sender reproduces the path. */
char *message_compose(const char *root, const char *inject, const char *payload, char **why);

#endif
