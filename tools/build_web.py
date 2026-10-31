# PlatformIO pre-build: web/index.html -> gzip -> include/web_ui.h (PROGMEM).
# Gegenereerd bestand staat in .gitignore; wordt alleen herschreven als de bron wijzigt.
import gzip
import os

Import("env")  # noqa: F821 (PlatformIO)

root = env["PROJECT_DIR"]  # noqa: F821
src = os.path.join(root, "web", "index.html")
dst = os.path.join(root, "include", "web_ui.h")

if not os.path.exists(dst) or os.path.getmtime(src) > os.path.getmtime(dst):
    with open(src, "rb") as f:
        raw = f.read()
    data = gzip.compress(raw, compresslevel=9, mtime=0)
    lines = [", ".join(f"0x{b:02x}" for b in data[i:i + 20]) for i in range(0, len(data), 20)]
    with open(dst, "w", newline="\n") as f:
        f.write("// GEGENEREERD door tools/build_web.py uit web/index.html – niet bewerken.\n")
        f.write("#pragma once\n#include <Arduino.h>\n\n")
        f.write(f"static const size_t WEB_UI_GZ_LEN = {len(data)};\n")
        f.write("static const uint8_t WEB_UI_GZ[] PROGMEM = {\n  " + ",\n  ".join(lines) + "\n};\n")
    print(f"web_ui.h: {len(raw)} -> {len(data)} bytes (gzip)")  # ASCII: Windows-console
