import {
  protoNameToSol,
  snakeToCamel,
  sanitizeFieldName,
  toSolFieldName,
  structNameToVarName,
  codecLibName,
  protoFileToSolFile,
  runtimeImportPath,
  relativeImportPath,
  SOL_PRAGMA,
  SPDX_LICENSE,
} from "../src/util/names"

describe("protoNameToSol", () => {
  it("extracts the last segment of a dotted FQN", () => {
    expect(protoNameToSol("my_package.MyMessage")).toBe("MyMessage")
  })

  it("handles deeply nested packages", () => {
    expect(protoNameToSol("a.b.c.d.Foo")).toBe("Foo")
  })

  it("returns the name itself when there is no package", () => {
    expect(protoNameToSol("SimpleMessage")).toBe("SimpleMessage")
  })
})

describe("snakeToCamel", () => {
  it("converts snake_case to camelCase", () => {
    expect(snakeToCamel("user_name")).toBe("userName")
  })

  it("handles multiple underscores", () => {
    expect(snakeToCamel("my_long_field_name")).toBe("myLongFieldName")
  })

  it("leaves already camelCase names untouched", () => {
    expect(snakeToCamel("alreadyCamel")).toBe("alreadyCamel")
  })

  it("does not convert uppercase after underscore if already uppercase", () => {
    // regex only matches _[a-z], so _A stays as-is
    expect(snakeToCamel("field_A")).toBe("field_A")
  })

  it("handles single-character name", () => {
    expect(snakeToCamel("x")).toBe("x")
  })
})

describe("sanitizeFieldName", () => {
  it("appends underscore to reserved words", () => {
    expect(sanitizeFieldName("type")).toBe("type_")
    expect(sanitizeFieldName("address")).toBe("address_")
    expect(sanitizeFieldName("mapping")).toBe("mapping_")
    expect(sanitizeFieldName("contract")).toBe("contract_")
  })

  it("does not modify non-reserved words", () => {
    expect(sanitizeFieldName("amount")).toBe("amount")
    expect(sanitizeFieldName("sender")).toBe("sender")
  })

  it("handles sized integer types as reserved", () => {
    expect(sanitizeFieldName("uint256")).toBe("uint256_")
    expect(sanitizeFieldName("int8")).toBe("int8_")
  })

  it("handles unit denominations as reserved", () => {
    expect(sanitizeFieldName("wei")).toBe("wei_")
    expect(sanitizeFieldName("ether")).toBe("ether_")
  })
})

describe("toSolFieldName", () => {
  it("converts snake_case and sanitizes", () => {
    expect(toSolFieldName("user_name")).toBe("userName")
  })

  it("sanitizes reserved words after camel conversion", () => {
    // "type" is reserved
    expect(toSolFieldName("type")).toBe("type_")
  })

  it("handles snake_case that becomes a reserved word", () => {
    // "my_type" → "myType" which is not reserved
    expect(toSolFieldName("my_type")).toBe("myType")
  })

  it("handles plain non-reserved field", () => {
    expect(toSolFieldName("amount")).toBe("amount")
  })
})

describe("structNameToVarName", () => {
  it("lowercases the first character", () => {
    expect(structNameToVarName("ChainId")).toBe("chainId")
  })

  it("handles single-char names", () => {
    expect(structNameToVarName("X")).toBe("x")
  })

  it("preserves rest of the name", () => {
    expect(structNameToVarName("MessageHeader")).toBe("messageHeader")
  })
})

describe("codecLibName", () => {
  it("appends 'Codec' to message name", () => {
    expect(codecLibName("MyMessage")).toBe("MyMessageCodec")
  })

  it("works with single-word names", () => {
    expect(codecLibName("Token")).toBe("TokenCodec")
  })
})

describe("protoFileToSolFile", () => {
  it("converts proto filename to PascalCase .sol", () => {
    expect(protoFileToSolFile("my_service.proto")).toBe("MyService.sol")
  })

  it("handles dashes in filename", () => {
    expect(protoFileToSolFile("my-service.proto")).toBe("MyService.sol")
  })

  it("strips directory prefix from proto path", () => {
    expect(protoFileToSolFile("path/to/my_service.proto")).toBe("MyService.sol")
  })

  it("prepends package directory when packageName is provided", () => {
    expect(protoFileToSolFile("my_service.proto", "example.nested")).toBe(
      "example/nested/MyService.sol"
    )
  })

  it("handles single-segment package", () => {
    expect(protoFileToSolFile("test.proto", "sysio")).toBe("sysio/Test.sol")
  })

  it("returns just the sol file when no package", () => {
    expect(protoFileToSolFile("simple.proto")).toBe("Simple.sol")
  })
})

describe("runtimeImportPath", () => {
  it("returns relative current-dir path for root-level file", () => {
    expect(runtimeImportPath("Example.sol")).toBe("./ProtobufRuntime.sol")
  })

  it("returns one level up for single directory depth", () => {
    expect(runtimeImportPath("example/Example.sol")).toBe("../ProtobufRuntime.sol")
  })

  it("returns correct depth for deeply nested files", () => {
    expect(runtimeImportPath("example/nested/test/Example.sol")).toBe(
      "../../../ProtobufRuntime.sol"
    )
  })
})

describe("relativeImportPath", () => {
  it("computes path to a deeper file", () => {
    expect(relativeImportPath("sysio/opp/Opp.sol", "sysio/opp/types/Types.sol")).toBe(
      "./types/Types.sol"
    )
  })

  it("computes path to a shallower file", () => {
    expect(
      relativeImportPath("sysio/opp/attestations/Attestations.sol", "sysio/opp/Opp.sol")
    ).toBe("../Opp.sol")
  })

  it("computes path between sibling files", () => {
    expect(relativeImportPath("pkg/A.sol", "pkg/B.sol")).toBe("./B.sol")
  })

  it("computes path across different subtrees", () => {
    expect(relativeImportPath("a/b/X.sol", "a/c/Y.sol")).toBe("../c/Y.sol")
  })

  it("handles root-level files", () => {
    expect(relativeImportPath("A.sol", "B.sol")).toBe("./B.sol")
  })
})

describe("constants", () => {
  it("SOL_PRAGMA is correct", () => {
    expect(SOL_PRAGMA).toBe(">=0.8.0 <0.9.0")
  })

  it("SPDX_LICENSE is MIT", () => {
    expect(SPDX_LICENSE).toBe("MIT")
  })
})
