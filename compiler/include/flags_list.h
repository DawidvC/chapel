// These are flags for Symbols - see flags.h for details.
// The format is:
//   symbolFlag(NAME, PRAGMA, MAPNAME, COMMENT)
// where
//   NAME     - the enum symbol (-> enum Flag, flagNames)
//   PRAGMA   - whether this flag can be set via a Chapel pragma
//              see YPR/NPR shorthands (-> flagPragma)
//   MAPNAME  - a unique string (-> flagMap);
//              the pragma string, if the flag is settable via a pragma
//   COMMENT  - a (possibly-empty) comment string (-> flagComments)

#define ypr true  /* YES, the flag can be set via a pragma */
#define npr false /* NO, cannot be set via a pragma */
#define ncm ""    /* no comment */

symbolFlag( FLAG_ALLOW_REF , ypr, "allow ref" , ncm )
symbolFlag( FLAG_ARG_THIS, npr, "arg this", "the hidden object argument")
symbolFlag( FLAG_ARRAY , ypr, "array" , ncm )
symbolFlag( FLAG_ARRAY_ALIAS , npr, "array alias" , "array alias declared via => syntax" )
symbolFlag( FLAG_AUTO_II , npr, "auto ii" , ncm )
symbolFlag( FLAG_BASE_ARRAY , ypr, "base array" , ncm )
symbolFlag( FLAG_BASE_DOMAIN , ypr, "base domain" , ncm )
symbolFlag( FLAG_BASE_DIST , ypr, "base dist" , ncm )
symbolFlag( FLAG_BEGIN , npr, "begin" , ncm )
symbolFlag( FLAG_BEGIN_BLOCK , npr, "begin block" , ncm )
symbolFlag( FLAG_CALLS_CONSTRUCTOR , npr, "calls constructor" , "for functions that return constructor return values" )
symbolFlag( FLAG_COBEGIN_OR_COFORALL , npr, "cobegin or coforall" , ncm )
symbolFlag( FLAG_COBEGIN_OR_COFORALL_BLOCK , npr, "cobegin or coforall block" , ncm )
symbolFlag( FLAG_COFORALL_INDEX_VAR , npr, "coforall index var" , ncm )
symbolFlag( FLAG_COMMAND_LINE_SETTING , ypr, "command line setting" , ncm )
symbolFlag( FLAG_COMPILER_NESTED_FUNCTION , npr, "compiler nested function" , ncm )
symbolFlag( FLAG_CONCURRENTLY_ACCESSED , npr, "concurrently accessed" , "local variables accessed by multiple threads" )
symbolFlag( FLAG_CONFIG , npr, "config" , "config variable, constant, or parameter" )
symbolFlag( FLAG_CONST , npr, "const" , "constant" )
symbolFlag( FLAG_CONSTRUCTOR , npr, "constructor" , "constructor (but not type constructor); loosely defined to include constructor wrappers" )
symbolFlag( FLAG_DATA_CLASS , ypr, "data class" , ncm )
symbolFlag( FLAG_DATA_SET_ERROR , npr, "data set error" , ncm )
symbolFlag( FLAG_DEFAULT_CONSTRUCTOR , npr, "default constructor" , ncm )
symbolFlag( FLAG_DESTRUCTOR , npr, "destructor" , ncm )
symbolFlag( FLAG_DISTRIBUTION , ypr, "distribution" , ncm )
symbolFlag( FLAG_DOMAIN , ypr, "domain" , ncm )
symbolFlag( FLAG_DONT_DISABLE_REMOTE_VALUE_FORWARDING , ypr, "dont disable remote value forwarding" , ncm )
symbolFlag( FLAG_EXPAND_TUPLES_WITH_VALUES , ypr, "expand tuples with values" , ncm )
symbolFlag( FLAG_EXPORT , ypr, "export" , ncm )
symbolFlag( FLAG_EXPR_TEMP , npr, "expr temp" , "temporary that stores the result of an expression" )
symbolFlag( FLAG_EXTERN , npr, "extern" , "extern variables, types, and functions" )
symbolFlag( FLAG_FAST_ON , npr, "fast on" , "with FLAG_ON/FLAG_ON_BLOCK, \"on block\" , use fast spawning option (if available)" )
symbolFlag( FLAG_FIXED_STRING , ypr, "fixed string" , "fixed-length string" )
symbolFlag( FLAG_FUNCTION_CLASS , npr, "function class" , "first-class function class representation" )
symbolFlag( FLAG_FIRST_CLASS_FUNCTION_INVOCATION, npr, "first class function invocation" , "proxy for first-class function invocation" )
symbolFlag( FLAG_FUNCTION_PROTOTYPE , npr, "function prototype" , "signature for function prototypes" )
symbolFlag( FLAG_GENERIC , npr, "generic" , "generic types and functions" )
symbolFlag( FLAG_HAS_RUNTIME_TYPE , ypr, "has runtime type" , "type that has an associated runtime type" )
symbolFlag( FLAG_HEAP , npr, "heap" , ncm )
symbolFlag( FLAG_HEAP_ALLOCATE , npr, "heap allocate" , ncm )
symbolFlag( FLAG_STAR_TUPLE , npr, "star tuple" , "mark tuple types as star tuple types" )
symbolFlag( FLAG_IMPLICIT_ALIAS_FIELD , npr, "implicit alias field" , ncm )
symbolFlag( FLAG_INDEX_VAR , npr, "index var" , ncm )
symbolFlag( FLAG_INLINE , npr, "inline" , ncm )
symbolFlag( FLAG_INLINE_ITERATOR , npr, "inline iterator" , "iterators that are always inlined, e.g., leaders" )
symbolFlag( FLAG_INVISIBLE_FN , npr, "invisible fn" , "invisible function (not a candidate for resolution)" )
symbolFlag( FLAG_INSERT_AUTO_COPY , npr, "insert auto copy" , ncm )
symbolFlag( FLAG_INSERT_AUTO_DESTROY , ypr, "insert auto destroy" , ncm )
symbolFlag( FLAG_INSERT_AUTO_DESTROY_FOR_EXPLICIT_NEW , npr, "insert auto destroy for explicit new" , ncm )
symbolFlag( FLAG_INSERT_LINE_FILE_INFO , ypr, "insert line file info" , ncm )
symbolFlag( FLAG_IS_MEME , npr, "is meme" , ncm )
symbolFlag( FLAG_ITERATOR_CLASS , npr, "iterator class" , ncm )
symbolFlag( FLAG_ITERATOR_FN , npr, "iterator fn" , ncm )
symbolFlag( FLAG_ITERATOR_RECORD , npr, "iterator record" , ncm )
symbolFlag( FLAG_LABEL_BREAK , npr, "label break" , ncm )
symbolFlag( FLAG_LABEL_CONTINUE , npr, "label continue" , ncm )
symbolFlag( FLAG_LOCAL , ypr, "local" , "local, e.g. exported function arguments should not be wide" )
symbolFlag( FLAG_LOOP_BODY_ARGUMENT_CLASS , npr, "loop body argument class" , ncm )
symbolFlag( FLAG_MAYBE_PARAM , npr, "maybe param" , "symbol can resolve to a param" )
symbolFlag( FLAG_MAYBE_TYPE , npr, "maybe type" , "symbol can resolve to a type" )
symbolFlag( FLAG_METHOD , npr, "method" , "function that is a method" )
symbolFlag( FLAG_MODULE_INIT , npr, "module init" , "a module init function" )
symbolFlag( FLAG_NO_AUTO_DESTROY , ypr, "no auto destroy" , ncm )
symbolFlag( FLAG_NO_CODEGEN , ypr, "no codegen" , "do not generate e.g. C code defining this symbol" )
symbolFlag( FLAG_NO_COPY , ypr, "no copy" , "do not apply chpl__initCopy to initialization of a variable" )
symbolFlag( FLAG_NO_DEFAULT_FUNCTIONS , ypr, "no default functions" , ncm )
symbolFlag( FLAG_NO_USE_CHAPELSTANDARD , ypr, "no use ChapelStandard" , "Do not implicitly use ChapelStandard" )
symbolFlag( FLAG_NO_FORMAL_TMP , npr, "no formal tmp" , ncm )
symbolFlag( FLAG_NO_IMPLICIT_COPY , ypr, "no implicit copy" , "function does not require autoCopy/autoDestroy" )
symbolFlag( FLAG_NO_INSTANTIATION_LIMIT , ypr, "no instantiation limit", "The instantiation limit is not checked for this function" )
symbolFlag( FLAG_NO_OBJECT , ypr, "no object" , ncm )
symbolFlag( FLAG_NO_PARENS , npr, "no parens" , "function without parentheses" )
symbolFlag( FLAG_NO_PROTOTYPE , ypr, "no prototype" , "do not generate a prototype this symbol" )
symbolFlag( FLAG_NO_WIDE_CLASS , npr, "no wide class" , ncm )
symbolFlag( FLAG_NON_BLOCKING , npr, "non blocking" , "with FLAG_ON/FLAG_ON_BLOCK, non-blocking on functions" )
symbolFlag( FLAG_OBJECT_CLASS , npr, "object class" , ncm )
symbolFlag( FLAG_OMIT_FROM_CONSTRUCTOR , ypr, "omit from constructor" , ncm )
symbolFlag( FLAG_GPU_ON , npr, "gpu on" , "Flag to mark GPU device kernel" )
symbolFlag( FLAG_GPU_CALL , npr, "gpu call" , "Flag to mark caller of GPU kernel" )
symbolFlag( FLAG_ON , npr, "on" , ncm )
symbolFlag( FLAG_ON_BLOCK , npr, "on block" , ncm )
symbolFlag( FLAG_PARAM , npr, "param" , "parameter (compile-time constant)" )
symbolFlag( FLAG_PRIMITIVE_TYPE , ypr, "primitive type" , "attached to primitive types to keep them from being deleted" )
symbolFlag( FLAG_PRIVATE , ypr, "private" , ncm )
symbolFlag( FLAG_PRIVATIZED_CLASS , ypr, "privatized class" , "privatized array or domain class" )
symbolFlag( FLAG_PROMOTION_WRAPPER , npr, "promotion wrapper" , ncm )
symbolFlag( FLAG_RANGE , ypr, "range" , "indicates that this type can be iterated" )
symbolFlag( FLAG_REF , ypr, "ref" , ncm )
symbolFlag( FLAG_REF_ITERATOR_CLASS , npr, "ref iterator class" , ncm )
symbolFlag( FLAG_REF_THIS , ypr, "ref this" , ncm )
symbolFlag( FLAG_REMOVABLE_AUTO_COPY , ypr, "removable auto copy" , ncm )
symbolFlag( FLAG_REMOVABLE_AUTO_DESTROY , ypr, "removable auto destroy" , ncm )
symbolFlag( FLAG_RUNTIME_TYPE_VALUE , npr, "runtime type value" , "associated runtime type (value)" )
symbolFlag( FLAG_SINGLE , ypr, "single" , ncm )
// Based on how this is used, I suggest renaming it to return_value_has_initializer
// or something similar <hilde>.
symbolFlag( FLAG_SPECIFIED_RETURN_TYPE , npr, "specified return type" , ncm )
symbolFlag( FLAG_SUPER_CLASS , npr, "super class" , ncm )
symbolFlag( FLAG_SYNC , ypr, "sync" , ncm )
symbolFlag( FLAG_SYNTACTIC_DISTRIBUTION , ypr, "syntactic distribution" , ncm )
symbolFlag( FLAG_TEMP , npr, "temp" , "compiler-inserted temporary" )
symbolFlag( FLAG_TUPLE , ypr, "tuple" , ncm )
symbolFlag( FLAG_TYPE_CONSTRUCTOR , npr, "type constructor" , ncm )
symbolFlag( FLAG_TYPE_VARIABLE , npr, "type variable" , "contains a type instead of a value" )
symbolFlag( FLAG_USER_NAMED , npr, "user named" , "named by the user" /* so leave it alone */ )
symbolFlag( FLAG_VIRTUAL , npr, "virtual" , ncm )
symbolFlag( FLAG_WIDE , npr, "wide" , ncm )
symbolFlag( FLAG_WIDE_CLASS , npr, "wide class" , ncm )
symbolFlag( FLAG_WRAPPER , npr, "wrapper" , "wrapper function" )
symbolFlag( FLAG_WRAP_WRITTEN_FORMAL , npr, "wrap written formal" , "formal argument for wrapper for out/inout intent" )
symbolFlag( FLAG_GLOBAL_TYPE_SYMBOL, npr, "global type symbol", "is accessible through a global type variable")

#undef ypr
#undef npr
#undef ncm
