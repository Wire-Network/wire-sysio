import { FieldInfo, genFieldDecode } from "../src/generator/field"

const roleEnumInfo = {
  solTypeName: "Role",
  underlyingType: "uint8"
}

describe("genFieldDecode enum safety", () => {
  it("decodes scalar enums through the checked enum library helper", () => {
    const field: FieldInfo = {
      name: "role",
      number: 1,
      type: 14,
      typeName: ".example.Role",
      label: 1,
      enumInfo: roleEnumInfo
    }

    const result = genFieldDecode(field, "profile")

    expect(result.body).toContain("profile.role = RoleLib.fromRaw(_v);")
    expect(result.body).not.toContain("Role.wrap(uint8(_v))")
  })

  it("decodes repeated enums through the checked enum library helper", () => {
    const field: FieldInfo = {
      name: "roles",
      number: 1,
      type: 14,
      typeName: ".example.Role",
      label: 3,
      enumInfo: roleEnumInfo
    }

    const result = genFieldDecode(field, "profile")

    expect(result.body).toContain(
      "profile.roles[_idx_roles++] = RoleLib.fromRaw(_elem);"
    )
    expect(result.body).not.toContain("Role.wrap(uint8(_elem))")
  })

  it("decodes map enum values through the checked enum library helper", () => {
    const field: FieldInfo = {
      name: "role_by_name",
      number: 1,
      type: 11,
      typeName: ".example.Profile.RoleByNameEntry",
      label: 3,
      mapEntry: {
        keyType: 9,
        valueType: 14,
        valueTypeName: ".example.Role",
        valueEnumInfo: roleEnumInfo
      }
    }

    const result = genFieldDecode(field, "profile")

    expect(result.body).toContain("_val = RoleLib.fromRaw(_raw);")
    expect(result.body).not.toContain("Role.wrap(uint8(_raw))")
  })
})
