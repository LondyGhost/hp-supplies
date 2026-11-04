hp-supplies 1.2.1 — HP 打印机耗材监控（Windows 32 位版，Win7 SP1+ 通吃）
========================================================================

 适用系统
--------------
  Windows 7 SP1 / 8 / 8.1 / 10 / 11 —— 32 位与 64 位均可运行
  （32 位程序在 64 位系统上经 WoW64 直接运行）

  无需安装任何运行库：不依赖 UCRT / .NET / GTK，解压即用。

 contents 说明
--------------
  hp-supplies.exe        单文件: 双击 = 图形界面（自动隐藏黑窗口）;
                         终端里带参数 = 命令行模式

 使用
--------------
  图形界面 : 双击 hp-supplies.exe
  命令行   : hp-supplies.exe --text        耗材余量/总页数/状态/告警
             hp-supplies.exe --get /DevMgmt/ProductStatusDyn.xml   调试原始报文
             hp-supplies.exe --host 打印机IP   走网络 EWS（打印机联网时）

 USB 驱动要求
--------------
  本程序通过 libusb 访问打印机 USB 复合设备上的 "HP LEDM" 接口
  (class 0xFF / subclass 0x04 / protocol 0x01)，要求该接口绑定
  WinUSB 驱动：
  - 装过 HP 官方"打印机助手"(HP ColorLaserJet M153-M154.exe) 的电脑
    已自带 WinUSB 绑定，无需任何操作；
  - 纯净系统（含全部 Win7/8 系统）用 Zadig (https://zadig.akeo.ie)
    在列表中选择 "HP LEDM" 接口，驱动选 WinUSB，点 Install 一次即可。
  （Zadig 在 Win7 上如无法运行，请先打 SP1 与 KB3033929 补丁。）

 已知限制
--------------
  - 打印机插在本机 USB 口时查询；无 LEDM 接口的老型号不支持
  - 界面为 Windows 原生控件（此 32 位版不再依赖 GTK3）

 许可：MIT。libusb 1.0.30 以 LGPL-2.1 许可静态链接进本程序，
 如需以不同运行库重新链接，请从源码自行构建（见项目仓库）。
