import type { Config } from "jest"

const config: Config = {
  displayName: "protobuf-bundler",
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
    "^@wireio/wire-protobuf-bundler$": "<rootDir>/src/index",
    "^@wireio/wire-protobuf-bundler/(.*)$": "<rootDir>/src/$1",
    "^(\\.{1,2}/.*)\\.js$": "$1"
  }
}

export default config
