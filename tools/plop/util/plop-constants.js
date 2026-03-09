const Sh = require("shelljs")
const Assert = require("node:assert");
const Path = require("node:path");

const rootDir = Path.resolve(__dirname, "..", "..", "..")

/**
 * @typedef {"library"|"plugin"|"program"} TargetType
 * @typedef {{[TargetType as T], T}} TargetTypeEnum
 * @type {TargetTypeEnum}
 */
const TargetType = ["library","plugin","program"].reduce((it, typeName) => ({...it,[typeName]:typeName}), {})

/**
 * @type {Record<TargetType, string>}
 */
const targetTypeToPath = {
	"library": "libraries",
	"plugin": "plugins",
	"program": "programs"
}

for (const targetType of Object.keys(targetTypeToPath)) {
	const targetTypePath = Path.join(rootDir,targetTypeToPath[targetType])
	Assert.ok(Sh.test("-e",targetTypePath), `Invalid target type path: ${targetTypePath}`)
	targetTypeToPath[targetType] =  targetTypePath
}

const targetTypes = Object.entries(targetTypeToPath)

module.exports = {
	TargetType,
	targetTypeToPath,
	targetTypes,
	rootDir
}
