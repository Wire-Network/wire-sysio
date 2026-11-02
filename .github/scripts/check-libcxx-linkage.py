#!/usr/bin/env python3
"""Reject host packages that accidentally link a different C++ runtime."""
import argparse
import pathlib
import platform
import re
import subprocess


def resolve_darwin_runtime(binary, name):
    """Resolve a direct runtime dependency using the executable's LC_RPATH entries."""
    directory = str(binary.resolve().parent)
    name = name.replace("@loader_path", directory).replace("@executable_path", directory)
    if not name.startswith("@rpath/"):
        return pathlib.Path(name)
    commands = subprocess.run(["otool", "-l", str(binary)], check=True, capture_output=True, text=True)
    for rpath in re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset", commands.stdout):
        rpath = rpath.replace("@loader_path", directory).replace("@executable_path", directory)
        candidate = pathlib.Path(rpath) / name.removeprefix("@rpath/")
        if candidate.is_file():
            return candidate
    raise SystemExit(f"No LC_RPATH resolves {name} in {binary}")


def main():
    """Inspect direct shared-library dependencies of each supplied executable."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shared-runtime-root", type=pathlib.Path,
                        help="Allow only the pinned shared runtime for native Debug builds")
    parser.add_argument("binaries", nargs="+", type=pathlib.Path)
    args = parser.parse_args()
    forbidden = re.compile(r"lib(?:stdc\+\+|c\+\+(?:abi)?|unwind|gcc_s)(?:\.[0-9]+)*\.(?:so|dylib)")
    for binary in args.binaries:
        if not binary.is_file():
            raise SystemExit(f"Missing executable: {binary}")
        command = ["otool", "-L"] if platform.system() == "Darwin" else ["readelf", "-d"]
        result = subprocess.run(command + [str(binary)], check=True, capture_output=True, text=True)
        if args.shared_runtime_root:
            if platform.system() == "Linux":
                result = subprocess.run(["ldd", str(binary)], check=True, capture_output=True, text=True)
            root = args.shared_runtime_root.resolve()
            found = set()
            for line in result.stdout.splitlines():
                match = forbidden.search(line)
                if not match:
                    continue
                name = match.group()
                if name.startswith(("libstdc++", "libgcc_s")):
                    raise SystemExit(f"Unexpected GNU runtime in {binary}: {line}")
                if platform.system() == "Linux":
                    path = pathlib.Path(line.split("=>", 1)[1].strip().split()[0])
                else:
                    path = resolve_darwin_runtime(binary, line.strip().split(" (", 1)[0])
                if not path.is_file() or not path.resolve().is_relative_to(root):
                    raise SystemExit(f"Runtime outside the pinned prefix in {binary}: {line}")
                found.add(name.split(".so")[0].split(".dylib")[0].split(".1")[0])
            if found != {"libc++", "libc++abi", "libunwind"}:
                raise SystemExit(f"Incomplete shared C++ runtime in {binary}: {sorted(found)}")
            print(f"{binary}: shared C++ runtime resolves within {root}")
        elif forbidden.search(result.stdout):
            raise SystemExit(f"Unexpected shared C++ runtime in {binary}:\n{result.stdout}")
        else:
            print(f"{binary}: no shared C++ standard-library or unwinder dependency")


if __name__ == "__main__":
    main()
