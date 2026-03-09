const PlopContext = require("../PlopContext")
const assert = require("assert")
const { rootDir } = PlopContext
const F = require("lodash/fp")
const { nestTargetToPath } = require("../util/plop-constants")

const { pkg } = PlopContext,
				{ version } = pkg


/**
 * @typedef {import("node-plop").NodePlopAPI} Plop
 * @param  plop {Plop.NodePlopAPI}
 */
function generator(plop) {
	return {
		description: "Add a new nest module to the mono-repo",
		prompts: [
			{
				type: "list",
				name: "target",
				choices:
								Object.keys(nestTargetToPath)
				,
				message: "target package or app"
			},
			{
				type: "input",
				name: "name",
				message: "Module base name, do not include the part name (Module,Manager,etc)",
				validate: (input) => /^[a-zA-Z0-9]+$/.test(input)
			},
			{
				type: "confirm",
				name: "overwrite",
				default: false,

				message: "Should overwrite if needed"
			}
		],

		actions: [

			// GENERATE NEW FILES
			{
				type: "add",
				data: {
					name: "{{name}}"
				},

				templateFile: `${rootDir}/tools/plop/templates/nest-module/NestModule.ts.hbs`,
				path: `${rootDir}/{{nestTargetToPath target}}/{{dashCase name}}/{{name}}Module.ts`
			},
			{
				type: "add",
				data: {
					name: "{{name}}"
				},

				templateFile: `${rootDir}/tools/plop/templates/nest-module/NestModuleManager.ts.hbs`,
				path: `${rootDir}/{{nestTargetToPath target}}/{{dashCase name}}/{{name}}Manager.ts`
			},
			{
				type: "add",
				data: {
					name: "{{name}}"
				},

				templateFile: `${rootDir}/tools/plop/templates/nest-module/NestModuleIndex.ts.hbs`,
				path: `${rootDir}/{{nestTargetToPath target}}/{{dashCase name}}/index.ts`
			},
			// PRINT FILES
			{
				type: "print-files",
				abortOnFail: true
			},

			// DUMP FILES
			{
				type: "flush-files",
				abortOnFail: true
			}

			// UPDATE PROJECT
			// {
			//   type: "update-project",
			//   abortOnFail: true
			// }
		]
	}
}

module.exports = generator
