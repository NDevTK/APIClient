/* Web IDL §3.2 JavaScript type mapping, asked per member: the ECMAScript language types a member's declared
   type converts to. A component minting a member's value as an unknown states them at the mint. */
#ifndef ENGINE_HOST_BROWSER_CORE_IDL_MEMBER_TYPES_H
#define ENGINE_HOST_BROWSER_CORE_IDL_MEMBER_TYPES_H

/* The JS_LANG_TYPE_* bits (quickjs-step.h) the declared type of `iface`'s attribute or operation `member`
   converts to: `DOMString` is String, `long` Number, `T?` adds Null, `any` is every type. A mixin's member is
   asked of an including interface. Asserts the corpus declares the member; `file`/`line` are the asker's. */
#define idl_member_types(iface, member) idl_member_types_at((iface), (member), __FILE__, __LINE__)
unsigned idl_member_types_at(const char *iface, const char *member, const char *file, int line);

#endif
