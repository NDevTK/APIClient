/* See agent_state.h. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "core/agent_state.h"

typedef enum {
    SLOT_ID,     /* pre-init: -1 */
    SLOT_REALM,  /* pre-init: JS_INVALID_CLASS_ID */
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
    /* WHERE THE DECLARATION WAS WRITTEN, captured by the macro at the caller — see agent_state.h. It is a
       string LITERAL's address and a line number, both of which outlive the agent exactly as `component` and
       `what` do, so this row owns none of the three and frees none of them. */
    const char *file;
    int         line;
    /* HAS THE RELEASE THAT GIVES THIS SLOT BACK RUN? Written by agent_state_reached, read by
       agent_state_undo, and false until somebody says otherwise — see agent_state.h. It is a fact about the
       declaring FILE rather than about this row, so every slot declared in one file carries the same answer;
       it is held per slot because the registry has no other index and a second one keyed on the file would be
       a table whose only reader is this bit.
       WRITTEN IN BOTH BUILDS, READ IN ONE. Gating the field would give this struct and agent_state_reached two
       shapes for the sake of one byte and a teardown-time loop, and a check that exists in only some builds is
       the shape quickjs.c's leak censuses were repaired away from. */
    bool        reached;
} AgentSlot;

/* GROWN, NOT CAPPED. A fixed table would be a bound on how much state one browser may hold, and the number it
   would be set from is the number of components that happen to exist today. */
static AgentSlot *g_slots;
static int        g_n, g_cap;

static void slot_declare(const char *component, const void *slot, const char *what, SlotKind kind,
                         const char *file, int line)
{
    int i;

    /* THE SITE IS REQUIRED AND NOT TOLERATED-IF-ABSENT: every caller reaches this through agent_state.h's
       macro, which spells __FILE__ at the call, so a null here is a hand-written call that bypassed the one
       spelling — and an address that may be absent is one no message can rely on. */
    DCHECK(file != NULL && *file && line > 0,
           "a slot of agent state was declared with no site — the declaring file and line are captured by the "
           "macro in core/agent_state.h at the CALL, so a declaration without them reached this registry past "
           "that macro and every assert about it can name a component but no file to edit");
    DCHECKF(component != NULL && *component,
            "a slot of agent state was declared by no component at %s:%d — the assert a forgotten release "
            "fires names the component, and an unnamed one can only report an address", file, line);
    DCHECKF(what != NULL && *what,
            "a slot of agent state was declared with no description at %s:%d — a `@WHY` reading a variable "
            "name tells its reader nothing about what the release failed to give back", file, line);
    DCHECKF(slot != NULL, "a slot of agent state was declared at no address, at %s:%d", file, line);
    for (i = 0; i < g_n; i++)
        /* BOTH SITES, AND THAT IS THE WHOLE REPAIR HERE: the two readings this abort offers — one component
           naming its own slot twice, or two components claiming one piece of state — are told apart by
           whether the two declaring FILES are the same, which is the one fact the message used to omit. */
        DCHECKF(g_slots[i].slot != slot,
                "one static was declared as agent state twice — `%s` at %s:%d (%s) and `%s` at %s:%d (%s) name "
                "ONE address. Either one component declared its own slot again, or two components claim one "
                "piece of state; the declaring files above say which. The release column can only be the "
                "inverse of ONE declaration",
                g_slots[i].component, g_slots[i].file, g_slots[i].line, g_slots[i].what,
                component, file, line, what);
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
    g_slots[g_n].file = file;
    g_slots[g_n].line = line;
    /* NOT `calloc`-CLEAN: the row above is `realloc`'d, so a slot's bytes are whatever the last agent left
       there. The one field whose pre-init value is not written by the caller is written here. */
    g_slots[g_n].reached = false;
    g_n++;
}

void agent_state_id_at(const char *c, const int *slot, const char *what, const char *f, int l) { slot_declare(c, slot, what, SLOT_ID, f, l); }
/* A PER-REALM VALUE SLOT — see agent_state.h for why it is its own kind and not an id. The ONE thing this
   entry does that agent_state_id could not is the assert: a realm slot is HANDED OUT BY core/realm.c's
   realm_value_declare, whose body is JS_NewClassID plus JS_NewClass and whose local starts at 0, so it always
   mints and what it returns is a real class id — which is `rt->js_class_id_alloc` at the moment of the call
   and therefore never JS_INVALID_CLASS_ID. THE PREDICATE READ `*slot > 0` AND COULD NOT SURVIVE THE TYPE:
   a slot is a JSClassID, which is unsigned, so a range over the sign admits every value there is bar one and
   refuses the one state this assert is about only by accident. `!= JS_INVALID_CLASS_ID` names that state.
   IT IS NOT core/realm.c's JS_IsRegisteredClass, WHICH IS THE STRONGER QUESTION AND CANNOT BE ASKED HERE: a
   declaration carries no JSRuntime, and inventing a parameter to carry one would make every component's
   declaration depend on a runtime it does not otherwise need. What this entry can see is whether the slot has
   been assigned, and that is exactly the state a row declared above its assignment is in. A row declared while
   its slot still holds the component's own pre-init is a row that is COUNTED against the allocator and
   contributed nothing TO it, so core/platform.c's identity comes out with MORE DECLARED THAN MINTED and
   reports a mint nobody made. That identity is asserted now rather than merely made possible, so this assert
   is what keeps the two directions distinguishable there: without it, a declaration one line too early and a
   mint nobody declared cancel, and the sum says nothing. That is the accusing direction, and it is a state a
   diff reaches by putting the declaration one line too early.
   THE NULL IS LEFT TO slot_declare, deliberately: it refuses a null address in its own words and with the
   right remedy, and short-circuiting to it here is what keeps ONE message for that state rather than two. */
void agent_state_realm_slot_at(const char *c, const JSClassID *slot, const char *what, const char *f, int l)
{
    DCHECKF(slot == NULL || *slot != JS_INVALID_CLASS_ID,
            "`%s` declared a per-realm value slot (%s) at %s:%d that has not been minted yet — it reads %u, "
            "and core/realm.c's realm_value_declare never returns that: it hands back a class id, which this "
            "runtime's allocator starts at JS_CLASS_INIT_COUNT. The declaration stands BELOW the line that "
            "assigns the slot, because a row declared above one is counted as a class id this agent minted "
            "while the allocator was never asked for it",
            c ? c : "(unnamed)", what ? what : "(undescribed)", f ? f : "(no file)", l,
            slot ? *slot : (JSClassID)JS_INVALID_CLASS_ID);
    slot_declare(c, slot, what, SLOT_REALM, f, l);
}
void agent_state_flag_at(const char *c, const int *slot, const char *what, const char *f, int l) { slot_declare(c, slot, what, SLOT_FLAG, f, l); }
void agent_state_class_at(const char *c, const JSClassID *slot, const char *what, const char *f, int l) { slot_declare(c, slot, what, SLOT_CLASS, f, l); }
void agent_state_atom_at(const char *c, const JSAtom *slot, const char *what, const char *f, int l) { slot_declare(c, slot, what, SLOT_ATOM, f, l); }
void agent_state_value_at(const char *c, const JSValue *slot, const char *what, const char *f, int l) { slot_declare(c, slot, what, SLOT_VALUE, f, l); }
void agent_state_ptr_at(const char *c, const void *slot, const char *what, const char *f, int l) { slot_declare(c, slot, what, SLOT_PTR, f, l); }

int agent_state_count(const char *component)
{
    int i, n = 0;

    DCHECK(component != NULL, "the agent-state registry was asked about no component");
    for (i = 0; i < g_n; i++)
        if (strcmp(g_slots[i].component, component) == 0) n++;
    return n;
}

/* THE TWO CLASS-ID KINDS, COUNTED TOGETHER — see agent_state.h for what the number is the left-hand side of.
   THEY ARE COUNTED TOGETHER AND NOT SEPARATELY, and that is the property that makes the identity honest
   rather than a preference about tidiness: agent_state.h records that these two entries have byte-identical
   signatures, so the compiler cannot separate a class slot from a realm slot and a declaration routed to the
   wrong one of the pair is silent. A sum over both is unchanged by that mis-routing, so the identity answers
   about the one thing it is for — was every class id this window minted declared at all — rather than about
   which door it came through. The other kinds hold no class id and are not counted: an id, a flag, an atom,
   a JSValue and a pointer are each minted by something that is not this allocator, and adding one of them to
   this total would report a mint nobody made. */
int agent_state_class_id_count(void)
{
    int i, n = 0;

    for (i = 0; i < g_n; i++)
        if (g_slots[i].kind == SLOT_CLASS || g_slots[i].kind == SLOT_REALM) n++;
    return n;
}

bool agent_state_slot(int i, const char **component, const char **what, const char **file, int *line)
{
    DCHECK(component != NULL && what != NULL && file != NULL && line != NULL,
           "the agent-state registry was walked with nowhere to put the declaration it was asked for");
    DCHECK(i >= 0, "the agent-state registry was walked from before its first declaration");
    /* NOT A BOUND. The end of the list is a FACT the caller reads to stop, which is what makes the walk
       unwritable as a fixed count the caller would have to keep in step with the registry. */
    if (i >= g_n) return false;
    *component = g_slots[i].component;
    *what = g_slots[i].what;
    /* THE SITE IS HANDED BACK WITH THE STRINGS AND NOT AS A SECOND CALL, because the one walk that reads this
       is the one that decides a declaration names no row — and that verdict's two repairs are chosen by
       reading the declaring FILE. An address a caller has to ask for separately is one a message can be
       written without. */
    *file = g_slots[i].file;
    *line = g_slots[i].line;
    return true;
}

/* IS THIS SLOT WHERE A FRESH PROCESS WOULD HAVE FOUND IT? Each read goes through the slot's own declared type,
   which is why there is a function per kind rather than a cast per read — except the pointer, whose bytes are
   compared against a null pointer's because no object pointer may be read through a `void *` lvalue. */
static int slot_is_pre_init(const AgentSlot *s)
{
    switch (s->kind) {
    case SLOT_ID:    return *(const int *)s->slot == -1;
    case SLOT_REALM: return *(const JSClassID *)s->slot == JS_INVALID_CLASS_ID;
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
    case SLOT_REALM: *(JSClassID *)p = JS_INVALID_CLASS_ID; break;
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
    DCHECKF(slot_is_pre_init(s),
            "undoing `%s`'s declaration of agent state (%s, declared at %s:%d) did not put the slot back at "
            "its pre-init value — the write side and the read side of one kind disagree, and the release that "
            "just ran will be reported as not having run",
            s->component, s->what, s->file, s->line);
}

/* THE DECLARING FILE'S RELEASE RAN — see agent_state.h for why the undo may not put a slot back without it.
   IT WRITES NO SLOT, and that is the property the whole design rests on: it can therefore be made in the
   MIDDLE of an owner's cascade, where a sub-component's release actually runs, while the reset stays at the
   row's last line where the ordering contract puts it. */
void agent_state_reached_at(const char *component, const char *file, int line)
{
    int i, under = 0, mine = 0;

    DCHECK(file != NULL && *file && line > 0,
           "a release said the cascade had reached it with no site — core/agent_state.h's macro spells "
           "__FILE__ at the call, and the file IS the claim here rather than decoration on it");
    DCHECKF(component != NULL && *component,
            "a release said the cascade had reached it without naming the row, at %s:%d", file, line);
    for (i = 0; i < g_n; i++) {
        if (strcmp(g_slots[i].component, component) != 0) continue;
        under++;
        /* BY CONTENT AND NOT BY POINTER. Both strings are `__FILE__` in one translation unit, so they are the
           same text; whether a compiler merges two identical literals is not a thing this may depend on. */
        if (strcmp(g_slots[i].file, file) != 0) continue;
        g_slots[i].reached = true;
        mine++;
    }
    /* TWO REFUSALS AND NOT ONE, BECAUSE THEY ARE TWO REPAIRS. A row name nothing declares is the misspelling
       agent_state_undo refuses in the same words; a row that exists and has nothing from THIS file is a
       release claiming a row it contributes no state to, which is the other spelling being wrong or the call
       standing in a file that never declared. One message for both would be the three-states-behind-one-answer
       shape this registry was written against. */
    DCHECKF(under > 0,
            "the release in %s said `%s`'s cascade had reached it, at line %d, and this registry holds no "
            "declaration under that name at all. The row is spelled once at each agent_state_* call and once "
            "at each release that claims it, and it is core/platform.c's ROW name rather than the file's — "
            "for a sub-component, the row whose release reaches it. Either this spelling is wrong or the "
            "declarations' is",
            file, component, line);
    DCHECKF(mine > 0,
            "the release at %s:%d said `%s`'s cascade had reached it, and `%s` is a real row that this file "
            "declares no agent state under. This entry says THE SLOTS THIS FILE DECLARED have been given "
            "back, so a file with none to give back has nothing to say here: either the row named is a "
            "neighbour's, or this call is in the wrong file",
            file, line, component, component);
}

void agent_state_undo_at(const char *component, const char *file, int line)
{
    int i, n = 0;

    DCHECK(file != NULL && *file && line > 0,
           "a component undid its agent state with no site — core/agent_state.h's macro spells __FILE__ at the "
           "call, so a release that reached here without one bypassed it");
    DCHECKF(component != NULL && *component,
            "a component undid its agent state without naming itself, at %s:%d", file, line);
    for (i = 0; i < g_n; i++) {
        if (strcmp(g_slots[i].component, component) != 0) continue;
        /* BEFORE THE RESET AND NOT AFTER IT, because the reset is what destroys the evidence: once the slot
           is back at its pre-init value, a release that never reached it and a release that did are the same
           bytes, which is exactly the state agent_state_check_released is left unable to tell apart.
           THE EXEMPTION IS THIS FILE'S OWN DECLARATIONS, and calling the undo IS this file's claim about
           them — so a row whose undo stands in a file that declares nothing under it has an empty exemption
           and every declaring file must have spoken, which is the stricter reading arriving for free. */
        DCHECKF(g_slots[i].reached || strcmp(g_slots[i].file, file) == 0,
                "`%s`'s release at %s:%d is putting back %s, which was declared at %s:%d, and that file has "
                "not said the cascade reached it. A row is declared from SEVERAL FILES and this one call "
                "resets every slot carrying the row's name, so putting this one back would report a release "
                "that never ran as one that did — silently for a pointer, which reaches no census anybody "
                "asserts over. Either the cascade in %s dropped the release that gives %s back, or that "
                "release does not end in agent_state_reached(\"%s\"); the declaring file above says which "
                "in one read",
                component, file, line, g_slots[i].what, g_slots[i].file, g_slots[i].line,
                file, g_slots[i].what, component);
        slot_set_pre_init(&g_slots[i]);
        n++;
    }
    /* THE NAME IS WRITTEN TWICE — here and at every declaration — so the state this refuses is the two
       spellings differing. A no-op would leave every one of that component's slots set and report the failure
       against the DECLARING name, several stages later, as a release that never ran.
       AND THE RELEASE'S OWN ADDRESS IS IN THE MESSAGE, because a remedy of the form "one of two spellings is
       wrong" names an ACTION and no object: the reader has the row name already and needs the file holding
       the release that just claimed it. */
    DCHECKF(n > 0,
            "`%s` undid its agent state at %s:%d and this registry holds no declaration under that name. The "
            "component argument is spelled once there and once at each agent_state_* call, and it is "
            "core/platform.c's ROW name rather than the file's — for a sub-component, the row whose release "
            "reaches it. Either this spelling is wrong or the declarations' is",
            component, file, line);
}

void agent_state_check_released(void)
{
#if APICLIENT_DEV
    int i;

    for (i = 0; i < g_n; i++) {
        /* The message is the COMPONENT and the STATE, because the reader of this `@WHY` is standing at a
           teardown that already ran: what they need is which release to go and finish, not an address. */
        if (slot_is_pre_init(&g_slots[i])) continue;
        /* THE DECLARING SITE, AND IT IS NOT DECORATION ON THIS ONE EITHER: this reader is standing at a
           teardown that already ran and is being told which release to go and finish — and for a
           SUB-COMPONENT the row named here is its OWNER'S, so the component string is precisely the name that
           does NOT lead to the file whose `_free` is short. */
        DFAILF("%s did not undo its declaration: %s (declared at %s:%d) is still set after the release column "
               "ran. A handle left behind is invisible to both of JS_FreeRuntime's censuses — the reference "
               "WAS given back — and the only code that ever reads it is this component's own next _init, "
               "which consults it to decide it has already been declared. The next agent in this process gets "
               "a component that reports itself built and whose every other handle is null.",
               g_slots[i].component, g_slots[i].what, g_slots[i].file, g_slots[i].line);
    }
#endif
}

void agent_state_reset(void)
{
    free(g_slots);
    g_slots = NULL;
    g_n = g_cap = 0;
}
