#!/usr/bin/env python3
"""Generate Flux's compilation database from the latest build's .cmd files.

The tools build runs in the Flux source tree; the embedded kernel build runs
in its output tree. Keep these working directories separate so relative
include paths resolve correctly. No compiler interception or rebuild is needed.
"""

import argparse
import json
import os
from pathlib import Path
import re
import tempfile


# Kbuild uses savedcmd_, while tools/build uses cmd_ (often after dependencies).
# Like the kernel's gen_compile_commands.py, omit post-compile shell commands.
COMMAND = re.compile(r"^(?:savedcmd|cmd)_[^ ]*\.o := (.* )([^ ]*\.[cS]) *(?:;|$)")


def collect(command_dir, working_dir):
    entries = []
    if not command_dir.is_dir():
        return entries
    for path in sorted(command_dir.rglob(".*.cmd")):
        for line in path.read_text().splitlines():
            match = COMMAND.match(line)
            if not match:
                continue
            prefix, source = match.groups()
            source_path = Path(os.path.abspath(working_dir / source))
            if not source_path.is_file():
                continue
            # Undo Make's escaping without changing compiler/shell quoting.
            command = (prefix + source).replace("$(pound)", "#").replace(r"\#", "#")
            entries.append({"directory": str(working_dir),
                            "file": str(source_path), "command": command})
    return entries


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=root / "build")
    parser.add_argument("--output", type=Path, default=root / "compile_commands.json")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    runtime = collect(build / "obj", root)
    kernel = collect(build / "kernel", build / "kernel")
    entries = runtime + kernel
    if not entries:
        parser.error("no saved compile commands found; build Flux first")
    entries.sort(key=lambda entry: (entry["file"], entry["directory"], entry["command"]))
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", dir=output.parent,
                                         prefix=".compile_commands-", delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(entries, stream, indent=2)
            stream.write("\n")
        temporary.chmod(0o644)
        temporary.replace(output)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    print(f"  COMPDB  {output} ({len(runtime)} runtime, {len(kernel)} kernel entries)")


if __name__ == "__main__":
    main()
