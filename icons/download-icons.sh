#!/bin/bash
# Download original-style RE4 item icons from evilresource.com into this folder.
# Run from the icons/ directory or adjust paths.

mkdir -p icons
cd icons || exit 1

base="https://www.evilresource.com/images/data/full/re4"
files=(
  handgun-ammo.png
  shotgun-shells.png
  rifle-ammo.png
  tmp-ammo.png
  magnum-ammo.png
  handcannon-ammo.png
  hand-grenade.png
  flash-grenade.png
  incendiary-grenade.png
  green-herb.png
  red-herb.png
  yellow-herb.png
  first-aid-spray.png
  chicken-egg.png
  black-bass.png
  ruby.png
  emerald.png
  gold-bar.png
  crown.png
  arrows.png
)

for f in "${files[@]}"; do
  echo "Downloading $f ..."
  curl -sL -o "$f" "$base/$f" || echo "Failed: $f"
done

echo "Done. Place the icons/ folder next to xinput1_3.dll."
