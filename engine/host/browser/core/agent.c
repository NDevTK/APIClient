/* The per-agent install column; see agent.h. */
#include <stdbool.h>
#include <stdlib.h>

#include "check.h"
#include "core/agent.h"
#include <lexbor/core/base.h>   /* lexbor_realloc: the allocator the engine gives its refusal edge */

static AgentInstall *g_list;
static int g_n, g_cap;
static bool g_agent_installed;   /* an agent has run the list, so a later declaration would miss it */

void agent_declare_install(AgentInstall install)
{
    int i;

    DCHECK(install != NULL, "a component declared a per-agent install with no install to run");
    DCHECK(!g_agent_installed,
           "a per-agent install was declared after an agent had already run the list, so that agent lacks what "
           "the install builds; declare it in the component's declare-column init with the others");
    for (i = 0; i < g_n; i++)
        DCHECK(g_list[i] != install, "a per-agent install was declared twice");
    if (g_n == g_cap) {
        int cap = g_cap ? g_cap * 2 : 4;
        AgentInstall *g = lexbor_realloc(g_list, (size_t)cap * sizeof *g);
        CHECK(g != NULL, "agent: OOM recording a per-agent install");
        g_list = g;
        g_cap = cap;
    }
    g_list[g_n++] = install;
}

void agent_install(JSContext *ctx, JSAgentRecord *agent)
{
    int i;

    DCHECK(ctx != NULL && agent != NULL, "a per-agent install ran with no realm to allocate in or no agent");
    g_agent_installed = true;
    for (i = 0; i < g_n; i++)
        g_list[i](ctx, agent);
}

void agent_installs_free(void)
{
    free(g_list);
    g_list = NULL;
    g_n = g_cap = 0;
    g_agent_installed = false;
}
