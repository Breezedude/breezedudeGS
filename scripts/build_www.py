#!/usr/bin/env python3
"""Minify and gzip the web UI into a directory that becomes the LittleFS image.

Called from CMake: build_www.py <source dir> <output dir> [firmware version]

The firmware version is written to version.txt (uncompressed); the firmware
compares it with its own version to decide whether the UI needs replacing.

The Node minifier from bundle/ is used when node is available (same toolchain as
the PlatformIO build); otherwise the files are copied verbatim and only gzipped,
so a plain ESP-IDF checkout without Node still builds.
"""

import gzip
import hashlib
import os
import re
import shutil
import subprocess
import sys

TEXT_SUFFIXES = (".html", ".css", ".js", ".json", ".svg", ".txt")
VERSION_FILE = "version.txt"
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def have_node() -> bool:
    return shutil.which("node") is not None and os.path.isdir(
        os.path.join(PROJECT_ROOT, "bundle", "node_modules")
    )


def minify_with_node(src: str, dst: str) -> bool:
    try:
        subprocess.run(
            ["node", os.path.join("bundle", "minify.js"), src, dst],
            check=True,
            cwd=PROJECT_ROOT,
        )
        return True
    except (subprocess.CalledProcessError, OSError) as err:
        print(f"build_www: node minifier unavailable ({err}), copying unminified")
        return False


def copy_tree(src: str, dst: str) -> None:
    for root, _dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        target = os.path.join(dst, rel) if rel != "." else dst
        os.makedirs(target, exist_ok=True)
        for name in files:
            shutil.copyfile(os.path.join(root, name), os.path.join(target, name))


def version_assets(directory: str) -> None:
    """Give index.html's references to the other files a content hash.

    A browser that knew this station before keeps its files cached under the
    same names - after the migration from the Arduino firmware, its style.css
    and app.js - and would otherwise combine them with the new index.html."""
    index = os.path.join(directory, "index.html")
    if not os.path.isfile(index):
        return
    with open(index, encoding="utf-8") as handle:
        html = handle.read()
    for name in sorted(os.listdir(directory)):
        path = os.path.join(directory, name)
        if name == "index.html" or not os.path.isfile(path):
            continue
        with open(path, "rb") as handle:
            digest = hashlib.sha256(handle.read()).hexdigest()[:8]
        pattern = r"((?:href|src)=[\"']?)" + re.escape(name) + r"(?=[\"'\s>])"
        html = re.sub(pattern, lambda m: m.group(1) + name + "?v=" + digest, html)
    with open(index, "w", encoding="utf-8") as handle:
        handle.write(html)


def gzip_tree(directory: str) -> None:
    """Gzip every file in place. The web server serves <name>.gz with
    Content-Encoding: gzip, which is what the Arduino build did too."""
    for root, _dirs, files in os.walk(directory):
        for name in files:
            if name.endswith(".gz") or name in (".stamp", VERSION_FILE):
                continue
            path = os.path.join(root, name)
            with open(path, "rb") as src, gzip.open(path + ".gz", "wb", compresslevel=9) as dst:
                shutil.copyfileobj(src, dst)
            os.remove(path)


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print(__doc__)
        return 2

    src, dst = sys.argv[1], sys.argv[2]
    version = sys.argv[3] if len(sys.argv) == 4 else ""
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    os.makedirs(dst, exist_ok=True)

    if not have_node() or not minify_with_node(src, dst):
        copy_tree(src, dst)

    version_assets(dst)
    gzip_tree(dst)

    if version:
        with open(os.path.join(dst, VERSION_FILE), "w", encoding="ascii") as handle:
            handle.write(version + "\n")

    total = 0
    for root, _dirs, files in os.walk(dst):
        for name in files:
            size = os.path.getsize(os.path.join(root, name))
            total += size
            print(f"  {os.path.relpath(os.path.join(root, name), dst):<28} {size:>8} B")
    print(f"build_www: {total} bytes of compressed web assets")

    # Stamp file so CMake can depend on a concrete output path.
    with open(os.path.join(dst, ".stamp"), "w", encoding="utf-8") as stamp:
        stamp.write("ok\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
