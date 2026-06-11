import { toSolFieldName, codecLibName } from "../util/names.js"
import {
  PROTO_TYPE_MAP,
  WireType,
  fieldTag,
  resolveSolType
} from "./type-map.js"
import type { EnumFieldInfo } from "./enum.js"
import { log } from "../util/logger.js"

export type { EnumFieldInfo } from "./enum.js"

/** Parsed field descriptor subset needed for codegen. */
export interface FieldInfo {
  name: string
  number: number
  type: number
  typeName?: string
  label: number // 1=optional, 2=required, 3=repeated
  oneofIndex?: number
  mapEntry?: { keyType: number; valueType: number; valueTypeName?: string; valueEnumInfo?: EnumFieldInfo }
  enumInfo?: EnumFieldInfo
}

/** Check if field is repeated (label == 3). */
export function isRepeated(field: FieldInfo): boolean {
  return field.label === 3
}

/** Check if field is a message type (type == 11). */
export function isMessage(field: FieldInfo): boolean {
  return field.type === 11
}

/** Check if field is an enum type with resolved enum info. */
export function isEnum(field: FieldInfo): boolean {
  return field.type === 14 && !!field.enumInfo
}

/**
 * Generate the Solidity struct member declaration for a field.
 */
export function genStructMember(field: FieldInfo): string {
  const solName = toSolFieldName(field.name)
  let solType = resolveSolType(field.type, field.typeName)

  if (field.mapEntry) {
    const keyType = resolveSolType(field.mapEntry.keyType, undefined)
    const valType = resolveSolType(
      field.mapEntry.valueType,
      field.mapEntry.valueTypeName
    )
    // Maps become parallel arrays: keys + values
    return [
      `    ${keyType}[] ${solName}_keys;`,
      `    ${valType}[] ${solName}_values;`
    ].join("\n")
  }

  if (isRepeated(field)) {
    solType = `${solType}[]`
  }

  return `    ${solType} ${solName};`
}

/**
 * Generate encode logic for a single field.
 * Returns Solidity statements that append encoded bytes to a `bytes memory buf`.
 * @param varName - the Solidity variable name for the struct instance (e.g. "chainId")
 */
export function genFieldEncode(field: FieldInfo, varName: string): string {
  const solName = toSolFieldName(field.name)
  const typeInfo = PROTO_TYPE_MAP[field.type]

  if (!typeInfo) {
    log.warn(`Skipping unsupported field type ${field.type} for ${field.name}`)
    return `    // TODO: unsupported field type ${field.type} for ${field.name}`
  }

  const tag = fieldTag(
    field.number,
    field.mapEntry ? WireType.LengthDelimited : typeInfo.wireType
  )
  const tagHex = `0x${tag.toString(16)}`

  if (field.mapEntry) {
    return genMapEncode(field, tagHex, varName)
  }

  if (isRepeated(field)) {
    return genRepeatedEncode(field, solName, typeInfo, tagHex, varName)
  }

  if (isMessage(field)) {
    return genMessageEncode(field, solName, tagHex, varName)
  }

  if (field.enumInfo) {
    return genEnumFieldEncode(field, solName, tagHex, varName)
  }

  return genScalarEncode(solName, typeInfo, tagHex, varName)
}

/**
 * Tag + body pair returned by genFieldDecode for if/else-if dispatch.
 */
export interface DecodeBranch {
  tag: number
  body: string
}

/**
 * Generate decode branch for a single field.
 * Returns the wire tag and the body statements (indented at 8 spaces)
 * to be placed inside an if/else-if block by the caller.
 */
export function genFieldDecode(field: FieldInfo, varName: string): DecodeBranch {
  const solName = toSolFieldName(field.name)
  const typeInfo = PROTO_TYPE_MAP[field.type]

  if (!typeInfo) {
    const tag = fieldTag(
      field.number,
      field.mapEntry ? WireType.LengthDelimited : 0
    )
    return { tag, body: `        // TODO: unsupported field type ${field.type} for ${field.name}` }
  }

  const tag = fieldTag(
    field.number,
    field.mapEntry ? WireType.LengthDelimited : typeInfo.wireType
  )

  if (field.mapEntry) {
    return { tag, body: genMapDecode(field, solName, varName) }
  }

  if (isRepeated(field)) {
    return { tag, body: genRepeatedDecode(field, solName, typeInfo, varName) }
  }

  if (isMessage(field)) {
    return { tag, body: genMessageDecode(field, solName, varName) }
  }

  if (field.enumInfo) {
    return { tag, body: genEnumFieldDecode(field, solName, varName) }
  }

  return { tag, body: genScalarDecode(solName, typeInfo, varName) }
}

// ── Cast helpers for varint encode/decode ────────────────────────────

/**
 * Solidity 0.8 only allows changing one of size or signedness per explicit cast.
 * _decode_varint returns uint64; this wraps the value to the target solType.
 */
function castFromUint64(solType: string, expr: string): string {
  if (solType === "uint64") return expr
  if (solType === "int64") return `int64(${expr})`
  if (solType === "uint32") return `uint32(${expr})`
  if (solType === "int32") return `int32(int64(${expr}))`
  return `${solType}(${expr})`
}

/**
 * _encode_varint expects uint64; this wraps the source solType value.
 */
function castToUint64(solType: string, expr: string): string {
  if (solType === "uint64" || solType === "uint32") return expr // implicit widening OK
  if (solType === "int64") return `uint64(${expr})`
  if (solType === "int32") return `uint64(int64(${expr}))`
  return `uint64(${expr})`
}

/** True when _decode_varint needs an explicit cast to the target type. */
function needsVarintDecodeCast(typeInfo: (typeof PROTO_TYPE_MAP)[number]): boolean {
  return typeInfo.decodeFunc === "_decode_varint" && typeInfo.solType !== "uint64"
}

/** True when _encode_varint needs an explicit cast from the source type. */
function needsVarintEncodeCast(typeInfo: (typeof PROTO_TYPE_MAP)[number]): boolean {
  return typeInfo.encodeFunc === "_encode_varint" &&
    typeInfo.solType !== "uint64" && typeInfo.solType !== "uint32"
}

// ── Internal codegen helpers ──────────────────────────────────────────

function genEnumFieldEncode(
  field: FieldInfo,
  solName: string,
  tagHex: string,
  varName: string
): string {
  const ei = field.enumInfo!
  return [
    `    buf = abi.encodePacked(buf, ProtobufRuntime._encode_key(${tagHex}));`,
    `    buf = abi.encodePacked(buf, ProtobufRuntime._encode_varint(uint64(${ei.solTypeName}.unwrap(${varName}.${solName}))));`
  ].join("\n")
}

function genEnumFieldDecode(
  field: FieldInfo,
  solName: string,
  varName: string
): string {
  const ei = field.enumInfo!
  return [
    `        { uint64 _v;`,
    `        (_v, pos) = ProtobufRuntime._decode_varint(data, pos);`,
    `        ${varName}.${solName} = ${ei.solTypeName}.wrap(${ei.underlyingType}(_v)); }`
  ].join("\n")
}

function genScalarEncode(
  solName: string,
  typeInfo: (typeof PROTO_TYPE_MAP)[number],
  tagHex: string,
  varName: string
): string {
  const val = needsVarintEncodeCast(typeInfo)
    ? `${castToUint64(typeInfo.solType, `${varName}.${solName}`)}`
    : `${varName}.${solName}`
  return [
    `    buf = abi.encodePacked(buf, ProtobufRuntime._encode_key(${tagHex}));`,
    `    buf = abi.encodePacked(buf, ProtobufRuntime.${typeInfo.encodeFunc}(${val}));`
  ].join("\n")
}

function genMessageEncode(
  field: FieldInfo,
  solName: string,
  tagHex: string,
  varName: string
): string {
  const nestedCodec = codecLibName(resolveSolType(field.type, field.typeName))
  return [
    `    buf = abi.encodePacked(buf, ProtobufRuntime._encode_key(${tagHex}));`,
    `    bytes memory ${solName}_encoded = ${nestedCodec}.encode(${varName}.${solName});`,
    `    buf = abi.encodePacked(buf, ProtobufRuntime._encode_varint(uint64(${solName}_encoded.length)));`,
    `    buf = abi.encodePacked(buf, ${solName}_encoded);`
  ].join("\n")
}

function genRepeatedEncode(
  field: FieldInfo,
  solName: string,
  typeInfo: (typeof PROTO_TYPE_MAP)[number],
  tagHex: string,
  varName: string
): string {
  const loopVar = `_i_${solName}`
  const lines = [
    `    for (uint256 ${loopVar} = 0; ${loopVar} < ${varName}.${solName}.length; ${loopVar}++) {`,
    `      buf = abi.encodePacked(buf, ProtobufRuntime._encode_key(${tagHex}));`
  ]

  if (isMessage(field)) {
    const nestedCodec = codecLibName(resolveSolType(field.type, field.typeName))
    lines.push(
      `      bytes memory _elem = ${nestedCodec}.encode(${varName}.${solName}[${loopVar}]);`,
      `      buf = abi.encodePacked(buf, ProtobufRuntime._encode_varint(uint64(_elem.length)));`,
      `      buf = abi.encodePacked(buf, _elem);`
    )
  } else if (field.enumInfo) {
    const ei = field.enumInfo
    lines.push(
      `      buf = abi.encodePacked(buf, ProtobufRuntime._encode_varint(uint64(${ei.solTypeName}.unwrap(${varName}.${solName}[${loopVar}]))));`
    )
  } else {
    const elemExpr = needsVarintEncodeCast(typeInfo)
      ? castToUint64(typeInfo.solType, `${varName}.${solName}[${loopVar}]`)
      : `${varName}.${solName}[${loopVar}]`
    lines.push(
      `      buf = abi.encodePacked(buf, ProtobufRuntime.${typeInfo.encodeFunc}(${elemExpr}));`
    )
  }

  lines.push(`    }`)
  return lines.join("\n")
}

function genMapEncode(field: FieldInfo, tagHex: string, varName: string): string {
  const solName = toSolFieldName(field.name)
  const loopVar = `_i_${solName}`
  const me = field.mapEntry!
  const keyInfo = PROTO_TYPE_MAP[me.keyType]
  const valInfo = PROTO_TYPE_MAP[me.valueType]

  const lines = [
    `    for (uint256 ${loopVar} = 0; ${loopVar} < ${varName}.${solName}_keys.length; ${loopVar}++) {`,
    `      bytes memory _entry = "";`,
    `      _entry = abi.encodePacked(_entry, ProtobufRuntime._encode_key(${fieldTag(1, keyInfo.wireType)}));`,
    `      _entry = abi.encodePacked(_entry, ProtobufRuntime.${keyInfo.encodeFunc}(${varName}.${solName}_keys[${loopVar}]));`
  ]

  if (me.valueType === 11) {
    const nestedCodec = codecLibName(
      resolveSolType(me.valueType, me.valueTypeName)
    )
    lines.push(
      `      bytes memory _val = ${nestedCodec}.encode(${varName}.${solName}_values[${loopVar}]);`,
      `      _entry = abi.encodePacked(_entry, ProtobufRuntime._encode_key(${fieldTag(2, WireType.LengthDelimited)}));`,
      `      _entry = abi.encodePacked(_entry, ProtobufRuntime._encode_varint(uint64(_val.length)));`,
      `      _entry = abi.encodePacked(_entry, _val);`
    )
  } else if (me.valueType === 14 && field.mapEntry?.valueEnumInfo) {
    const vei = field.mapEntry.valueEnumInfo
    lines.push(
      `      _entry = abi.encodePacked(_entry, ProtobufRuntime._encode_key(${fieldTag(2, valInfo.wireType)}));`,
      `      _entry = abi.encodePacked(_entry, ProtobufRuntime._encode_varint(uint64(${vei.solTypeName}.unwrap(${varName}.${solName}_values[${loopVar}]))));`
    )
  } else {
    lines.push(
      `      _entry = abi.encodePacked(_entry, ProtobufRuntime._encode_key(${fieldTag(2, valInfo.wireType)}));`,
      `      _entry = abi.encodePacked(_entry, ProtobufRuntime.${valInfo.encodeFunc}(${varName}.${solName}_values[${loopVar}]));`
    )
  }

  lines.push(
    `      buf = abi.encodePacked(buf, ProtobufRuntime._encode_key(${tagHex}));`,
    `      buf = abi.encodePacked(buf, ProtobufRuntime._encode_varint(uint64(_entry.length)));`,
    `      buf = abi.encodePacked(buf, _entry);`,
    `    }`
  )
  return lines.join("\n")
}

function genScalarDecode(
  solName: string,
  typeInfo: (typeof PROTO_TYPE_MAP)[number],
  varName: string
): string {
  if (needsVarintDecodeCast(typeInfo)) {
    const cast = castFromUint64(typeInfo.solType, "_v")
    return [
      `        { uint64 _v;`,
      `        (_v, pos) = ProtobufRuntime.${typeInfo.decodeFunc}(data, pos);`,
      `        ${varName}.${solName} = ${cast}; }`
    ].join("\n")
  }
  return `        (${varName}.${solName}, pos) = ProtobufRuntime.${typeInfo.decodeFunc}(data, pos);`
}

function genMessageDecode(
  field: FieldInfo,
  solName: string,
  varName: string
): string {
  const nestedCodec = codecLibName(resolveSolType(field.type, field.typeName))
  return [
    `        uint64 _len;`,
    `        (_len, pos) = ProtobufRuntime._decode_varint(data, pos);`,
    `        bytes memory _sub = ProtobufRuntime._slice(data, pos, pos + uint256(_len));`,
    `        ${varName}.${solName} = ${nestedCodec}.decode(_sub);`,
    `        pos += uint256(_len);`
  ].join("\n")
}

function genRepeatedDecode(
  field: FieldInfo,
  solName: string,
  typeInfo: (typeof PROTO_TYPE_MAP)[number],
  varName: string
): string {
  const idxVar = `_idx_${solName}`

  if (isMessage(field)) {
    const nestedCodec = codecLibName(resolveSolType(field.type, field.typeName))
    return [
      `        uint64 _len;`,
      `        (_len, pos) = ProtobufRuntime._decode_varint(data, pos);`,
      `        bytes memory _sub = ProtobufRuntime._slice(data, pos, pos + uint256(_len));`,
      `        ${varName}.${solName}[${idxVar}++] = ${nestedCodec}.decode(_sub);`,
      `        pos += uint256(_len);`
    ].join("\n")
  }

  if (field.enumInfo) {
    const ei = field.enumInfo
    return [
      `        { uint64 _elem;`,
      `        (_elem, pos) = ProtobufRuntime._decode_varint(data, pos);`,
      `        ${varName}.${solName}[${idxVar}++] = ${ei.solTypeName}.wrap(${ei.underlyingType}(_elem)); }`
    ].join("\n")
  }

  if (needsVarintDecodeCast(typeInfo)) {
    const cast = castFromUint64(typeInfo.solType, "_elem")
    return [
      `        { uint64 _elem;`,
      `        (_elem, pos) = ProtobufRuntime.${typeInfo.decodeFunc}(data, pos);`,
      `        ${varName}.${solName}[${idxVar}++] = ${cast}; }`
    ].join("\n")
  }

  return [
    `        ${resolveSolType(field.type, field.typeName)} _elem;`,
    `        (_elem, pos) = ProtobufRuntime.${typeInfo.decodeFunc}(data, pos);`,
    `        ${varName}.${solName}[${idxVar}++] = _elem;`
  ].join("\n")
}

function genMapDecode(field: FieldInfo, solName: string, varName: string): string {
  const me = field.mapEntry!
  const keyInfo = PROTO_TYPE_MAP[me.keyType]
  const valInfo = PROTO_TYPE_MAP[me.valueType]
  const keySol = resolveSolType(me.keyType, undefined)
  const valSol = resolveSolType(me.valueType, me.valueTypeName)

  const lines = [
    `        uint64 _entryLen;`,
    `        (_entryLen, pos) = ProtobufRuntime._decode_varint(data, pos);`,
    `        uint256 _entryEnd = pos + uint256(_entryLen);`,
    `        ${keySol} _key;`,
    `        ${valSol} _val;`,
    `        while (pos < _entryEnd) {`,
    `          uint64 _entryTag;`,
    `          (_entryTag, pos) = ProtobufRuntime._decode_key(data, pos);`,
    `          if (_entryTag == ${fieldTag(1, keyInfo.wireType)}) {`,
    `            (_key, pos) = ProtobufRuntime.${keyInfo.decodeFunc}(data, pos);`
  ]

  if (me.valueType === 11) {
    const nestedCodec = codecLibName(
      resolveSolType(me.valueType, me.valueTypeName)
    )
    lines.push(
      `          } else if (_entryTag == ${fieldTag(2, WireType.LengthDelimited)}) {`,
      `            uint64 _vLen;`,
      `            (_vLen, pos) = ProtobufRuntime._decode_varint(data, pos);`,
      `            bytes memory _vSub = ProtobufRuntime._slice(data, pos, pos + uint256(_vLen));`,
      `            _val = ${nestedCodec}.decode(_vSub);`,
      `            pos += uint256(_vLen);`
    )
  } else if (me.valueType === 14 && field.mapEntry?.valueEnumInfo) {
    const vei = field.mapEntry.valueEnumInfo
    lines.push(
      `          } else if (_entryTag == ${fieldTag(2, valInfo.wireType)}) {`,
      `            { uint64 _raw;`,
      `            (_raw, pos) = ProtobufRuntime._decode_varint(data, pos);`,
      `            _val = ${vei.solTypeName}.wrap(${vei.underlyingType}(_raw)); }`
    )
  } else {
    lines.push(
      `          } else if (_entryTag == ${fieldTag(2, valInfo.wireType)}) {`,
      `            (_val, pos) = ProtobufRuntime.${valInfo.decodeFunc}(data, pos);`
    )
  }

  lines.push(
    `          } else {`,
    `            revert("Unknown map entry tag");`,
    `          }`,
    `        }`,
    `        ${varName}.${solName}_keys[_idx_${solName}] = _key;`,
    `        ${varName}.${solName}_values[_idx_${solName}++] = _val;`
  )
  return lines.join("\n")
}
