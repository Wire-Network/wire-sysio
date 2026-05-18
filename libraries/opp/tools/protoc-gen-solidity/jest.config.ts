import type { Config } from "jest"

const config: Config = {
  displayName: "protoc-gen-solidity",
  testEnvironment: "node",
  roots: ["<rootDir>/tests"],
  testMatch: ["**/*.test.ts"],
  transform: {
    "^.+\\.ts$": [
      "ts-jest",
      {
        tsconfig: "<rootDir>/tsconfig.cjs.jest.json"
      }
    ]
  },
  moduleNameMapper: {
    "^(\\.{1,2}/.*)\\.js$": "$1",
    "^@wireio/protoc-gen-solidity$": "<rootDir>/src/index",
    "^@wireio/protoc-gen-solidity/(.*)$": "<rootDir>/src/$1"
  }
}

export default config
