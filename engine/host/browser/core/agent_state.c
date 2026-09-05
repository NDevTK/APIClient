/* See agent_state.h. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "core/agent_state.h"

typedef enum {
    SLOT_ID,     /* pre-init: -1 */
    SLOT_FLAG,   /* pre-init: 0 */
    SLOT_CLASS,  /* pre-init: 0 */
    SLOT_ATOM,   /* pre-init: JS_ATOM_NULL */
    SLOT_VALUE,  /* pre-init: JS_UNDEFINED */
    SLOT_PTR,    /* pre-init: NULL */
} SlotKind;

typedef struct {
    const char *component;   /* the component, as core/platform.c's row names it */
    const char *what;        /* what the slot IS — the second half of the assert */
    const void *slot;        /* the static itself; a static outlives the agent, so reading one here is safe */
    SlotKind    kind;
} AgentSlot;

/* GROWN, NOT CAPPED. A fixed table would be a bound on how much state one browser may hold, and the number it
   would be set from is the number of components that happen to exist today. */
static AgentSlot *g_slots;
static int        g_n, g_cap;

static void slot_declare(const char *component, const void *slot, const char *what, SlotKind kind)
{
    int i;

    DCHECK(component != NULL && *component,
           "a slot of agent state was declared by no component — the assert a forgotten release fires names "
           "the component, and an unnamed one can only report an address");
    DCHECK(what != NULL && *what,
           "a slot of agent state was declared with no description — a `@WHY` reading a variable name tells "
           "its reader nothing about what the release failed to give back");
    DCHECK(slot != NULL, "a slot of agent state was declared at no address");
    for (i = 0; i < g_n; i++)
        DCHECK(g_slots[i].slot != slot,
               "one static was declared as agent state twice — the second declaration is either a component "
               "naming its own slot again or two components claiming one piece of state, and the release "
               "column can only be the inverse of ONE declaration");
    if (g_n == g_cap) {
        int nc = g_cap ? g_cap * 2 : 32;
        AgentSlot *n = realloc(g_slots, (size_t)nc * sizeof *n);

        CHECK(n != NULL, "the agent-state registry could not grow — what it holds is the only record of what "
                         "this browser's releases owe, and a dropped row is a release nothing checks");
        g_slots = n;
        g_cap = nc;
    }
    g_slots[g_n].component = component;
    g_slots[g_n].what = what;
    g_slots[g_n].slot = slot;
    g_slots[g_n].kind = kind;
    g_n++;
}

void agent_state_id(const char *c, const int *slot, const char *what) { slot_declare(c, slot, what, SLOT_ID); }
void agent_state_flag(const char *c, const int *slot, const char *what) { slot_declare(c, slot, what, SLOT_FLAG); }
void agent_state_class(const char *c, const JSClassID *slot, const char *what) { slot_declare(c, slot, what, SLOT_CLASS); }
void agent_state_atom(const char *c, const JSAtom *slot, const char *what) { slot_declare(c, slot, what, SLOT_ATOM); }
void agent_state_value(const char *c, const JSValue *slot, const char *what) { slot_declare(c, slot, what, SLOT_VALUE); }
void agent_state_ptr(const char *c, const void *slot, const char *what) { slot_declare(c, slot, what, SLOT_PTR); }

int agent_state_count(const char *component)
{
    int i, n = 0;

    DCHECK(component != NULL, "the agent-state registry was asked about no component");
    for (i = 0; i < g_n; i++)
        if (strcmp(g_slots[i].component, component) == 0) n++;
    return n;
}

bool agent_state_slot(int i, const char **component, const char **what)
{
    DCHECK(component != NULL && what != NULL,
           "the agent-state registry was walked with nowhere to put the declaration it was asked for");
    DCHECK(i >= 0, "the agent-state registry was walked from before its first declaration");
    /* NOT A BOUND. The end of the list is a FACT the caller reads to stop, which is what makes the walk
       unwritable as a fixed count the caller would have to keep in step with the registry. */
    if (i >= g_n) return false;
    *component = g_slots[i].component;
    *what = g_slots[i].what;
    return true;
}

/* IS THIS SLOT WHERE A FRESH PROCESS WOULD HAVE FOUND IT? Each read goes through the slot's own declared type,
   which is why there is a function per kind rather than a cast per read — except the pointer, whose bytes are
   compared against a null pointer's because no object pointer may be read through a `void *` lvalue. */
static int slot_is_pre_init(const AgentSlot *s)
{
    switch (s->kind) {
    case SLOT_ID:    return *(const int *)s->slot == -1;
    case SLOT_FLAG:  return *(const int *)s->slot == 0;
    case SLOT_CLASS: return *(const JSClassID *)s->slot == 0;
    case SLOT_ATOM:  return *(const JSAtom *)s->slot == JS_ATOM_NULL;
    case SLOT_VALUE: return JS_IsUndefined(*(const JSValue *)s->slot);
    case SLOT_PTR:   { void *null = NULL; return memcmp(s->slot, &null, sizeof null) == 0; }
    }
    DFAIL("a slot of agent state has a kind this file does not have — every kind IS a pre-init value, so a "
          "kind with no case is a slot whose released state is undefined");
    return 0;
}

/* THE WRITE SIDE OF slot_is_pre_init, PAIRED WITH IT ARM FOR ARM — see agent_state.h for why the undo is
   derived from the declarations rather than written out a second time in each release.
   THE CONST IS DROPPED HERE AND NOWHERE ELSE, and it is dropped over a real object rather than a claim: every
   declared slot is a MUTABLE static, which its own `_init` proves by writing it. The `const` on the declaring
   entry points says the DECLARATION does not write the slot, and that stays true — this is a different entry
   point and it is the only one that writes.
   THE POINTER ARM MIRRORS THE READ'S memcmp WITH A memcpy for the reason agent_state.h gives for the read: a
   slot may hold a function pointer, and neither a read nor a write of one may go through a `void *` lvalue.
   Copying a null object pointer's bytes over one is what the check ALREADY assumes when it compares them, so
   the two arms make the same assumption or neither does — and the assert below is what makes that assumption
   fire instead of being trusted. */
static void slot_set_pre_init(const AgentSlot *s)
{
    void *p = (void *)s->slot;

    switch (s->kind) {
    case SLOT_ID:    *(int *)p = -1; break;
    case SLOT_FLAG:  *(int *)p = 0; break;
    case SLOT_CLASS: *(JSClassID *)p = 0; break;
    case SLOT_ATOM:  *(JSAtom *)p = JS_ATOM_NULL; break;
    case SLOT_VALUE: *(JSValue *)p = JS_UNDEFINED; break;
    case SLOT_PTR:   { void *null = NULL; memcpy(p, &null, sizeof null); break; }
    default:
        DFAIL("a slot of agent state has a kind this file cannot undo — every kind IS a pre-init value, so a "
              "kind the write side has no case for is a slot whose release would silently do nothing");
        return;
    }
    /* TWO-SIDED, AND NOT DECORATION: this is the one place the engine ASSUMES a set of bytes spells a kind's
       pre-init value, and the pointer arm assumes it hardest. Asking the reader back is what turns that
       assumption into something that fires at the write instead of at the next agent's `_init`. */
    DCHECK(slot_is_pre_init(s),
           "undoing a declaration of agent state did not put the slot back at its pre-init value — the write "
           "side and the read side of one kind disagree, and the release that just ran will be reported as "
           "not having run");
}

void agent_state_undo(const char *component)
{
    int i, n = 0;

    DCHECK(component != NULL && *component, "a component undid its agent state without naming itself");
    for (i = 0; i < g_n; i++) {
        if (strcmp(g_slots[i].component, component) != 0) continue;
        slot_set_pre_init(&g_slots[i]);
        n++;
    }
    /* THE NAME IS WRITTEN TWICE — here and at every declaration — so the state this refuses is the two
       spellings differing. A no-op would leave every one of that component's slots set and report the failure
       against the DECLARING name, several stages later, as a release that never ran. */
    DCHECKF(n > 0,
            "`%s` undid its agent state and this registry holds no declaration under that name. The component "
            "argument is spelled once here and once at each agent_state_* call, and it is core/platform.c's "
            "ROW name rather than the file's — for a sub-component, the row whose release reaches it. Either "
            "this spelling is wrong or the declarations' is",
            component);
}

void agent_state_check_released(void)
{
#if APICLIENT_DEV
    int i;

    for (i = 0; i < g_n; i++) {
        /* The message is the COMPONENT and the STATE, because the reader of this `@WHY` is standing at a
           teardown that already ran: what they need is which release to go and finish, not an address. */
        if (slot_is_pre_init(&g_slots[i])) continue;
        DFAILF("%s did not undo its declaration: %s is still set after the release column ran. A handle left "
               "behind is invisible to both of JS_FreeRuntime's censuses — the reference WAS given back — and "
               "the only code that ever reads it is this component's own next _init, which consults it to "
               "decide it has already been declared. The next agent in this process gets a component that "
               "reports itself built and whose every other handle is null.",
               g_slots[i].component, g_slots[i].what);
    }
#endif
}

void agent_state_reset(void)
{
    free(g_slots);
    g_slots = NULL;
    g_n = g_cap = 0;
}
