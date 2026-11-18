# 来源与修改声明

USB Link 是 Prostec Labs TetherKit 的衍生项目，不是上游官方发行版。

- 上游：https://github.com/prostec-labs/TetherKit
- 原始参考提交：e12c4a2fceb25a4ebdb3108f9357e0b1de148a58
- HoRNDIS：https://github.com/jwise/HoRNDIS
- 原始源码中的版权声明归原作者所有，包括 Joshua Wise、Mikhail Iakhiaev、David Brownell 和 Prostec Labs；各文件保留具体声明。
- 许可证：GNU GPL version 3 or (at your option) any later version。

## USB Link 修改（2026-11-18）

本公开源码是修改版本。主要修改文件/范围：

- `DriverKit/TetherKitDriver/`：网络队列接入、发送包生命周期、长度检查、批量接收提交、错误预算及停止/取消流程；本机调试日志与缓冲配置。
- `DriverKit/TetherKitLoader/`：USB Link 名称、图标与界面、状态读取、手动 USB 重连、事件通知与超时清理、操作互斥及窗口关闭行为。
- `DriverKit/TetherKitDriverTests/`：中文状态和网络地址就绪判断回归测试。
- `DriverKit/TetherKit.xcodeproj/`、`DriverKit/Configs/`：本机构建及默认占位 Bundle ID 配置。
- `DriverKit/build-local-test.sh`：Release 构建、本机签名、符号校验及产物清理。
- 仓库文档和忽略规则：公开源码使用说明、验证边界及本地资料隔离。

标识符 `io.local.androidrndis` 是本地开发占位值，不表示已注册的开发者身份。公开源码不包含 Apple 签名凭据。
