# ASUS AIO Screen Control

> **替换华硕官方巨难用的 ASUS InfoHub，为 LCD 水冷用户提供现代、美观、流畅的使用体验。**

ASUS AIO Screen Control 是一款针对华硕 ASUS InfoHub LCD 一体式水冷散热器的第三方屏幕控制工具。通过 HID 直接与设备通信，摆脱官方 InfoHub 软件臃肿的界面与繁琐流程，让水冷屏幕的控制回归简单、直接、好用。

当前版本：**v1.0**（Python 版）

## 适配状态

> ⚠️ 说明：不同型号使用不同 InfoHub 固件版本，协议存在差异。以下分类基于官方「驱动程序和工具软件」页提供的 InfoHub 软件版本划分。

### ✅ 已实测支持
| 型号 | InfoHub 软件版本 |
|---|---|
| **TUF Gaming LC III 360 ARGB LCD** | v1.0.0.15 |

### 🔜 同协议待适配（理论上可直接适配，待实测验证）
以下型号的官方 InfoHub 安装包已下载，协议应与 TUF LC III 高度一致，后续版本逐步验证：
| 型号 | InfoHub 软件版本 |
|---|---|
| ROG STRIX 吹雪 360 LCD 方屏版 | v1.0.7 |
| ROG STRIX RO姬 360 ARGB LCD | v1.0.8 |
| Prime LC/SLC II 360 ARGB LCD | v1.0.7 |
| AYW Gaming LC 360 ARGB LCD | v1.0.6 |

### 📋 其他 InfoHub 机型（待适配）
| 型号 | InfoHub 软件版本 |
|---|---|
| ROG 龙王4代 RYUO IV 360 ARGB / SLC 360 ARGB | v0.8.2 |
| ROG 飞龙4代 Strix LC IV / SLC IV 360 ARGB LCD | v1.2.2 |
| ROG RYUJIN 360 EDITION 20 | 官方 FAQ 支持 |

## 功能

- 视频 / 图片播放（MP4、JPG）
- 硬件信息显示、时钟、自定义文本叠加
- 亮度、方向、播放模式控制
- 悬浮窗 / 主题自定义
- 电源联动（睡眠、关机等）
- HID 直接通信，不依赖官方后台服务

## 依赖

- Python 3.10+
- OpenCV（opencv-python）
- FFmpeg（视频处理，需自行安装并加入 PATH）

## 使用

```bash
pip install -r requirements.txt
python tuf_gui.py
```

或双击 `启动GUI.bat`。

## 未来规划

本项目 **v1.0 是最后一个 Python 版本**，后续将使用 **C++ / WinUI3** 重构，以原生性能与现代化界面带来更流畅的使用体验。重构版将以独立大版本发布（v2+）。

## 声明

本项目为个人开源项目，与华硕（ASUS）官方无关，不包含官方 InfoHub 软件的任何代码。请遵守设备厂商的使用条款。
