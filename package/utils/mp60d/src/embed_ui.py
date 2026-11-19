#!/usr/bin/env python3
"""Convert the self-contained UI into a C array without external dependencies."""
from pathlib import Path
import sys

source = Path(sys.argv[1])
target = Path(sys.argv[2])
data = source.read_bytes()
lines = ["#include <stddef.h>", "", "const char ui_html[] = {"]
for offset in range(0, len(data), 20):
    lines.append("    " + ", ".join(str(b) for b in data[offset:offset + 20]) + ",")
lines.extend(["    0", "};", ""])
target.write_text("\n".join(lines), encoding="ascii")
