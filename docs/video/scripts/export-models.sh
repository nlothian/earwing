#!/usr/bin/env bash
# Export the earwing showcase assembly from 3dprint/micarray.scad as separate
# binary STL meshes for the film's three.js scenes, one mesh per material:
#
#   body.stl    enclosure body (beige)
#   floor.stl   snap-in floor, seated (beige)
#   mounts.stl  the four microphone fittings in their sockets (black)
#   mount.stl   one fitting centred on X = Y = 0, for exploded animations
#   screen.stl  the display glass insert (near-black)
#
# All but mount.stl share the assembled product's world coordinates (mm,
# +Z up). The STLs are gitignored: rerun this after changing the SCAD.
#
# Usage: bash docs/video/scripts/export-models.sh   (from anywhere)
# Set OPENSCAD to use a particular binary.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
scad="$here/../../../3dprint/micarray.scad"
out="$here/../assets/models"

openscad="${OPENSCAD:-}"
if [[ -z "$openscad" ]]; then
    for candidate in \
        "$(command -v openscad || true)" \
        /opt/homebrew/bin/openscad \
        /Applications/OpenSCAD.app/Contents/MacOS/OpenSCAD; do
        if [[ -n "$candidate" && -x "$candidate" ]]; then
            openscad="$candidate"
            break
        fi
    done
fi
if [[ -z "$openscad" ]]; then
    echo "export-models: openscad not found; set OPENSCAD" >&2
    exit 1
fi

# Manifold is much faster than CGAL; older builds lack the flag.
backend=()
if "$openscad" --help 2>&1 | grep -q -- '--backend'; then
    backend=(--backend=manifold)
fi

mkdir -p "$out"

parts=(body floor mounts mount screen)
pids=()
for part in "${parts[@]}"; do
    "$openscad" -q ${backend[@]+"${backend[@]}"} \
        -D 'output_part="showcase"' \
        -D "showcase_part=\"$part\"" \
        --export-format binstl \
        -o "$out/$part.stl" \
        "$scad" &
    pids+=($!)
done

failed=0
for i in "${!parts[@]}"; do
    if ! wait "${pids[$i]}"; then
        echo "export-models: ${parts[$i]} failed" >&2
        failed=1
    fi
done
[[ $failed -eq 0 ]] || exit 1

# A binary STL is an 80-byte header, a uint32 triangle count, then 50 bytes
# per triangle: check each file is complete before reporting it.
for part in "${parts[@]}"; do
    file="$out/$part.stl"
    size=$(wc -c < "$file" | tr -d ' ')
    triangles=$(od -An -t u4 -j 80 -N 4 "$file" | tr -d ' ')
    if [[ "$size" -ne $((84 + 50 * triangles)) || "$triangles" -eq 0 ]]; then
        echo "export-models: $file is not a valid binary STL" >&2
        exit 1
    fi
    printf '%-10s %8d triangles %10d bytes\n' "$part.stl" "$triangles" "$size"
done
