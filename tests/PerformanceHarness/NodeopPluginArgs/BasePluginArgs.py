#!/usr/bin/env python3

import dataclasses
import re

from dataclasses import dataclass

@dataclass
class BasePluginArgs:

    def supportedNodeopArgs(self) -> list:
        args = []
        for field in dataclasses.fields(self):
            match = re.search("\w*NodeopArg", field.name)
            if match is not None:
                args.append(getattr(self, field.name))
        return args

    def __str__(self) -> str:
        args = [] 
        for field in dataclasses.fields(self):
            match = re.search("[^_]", field.name[0])
            if match is not None:
                default = getattr(self, f"_{field.name}NodeopDefault")
                current = getattr(self, field.name)
                if current is not None and current != default:
                    arg = getattr(self, f'_{field.name}NodeopArg')
                    # Emit a bare flag only for options nodeop declares as a switch. The generator
                    # types those `bool`; an option that takes a value is typed from its default
                    # (`int`, `str`, ...). Keying on the runtime type instead would emit a bare flag
                    # for any field merely *assigned* a Python bool -- which nodeop then rejects,
                    # because boost consumes the following argument as the missing value.
                    if field.type is bool:
                        args.append(f"{arg}")
                    else:
                        # A value-taking option needs 1/0; f-string on a bool renders True/False.
                        value = int(current) if isinstance(current, bool) else current
                        args.append(f"{arg} {value}")

        return "--plugin " + self._pluginNamespace + "::" + self._pluginName + " " + " ".join(args) if len(args) > 0 else ""
