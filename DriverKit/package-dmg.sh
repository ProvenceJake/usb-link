#!/bin/bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
repo_root="$PWD"
[[ -z "$(git status --porcelain --untracked-files=no)" ]] || {
  echo 'Commit tracked source changes before creating a release image.' >&2; exit 1;
}
archive="$repo_root/DriverKit/Packages/USB-Link-v7-Release.zip"
output="$repo_root/DriverKit/Packages/USB-Link-0.1.0-preview.1-arm64.dmg"
[[ -f "$archive" && ! -e "$output" ]] || {
  echo 'Input ZIP must exist and output DMG must not already exist.' >&2; exit 1;
}
mkdir -p "$repo_root/DriverKit/Build"
stage="$(mktemp -d "$repo_root/DriverKit/Build/dmg.XXXXXXXX")"
trap '/bin/rm -r -- "$stage"' EXIT
mkdir "$stage/contents"
ditto -x -k "$archive" "$stage/contents"
app="$stage/contents/USB Link.app"
driver="$app/Contents/Library/SystemExtensions/io.local.androidrndis.driver.dext"
codesign --verify --strict "$driver"
codesign --verify --deep --strict "$app"
[[ "$(plutil -extract CFBundleVersion raw -o - "$driver/Info.plist")" == 7 ]]
ln -s /Applications "$stage/contents/Applications"
cp LICENSE NOTICE.md DriverKit/INSTALL-preview.txt "$stage/contents/"
git archive --format=tar.gz --prefix=usb-link/ HEAD > "$stage/contents/Source.tar.gz"
git rev-parse HEAD > "$stage/contents/BUILD-SOURCE.txt"
hdiutil create -volname 'USB Link Preview' -srcfolder "$stage/contents" \
  -format UDZO -ov "$stage/release.dmg"
hdiutil verify "$stage/release.dmg"
mv "$stage/release.dmg" "$output"
(cd "$repo_root/DriverKit/Packages" && shasum -a 256 "$(basename "$output")" > SHA256SUMS.txt)
printf 'Created: %s\n' "$output"
