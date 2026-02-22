const PlopContext = require("../PlopContext")
const assert = require("assert")
const { rootDir } = PlopContext
const F = require("lodash/fp")
const { nestTargetToPath, targetTypeToPath, TargetType} = require("../util/plop-constants")
const Path = require("node:path");
const {toTargetPath} = require("../util/plop-shared-utils");

const { rootPkg:pkg } = PlopContext,
				{ version } = pkg


/**
 * @typedef {import("node-plop").NodePlopAPI} Plop
 * @param  plop {Plop.NodePlopAPI}
 */
function generator(plop) {
	return {
		description: "Add a new C++ library",
		prompts: [
			{
				type: "input",
				name: "name",
				message: "C++ Library Name",
				validate: (input) => {
					return input.length && /^[a-z0-9_]+$/.test(input)
						// !Path.existsSync(toTargetPath(TargetType.library, input))
				},

			},
			{
				type: "input",
				name: "namespace",
				message: "C++ Namespace",
				validate: (input) => {
					return input.length && /^([a-z0-9_](::)?)+$/.test(input) &&
						!input.startsWith("::") &&
						!input.endsWith("::")
				},
				value: (answers) => answers.name

			},
			// {
			// 	type: "confirm",
			// 	name: "overwrite",
			// 	default: false,
			//
			// 	message: "Should overwrite if needed"
			// }
		],

		actions: [

			// GENERATE NEW FILES
			{
				type: "add",
				data: {
					name: "{{name}}",
					namespace: "{{namespace}}"
				},

				templateFile: `${rootDir}/tools/plop/templates/create-cxx-library/CMakeLists.txt.hbs`,
				path: `${rootDir}/{{targetTypeToPath "library"}}/{{name}}/CMakeLists.txt`
			},
			{
				type: "add",
				data: {
					name: "{{name}}",
					namespace: "{{namespace}}"
				},

				templateFile: `${rootDir}/tools/plop/templates/create-cxx-library/library-header.hpp.hbs`,
				path: `${rootDir}/{{targetTypeToPath "library"}}/{{name}}/include/{{namespaceToPath namespace}}/{{name}}.hpp`
			},
			{
				type: "add",
				data: {
					name: "{{name}}",
					namespace: "{{namespace}}"
				},

				templateFile: `${rootDir}/tools/plop/templates/create-cxx-library/library-src.cpp.hbs`,
				path: `${rootDir}/{{targetTypeToPath "library"}}/{{name}}/src/{{name}}.cpp`
			},

			{
				type: "add",
				data: {
					name: "{{name}}",
					namespace: "{{namespace}}"
				},

				templateFile: `${rootDir}/tools/plop/templates/create-cxx-library/library-test.cpp.hbs`,
				path: `${rootDir}/{{targetTypeToPath "library"}}/{{name}}/test/main.cpp`
			},
			{
				type: "add",
				data: {
					name: "{{name}}",
					namespace: "{{namespace}}"
				},

				templateFile: `${rootDir}/tools/plop/templates/create-cxx-library/library-testcase.cpp.hbs`,
				path: `${rootDir}/{{targetTypeToPath "library"}}/{{name}}/test/test_{{name}}.cpp`
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
