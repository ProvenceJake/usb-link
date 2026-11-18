# Attribution and modification notice

USB Link is a modified derivative of Prostec Labs TetherKit, not an official upstream distribution.

- Upstream: https://github.com/prostec-labs/TetherKit
- Original reference commit: e12c4a2fceb25a4ebdb3108f9357e0b1de148a58
- HoRNDIS: https://github.com/jwise/HoRNDIS
- Original copyright holders include Joshua Wise, Mikhail Iakhiaev, David Brownell, and Prostec Labs. Individual source files retain their applicable notices.
- License: GNU GPL version 3 or, at your option, any later version.

## USB Link modifications (2026-11-18)

This source distribution contains modifications in the following areas:

- `DriverKit/TetherKitDriver/`: network queues, TX packet lifetime, length checks, batched RX submission, bounded errors, stop/cancellation handling, local logging and buffer configuration.
- `DriverKit/TetherKitLoader/`: USB Link branding, icon and UI, status checks, manual USB reset, temporary event observers, timeout cleanup, operation exclusion and window-close behavior.
- `DriverKit/TetherKitDriverTests/`: localized state and network-address readiness regression tests.
- `DriverKit/TetherKit.xcodeproj/` and `DriverKit/Configs/`: local build configuration and placeholder bundle identifiers.
- Build and packaging scripts: Release builds, local signing, symbol verification, artifact cleanup, and preview DMG packaging.
- Documentation: English public documentation, validation limitations, installation notes and local-artifact exclusions.

`io.local.androidrndis` is a local development placeholder, not a registered developer identity. Signing credentials are not included.
