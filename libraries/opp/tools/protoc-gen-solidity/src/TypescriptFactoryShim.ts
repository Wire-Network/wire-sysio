/**
 * Compatibility shim for @protobuf-ts/plugin with TypeScript 5.x
 *
 * TypeScript 5.x removed the deprecated top-level ts.create* factory functions
 * that @protobuf-ts/plugin@2.11.1 relies on (607 call sites). This shim restores
 * them as thin wrappers around the current ts.factory.create* API.
 *
 * Three categories of changes between TS 4.x and 5.x factory APIs:
 *   1. Same-name moves: ts.createFoo -> ts.factory.createFoo
 *   2. Renames: ts.createCall -> ts.factory.createCallExpression
 *   3. Signature changes: decorators param merged into modifiers
 */

import ts from "typescript"

const tsAny = ts as any
const factoryAny = ts.factory as any

// Only patch if the deprecated APIs are actually missing
if (ts.factory && typeof tsAny.createTypeReferenceNode !== "function") {

  // ── 1. Auto-map factory methods that kept the same name ─────────────────
  for (const key of Object.getOwnPropertyNames(ts.factory)) {
    if (
      key.startsWith("create") &&
      typeof factoryAny[key] === "function" &&
      typeof tsAny[key] !== "function"
    ) {
      tsAny[key] = factoryAny[key].bind(ts.factory)
    }
  }

  // ── 2. Renamed methods (old short name → new full name) ─────────────────
  const renames: Record<string, string> = {
    createArrayLiteral: "createArrayLiteralExpression",
    createBinary: "createBinaryExpression",
    createBreak: "createBreakStatement",
    createCall: "createCallExpression",
    createConditional: "createConditionalExpression",
    createElementAccess: "createElementAccessExpression",
    createFor: "createForStatement",
    createForOf: "createForOfStatement",
    createIf: "createIfStatement",
    createNew: "createNewExpression",
    createObjectLiteral: "createObjectLiteralExpression",
    createParen: "createParenthesizedExpression",
    createPostfix: "createPostfixUnaryExpression",
    createPropertyAccess: "createPropertyAccessExpression",
    createReturn: "createReturnStatement",
    createSwitch: "createSwitchStatement",
    createThrow: "createThrowStatement",
    createWhile: "createWhileStatement",
  }

  for (const [oldName, newName] of Object.entries(renames)) {
    if (typeof tsAny[oldName] !== "function" && typeof factoryAny[newName] === "function") {
      tsAny[oldName] = factoryAny[newName].bind(ts.factory)
    }
  }

  // ── 3. Signature changes: decorators param removed in TS 5.x ───────────
  //
  // In TS 4.x many declaration functions took (decorators?, modifiers?, ...rest).
  // In TS 5.x the signature is (modifiers?, ...rest) — decorators are merged
  // into the modifiers array. @protobuf-ts/plugin always passes `undefined`
  // for decorators, but we handle the general case for safety.

  function withMergedDecorators(factoryFn: (...args: any[]) => any) {
    return function (decorators: any, modifiers: any, ...rest: any[]) {
      const parts: any[] = []
      if (Array.isArray(decorators)) parts.push(...decorators)
      if (Array.isArray(modifiers)) parts.push(...modifiers)
      return factoryFn.call(ts.factory, parts.length > 0 ? parts : undefined, ...rest)
    }
  }

  // Same-name functions that lost the decorators param
  const sameNameDecoratorFns = [
    "createClassDeclaration",
    "createInterfaceDeclaration",
    "createEnumDeclaration",
    "createImportDeclaration",
    "createIndexSignature",
  ]
  for (const name of sameNameDecoratorFns) {
    if (typeof factoryAny[name] === "function") {
      tsAny[name] = withMergedDecorators(factoryAny[name])
    }
  }

  // Renamed functions that ALSO lost the decorators param
  const renamedDecoratorFns: Record<string, string> = {
    createMethod: "createMethodDeclaration",
    createProperty: "createPropertyDeclaration",
    createConstructor: "createConstructorDeclaration",
    createParameter: "createParameterDeclaration",
  }
  for (const [oldName, newName] of Object.entries(renamedDecoratorFns)) {
    if (typeof factoryAny[newName] === "function") {
      tsAny[oldName] = withMergedDecorators(factoryAny[newName])
    }
  }

  // ── 4. createPropertySignature: initializer param removed ───────────────
  // Old: (modifiers?, name, questionToken?, type?, initializer?)
  // New: (modifiers?, name, questionToken?, type?)
  if (typeof ts.factory.createPropertySignature === "function") {
    const orig = ts.factory.createPropertySignature.bind(ts.factory)
    tsAny.createPropertySignature = function (modifiers: any, name: any, questionToken: any, type: any, _initializer: any) {
      return orig(modifiers, name, questionToken, type)
    }
  }
}
