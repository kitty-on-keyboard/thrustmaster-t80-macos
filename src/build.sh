#!/bin/sh
# Everything here is one file against two system frameworks. No dependencies,
# no package manager, no Xcode project -- just the command line tools.
set -e
cd "$(dirname "$0")"
for f in t80_bridge hid_report_dump hid_phase_map; do
    [ -f "$f.c" ] || continue
    clang -O2 -o "$f" "$f.c" -framework IOKit -framework CoreFoundation
    echo "built $(pwd)/$f"
done
