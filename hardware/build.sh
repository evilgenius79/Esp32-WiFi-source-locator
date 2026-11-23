#!/bin/sh
# Exports the printable STL files and the preview pictures from holder.scad.
# Needs OpenSCAD; the pictures also need xvfb-run and Python with Pillow.
#   hardware/build.sh          STL files + pictures
#   hardware/build.sh stl      STL files only
set -e
cd "$(dirname "$0")"
mkdir -p stl ../docs
for p in hub clamp cradle latch handle; do
  openscad -o "stl/$p.stl" --export-format binstl -D "part=\"$p\"" holder.scad 2>&1 | grep -E "ERROR|WARNING" || true
  echo "stl/$p.stl"
done
[ "$1" = "stl" ] && exit 0

shot() {   # name, part, camera
  xvfb-run -a -s "-screen 0 2800x2100x24" openscad -o "/tmp/holder_$1.png" --imgsize=2800,2100 \
    --projection=p --camera="$3" --colorscheme=Tomorrow -D "part=\"$2\"" holder.scad >/dev/null 2>&1
  python3 -c "
from PIL import Image
im = Image.open('/tmp/holder_$1.png').convert('RGB').resize((1400, 1050), Image.LANCZOS)
im.save('../docs/$1.png', optimize=True)"
  echo "docs/$1.png"
}
shot holder          assembly "0,0,30,58,0,25,840"
shot holder_top      top      "0,-4,0,0,0,0,1080"
shot holder_exploded exploded "0,0,40,62,0,25,1250"
