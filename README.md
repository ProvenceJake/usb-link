# USB Link

**Android USB tethering for macOS, built with DriverKit.**

USB Link connects Android phones, mobile hotspots, and other devices exposing an **RNDIS USB network interface** to the macOS network stack. It focuses on low resource use, bounded error handling, and manual software reconnection.

This is an experimental derivative of [Prostec Labs TetherKit](https://github.com/prostec-labs/TetherKit), not an official upstream release or a device-vendor product.

## Download

Download the **arm64 DMG from this repository's Releases page**. Open the image and copy **USB Link.app** to Applications. Read the included installation notes before attempting to activate the driver.

**The preview is ad-hoc signed and is not notarized. Copying the app does not grant DriverKit authorization.** It is intended for a development Mac configured for local DriverKit testing according to Apple's documentation. It is not a general-purpose, one-click installer for a normally secured Mac. No signing certificates or private keys are distributed.

The DMG includes the corresponding source archive, license, and notices. Release notes and this README are in English; the current app interface is primarily Chinese.

## Compatibility

| Requirement | Support |
| --- | --- |
| Mac architecture | Apple Silicon / arm64 binary; Intel builds are not validated |
| macOS | Deployment target 13+; limited runtime testing on macOS 15.8.3 |
| USB protocol | RNDIS; matching is based on interface classes rather than one device's USB ID |
| Android devices | Phones and mobile hotspots that expose a supported RNDIS interface |
| Other USB modes | ECM, NCM, MTP, charging-only and ADB are not handled by this driver |

Enable USB tethering on the Android device when required and use a data-capable cable. A broad matching rule does not mean every Android device has been tested. Cellular network selection remains the device's responsibility.

## Features

- Native DriverKit network integration with a SwiftUI management app.
- Installation, removal, on-demand status checks, and user-initiated USB reconnection.
- No additional always-running management service; closing the app leaves an established driver connection available.
- Temporary device/network observers during reconnect, with no idle polling or automatic device resets.
- TX length and short-write checks, bounded consecutive-error handling, and callback cancellation before normal resource teardown.

## Build

Install full Xcode with the DriverKit and NetworkingDriverKit SDKs. Clone this repository using its **Code** button, then run from the checkout:

```sh
./DriverKit/build-local-test.sh
```

The script builds an arm64 Release app, applies a local ad-hoc signature, verifies signatures and matching dSYM UUIDs, and writes ZIP archives to `DriverKit/Packages/`. It does not install the app or activate a driver. Successful builds remove their intermediate cache and superseded candidate ZIPs while retaining rollback archives.

For normally secured distribution, use an Apple Developer team with the necessary Apple-approved DriverKit entitlements and an appropriate signing/provisioning configuration. See [build, signing, packaging, and test instructions](DriverKit/README.md).

## Validation

- 20 unit tests passed: 15 protocol tests and 5 app-state/address-readiness tests.
- Limited hardware checks covered software reconnect, a fixed-size HTTPS download, byte-for-byte upload echo integrity, and connectivity after closing the app.
- One short sample measured approximately 3.7 MiB driver RSS and 0.11 seconds of cumulative driver CPU time over 30 seconds without generated traffic. Background traffic was not isolated; these are observations, not performance guarantees.
- Long-running operation, sleep/wake, multiple devices, and injected USB short writes, cancellation failures, or late callbacks are not comprehensively validated.
- No claim is made that this project is faster, more efficient, or more stable than other drivers.

## Known limitations

- Some DriverKit interfaces do not publish a system Link state. In that case, a usable IPv4 address indicates address readiness, not Internet reachability or complete driver health.
- USB reset is a synchronous system call and cannot be forcibly cancelled. The 30-second wait deadline releases observers, but app exit may wait for an in-flight reset to return.
- If action cancellation fails, resources are retained until driver process exit to avoid a possible use-after-free. This exceptional path still needs fault-injection testing.
- Keep the host app in Applications while its system extension is installed. Use the app's driver-removal action before deleting the app.

## Attribution and license

Based on Prostec Labs TetherKit, originally imported from commit [`e12c4a2fceb25a4ebdb3108f9357e0b1de148a58`](https://github.com/prostec-labs/TetherKit/tree/e12c4a2fceb25a4ebdb3108f9357e0b1de148a58), with work inherited from HoRNDIS and the Linux RNDIS host implementation.

Licensed under **GPL-3.0-or-later**. Original copyright notices are retained. See [NOTICE.md](NOTICE.md) for modifications and [LICENSE](LICENSE) for the full license. Distributed without warranty.
