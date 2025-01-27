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
                    if type(current) is bool:
                        args.append(f"{getattr(self, f'_{field.name}NodeopArg')}")
                    else:
                        args.append(f"{getattr(self, f'_{field.name}NodeopArg')} {getattr(self, field.name)}")

        return "--plugin " + self._pluginNamespace + "::" + self._pluginName + " " + " ".join(args) if len(args) > 0 else ""
