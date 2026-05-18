import { screamingSnakeToPascalCase, protoNameToRust } from "../util/names.js"
import { log } from "../util/logger.js"

/**
 * Descriptor for a single enum value (variant).
 */
export interface EnumValueDescriptor {
  name: string
  number: number
}

/**
 * Descriptor subset for a protobuf enum needed by the codegen.
 */
export interface EnumDescriptor {
  /** Simple name (e.g. "Role") */
  name: string
  /** Fully qualified name (e.g. "example.Role") */
  fullName: string
  /** Enum values (variants) */
  values: EnumValueDescriptor[]
}

/**
 * Generate a complete Rust enum definition with Default, From<i32>,
 * and Into<i32> implementations.
 *
 * Uses `#[repr(i32)]` so the enum is wire-compatible with protobuf
 * varint encoding and castable to/from i32 directly.
 */
export function genEnum(desc: EnumDescriptor): string {
  const enumName = protoNameToRust(desc.fullName)
  log.debug(`Generating enum ${enumName} (${desc.values.length} values)`)

  const defaultVariant = desc.values.find(v => v.number === 0)
  const defaultVariantName = defaultVariant
    ? screamingSnakeToPascalCase(defaultVariant.name)
    : desc.values.length > 0
      ? screamingSnakeToPascalCase(desc.values[0].name)
      : "Unknown"

  const variants = desc.values.map(
    v => `    ${screamingSnakeToPascalCase(v.name)} = ${v.number},`
  )

  const matchArms = desc.values.map(
    v =>
      `            ${v.number} => ${enumName}::${screamingSnakeToPascalCase(v.name)},`
  )

  const idlVariants = desc.values.map(
    v =>
      `            anchor_lang::idl::types::IdlEnumVariant { name: "${screamingSnakeToPascalCase(v.name)}".to_string(), fields: None },`
  )

  return [
    `#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]`,
    `#[repr(i32)]`,
    `pub enum ${enumName} {`,
    ...variants,
    `}`,
    ``,
    `impl Default for ${enumName} {`,
    `    fn default() -> Self {`,
    `        ${enumName}::${defaultVariantName}`,
    `    }`,
    `}`,
    ``,
    `impl From<i32> for ${enumName} {`,
    `    fn from(value: i32) -> Self {`,
    `        match value {`,
    ...matchArms,
    `            _ => ${enumName}::default(),`,
    `        }`,
    `    }`,
    `}`,
    ``,
    `impl From<${enumName}> for i32 {`,
    `    fn from(value: ${enumName}) -> Self {`,
    `        value as i32`,
    `    }`,
    `}`,
    ``,
    `#[cfg(feature = "borsh")]`,
    `impl borsh::BorshSerialize for ${enumName} {`,
    `    fn serialize<W: std::io::Write>(&self, writer: &mut W) -> std::io::Result<()> {`,
    `        <i32 as borsh::BorshSerialize>::serialize(&(*self as i32), writer)`,
    `    }`,
    `}`,
    ``,
    `#[cfg(feature = "borsh")]`,
    `impl borsh::BorshDeserialize for ${enumName} {`,
    `    fn deserialize_reader<R: std::io::Read>(reader: &mut R) -> std::io::Result<Self> {`,
    `        let discriminant = <i32 as borsh::BorshDeserialize>::deserialize_reader(reader)?;`,
    `        Ok(${enumName}::from(discriminant))`,
    `    }`,
    `}`,
    ``,
    `#[cfg(feature = "idl-build")]`,
    `impl anchor_lang::idl::build::IdlBuild for ${enumName} {`,
    `    fn create_type() -> Option<anchor_lang::idl::types::IdlTypeDef> {`,
    `        Some(anchor_lang::idl::types::IdlTypeDef {`,
    `            name: Self::get_full_path(),`,
    `            docs: vec![],`,
    `            serialization: anchor_lang::idl::types::IdlSerialization::default(),`,
    `            repr: None,`,
    `            generics: vec![],`,
    `            ty: anchor_lang::idl::types::IdlTypeDefTy::Enum {`,
    `                variants: vec![`,
    ...idlVariants,
    `                ],`,
    `            },`,
    `        })`,
    `    }`,
    ``,
    `    fn insert_types(types: &mut std::collections::BTreeMap<String, anchor_lang::idl::types::IdlTypeDef>) {`,
    `        if let Some(ty) = Self::create_type() {`,
    `            types.insert(Self::get_full_path(), ty);`,
    `        }`,
    `    }`,
    ``,
    `    fn get_full_path() -> String {`,
    `        format!("{}::{}", module_path!(), stringify!(${enumName}))`,
    `    }`,
    `}`
  ].join("\n")
}
