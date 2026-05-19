import { toSnakeCase, toRustFieldName } from "../util/names.js"
import {
  PROTO_TYPE_MAP,
  WireType,
  fieldTag,
  resolveRustType,
  needsVarintCast,
  varintDecodeCast
} from "./type-map.js"
import { log } from "../util/logger.js"

/** Parsed field descriptor subset needed for codegen. */
export interface FieldInfo {
  name: string
  number: number
  type: number
  typeName?: string
  label: number // 1=optional, 2=required, 3=repeated
  oneofIndex?: number
  mapEntry?: { keyType: number; valueType: number; valueTypeName?: string }
}

/** Check if field is repeated (label == 3). */
export function isRepeated(field: FieldInfo): boolean {
  return field.label === 3
}

/** Check if field is a message type (type == 11). */
export function isMessage(field: FieldInfo): boolean {
  return field.type === 11
}

/** Check if field is an enum type (type == 14). */
export function isEnum(field: FieldInfo): boolean {
  return field.type === 14
}

/**
 * Generate the Rust struct member declaration for a field.
 */
export function genStructMember(field: FieldInfo): string {
  const rustName = toRustFieldName(field.name)
  let rustType = resolveRustType(field.type, field.typeName)

  if (field.mapEntry) {
    const keyType = resolveRustType(field.mapEntry.keyType, undefined)
    const valType = resolveRustType(
      field.mapEntry.valueType,
      field.mapEntry.valueTypeName
    )
    // Maps become parallel Vecs: keys + values
    return [
      `    pub ${rustName}_keys: Vec<${keyType}>,`,
      `    pub ${rustName}_values: Vec<${valType}>,`
    ].join("\n")
  }

  if (isRepeated(field)) {
    rustType = `Vec<${rustType}>`
  }

  return `    pub ${rustName}: ${rustType},`
}

/**
 * Generate encode logic for a single field.
 * Returns Rust statements that append encoded bytes to a `Vec<u8> buf`.
 */
export function genFieldEncode(field: FieldInfo): string {
  const rustName = toRustFieldName(field.name)
  const typeInfo = PROTO_TYPE_MAP[field.type]

  if (!typeInfo) {
    log.warn(`Skipping unsupported field type ${field.type} for ${field.name}`)
    return `        // TODO: unsupported field type ${field.type} for ${field.name}`
  }

  const tag = fieldTag(
    field.number,
    field.mapEntry ? WireType.LengthDelimited : typeInfo.wireType
  )
  const tagHex = `0x${tag.toString(16).padStart(2, "0")}`

  if (field.mapEntry) {
    return genMapEncode(field, tagHex)
  }

  if (isRepeated(field)) {
    return genRepeatedEncode(field, rustName, typeInfo, tagHex)
  }

  if (isMessage(field)) {
    return genMessageEncode(field, rustName, tagHex)
  }

  if (isEnum(field)) {
    return genEnumEncode(field, rustName, tagHex)
  }

  return genScalarEncode(field, rustName, typeInfo, tagHex)
}

/**
 * Generate decode branch for a single field within the tag-dispatch match.
 * Returns a `TAG => { ... }` arm.
 */
export function genFieldDecode(field: FieldInfo): string {
  const rustName = toRustFieldName(field.name)
  const typeInfo = PROTO_TYPE_MAP[field.type]

  if (!typeInfo) {
    return `            // TODO: unsupported field type ${field.type} for ${field.name}`
  }

  const tag = fieldTag(
    field.number,
    field.mapEntry ? WireType.LengthDelimited : typeInfo.wireType
  )

  if (field.mapEntry) {
    return genMapDecode(field, rustName, tag)
  }

  if (isRepeated(field)) {
    return genRepeatedDecode(field, rustName, typeInfo, tag)
  }

  if (isMessage(field)) {
    return genMessageDecode(field, rustName, tag)
  }

  if (isEnum(field)) {
    return genEnumDecode(field, rustName, tag)
  }

  return genScalarDecode(field, rustName, typeInfo, tag)
}

// ── Internal codegen helpers ──────────────────────────────────────────

function genEnumEncode(
  field: FieldInfo,
  rustName: string,
  tagHex: string
): string {
  return [
    `        encode_key(&mut buf, ${tagHex});`,
    `        encode_varint(&mut buf, i32::from(self.${rustName}) as u64);`
  ].join("\n")
}

function genScalarEncode(
  field: FieldInfo,
  rustName: string,
  typeInfo: (typeof PROTO_TYPE_MAP)[number],
  tagHex: string
): string {
  const cast = needsVarintCast(field.type)
  // For float types, need to pass bits
  if (field.type === 1) {
    // double → f64.to_bits() → u64
    return [
      `        encode_key(&mut buf, ${tagHex});`,
      `        encode_fixed64(&mut buf, self.${rustName}.to_bits());`
    ].join("\n")
  }
  if (field.type === 2) {
    // float → f32.to_bits() → u32
    return [
      `        encode_key(&mut buf, ${tagHex});`,
      `        encode_fixed32(&mut buf, self.${rustName}.to_bits());`
    ].join("\n")
  }
  if (typeInfo.encodeRef) {
    return [
      `        encode_key(&mut buf, ${tagHex});`,
      `        ${typeInfo.encodeFunc}(&mut buf, &self.${rustName});`
    ].join("\n")
  }
  return [
    `        encode_key(&mut buf, ${tagHex});`,
    `        ${typeInfo.encodeFunc}(&mut buf, self.${rustName}${cast});`
  ].join("\n")
}

function genMessageEncode(
  field: FieldInfo,
  rustName: string,
  tagHex: string
): string {
  return [
    `        encode_key(&mut buf, ${tagHex});`,
    `        let ${rustName}_encoded = self.${rustName}.encode();`,
    `        encode_varint(&mut buf, ${rustName}_encoded.len() as u64);`,
    `        buf.extend_from_slice(&${rustName}_encoded);`
  ].join("\n")
}

function genRepeatedEncode(
  field: FieldInfo,
  rustName: string,
  typeInfo: (typeof PROTO_TYPE_MAP)[number],
  tagHex: string
): string {
  const lines: string[] = []
  lines.push(`        for elem in &self.${rustName} {`)
  lines.push(`            encode_key(&mut buf, ${tagHex});`)

  if (isMessage(field)) {
    lines.push(
      `            let elem_encoded = elem.encode();`,
      `            encode_varint(&mut buf, elem_encoded.len() as u64);`,
      `            buf.extend_from_slice(&elem_encoded);`
    )
  } else if (isEnum(field)) {
    lines.push(`            encode_varint(&mut buf, i32::from(*elem) as u64);`)
  } else if (field.type === 1) {
    // repeated double
    lines.push(`            encode_fixed64(&mut buf, elem.to_bits());`)
  } else if (field.type === 2) {
    // repeated float
    lines.push(`            encode_fixed32(&mut buf, elem.to_bits());`)
  } else if (typeInfo.encodeRef) {
    lines.push(`            ${typeInfo.encodeFunc}(&mut buf, elem);`)
  } else {
    const cast = needsVarintCast(field.type)
    lines.push(`            ${typeInfo.encodeFunc}(&mut buf, *elem${cast});`)
  }

  lines.push(`        }`)
  return lines.join("\n")
}

function genMapEncode(field: FieldInfo, tagHex: string): string {
  const rustName = toRustFieldName(field.name)
  const me = field.mapEntry!
  const keyInfo = PROTO_TYPE_MAP[me.keyType]
  const valInfo = PROTO_TYPE_MAP[me.valueType]

  const keyTag = `0x${fieldTag(1, keyInfo.wireType).toString(16).padStart(2, "0")}`
  const keyCast = needsVarintCast(me.keyType)

  const lines = [
    `        for i in 0..self.${rustName}_keys.len() {`,
    `            let mut entry = Vec::new();`,
    `            encode_key(&mut entry, ${keyTag});`
  ]

  if (keyInfo.encodeRef) {
    lines.push(`            ${keyInfo.encodeFunc}(&mut entry, &self.${rustName}_keys[i]);`)
  } else {
    lines.push(`            ${keyInfo.encodeFunc}(&mut entry, self.${rustName}_keys[i]${keyCast});`)
  }

  if (me.valueType === 11) {
    const valTag = `0x${fieldTag(2, WireType.LengthDelimited).toString(16).padStart(2, "0")}`
    lines.push(
      `            encode_key(&mut entry, ${valTag});`,
      `            let val_encoded = self.${rustName}_values[i].encode();`,
      `            encode_varint(&mut entry, val_encoded.len() as u64);`,
      `            entry.extend_from_slice(&val_encoded);`
    )
  } else {
    const valTag = `0x${fieldTag(2, valInfo.wireType).toString(16).padStart(2, "0")}`
    const valCast = needsVarintCast(me.valueType)
    lines.push(`            encode_key(&mut entry, ${valTag});`)
    if (valInfo.encodeRef) {
      lines.push(`            ${valInfo.encodeFunc}(&mut entry, &self.${rustName}_values[i]);`)
    } else {
      lines.push(`            ${valInfo.encodeFunc}(&mut entry, self.${rustName}_values[i]${valCast});`)
    }
  }

  lines.push(
    `            encode_key(&mut buf, ${tagHex});`,
    `            encode_varint(&mut buf, entry.len() as u64);`,
    `            buf.extend_from_slice(&entry);`,
    `        }`
  )
  return lines.join("\n")
}

function genEnumDecode(
  field: FieldInfo,
  rustName: string,
  tag: number
): string {
  const enumType = resolveRustType(field.type, field.typeName)
  return [
    `            ${tag} => {`,
    `                let (v, new_pos) = decode_varint(data, pos)?;`,
    `                msg.${rustName} = ${enumType}::from(v as i32);`,
    `                pos = new_pos;`,
    `            }`
  ].join("\n")
}

function genScalarDecode(
  field: FieldInfo,
  rustName: string,
  typeInfo: (typeof PROTO_TYPE_MAP)[number],
  tag: number
): string {
  const cast = varintDecodeCast(field.type)

  // Float types need from_bits
  if (field.type === 1) {
    // double → f64::from_bits
    return [
      `            ${tag} => {`,
      `                let (v, new_pos) = decode_fixed64(data, pos)?;`,
      `                msg.${rustName} = f64::from_bits(v);`,
      `                pos = new_pos;`,
      `            }`
    ].join("\n")
  }
  if (field.type === 2) {
    // float → f32::from_bits
    return [
      `            ${tag} => {`,
      `                let (v, new_pos) = decode_fixed32(data, pos)?;`,
      `                msg.${rustName} = f32::from_bits(v);`,
      `                pos = new_pos;`,
      `            }`
    ].join("\n")
  }

  return [
    `            ${tag} => {`,
    `                let (v, new_pos) = ${typeInfo.decodeFunc}(data, pos)?;`,
    `                msg.${rustName} = v${cast};`,
    `                pos = new_pos;`,
    `            }`
  ].join("\n")
}

function genMessageDecode(
  field: FieldInfo,
  rustName: string,
  tag: number
): string {
  const structType = resolveRustType(field.type, field.typeName)
  return [
    `            ${tag} => {`,
    `                let (len, new_pos) = decode_varint(data, pos)?;`,
    `                let end = new_pos + len as usize;`,
    `                msg.${rustName} = ${structType}::decode(&data[new_pos..end])?;`,
    `                pos = end;`,
    `            }`
  ].join("\n")
}

function genRepeatedDecode(
  field: FieldInfo,
  rustName: string,
  typeInfo: (typeof PROTO_TYPE_MAP)[number],
  tag: number
): string {
  if (isMessage(field)) {
    const structType = resolveRustType(field.type, field.typeName)
    return [
      `            ${tag} => {`,
      `                let (len, new_pos) = decode_varint(data, pos)?;`,
      `                let end = new_pos + len as usize;`,
      `                msg.${rustName}.push(${structType}::decode(&data[new_pos..end])?);`,
      `                pos = end;`,
      `            }`
    ].join("\n")
  }

  if (isEnum(field)) {
    const enumType = resolveRustType(field.type, field.typeName)
    return [
      `            ${tag} => {`,
      `                let (v, new_pos) = decode_varint(data, pos)?;`,
      `                msg.${rustName}.push(${enumType}::from(v as i32));`,
      `                pos = new_pos;`,
      `            }`
    ].join("\n")
  }

  const cast = varintDecodeCast(field.type)

  if (field.type === 1) {
    return [
      `            ${tag} => {`,
      `                let (v, new_pos) = decode_fixed64(data, pos)?;`,
      `                msg.${rustName}.push(f64::from_bits(v));`,
      `                pos = new_pos;`,
      `            }`
    ].join("\n")
  }
  if (field.type === 2) {
    return [
      `            ${tag} => {`,
      `                let (v, new_pos) = decode_fixed32(data, pos)?;`,
      `                msg.${rustName}.push(f32::from_bits(v));`,
      `                pos = new_pos;`,
      `            }`
    ].join("\n")
  }

  return [
    `            ${tag} => {`,
    `                let (v, new_pos) = ${typeInfo.decodeFunc}(data, pos)?;`,
    `                msg.${rustName}.push(v${cast});`,
    `                pos = new_pos;`,
    `            }`
  ].join("\n")
}

function genMapDecode(field: FieldInfo, rustName: string, tag: number): string {
  const me = field.mapEntry!
  const keyInfo = PROTO_TYPE_MAP[me.keyType]
  const valInfo = PROTO_TYPE_MAP[me.valueType]
  const keySol = resolveRustType(me.keyType, undefined)
  const valSol = resolveRustType(me.valueType, me.valueTypeName)

  const keyTag = fieldTag(1, keyInfo.wireType)
  const keyCast = varintDecodeCast(me.keyType)

  const lines = [
    `            ${tag} => {`,
    `                let (entry_len, new_pos) = decode_varint(data, pos)?;`,
    `                pos = new_pos;`,
    `                let entry_end = pos + entry_len as usize;`,
    `                let mut key: ${keySol} = Default::default();`,
    `                let mut val: ${valSol} = Default::default();`,
    `                while pos < entry_end {`,
    `                    let (entry_tag, new_pos) = decode_key(data, pos)?;`,
    `                    pos = new_pos;`,
    `                    match entry_tag {`,
    `                        ${keyTag} => {`,
    `                            let (v, new_pos) = ${keyInfo.decodeFunc}(data, pos)?;`,
    `                            key = v${keyCast};`,
    `                            pos = new_pos;`,
    `                        }`
  ]

  if (me.valueType === 11) {
    const valTag = fieldTag(2, WireType.LengthDelimited)
    lines.push(
      `                        ${valTag} => {`,
      `                            let (v_len, new_pos) = decode_varint(data, pos)?;`,
      `                            let v_end = new_pos + v_len as usize;`,
      `                            val = ${valSol}::decode(&data[new_pos..v_end])?;`,
      `                            pos = v_end;`,
      `                        }`
    )
  } else if (me.valueType === 14) {
    const valTag = fieldTag(2, valInfo.wireType)
    lines.push(
      `                        ${valTag} => {`,
      `                            let (v, new_pos) = decode_varint(data, pos)?;`,
      `                            val = ${valSol}::from(v as i32);`,
      `                            pos = new_pos;`,
      `                        }`
    )
  } else {
    const valTag = fieldTag(2, valInfo.wireType)
    const valCast = varintDecodeCast(me.valueType)
    lines.push(
      `                        ${valTag} => {`,
      `                            let (v, new_pos) = ${valInfo.decodeFunc}(data, pos)?;`,
      `                            val = v${valCast};`,
      `                            pos = new_pos;`,
      `                        }`
    )
  }

  lines.push(
    `                        _ => {`,
    `                            pos = skip_field(data, pos, entry_tag & 0x07)?;`,
    `                        }`,
    `                    }`,
    `                }`,
    `                msg.${rustName}_keys.push(key);`,
    `                msg.${rustName}_values.push(val);`,
    `            }`
  )
  return lines.join("\n")
}
