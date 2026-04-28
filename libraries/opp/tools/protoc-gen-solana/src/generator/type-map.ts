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
 * Mapping entry: protobuf field type → Rust type + wire metadata.
 */
export interface RustTypeInfo {
  /** Rust type string (e.g. "u64", "Vec<u8>", "String") */
  rustType: string
  /** Wire type for encode/decode dispatch */
  wireType: WireType
  /** Runtime encode function name */
  encodeFunc: string
  /** Runtime decode function name */
  decodeFunc: string
  /** Default value expression in Rust */
  defaultValue: string
  /** Whether the encode func takes a reference (&self.field) */
  encodeRef: boolean
}

/**
 * Map of protobuf FieldDescriptorProto.Type enum values to Rust metadata.
 *
 * Enum values from google/protobuf/descriptor.proto:
 *   1=double, 2=float, 3=int64, 4=uint64, 5=int32, 6=fixed64,
 *   7=fixed32, 8=bool, 9=string, 10=group, 11=message, 12=bytes,
 *   13=uint32, 14=enum, 15=sfixed32, 16=sfixed64, 17=sint32, 18=sint64
 */
export const PROTO_TYPE_MAP: Record<number, RustTypeInfo> = {
  // TYPE_DOUBLE = 1
  1: {
    rustType: "f64",
    wireType: WireType.Fixed64,
    encodeFunc: "encode_fixed64",
    decodeFunc: "decode_fixed64",
    defaultValue: "0.0",
    encodeRef: false
  },
  // TYPE_FLOAT = 2
  2: {
    rustType: "f32",
    wireType: WireType.Fixed32,
    encodeFunc: "encode_fixed32",
    decodeFunc: "decode_fixed32",
    defaultValue: "0.0",
    encodeRef: false
  },
  // TYPE_INT64 = 3
  3: {
    rustType: "i64",
    wireType: WireType.Varint,
    encodeFunc: "encode_varint",
    decodeFunc: "decode_varint",
    defaultValue: "0",
    encodeRef: false
  },
  // TYPE_UINT64 = 4
  4: {
    rustType: "u64",
    wireType: WireType.Varint,
    encodeFunc: "encode_varint",
    decodeFunc: "decode_varint",
    defaultValue: "0",
    encodeRef: false
  },
  // TYPE_INT32 = 5
  5: {
    rustType: "i32",
    wireType: WireType.Varint,
    encodeFunc: "encode_varint",
    decodeFunc: "decode_varint",
    defaultValue: "0",
    encodeRef: false
  },
  // TYPE_FIXED64 = 6
  6: {
    rustType: "u64",
    wireType: WireType.Fixed64,
    encodeFunc: "encode_fixed64",
    decodeFunc: "decode_fixed64",
    defaultValue: "0",
    encodeRef: false
  },
  // TYPE_FIXED32 = 7
  7: {
    rustType: "u32",
    wireType: WireType.Fixed32,
    encodeFunc: "encode_fixed32",
    decodeFunc: "decode_fixed32",
    defaultValue: "0",
    encodeRef: false
  },
  // TYPE_BOOL = 8
  8: {
    rustType: "bool",
    wireType: WireType.Varint,
    encodeFunc: "encode_bool",
    decodeFunc: "decode_bool",
    defaultValue: "false",
    encodeRef: false
  },
  // TYPE_STRING = 9
  9: {
    rustType: "String",
    wireType: WireType.LengthDelimited,
    encodeFunc: "encode_string",
    decodeFunc: "decode_string",
    defaultValue: "String::new()",
    encodeRef: true
  },
  // TYPE_MESSAGE = 11
  11: {
    rustType: "", // resolved per-field from typeName
    wireType: WireType.LengthDelimited,
    encodeFunc: "", // delegated to nested codec
    decodeFunc: "",
    defaultValue: "", // Default::default()
    encodeRef: true
  },
  // TYPE_BYTES = 12
  12: {
    rustType: "Vec<u8>",
    wireType: WireType.LengthDelimited,
    encodeFunc: "encode_bytes",
    decodeFunc: "decode_bytes",
    defaultValue: "Vec::new()",
    encodeRef: true
  },
  // TYPE_UINT32 = 13
  13: {
    rustType: "u32",
    wireType: WireType.Varint,
    encodeFunc: "encode_varint",
    decodeFunc: "decode_varint",
    defaultValue: "0",
    encodeRef: false
  },
  // TYPE_ENUM = 14
  14: {
    rustType: "i32",
    wireType: WireType.Varint,
    encodeFunc: "encode_varint",
    decodeFunc: "decode_varint",
    defaultValue: "0",
    encodeRef: false
  },
  // TYPE_SFIXED32 = 15
  15: {
    rustType: "i32",
    wireType: WireType.Fixed32,
    encodeFunc: "encode_sfixed32",
    decodeFunc: "decode_sfixed32",
    defaultValue: "0",
    encodeRef: false
  },
  // TYPE_SFIXED64 = 16
  16: {
    rustType: "i64",
    wireType: WireType.Fixed64,
    encodeFunc: "encode_sfixed64",
    decodeFunc: "decode_sfixed64",
    defaultValue: "0",
    encodeRef: false
  },
  // TYPE_SINT32 = 17
  17: {
    rustType: "i32",
    wireType: WireType.Varint,
    encodeFunc: "encode_zigzag32",
    decodeFunc: "decode_zigzag32",
    defaultValue: "0",
    encodeRef: false
  },
  // TYPE_SINT64 = 18
  18: {
    rustType: "i64",
    wireType: WireType.Varint,
    encodeFunc: "encode_zigzag64",
    decodeFunc: "decode_zigzag64",
    defaultValue: "0",
    encodeRef: false
  }
}

/**
 * Resolve the Rust type for a field descriptor.
 * For TYPE_MESSAGE, resolves from the nested message name.
 */
export function resolveRustType(
  fieldType: number,
  typeName: string | undefined
): string {
  if ((fieldType === 11 || fieldType === 14) && typeName) {
    // Message or enum → type name (strip leading dot and package prefix)
    const parts = typeName.replace(/^\./, "").split(".")
    return parts[parts.length - 1]
  }
  const info = PROTO_TYPE_MAP[fieldType]
  if (!info) {
    throw new Error(`Unsupported protobuf field type: ${fieldType}`)
  }
  return info.rustType
}

/**
 * Build a protobuf field tag (field_number << 3 | wire_type).
 */
export function fieldTag(fieldNumber: number, wireType: WireType): number {
  return (fieldNumber << 3) | wireType
}

/**
 * Check if a Rust type needs a cast for varint encode (non-u64 types).
 * Varint encode always takes u64, so smaller types need `as u64`.
 */
export function needsVarintCast(fieldType: number): string {
  const info = PROTO_TYPE_MAP[fieldType]
  if (!info) return ""
  if (info.encodeFunc === "encode_varint" && info.rustType !== "u64") {
    return " as u64"
  }
  return ""
}

/**
 * Get the cast needed when decoding a varint into a non-u64 type.
 */
export function varintDecodeCast(fieldType: number): string {
  const info = PROTO_TYPE_MAP[fieldType]
  if (!info) return ""
  if (info.decodeFunc === "decode_varint" && info.rustType !== "u64") {
    return ` as ${info.rustType}`
  }
  return ""
}
