const Assert = require("node:assert");
const Path = require("node:path")
const Fs = require("node:fs")
const Constants = require("./util/plop-constants")

const {rootDir, targetTypes} = Constants
const rootPkgFile = Path.join(rootDir, "package.json")

Assert.ok(Fs.existsSync(rootPkgFile), `Unable to find package.json at ${rootPkgFile}`)
const rootPkg = JSON.parse(Fs.readFileSync(rootPkgFile, "utf8"))


const PlopContext = {
    rootDir,
    rootPkgFile,
    rootPkg,
    version: rootPkg.version,
    outputs: Array()
}


module.exports = PlopContext
