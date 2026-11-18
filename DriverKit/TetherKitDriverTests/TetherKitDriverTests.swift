import XCTest
@testable import TetherKitLoader

final class TetherKitDriverTests: XCTestCase {
    func testAddressReadyWhenDriverKitOmitsLinkKey() {
        XCTAssertTrue(NetworkReadiness.isReady(addresses: ["192.0.2.10"], linkActive: nil))
        XCTAssertTrue(NetworkReadiness.isReady(addresses: ["192.0.2.10"], linkActive: true))
    }

    func testExplicitLinkDownOverridesStaleAddress() {
        XCTAssertFalse(NetworkReadiness.isReady(addresses: ["192.0.2.10"], linkActive: false))
    }

    func testMissingOrUnusableAddressCannotCompleteReconnect() {
        for addresses in [[], ["0.0.0.0"], ["169.254.1.2"], ["127.0.0.1"], ["999.1.2.3"], ["192.168..1"]] {
            XCTAssertFalse(NetworkReadiness.isReady(addresses: addresses, linkActive: nil))
        }
    }

    func testDriverStateBadgeLabelsMatchUIStates() {
        XCTAssertEqual(DriverState.unknown.badgeLabel, "待检查")
        XCTAssertEqual(DriverState.notInstalled.badgeLabel, "未安装")
        XCTAssertEqual(DriverState.waitingForApproval.badgeLabel, "等待授权")
        XCTAssertEqual(DriverState.installed.badgeLabel, "已启用")
        XCTAssertEqual(DriverState.pendingInstallAfterReboot.badgeLabel, "重启后启用")
        XCTAssertEqual(DriverState.pendingUninstallAfterReboot.badgeLabel, "重启后移除")
    }

    func testDriverStateSummariesRemainActionable() {
        XCTAssertTrue(DriverState.unknown.summary.contains("读取系统驱动状态"))
        XCTAssertTrue(DriverState.waitingForApproval.summary.contains("系统设置"))
        XCTAssertTrue(DriverState.pendingInstallAfterReboot.summary.contains("重启"))
        XCTAssertTrue(DriverState.installed.summary.contains("已启用"))
    }
}
