import { protoNameToSol } from "../util/names.js"

/** A single enum value (name + numeric value). */
export interface EnumValueInfo {
  name: string
  number: number
}

/** Descriptor for a protobuf enum, ready for Solidity codegen. */
export interface EnumDescriptor {
  /** Simple name (e.g. "Role") */
  name: string
  /** Fully qualified name (e.g. "example.nested.test.Role") */
  fullName: string
  /** Enum values */
  values: EnumValueInfo[]
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
export function computeUnderlyingType(values: EnumValueInfo[]): string {
  if (values.length === 0) return "uint8"
  const maxVal = Math.max(0, ...values.map(v => v.number))
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

  // Library with constants and isValid
  lines.push(`library ${lib} {`)

  for (const val of desc.values) {
    lines.push(`    ${name} constant ${val.name} = ${name}.wrap(${val.number});`)
  }

  if (desc.values.length > 0) {
    const maxVal = desc.values.reduce(
      (max, v) => (v.number > max.number ? v : max),
      desc.values[0]
    )
    lines.push(``)
    lines.push(`    function isValid(${name} _v) internal pure returns (bool) {`)
    lines.push(`        return ${name}.unwrap(_v) <= ${name}.unwrap(${maxVal.name});`)
    lines.push(`    }`)
  }

  lines.push(`}`)

  return lines.join("\n")
}
