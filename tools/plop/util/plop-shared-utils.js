const { asOption } = require("@3fv/prelude-ts")
const { isArray } = require("@3fv/guard")
const Fs = require("fs")
const Sh = require("shelljs")
const JSON5 = require("json5")
const Assert = require("node:assert");
const {targetTypeToPath, rootDir} = require("./plop-constants");
const Path = require("node:path");

module.exports.plopReadJsonFile = filename => {
  return JSON5.parse(Fs.readFileSync(filename, "utf8"))
}

module.exports.plopFileWriter =
  outputs => (filename, content) => {
    asOption(outputs)
      .orElse(() =>
        asOption(require("../PlopContext").outputs)
      )
      .filter(isArray)
    outputs.push({ filename, content })
  }

/**
 * @param {string} targetType
 * @param {string} name
 * @returns {string}
 */
function toTargetPath(targetType, name) {
    return Path.join(targetTypeToPath[targetType], name)
}

/**
 * List targets of a given type
 *
 * @param {string} targetType
 * @returns {name:string, path:string}[]
 */
function listTargets(targetType) {
    Assert.ok(Object.hasOwn(targetTypeToPath, targetType), `Unknown target type: ${targetType}`)
    const targetTypePath = Path.join(rootDir, targetTypeToPath[targetType])
    return Sh.ls("-d", targetTypePath)
        .filter(it => it.isDirectory())
        .map(it => ({name: it, path: Path.join(targetTypePath,it)}))
        .filter(({path}) => Path.existsSync(Path.join(path, "CMakeLists.txt")))
}

module.exports.toTargetPath = toTargetPath
module.exports.listTargets = listTargets
