const PlopContext = require("../PlopContext")
const { rootDir } = PlopContext
const { TargetType } = require("../util/plop-constants")

const { rootPkg:pkg } = PlopContext,
				{ version } = pkg


/**
 * @typedef {import("node-plop").NodePlopAPI} Plop
 * @param  plop {Plop.NodePlopAPI}
 */
function generator(plop) {
	return {
		description: "Add a new C++ plugin",
		prompts: [
			{
				type: "input",
				name: "name",
				message: "C++ Plugin Name (must end with _plugin)",
				validate: (input) => {
					if (!input.length || !/^[a-z0-9_]+$/.test(input))
						return "Name must be lowercase alphanumeric with underscores only"
					if (!input.endsWith("_plugin"))
						return "Plugin name must end with _plugin"
					return true
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
				default: () => "sysio"
			},
		],

		actions: [

			// GENERATE NEW FILES
			{
				type: "add",
				data: {
					name: "{{name}}",
					namespace: "{{namespace}}"
				},

				templateFile: `${rootDir}/tools/plop/templates/create-cxx-plugin/CMakeLists.txt.hbs`,
				path: `${rootDir}/{{targetTypeToPath "plugin"}}/{{name}}/CMakeLists.txt`
			},
			{
				type: "add",
				data: {
					name: "{{name}}",
					namespace: "{{namespace}}"
				},

				templateFile: `${rootDir}/tools/plop/templates/create-cxx-plugin/plugin-header.hpp.hbs`,
				path: `${rootDir}/{{targetTypeToPath "plugin"}}/{{name}}/include/sysio/{{name}}/{{name}}.hpp`
			},
			{
				type: "add",
				data: {
					name: "{{name}}",
					namespace: "{{namespace}}"
				},

				templateFile: `${rootDir}/tools/plop/templates/create-cxx-plugin/plugin-src.cpp.hbs`,
				path: `${rootDir}/{{targetTypeToPath "plugin"}}/{{name}}/src/{{name}}.cpp`
			},
			{
				type: "add",
				data: {
					name: "{{name}}",
					namespace: "{{namespace}}"
				},

				templateFile: `${rootDir}/tools/plop/templates/create-cxx-plugin/plugin-test.cpp.hbs`,
				path: `${rootDir}/{{targetTypeToPath "plugin"}}/{{name}}/test/main.cpp`
			},
			{
				type: "add",
				data: {
					name: "{{name}}",
					namespace: "{{namespace}}"
				},

				templateFile: `${rootDir}/tools/plop/templates/create-cxx-plugin/plugin-testcase.cpp.hbs`,
				path: `${rootDir}/{{targetTypeToPath "plugin"}}/{{name}}/test/test_{{name}}.cpp`
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
		]
	}
}

module.exports = generator
