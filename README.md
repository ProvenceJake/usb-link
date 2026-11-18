# USB Link

基于 [Prostec Labs TetherKit](https://github.com/prostec-labs/TetherKit) 的 macOS RNDIS USB 网络共享驱动改进版，采用 DriverKit 与 SwiftUI。重点是低占用、可控的错误处理及手动软件重连。

**实验性项目。当前主要在 Apple 芯片 Mac、macOS 15.8.3 和飞猫 M50 上验证，不保证所有 RNDIS 设备兼容。不是从零独立实现，也不隶属于上游或设备厂商。**

## 功能

- 将支持 RNDIS 的 USB 网络设备接入 macOS 网络栈。
- App 用于安装、卸载、状态读取与手动软件重连；正常关闭 App 后驱动继续工作，无额外常驻管理服务。
- 软件重连使用临时 IOKit / 网络配置通知，不进行空闲轮询；不会自动重置设备。
- 检查发送长度和短写，限制连续传输错误后的重试，取消回调后释放资源。
- 保留 RNDIS 类匹配，不限定 M50 的 USB ID；不支持将任意安卓 USB 模式自动变为 RNDIS。

## 构建与安装

需要完整 Xcode（包含 DriverKit / NetworkingDriverKit SDK），部署目标 macOS 13+。本仓库构建流程在 Apple 芯片上验证。

```sh
git clone https://github.com/changshuaicheng/usb-link.git
cd usb-link
./DriverKit/build-local-test.sh
```

脚本生成 arm64 Release、本机 Ad Hoc 签名和独立 dSYM，产物位于 `DriverKit/Packages/`。成功校验后清理构建缓存和旧候选包，保留名称含 `Rollback` 的回退包。**构建脚本不安装或激活驱动。**

**本机测试签名不等于可公开安装的正式签名。** 正常安全配置下的分发需要 Apple Developer 团队、Apple 批准的 DriverKit 权限及正确的签名配置。当前仓库不提供通用可安装的二进制发布包，不附带任何证书或私钥。不要将本项目描述为无需授权、无需安全配置即可安装的驱动。

详见 [DriverKit 构建、签名和测试说明](DriverKit/README.md)。

## 验证范围

- 20 项单元测试通过：15 项协议测试，5 项 App 状态/网络地址就绪判断测试。
- M50 上验证了软件重连、固定流量 HTTPS 下载、147456 字节上传回显一致、关闭 App 后仍可联网。
- 一次短时样本中，驱动 RSS 约 3.7 MiB，30 秒未主动发送流量时累计 CPU 增量为 0.11 秒。背景流量未隔离，该数据不代表通用性能保证。
- 未充分覆盖长时间运行、睡眠唤醒、多设备、USB 短写/取消失败/迟到回调的故障注入。
- 未证明比其他开源驱动更省资源或更稳定。

## 已知限制

- 某些 DriverKit 网卡不发布系统 Link 状态。App 此时根据有效 IPv4 地址判断“地址就绪”，不等同于公网可达或完整的驱动健康诊断。
- 系统 USB reset 是同步调用，不能强制取消。30 秒等待超时会释放观察器，但若 reset 尚未返回，App 会等它返回后完成退出。
- 如果系统拒绝取消驱动动作，为防止迟到回调访问已释放内存，会保留相关资源直到进程退出；该异常路径仍需验证。
- 不提供 ADB 解锁、设备固件修改或云控绕过功能。

## 来源与许可证

基于 Prostec Labs TetherKit，原始参考提交：[`e12c4a2fceb25a4ebdb3108f9357e0b1de148a58`](https://github.com/prostec-labs/TetherKit/tree/e12c4a2fceb25a4ebdb3108f9357e0b1de148a58)。其协议实现继承了 HoRNDIS 和 Linux RNDIS host 的工作。

本项目采用 **GPL-3.0-or-later**，保留原作者版权声明。修改说明见 [NOTICE.md](NOTICE.md)，完整许可证见 [LICENSE](LICENSE)。分发修改后的二进制时应遵守 GPL 的对应源码要求。无任何担保。
