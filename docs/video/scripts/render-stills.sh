#!/usr/bin/env bash
# Render native OpenSCAD product stills of the earwing showcase assembly
# (output_part="showcase") into docs/video/assets/renders/, at 3840x2160 and
# downscaled with Lanczos to 1920x1080:
#
#   earwing-hero-*.png     3/4 view from the front (mic side) and above
#   earwing-top-*.png      top-down, mic row at the bottom of the frame
#   earwing-grills-*.png   close-up on the four grills
#   earwing-side-*.png     side profile from -X, the USB side
#
# and, for the film's four-cut montage, 1920x1080 only (rendered at 4K and
# downscaled), each clear of a headline band at x < 900, y > 780:
#
#   montage-1.png   3/4 from the mic side, front-left, 28 degrees up
#   montage-2.png   straight top-down, mic row at the bottom, M1 on the left
#   montage-3.png   3/4 from the mic side, front-right, 25 degrees up
#   montage-4.png   low near-frontal hero from the mic side, slightly left
#   montage.json    where each grill face (M1..M4) lands in each montage
#                   frame, from grill-ellipses.py with the same cameras
#
# The warm-white background (#F2EFEA) comes from the custom colour scheme
# earwing-warm-white.json beside the renders. OpenSCAD only loads render
# colour schemes from its resources or from <User Config Path>/color-schemes/
# render/, so this script copies the JSON there (on macOS:
# ~/Library/Application Support/OpenSCAD/color-schemes/render/). Delete that
# copy to uninstall it; OpenSCAD then lists "Earwing Warm White" no more.
#
# Cameras are eye,centre pairs in model millimetres, framed for 16:9 with
# OpenSCAD's fixed 22.5 degree vertical field of view.
#
# Usage: bash docs/video/scripts/render-stills.sh   (from anywhere)
# Needs ImageMagick (magick) and python3 with numpy. Set OPENSCAD to use a
# particular binary.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
scad="$here/../../../3dprint/micarray.scad"
out="$here/../assets/renders"
scheme_json="$out/earwing-warm-white.json"
scheme_name="Earwing Warm White"

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
    echo "render-stills: openscad not found; set OPENSCAD" >&2
    exit 1
fi

config_dir=$("$openscad" --info 2>/dev/null \
    | sed -n 's/^User Config Path: //p')
if [[ -z "$config_dir" ]]; then
    echo "render-stills: could not read OpenSCAD's User Config Path" >&2
    exit 1
fi
mkdir -p "$config_dir/color-schemes/render"
cp "$scheme_json" "$config_dir/color-schemes/render/"

backend=()
if "$openscad" --help 2>&1 | grep -q -- '--backend'; then
    backend=(--backend=manifold)
fi

mkdir -p "$out"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# name  kind  eye_x,eye_y,eye_z,centre_x,centre_y,centre_z
# A "still" keeps its 3840x2160 render beside the 1920x1080 downscale; a
# "montage" frame keeps only the downscale, named <name>.png.
views=(
    "earwing-hero   still    135.52,-200.32,136.44,-2.24,20.14,-13.65"
    "earwing-top    still    0,28.45,277.26,0,28.46,-2.70"
    "earwing-grills still    67.36,-141.83,86.62,-4.33,19.20,-15.15"
    "earwing-side   still    -201.69,27.28,4.68,-0.17,27.28,-5.88"
    "montage-1      montage  -172.78,-218.06,132.19,-1.79,17.29,-22.49"
    "montage-2      montage  0.00,10.84,330.48,0.00,10.85,-2.70"
    "montage-3      montage  159.55,-199.96,104.84,0.63,18.78,-21.23"
    "montage-4      montage  -38.69,-246.84,65.32,8.82,22.59,-18.33"
)

pids=()
names=()
for view in "${views[@]}"; do
    read -r name kind camera <<< "$view"
    "$openscad" -q ${backend[@]+"${backend[@]}"} \
        -D 'output_part="showcase"' \
        --render \
        --projection=p \
        --imgsize=3840,2160 \
        --camera="$camera" \
        --colorscheme="$scheme_name" \
        -o "$work/$name.png" \
        "$scad" &
    pids+=($!)
    names+=("$name")
done

failed=0
for i in "${!pids[@]}"; do
    if ! wait "${pids[$i]}"; then
        echo "render-stills: ${names[$i]} failed" >&2
        failed=1
    fi
done
[[ $failed -eq 0 ]] || exit 1

montage_cameras=()
for view in "${views[@]}"; do
    read -r name kind camera <<< "$view"
    if [[ "$kind" == still ]]; then
        cp "$work/$name.png" "$out/$name-3840x2160.png"
        small="$out/$name-1920x1080.png"
    else
        small="$out/$name.png"
        montage_cameras+=("$name.png=$camera")
    fi
    magick "$work/$name.png" \
        -colorspace RGB -filter Lanczos -resize 1920x1080 -colorspace sRGB \
        "$small"
    echo "$name: $(basename "$small")"
done

python3 "$here/grill-ellipses.py" "$out/montage.json" 1920 1080 \
    "${montage_cameras[@]}"
echo "montage.json: grill ellipses for ${#montage_cameras[@]} frames"
