<p align="center">
  <img alt="BambuLED" src="./image/bambuLED.png" width="720" />
</p>

<h1 align="center">✨ BambuLED</h1>

<p align="center">
  一个基于 <b>ESP32-S3 + WS2812</b> 的拓竹打印机状态联动灯带项目。<br/>
  通过 <b>SoftAP + Captive Portal</b> 网页完成 WiFi / 打印机 MQTT / 灯效配置，设备会根据打印机状态自动切换灯效。
</p>

<p align="center">
  简体中文 · <a href="#快速开始">快速开始</a> · <a href="#配置页面">配置页面</a> · <a href="#排错">排错</a>
</p>

<p align="center">
  <img alt="ESP32" src="https://img.shields.io/badge/Platform-ESP32--S3-77216F?logo=espressif" />
  <img alt="ESP-IDF" src="https://img.shields.io/badge/ESP--IDF-v5.5.1-orange" />
  <img alt="Protocol" src="https://img.shields.io/badge/Protocol-MQTT-660066?logo=mqtt" />
  <img alt="LED" src="https://img.shields.io/badge/LED-WS2812-222222" />
  <img alt="License" src="https://img.shields.io/badge/License-MIT-green" />
  <a href="https://github.com/NingZiXi/BambuLED/stargazers"><img alt="Stars" src="https://img.shields.io/github/stars/NingZiXi/BambuLED?style=social" /></a>
  <img alt="Visitors" src="https://visitor-badge.laobi.icu/badge?page_id=NingZiXi.BambuLED&left_color=grey&right_color=blue" />
</p>

---

## 📌 概述

本项目使用 ESP-IDF 原生组件实现：

- **WiFi**：SoftAP 常开 + STA 联网（APSTA），便于手机随时进入配置页
- **Captive Portal**：DNS 劫持 + 探测 URI 兼容处理，支持手机“自动弹出配置页面”
- **MQTT**：连接拓竹打印机本地 MQTT，订阅 `device/<serial>/report` 获取状态
- **WS2812**：灯带独立渲染任务，多灯效 + 进度条 + 状态映射
- **NVS**：持久化保存 WiFi / 打印机 / 灯效配置

## ✨ 功能

- 网页配置（3 个菜单）：**WiFi 配置 / 连接打印机 / 灯光设置**
- WiFi 流程：点击“连接 WiFi”后先尝试连接，成功才进入重启流程；失败会在页面提示
- 打印机连接：支持本地 MQTT（默认 **8883 + TLS**），用户名固定 `bblp`，密码为打印机访问码
- 灯效支持：常亮 / 呼吸 / 渐变 / 跑马 / 彩虹 / 进度条 / 闪烁 / 关闭
- 状态映射：每个打印状态都可单独配置“灯效 + 速度 + 颜色”，并支持“关闭”
- 舱灯联动：可根据打印机上报的 `system.ledctrl`（chamber_light/chamber_light2）联动 WS2812

## 🧱 架构

```mermaid
%%{init: {"look":"handDrawn","theme":"base","themeVariables":{"fontFamily":"ui-sans-serif, system-ui","lineColor":"#111827"}}}%%
flowchart LR
  classDef task fill:#f3f4f6,stroke:#111827,color:#111827,stroke-width:2,rx:10,ry:10;
  classDef core fill:#ffe8a3,stroke:#111827,color:#111827,stroke-width:2,rx:10,ry:10;
  classDef sys fill:#ffffff,stroke:#111827,color:#111827,stroke-width:2,stroke-dasharray: 6 4,rx:10,ry:10;

  subgraph ESP["ESP32-S3（BambuLED）多任务架构"]
    direction TB
    AppTask["AppController<br/>总控任务"]:::core
    LedTask["LedController<br/>渲染任务"]:::task
    SysHttp["HTTP Server 任务<br/>(ESP-IDF)"]:::sys
    SysMqtt["MQTT Client 任务<br/>(ESP-IDF)"]:::sys
    SysWifi["WiFi / EventLoop<br/>(ESP-IDF)"]:::sys
  end

  SysHttp --> AppTask
  SysWifi --> AppTask
  SysMqtt --> AppTask
  AppTask --> LedTask
```

### 🧵 多任务架构

- AppController 作为“总控任务”，接收 WiFi/MQTT/Web 的事件，统一做状态机与灯效映射
- LedController 作为“渲染任务”，专注输出 WS2812 像素数据（避免 UI/网络抖动影响灯效）

### 📬 任务队列通信流程

```mermaid
%%{init: {"look":"handDrawn","theme":"base","themeVariables":{"fontFamily":"ui-sans-serif, system-ui","lineColor":"#111827","actorBkg":"#f3f4f6","actorBorder":"#111827","actorTextColor":"#111827","signalColor":"#111827","signalTextColor":"#111827","activationBkgColor":"#ffe8a3","activationBorderColor":"#111827","noteBkgColor":"#ffffff","noteBorderColor":"#111827","noteTextColor":"#111827"}}}%%
sequenceDiagram
  participant Web as WebServer(API)
  participant App as AppController(队列接收)
  participant Wifi as WifiService
  participant Mqtt as MqttService
  participant Led as LedController(渲染任务)

  Web->>App: 保存配置 / 触发动作(事件入队)
  Wifi->>App: WiFi 连接/断开/拿到 IP(事件入队)
  Mqtt->>App: 打印机状态上报(事件入队)
  App->>Led: 更新灯效参数(队列/共享状态)
  Led-->>Led: 独立周期渲染 WS2812
```
<a id="快速开始"></a>
## 🚀 快速开始

### 1️⃣ 编译与烧录

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

### 2️⃣ 进入配置页面

- 手机上连接热点：`BambuLED-XXXX`
- 大多数手机会自动弹出配置页；若未弹出，手动打开：
  - `http://192.168.4.1/`

### 3️⃣ 配置 WiFi / 打印机 / 灯光

- **WiFi 配置**：填写 2.4GHz WiFi，点“连接 WiFi”
- **连接打印机**：填写打印机 IP / 访问码 / 序列号（每个字段都有问号提示）
- **灯光设置**：按状态自定义灯效与颜色（每个状态支持“关闭”）

<a id="配置页面"></a>
## 🧭 配置页面

页面入口：
- `GET /`：配置首页（静态 HTML，已嵌入固件）
- `GET /api/config`：读取当前配置与状态
- `POST /api/config`：保存 WiFi / 打印机 / 灯光配置（支持局部提交）
- `POST /api/wifi/disconnect`：断开当前 WiFi（不重启）

## 🎛️ 状态说明（系统灯效）

- WiFi 未连接：蓝色呼吸（更偏蓝）
- WiFi 已连接但 MQTT 断开：粉色呼吸
- 打印机状态联动：按 Idle/Heating/Printing/Finished/Paused/Alert 映射
- 舱灯联动：如果上报过舱灯状态，则按舱灯开/关覆盖灯带显示

<a id="排错"></a>
## 🧰 排错

### WiFi

- 手机能连热点但不自动弹窗：尝试手动打开 `http://192.168.4.1/`
- 点击“连接 WiFi”后失败：检查 SSID/密码是否正确、是否为 2.4GHz

### MQTT

- 本地 MQTT 常见配置：`mqtts://<printer_ip>:8883`（TLS 开启）
- 如果无法连接：确认打印机已开启 LAN 模式/开发者模式，并检查访问码/序列号

### WS2812

- GPIO 是否正确、是否共地、供电是否足够
- 若颜色不对：注意 WS2812 为 GRB 顺序（项目已按 GRB 配置）

---

## 📄 许可证

本项目基于 MIT License 开源，详情请参阅 [LICENSE](LICENSE) 文件


## ❤️ 支持项目

如果 BambuLED 对你有帮助，欢迎赞助一杯咖啡☕，支持持续开发与更新～

<p align="center">
  <a href="https://ifdian.net/a/NingZiXi" target="_blank">
    <img src="https://img.shields.io/badge/%E7%88%B1%E5%8F%91%E7%94%B5-%E8%B5%9E%E5%8A%A9%E6%94%AF%E6%8C%81-FFD54F?style=for-the-badge&labelColor=6A5ACD" width="220" alt="爱发电赞助">
  </a>
</p>

<p align="center">也可以微信或支付宝直接扫码赞赏</p>
<p align="center">
  <img src="./image/PaymentQRcode.jpg" width="260" alt="赞赏二维码">
</p>

## 🤝 贡献

欢迎提交 Issue / PR 来完善本项目（功能、兼容性、文档与排错经验都很有价值）。

**📈 Star History**
[![Star History Chart](https://api.star-history.com/svg?repos=NingZiXi/BambuLED&type=Date)](https://star-history.com/#NingZiXi/BambuLED&Date)

**👤 贡献者**
<p align="center">
  <a href="https://github.com/NingZiXi/BambuLED/graphs/contributors">
    <img src="https://contrib.rocks/image?repo=NingZiXi/BambuLED" />
  </a>
</p>

<p align="center">
感谢您使用 BambuLED！<br/>
如果这个项目对您有帮助，欢迎点个 ⭐ Star 支持一下！
</p>
