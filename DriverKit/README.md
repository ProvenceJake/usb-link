# DriverKit 开发说明

项目介绍、验证范围及已知限制见[仓库 README](../README.md)，来源与修改说明见 [NOTICE](../NOTICE.md)。

## 构建

用完整 Xcode 打开 `TetherKit.xcodeproj`，选择 `TetherKitDriver` scheme。命令行从仓库根目录执行：

```sh
./DriverKit/build-local-test.sh
```

该脚本仅构建和打包，不替换已安装 App，也不会重置 USB 设备。输出为 `Packages/USB-Link-v<CFBundleVersion>-Release.zip` 与对应 `Symbols.zip`。独立 dSYM 的 UUID 会与可执行文件核对，压缩包和本机签名也会校验。

脚本使用 arm64；Intel 构建和运行尚未验证。默认配置仍使用原有内部 target / executable 名称 `TetherKitLoader`，界面显示 USB Link。

## 正式签名

创建不纳入版本控制的 `Configs/LocalSigning.xcconfig`：

```xcconfig
DEVELOPMENT_TEAM = YOUR_TEAM_ID
ANDROID_RNDIS_BUNDLE_PREFIX = com.example.usblink
```

为 `.loader` 与 `.driver` 后缀的 Bundle ID 设置对应的签名和授权。DriverKit、NetworkingDriverKit 和 USB transport 权限需要 Apple 批准；如果 Apple 仅批准限定 USB 设备的权限，必须同时调整匹配配置。不能仅通过改 Bundle ID 获得授权。

- [Apple：申请 DriverKit 权限](https://developer.apple.com/documentation/driverkit/requesting-entitlements-for-driverkit-development)
- [Apple：DriverKit 本机开发示例](https://developer.apple.com/documentation/driverkit/communicating-between-a-driverkit-extension-and-a-client-app)

`build-local-test.sh` 强制使用 Ad Hoc 签名，仅适合按 Apple 文档配置的本机开发环境；正式签名应在 Xcode 中另行配置。不要把本机测试产物当作可供任意用户直接安装的正式发行包。

## 测试

在仓库根目录运行（不执行 USB 重连）：

```sh
xcodebuild -project DriverKit/TetherKit.xcodeproj \
  -scheme TetherKitDriver -configuration Debug \
  -destination 'platform=macOS,arch=arm64' \
  -derivedDataPath DriverKit/Build/Validation \
  ARCHS=arm64 ENABLE_DEBUG_DYLIB=NO CODE_SIGNING_ALLOWED=NO test
```

单元测试使用 Debug 配置，发行包使用 Release。直接以 Release 运行当前 XCTest target 的模块依赖尚未配置完成。

实机验证应独立覆盖连续重连、上传完整性、操作中关闭 App、取消时序、USB 故障与睡眠唤醒；性能对比使用相同传输量的累计 CPU 时间。运行日志、设备信息和本机验证记录不纳入公开仓库。

## 使用与卸载

完成正确签名和系统批准后，从 App 安装驱动。插入支持 RNDIS 的设备后由系统加载；关闭 App 不影响已建立的驱动连接。请保留承载系统扩展的 App。需要卸载时先使用 App 的“卸载驱动”，按系统提示完成后再删除 App。

支持匹配 RNDIS 类接口，不代表支持全部 Android 设备。ECM/NCM 设备应使用相应协议驱动。Mac 使用的是 USB 网络，设备是否连接 5G 取决于设备自身。
