# HP LEDM over USB 协议笔记（独立观察记录）

> 本文档是对自购 HP 打印机 USB 接口**公开行为**的独立观察与记录，
> 全部示例数据均为虚构。同类协议的公开资料：HP 开发者门户的 LEDM XML
> Schema 文档、Apple AirPrint/eSCL 规范、PWG IPP-over-USB 规范（5100.14）、
> 以及 `ipp-usb`、SANE eSCL/hp-ledm 等开源实现。

## 1. USB 设备布局（以一台 Color LaserJet 为例）

复合设备（如 VID 03F0 / PID AE2A），5 个接口：

| 接口 | 签名 | 端点 | 用途 |
|---|---|---|---|
| 0 | class 0x07 (Printer) sub 0x01 proto 0x02 | bulk OUT/IN | 打印数据流（usblp/CUPS） |
| 1 | class 0xFF sub 0x04 proto 0x01，字符串 **"HP LEDM"** | bulk OUT/IN + int IN | **设备管理 HTTP 隧道** |
| 2 | 同接口1签名（部分固件标注 REST） | 同上 | 备用管理通道 |
| 3/4 | class 0xFF sub 0x09 (IPP) proto 0x01 | bulk OUT/IN | IPP over USB |

打印与设备管理分属不同接口：CUPS 占用接口 0 打印时，接口 1 仍可独立查询。

## 2. HTTP over USB 隧道

- 向 bulk OUT 写入标准 HTTP/1.1 请求，从 bulk IN 读响应；
- **必须带 `Host: localhost`**：值错误返回 `400 Bad Request`，缺失则无响应；
- 建议 `Connection: close`：否则固件在接口上保持会话并把旧响应排队；
- 响应为普通 HTTP/1.1（含 `Content-Length`），XML 为 UTF-8。

请求示例（真实可复现的最小请求）：

```
GET /DevMgmt/ConsumableConfigDyn.xml HTTP/1.1
Host: localhost
Connection: close

```

## 3. 设备管理 XML 端点（LEDM）

| 端点 | 内容 |
|---|---|
| `/DevMgmt/ConsumableConfigDyn.xml` | 耗材：颜色/型号/序列号/剩余百分比/状态/品牌/装机日期/标称页数 |
| `/DevMgmt/ProductUsageDyn.xml` | 用量：`TotalImpressions` 总页数、黑白/彩色、卡纸次数、每耗材已打印与预计剩余 |
| `/DevMgmt/ProductStatusDyn.xml` | 机器状态与告警表（卡纸、缺纸、耗材告警等） |

关键字段（本地名，命名空间前缀 dd:/ccdyn:/pudyn:/psdyn: 各固件略有差异）：

```
ConsumableInfo {
  ConsumableLabelCode            # K / C / M / Y
  ConsumableFamilyName           # "Black toner supply"
  ProductNumber                  # 如 CF510A
  SerialNumber
  ConsumableLifeState { ConsumableState, MeasuredQuantityState, Brand }
  ConsumablePercentageLevelRemaining
  ConsumableEstimatedPages { ValueInt }
  Installation { Date }
}
PrinterSubunit { TotalImpressions, MonochromeImpressions,
                 ColorImpressions, JamEvents, MispickEvents }
```

脱敏示例（**全部为虚构数据**）：

```xml
<ccdyn:ConsumableInfo>
  <dd:ConsumableLabelCode>C</dd:ConsumableLabelCode>
  <dd:ConsumableFamilyName>Cyan toner supply</dd:ConsumableFamilyName>
  <dd:ConsumableLifeState>
    <dd:ConsumableState>ok</dd:ConsumableState>
    <dd:MeasuredQuantityState>low</dd:MeasuredQuantityState>
    <dd:Brand>clone</dd:Brand>
  </dd:ConsumableLifeState>
  <dd:ConsumablePercentageLevelRemaining>10</dd:ConsumablePercentageLevelRemaining>
  <dd:ProductNumber>CF511A</dd:ProductNumber>
  <dd:SerialNumber>00000000</dd:SerialNumber>
</ccdyn:ConsumableInfo>
```

## 4. 固件行为要点（实测坑）

1. **残留响应队列**：会话被中断后，固件把旧响应留在发送队列里按序吐出
   （可能不带 HTTP 头、可能是别的端点的内容）。客户端必须校验响应根元素
   与闭合标签，不匹配就清空管道重发。
2. **睡眠切换复位 USB**：进入/退出睡眠或开始接收任务时固件可能复位 USB 栈，
   设备以新设备号重新枚举，期间查询应退避重试。
3. **复位后 usblp 不自动回绑**（部分内核版本）：表现为 `/dev/usb/lp0` 消失、
   CUPS 无法打印。恢复方法：重新插拔，或
   `echo <bus>-<port>:1.0 > /sys/bus/usb/drivers/usblp/bind`。

## 5. 访问权限

vendor 接口（class 0xFF）默认无内核驱动绑定；普通用户访问需 udev 放行，
参见仓库 `data/60-hp-supplies.rules`（可按需用 idProduct 收紧）。
