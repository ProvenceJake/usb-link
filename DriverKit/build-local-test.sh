#!/bin/bash
set -euo pipefail

cd -- "$(dirname -- "$0")"
project_dir="$PWD"
build_dir="$project_dir/Build/ReleaseLocal"
package_dir="$project_dir/Packages"
lock_dir="$project_dir/Build/.release-lock"
mkdir -p "$project_dir/Build" "$package_dir"
if ! mkdir "$lock_dir" 2>/dev/null; then
  printf '已有构建占用 %s。请先确认原构建已结束。\n' "$lock_dir" >&2
  exit 1
fi
stage=""
cleanup() {
  result=$?
  if [[ "$result" -ne 0 && -n "$stage" && -f "$stage/Release-build.log" ]]; then
    /bin/cp "$stage/Release-build.log" "$package_dir/Release-build-failed.log"
  fi
  if [[ -n "$stage" && -d "$stage" ]]; then /bin/rm -r -- "$stage"; fi
  /bin/rmdir "$lock_dir"
}
trap cleanup EXIT
stage="$(mktemp -d "$project_dir/Build/package.XXXXXXXX")"

/usr/bin/xcodebuild -project TetherKit.xcodeproj -scheme TetherKitDriver \
  -configuration Release -destination 'generic/platform=macOS' \
  -derivedDataPath "$build_dir" ARCHS=arm64 ENABLE_DEBUG_DYLIB=NO \
  CODE_SIGNING_ALLOWED=NO build 2>&1 | /usr/bin/tee "$stage/Release-build.log"

app="$stage/USB Link.app"
/usr/bin/ditto "$build_dir/Build/Products/Release/TetherKitLoader.app" "$app"
bundle_id="$(/usr/bin/plutil -extract DriverBundleIdentifier raw -o - "$app/Contents/Info.plist")"
driver="$app/Contents/Library/SystemExtensions/$bundle_id.dext"
test -d "$driver"
version="$(/usr/bin/plutil -extract CFBundleVersion raw -o - "$driver/Info.plist")"
case "$version" in
  ''|*[!0-9]*) printf '无效驱动版本：%s\n' "$version" >&2; exit 1 ;;
esac

/usr/bin/codesign --force --sign - --timestamp=none --generate-entitlement-der \
  --entitlements TetherKitDriver/TetherKitDriver.entitlements "$driver"
/usr/bin/codesign --force --sign - --timestamp=none --options runtime --generate-entitlement-der \
  --entitlements TetherKitLoader/TetherKitLoader.entitlements "$app"
/usr/bin/codesign --verify --strict "$driver"
/usr/bin/codesign --verify --deep --strict "$app"

mkdir "$stage/Symbols"
/usr/bin/ditto "$build_dir/Build/Products/Release/TetherKitLoader.app.dSYM" \
  "$stage/Symbols/TetherKitLoader.app.dSYM"
/usr/bin/ditto "$build_dir/Build/Products/Release-driverkit/$bundle_id.dext.dSYM" \
  "$stage/Symbols/$bundle_id.dext.dSYM"
for executable in "$driver/$bundle_id" "$app/Contents/MacOS/TetherKitLoader"; do
  if [[ "$executable" == "$driver/$bundle_id" ]]; then
    symbol="$stage/Symbols/$bundle_id.dext.dSYM"
  else
    symbol="$stage/Symbols/TetherKitLoader.app.dSYM"
  fi
  binary_uuid="$(xcrun dwarfdump --uuid "$executable" | awk '{print $2, $3}')"
  symbol_uuid="$(xcrun dwarfdump --uuid "$symbol" | awk '{print $2, $3}')"
  [[ -n "$binary_uuid" && "$binary_uuid" == "$symbol_uuid" ]] || exit 1
done

release="USB-Link-v$version-Release.zip"
symbols="USB-Link-v$version-Symbols.zip"
ditto -c -k --keepParent "$app" "$stage/$release"
ditto -c -k --keepParent "$stage/Symbols" "$stage/$symbols"
unzip -tq "$stage/$release"
unzip -tq "$stage/$symbols"
mv "$stage/$release" "$package_dir/$release"
mv "$stage/$symbols" "$package_dir/$symbols"
mv "$stage/Release-build.log" "$package_dir/Release-build.log"

# Remove superseded candidate artifacts only after both new archives validate.
# Installed-version rollback archives are deliberately outside these patterns.
for previous in "$package_dir"/USB-Link-v*-Release.zip "$package_dir"/USB-Link-v*-Symbols.zip; do
  [[ -f "$previous" ]] || continue
  if [[ "$previous" != "$package_dir/$release" && "$previous" != "$package_dir/$symbols" ]]; then
    /bin/rm -- "$previous"
  fi
done
/bin/rm -r -- "$build_dir"
printf '\n候选包：%s\n调试符号：%s\n构建缓存已清理。\n' "$package_dir/$release" "$package_dir/$symbols"
