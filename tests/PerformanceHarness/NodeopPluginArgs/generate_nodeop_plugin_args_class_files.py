#!/usr/bin/env python3

import re
import subprocess

"""
The purpose of this script is to attempt to generate *PluginArgs.py files, containing respective dataclass objects,
to encapsulate the configurations options available for each plugin as currently documented in nodeop's --help command.

It makes use of the compiled nodeop program and runs the --help command, capturing the output.
It then parses the output, breaking down the presented configuration options by plugin section (ignoring application and test plugin config options).
This provides a rudimentary list of plugins supported, config options for each plugin, and attempts to acertain default values and types.
The script then uses the parsed output to generate *PluginArgs.py scripts, placing them in the NodeopPluginArgs directory.

Currently it generates the following scripts:
- ChainPluginArgs.py
- HttpPluginArgs.py
- NetPluginArgs.py
- ProducerPluginArgs.py
- ResourceMonitorPluginArgs.py
- SignatureProviderManagerPluginArgs.py
- StateHistoryPluginArgs.py
- TraceApiPluginArgs.py

Each *PluginArgs.py file contains one dataclass that captures the available configuration options for that plugin via nodeop command line.

Each config option is represented by 3 member variables, for example:
1) blocksDir: str=None
    --This is the field that will be populated when the dataclass is used by other scripts to configure nodeop
2) _blocksDirNodeopDefault: str='"blocks"'
    --This field captures the default value in the nodeop output.  This will be compared against the first field to see if the configuration
    option will be required on the command line to override the default value when running nodeop.
3) _blocksDirNodeopArg: str="--blocks-dir"
    --This field captures the command line config option for use when creating the command line string

The BasePluginArgs class provides implementations for 2 useful functions for each of these classes:
1) supportedNodeopArgs
    -- Provides a list of all the command line config options currently supported by the dataclass
2) __str__
    -- Provides the command line argument string for the current configuration to pass to nodeop
       (this only provides command line options where configured values differ from defaults)

Some current limitations:
- There are some hardcoded edge cases when trying to determine the types associated with certain default argument parameters.
  These may need to be updated to account for new/different options as they are added/removed/modified by nodeop

Note:
- To help with maintainability the validate_nodeop_plugin_args.py test script is provided which validates the current
  *PluginArgs dataclass configuration objects against the current nodeop --help output to notify developers when
  configuration options have changed and updates are required.
"""


def main():
    result = subprocess.run(["../../../bin/nodeop", "--help"], stdout=subprocess.PIPE, universal_newlines=True)

    def parseArity(helpText: str) -> dict:
        """Map each --option to whether nodeop consumes a value for it.

        boost::program_options puts the value placeholder one space after the option name and starts
        the description column after two or more spaces. The placeholder is "arg" only when the
        option did not set a value_name -- "--wasm-runtime runtime (=sys-vm-jit)" is a value option
        too -- so its presence, not its spelling, is the signal. This must run before whitespace is
        collapsed below, which erases the distinction.

        Anchored to the two-space option column so that wrapped description lines, which are
        indented far further and may themselves mention an option, cannot overwrite a real entry.
        The optional group covers the short-form spelling, "-e [ --enable-stale-production ]".
        """
        arity = {}
        for line in helpText.splitlines():
            match = re.match("  (?:-\\w \\[ )?(--[\\w\\-]+)(?: \\])?(.*)", line)
            if match is not None:
                arity[match.group(1)] = re.match(" \\S", match.group(2)) is not None
        return arity

    argTakesValue = parseArity(result.stdout)

    myStr = result.stdout
    myStr = myStr.rstrip("\n")
    myStr = re.sub(":\n\\s+-",':@@@\n  -', string=myStr)
    myStr = re.sub("\n\n",'\n@@@', string=myStr)
    myStr = re.sub("Application Options:\n",'', string=myStr)
    pluginSections = re.split("(@@@.*?@@@\n)", string=myStr)

    def pairwise(iterable):
        "s -> (s0, s1), (s2, s3), (s4, s5), ..."
        a = iter(iterable)
        return zip(a, a)

    pluginOptsDict = {}
    for section, options in pairwise(pluginSections[1:]):
        myOpts = re.sub("\\s+", " ", options)
        myOpts = re.sub("\n", " ", myOpts)
        myOpts = re.sub(" --", "\n--",string = myOpts)
        splitOpts=re.split("\n", myOpts)

        argDescDict = {}
        for opt in splitOpts[1:]:
            secondSplit = re.split("(--[\\w\\-]+)", opt)[1:]
            argument=secondSplit[0]
            argDefaultDesc=secondSplit[1].lstrip("\\s")
            argDescDict[argument] = argDefaultDesc
        section=re.sub("@@@", "", section)
        section=re.sub("\n", "", section)
        sectionSplit=re.split("::", section)
        configSection = section
        if len(sectionSplit) > 1:
            configSection=sectionSplit[1]

        if pluginOptsDict.get(configSection) is not None:
            pluginOptsDict[configSection].update(argDescDict)
        else:
            pluginOptsDict[configSection] = argDescDict

    newDict = {}
    for key, value in pluginOptsDict.items():
        newPlugin="".join([x.capitalize() for x in key.split('_')]).replace(":","")

        newArgs = {}
        for key, value in value.items():
            newKey="".join([x.capitalize() for x in key.split('-')]).replace('--','')
            newKey="".join([newKey[0].lower(), newKey[1:]])
            newArgs[newKey]=value
        newDict[newPlugin]=newArgs

    def writeDataclass(plugin:str, dataFieldDict:dict, pluginOptsDict:dict):
        newPlugin="".join([x.capitalize() for x in plugin.split('_')]).replace(":","")
        pluginArgsFile=f"./{newPlugin}Args.py"
        with open(pluginArgsFile, 'w') as dataclassFile:
            chainPluginArgs = dataFieldDict[newPlugin]

            dataclassFile.write(f"#!/usr/bin/env python3\n\n")
            dataclassFile.write(f"from dataclasses import dataclass\n")
            dataclassFile.write(f"from .BasePluginArgs import BasePluginArgs\n\n")
            dataclassFile.write(f"\"\"\"\n")
            dataclassFile.write(f"This file/class was generated by generate_nodeop_plugin_args_class_files.py\n")
            dataclassFile.write(f"\"\"\"\n\n")
            dataclassFile.write(f"@dataclass\nclass {newPlugin}Args(BasePluginArgs):\n")
            dataclassFile.write(f"    _pluginNamespace: str=\"sysio\"\n")
            dataclassFile.write(f"    _pluginName: str=\"{plugin[:-1]}\"\n")

            for key, value in pluginOptsDict[plugin].items():
                newKey="".join([x.capitalize() for x in key.split('-')]).replace('--','')
                newKey="".join([newKey[0].lower(), newKey[1:]])
                value = chainPluginArgs[newKey]
                # Arity comes from nodeop's own output, not from the option's name or default.
                # Unknown options are assumed to take a value: rendering "--switch value" fails
                # loudly, while a bare flag on a value option silently eats the next argument.
                takesValue = argTakesValue.get(key, True)
                match = re.search("\\(=.*?\\)", value)
                if not takesValue:
                    fieldType, defaultVal = "bool", "False"
                elif match is not None:
                    defaultStr = match.group(0)[2:-1]
                    try:
                        fieldType, defaultVal = "int", str(int(defaultStr))
                    except ValueError:
                        quote = "\'" if re.search("\"", defaultStr) else "\""
                        fieldType, defaultVal = "str", f"{quote}{defaultStr}{quote}"
                elif re.search("sizegb|maxage|retainblocks", newKey, re.IGNORECASE):
                    # Value options nodeop prints without a default. The annotation is documentation
                    # for harness users; takesValue above is what decides how they are rendered.
                    fieldType, defaultVal = "int", "None"
                else:
                    fieldType, defaultVal = "str", "None"
                dataclassFile.write(f"    {newKey}: {fieldType}=None\n")
                dataclassFile.write(f"    _{newKey}NodeopDefault: {fieldType}={defaultVal}\n")
                dataclassFile.write(f"    _{newKey}NodeopArg: str=\"{key}\"\n")
                dataclassFile.write(f"    _{newKey}NodeopArgTakesValue: bool={takesValue}\n")

            def writeMainFxn(pluginName: str) -> str:
                return f"""\
def main():\n\
    pluginArgs = {pluginName}()\n\
    print(pluginArgs.supportedNodeopArgs())\n\
    exit(0)\n\n\
if __name__ == '__main__':\n\
    main()\n"""

            def writeHelpers(pluginName: str) -> str:
                return "\n" + writeMainFxn(pluginName)

            dataclassFile.write(writeHelpers(f"{newPlugin}Args"))
    
    writeDataclass(plugin="chain_plugin:", dataFieldDict=newDict, pluginOptsDict=pluginOptsDict)
    writeDataclass(plugin="http_plugin:", dataFieldDict=newDict, pluginOptsDict=pluginOptsDict)
    writeDataclass(plugin="net_plugin:", dataFieldDict=newDict, pluginOptsDict=pluginOptsDict)
    writeDataclass(plugin="producer_plugin:", dataFieldDict=newDict, pluginOptsDict=pluginOptsDict)
    writeDataclass(plugin="resource_monitor_plugin:", dataFieldDict=newDict, pluginOptsDict=pluginOptsDict)
    writeDataclass(plugin="signature_provider_manager_plugin:", dataFieldDict=newDict, pluginOptsDict=pluginOptsDict)
    writeDataclass(plugin="state_history_plugin:", dataFieldDict=newDict, pluginOptsDict=pluginOptsDict)
    writeDataclass(plugin="trace_api_plugin:", dataFieldDict=newDict, pluginOptsDict=pluginOptsDict)

    exit(0)

if __name__ == '__main__':
    main()
