/* The acts a run performed, in performance order, as the `acts` a PoC or a recipe states. See act_list.c. */
#ifndef ENGINE_HOST_SOLVER_ACT_LIST_H
#define ENGINE_HOST_SOLVER_ACT_LIST_H

struct Flow;

/* The acts `f` performed, as the envelope's `acts` array in performance order (its drives' acts and its
   modelled user acts), malloc'd, or NULL when `f` has neither. `*no_act` is set where a called member of the
   chain has no act a page user can perform: a browser never makes that call. Members a resumed drive has not
   built a call for yet are not part of the path. A message act states the origin and data `f` delivers for it,
   or else what their domains compose (message_compose_origin, message_compose): then `*unsolved` is raised, the
   compositions are appended to `*dl` (flow_deliveries_text's format, malloc'd, NULL for none) and `*refused`
   takes the first data composition's refusal (malloc'd). */
char *act_list_json(const struct Flow *f, int *no_act, int *unsolved, char **refused, char **dl);
/* The origin and data of every message act `f`'s chain performed, each composed from its domain
   (message_compose_origin, message_compose), with `payload` at `inject` in the message whose data `inject` is or
   is a member of: the deliveries (in flow_deliveries_text's format, malloc'd) a candidate runs under. NULL
   where the chain has no message act or a composition refuses. */
char *act_list_message_deliveries(const struct Flow *f, const char *inject, const char *payload);

#endif
