#!/usr/bin/env bash
# Export the printable case and floor, plus one microphone insert.
# Usage: ./3dprint/export-stls.sh [output-directory]
# The default output directory is 3dprint/. Print four copies of insert.stl.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
output_dir="${1:-$script_dir}"
scad="$script_dir/micarray.scad"
insert="$script_dir/../inputs/INMP441+Mic+Mount.STL"
openscad="${OPENSCAD:-openscad}"

if ! command -v "$openscad" >/dev/null 2>&1; then
    echo "OpenSCAD not found; install it or set OPENSCAD to its executable path." >&2
    exit 1
fi

mkdir -p "$output_dir"

"$openscad" -q -D 'output_part="body"' --export-format binstl \
    -o "$output_dir/case.stl" "$scad"
"$openscad" -q -D 'output_part="floor"' --export-format binstl \
    -o "$output_dir/floor.stl" "$scad"
cp "$insert" "$output_dir/insert.stl"

printf 'Exported %s\n' \
    "$output_dir/case.stl" \
    "$output_dir/floor.stl" \
    "$output_dir/insert.stl"
