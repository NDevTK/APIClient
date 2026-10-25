/* The acts a run performed, in performance order, as the `acts` a PoC or a recipe states. See act_list.c. */
#ifndef ENGINE_HOST_SOLVER_ACT_LIST_H
#define ENGINE_HOST_SOLVER_ACT_LIST_H

struct Flow;

/* The acts `f` performed, as the envelope's `acts` array in performance order (its drives' acts and its
   modelled user acts), malloc'd, or NULL when `f` has neither. `*no_act` is set where a called member of the
   chain has no act a page user can perform: a browser never makes that call. Members a resumed drive has not
   built a call for yet are not part of the path. A message act states the data `f` delivers for it, or else
   the data its domain composes (message_compose): then `*unsolved` is raised, the composition is appended to
   `*dl` (flow_deliveries_text's format, malloc'd, NULL for none) and `*refused` takes the first composition's
   refusal (malloc'd). */
char *act_list_json(const struct Flow *f, int *no_act, int *unsolved, char **refused, char **dl);

#endif
