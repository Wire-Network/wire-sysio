/** @typedef {import('plop').NodePlopAPI} NodePlopAPI */
const PlopContext = require("../PlopContext")

/**
 *
 * @param plop {NodePlopAPI}
 */
function registerNamingHelpers(plop) {
    plop.setHelper("getSuffix", (skipSuffix, suffix) => {
        return ([true, "true"].includes(skipSuffix)) ? "" : suffix
    })
}

module.exports = registerNamingHelpers
