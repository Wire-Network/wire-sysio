require("source-map-support/register")

const registerSharedActions = require("./tools/plop/actions/SharedActions")
const Fs = require("fs")
const {asOption} = require("@3fv/prelude-ts")
const Path = require("path")
const {echo} = require("shelljs")
const {isFunction} = require("@3fv/guard")
const registerStringHelpers = require("./tools/plop/helpers/plop-string-helpers")
const registerTargetTypeHelpers = require("./tools/plop/helpers/plop-target-type-helpers")


module.exports = function plopfile(plop) {

    registerSharedActions(plop)
    registerStringHelpers(plop)
    registerTargetTypeHelpers(plop)

    const addGenerator = (name, configFilename = name) =>
        asOption(configFilename)
            .map(configFilename =>
                Path.resolve(
                    __dirname,
                    "tools",
                    "plop",
                    "generators",
                    `${configFilename}.js`
                )
            )
            .tap(configFile =>
                echo(
                    `Loading generator from config file (${configFile})`
                )
            )
            .filter(Fs.existsSync)
            .map(require)
            // .filter(mod => !!mod.default)
            .match({
                Some: config =>
                    plop.setGenerator(
                        name,
                        isFunction(config) ? config(plop) : config
                    ),
                None: () => {
                    throw Error(
                        `failed to load config (${configFilename}) for ${name}`
                    )
                }
            })

    addGenerator("create-cxx-library")
    // addGenerator("new-api-client-package")
    // addGenerator("nest-module")
    // addGenerator("service")
    // addGenerator("job-runner-module")
    // addGenerator("redux-slice")
}
