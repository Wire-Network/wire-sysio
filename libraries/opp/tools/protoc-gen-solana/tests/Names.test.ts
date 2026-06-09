import {
  protoNameToRust,
  toSnakeCase,
  protoFileToRsFile,
  screamingSnakeToPascalCase
} from "@wireio/protoc-gen-solana/util/names"

describe("protoNameToRust", () => {
  it("extracts last segment from fully-qualified name", () => {
    expect(protoNameToRust("my_package.MyMessage")).toBe("MyMessage")
  })

  it("handles deeply nested packages", () => {
    expect(protoNameToRust("com.example.v1.MyService")).toBe("MyService")
  })

  it("returns the name as-is when there is no package prefix", () => {
    expect(protoNameToRust("SimpleMessage")).toBe("SimpleMessage")
  })

  it("handles empty segments correctly", () => {
    expect(protoNameToRust(".leading.dot.Name")).toBe("Name")
  })
})

describe("toSnakeCase", () => {
  it("converts camelCase to snake_case", () => {
    expect(toSnakeCase("userName")).toBe("user_name")
  })

  it("converts PascalCase to snake_case", () => {
    expect(toSnakeCase("UserName")).toBe("user_name")
  })

  it("leaves already snake_case unchanged", () => {
    expect(toSnakeCase("user_name")).toBe("user_name")
  })

  it("handles single word", () => {
    expect(toSnakeCase("name")).toBe("name")
  })

  it("handles consecutive uppercase as a single block", () => {
    expect(toSnakeCase("myHTTPResponse")).toBe("my_httpresponse")
  })

  it("lowercases a single uppercase word", () => {
    expect(toSnakeCase("Name")).toBe("name")
  })
})

describe("screamingSnakeToPascalCase", () => {
  it("converts SCREAMING_SNAKE_CASE to PascalCase", () => {
    expect(screamingSnakeToPascalCase("ROLE_UNSPECIFIED")).toBe(
      "RoleUnspecified"
    )
  })

  it("converts single-word screaming snake", () => {
    expect(screamingSnakeToPascalCase("ADMIN")).toBe("Admin")
  })

  it("converts multi-segment names", () => {
    expect(screamingSnakeToPascalCase("CHAIN_TYPE_ETH")).toBe("ChainTypeEth")
  })

  it("handles leading/trailing underscores", () => {
    expect(screamingSnakeToPascalCase("_FOO_BAR_")).toBe("FooBar")
  })

  it("handles empty string", () => {
    expect(screamingSnakeToPascalCase("")).toBe("")
  })
})

describe("protoFileToRsFile", () => {
  it("replaces .proto with .rs", () => {
    expect(protoFileToRsFile("my_service.proto")).toBe("my_service.rs")
  })

  it("prepends package-derived directory path", () => {
    expect(protoFileToRsFile("my_service.proto", "example.nested")).toBe(
      "example/nested/my_service.rs"
    )
  })

  it("handles no package name", () => {
    expect(protoFileToRsFile("messages.proto")).toBe("messages.rs")
  })

  it("converts CamelCase filename to snake_case", () => {
    expect(protoFileToRsFile("MyService.proto")).toBe("my_service.rs")
  })

  it("handles proto file with directory prefix", () => {
    expect(protoFileToRsFile("protos/MyService.proto")).toBe("my_service.rs")
  })

  it("handles single-segment package name", () => {
    expect(protoFileToRsFile("types.proto", "mypackage")).toBe(
      "mypackage/types.rs"
    )
  })
})
