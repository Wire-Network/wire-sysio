import Fs from "node:fs"
import Path from "node:path"
import { match } from "ts-pattern"
import { asOption } from "@3fv/prelude-ts"
import { log } from "../util/logger.js"
import { renderTemplate } from "../util/templates.js"
import { deepMerge } from "../util/merge.js"
import {
  Target,
  TargetPackageName,
  TemplatePath,
  TemplateFile
} from "../constants.js"

/**
 * Read the bundler's own package.json to extract dependency versions.
 */
function readBundlerPackageJson(): Record<string, any> {
  return asOption(
    [
      Path.join(__dirname, "../../package.json"),
      Path.join(__dirname, "../../../package.json")
    ].find(candidate => Fs.existsSync(candidate))
  )
    .map(candidate => JSON.parse(Fs.readFileSync(candidate, "utf-8")))
    .getOrThrow("Could not find wire-protobuf-bundler package.json")
}

export interface GeneratePackageOptions {
  target: Target
  outputDir: string
  packageVersion: string
  generatedFiles: string[]
  genDir: string
  repo: string
}

export async function generatePackage(
  opts: GeneratePackageOptions
): Promise<void> {
  log.info("Generating %s package in %s", opts.target, opts.outputDir)

  await match(opts.target)
    .with(Target.Solana, () => generateSolanaPackage(opts))
    .with(Target.Solidity, () => generateSolidityPackage(opts))
    .with(Target.Typescript, () => generateTypescriptPackage(opts))
    .exhaustive()

  log.info("Package generation complete")
}

/**
 * Strip `.pb` from a filename.
 * e.g. "Types.pb.sol" → "Types.sol", "types.pb.rs" → "types.rs"
 */
function stripPb(filename: string): string {
  return filename.replace(/\.pb\.(\w+)$/, ".$1")
}

/**
 * Copy generated files to a destination, preserving directory structure
 * and stripping `.pb` from filenames.
 */
function copyGeneratedFiles(
  generatedFiles: string[],
  genDir: string,
  destDir: string,
  extension?: string
): void {
  Fs.mkdirSync(destDir, { recursive: true })
  generatedFiles
    .filter(file => !extension || file.endsWith(extension))
    .forEach(file => {
      const rel = Path.relative(genDir, file),
        cleaned = Path.join(Path.dirname(rel), stripPb(Path.basename(rel))),
        destPath = Path.join(destDir, cleaned)
      Fs.mkdirSync(Path.dirname(destPath), { recursive: true })
      Fs.copyFileSync(file, destPath)
    })
}

// ─── Solana (Rust crate) ────────────────────────────────────────────────────

async function generateSolanaPackage(
  opts: GeneratePackageOptions
): Promise<void> {
  const { outputDir, packageVersion, generatedFiles, genDir, repo } = opts,
    packageName = TargetPackageName[Target.Solana]

  const context: Record<string, any> = {
    packageName,
    version: packageVersion,
    repo,
    modules: [] as string[],
    dependencies: {}
  }

  const srcDir = Path.join(outputDir, "src")
  Fs.mkdirSync(srcDir, { recursive: true })

  // Copy generated .rs files preserving directory structure, stripping .pb
  const relPaths = generatedFiles
    .map(file => {
      const rel = Path.relative(genDir, file),
        cleaned = Path.join(Path.dirname(rel), stripPb(Path.basename(rel))),
        destPath = Path.join(srcDir, cleaned)
      Fs.mkdirSync(Path.dirname(destPath), { recursive: true })
      Fs.copyFileSync(file, destPath)
      return cleaned
    })
    .filter(cleaned => cleaned.endsWith(".rs"))

  // Build the Rust module tree: generate mod.rs files + lib.rs barrel
  const moduleTree = buildModuleTree(
    relPaths.filter(p => Path.basename(p) !== "protobuf_runtime.rs")
  )
  writeModFiles(srcDir, moduleTree)

  const topModules = Object.keys(moduleTree)
    .filter(k => k !== "_files")
    .sort()
  const reexports = collectLeafModulePaths(moduleTree, [])
  context.modules = topModules
  context.reexports = reexports

  const cargoToml = renderTemplate(
    TemplatePath[Target.Solana][TemplateFile.PackageJson].replace(
      TemplateFile.PackageJson,
      "Cargo.toml.hbs"
    ),
    context
  )
  Fs.writeFileSync(Path.join(outputDir, "Cargo.toml"), cargoToml)

  const libRs = renderTemplate("solana/src/lib.rs.hbs", context)
  Fs.writeFileSync(Path.join(srcDir, "lib.rs"), libRs)

  const readme = renderTemplate(
    TemplatePath[Target.Solana][TemplateFile.README],
    context
  )
  Fs.writeFileSync(Path.join(outputDir, "README.md"), readme)
}

/**
 * A node in the module tree. Keys are directory/module names.
 * Leaf files are stored under `_files`.
 */
interface ModuleNode {
  [key: string]: ModuleNode | string[]
  _files: string[]
}

function newModuleNode(): ModuleNode {
  return { _files: [] } as ModuleNode
}

function buildModuleTree(relPaths: string[]): ModuleNode {
  const root = newModuleNode()
  relPaths.forEach(rel => {
    const parts = rel.replace(/\.rs$/, "").split(Path.sep),
      filename = parts.pop()!
    let node = root
    parts.forEach(dir => {
      if (!node[dir] || typeof node[dir] === "string") {
        ;(node as any)[dir] = newModuleNode()
      }
      node = node[dir] as ModuleNode
    })
    node._files.push(filename)
  })
  return root
}

function writeModFiles(baseDir: string, tree: ModuleNode): void {
  Object.keys(tree)
    .filter(k => k !== "_files")
    .forEach(dir => {
      const subTree = tree[dir] as ModuleNode,
        subDir = Path.join(baseDir, dir)
      Fs.mkdirSync(subDir, { recursive: true })

      const childDirs = Object.keys(subTree).filter(k => k !== "_files"),
        childFiles = subTree._files || []

      const lines = [
        "// Auto-generated by protobuf-bundler — do not edit",
        "",
        ...childDirs.sort().map(d => `pub mod ${d};`),
        ...childFiles.sort().map(f => `pub mod ${f};`),
        ""
      ]

      Fs.writeFileSync(Path.join(subDir, "mod.rs"), lines.join("\n"))
      writeModFiles(subDir, subTree)
    })
}

function collectLeafModulePaths(tree: ModuleNode, prefix: string[]): string[] {
  const dirs = Object.keys(tree).filter(k => k !== "_files")
  return [
    ...(tree._files || []).map(f => [...prefix, f].join("::")),
    ...dirs.flatMap(d =>
      collectLeafModulePaths(tree[d] as ModuleNode, [...prefix, d])
    )
  ].sort()
}

// ─── Typescript (npm package) ───────────────────────────────────────────────

async function generateTypescriptPackage(
  opts: GeneratePackageOptions
): Promise<void> {
  const { outputDir, packageVersion, repo } = opts,
    packageName = TargetPackageName[Target.Typescript]

  const bundlerPkg = readBundlerPackageJson(),
    protobufTsRuntimeVersion =
      bundlerPkg.dependencies?.["@protobuf-ts/runtime"] ?? "^2.9.4"

  const context: Record<string, any> = {
    packageName,
    version: packageVersion,
    repo,
    protobufTsRuntimeVersion
  }

  const packageJson = renderTemplate(
    TemplatePath[Target.Typescript][TemplateFile.PackageJson],
    context
  )
  Fs.writeFileSync(Path.join(outputDir, "package.json"), packageJson)

  const readme = renderTemplate(
    TemplatePath[Target.Typescript][TemplateFile.README],
    context
  )
  Fs.writeFileSync(Path.join(outputDir, "README.md"), readme)
}

// ─── Solidity (npm package) ─────────────────────────────────────────────────

async function generateSolidityPackage(
  opts: GeneratePackageOptions
): Promise<void> {
  const { outputDir, packageVersion, generatedFiles, genDir, repo } = opts,
    packageName = TargetPackageName[Target.Solidity]

  const bundlerPkg = readBundlerPackageJson(),
    protobufTsRuntimeVersion =
      bundlerPkg.dependencies?.["@protobuf-ts/runtime"] ?? "^2.9.4"

  const context: Record<string, any> = {
    packageName,
    version: packageVersion,
    repo,
    protobufTsRuntimeVersion
  }

  // Copy generated .sol files
  const contractsDir = Path.join(outputDir, "contracts")
  copyGeneratedFiles(generatedFiles, genDir, contractsDir)

  // Render package.json and README
  const packageJson = renderTemplate(
    TemplatePath[Target.Solidity][TemplateFile.PackageJson],
    context
  )
  Fs.writeFileSync(Path.join(outputDir, "package.json"), packageJson)

  const readme = renderTemplate(
    TemplatePath[Target.Solidity][TemplateFile.README],
    context
  )
  Fs.writeFileSync(Path.join(outputDir, "README.md"), readme)
}
