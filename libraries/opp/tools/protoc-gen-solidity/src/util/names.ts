/**
 * Convert a protobuf fully-qualified name to a Solidity-safe identifier.
 * e.g. "my_package.MyMessage" → "MyMessage"
 */
export function protoNameToSol(fqn: string): string {
  const parts = fqn.split(".")
  return parts[parts.length - 1]
}

/**
 * Convert snake_case field name to camelCase for Solidity struct members.
 * e.g. "user_name" → "userName"
 */
export function snakeToCamel(name: string): string {
  return name.replace(/_([a-z])/g, (_, c: string) => c.toUpperCase())
}

/**
 * Solidity reserved words that cannot be used as identifiers.
 * Includes keywords, built-in type names, reserved future keywords,
 * and unit denominations.
 */
const SOL_RESERVED_WORDS: ReadonlySet<string> = new Set([
  // Language keywords
  "abstract", "after", "anonymous", "apply", "auto",
  "break", "byte",
  "calldata", "case", "catch", "constant", "constructor", "continue",
  "contract", "copyof",
  "default", "define", "delete", "do",
  "else", "emit", "enum", "error", "event", "external",
  "fallback", "false", "final", "for", "from", "function",
  "global",
  "hex",
  "if", "immutable", "implements", "import", "in", "indexed",
  "inline", "interface", "internal", "is",
  "let", "library",
  "mapping", "match", "memory", "modifier", "mutable",
  "new", "null",
  "of", "override",
  "partial", "payable", "pragma", "private", "promise", "public", "pure",
  "receive", "reference", "relocatable", "return", "returns", "revert",
  "sealed", "sizeof", "static", "storage", "struct", "super",
  "supports", "switch",
  "this", "throw", "true", "try", "type", "typedef", "typeof",
  "unchecked", "unicode", "using",
  "var", "view", "virtual",
  "while",
  // Built-in type names
  "address", "bool", "string", "bytes", "int", "uint", "fixed", "ufixed",
  // Sized int/uint types (int8..int256, uint8..uint256)
  ...Array.from({ length: 32 }, (_, i) => `int${(i + 1) * 8}`),
  ...Array.from({ length: 32 }, (_, i) => `uint${(i + 1) * 8}`),
  // Sized bytes types (bytes1..bytes32)
  ...Array.from({ length: 32 }, (_, i) => `bytes${i + 1}`),
  // Unit denominations
  "wei", "gwei", "ether", "seconds", "minutes", "hours", "days", "weeks", "years",
])

/**
 * Sanitize a Solidity identifier by appending a trailing underscore if it
 * collides with a reserved word. Safe because protobuf encodes by field
 * number, not name.
 * e.g. "type" → "type_", "address" → "address_"
 */
export function sanitizeFieldName(name: string): string {
  return SOL_RESERVED_WORDS.has(name) ? `${name}_` : name
}

/**
 * Convert a proto field name to a Solidity-safe struct member name.
 * Applies snake_case → camelCase conversion then reserved word sanitization.
 * e.g. "user_name" → "userName", "type" → "type_", "my_address" → "myAddress"
 */
export function toSolFieldName(protoFieldName: string): string {
  return sanitizeFieldName(snakeToCamel(protoFieldName))
}

/**
 * Convert a PascalCase struct name to a camelCase variable name.
 * e.g. "ChainId" → "chainId", "MessageHeader" → "messageHeader"
 */
export function structNameToVarName(structName: string): string {
  return structName.charAt(0).toLowerCase() + structName.slice(1)
}

/**
 * Generate the Solidity library name for a message's codec.
 * e.g. "MyMessage" → "MyMessageCodec"
 */
export function codecLibName(messageName: string): string {
  return `${messageName}Codec`
}

/**
 * Generate output .sol filename for a given .proto file, optionally rooted
 * under a directory derived from the proto package name.
 * e.g. "my_service.proto" with package "example.nested"
 *      → "example/nested/MyService.sol"
 */
export function protoFileToSolFile(protoFile: string, packageName?: string): string {
  const base = protoFile.replace(/\.proto$/, "")
  const parts = base.split("/")
  const filename = parts[parts.length - 1]
  const pascal = filename
    .split(/[_\-.]/)
    .map(s => s.charAt(0).toUpperCase() + s.slice(1))
    .join("")
  const solBasename = `${pascal}.sol`
  if (!packageName) return solBasename
  const dir = packageName.split(".").join("/")
  return `${dir}/${solBasename}`
}

/**
 * Compute the relative import path from a generated .sol file back to
 * ProtobufRuntime.sol (which is always emitted at the output root).
 * e.g. "example/nested/test/Example.sol" → "../../../ProtobufRuntime.sol"
 *      "Example.sol"                     → "./ProtobufRuntime.sol"
 */
export function runtimeImportPath(solFilePath: string): string {
  const parts = solFilePath.split("/")
  if (parts.length <= 1) {
    return "./ProtobufRuntime.sol"
  }
  const depth = parts.length - 1
  return "../".repeat(depth) + "ProtobufRuntime.sol"
}

/**
 * Compute the relative import path between two generated .sol files.
 * e.g. from "sysio/opp/Opp.sol" to "sysio/opp/types/Types.sol"
 *      → "./types/Types.sol"
 *      from "sysio/opp/attestations/Attestations.sol" to "sysio/opp/Opp.sol"
 *      → "../Opp.sol"
 */
export function relativeImportPath(fromSolFile: string, toSolFile: string): string {
  const fromDir = fromSolFile.split("/").slice(0, -1)
  const toParts = toSolFile.split("/")
  let common = 0
  while (
    common < fromDir.length &&
    common < toParts.length - 1 &&
    fromDir[common] === toParts[common]
  ) {
    common++
  }
  const ups = fromDir.length - common
  const remaining = toParts.slice(common)
  if (ups === 0) {
    return "./" + remaining.join("/")
  }
  return "../".repeat(ups) + remaining.join("/")
}

/**
 * Solidity pragma version range.
 */
export const SOL_PRAGMA = ">=0.8.0 <0.9.0"

/**
 * SPDX license identifier for generated files.
 */
export const SPDX_LICENSE = "MIT"
