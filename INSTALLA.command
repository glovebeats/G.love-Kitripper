#!/bin/bash
# G.LOVE Kit Ripper — compila e installa il plugin AU per Logic Pro
# Doppio click su questo file (o trascinalo nel Terminale e premi Invio).
set -e
cd "$(dirname "$0")"
echo ""
echo "=== G.LOVE Kit Ripper: installazione ==="

# 1. Strumenti Apple (compilatore). Se mancano, macOS apre la finestra di installazione.
if ! xcode-select -p >/dev/null 2>&1; then
  echo "Mancano gli strumenti di sviluppo Apple: si apre l'installazione."
  echo "Quando ha finito, rilancia questo file."
  xcode-select --install || true
  read -p "Premi Invio per chiudere..."
  exit 1
fi

# Trova un SDK di macOS con cui il linker funziona davvero
# (a volte gli strumenti Apple hanno un SDK piu' nuovo del linker: "tapi error: malformed file")
ARCH="$(uname -m)"
TMPT="$(mktemp -d)"
printf '#include <algorithm>\nint main(){int a[2]={2,1};std::sort(a,a+2);return 0;}\n' > "$TMPT/t.cpp"
SDKROOT=""
CANDIDATES="$(xcrun --sdk macosx --show-sdk-path 2>/dev/null)"
for d in $(ls -d /Library/Developer/CommandLineTools/SDKs/MacOSX*.*.sdk /Library/Developer/CommandLineTools/SDKs/MacOSX[0-9]*.sdk 2>/dev/null | sort -rV | uniq); do
  CANDIDATES="$CANDIDATES $d"
done
for sdk in $CANDIDATES; do
  [ -d "$sdk" ] || continue
  if xcrun clang++ -arch "$ARCH" -isysroot "$sdk" "$TMPT/t.cpp" -o "$TMPT/t" >/dev/null 2>&1; then
    SDKROOT="$sdk"; break
  else
    echo "SDK non utilizzabile: $sdk"
  fi
done
rm -rf "$TMPT"
if [ -z "$SDKROOT" ]; then
  echo ""
  echo "Nessun SDK funziona con il compilatore installato. Reinstalla gli strumenti Apple:"
  echo "  sudo rm -rf /Library/Developer/CommandLineTools && xcode-select --install"
  echo "Elenco SDK presenti:"; ls /Library/Developer/CommandLineTools/SDKs/ 2>/dev/null
  read -p "Premi Invio per chiudere..."
  exit 1
fi
export SDKROOT
echo "SDK: $SDKROOT  (architettura: $ARCH)"

# La compilazione avviene fuori da Google Drive, per non sincronizzare migliaia di file
WORK="$HOME/Library/Caches/GLoveKitRipper-build"
mkdir -p "$WORK"

# 2. CMake: usa quello installato, altrimenti lo scarica (una volta sola)
CMAKE="$(command -v cmake || true)"
if [ -z "$CMAKE" ]; then
  CMAKE="$WORK/cmake/CMake.app/Contents/bin/cmake"
  if [ ! -x "$CMAKE" ]; then
    echo "Scarico CMake..."
    mkdir -p "$WORK/cmake"
    curl -L --fail -o "$WORK/cmake.tgz" \
      https://github.com/Kitware/CMake/releases/download/v3.30.5/cmake-3.30.5-macos-universal.tar.gz
    tar -xzf "$WORK/cmake.tgz" -C "$WORK/cmake" --strip-components 1
    rm -f "$WORK/cmake.tgz"
  fi
fi

# 3. Compila (la prima volta scarica JUCE: qualche minuto)
echo "Compilo... (la prima volta ci vogliono alcuni minuti)"
rm -rf "$WORK/out/CMakeCache.txt" "$WORK/out/CMakeFiles"
"$CMAKE" -S . -B "$WORK/out" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_SYSROOT="$SDKROOT" -DCMAKE_OSX_ARCHITECTURES="$ARCH"
"$CMAKE" --build "$WORK/out" --config Release -j "$(sysctl -n hw.ncpu)"

# 4. Il plugin viene copiato in ~/Library/Audio/Plug-Ins/Components
COMP="$HOME/Library/Audio/Plug-Ins/Components/G.LOVE Kit Ripper.component"
if [ ! -d "$COMP" ]; then
  cp -R "$WORK/out/GLoveKitRipper_artefacts/Release/AU/G.LOVE Kit Ripper.component" "$HOME/Library/Audio/Plug-Ins/Components/"
fi

# 5. Fa rileggere i plugin a macOS/Logic e lo valida
killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true
echo ""
echo "Validazione Audio Unit:"
auval -v aumu Gkrp Glov | tail -n 3 || true

echo ""
echo "FATTO. Apri Logic: traccia Software Instrument > AU Instruments > G.LOVE > G.LOVE Kit Ripper"
read -p "Premi Invio per chiudere..."
