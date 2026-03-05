/** @typedef {import('plop').NodePlopAPI} NodePlopAPI */
const PlopContext = require("../PlopContext")

/**
 *
 * @param plop: NodePlopAPI
 */
function registerStringHelpers(plop) {
	plop.setHelper("doubleBracketLeft", () => {
    return  "{{"
	})

	plop.setHelper("doubleBracketRight", () => {
		return  "}}"
	})

	plop.setHelper("lowerFirst", (str) => {
		return  !str || !str.length ? str : (str[0] + str.slice(1))
	})

}

module.exports = registerStringHelpers
