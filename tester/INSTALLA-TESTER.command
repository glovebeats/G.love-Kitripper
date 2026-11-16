#!/bin/bash
# G.LOVE Kit Ripper — installazione per tester
cd "$(dirname "$0")"
DEST="$HOME/Library/Audio/Plug-Ins/Components"
mkdir -p "$DEST"
rm -rf "$DEST/G.LOVE Kit Ripper.component"
cp -R "G.LOVE Kit Ripper.component" "$DEST/"
xattr -cr "$DEST/G.LOVE Kit Ripper.component"
killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true
echo ""
echo "Installato. Apri Logic: traccia Software Instrument > AU Instruments > G.LOVE > G.LOVE Kit Ripper"
read -p "Premi Invio per chiudere..."
