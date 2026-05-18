/**
 * Protobuf wire types (proto3 encoding).
 */
export const enum WireType {
  Varint = 0,
  Fixed64 = 1,
  LengthDelimited = 2,
  Fixed32 = 5
}

/**
 * Mapping entry: protobuf field type → Solidity type + wire metadata.
 */
export interface SolTypeInfo {
  /** Solidity type string (e.g. "uint64", "bytes", "string") */
  solType: string
  /** Wire type for encode/decode dispatch */
  wireType: WireType
  /** Runtime encode function name */
  encodeFunc: string
  /** Runtime decode function name */
  decodeFunc: string
  /** Default value expression in Solidity */
  defaultValue: string
}

/**
 * Map of protobuf FieldDescriptorProto.Type enum values to Solidity metadata.
 *
 * Enum values from google/protobuf/descriptor.proto:
 *   1=double, 2=float, 3=int64, 4=uint64, 5=int32, 6=fixed64,
 *   7=fixed32, 8=bool, 9=string, 10=group, 11=message, 12=bytes,
 *   13=uint32, 14=enum, 15=sfixed32, 16=sfixed64, 17=sint32, 18=sint64
 */
export const PROTO_TYPE_MAP: Record<number, SolTypeInfo> = {
  // TYPE_DOUBLE = 1
  1: {
    solType: "int64",
    wireType: WireType.Fixed64,
    encodeFunc: "_encode_fixed64",
    decodeFunc: "_decode_fixed64",
    defaultValue: "0"
  },
  // TYPE_FLOAT = 2
  2: {
    solType: "int32",
    wireType: WireType.Fixed32,
    encodeFunc: "_encode_fixed32",
    decodeFunc: "_decode_fixed32",
    defaultValue: "0"
  },
  // TYPE_INT64 = 3
  3: {
    solType: "int64",
    wireType: WireType.Varint,
    encodeFunc: "_encode_varint",
    decodeFunc: "_decode_varint",
    defaultValue: "0"
  },
  // TYPE_UINT64 = 4
  4: {
    solType: "uint64",
    wireType: WireType.Varint,
    encodeFunc: "_encode_varint",
    decodeFunc: "_decode_varint",
    defaultValue: "0"
  },
  // TYPE_INT32 = 5
  5: {
    solType: "int32",
    wireType: WireType.Varint,
    encodeFunc: "_encode_varint",
    decodeFunc: "_decode_varint",
    defaultValue: "0"
  },
  // TYPE_FIXED64 = 6
  6: {
    solType: "uint64",
    wireType: WireType.Fixed64,
    encodeFunc: "_encode_fixed64",
    decodeFunc: "_decode_fixed64",
    defaultValue: "0"
  },
  // TYPE_FIXED32 = 7
  7: {
    solType: "uint32",
    wireType: WireType.Fixed32,
    encodeFunc: "_encode_fixed32",
    decodeFunc: "_decode_fixed32",
    defaultValue: "0"
  },
  // TYPE_BOOL = 8
  8: {
    solType: "bool",
    wireType: WireType.Varint,
    encodeFunc: "_encode_bool",
    decodeFunc: "_decode_bool",
    defaultValue: "false"
  },
  // TYPE_STRING = 9
  9: {
    solType: "string",
    wireType: WireType.LengthDelimited,
    encodeFunc: "_encode_string",
    decodeFunc: "_decode_string",
    defaultValue: '""'
  },
  // TYPE_MESSAGE = 11
  11: {
    solType: "", // resolved per-field from typeName
    wireType: WireType.LengthDelimited,
    encodeFunc: "", // delegated to nested codec
    decodeFunc: "",
    defaultValue: "" // struct zero-init
  },
  // TYPE_BYTES = 12
  12: {
    solType: "bytes",
    wireType: WireType.LengthDelimited,
    encodeFunc: "_encode_bytes",
    decodeFunc: "_decode_bytes",
    defaultValue: '""'
  },
  // TYPE_UINT32 = 13
  13: {
    solType: "uint32",
    wireType: WireType.Varint,
    encodeFunc: "_encode_varint",
    decodeFunc: "_decode_varint",
    defaultValue: "0"
  },
  // TYPE_ENUM = 14
  14: {
    solType: "uint64",
    wireType: WireType.Varint,
    encodeFunc: "_encode_varint",
    decodeFunc: "_decode_varint",
    defaultValue: "0"
  },
  // TYPE_SFIXED32 = 15
  15: {
    solType: "int32",
    wireType: WireType.Fixed32,
    encodeFunc: "_encode_sfixed32",
    decodeFunc: "_decode_sfixed32",
    defaultValue: "0"
  },
  // TYPE_SFIXED64 = 16
  16: {
    solType: "int64",
    wireType: WireType.Fixed64,
    encodeFunc: "_encode_sfixed64",
    decodeFunc: "_decode_sfixed64",
    defaultValue: "0"
  },
  // TYPE_SINT32 = 17
  17: {
    solType: "int32",
    wireType: WireType.Varint,
    encodeFunc: "_encode_zigzag32",
    decodeFunc: "_decode_zigzag32",
    defaultValue: "0"
  },
  // TYPE_SINT64 = 18
  18: {
    solType: "int64",
    wireType: WireType.Varint,
    encodeFunc: "_encode_zigzag64",
    decodeFunc: "_decode_zigzag64",
    defaultValue: "0"
  }
}

/**
 * Resolve the Solidity type for a field descriptor.
 * For TYPE_MESSAGE, resolves from the nested message name.
 */
export function resolveSolType(
  fieldType: number,
  typeName: string | undefined
): string {
  if ((fieldType === 11 || fieldType === 14) && typeName) {
    // Message → struct name, Enum → UDVT name (strip leading dot and package prefix)
    const parts = typeName.replace(/^\./, "").split(".")
    return parts[parts.length - 1]
  }
  const info = PROTO_TYPE_MAP[fieldType]
  if (!info) {
    throw new Error(`Unsupported protobuf field type: ${fieldType}`)
  }
  return info.solType
}

/**
 * Build a protobuf field tag (field_number << 3 | wire_type).
 */
export function fieldTag(fieldNumber: number, wireType: WireType): number {
  return (fieldNumber << 3) | wireType
}
