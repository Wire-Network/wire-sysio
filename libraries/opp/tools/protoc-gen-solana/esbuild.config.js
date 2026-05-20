const esbuild = require('esbuild')
const { chmodSync } = require('fs')

const shouldWatch = process.argv.includes('--watch') || process.argv.includes('-w') ||
  process.env.WATCH === "1"

const chmodPlugin = {
  name: 'chmod',
  setup(build) {
    build.onEnd(result => {
      if (result.errors.length > 0) return
      const outfile = build.initialOptions.outfile
      try {
        chmodSync(outfile, 0o755)
      } catch (err) {
        console.error(`chmod failed for ${outfile}:`, err.message)
      }
    })
  }
}

async function main() {
  const ctx = await esbuild.context({
    entryPoints: ["src/index.ts"],
    bundle: true,
    platform: "node",
    target: "node24",
    format: "cjs",
    outfile: "dist/bundle/protoc-gen-solana.cjs",
    sourcemap: true,
    minify: false,
    banner: {
      js: "#!/usr/bin/env node\nvar import_meta_url = require('url').pathToFileURL(__filename).href;"
    },
    define: {
      "import.meta.url": "import_meta_url",
    },
    external: [],
    logLevel: "info",
    plugins: [chmodPlugin]
  })

  if (shouldWatch) {
    await ctx.watch()
  } else {
    await ctx.rebuild()
    await ctx.dispose()
  }
}

main()
