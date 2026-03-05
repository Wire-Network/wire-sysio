const Fs = require("fs")
const {exec, test, rm} = require("shelljs")
const {ok} = require("assert")
const Path = require("path")
const assert = require("assert")
const _ = require("lodash")
const PlopContext = require("../PlopContext")
const {plopFileWriter, plopReadJsonFile} = require("../util/plop-shared-utils")
// const {rootDir} = PlopContext

module.exports = function register(plop) {
    const {
            outputs
        } = PlopContext,
        writeFile = plopFileWriter(outputs)

    plop.setActionType("print-files", (answers, config, plopfileApi) => {
        return "success"
    })

    plop.setActionType("flush-files", (answers, config, plopfileApi) => {
        return "success"
    })

}
