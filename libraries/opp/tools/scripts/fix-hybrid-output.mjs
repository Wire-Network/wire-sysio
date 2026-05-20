#!/usr/bin/env node
/**
 * Post-build fixup for hybrid ESM/CJS packages:
 * 1. Creates lib/cjs/package.json with {"type":"commonjs"} so Node.js treats .js as CJS
 * 2. Fixes extensionless relative imports in ESM output for Node.js compatibility
 *
 * Usage: node etc/scripts/fix-hybrid-output.mjs protobuf-bundler
 */
import {
  readdirSync,
  readFileSync,
  writeFileSync,
  existsSync,
  mkdirSync
} from "fs"
import { join, dirname } from "path"

const pkgDir = process.argv[2]
if (!pkgDir) {
  console.error("Usage: fix-hybrid-output.mjs <package-dir>")
  process.exit(1)
}

// 1. Create lib/cjs/package.json with {"type":"commonjs"}
const cjsDir = join(pkgDir, "lib", "cjs")
if (existsSync(cjsDir)) {
  writeFileSync(join(cjsDir, "package.json"), '{"type":"commonjs"}\n')
}

// 2. Create lib/esm/package.json with {"type":"module"}
const esmDir = join(pkgDir, "lib", "esm")
if (existsSync(esmDir)) {
  writeFileSync(join(esmDir, "package.json"), '{"type":"module"}\n')
}

// 3. Fix ESM imports — add .js extensions for Node.js native ESM resolution
if (existsSync(esmDir)) {
  fixImports(esmDir)
}

function fixImports(dir) {
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const fullPath = join(dir, entry.name)
    if (entry.isDirectory()) {
      fixImports(fullPath)
      continue
    }
    if (!entry.name.endsWith(".js") && !entry.name.endsWith(".d.ts")) continue
    // skip .d.ts.map (JSON sourcemaps)
    if (entry.name.endsWith(".d.ts.map")) continue

    let content = readFileSync(fullPath, "utf8")
    let changed = false

    content = content.replace(
      /((?:from|import)\s*\(?\s*["'])(\.\.?\/[^"']*)(["'])/g,
      (match, pre, spec, suf) => {
        // Already has an extension
        if (
          /\.\w+$/.test(spec) &&
          (spec.endsWith(".js") ||
            spec.endsWith(".mjs") ||
            spec.endsWith(".cjs") ||
            spec.endsWith(".json"))
        ) {
          return match
        }
        const base = dirname(fullPath)
        // Directory import → /index.js
        if (existsSync(join(base, spec, "index.js"))) {
          changed = true
          return `${pre}${spec}/index.js${suf}`
        }
        // File import → .js
        if (existsSync(join(base, spec + ".js"))) {
          changed = true
          return `${pre}${spec}.js${suf}`
        }
        return match
      }
    )

    if (changed) writeFileSync(fullPath, content)
  }
}

console.log(`Fixed hybrid output for ${pkgDir}`)
