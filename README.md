# hp-supplies — HP 打印机耗材监控（Linux）

在 Linux 上查看 HP 打印机的 **碳粉/墨水余量**、**耗材状态**、**总打印页数**，
以及 **打印机状态与活动提醒**（卡纸、仓门未关、缺纸、打印机错误、硒鼓告警等）。
功能对等于 Windows 上 HP 打印机助手的"设备/耗材"页面
（`HP ColorLaserJet M153-M154.exe -Start UDCDevicePage`），不依赖 HPLIP
（HPLIP 对很多新型号读不到页数与碳粉百分比）。

## 适用范围

协议逆向自 HP 的 **LEDM over USB**（Lightweight Embedded Device Management）
管理通道，这是 HP 一整代打印机的通用标准接口。程序启动时自动扫描 USB 总线
上的打印机，凡是带 **"HP LEDM"** USB 接口（class `0xFF` / subclass `0x04` /
protocol `0x01`）的 HP 设备都可以读取，包括较新的：

- InkJet / DeskJet / OfficeJet 系列
- LaserJet / Color LaserJet 系列（M153/M154、M232 等平台已验证）
- Smart Tank / Tank 系列

老型号（没有 LEDM 接口的）会提示"未找到带 LEDM 接口的 USB 打印机"，
此时可尝试 `--vid/--pid` 手动指定（接口签名匹配失败则确实不支持）。
联网的打印机也可以走网络 EWS：`--host 打印机IP`。

## 原理

打印机的 USB 描述符是一个复合设备，其中一个 vendor 接口（字符串描述符
**"HP LEDM"**，bulk OUT/IN 一对）是一条 **HTTP over USB** 隧道：

1. 向 bulk OUT 端点写入标准 HTTP/1.1 请求（必须带 `Host: localhost` 头）；
2. 从 bulk IN 端点读回 HTTP 响应；
3. 打印机固件内置的 LEDM Web 服务返回设备管理 XML：

| 资源 | 内容 |
|---|---|
| `/DevMgmt/ConsumableConfigDyn.xml` | 每个耗材：颜色、型号、序列号、剩余百分比、状态、品牌(原装/兼容)、装机日期、标称页数 |
| `/DevMgmt/ProductUsageDyn.xml` | `TotalImpressions` 总打印页数、黑白/彩色页数、卡纸次数、每个耗材的已打印页数与预计剩余页数 |
| `/DevMgmt/ProductStatusDyn.xml` | 整机状态（`pscat:StatusCategory`，含固件自带中文描述）+ 活动告警表（卡纸 / 仓门未关 / 缺纸 / 打印机错误 / 硒鼓告警，含严重级别与颜色） |

Windows 的 HP 工具通过 WinUSB 走同一条通道；本程序在 Linux 上用 libusb 实现。

注意事项：
- 请求必须带 `Host: localhost`（`Host: 其它值` 会得到 400，无 Host 头会无响应）。
- 建议 `Connection: close`，否则固件会在接口上保持 HTTP 会话并把旧响应排队。
- 固件偶发吐出残留队列里的旧/截断响应，程序会校验 XML 根元素与闭合标签并自动重试。

## 安装

### deb (Ubuntu/Debian/Deepin/UOS)
```bash
sudo dpkg -i hp-supplies-1.2.1-amd64.deb
```

### rpm (Fedora/openEuler/openSUSE)
```bash
sudo rpm -ivh hp-supplies-1.2.1-1*.rpm
```

### AppImage (任意发行版)
```bash
chmod +x hp-supplies-1.2.1-x86_64.AppImage
./hp-supplies-1.2.1-x86_64.AppImage
```
AppImage 依赖系统 GTK3（主流桌面发行版自带）。

安装包同时安装 udev 规则 `/usr/lib/udev/rules.d/60-hp-supplies.rules`
（放行所有 HP/03f0 USB 设备给普通用户），安装后重新插拔一次 USB 线即可生效。
如需收紧权限，可在规则里加 `ATTRS{idProduct}=="xxxx"` 限定具体型号。

## 使用

```bash
hp-supplies                          # GUI（自动扫描打印机）
hp-supplies --text                   # 终端输出
hp-supplies --host 192.168.1.xxx       # 走网络 EWS（打印机联网时）
hp-supplies --vid 03f0 --pid ae2a    # 指定 USB VID/PID（多台打印机时）
hp-supplies --get /DevMgmt/ProductStatusDyn.xml   # 调试: 原始 LEDM 路径输出
hp-supplies --version
```

退出码：查询成功 0，失败 1（错误信息在 stderr），方便脚本/cron 做耗材告警。

`--text` 输出示例（真实设备 HP ColorLaserJet M154a）：
```
打印机 : HP ColorLaserJet M153-M154  序列号 VNCNRXXXXXX
状态   : 已打开睡眠模式。
== 活动提醒 ==
  [严重警告] 黄色 硒鼓余量极低
  [警告] 青色 硒鼓余量低
  [警告] 黑色 硒鼓已用尽
  [提示] 黄色 原装 HP 硒鼓
== 用量统计 ==
总打印页数 : 13976  (黑白 123 / 彩色 13853)
卡纸 12 次  进纸失败 8 次
== 耗材 ==
黑色 [CF510A] 余量 —    状态 used     本支已打印 743 页  预计剩余 850 页  标称 1100 页  品牌 clone  装机 2026-08-16  S/N 16980275
青色 [CF511A] 余量 1%   状态 low      本支已打印 599 页  预计剩余 50 页   标称 900 页   品牌 clone  装机 2026-03-09  S/N 1698XXXX
品红 [CF513A] 余量 10%  状态 ok       本支已打印 599 页  预计剩余 100 页  标称 900 页   品牌 clone  装机 2026-03-09  S/N 8434XXXX
黄色 [CF512A] 余量 0%   状态 veryLow  本支已打印 771 页  预计剩余 0 页    标称 700 页   品牌 HP     装机 2026-05-13  S/N 1010XXXXX
```

状态行优先显示固件自带的中文描述（卡纸/仓门未关/错误码等会直接给出
官方提示文本），活动提醒按 **错误 > 严重警告 > 警告 > 提示** 分级；
GUI 顶部状态行同样着色显示（绿=正常 / 橙=提醒 / 红=错误）。

## Windows 版

`dist/hp-supplies-1.2.1-windows-x64.zip`：解压即用，含 GUI 与命令行
（`hp-supplies-gui.exe` / `hp-supplies.exe`，GTK3 运行库已随包附带）。

USB 驱动前提：LEDM 接口需绑定为 WinUSB 驱动——装过 HP 官方打印机助手
的电脑已自带；纯净系统用 [Zadig](https://zadig.akeo.ie) 给 "HP LEDM"
接口绑定 WinUSB 即可。

从源码构建见 `packaging/windows/build-msys2.sh`（MSYS2 UCRT64 环境，
依赖 gtk3 / libusb / pkgconf 包）。

## 从源码构建

```bash
sudo apt install build-essential libgtk-3-dev libusb-1.0-dev   # Debian 系
# sudo dnf install gcc make gtk3-devel libusb1-devel            # Fedora 系
make
sudo make install
```

打包三种格式：`bash packaging/build-all.sh`（产物在 `out/`）。

## 法律声明

- 本项目为**独立开发的互操作实现**，与 HP Inc. 无任何关联、合作或背书关系
- HP、Hewlett-Packard 及 ColorLaserJet 等为 HP Inc. 的商标，本项目中仅作描述性引用（指示用途），不使用任何 HP 标志/Logo
- 项目**不包含、不复制、不分发**任何 HP 软件代码或资源；协议信息来自公开资料与对自有设备的接口行为观察
- 本工具**只读**：仅查询设备状态，不写入、不修改固件、不绕过任何原装耗材认证（HP+）或数字版权保护机制
- 本软件按 MIT 许可证提供，无任何担保；仅供个人设备管理与互操作使用
- 详细法律分析与合规措施见 [docs/LEGAL.md](docs/LEGAL.md)

## 许可

MIT
