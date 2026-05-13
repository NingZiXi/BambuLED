# BambuLED

BambuLED 是一个基于 `ESP32-C3 + WS2812` 的拓竹打印机状态灯带项目，使用 `ESP-IDF v5.5.1` 原生组件实现 `WiFi`、`ESP-MQTT`、`http_server`、`NVS`、`led_strip`。

## 功能

- `WS2812` 灯带控制，支持灯珠数量、GPIO、亮度、默认颜色
- 灯效支持：常亮、呼吸、渐变、跑马、彩虹、进度条、告警闪烁
- `NVS` 持久化保存 WiFi / MQTT / LED 配置
- `SoftAP + Captive Portal` 配网页面
- HTTP 配置页支持修改 WiFi、打印机 MQTT 参数、灯带参数
- MQTT 订阅 `device/<serial>/report`，按打印机状态自动切换灯效
- 采用 `FreeRTOS Queue + EventGroup` 解耦 WiFi / MQTT / LED / Web

## 架构

- `AppController`：系统总控，处理队列事件和状态机
- `WifiService`：WiFi STA / SoftAP / Captive Portal 模式切换
- `WebServer`：配置页与 REST 接口
- `MqttService`：连接拓竹本地 MQTT 并解析 JSON
- `LedController`：独立 LED 渲染任务
- `ConfigStore`：NVS 参数读写
- `components/dns_server`：使用乐鑫官方 Captive Portal DNS 组件进行 DNS 劫持

## 状态映射

- 待机 / 空闲：白色柔和呼吸
- 加热中：暖黄色慢渐变
- 打印中：蓝色进度条
- 打印完成：彩虹流光
- 打印暂停：橙色呼吸
- 故障 / 告警 / 断料：红色高频闪烁

## 默认参数

- LED GPIO：`1`
- LED 数量：`10`
- MQTT 端口：`1883`
- SoftAP SSID：`BambuLED-XXXX`
- SoftAP 密码：无

## 编译与烧录

```bash
idf.py set-target esp32c3
idf.py build
idf.py -p COM3 flash monitor
```

## 配网页面

- 设备会始终开启 `SoftAP`，方便手机随时进入配置页面
- 如已保存 WiFi，设备会同时以 `APSTA` 模式连接路由器
- 连接热点后访问 `http://192.168.4.1/`
- 保存后设备自动重连 WiFi，并在参数完整时自动连接打印机 MQTT

## 说明

- 当前工程按你的需求默认使用 `1883` 纯 TCP，本地页面也可切换为 `TLS`
- 若你的拓竹机型本地 MQTT 实际为 `8883/TLS`，可在网页中修改端口并勾选 `Use TLS`
