/** Code generation target. */
export enum Target {
  Solana = "solana",
  Solidity = "solidity",
  Typescript = "typescript"
}

export const ALL_TARGETS = Object.values(Target)

/** Targets that are published to npm (typescript/solidity). */
export const PUBLISHABLE_TARGETS: Target[] = [
  Target.Typescript,
  Target.Solidity
]

/** Targets that are published to the WIRE CodeArtifact cargo registry (solana). */
export const CARGO_PUBLISHABLE_TARGETS: Target[] = [Target.Solana]

/**
 * Release channel for published bundle versions, matching the wire-sysio
 * release channels: `dev` appends the prerelease suffix to the resolved
 * version (e.g. `1.0.41-dev`); `stable` publishes the bare version
 * (identical to the historical no-channel behavior). The version RESOLUTION
 * (latest published → patch bump) is the same for every channel.
 */
export enum Channel {
  dev = "dev",
  stable = "stable"
}

/**
 * Prerelease suffix appended to the resolved version on the dev channel.
 * The suffix spelling is the channel name itself.
 */
export const DEV_PRERELEASE_SUFFIX = Channel.dev

/**
 * Cargo registry name for the WIRE CodeArtifact repository. The publish
 * environment supplies `CARGO_REGISTRIES_WIRE_INDEX` / `_TOKEN`
 * (see libraries/opp/tools/scripts/generate-opp-bundles.sh).
 */
export const CARGO_REGISTRY_NAME = "wire"

/** SPDX license expression stamped into the generated Rust crate. */
export const CRATE_LICENSE = "LicenseRef-Wire-Proprietary"

/** Canonical repository URL stamped into the generated Rust crate. */
export const CRATE_REPOSITORY_URL = "https://github.com/Wire-Network/wire-sysio"

/**
 * Canonical build order — typescript before solidity (dependency).
 * Solana is independent so its position doesn't matter.
 */
export const TARGET_BUILD_ORDER: Target[] = [
  Target.Solana,
  Target.Typescript,
  Target.Solidity
]

/**
 * Pre-computed package name for every target.
 * Publishable targets (ts/solidity) → `@wireio/opp-<target>-models`
 * Others (solana) → `wire-opp-<target>-models`
 */
export const TargetPackageName: Record<Target, string> = Object.values(
  Target
).reduce(
  (map, target) => ({
    ...map,
    [target]: PUBLISHABLE_TARGETS.includes(target)
      ? `@wireio/opp-${target}-models`
      : `wire-opp-${target}-models`
  }),
  {} as Record<Target, string>
)

/**
 * Known template file names — no raw string literals in rendering calls.
 */
export enum TemplateFile {
  PackageJson = "package.json.hbs",
  README = "README.md.hbs",
  TSConfig = "tsconfig.json.hbs",
  TSConfigCJS = "tsconfig.cjs.json.hbs",
  TSConfigESM = "tsconfig.esm.json.hbs"
}

/**
 * Pre-computed template path for every (target, file) pair.
 * `TemplatePath[Target.Solidity][TemplateFile.PackageJson]` → `"solidity/package.json.hbs"`
 */
export const TemplatePath: Record<
  Target,
  Record<TemplateFile, string>
> = Object.values(Target).reduce(
  (outer, target) => ({
    ...outer,
    [target]: Object.values(TemplateFile).reduce(
      (inner, file) => ({
        ...inner,
        [file]: `${target}/${file}`
      }),
      {} as Record<TemplateFile, string>
    )
  }),
  {} as Record<Target, Record<TemplateFile, string>>
)
