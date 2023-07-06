#!/usr/bin/env python3
"""CMake's regex engine can't seem to handle multiline, so this extracts our
rust plugins.

Returns a combination of var names and crate names:

```
PLUGIN_ENV_NAME|target_name|cargo-name|staticlib_name.a|dylib_name.so;PLUGIN_BAR|bar...
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

        env_name = f"PLUGIN_{ex}{name_var}"
        target_name = env_name.lower()
        static_name = f"lib{name_var.lower()}.a"
        dyn_name = f"lib{name_var.lower()}.so"

        ret.append(f"{env_name}|{target_name}|{cargo_name}|{static_name}|{dyn_name}")

    print(";".join(ret), end="")

if __name__ == "__main__":
    main()
