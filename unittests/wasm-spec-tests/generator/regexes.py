TYPE_REGEX = r'(\(type \(;)([0-9]+)(.*)'
TRAILING_PARENS_REGEX = r'\)+\s*$'
FUNC_REGEX = r'(\(func \(;)([0-9]+)(;\) \(type )([0-9]+)(.*)'
CALL_REGEX = r'(call )([0-9]+)(.*)'
CALL_INDIRECT_REGEX = r'(call_indirect \(type )([0-9]+)(.*)'
EXPORT_REGEX = r'(\(export ")([\w\-\.]+)(" \(func )([0-9]+)(.*)'
EXPORT_GLOBAL_REGEX = r'(\(export "[^"]*" \(global )([0-9]+)(.*)'
# wasm2wast prints the segment index comment and the `func` keyword only in newer wabt versions. The function list
# must run to the segment's closing parenthesis, so other element forms (e.g. `funcref (ref.func N)`) do not match.
ELEM_REGEX = r'(\(elem (?:\(;[0-9]+;\) )?\(i32.const 0\) (?:func(?=[\s)]) ?)?)([0-9\s]*)(\).*)'
IMPORT_REGEX = r'(\(import ")([\w]+)(" ")([\w]+)(" \(func \(;)([0-9]+)(;\) \(type )([0-9]+)(.*)'
GLOBAL_REGEX = r'(\(global \(;)([0-9]+)(.*)'
# get_global/set_global is the pre-1.0 instruction spelling, global.get/global.set the current one.
GET_SET_GLOBAL_REGEX = r'((?:get_global|set_global|global\.get|global\.set) )([0-9]+)(.*)'
START_REGEX = r'(start )([0-9]+)(.*)'
# wasm2wast prints the segment index comment only in newer wabt versions.
DATA_REGEX = r'(\(data (?:\(;[0-9]+;\) )?\(i32.const )([0-9]+)(.*)'
