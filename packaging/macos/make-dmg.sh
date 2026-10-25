#!/bin/sh
# Builds dist/SkillHub-VERSION-macos-universal.dmg from a CMake build directory (needs Qt's macdeployqt, run on macOS).
#   packaging/macos/make-dmg.sh build 2.0.0
# The bundle holds the window (Contents/MacOS/skillhub-gui) and the command line (Contents/MacOS/skillhub); the last line
# printed is the command line's path.
# The app is signed ad hoc only (no Apple developer identity): Gatekeeper asks the user to confirm the first start.
set -eu
BUILD=$1
V=$2
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
STAGE="$ROOT/dist/dmg"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -R "$BUILD/skillhub-gui.app" "$STAGE/Skill Hub.app"
macdeployqt "$STAGE/Skill Hub.app" -qmldir="$ROOT/qml"
codesign --force --deep --sign - "$STAGE/Skill Hub.app"
cp "$ROOT/LICENSE" "$ROOT/NOTICE.md" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
hdiutil create -volname "Skill Hub $V" -srcfolder "$STAGE" -ov -format UDZO "$ROOT/dist/SkillHub-$V-macos-universal.dmg"
echo "$STAGE/Skill Hub.app/Contents/MacOS/skillhub"
