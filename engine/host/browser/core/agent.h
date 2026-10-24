/* HTML §8.1.2.1 "Integration with the JavaScript agent formalism"'s create an agent: what each agent owns beside
   its ECMA-262 §9.6 Agent Record, starting with step 4's "Set agent's event loop to a new event loop". */
#ifndef ENGINE_HOST_BROWSER_CORE_AGENT_H
#define ENGINE_HOST_BROWSER_CORE_AGENT_H

#include "quickjs.h"

/* A component's per-agent install: builds what one agent owns into the agent's host slots (quickjs.h's
   JS_SetAgentSlot). `ctx` is the realm the agent's first realm will be, allocated in but not yet named to it. */
typedef void (*AgentInstall)(JSContext *ctx, JSAgentRecord *agent);

/* Declared from a component's declare-column init, before any agent is installed. */
void agent_declare_install(AgentInstall install);
/* Runs every declared install for `agent`, once, after the declare column and before its first realm is built. */
void agent_install(JSContext *ctx, JSAgentRecord *agent);
/* Gives the install list back with the platform. */
void agent_installs_free(void);

#endif
