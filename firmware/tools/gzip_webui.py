#!/usr/bin/env python3
"""Stage firmware/webui/ into a build directory as gzip-compressed assets.

WHY THIS EXISTS
---------------
The web UI is ~440 KB of plain text (app.js alone is ~350 KB). The 4 MB board's
`storage` SPIFFS partition is 0x80000 = 512 KB, of which roughly 470 KB is
usable once SPIFFS' own page/object overhead is paid — so the *uncompressed* UI
already sat at ~94 % full. Adding a second language to app.js would not fit, and
CI builds the 4 MB variant, so it would simply go red.

Text compresses roughly 3.2:1, which turns 94 % into low-20s % and buys back the
headroom. http_api.c has always known how to serve a pre-compressed asset
(open_asset() prefers "<path>.gz" and sets Content-Encoding: gzip); the build
just never produced one. This script is that missing step.

WHAT IT DOES
------------
Mirrors <src> into <stage> with every file rewritten as "<name>.gz". The source
tree is never touched: firmware/webui/ must stay plain, editable text that you
can open in an editor and diff in a review. Only the staged copy — which lives
in the build directory and is gitignored along with the rest of it — is
compressed, and it is that copy which gets imaged into storage.bin.

ONLY .gz IS SHIPPED. There is no uncompressed twin in the image; shipping both
would defeat the entire point. The consequence is deliberate and is documented
in http_api.c's open_asset(): the server falls back to the ".gz" file even for a
client that did not send "Accept-Encoding: gzip", and labels it honestly with
"Content-Encoding: gzip". Every browser has understood gzip for two decades and
this is a LAN appliance, so a client that cannot is not a client we have. A
plain `curl` (no --compressed) therefore gets correctly-labelled gzip bytes
rather than a 404 — pipe it through `gunzip`, or just pass `--compressed`.

Determinism: mtime is forced to 0 and the original filename is left out of the
gzip header, so an unchanged UI produces a byte-identical storage.bin. That
keeps release artefacts reproducible and stops `idf.py flash` from re-writing
the storage partition when nothing actually changed.
"""

import gzip
import os
import shutil
import sys


# Files that exist for the developer, not for the browser. README.md is 35 KB of
# design notes that nothing ever fetches; it has no business on the flash chip.
SKIP_NAMES = {"README.md"}
SKIP_SUFFIXES = (".gz", "~", ".orig", ".rej", ".swp")


def should_skip(name):
    return name in SKIP_NAMES or name.startswith(".") or name.endswith(SKIP_SUFFIXES)


def compress(src_path, dst_path):
    """Write src_path to dst_path.gz, deterministically (mtime 0, no filename)."""
    with open(src_path, "rb") as fin:
        raw = fin.read()
    # mtime=0 and an explicit empty filename keep the output reproducible. The
    # empty `filename` matters: given only a fileobj, GzipFile would take the
    # FNAME field from fileobj.name and stamp the build path's basename into
    # every file.
    with open(dst_path, "wb") as fout:
        with gzip.GzipFile(filename="", fileobj=fout, mode="wb",
                           compresslevel=9, mtime=0) as gz:
            gz.write(raw)
    return len(raw), os.path.getsize(dst_path)


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: gzip_webui.py <src-dir> <stage-dir>\n")
        return 2
    src, stage = os.path.abspath(argv[1]), os.path.abspath(argv[2])

    if not os.path.isdir(src):
        sys.stderr.write("gzip_webui: no such source directory: %s\n" % src)
        return 1

    # Rebuild the staging directory from scratch every time. A file deleted from
    # webui/ must disappear from the image too, and an incremental copy would
    # leave the stale one behind to be flashed forever.
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(stage)

    total_raw = 0
    total_gz = 0
    count = 0
    for root, dirs, files in os.walk(src):
        dirs[:] = sorted(d for d in dirs if not d.startswith("."))
        rel = os.path.relpath(root, src)
        out_dir = stage if rel == "." else os.path.join(stage, rel)
        if not os.path.isdir(out_dir):
            os.makedirs(out_dir)
        for name in sorted(files):
            if should_skip(name):
                continue
            raw, packed = compress(os.path.join(root, name),
                                   os.path.join(out_dir, name + ".gz"))
            total_raw += raw
            total_gz += packed
            count += 1

    if not count:
        sys.stderr.write("gzip_webui: nothing to compress in %s\n" % src)
        return 1

    pct = (100.0 * total_gz / total_raw) if total_raw else 0.0
    sys.stdout.write("gzip_webui: %d files, %d -> %d bytes (%.0f%%)\n"
                     % (count, total_raw, total_gz, pct))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
