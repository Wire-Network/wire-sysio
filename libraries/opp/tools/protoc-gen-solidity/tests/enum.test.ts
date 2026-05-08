import {
  computeUnderlyingType,
  enumLibName,
  genEnumDefinition,
  EnumValueInfo,
  EnumDescriptor,
} from "../src/generator/enum"

describe("computeUnderlyingType", () => {
  it("returns uint8 for empty values array", () => {
    expect(computeUnderlyingType([])).toBe("uint8")
  })

  it("returns uint8 when max value fits in 1 byte", () => {
    const values: EnumValueInfo[] = [
      { name: "A", number: 0 },
      { name: "B", number: 1 },
      { name: "C", number: 255 },
    ]
    expect(computeUnderlyingType(values)).toBe("uint8")
  })

  it("returns uint16 when max value exceeds uint8", () => {
    const values: EnumValueInfo[] = [
      { name: "A", number: 0 },
      { name: "B", number: 256 },
    ]
    expect(computeUnderlyingType(values)).toBe("uint16")
  })

  it("returns uint16 for max value at uint16 boundary", () => {
    const values: EnumValueInfo[] = [{ name: "A", number: 0xffff }]
    expect(computeUnderlyingType(values)).toBe("uint16")
  })

  it("returns uint24 when max value exceeds uint16", () => {
    const values: EnumValueInfo[] = [{ name: "A", number: 0x10000 }]
    expect(computeUnderlyingType(values)).toBe("uint24")
  })

  it("returns uint24 for max value at uint24 boundary", () => {
    const values: EnumValueInfo[] = [{ name: "A", number: 0xffffff }]
    expect(computeUnderlyingType(values)).toBe("uint24")
  })

  it("returns uint32 when max value exceeds uint24", () => {
    const values: EnumValueInfo[] = [{ name: "A", number: 0x1000000 }]
    expect(computeUnderlyingType(values)).toBe("uint32")
  })

  it("returns uint32 for max value at uint32 boundary", () => {
    const values: EnumValueInfo[] = [{ name: "A", number: 0xffffffff }]
    expect(computeUnderlyingType(values)).toBe("uint32")
  })

  it("returns uint64 when max value exceeds uint32", () => {
    const values: EnumValueInfo[] = [{ name: "A", number: 0x100000000 }]
    expect(computeUnderlyingType(values)).toBe("uint64")
  })
})

describe("enumLibName", () => {
  it("appends Lib to enum name", () => {
    expect(enumLibName("Role")).toBe("RoleLib")
  })

  it("works with longer names", () => {
    expect(enumLibName("TransactionStatus")).toBe("TransactionStatusLib")
  })
})

describe("genEnumDefinition", () => {
  it("generates UDVT, using statement, and library with constants", () => {
    const desc: EnumDescriptor = {
      name: "Role",
      fullName: "example.Role",
      values: [
        { name: "UNSPECIFIED", number: 0 },
        { name: "ADMIN", number: 1 },
        { name: "USER", number: 2 },
      ],
      underlyingType: "uint8",
    }

    const result = genEnumDefinition(desc)

    // UDVT definition
    expect(result).toContain("type Role is uint8;")

    // Using statement
    expect(result).toContain("using {RoleLib.isValid} for Role global;")

    // Library header
    expect(result).toContain("library RoleLib {")

    // Constants
    expect(result).toContain("Role constant UNSPECIFIED = Role.wrap(0);")
    expect(result).toContain("Role constant ADMIN = Role.wrap(1);")
    expect(result).toContain("Role constant USER = Role.wrap(2);")

    // isValid checks against max value
    expect(result).toContain("function isValid(Role _v) internal pure returns (bool)")
    expect(result).toContain("return Role.unwrap(_v) <= Role.unwrap(USER);")
  })

  it("uses protoNameToSol to extract name from fullName", () => {
    const desc: EnumDescriptor = {
      name: "Status",
      fullName: "deep.nested.package.Status",
      values: [{ name: "OK", number: 0 }],
      underlyingType: "uint8",
    }

    const result = genEnumDefinition(desc)
    expect(result).toContain("type Status is uint8;")
    expect(result).toContain("library StatusLib {")
  })

  it("handles enum with no values (no isValid function body)", () => {
    const desc: EnumDescriptor = {
      name: "Empty",
      fullName: "Empty",
      values: [],
      underlyingType: "uint8",
    }

    const result = genEnumDefinition(desc)
    expect(result).toContain("type Empty is uint8;")
    expect(result).toContain("library EmptyLib {")
    // The using statement is always emitted, but the function body is not
    expect(result).not.toContain("function isValid")
  })

  it("picks the correct max value for isValid", () => {
    const desc: EnumDescriptor = {
      name: "Priority",
      fullName: "Priority",
      values: [
        { name: "LOW", number: 0 },
        { name: "HIGH", number: 100 },
        { name: "MEDIUM", number: 50 },
      ],
      underlyingType: "uint8",
    }

    const result = genEnumDefinition(desc)
    // HIGH has the largest number (100), so isValid checks against HIGH
    expect(result).toContain("return Priority.unwrap(_v) <= Priority.unwrap(HIGH);")
  })

  it("uses the descriptor underlyingType in the UDVT", () => {
    const desc: EnumDescriptor = {
      name: "Big",
      fullName: "Big",
      values: [{ name: "VAL", number: 0x10000 }],
      underlyingType: "uint24",
    }

    const result = genEnumDefinition(desc)
    expect(result).toContain("type Big is uint24;")
  })
})
