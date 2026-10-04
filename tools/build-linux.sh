#!/bin/bash
# Baut die Linux-Pakete, ein .deb und ein portables .tar.gz, und legt beide, für ein
# GitHub-Release umbenannt, zusammen mit SHA256-Prüfsummen in dist/linux ab. jpackage baut
# für das System, auf dem es läuft, daher geht das nur unter Linux; das .deb braucht
# dpkg-deb und fakeroot, die native Engine g++ und libx11-dev, das VST3-SDK kommt aus dem
# Submodul (siehe README).
#
#   ./tools/build-linux.sh
set -euo pipefail
cd "$(dirname "$0")/.."

[ "$(uname -s)" = Linux ] || { echo "Dieses Skript baut nur unter Linux." >&2; exit 1; }

VERSION="$(sed -n 's/^ *packageVersion = "\(.*\)"$/\1/p' build.gradle.kts | head -n 1)"
[ -n "$VERSION" ] || { echo "Version nicht in build.gradle.kts gefunden." >&2; exit 1; }
ARCH="$(dpkg --print-architecture 2>/dev/null || uname -m)"
OUT_DIR="dist/linux"

echo "==> Tests, DEB und portables Paket..."
./gradlew test packageDeb createDistributable

DEB="$(ls build/compose/binaries/main/deb/*.deb 2>/dev/null | head -n 1)"
APP_DIR="build/compose/binaries/main/app"
for artifact in "$DEB" "$APP_DIR/NicheLooper/bin/NicheLooper"; do
  [ -s "$artifact" ] || { echo "Build-Artefakt fehlt: $artifact" >&2; exit 1; }
done

mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR"/*
cp "$DEB" "$OUT_DIR/NicheLooper_${VERSION}_linux-${ARCH}.deb"
tar -C "$APP_DIR" -czf "$OUT_DIR/NicheLooper_${VERSION}_linux-${ARCH}.tar.gz" NicheLooper
(cd "$OUT_DIR" && sha256sum *.deb *.tar.gz > SHA256SUMS.txt)
echo "==> Fertig:"
ls -la "$OUT_DIR"
