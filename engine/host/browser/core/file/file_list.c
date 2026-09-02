/* THE FileList INTERFACE — W3C File API §5.
 *
 * WHAT IT IS. "This interface is a list of File objects" — an indexed getter, a `length`, and nothing else.
 * §5 declares it `[Exposed=(Window,Worker), Serializable]` with exactly two members:
 *
 *     interface FileList {
 *       getter File? item(unsigned long index);
 *       readonly attribute unsigned long length;
 *     };
 *
 * WHY IT EXISTS SEPARATELY FROM THE LIST IT VIEWS. HTML §4.10.5.1.17 gives an `<input type=file>` a LIST OF
 * SELECTED FILES, and §4.10.5.4's `files` member must "return a FileList object that represents the current
 * selected files. THE SAME OBJECT must be returned until the list of selected files changes." So the FileList
 * is an OBJECT with an identity the page can hold, not a snapshot minted per read — which is why the element
 * stores the FileList itself (input_value.c) rather than a list this file would have to wrap again.
 *
 * THE LIST IS A JS ARRAY hung off a private Symbol, for the reason idl_slots.h states: a slot written as a
 * property write is captured by the per-flow COW delta, so a selection made by one flow is that flow's and
 * parks with it. A malloc'd C vector of JSValues would revert a POINTER on a context switch and leave the
 * Files reachable from nothing.
 *
 * IT IS NOT SERIALIZABLE YET, honestly: File API §5's serialization steps — HTML §2.7.1 Serializable objects
 * is what declares that concept — are sub-serializations of each File, and the File arm of structured clone
 * does not exist. A shape-only entry in the clone table would be the stub the IDL audit exists to expose.
 * BOTH STANDARDS ARE NAMED IN THAT SENTENCE and neither used to be, which is what made every bare `§5` in this
 * file unattributable: `serialization steps` is an HTML term, so this ONE site read as naming HTML §5 — which
 * is Microdata — and the whole file's bare numbers were then inferred off it. It is the same defect
 * core/css/media_list.c carried at its own indexed-getter comment, and the fix is the same: a file whose
 * convention is one standard still has to say so at any site where a term belongs to another. */
#include <stdlib.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/file/blob.h"
#include "core/file/file_list.h"
#include "core/idl_args.h"
#include "core/idl_index_arg.h"
#include "core/idl_indexed.h"
#include "core/realm.h"
#include "solver/concolic.h"   /* §5.2's `item` takes an index unknown external input crosses AS ITSELF */

static JSClassID g_fl_class;
static JSValue   g_files_key = JS_UNDEFINED;   /* the Symbol the Array hangs off — minted once per AGENT */
static JSAtom    g_files_atom = JS_ATOM_NULL;
static int       g_item_id = -1;
static int       g_ready;

/* THE LIST THIS FileList VIEWS — the Array, OWNED, or JS_UNDEFINED for anything that is not a FileList. An
   own-slot read, never a lookup: a page's `Object.prototype` cannot answer it. OWNED and not borrowed because
   the borrowed form is written by freeing the reference and then using it, which is a use-after-free the
   instant the list is the object's last one. */
static JSValue fl_files(JSContext *ctx, JSValueConst v)
{
    JSValue files;

    if (!JS_IsObject(v) || g_files_atom == JS_ATOM_NULL) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &files, v, g_files_atom) <= 0) return JS_UNDEFINED;
    if (!JS_IsArray(files)) {
        DCHECK(JS_IsUndefined(files), "a FileList's slot held something that is not a list — the slot is under "
                                      "a Symbol this component minted and never published, so nothing but this "
                                      "file can have written it");
        JS_FreeValue(ctx, files);
        return JS_UNDEFINED;
    }
    return files;
}

/* The Array's own `length`, which IS §5.1's — an engine-built Array with no prototype chain of the page's on
   the path, so the read runs none of its code. */
static uint32_t fl_count(JSContext *ctx, JSValueConst files)
{
    JSValue len = JS_GetPropertyStr(ctx, files, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    return n;
}

bool file_list_is(JSContext *ctx, JSValueConst v)
{
    JSValue files = fl_files(ctx, v);
    bool is = !JS_IsUndefined(files);

    JS_FreeValue(ctx, files);
    return is;
}

uint32_t file_list_length(JSContext *ctx, JSValueConst v)
{
    JSValue files = fl_files(ctx, v);
    uint32_t n;

    if (JS_IsUndefined(files)) return 0;
    n = fl_count(ctx, files);
    JS_FreeValue(ctx, files);
    return n;
}

JSValue file_list_item(JSContext *ctx, JSValueConst v, uint32_t i)
{
    JSValue files = fl_files(ctx, v), r = JS_UNDEFINED;

    if (JS_IsUndefined(files)) return JS_UNDEFINED;
    if (i < fl_count(ctx, files)) r = JS_GetPropertyUint32(ctx, files, i);
    JS_FreeValue(ctx, files);
    return r;
}

/* ---- Web IDL §3.9 Legacy platform objects' indexed property getter ------------------------------------------
 *
 * THE NUMBER HERE WAS A BARE `§3.9`, AND EVERY OTHER BARE `§N` IN THIS FILE IS THE FILE API — which has no
 * §3.9 at all (§3 is The Blob Interface and Binary Data and ends at §3.3.6 The textStream() method). It read
 * as a File API section for as long as nobody opened it. The STANDARD is now named, because a bare number is
 * only checkable against the standard the file's own convention picks.
 *
 * WHICH SECTION THE QUOTE IS FROM IS THE OTHER HALF, and it is File API §5.2 Methods and Parameters — the same
 * section that defines `item(index)`, not a section of Web IDL: "Supported property indices are the numbers in
 * the range zero to one less than the number of File objects represented by the FileList object. If there are
 * no such File objects, then there are no supported property indices." That is idl_indexed.c's contract
 * exactly: JS_UNDEFINED past the end means the property is not there, so `fl[0]` on an empty list is undefined
 * and `0 in fl` is false. */
static uint32_t fl_indexed_length(JSContext *ctx, JSValueConst self) { return file_list_length(ctx, self); }
static JSValue  fl_indexed_item(JSContext *ctx, JSValueConst self, uint32_t i)
{
    return file_list_item(ctx, self, i);
}

static const IdlIndexedDecl FILE_LIST_INDEXED = { "FileList", fl_indexed_length, fl_indexed_item, NULL, 0 };

/* ---- the members ------------------------------------------------------------------------------------------ */

/* §5.1: "must return the number of files in the FileList object. If there are no files, this attribute must
   return 0." The brand check is Web IDL §3.7.5's, and it is this member's: `FileList.prototype.length` read off
   a plain object is a TypeError, not zero. */
static JSValue js_fl_length(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!file_list_is(ctx, this_val)) return JS_ThrowTypeError(ctx, "not a FileList");
    return JS_NewUint32(ctx, file_list_length(ctx, this_val));
}

/* §5.2's `item(index)`: "must return the indexth File object in the FileList. If there is no indexth File
 * object in the FileList, then this method must return null." NULL and not undefined — that is the whole
 * difference between the OPERATION and the indexed getter above, and `fl.item(0)` on an empty list is the way
 * §5's own sample code tests for a file.
 *
 * IT IS A STEP MACHINE BECAUSE ITS ONE ARGUMENT CAN BE UNKNOWN, AND IT IS NOT "a real number by now" — the
 * sentence that once stood here said the declaration had converted it, and core/idl_args.h says the opposite
 * BY NAME: idl_concolic_rule answers IDL_CONCOLIC_CROSSES for every integer type, IDL_UNSIGNED_LONG included,
 * so a Web IDL §3.2 conversion is a BOUNDARY unknown external input crosses AS ITSELF and the body is handed
 * the concolic. Running `JS_ToUint32` over it is the shape idl_args.h bans: a concolic is a real JSObject, so
 * ToNumber reaches ToPrimitive from a plain C frame and the engine aborts INSIDE the coercion, one frame below
 * this file, which is why checking the return is no defence. The fork is core/idl_index_arg.h's elimination
 * chain, and asking it is what a plain C activation has nowhere to park for. */
#define FL_ITEM_ALGORITHM "File API §5.2 Methods and Parameters item(index)"
#define FL_ITEM_STAGES(X)                                                                                     \
    X(FL_ITEM_READ, FL_ITEM_ALGORITHM " (return the indexth File object in the FileList, or null)")
enum { IDL_STEP_STAGE_BASE(FL_ITEM_STAGES) FL_ITEM_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const FL_ITEM_STEPS[] = { FL_ITEM_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_fl_item(JSContext *ctx, JSStepHdr *hdr, void *state, int argc, JSValueConst *argv,
                      JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    IdlIndexChain *s = state;
    uint32_t i = 0;
    bool past_end = false;
    JSValue r;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);   /* this machine makes no request that delivers a value */
    *presult = JS_UNDEFINED;
    DCHECK(hdr->stage == FL_ITEM_READ,
           "§5.2's item(index) resumed into a stage the algorithm does not have — it is ONE sentence, and the "
           "chain of questions it may ask is a cursor on this machine's own state rather than a stage apiece");
    DCHECK(argc == 1,
           "§5.2's item(index) reached its body with an argument count its declaration does not produce — "
           "`index` is REQUIRED, so §3.6's argument-count check refuses a bare item() before this body runs");
    if (!file_list_is(ctx, hdr->this_val)) {
        JS_ThrowTypeError(ctx, "not a FileList");
        return JS_STEP_ABRUPT;
    }
    if (concolic_is(argv[0])) {
        int rc = idl_index_chain_run(ctx, hdr, s, argv[0], file_list_length(ctx, hdr->this_val),
                                     FL_ITEM_ALGORITHM, &i, &past_end);
        if (rc)
            return rc;   /* parked at the fork */
        if (past_end) {
            *presult = JS_NULL;   /* §5.2's own answer when there is no indexth File object */
            return JS_STEP_DONE;
        }
    } else {
        /* The declaration ran Web IDL §3.2.4.6 unsigned long's ConvertToInt(V, 32, "unsigned"), which is
           §3.2.4.9 Abstract operations' modulo and not a clamp — `fl.item(2**32)` is item 0. */
        i = idl_index_arg_known(ctx, argv[0], FL_ITEM_ALGORITHM);
    }
    r = file_list_item(ctx, hdr->this_val, i);
    *presult = JS_IsUndefined(r) ? JS_NULL : r;
    return JS_STEP_DONE;
}

static const IdlStepDecl FL_ITEM_DECL = {
    js_fl_item, sizeof(IdlIndexChain), idl_index_chain_visit, NULL,
    FL_ITEM_ALGORITHM, FL_ITEM_STEPS, 0, NULL
};

/* ---- construction ----------------------------------------------------------------------------------------- */

JSValue file_list_new(JSContext *ctx, JSValue files)
{
    JSValue proto, obj;
    uint32_t n = 0, i;

    DCHECK(g_ready, "a FileList was built before file_list_init declared the interface");
    DCHECK(JS_IsArray(files), "a FileList was built over something that is not a list of files");
    {
        JSValue len = JS_GetPropertyStr(ctx, files, "length");
        JS_ToUint32(ctx, &n, len);
        JS_FreeValue(ctx, len);
    }
    for (i = 0; i < n; i++) {
        JSValue f = JS_GetPropertyUint32(ctx, files, i);
        /* §5 IS a list of FILE objects. A plain Blob has no name, and a page reading `files[0].name` off one
           would get §4's TypeError from a list this engine built — so the list is asserted where it is BUILT,
           which is the one place that can name who built it. */
        DCHECK(blob_file_name_of(f) != NULL,
               "a FileList was built over a value that is not a File — §5 is a list of File objects, and a "
               "plain Blob carries no name for §4.2's `name` to answer with");
        JS_FreeValue(ctx, f);
    }
    proto = file_list_proto(ctx);
    obj = idl_indexed_new(ctx, proto, &FILE_LIST_INDEXED);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "FileList: a list object could not be allocated");
    DCHECK(g_files_atom != JS_ATOM_NULL, "the FileList slot key was not interned");
    JS_DefinePropertyValue(ctx, obj, g_files_atom, files, 0);   /* CONSUMES `files` */
    return obj;
}

JSValue file_list_new_empty(JSContext *ctx)
{
    JSValue files = JS_NewArray(ctx);

    CHECK(!JS_IsException(files), "FileList: an empty list could not be allocated");
    return file_list_new(ctx, files);
}

/* ---- install ---------------------------------------------------------------------------------------------- */

void file_list_init(JSContext *ctx)
{
    JSClassDef def = { "FileList" };
    static const IdlArgType ITEM_ARGS[1] = { IDL_UNSIGNED_LONG };

    DCHECK(!g_ready, "file_list_init ran twice — the interface is declared once per AGENT");
    /* THE CLASS EXISTS FOR THE PER-REALM PROTOTYPE SLOT ALONE, exactly as §4's File class does: a FileList
       INSTANCE wears idl_indexed.c's class, because the indexed property getter is what the object IS. */
    JS_NewClassID(JS_GetRuntime(ctx), &g_fl_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_fl_class, &def) == 0,
          "FileList: the per-realm prototype slot could not be declared");
    g_files_key = JS_NewSymbol(ctx, "fileListFiles", false);
    CHECK(!JS_IsException(g_files_key), "the FileList slot key allocation failed");
    g_files_atom = JS_ValueToAtom(ctx, g_files_key);
    CHECK(g_files_atom != JS_ATOM_NULL, "the FileList slot key could not be interned");
    /* §5.2's `item` IS A MACHINE — a declaration and not a dispatch, since there is no second body for
       anything to select against. `index` is REQUIRED, and it can be unknown external input. */
    g_item_id = idl_method_id_step(ctx, ITEM_ARGS, 1, NULL, 0, &FL_ITEM_DECL, 0);
    g_ready = 1;
    realm_declare_intrinsic(file_list_install_protos);
}

void file_list_install_protos(JSContext *ctx)
{
    JSValue proto, prev, ctor, global;

    DCHECK(g_ready, "a realm asked for FileList.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_fl_class);
    DCHECK(JS_IsNull(prev), "file_list_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "FileList.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "FileList");
    idl_install_accessor_no_user_code(ctx, proto, "length", js_fl_length, 0, -1);
    idl_install_method(ctx, proto, "item", g_item_id);
    /* §3.7.9 step 1.1: an interface with an indexed getter and an integer `length` is given %Array.prototype.values% as
       its @@iterator, which is why `for (const f of input.files)` is ordinary code. §5 declares NO `iterable<>`,
       so `entries`, `keys`, `values` and `forEach` are honestly absent — the same split HTMLCollection is on. */
    idl_indexed_install_iterable(ctx, proto);
    JS_SetClassProto(ctx, g_fl_class, JS_DupValue(ctx, proto));

    /* §3.7.1's INTERFACE OBJECT, on THIS realm's global. §5 declares no constructor, so it throws a TypeError
       when called or constructed — which is what a page's `new FileList()` must get, and what tells a
       feature-detecting bundle that the interface EXISTS. */
    ctor = idl_interface_object(ctx, "FileList", proto);
    CHECK(!JS_IsException(ctor), "the FileList interface object could not be allocated");
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "FileList", ctor);
    JS_FreeValue(ctx, global);
}

JSValue file_list_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_fl_class);

    DCHECK(!JS_IsNull(proto), "FileList.prototype was asked for in a realm that never ran its install");
    return proto;   /* OWNED */
}

void file_list_free(JSContext *ctx)
{
    if (!g_ready) return;
    /* The prototypes and interface objects are the REALMS' — each is released with its context. The Symbol is
       the AGENT's, and a runtime-lifetime value nobody frees is a live GC object JS_FreeRuntime's walk counts
       as a leak. */
    JS_FreeAtom(ctx, g_files_atom);
    g_files_atom = JS_ATOM_NULL;
    JS_FreeValue(ctx, g_files_key);
    g_files_key = JS_UNDEFINED;
    g_item_id = -1;
    g_ready = 0;
}
