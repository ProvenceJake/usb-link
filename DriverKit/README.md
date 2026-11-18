# Development guide

See the [project README](../README.md) for compatibility and limitations, and [NOTICE](../NOTICE.md) for provenance.

## Build a local preview

Open `TetherKit.xcodeproj` in full Xcode and select the `TetherKitDriver` scheme, or run from the repository root:

```sh
./DriverKit/build-local-test.sh
```

This produces `Packages/USB-Link-v<driver-build>-Release.zip` and a separate symbols archive. The script verifies local signatures, executable/dSYM UUID pairs, and archive integrity. It does not replace an installed app or reset a device.

Only arm64 builds have been validated. Internal target/executable names remain `TetherKitLoader`; the displayed product name is USB Link.

## Signing and installation

The local script deliberately applies ad-hoc signatures. These are not Developer ID signatures, and the preview is not notarized. A DMG is a container, not a way to bypass system-extension authorization.

For development-machine setup, follow [Apple's DriverKit local development example](https://developer.apple.com/documentation/driverkit/communicating-between-a-driverkit-extension-and-a-client-app). Do not assume this build will activate with ordinary security settings on another Mac.

For appropriately provisioned builds, create the ignored file `Configs/LocalSigning.xcconfig`:

```xcconfig
DEVELOPMENT_TEAM = YOUR_TEAM_ID
ANDROID_RNDIS_BUNDLE_PREFIX = com.example.usblink
```

Configure matching signing identities and provisioning for the `.loader` and `.driver` bundle identifiers in Xcode. DriverKit, NetworkingDriverKit and USB transport entitlements need Apple approval. If approval is restricted to specific devices, adjust the matching configuration and entitlements accordingly. Changing a bundle identifier does not grant these permissions.

See [Apple: requesting DriverKit entitlements](https://developer.apple.com/documentation/driverkit/requesting-entitlements-for-driverkit-development). Use Xcode's signing configuration for provisioned builds; `build-local-test.sh` is only the local test path.

## Package a DMG

Commit the source tree first, then run:

```sh
./DriverKit/package-dmg.sh
```

This packages the previously built version-7 ZIP with an Applications shortcut, English installation notes, the license and modification notice, and a source archive of HEAD. Only use this script when the ZIP was built from the same driver/app sources as HEAD. It verifies the enclosed app and driver signatures and the DMG checksum. It does not notarize or install anything. The version-7 preview retains app version 1.0/build 1 and driver version 1.0/build 7; the GitHub release tag identifies the distribution revision.

## Tests

From the repository root:

```sh
xcodebuild -project DriverKit/TetherKit.xcodeproj \
  -scheme TetherKitDriver -configuration Debug \
  -destination 'platform=macOS,arch=arm64' \
  -derivedDataPath DriverKit/Build/Validation \
  ARCHS=arm64 ENABLE_DEBUG_DYLIB=NO CODE_SIGNING_ALLOWED=NO test
```

Unit tests use Debug; distributable app builds use Release. Direct Release XCTest module dependencies are not currently configured. Unit tests do not perform hardware reconnects.

Hardware validation should separately cover repeated reconnects, upload integrity, app closure during operations, cancellation timing, USB failures, and sleep/wake. Compare cumulative CPU time for equal transfer sizes rather than isolated CPU peaks. Keep personal logs and signing configuration out of published source.

## Removal

Use the app's driver-removal action, complete any system prompts, then remove the host app. Keep the host app installed while using the extension.
