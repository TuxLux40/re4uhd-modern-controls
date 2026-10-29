#!/bin/bash
# Download original item icons from evilresource.com for the Pickup Ticker.
# Place the resulting PNGs in an 'icons' folder next to xinput1_3.dll.
set -e
mkdir -p icons
cd icons
BASE="https://www.evilresource.com/images/data/full/re4"
for f in handgun-ammo shotgun-ammo rifle-ammo magnum-ammo tmp-ammo handcannon-ammo \
         hand-grenade incendiary-grenade flash-grenade grenade \
         green-herb red-herb yellow-herb first-aid-spray \
         white-egg brown-egg gold-egg black-bass pesetas; do
  echo "Fetching $f.png"
  curl -fsSL -o "$f.png" "$BASE/$f.png" || echo "Failed: $f"
done
echo "Done. Copy the icons/ folder next to your xinput1_3.dll."
