/**
 * Convert a protobuf fully-qualified name to a Rust-safe identifier (PascalCase struct name).
 * e.g. "my_package.MyMessage" → "MyMessage"
 */
export function protoNameToRust(fqn: string): string {
  const parts = fqn.split(".")
  return parts[parts.length - 1]
}

/**
 * Keep snake_case field name as-is for Rust struct members.
 * e.g. "user_name" → "user_name"
 * Only converts camelCase → snake_case if needed.
 */
export function toSnakeCase(name: string): string {
  // Already snake_case in proto, but handle camelCase just in case
  return name.replace(/([a-z])([A-Z])/g, "$1_$2").toLowerCase()
}

/**
 * Rust reserved words (keywords + weak keywords) that cannot be used as
 * identifiers. Appending a trailing underscore is safe because protobuf
 * encodes by field number, not name.
 */
const RUST_RESERVED_WORDS: ReadonlySet<string> = new Set([
  // Strict keywords
  "as", "async", "await", "break", "const", "continue", "crate",
  "dyn", "else", "enum", "extern", "false", "fn", "for", "if",
  "impl", "in", "let", "loop", "match", "mod", "move", "mut",
  "pub", "ref", "return", "self", "Self", "static", "struct",
  "super", "trait", "true", "type", "unsafe", "use", "where", "while",
  // Reserved for future use
  "abstract", "become", "box", "do", "final", "macro", "override",
  "priv", "try", "typeof", "unsized", "virtual", "yield",
])

/**
 * Sanitize a Rust identifier by appending a trailing underscore if it
 * collides with a reserved word. Safe because protobuf encodes by field
 * number, not name.
 * e.g. "type" → "type_", "match" → "match_"
 */
export function sanitizeRustFieldName(name: string): string {
  return RUST_RESERVED_WORDS.has(name) ? `${name}_` : name
}

/**
 * Convert a proto field name to a Rust-safe struct member name.
 * Applies camelCase → snake_case conversion then reserved word sanitization.
 * e.g. "user_name" → "user_name", "type" → "type_", "myAddress" → "my_address"
 */
export function toRustFieldName(protoFieldName: string): string {
  return sanitizeRustFieldName(toSnakeCase(protoFieldName))
}

/**
 * Convert a SCREAMING_SNAKE_CASE protobuf enum variant name to PascalCase.
 * e.g. "ROLE_UNSPECIFIED" → "RoleUnspecified", "MY_VALUE" → "MyValue"
 */
export function screamingSnakeToPascalCase(name: string): string {
  return name
    .toLowerCase()
    .split("_")
    .filter(seg => seg.length > 0)
    .map(seg => seg.charAt(0).toUpperCase() + seg.slice(1))
    .join("")
}

/**
 * Generate output .rs filename for a given .proto file, optionally rooted
 * under a directory derived from the proto package name.
 * e.g. "my_service.proto" with package "example.nested"
 *      → "example/nested/my_service.rs"
 */
export function protoFileToRsFile(protoFile: string, packageName?: string): string {
  const base = protoFile.replace(/\.proto$/, "")
  const parts = base.split("/")
  const filename = parts[parts.length - 1]
  // Rust files use snake_case
  const snakeFilename = filename
    .replace(/([a-z])([A-Z])/g, "$1_$2")
    .toLowerCase()
  const rsBasename = `${snakeFilename}.rs`
  if (!packageName) return rsBasename
  const dir = packageName.split(".").join("/")
  return `${dir}/${rsBasename}`
}
