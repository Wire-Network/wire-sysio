import {
  PROTO_TYPE_MAP,
  resolveSolType,
  fieldTag,
  WireType
} from "@wireio/protoc-gen-solidity/generator/type-map"

describe("PROTO_TYPE_MAP", () => {
  it("maps TYPE_BOOL (8) to bool", () => {
    expect(PROTO_TYPE_MAP[8].solType).toBe("bool")
    expect(PROTO_TYPE_MAP[8].defaultValue).toBe("false")
  })

  it("maps TYPE_STRING (9) to string", () => {
    expect(PROTO_TYPE_MAP[9].solType).toBe("string")
    expect(PROTO_TYPE_MAP[9].wireType).toBe(WireType.LengthDelimited)
  })

  it("maps TYPE_BYTES (12) to bytes", () => {
    expect(PROTO_TYPE_MAP[12].solType).toBe("bytes")
    expect(PROTO_TYPE_MAP[12].wireType).toBe(WireType.LengthDelimited)
  })

  it("maps TYPE_UINT64 (4) to uint64 with varint wire type", () => {
    expect(PROTO_TYPE_MAP[4].solType).toBe("uint64")
    expect(PROTO_TYPE_MAP[4].wireType).toBe(WireType.Varint)
  })

  it("maps TYPE_INT32 (5) to int32", () => {
    expect(PROTO_TYPE_MAP[5].solType).toBe("int32")
  })

  it("maps TYPE_FIXED64 (6) to uint64 with Fixed64 wire type", () => {
    expect(PROTO_TYPE_MAP[6].solType).toBe("uint64")
    expect(PROTO_TYPE_MAP[6].wireType).toBe(WireType.Fixed64)
  })

  it("maps TYPE_FIXED32 (7) to uint32 with Fixed32 wire type", () => {
    expect(PROTO_TYPE_MAP[7].solType).toBe("uint32")
    expect(PROTO_TYPE_MAP[7].wireType).toBe(WireType.Fixed32)
  })

  it("maps TYPE_UINT32 (13) to uint32", () => {
    expect(PROTO_TYPE_MAP[13].solType).toBe("uint32")
  })

  it("maps TYPE_MESSAGE (11) with empty solType (resolved per-field)", () => {
    expect(PROTO_TYPE_MAP[11].solType).toBe("")
    expect(PROTO_TYPE_MAP[11].wireType).toBe(WireType.LengthDelimited)
  })

  it("maps TYPE_ENUM (14) to uint64 varint", () => {
    expect(PROTO_TYPE_MAP[14].solType).toBe("uint64")
    expect(PROTO_TYPE_MAP[14].wireType).toBe(WireType.Varint)
  })

  it("maps TYPE_SINT32 (17) with zigzag encode/decode", () => {
    expect(PROTO_TYPE_MAP[17].encodeFunc).toBe("_encode_zigzag32")
    expect(PROTO_TYPE_MAP[17].decodeFunc).toBe("_decode_zigzag32")
  })

  it("maps TYPE_SINT64 (18) with zigzag encode/decode", () => {
    expect(PROTO_TYPE_MAP[18].encodeFunc).toBe("_encode_zigzag64")
    expect(PROTO_TYPE_MAP[18].decodeFunc).toBe("_decode_zigzag64")
  })

  it("has entries for all expected types", () => {
    const expectedTypes = [
      1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 13, 14, 15, 16, 17, 18
    ]
    for (const t of expectedTypes) {
      expect(PROTO_TYPE_MAP[t]).toBeDefined()
    }
  })
})

describe("resolveSolType", () => {
  it("resolves scalar types by field type number", () => {
    expect(resolveSolType(8, undefined)).toBe("bool")
    expect(resolveSolType(9, undefined)).toBe("string")
    expect(resolveSolType(4, undefined)).toBe("uint64")
    expect(resolveSolType(12, undefined)).toBe("bytes")
  })

  it("resolves TYPE_MESSAGE (11) from typeName", () => {
    expect(resolveSolType(11, ".my_package.MyMessage")).toBe("MyMessage")
  })

  it("resolves TYPE_ENUM (14) from typeName", () => {
    expect(resolveSolType(14, ".example.nested.Role")).toBe("Role")
  })

  it("handles typeName without leading dot", () => {
    expect(resolveSolType(11, "my_package.Nested")).toBe("Nested")
  })

  it("falls back to PROTO_TYPE_MAP for message/enum without typeName", () => {
    // fieldType 14 without typeName → uses PROTO_TYPE_MAP[14].solType = "uint64"
    expect(resolveSolType(14, undefined)).toBe("uint64")
  })

  it("throws for unsupported field type", () => {
    expect(() => resolveSolType(99, undefined)).toThrow(
      "Unsupported protobuf field type: 99"
    )
  })
})

describe("fieldTag", () => {
  it("computes tag as (fieldNumber << 3 | wireType)", () => {
    // field 1, varint → (1 << 3) | 0 = 8
    expect(fieldTag(1, WireType.Varint)).toBe(8)
  })

  it("computes tag for LengthDelimited", () => {
    // field 2, length-delimited → (2 << 3) | 2 = 18
    expect(fieldTag(2, WireType.LengthDelimited)).toBe(18)
  })

  it("computes tag for Fixed32", () => {
    // field 3, fixed32 → (3 << 3) | 5 = 29
    expect(fieldTag(3, WireType.Fixed32)).toBe(29)
  })

  it("computes tag for Fixed64", () => {
    // field 1, fixed64 → (1 << 3) | 1 = 9
    expect(fieldTag(1, WireType.Fixed64)).toBe(9)
  })

  it("handles larger field numbers", () => {
    // field 15, varint → (15 << 3) | 0 = 120
    expect(fieldTag(15, WireType.Varint)).toBe(120)
  })
})
