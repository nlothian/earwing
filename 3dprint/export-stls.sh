#!/usr/bin/env bash
# Export the case, floor, separate dome grille, and microphone fit coupon.
# Print four copies of grille.stl; test one in mic-fit-test.stl first.
# Usage: ./3dprint/export-stls.sh [output-directory]
# The default output directory is 3dprint/.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
output_dir="${1:-$script_dir}"
scad="$script_dir/micarray.scad"
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
"$openscad" -q -D 'output_part="mic_test"' --export-format binstl \
    -o "$output_dir/mic-fit-test.stl" "$scad"
"$openscad" -q -D 'output_part="grille"' --export-format binstl \
    -o "$output_dir/grille.stl" "$scad"
printf 'Exported %s\n' \
    "$output_dir/case.stl" \
    "$output_dir/floor.stl" \
    "$output_dir/mic-fit-test.stl" \
    "$output_dir/grille.stl"
