#!/usr/bin/env python3
"""CMake's regex engine can't seem to handle multiline, so this extracts our
rust plugins.

Returns a combination of var names and crate names:

```
PLUGIN_CACHE_NAME|target_name|cargo-name|staticlib_name.a|libdylib_name.so|dylib_name.so;PLUGIN_BAR|bar...
```
"""

import re
import os

def main():
    this_path = os.path.dirname(__file__)

    with open(f"{this_path}/Cargo.toml") as f:
        cargo = f.read()
    
    members = re.search(r"members\s+=\s+\[(.*)\]", cargo, re.DOTALL).group(1)
    paths = [
        m.strip().split("\"")[1]
        for m in members.strip().split(",")
        if len(m) > 0
    ]

    ret = []

    for path in paths:
        if not (path.startswith("examples") or path.startswith("plugins")):
            continue

        with open(f"{this_path}/{path}/Cargo.toml") as f:
            data = f.read()
        
        cargo_name = re.search(r"name\s+=\s+\"(\S+)\"", data, re.MULTILINE).group(1)
        name_var = cargo_name.upper().replace("-","_")
        ex = "EXAMPLE_" if path.startswith("example") else ""

        # Cmake config name
        cache_name = f"PLUGIN_{ex}{name_var}"
        # Name of the target
        target_name = cache_name.lower()
        # Name of the staticlib
        static_name = f"lib{name_var.lower()}.a"
        # Name we want in the plugins dir, no lib prefix
        dyn_name_final = f"{name_var.lower()}.so"
        # Name that is output by rust
        dyn_name_out = f"lib{dyn_name_final}"

        ret.append(f"{cache_name}|{target_name}|{cargo_name}|{static_name}|{dyn_name_out}|{dyn_name_final}")

    print(";".join(ret), end="")

if __name__ == "__main__":
    main()
