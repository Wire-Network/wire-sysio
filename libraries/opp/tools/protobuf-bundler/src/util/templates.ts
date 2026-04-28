import Fs from "node:fs"
import Path from "node:path"
import { fileURLToPath } from "node:url"
import Handlebars from "handlebars"

// const __filename = fileURLToPath(import.meta.url)
// const __dirname = Path.dirname(__filename)

function resolveTemplatesDir(): string {
  const candidates = [
    Path.join(__dirname, "../../templates"),
    Path.join(__dirname, "../templates"),
    Path.join(__dirname, "templates")
  ]
  const dir = candidates.find(p => Fs.existsSync(p))
  if (!dir) {
    throw new Error(
      `Templates directory not found. Searched: ${candidates.join(", ")}`
    )
  }
  return dir
}

const TEMPLATES_DIR = resolveTemplatesDir()

Handlebars.registerHelper("json", function (context) {
  return new Handlebars.SafeString(JSON.stringify(context, null, 2))
})

export function renderTemplate(
  relativePath: string,
  context: Record<string, any>
): string {
  const fullPath = Path.join(TEMPLATES_DIR, relativePath)
  const source = Fs.readFileSync(fullPath, "utf-8")
  const template = Handlebars.compile(source)
  return template(context)
}

export function listTemplates(target: string): string[] {
  const dir = Path.join(TEMPLATES_DIR, target)
  return walkDir(dir)
    .filter(f => f.endsWith(".hbs"))
    .map(f => Path.relative(dir, f))
}

function walkDir(dir: string): string[] {
  const results: string[] = []
  if (!Fs.existsSync(dir)) return results
  for (const entry of Fs.readdirSync(dir, { withFileTypes: true })) {
    const fullPath = Path.join(dir, entry.name)
    if (entry.isDirectory()) {
      results.push(...walkDir(fullPath))
    } else {
      results.push(fullPath)
    }
  }
  return results
}
