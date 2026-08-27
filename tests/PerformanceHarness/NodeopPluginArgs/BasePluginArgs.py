#!/usr/bin/env python3

import dataclasses
import re

from dataclasses import dataclass

@dataclass
class BasePluginArgs:

    def supportedNodeopArgs(self) -> list:
        args = []
        for field in dataclasses.fields(self):
            # Anchored: sibling metadata fields such as _<key>NodeopArgTakesValue must not be
            # mistaken for the option string itself.
            if field.name.endswith("NodeopArg"):
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
                    # Arity comes from what nodeop declares, recorded by the generator. The field's
                    # Python type does not track it: an option can be typed bool and still consume a
                    # value, and a switch can be typed str.
                    if getattr(self, f'_{field.name}NodeopArgTakesValue'):
                        # A value-taking option needs 1/0; f-string on a bool renders True/False.
                        value = int(current) if isinstance(current, bool) else current
                        args.append(f"{arg} {value}")
                    else:
                        args.append(f"{arg}")

        return "--plugin " + self._pluginNamespace + "::" + self._pluginName + " " + " ".join(args) if len(args) > 0 else ""
