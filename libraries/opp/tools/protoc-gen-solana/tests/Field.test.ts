import {
  FieldInfo,
  isRepeated,
  isMessage,
  isEnum,
  genStructMember
} from "@wireio/protoc-gen-solana/generator/field"

describe("isRepeated", () => {
  it("returns true when label is 3 (repeated)", () => {
    const field: FieldInfo = { name: "items", number: 1, type: 5, label: 3 }
    expect(isRepeated(field)).toBe(true)
  })

  it("returns false when label is 1 (optional)", () => {
    const field: FieldInfo = { name: "name", number: 1, type: 9, label: 1 }
    expect(isRepeated(field)).toBe(false)
  })

  it("returns false when label is 2 (required)", () => {
    const field: FieldInfo = { name: "id", number: 1, type: 5, label: 2 }
    expect(isRepeated(field)).toBe(false)
  })
})

describe("isMessage", () => {
  it("returns true when type is 11 (message)", () => {
    const field: FieldInfo = {
      name: "nested",
      number: 1,
      type: 11,
      typeName: ".pkg.Nested",
      label: 1
    }
    expect(isMessage(field)).toBe(true)
  })

  it("returns false for scalar types", () => {
    const field: FieldInfo = { name: "count", number: 1, type: 5, label: 1 }
    expect(isMessage(field)).toBe(false)
  })

  it("returns false for string type", () => {
    const field: FieldInfo = { name: "name", number: 1, type: 9, label: 1 }
    expect(isMessage(field)).toBe(false)
  })
})

describe("isEnum", () => {
  it("returns true when type is 14 (enum)", () => {
    const field: FieldInfo = {
      name: "role",
      number: 1,
      type: 14,
      typeName: ".example.Role",
      label: 1
    }
    expect(isEnum(field)).toBe(true)
  })

  it("returns false for scalar types", () => {
    const field: FieldInfo = { name: "count", number: 1, type: 5, label: 1 }
    expect(isEnum(field)).toBe(false)
  })

  it("returns false for message types", () => {
    const field: FieldInfo = {
      name: "nested",
      number: 1,
      type: 11,
      typeName: ".pkg.Nested",
      label: 1
    }
    expect(isEnum(field)).toBe(false)
  })
})

describe("genStructMember", () => {
  it("generates a scalar field declaration", () => {
    const field: FieldInfo = { name: "user_id", number: 1, type: 4, label: 1 }
    expect(genStructMember(field)).toBe("    pub user_id: u64,")
  })

  it("generates a string field declaration", () => {
    const field: FieldInfo = { name: "name", number: 2, type: 9, label: 1 }
    expect(genStructMember(field)).toBe("    pub name: String,")
  })

  it("generates a bytes field declaration", () => {
    const field: FieldInfo = { name: "data", number: 3, type: 12, label: 1 }
    expect(genStructMember(field)).toBe("    pub data: Vec<u8>,")
  })

  it("generates a repeated scalar field as Vec", () => {
    const field: FieldInfo = { name: "scores", number: 4, type: 5, label: 3 }
    expect(genStructMember(field)).toBe("    pub scores: Vec<i32>,")
  })

  it("generates a repeated message field as Vec", () => {
    const field: FieldInfo = {
      name: "items",
      number: 5,
      type: 11,
      typeName: ".pkg.Item",
      label: 3
    }
    expect(genStructMember(field)).toBe("    pub items: Vec<Item>,")
  })

  it("generates a message field with resolved type name", () => {
    const field: FieldInfo = {
      name: "metadata",
      number: 6,
      type: 11,
      typeName: ".example.v1.Metadata",
      label: 1
    }
    expect(genStructMember(field)).toBe("    pub metadata: Metadata,")
  })

  it("converts camelCase field names to snake_case", () => {
    const field: FieldInfo = { name: "userName", number: 1, type: 9, label: 1 }
    expect(genStructMember(field)).toBe("    pub user_name: String,")
  })

  it("generates map fields as parallel key/value Vecs", () => {
    const field: FieldInfo = {
      name: "attributes",
      number: 7,
      type: 11,
      typeName: ".pkg.AttributesEntry",
      label: 3,
      mapEntry: { keyType: 9, valueType: 9 }
    }
    const result = genStructMember(field)
    expect(result).toBe(
      "    pub attributes_keys: Vec<String>,\n    pub attributes_values: Vec<String>,"
    )
  })

  it("generates map fields with message values", () => {
    const field: FieldInfo = {
      name: "entries",
      number: 8,
      type: 11,
      typeName: ".pkg.EntriesEntry",
      label: 3,
      mapEntry: { keyType: 5, valueType: 11, valueTypeName: ".pkg.Entry" }
    }
    const result = genStructMember(field)
    expect(result).toBe(
      "    pub entries_keys: Vec<i32>,\n    pub entries_values: Vec<Entry>,"
    )
  })

  it("generates bool field declaration", () => {
    const field: FieldInfo = { name: "is_active", number: 9, type: 8, label: 1 }
    expect(genStructMember(field)).toBe("    pub is_active: bool,")
  })

  it("generates an enum field with the enum type name", () => {
    const field: FieldInfo = {
      name: "role",
      number: 5,
      type: 14,
      typeName: ".example.Role",
      label: 1
    }
    expect(genStructMember(field)).toBe("    pub role: Role,")
  })

  it("generates a repeated enum field as Vec", () => {
    const field: FieldInfo = {
      name: "roles",
      number: 6,
      type: 14,
      typeName: ".example.Role",
      label: 3
    }
    expect(genStructMember(field)).toBe("    pub roles: Vec<Role>,")
  })
})
