import { protoNameToSol } from "../util/names.js"

/** A single enum value (name + numeric value). */
export interface EnumValueInfo {
  name: string
  number: number
}

/** A protobuf enum reservation. Both bounds are inclusive in descriptor.proto. */
export interface EnumReservedRangeInfo {
  start: number
  end: number
}

/** Descriptor for a protobuf enum, ready for Solidity codegen. */
export interface EnumDescriptor {
  /** Simple name (e.g. "Role") */
  name: string
  /** Fully qualified name (e.g. "example.nested.test.Role") */
  fullName: string
  /** Enum values */
  values: EnumValueInfo[]
  /** Numeric slots retired with `reserved`; decoded opaquely but never valid. */
  reservedRanges: EnumReservedRangeInfo[]
  /** Computed smallest unsigned integer type that fits all values */
  underlyingType: string
}

/** Registry mapping fully-qualified enum names (e.g. ".example.Role") to descriptors. */
export type EnumRegistry = Map<string, EnumDescriptor>

/**
 * Metadata attached to a field that references an enum type.
 */
export interface EnumFieldInfo {
  /** Solidity UDVT name (e.g. "Role") */
  solTypeName: string
  /** Underlying uint type (e.g. "uint8") */
  underlyingType: string
}

/**
 * Compute the smallest unsigned integer type that can hold all enum values.
 */
export function computeUnderlyingType(
  values: EnumValueInfo[],
  reservedRanges: EnumReservedRangeInfo[] = []
): string {
  const maxReserved = reservedRanges.map(range => range.end)
  const maxVal = Math.max(0, ...values.map(v => v.number), ...maxReserved)
  if (maxVal <= 0xff) return "uint8"
  if (maxVal <= 0xffff) return "uint16"
  if (maxVal <= 0xffffff) return "uint24"
  if (maxVal <= 0xffffffff) return "uint32"
  return "uint64"
}

/**
 * Generate the Solidity library name for an enum.
 * e.g. "Role" -> "RoleLib"
 */
export function enumLibName(enumName: string): string {
  return `${enumName}Lib`
}

/**
 * Generate Solidity code for an enum: UDVT definition, using statement, and library.
 */
export function genEnumDefinition(desc: EnumDescriptor): string {
  const name = protoNameToSol(desc.fullName)
  const lib = enumLibName(name)
  const underlying = desc.underlyingType

  const lines: string[] = []

  // User-defined value type
  lines.push(`type ${name} is ${underlying};`)
  lines.push(``)
  lines.push(`using {${lib}.isValid} for ${name} global;`)
  lines.push(``)

  // Library with constants and checked conversions.
  lines.push(`library ${lib} {`)
  lines.push(`    error InvalidEnumValue(uint64 raw);`)
  lines.push(``)

  for (const val of desc.values) {
    lines.push(
      `    ${name} constant ${val.name} = ${name}.wrap(${val.number});`
    )
  }

  const seenNumbers = new Set<number>()
  const uniqueValues = desc.values.filter(val => {
    if (seenNumbers.has(val.number)) return false
    seenNumbers.add(val.number)
    return true
  })
  const checks = uniqueValues.map(val => `_raw == ${val.number}`).join(" || ")

  lines.push(``)
  lines.push(`    function isValid(${name} _v) internal pure returns (bool) {`)
  lines.push(`        uint64 _raw = uint64(${name}.unwrap(_v));`)
  lines.push(`        return ${checks || "false"};`)
  lines.push(`    }`)

  lines.push(``)
  lines.push(
    `    function fromRaw(uint64 _raw) internal pure returns (${name}) {`
  )
  for (const val of uniqueValues) {
    lines.push(`        if (_raw == ${val.number}) return ${val.name};`)
  }
  for (const range of desc.reservedRanges) {
    const condition =
      range.end === range.start
        ? `_raw == ${range.start}`
        : `_raw >= ${range.start} && _raw <= ${range.end}`
    lines.push(
      `        if (${condition}) return ${name}.wrap(${underlying}(_raw));`
    )
  }
  lines.push(`        revert InvalidEnumValue(_raw);`)
  lines.push(`    }`)

  lines.push(`}`)

  return lines.join("\n")
}
