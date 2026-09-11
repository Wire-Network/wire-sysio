import Solc from "solc"
import { generateRuntime } from "../src/generator/runtime"

/** Solidity warning emitted when a declaration shadows another declaration. */
const SHADOWING_WARNING_CODE = "2519"

describe("generated ProtobufRuntime", () => {
  it("uses the exact configured compiler version", () => {
    expect(Solc.version()).toMatch(/^0\.8\.25\+/)
    expect(generateRuntime()).toContain("pragma solidity 0.8.25;")
  })

  it("compiles without declaration-shadowing warnings", () => {
    const compilerInput = {
      language: "Solidity",
      sources: {
        "ProtobufRuntime.sol": { content: generateRuntime() }
      },
      settings: {
        outputSelection: { "*": { "*": ["abi"] } }
      }
    }
    const compilerOutput = JSON.parse(
      Solc.compile(JSON.stringify(compilerInput))
    )
    const diagnostics = compilerOutput.errors ?? []

    expect(
      diagnostics.filter(
        (error: { severity: string }) => error.severity === "error"
      )
    ).toEqual([])
    expect(
      diagnostics.filter(
        (error: { errorCode?: string }) =>
          error.errorCode === SHADOWING_WARNING_CODE
      )
    ).toEqual([])
  })
})
