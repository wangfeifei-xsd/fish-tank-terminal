# 桌面鱼缸终端固件

ESP32-P4 CB V3.2 + WKS50HD071（5 寸 720×1280 MIPI）桌面鱼缸终端，对接 `https://api.marinefish.fun`。

## 目录结构

```
firmware/fish-tank-terminal/
├── main/
│   ├── app_main.c          # 启动与业务编排
│   ├── anim/               # 鱼缸动画引擎（1280×720 逻辑横屏）
│   ├── ble/                # NimBLE 配网 GATT
│   ├── net/                # WiFi / SNTP / Device API
│   ├── storage/            # NVS + SPIFFS 资源缓存
│   └── ui/                 # LVGL 壳层 + 显示旋转
├── components/BSP/         # 板厂 MIPI / GT911 驱动
├── partitions.csv          # 16MB Flash（含 4MB SPIFFS）
└── sdkconfig.defaults
```

## 环境要求

- ESP-IDF **6.0.x**（与板厂例程一致）
- 目标芯片：`esp32p4`
- 先烧录 **ESP32-C6 协处理器固件**（见 [flash-guide.md](../docs/flash-guide.md)）

## 编译与烧录

```bash
cd firmware/fish-tank-terminal
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## 功能概览

| 模块 | 说明 |
|------|------|
| 显示 | 720×1280 竖屏旋转为 1280×720 逻辑横屏 |
| BLE | Fish-Guard 协议：WiFi 配网 + 设备参数 |
| API | HMAC-SHA256 签名，支持 mock 离线模式 |
| 动画 | 游动/气泡/藻斑/喂食/刮藻/换水（30 FPS） |
| 同步 | 手动刷新 + 60s 自动轮询 |

## 配置

- `idf.py menuconfig` → **Fish Tank Terminal**
- `CONFIG_FISH_MOCK_API=y`：无网关时使用内置测试数据
- API 凭证见 [api-config.md](../docs/api-config.md)
