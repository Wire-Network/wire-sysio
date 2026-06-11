import {
  genEnum,
  EnumDescriptor
} from "@wireio/protoc-gen-solana/generator/enum"

describe("genEnum", () => {
  const roleEnum: EnumDescriptor = {
    name: "Role",
    fullName: "example.Role",
    values: [
      { name: "ROLE_UNSPECIFIED", number: 0 },
      { name: "ROLE_USER", number: 1 },
      { name: "ROLE_ADMIN", number: 2 },
      { name: "ROLE_OPERATOR", number: 3 }
    ]
  }

  it("generates a Rust enum with #[repr(i32)]", () => {
    const output = genEnum(roleEnum)
    expect(output).toContain("#[repr(i32)]")
    expect(output).toContain("pub enum Role {")
  })

  it("converts SCREAMING_SNAKE variant names to PascalCase", () => {
    const output = genEnum(roleEnum)
    expect(output).toContain("RoleUnspecified = 0,")
    expect(output).toContain("RoleUser = 1,")
    expect(output).toContain("RoleAdmin = 2,")
    expect(output).toContain("RoleOperator = 3,")
  })

  it("generates standard derives (Clone, Copy, Debug, PartialEq, Eq, Hash)", () => {
    const output = genEnum(roleEnum)
    expect(output).toContain(
      "#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]"
    )
  })

  it("generates Default impl using the zero-value variant", () => {
    const output = genEnum(roleEnum)
    expect(output).toContain("impl Default for Role {")
    expect(output).toContain("Role::RoleUnspecified")
  })

  it("generates From<i32> impl with match arms for all values", () => {
    const output = genEnum(roleEnum)
    expect(output).toContain("impl From<i32> for Role {")
    expect(output).toContain("0 => Role::RoleUnspecified,")
    expect(output).toContain("1 => Role::RoleUser,")
    expect(output).toContain("_ => Role::default(),")
  })

  it("generates From<Enum> for i32 impl", () => {
    const output = genEnum(roleEnum)
    expect(output).toContain("impl From<Role> for i32 {")
    expect(output).toContain("value as i32")
  })

  it("defaults to the first variant when no zero-value variant exists", () => {
    const noZero: EnumDescriptor = {
      name: "Status",
      fullName: "example.Status",
      values: [
        { name: "STATUS_ACTIVE", number: 1 },
        { name: "STATUS_INACTIVE", number: 2 }
      ]
    }
    const output = genEnum(noZero)
    expect(output).toContain("Status::StatusActive")
  })

  it("handles a single-variant enum", () => {
    const single: EnumDescriptor = {
      name: "Singleton",
      fullName: "Singleton",
      values: [{ name: "SINGLETON_ONLY", number: 0 }]
    }
    const output = genEnum(single)
    expect(output).toContain("pub enum Singleton {")
    expect(output).toContain("SingletonOnly = 0,")
  })

  it("strips package prefix from fully-qualified name for the Rust enum name", () => {
    const nested: EnumDescriptor = {
      name: "ChainType",
      fullName: "sysio.opp.types.ChainType",
      values: [
        { name: "CHAIN_TYPE_UNKNOWN", number: 0 },
        { name: "CHAIN_TYPE_ETH", number: 1 }
      ]
    }
    const output = genEnum(nested)
    expect(output).toContain("pub enum ChainType {")
  })
})
