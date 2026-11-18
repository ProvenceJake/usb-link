import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var manager: SystemExtensionManager
    @Environment(\.scenePhase) private var scenePhase
    @State private var showLog = false
    @State private var confirmRemoval = false
    @State private var confirmReconnect = false

    var body: some View {
        VStack(alignment: .leading, spacing: 26) {
            HStack(spacing: 16) {
                Image(nsImage: NSApplication.shared.applicationIconImage)
                    .resizable().frame(width: 70, height: 70)
                VStack(alignment: .leading, spacing: 5) {
                    Text("USB Link").font(.system(size: 28, weight: .bold, design: .rounded))
                    Text("安卓 USB 网络共享").foregroundStyle(.secondary)
                }
                Spacer()
                Button { manager.refreshStatus() } label: {
                    Image(systemName: "arrow.clockwise")
                }.buttonStyle(.borderless).help("读取系统状态").disabled(manager.isBusy)
            }
            VStack(alignment: .leading, spacing: 16) {
                HStack {
                    Image(systemName: manager.isInstalled ? "checkmark.circle.fill" : "cable.connector")
                        .foregroundStyle(manager.isInstalled ? Color.green : Color.accentColor)
                    Text(manager.driverState.badgeLabel).font(.headline)
                    Spacer()
                    if manager.isBusy { ProgressView().controlSize(.small) }
                }
                Text(manager.driverState.summary).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                Button(manager.isInstalled ? "更新驱动" : "安装驱动") { manager.installDriver() }
                    .buttonStyle(.borderedProminent).controlSize(.large)
                    .disabled(!manager.canInstall)
            }
            .padding(22).frame(maxWidth: .infinity, alignment: .leading)
            .background(.background, in: RoundedRectangle(cornerRadius: 20))
            Button {
                confirmReconnect = true
            } label: {
                Label("软件重连", systemImage: "arrow.triangle.2.circlepath")
            }
            .disabled(manager.isBusy || !manager.isInstalled)
            .help("短暂断开 USB 网络并重新加载驱动")
            if manager.isReconnecting {
                Button("取消等待") { manager.cancelReconnect() }
                    .buttonStyle(.borderless)
            }
            if !manager.reconnectStatus.isEmpty {
                Text(manager.reconnectStatus).font(.caption).foregroundStyle(.secondary)
            }
            if !manager.connectionStatus.isEmpty {
                Text(manager.connectionStatus).font(.caption).foregroundStyle(.secondary)
            }
            VStack(alignment: .leading, spacing: 14) {
                Label("连接设备后自动加载", systemImage: "bolt.horizontal")
                Label("退出 App 后驱动继续可用", systemImage: "power")
                Text("请保留应用程序中的 USB Link。移除 App 会同时移除驱动。")
                    .font(.caption).foregroundStyle(.secondary)
            }.font(.subheadline)
            DisclosureGroup("活动记录", isExpanded: $showLog) {
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 6) {
                        ForEach(manager.statusLines) { Text($0.text).textSelection(.enabled) }
                    }.frame(maxWidth: .infinity, alignment: .leading)
                }.font(.system(size: 11, design: .monospaced)).frame(height: 125).padding(.top, 10)
            }.foregroundStyle(.secondary)
            HStack {
                Text("本机测试签名").font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button("卸载驱动…", role: .destructive) { confirmRemoval = true }
                    .buttonStyle(.borderless).disabled(!manager.canUninstall || !manager.isInstalled)
            }
        }
        .padding(30).frame(width: 480)
        .background(Color(nsColor: .windowBackgroundColor))
        .onAppear { manager.refreshStatus() }
        .onChange(of: scenePhase) { phase in
            if phase == .active { manager.refreshStatus() }
        }
        .alert("重新连接 USB 设备？", isPresented: $confirmReconnect) {
            Button("取消", role: .cancel) {}
            Button("重连") {
                showLog = true
                manager.reconnectDevice()
            }
        } message: { Text("USB 网络会短暂中断。系统将重新识别设备并加载驱动。") }
        .alert("卸载 USB 网络驱动？", isPresented: $confirmRemoval) {
            Button("取消", role: .cancel) {}
            Button("卸载", role: .destructive) { manager.uninstallDriver() }
        } message: { Text("卸载后，安卓设备将无法通过此驱动使用 USB 网络共享。") }
    }
}
