/* FILE SYSTEM ACCESS §3 — ACCESSING THE LOCAL FILE SYSTEM. The three local file system handle factories, the
 * options they take, and the verification every one of them performs first. See file_picker.c.
 *
 *   [SecureContext]
 *   partial interface Window {
 *     Promise<sequence<FileSystemFileHandle>> showOpenFilePicker(optional OpenFilePickerOptions options = {});
 *     Promise<FileSystemFileHandle> showSaveFilePicker(optional SaveFilePickerOptions options = {});
 *     Promise<FileSystemDirectoryHandle> showDirectoryPicker(optional DirectoryPickerOptions options = {});
 *   };
 *
 * WHY THIS IS A COMPONENT AND NOT THREE MEMBERS ON THE WINDOW. The three factories differ in what they select
 * and agree in everything around it — the same option processing, the same verification, the same prompt, the
 * same AbortError, the same activation notification at the end. Written as three they would be three copies of
 * a verification whose whole job is to be identical.
 *
 * WHAT THE PICKER RETURNS, AND WHICH HALF OF IT IS CONCOLIC. This is the modelling decision the component
 * exists to take, and it splits exactly where CLAUDE.md's §Headless-is-not-valueless line puts it: the missing
 * piece is a physical prompt, not the behaviour behind one.
 *   THE USER'S DECISIONS ARE THE UNKNOWN, and there are two of them. Whether the dialog was dismissed (§3.3
 * step 7.4's `dismissed`, and §3.4's "if the user dismissed the prompt without making a selection") is a fact
 * about a human this engine cannot observe, and BOTH answers reach code a real bundle ships — the `.catch`
 * that re-enables a button and reports "no file chosen", and the read/upload path behind the handle. So it is
 * a source, asked through step_fork_run, with outcome 0 the ordinary completion (the modelled device holds
 * files and hands them over). Whether the user grants the permission the returned handle then needs is the
 * other, and it is asked one standard over, by Permissions §5.2 through §2.3.2's requestPermission.
 *   EVERYTHING THE ENGINE ITSELF WROTE IS CONCRETE. The files the prompt chooses from are the ones on
 * core/file/file_system.c's local file system; which of them this control's filter admits is COMPUTED by
 * running the filter; the handle, its kind, its name and its locator are the model's own facts; and §3.1's
 * grant is a store entry this user agent has just watched itself write. Forking any of those would park a
 * sibling flow in which the engine did something other than what it did.
 *   AND THE FILE'S BYTES ARE ALREADY A SOURCE, which is why the handle needs no wrapping of its own. A file on
 * the device enters through file_system_local_add, which declares `file:NAME` as an attacker source and wraps
 * the byte sequence in a concolic carrying those bytes as its example — so `(await (await
 * handle.getFile()).text())` reaches a sink carrying both the real contents and the fact that an attacker
 * chose them, which is the "an FSA file containing XSS" CLAUDE.md names as the reason virtual systems exist.
 * A picker is one more door onto that source, not a second source.
 *
 * WHAT IS NOT BUILT, BY NAME. `FilePickerOptions`'s `types` member is `sequence<FilePickerAcceptType>`, whose
 * element is a DICTIONARY containing a `record<USVString, (USVString or sequence<USVString>)>` — and
 * core/idl_args.h declares no `sequence<D>` over a dictionary and no `record` type at all (its nearest,
 * IDL_SEQUENCE_STRING_OR_DICT, has a string arm that would silently swallow `types: ["x"]`, which Web IDL
 * §3.2.17 makes a TypeError). A picker called WITH `types` therefore reaches a DFAIL naming that conversion,
 * rather than a filter quietly built out of an unconverted value. Called without it, §3.2.1's process accept
 * types is its steps 1 and 5-7 — the "all files" option, which is what a filter that admits everything is —
 * and that is the whole algorithm for the ordinary call. */
#ifndef ENGINE_HOST_BROWSER_CORE_FILE_FILE_PICKER_H
#define ENGINE_HOST_BROWSER_CORE_FILE_FILE_PICKER_H

#include "quickjs.h"

/* Declared ONCE PER AGENT, after core/file/file_system_access.c (whose §3.1 grant this calls) and after
   core/file/file_system_handle.c (whose handles it mints). The three members are `partial interface Window`
   members and every global this agent builds is a Window, so the per-realm install is DECLARED into
   core/realm.h's one list from here — there is no install line for a host to write, and therefore none for a
   host to forget. */
void file_picker_init(JSContext *ctx);
void file_picker_free(void);

#endif
