import {
  WireType,
  PROTO_TYPE_MAP,
  resolveRustType,
  fieldTag,
  needsVarintCast,
  varintDecodeCast,
} from "../src/generator/type-map"

describe("PROTO_TYPE_MAP", () => {
  it("maps TYPE_DOUBLE (1) to f64 with Fixed64 wire type", () => {
    const info = PROTO_TYPE_MAP[1]
    expect(info.rustType).toBe("f64")
    expect(info.wireType).toBe(WireType.Fixed64)
  })

  it("maps TYPE_FLOAT (2) to f32 with Fixed32 wire type", () => {
    const info = PROTO_TYPE_MAP[2]
    expect(info.rustType).toBe("f32")
    expect(info.wireType).toBe(WireType.Fixed32)
  })

  it("maps TYPE_INT64 (3) to i64 with Varint wire type", () => {
    const info = PROTO_TYPE_MAP[3]
    expect(info.rustType).toBe("i64")
    expect(info.wireType).toBe(WireType.Varint)
  })

  it("maps TYPE_UINT64 (4) to u64 with Varint wire type", () => {
    const info = PROTO_TYPE_MAP[4]
    expect(info.rustType).toBe("u64")
    expect(info.wireType).toBe(WireType.Varint)
  })

  it("maps TYPE_STRING (9) to String with LengthDelimited wire type", () => {
    const info = PROTO_TYPE_MAP[9]
    expect(info.rustType).toBe("String")
    expect(info.wireType).toBe(WireType.LengthDelimited)
    expect(info.encodeRef).toBe(true)
  })

  it("maps TYPE_BYTES (12) to Vec<u8>", () => {
    const info = PROTO_TYPE_MAP[12]
    expect(info.rustType).toBe("Vec<u8>")
    expect(info.wireType).toBe(WireType.LengthDelimited)
  })

  it("maps TYPE_BOOL (8) with encode_bool func", () => {
    const info = PROTO_TYPE_MAP[8]
    expect(info.rustType).toBe("bool")
    expect(info.encodeFunc).toBe("encode_bool")
    expect(info.decodeFunc).toBe("decode_bool")
  })

  it("maps TYPE_MESSAGE (11) with empty rustType (resolved per-field)", () => {
    const info = PROTO_TYPE_MAP[11]
    expect(info.rustType).toBe("")
    expect(info.wireType).toBe(WireType.LengthDelimited)
  })

  it("maps TYPE_ENUM (14) to i32", () => {
    expect(PROTO_TYPE_MAP[14].rustType).toBe("i32")
  })

  it("maps TYPE_SINT32 (17) with zigzag encode", () => {
    const info = PROTO_TYPE_MAP[17]
    expect(info.rustType).toBe("i32")
    expect(info.encodeFunc).toBe("encode_zigzag32")
    expect(info.decodeFunc).toBe("decode_zigzag32")
  })

  it("maps TYPE_SINT64 (18) with zigzag encode", () => {
    const info = PROTO_TYPE_MAP[18]
    expect(info.rustType).toBe("i64")
    expect(info.encodeFunc).toBe("encode_zigzag64")
    expect(info.decodeFunc).toBe("decode_zigzag64")
  })

  it("covers all 16 supported field types (1-9, 11-18)", () => {
    const expectedKeys = [1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 13, 14, 15, 16, 17, 18]
    for (const key of expectedKeys) {
      expect(PROTO_TYPE_MAP[key]).toBeDefined()
    }
  })
})

describe("resolveRustType", () => {
  it("resolves scalar types from PROTO_TYPE_MAP", () => {
    expect(resolveRustType(5, undefined)).toBe("i32")
    expect(resolveRustType(4, undefined)).toBe("u64")
    expect(resolveRustType(9, undefined)).toBe("String")
  })

  it("resolves TYPE_MESSAGE from typeName, stripping package prefix", () => {
    expect(resolveRustType(11, ".my_package.MyMessage")).toBe("MyMessage")
  })

  it("resolves TYPE_MESSAGE with deeply nested package", () => {
    expect(resolveRustType(11, ".com.example.v1.Transfer")).toBe("Transfer")
  })

  it("resolves TYPE_MESSAGE without leading dot", () => {
    expect(resolveRustType(11, "my_package.MyMessage")).toBe("MyMessage")
  })

  it("throws for unsupported field type", () => {
    expect(() => resolveRustType(99, undefined)).toThrow(
      "Unsupported protobuf field type: 99"
    )
  })

  it("uses typeName for TYPE_MESSAGE even if it has no package", () => {
    expect(resolveRustType(11, "SimpleMsg")).toBe("SimpleMsg")
  })

  it("resolves TYPE_ENUM (14) from typeName, stripping package prefix", () => {
    expect(resolveRustType(14, ".example.Role")).toBe("Role")
  })

  it("resolves TYPE_ENUM with deeply nested package", () => {
    expect(resolveRustType(14, ".sysio.opp.types.ChainType")).toBe("ChainType")
  })

  it("falls back to i32 for TYPE_ENUM without typeName", () => {
    expect(resolveRustType(14, undefined)).toBe("i32")
  })
})

describe("fieldTag", () => {
  it("computes (fieldNumber << 3) | wireType", () => {
    // field 1, Varint(0) → 8 | 0 = 8
    expect(fieldTag(1, WireType.Varint)).toBe(8)
  })

  it("computes tag for field 2, LengthDelimited", () => {
    // field 2, LengthDelimited(2) → 16 | 2 = 18
    expect(fieldTag(2, WireType.LengthDelimited)).toBe(18)
  })

  it("computes tag for field 1, Fixed64", () => {
    // field 1, Fixed64(1) → 8 | 1 = 9
    expect(fieldTag(1, WireType.Fixed64)).toBe(9)
  })

  it("computes tag for field 3, Fixed32", () => {
    // field 3, Fixed32(5) → 24 | 5 = 29
    expect(fieldTag(3, WireType.Fixed32)).toBe(29)
  })

  it("handles large field numbers", () => {
    expect(fieldTag(100, WireType.Varint)).toBe(800)
  })
})

describe("needsVarintCast", () => {
  it("returns ' as u64' for i32 varint types", () => {
    // TYPE_INT32 = 5 → i32, encode_varint → needs cast
    expect(needsVarintCast(5)).toBe(" as u64")
  })

  it("returns empty string for u64 (no cast needed)", () => {
    // TYPE_UINT64 = 4 → u64, no cast needed
    expect(needsVarintCast(4)).toBe("")
  })

  it("returns ' as u64' for i64 varint types", () => {
    // TYPE_INT64 = 3 → i64, encode_varint → needs cast
    expect(needsVarintCast(3)).toBe(" as u64")
  })

  it("returns empty string for non-varint types", () => {
    // TYPE_DOUBLE = 1 → encode_fixed64, not varint
    expect(needsVarintCast(1)).toBe("")
  })

  it("returns empty string for bool (uses encode_bool, not encode_varint)", () => {
    expect(needsVarintCast(8)).toBe("")
  })

  it("returns empty string for unknown type", () => {
    expect(needsVarintCast(99)).toBe("")
  })

  it("returns ' as u64' for u32 varint type", () => {
    // TYPE_UINT32 = 13
    expect(needsVarintCast(13)).toBe(" as u64")
  })
})

describe("varintDecodeCast", () => {
  it("returns ' as i32' for i32 varint types", () => {
    // TYPE_INT32 = 5
    expect(varintDecodeCast(5)).toBe(" as i32")
  })

  it("returns empty string for u64 (no cast needed)", () => {
    expect(varintDecodeCast(4)).toBe("")
  })

  it("returns ' as i64' for i64 varint types", () => {
    expect(varintDecodeCast(3)).toBe(" as i64")
  })

  it("returns ' as u32' for u32 varint types", () => {
    // TYPE_UINT32 = 13
    expect(varintDecodeCast(13)).toBe(" as u32")
  })

  it("returns empty string for non-varint types", () => {
    expect(varintDecodeCast(1)).toBe("")
  })

  it("returns empty string for unknown type", () => {
    expect(varintDecodeCast(99)).toBe("")
  })
})
