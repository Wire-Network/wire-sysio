import type { Config } from "jest"

const config: Config = {
  displayName: "protoc-gen-solana",
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
    "^@wireio/protoc-gen-solana$": "<rootDir>/src/index",
    "^@wireio/protoc-gen-solana/(.*)$": "<rootDir>/src/$1"
  }
}

export default config
