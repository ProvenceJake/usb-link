// SystemExtensionManager.swift
// Manages activation and deactivation of the TetherKitDriver system extension.
// TetherKit Loader
//
//   Copyright (c) 2012 Joshua Wise.
//   Copyright (c) 2018 Mikhail Iakhiaev
//
// Ported to DriverKit by Prostec Labs, 2026.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.

import Foundation
import IOKit
import IOUSBHost
import SystemConfiguration
import AppKit
import SystemExtensions
import os.log

/// A single timestamped line in the activity log.
struct LogLine: Identifiable {
    let id: Int
    let text: String
}

enum DriverState: Equatable {
    case unknown
    case notInstalled
    case waitingForApproval
    case installed
    case pendingInstallAfterReboot
    case pendingUninstallAfterReboot

    var badgeLabel: String {
        switch self {
        case .unknown:
            return "待检查"
        case .notInstalled:
            return "未安装"
        case .waitingForApproval:
            return "等待授权"
        case .installed:
            return "已启用"
        case .pendingInstallAfterReboot:
            return "重启后启用"
        case .pendingUninstallAfterReboot:
            return "重启后移除"
        }
    }

    var summary: String {
        switch self {
        case .unknown:
            return "正在读取系统驱动状态。"
        case .notInstalled:
            return "安装驱动后，即可连接支持 RNDIS 的安卓设备。"
        case .waitingForApproval:
            return "请在系统设置中批准驱动。"
        case .installed:
            return "驱动已启用，连接设备后由系统按需加载。"
        case .pendingInstallAfterReboot:
            return "重启后完成安装。"
        case .pendingUninstallAfterReboot:
            return "重启后完成卸载。"
        }
    }
}

/// Manages the lifecycle of the TetherKitDriver DriverKit system extension.
///
/// This class implements `OSSystemExtensionRequestDelegate` and posts status
/// updates via `@Published` properties so SwiftUI views can react to changes.
///
/// Deployment model (mirrors the ASIX/Plugable AX88179 pattern):
///   - The .dext bundle lives inside this app at
///       Contents/Library/SystemExtensions/<driver bundle ID>.dext
///   - On "Install Driver", we submit an `OSSystemExtensionRequest` for
///       activation. macOS copies the dext to the system location and prompts
///       the user in System Settings -> Privacy & Security.
///   - This loader app must remain installed (though not necessarily running)
///       for macOS to keep the dext active. Uninstalling this app silently
///       deactivates the dext on next reboot.
///   - On "Uninstall Driver", we submit a deactivation request.
final class SystemExtensionManager: NSObject, ObservableObject {

    // MARK: - Public state

    /// Timestamped log lines shown in the UI activity log.
    @Published var statusLines: [LogLine] = [LogLine(id: 0, text: "Ready.")]

    /// Whether an install or uninstall request is in flight.
    @Published var isBusy: Bool = false {
        didSet {
            if !isBusy { NotificationCenter.default.post(name: .usbLinkOperationFinished, object: self) }
        }
    }
    @Published private(set) var reconnectStatus = ""
    @Published private(set) var connectionStatus = ""
    private var reconnectOperation: USBReconnectOperation?
    var isReconnecting: Bool { reconnectOperation != nil }
    private var currentRequest: OSSystemExtensionRequest?
    private var requestDeadline: DispatchWorkItem?

    /// Current user-visible state of the system extension.
    @Published private(set) var driverState: DriverState = .unknown

    // MARK: - Private

    private let log = Logger(subsystem: Bundle.main.bundleIdentifier ?? "io.local.androidrndis.loader",
                             category: "SystemExtensionManager")

    /// Bundle identifier of the dext - must match IOUserServerName in Info.plist
    /// and the embedded dext bundle at Contents/Library/SystemExtensions/.
    static let driverIdentifier = Bundle.main.object(
        forInfoDictionaryKey: "DriverBundleIdentifier"
    ) as? String ?? ""

    private enum PendingOperation { case install, uninstall }
    private var pendingOperation: PendingOperation?
    private var stateBeforeRequest: DriverState = .unknown
    private var nextLineId: Int = 1

    var isInstalled: Bool {
        driverState == .installed || driverState == .pendingUninstallAfterReboot
    }

    var canInstall: Bool {
        !isBusy && driverState != .waitingForApproval
            && driverState != .pendingInstallAfterReboot
    }

    var canUninstall: Bool {
        !isBusy && driverState != .notInstalled && driverState != .pendingUninstallAfterReboot
    }

    // MARK: - Public API

    private func submit(_ request: OSSystemExtensionRequest) {
        dispatchPrecondition(condition: .onQueue(.main))
        currentRequest = request
        let deadline = DispatchWorkItem { [weak self, weak request] in
            guard let self, let request, self.currentRequest === request else { return }
            self.currentRequest = nil
            self.requestDeadline = nil
            self.pendingOperation = nil
            self.driverState = .unknown
            self.appendStatus("系统请求等待超过 30 秒。授权或安装可能仍由 macOS 处理，请稍后刷新状态。")
            self.isBusy = false
        }
        requestDeadline = deadline
        DispatchQueue.main.asyncAfter(deadline: .now() + 30, execute: deadline)
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    private func finishRequest(_ request: OSSystemExtensionRequest) -> Bool {
        dispatchPrecondition(condition: .onQueue(.main))
        guard currentRequest === request else { return false }
        requestDeadline?.cancel()
        requestDeadline = nil
        currentRequest = nil
        return true
    }

    /// Clear the activity log.
    func clearLog() {
        let line = LogLine(id: nextLineId, text: "Log cleared.")
        nextLineId += 1
        statusLines = [line]
    }

    /// Submit an activation (install) request for the driver extension.
    func installDriver() {
        guard !isBusy else { return }
        guard !Self.driverIdentifier.isEmpty else {
            appendStatus("缺少驱动标识。请检查 Xcode 构建配置。")
            return
        }
        isBusy = true
        pendingOperation = .install
        stateBeforeRequest = driverState
        driverState = .waitingForApproval
        appendStatus("Requesting driver activation...")
        log.info("Submitting activation request for \(Self.driverIdentifier)")

        let request = OSSystemExtensionRequest.activationRequest(
            forExtensionWithIdentifier: Self.driverIdentifier,
            queue: .main)
        request.delegate = self
        submit(request)
    }

    /// Submit a deactivation (uninstall) request for the driver extension.
    func uninstallDriver() {
        guard !isBusy else { return }
        guard !Self.driverIdentifier.isEmpty else {
            appendStatus("缺少驱动标识。请检查 Xcode 构建配置。")
            return
        }
        isBusy = true
        pendingOperation = .uninstall
        stateBeforeRequest = driverState
        appendStatus("Requesting driver deactivation...")
        log.info("Submitting deactivation request for \(Self.driverIdentifier)")

        let request = OSSystemExtensionRequest.deactivationRequest(
            forExtensionWithIdentifier: Self.driverIdentifier,
            queue: .main)
        request.delegate = self
        submit(request)
    }

    /// Query macOS only on demand, without a polling timer.
    func refreshStatus() {
        guard !isBusy, !Self.driverIdentifier.isEmpty else { return }
        isBusy = true
        let request = OSSystemExtensionRequest.propertiesRequest(
            forExtensionWithIdentifier: Self.driverIdentifier, queue: .main)
        request.delegate = self
        submit(request)
    }

    func request(_ request: OSSystemExtensionRequest,
                 foundProperties properties: [OSSystemExtensionProperties]) {
        guard finishRequest(request) else { return }
        isBusy = false
        if let current = properties.first(where: { $0.isEnabled }) {
            driverState = current.isUninstalling ? .pendingUninstallAfterReboot : .installed
        } else {
            driverState = properties.isEmpty ? .notInstalled : .waitingForApproval
        }
        updateConnectionStatus()
        appendStatus(driverState.summary)
    }

    func reconnectDevice() {
        dispatchPrecondition(condition: .onQueue(.main))
        guard !isBusy else { return }
        isBusy = true
        let operation = USBReconnectOperation(
            progress: { [weak self] text in self?.reconnectStatus = text },
            completion: { [weak self] text in
                guard let self else { return }
                self.reconnectStatus = text
                self.appendStatus(text)
                self.reconnectOperation = nil
                self.updateConnectionStatus()
                self.isBusy = false
            })
        reconnectOperation = operation
        operation.start()
    }

    func cancelReconnect() {
        reconnectOperation?.cancel()
    }

    private func updateConnectionStatus() {
        let devices = Self.connectedDevices()
        defer { devices.forEach { IOObjectRelease($0) } }
        guard !devices.isEmpty else { connectionStatus = "未连接 USB 网络设备"; return }
        let interfaces = devices.compactMap { USBReconnectOperation.interfaceName($0) }
        let states = interfaces.map { USBReconnectOperation.networkState($0) }
        if states.contains(where: { $0.ready }) {
            connectionStatus = "USB 网络地址已就绪"
        } else if states.contains(where: { $0.linkDown }) {
            connectionStatus = "USB 链路未就绪；若持续无法联网，请点击软件重连"
        } else {
            connectionStatus = "驱动已接管，等待网络地址"
        }
    }

    fileprivate static func connectedDevices() -> [io_service_t] {
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault,
              IOServiceMatching("IOUserNetworkEthernet"), &iterator) == KERN_SUCCESS else { return [] }
        defer { IOObjectRelease(iterator) }
        var devices: [io_service_t] = []
        var seen = Set<UInt64>()
        while true {
            let driver = IOIteratorNext(iterator)
            if driver == 0 { break }
            defer { IOObjectRelease(driver) }
            let identifier = IORegistryEntryCreateCFProperty(
                driver, "IOUserServerName" as CFString, nil, 0)?.takeRetainedValue() as? String
            guard identifier == driverIdentifier else { continue }
            var parent: io_registry_entry_t = 0
            guard IORegistryEntryGetParentEntry(driver, kIOServicePlane, &parent) == KERN_SUCCESS else { continue }
            while parent != 0 {
                if IOObjectConformsTo(parent, "IOUSBHostDevice") != 0 {
                    var id: UInt64 = 0
                    IORegistryEntryGetRegistryEntryID(parent, &id)
                    if seen.insert(id).inserted { devices.append(parent) }
                    else { IOObjectRelease(parent) }
                    break
                }
                var next: io_registry_entry_t = 0
                let result = IORegistryEntryGetParentEntry(parent, kIOServicePlane, &next)
                IOObjectRelease(parent)
                parent = result == KERN_SUCCESS ? next : 0
            }
        }
        return devices
    }

    // MARK: - Helpers

    private func appendStatus(_ message: String) {
        let timestamp = DateFormatter.localizedString(from: Date(),
                                                     dateStyle: .none,
                                                     timeStyle: .medium)
        let line = LogLine(id: nextLineId, text: "[\(timestamp)] \(message)")
        nextLineId += 1
        dispatchPrecondition(condition: .onQueue(.main))
        statusLines.append(line)
        if statusLines.count > 200 { statusLines.removeFirst(statusLines.count - 200) }
        log.info("\(message)")
    }
}

// MARK: - OSSystemExtensionRequestDelegate

extension SystemExtensionManager: OSSystemExtensionRequestDelegate {

    func request(_ request: OSSystemExtensionRequest,
                 actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties)
    -> OSSystemExtensionRequest.ReplacementAction
    {
        guard currentRequest === request else { return .cancel }
        // Replace with the version explicitly requested by this installation.
        appendStatus("Replacing existing driver v\(existing.bundleShortVersion) "
                   + "with v\(ext.bundleShortVersion).")
        return .replace
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        guard currentRequest === request else { return }
        driverState = .waitingForApproval
        appendStatus("请按 macOS 提示批准驱动。")
        if #available(macOS 15, *) {
            appendStatus("在「系统设置 → 通用 → 登录项与扩展 → 驱动程序」中启用此驱动。")
        } else {
            appendStatus("在「系统设置 → 隐私与安全性」中允许此驱动。")
        }
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFinishWithResult result: OSSystemExtensionRequest.Result)
    {
        guard finishRequest(request) else { return }
        defer {
            pendingOperation = nil
            stateBeforeRequest = driverState
        }
        isBusy = false
        switch result {
        case .completed:
            if pendingOperation == .install {
                driverState = .installed
                appendStatus("Driver installed successfully. "
                           + "Connect an Android phone and enable USB tethering.")
            } else {
                driverState = .notInstalled
                appendStatus("Driver uninstalled. Reconnect the phone to verify.")
            }

        case .willCompleteAfterReboot:
            if pendingOperation == .install {
                driverState = .pendingInstallAfterReboot
            } else {
                driverState = .pendingUninstallAfterReboot
            }
            appendStatus("Change will take effect after reboot.")

        @unknown default:
            driverState = .unknown
            appendStatus("Request completed with unknown result.")
        }
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFailWithError error: Error)
    {
        guard finishRequest(request) else { return }
        defer {
            pendingOperation = nil
            stateBeforeRequest = driverState
        }
        isBusy = false
        if case .waitingForApproval = driverState {
            driverState = stateBeforeRequest
        }
        let nsErr = error as NSError
        appendStatus("Request failed: \(nsErr.localizedDescription) "
                   + "(domain=\(nsErr.domain) code=\(nsErr.code))")

        if nsErr.domain == OSSystemExtensionErrorDomain {
            switch OSSystemExtensionError.Code(rawValue: nsErr.code) {
            case .unsupportedParentBundleLocation:
                appendStatus("Hint: the app must be run from /Applications - "
                           + "move it out of Downloads / DMG and try again.")
            case .extensionNotFound:
                appendStatus("Hint: the .dext bundle was not found inside "
                           + "this app. Re-install the application.")
            case .authorizationRequired:
                appendStatus("Hint: re-run with administrator privileges, or "
                           + "approve the extension in System Settings.")
            case .requestSuperseded:
                appendStatus("Hint: a previous identical request is already pending.")
            default:
                break
            }
        }
        log.error("Extension request failed: \(error.localizedDescription)")
    }
}


// Observers exist only during a requested reconnect operation.
extension Notification.Name {
    static let usbLinkOperationFinished = Notification.Name("USBLinkOperationFinished")
}

private final class USBReconnectOperation {
    private struct Identity: Equatable {
        let location: NSNumber
        let vendor: NSNumber
        let product: NSNumber
        let serial: String?

        init?(_ service: io_service_t) {
            func property(_ key: String) -> Any? {
                IORegistryEntryCreateCFProperty(service, key as CFString, nil, 0)?.takeRetainedValue()
            }
            guard let location = property("locationID") as? NSNumber,
                  let vendor = property("idVendor") as? NSNumber,
                  let product = property("idProduct") as? NSNumber else { return nil }
            self.location = location
            self.vendor = vendor
            self.product = product
            self.serial = property("USB Serial Number") as? String
        }
    }

    private var identity: Identity?
    private var oldID: UInt64 = 0
    private var notificationPort: IONotificationPortRef?
    private var iterators: [io_iterator_t] = []
    private var store: SCDynamicStore?
    private var timeout: DispatchWorkItem?
    private var active = false
    private var resetInFlight = false
    private var deferredResult: String?
    private var resetFinished = false
    private let progress: (String) -> Void
    private let completion: (String) -> Void

    init(progress: @escaping (String) -> Void, completion: @escaping (String) -> Void) {
        self.progress = progress
        self.completion = completion
    }

    func start() {
        dispatchPrecondition(condition: .onQueue(.main))
        active = true
        let devices = SystemExtensionManager.connectedDevices()
        defer { devices.forEach { IOObjectRelease($0) } }
        guard devices.count == 1 else {
            finish(devices.isEmpty ? "未找到本驱动管理的设备" : "请只连接一台待重连的 USB 网络设备")
            return
        }
        let service = devices[0]
        guard let identity = Identity(service),
              IORegistryEntryGetRegistryEntryID(service, &oldID) == KERN_SUCCESS else {
            finish("无法确认设备身份，已取消重连")
            return
        }
        self.identity = identity
        guard installObservers() else {
            finish("无法监听设备和网络状态，已取消重连")
            return
        }
        let deadline = DispatchWorkItem { [weak self] in
            self?.finish("30 秒内未恢复网络，请检查设备供电和驱动授权")
        }
        timeout = deadline
        DispatchQueue.main.asyncAfter(deadline: .now() + 30, execute: deadline)
        progress("正在重置")
        resetInFlight = true
        IOObjectRetain(service)
        DispatchQueue.global(qos: .utility).async {
            let result: String?
            do {
                let device = try IOUSBHostDevice(__ioService: service, options: [],
                                                queue: nil, interestHandler: nil)
                defer { device.destroy() }
                try device.reset()
                result = nil
            } catch { result = "重连失败：\(error.localizedDescription)" }
            IOObjectRelease(service)
            DispatchQueue.main.async {
                self.resetInFlight = false
                self.resetFinished = true
                if let deferred = self.deferredResult { self.finish(deferred); return }
                if let result { self.finish(result); return }
                self.evaluate()
            }
        }
    }

    func cancel() { finish("已取消等待；已提交的 USB 重置由系统完成") }

    private func installObservers() -> Bool {
        guard let port = IONotificationPortCreate(kIOMainPortDefault) else { return false }
        notificationPort = port
        IONotificationPortSetDispatchQueue(port, .main)
        let context = Unmanaged.passUnretained(self).toOpaque()
        for className in ["IOUSBHostDevice", "IOUserNetworkEthernet"] {
            var iterator: io_iterator_t = 0
            let result = IOServiceAddMatchingNotification(
                port, kIOFirstMatchNotification, IOServiceMatching(className),
                { context, iterator in
                    while true {
                        let item = IOIteratorNext(iterator)
                        if item == 0 { break }
                        IOObjectRelease(item)
                    }
                    guard let context else { return }
                    Unmanaged<USBReconnectOperation>.fromOpaque(context).takeUnretainedValue().evaluate()
                }, context, &iterator)
            guard result == KERN_SUCCESS else { return false }
            iterators.append(iterator)
            while true {
                let item = IOIteratorNext(iterator)
                if item == 0 { break }
                IOObjectRelease(item)
            }
        }
        var storeContext = SCDynamicStoreContext(version: 0, info: context,
                                                retain: nil, release: nil, copyDescription: nil)
        store = SCDynamicStoreCreate(nil, "USBLinkReconnect" as CFString, { _, _, context in
            guard let context else { return }
            Unmanaged<USBReconnectOperation>.fromOpaque(context).takeUnretainedValue().evaluate()
        }, &storeContext)
        guard let store,
              SCDynamicStoreSetNotificationKeys(store, nil,
                ["State:/Network/Interface/.*/IPv4", "State:/Network/Interface/.*/Link"] as CFArray),
              SCDynamicStoreSetDispatchQueue(store, .main) else { return false }
        return true
    }

    private func evaluate() {
        guard active, resetFinished else { return }
        let devices = SystemExtensionManager.connectedDevices()
        defer { devices.forEach { IOObjectRelease($0) } }
        for device in devices where Identity(device) == identity {
            var newID: UInt64 = 0
            guard IORegistryEntryGetRegistryEntryID(device, &newID) == KERN_SUCCESS,
                  newID != oldID else { continue }
            progress("等待网络地址")
            if let name = Self.interfaceName(device), Self.networkState(name).ready {
                finish("重连完成，USB 驱动和网络地址均已恢复")
            }
            return
        }
        progress("等待驱动")
    }

    fileprivate static func interfaceName(_ device: io_service_t) -> String? {
        var drivers: io_iterator_t = 0
        guard IORegistryEntryCreateIterator(device, kIOServicePlane,
            IOOptionBits(kIORegistryIterateRecursively), &drivers) == KERN_SUCCESS else { return nil }
        defer { IOObjectRelease(drivers) }
        while true {
            let driver = IOIteratorNext(drivers)
            if driver == 0 { break }
            defer { IOObjectRelease(driver) }
            let identifier = IORegistryEntryCreateCFProperty(
                driver, "IOUserServerName" as CFString, nil, 0)?.takeRetainedValue() as? String
            guard identifier == SystemExtensionManager.driverIdentifier else { continue }
            var interfaces: io_iterator_t = 0
            guard IORegistryEntryCreateIterator(driver, kIOServicePlane,
                IOOptionBits(kIORegistryIterateRecursively), &interfaces) == KERN_SUCCESS else { continue }
            defer { IOObjectRelease(interfaces) }
            while true {
                let entry = IOIteratorNext(interfaces)
                if entry == 0 { break }
                let name = IORegistryEntryCreateCFProperty(entry, "BSD Name" as CFString, nil, 0)?
                    .takeRetainedValue() as? String
                IOObjectRelease(entry)
                if let name, name.hasPrefix("en") { return name }
            }
        }
        return nil
    }

    fileprivate static func networkState(_ name: String) -> (ready: Bool, linkDown: Bool) {
        let ipv4 = SCDynamicStoreCopyValue(nil, "State:/Network/Interface/\(name)/IPv4" as CFString)
            as? [String: Any]
        let link = SCDynamicStoreCopyValue(nil, "State:/Network/Interface/\(name)/Link" as CFString)
            as? [String: Any]
        let down = (link?["Active"] as? Bool) == false
        let addresses = ipv4?["Addresses"] as? [String] ?? []
        return (NetworkReadiness.isReady(addresses: addresses,
                                        linkActive: link?["Active"] as? Bool), down)
    }

    private func finish(_ message: String) {
        guard active else { return }
        timeout?.cancel()
        timeout = nil
        if let store { SCDynamicStoreSetDispatchQueue(store, nil) }
        store = nil
        if let notificationPort { IONotificationPortDestroy(notificationPort) }
        notificationPort = nil
        iterators.forEach { IOObjectRelease($0) }
        iterators.removeAll()
        // reset() is synchronous and cannot be cancelled. Keep the operation alive
        // until it returns, even if the UI timeout or cancel has already fired.
        if resetInFlight {
            deferredResult = message
            progress(message)
            return
        }
        active = false
        completion(message)
    }
}

enum NetworkReadiness {
    static func isReady(addresses: [String], linkActive: Bool?) -> Bool {
        // DriverKit interfaces may omit the SC Link key even while transferring.
        // An explicit link-down still overrides an address left in the store.
        guard linkActive != false else { return false }
        return addresses.contains { address in
            let parts = address.split(separator: ".", omittingEmptySubsequences: false)
            guard parts.count == 4 else { return false }
            let octets = parts.compactMap { UInt8($0) }
            return octets.count == 4 && octets[0] != 0 && octets[0] != 127 &&
                octets[0] < 224 && !(octets[0] == 169 && octets[1] == 254)
        }
    }
}
