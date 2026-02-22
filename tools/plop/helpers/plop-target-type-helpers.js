// @ts-check
/** @typedef {import('plop').NodePlopAPI} NodePlopAPI */

const { assert } = require("@3fv/guard")
const Sh = require("shelljs")
const { targetTypeToPath, rootDir} = require("../util/plop-constants")
const Path = require("node:path");


/**
 * @param {NodePlopAPI} plop
 */
function registerTargetTypeHelpers(plop) {
	plop.setHelper("targetTypeToPath", (targetType) => {
		const path = Path.relative(rootDir,targetTypeToPath[targetType])
		assert(Sh.test("-d", path), `Unable to find valid target (${targetType}) mapped to ${path}`)
		return path
	})

	plop.setHelper("namespaceToPath", (ns) => {
		return ns.split("::").join("/")
	})

	plop.setHelper("getNamespace0", (ns) => {
		return ns.split("::")[0]
	})

}

module.exports = registerTargetTypeHelpers
