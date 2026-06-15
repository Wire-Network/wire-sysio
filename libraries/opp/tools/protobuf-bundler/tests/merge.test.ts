import { deepMerge } from "@wireio/wire-protobuf-bundler/util/merge"

describe("deepMerge", () => {
  it("should return a new object (not mutate target)", () => {
    const target = { a: 1 }
    const result = deepMerge(target, { a: 2 })
    expect(result).not.toBe(target)
    expect(target.a).toBe(1)
  })

  it("should shallow merge flat objects", () => {
    const result = deepMerge({ a: 1, b: 2 }, { b: 3, c: 4 } as any)
    expect(result).toEqual({ a: 1, b: 3, c: 4 })
  })

  it("should deep merge nested objects", () => {
    const target = { nested: { a: 1, b: 2 } }
    const source = { nested: { b: 99 } } as any
    const result = deepMerge(target, source)
    expect(result).toEqual({ nested: { a: 1, b: 99 } })
  })

  it("should deep merge multiple levels", () => {
    const target = { l1: { l2: { l3: "original", keep: true } } }
    const source = { l1: { l2: { l3: "replaced" } } }
    const result = deepMerge(target, source as any)
    expect(result).toEqual({ l1: { l2: { l3: "replaced", keep: true } } })
  })

  it("should replace arrays instead of merging them", () => {
    const target = { arr: [1, 2, 3] }
    const source = { arr: [4, 5] }
    const result = deepMerge(target, source)
    expect(result.arr).toEqual([4, 5])
  })

  it("should skip undefined source values", () => {
    const target = { a: 1, b: 2 }
    const source = { a: undefined, b: 3 }
    const result = deepMerge(target, source)
    expect(result).toEqual({ a: 1, b: 3 })
  })

  it("should overwrite target with null source values", () => {
    const target = { a: 1, b: "hello" }
    const source = { a: null } as any
    const result = deepMerge(target, source)
    expect(result.a).toBeNull()
    expect(result.b).toBe("hello")
  })

  it("should handle empty source", () => {
    const target = { a: 1, b: 2 }
    const result = deepMerge(target, {})
    expect(result).toEqual({ a: 1, b: 2 })
  })

  it("should handle empty target", () => {
    const result = deepMerge({} as any, { a: 1 })
    expect(result).toEqual({ a: 1 })
  })

  it("should replace nested object with array from source", () => {
    const target = { data: { nested: true } } as any
    const source = { data: [1, 2, 3] } as any
    const result = deepMerge(target, source)
    expect(result.data).toEqual([1, 2, 3])
  })

  it("should replace array with nested object from source", () => {
    const target = { data: [1, 2] } as any
    const source = { data: { nested: true } } as any
    const result = deepMerge(target, source)
    expect(result.data).toEqual({ nested: true })
  })

  it("should handle source overwriting primitive with object", () => {
    const target = { a: 42 } as any
    const source = { a: { nested: true } } as any
    const result = deepMerge(target, source)
    expect(result.a).toEqual({ nested: true })
  })

  it("should handle source overwriting object with primitive", () => {
    const target = { a: { nested: true } } as any
    const source = { a: 42 } as any
    const result = deepMerge(target, source)
    expect(result.a).toBe(42)
  })

  it("should not deep merge when target value is null", () => {
    const target = { a: null } as any
    const source = { a: { nested: true } } as any
    const result = deepMerge(target, source)
    expect(result.a).toEqual({ nested: true })
  })

  it("should not deep merge when source value is null", () => {
    const target = { a: { nested: true } } as any
    const source = { a: null } as any
    const result = deepMerge(target, source)
    expect(result.a).toBeNull()
  })
})
