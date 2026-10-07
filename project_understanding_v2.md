# ASUS AIO Screen Control — 项目理解与 v2 重构规划

> 本文档基于对 v1.0（Python）全部源码 + `PROTOCOL_REVERSE_REPORT.md` 的通读整理，
> 作为 v2（C++/WinUI3）工程开工前的架构共识基线。
> 只读分析阶段产物，**不含任何已写入的代码**。

---

## 0. 阅读范围与一个前置阻塞项

### 已通读的文件
| 文件 | 行数 | 角色 |
|---|---|---|
| `README.md` | 64 | 项目宗旨、适配矩阵、v1→v2 路线声明 |
| `PROTOCOL_REVERSE_REPORT.md` | 586 | HID 协议真值文档（静态逆向 + USBPcap 12 万事件 + 实机验证） |
| `tuf_gui.py` | 2156 | 应用主体：UI + 4 个流线程 + 素材线程 + 设置对话框 + 配置编排 |
| `asset_lib.py` | 453 | 素材入库（ffmpeg 转码）+ 帧适配/旋转/JPEG 编码 + 质量档位表 |
| `tuf_hid.py` | 270 | HID 协议封装：枚举、控制命令、JPEG 分块发送 |
| `config_store.py` | 75 | config.json 读写与默认值合并 |
| `overlay.py` | 90 | 覆盖层绘制（时钟/自定义文本），v2 暂缓 |
| `theme.py` | 255 | 设计令牌 + 浅色 QSS，v2 不迁移（改用 WinUI 原生主题资源） |
| `tuf_direct.py` | 163 | 纯 JPEG 直发验证脚本（协议探针，非应用组成部分） |
| `config.json` | 40 | 运行期实际配置快照 |
| `.gitignore` / `启动GUI.bat` | 20 / 23 | 忽略规则 / 启动壳 |

### ⚠️ 阻塞项：当前会话无法执行任何 shell 命令
本次分析期间，**每一条 `pwsh` 调用都在沙箱建立阶段失败**：

```
Error: SetNamedSecurityInfoW failed (Win32 5): grantWrite(D:\asus-aio-screen-control)
```

即 DSH 无法为工作区授予写 ACL（Win32 5 = ACCESS_DENIED）。直接后果是
**开工前环境验证无法执行、git 提交推送无法执行**（详见 §8.2）。
文件读写工具（read / write / glob / grep）正常，因此纯分析工作不受影响。

---

## 1. 整体架构与数据流

### 1.1 一句话定位
这是一个**单向的"帧泵"应用**：把某种来源的画面（静态图 / 视频 / 桌面 / 媒体封面）渲染成
320×320 BGR 帧，JPEG 编码后经 USB HID 图像接口持续推流到水冷屏。
设备不保存最后一帧，所以"显示"的本质是**永不停歇地重复发送**；软件一停，屏幕就回自带动画。

### 1.2 分层结构（v1 现状）

```
┌───────────────────────────────────────────────────────────────┐
│ 表现层  tuf_gui.MainWindow (PySide6)                          │
│   顶栏 / 预览 / 控制卡 / 素材库 / 捕获页 / 覆盖层编辑 / 设置对话框  │
└───────┬───────────────────────────────────────────┬───────────┘
        │ 控件回调 → 写配置(set→立即落盘)            │ 帧回调 → QImage 预览
┌───────▼───────────────────────────────────────────▼───────────┐
│ 编排层  MainWindow._init_device / start_photo / start_video    │
│         start_capture / stop_display / _maybe_power_off        │
│         （当前是"应用状态机"的实际所在地）                        │
└───────┬───────────────────────────────────────────────────────┘
        │ 启动/停止 4 个 QThread
┌───────▼───────────────────────────────────────────────────────┐
│ 流线程层（4 条并行的帧泵，同一套流水线）                          │
│   PhotoStreamThread    静态图循环重发                           │
│   VideoStreamThread    OpenCV 解码 + 时间轴丢帧 + 动态画质       │
│   ScreenCaptureThread  dxcam 截屏 + 前馈画质                    │
│   SMTCCaptureThread    winsdk 读媒体封面 + 心跳维持              │
└───────┬───────────────────────────────────────────────────────┘
        │ fit_frame → draw_overlay → rotate → bgr_to_jpeg
┌───────▼───────────────────────────────────────────────────────┐
│ 图像层  asset_lib（适配/旋转/编码）+ overlay（叠加绘制）           │
└───────┬───────────────────────────────────────────────────────┘
        │ jpeg bytes
┌───────▼───────────────────────────────────────────────────────┐
│ 协议层  tuf_hid.TufCooler   控制命令(441B) / 图像分块(1025B)      │
└───────────────────────────────────────────────────────────────┘
        │ WriteFile
      USB HID  MI_00 控制端点 440B   MI_01 图像端点 1024B
```

### 1.3 关键数据流：从素材到屏幕

**A. 素材入库（离线，一次性）**
```
用户选文件 → AddAssetThread
   照片: imread_unicode → ffmpeg scale 短边320 lanczos → -q:v 2 → assets/photos/*.jpg
   视频: ffprobe 探测 → ffmpeg scale 短边320 → NVENC/QSV/AMF 或 libx264 CRF15 → assets/videos/*.mp4
   中文路径: 先复制到 assets/cache/work_<uuid> 再交给 ffmpeg（MinGW 不认 Unicode 路径）
```
注意：入库是**短边 320**（1:1→320×320；16:9→569×320），**不是**最终 320×320。
真正压到 320×320 发生在发送前的 `fit_frame`。

**B. 显示链路（在线，每帧重复）**
```
① 取帧     照片: imread 一次常驻内存 / 视频: cv2.read() 顺序解码 / 桌面: dxcam.get_latest_frame()
② 适配     fit_frame(bgr, mode, rotation=0)        → 320×320 BGR（contain 补黑边 / cover 裁切 / stretch 拉伸）
③ 叠加     draw_overlay(帧, cfg)                    → 时钟/自定义文本（draw_overlay 内部 bgr.copy()）
④ 预览     preview 信号 → QImage(BGR888).copy() → QLabel（预览不含旋转，文本正读）
⑤ 旋转     rotate_frame(帧, config.rotation)        → 仅发送路径旋转，0/90/180/270
⑥ 编码     bgr_to_jpeg(帧, quality)                 → BGR 直接编码，禁止转 RGB
⑦ 分块     send_jpeg_frame(jpg, block_delay)        → N=ceil(len/1020)，首块头 [08][N][00][80]
⑧ 写 USB   WriteFile(1025 字节，首字节 0x00 报告 ID)
⑨ 节流     照片按 photo_fps(默认10)；视频按源帧率时间轴；桌面目标60
```
**每帧都是完整重算**：没有任何缓存/脏标记——即使画面完全静止，也逐帧重做 ②③⑥。
时钟叠加每帧取 `time.strftime`，所以时钟是"随帧率刷新"的。

**C. 控制链路（事件驱动，稀疏）**
```
亮度滑杆 → config.set('brightness') → set_brightness(v) → 12 01 00 80 <0-100>
屏幕开关 → config.set('screen_on')  → set_screen(on)    → 10 01 00 80 <00开/01关>
启动恢复 → _init_device(): 用 config 里的 screen_on/brightness 复原设备状态，
                          再用 display_type 复原显示内容（photo/video/screen/smtc）
```
设备接受两类输入：图像接口的 JPEG 数据流，以及控制接口的控制命令
（`0x80000112` 亮度、`0x80000110` 屏幕开关）。

**D. 配置链路（贯穿全程）**
```
控件变化 → config.set(k,v) → ConfigStore.set() → 立刻 json.dump 整个文件
启动     → ConfigStore.load() → DEFAULT_CONFIG 打底 + 文件覆盖（新版本字段自动获得默认值）
路径     → 素材库存相对 APP_DIR 的相对路径（软件目录可整体搬移）；外部文件存绝对路径
```

### 1.4 状态归属（重要设计事实）
v1 里**没有统一的应用状态对象**，状态散落在三处，重构时必须内聚：
- 设备态：`tuf_hid.TufCooler` 内的两个 HANDLE
- 应用态：`MainWindow.current_kind / _screen_on / _custom_texts / _clock_text / _current_src`
- 持久态：`config.json`（且每次控件变化都立即落盘，二者近乎实时同步）

---

## 2. 模块职责、关键接口与调用关系

### 2.1 `tuf_hid.py` — 协议层（v2 的核心资产，逻辑必须逐字保留）

**职责**：设备发现、双接口分类、控制命令发送、JPEG 帧分块发送。无 UI、无配置、无图像处理。

**关键接口**
| 符号 | 说明 |
|---|---|
| `find_hid_interfaces(vid=0x0B05, pid=0x1C7B)` | SetupDi 枚举 HID 设备接口，按路径里的 `vid_0b05&pid_1c7b` 过滤，返回路径列表 |
| `get_output_report_len(path)` | `HidD_GetPreparsedData` + `HidP_GetCaps` 读 `OutputReportByteLength`（校验必须返回 `0x00110000`） |
| `TufCooler.open()` | **按 olen 分类**：440/441→控制接口，1024/1025→图像接口；两者都打不开才报错 |
| `TufCooler.open_img_only()` | 只开图像接口，用于"零控制命令"验证（`tuf_direct.py` 用） |
| `TufCooler._write(h, data, olen)` | **唯一写出口**：补零到 `olen-1` → 前置 `0x00` 报告 ID → `WriteFile(olen)`，校验返回字节数 |
| `TufCooler.send_ctrl(cmd_id, value)` | 5 字节：小端 4 字节 cmd_id + 1 字节 value |
| `TufCooler.set_brightness(v)` | `0x80000112`，钳位 0–100 |
| `TufCooler.set_screen(on)` | `0x80000110`，0x00 开 / 0x01 关 |
| `TufCooler.wake(brightness=None)` | 残留接口，实为亮度命令的包装；v2 无需保留 |
| `TufCooler.send_jpeg_frame(jpg, block_delay=0.004)` | 分块 + 每块 3 次重试，返回 `(块数, 是否全部成功)` |

**已知问题**
- `find_hid_interfaces` 里 `SetupDiGetDeviceInterfaceDetailW` 用 `cbSize=8`（硬编码 x64）+
  `addressof(detail)+4` 手工取路径——64 位下可行但脆弱。
- `open()` 对每个候选路径调用 `get_output_report_len()`（内部又 CreateFile 一次），
  然后**再次** CreateFile 打开——即每个接口开两次句柄。v2 应一次打开、用该句柄查 caps。
- `is_open` 只看 `ctrl_h`；只有图像接口时 `is_open` 为假。
- 无设备热插拔/掉线检测；一旦 `send_jpeg_frame` 返回失败，上层直接 `stop_display()` + 弹窗，需手动重启显示。

### 2.2 `asset_lib.py` — 图像与素材层

**职责**：素材入库（转码管线）、帧几何变换、JPEG 编码、画质档位参数表。

**关键接口 / 数据**
| 符号 | 说明 |
|---|---|
| `SIZE=320` / `JPEG_QUALITY=90` | 目标边长 / 入库照片质量 |
| `FIT_MODES=('contain','cover','stretch')` | 适配模式 |
| `QUALITY_PRESETS={'low':40,'medium':60,'high':80}` | 档位 → 静态 JPEG 质量（`tuf_direct.py` 用） |
| `QUALITY_TARGETS` | 动态画质控制表：`{fps, q0, qmin, qmax}`，`fps=0` 表示取 `min(60, 源帧率)` |
| `imread_unicode(path)` | `np.fromfile` + `cv2.imdecode`，绕开 cv2 不认中文路径 |
| `short_path(p)` | `GetShortPathNameW` 转 8.3 短路径 |
| `find_ffmpeg()` / `detect_video_encoder()` | 定位 `tools/ffmpeg.exe`；NVENC > QSV > AMF > libx264 |
| `AssetLib.add_photo(src)` | ffmpeg 单帧转 JPG（`-q:v 2`），无 ffmpeg 时回退 OpenCV |
| `AssetLib.add_video(src, on_progress)` | ffmpeg 转码 CRF15，进度写文件轮询（避免管道块缓冲），编码器失败自动回退 libx264 |
| `AssetLib.list_photos/list_videos` | 目录枚举 + 扩展名过滤 |
| `rotate_frame(bgr, rotation)` | `cv2.rotate` 整体旋转 |
| `fit_frame(bgr, mode, size, rotation)` | **发送前唯一几何变换入口**（先旋转后适配） |
| `bgr_to_jpeg(bgr, quality)` | BGR 直编码，绝不转 RGB |

**关键约束（协议层要求，v2 必须继承）**
- 降采样插值统一用 `INTER_LINEAR`（缩小时其实 `INTER_AREA` 更好，是 v1 的可优化点）。
- `cover` 分支必须 `.copy()` —— 切片视图非连续内存，编码器/QImage 会花屏。
- `imread_unicode` / `buf.tofile` 是中文路径兼容的两个关键点。

### 2.3 `config_store.py` — 持久化层

**职责**：JSON 配置的模板、合并式加载、整体保存。

```python
DEFAULT_CONFIG = { screen_on, brightness, display_type, photo_path, video_path,
                   fit_mode, photo_fps, video_quality, rotation, playback_mode,
                   auto_start, power_actions, power_close_app, power_shutdown,
                   power_logout, power_sleep, overlay_type, clock_style, clock_text,
                   custom_texts, smtc_filter_mode, smtc_filter_apps }
class ConfigStore: load() / save() / get(k, d) / set(k, v)
```
**行为特征**
- `load()`：`DEFAULT_CONFIG` 打底 → 文件值覆盖。**未知键也被保留**，缺失键自动补默认 ⇒ 天然向前兼容。
- `set()` = 改内存 + **立即整文件重写**。滑杆拖动时每像素一次磁盘写（`_on_brightness_changed`）。
- 无写入节流、无原子写（`open('w')` 直接截断，崩溃可能留下半截文件）、无版本号字段。
- `_lock` 只保护 `save()`，`load()` 与 `data` 字典本身无锁。

### 2.4 `overlay.py` — 叠加绘制层（v2 暂缓）

| 符号 | 说明 |
|---|---|
| `_put_text(out, text, x, y, scale, color, thick, bg, bg_alpha)` | 文本绘制：先半透明底色矩形（`addWeighted`），再黑描边（thick+2），最后本体 |
| `_draw_clock_analog(out, t=None)` | 图形钟表：表盘 + 12 刻度 + 时/分/秒针，秒针橙色 |
| `_draw_text_items(out, items)` | 统一文本条目绘制；`text == '__time__'` 表示实时时间 `%H:%M:%S` |
| `draw_overlay(bgr, hub, placements, cfg)` | 入口。`cfg={overlay_type, clock_style, clock_text, custom_texts}`；`hub`/`placements` 是已废弃的 HWInfo 参数占位 |

**设计要点**：时钟"纯文本"与"自定义文本"**共用同一套条目结构**（`__time__` 作特殊值），
这是 v1 里很干净的一处抽象，v2 应保留。
**性能**：`draw_overlay` 无条件 `bgr.copy()`；底色矩形每帧 `out.copy()` + `addWeighted` 全幅混合。

### 2.5 `theme.py` — 样式层（v2 不迁移）

集中设计令牌 `TOK`（bg/surface/line/txt/accent/ok/warn/danger…），**每个令牌注释里已经标好了对应的 WinUI 画刷名**，
这是 v1 作者为 v2 留的迁移地图（如 `txt → TextFillColorPrimary`、`accent → AccentFillColorDefaultBrush`）。
`app_qss()` 是全局 QSS；`apply_light_titlebar(hwnd)` 用 `DwmSetWindowAttribute(20, 0)` 强制原生标题栏浅色。

**v2 决策**：不迁移 `theme.py` 的 QSS 机制，改用 WinUI 原生 `ThemeResource`；令牌→系统画刷的对应表保留作参考。
唯一需要带走的知识点是：v1 **强制浅色、不跟随系统**（`theme.py` 注释明确写了），而 v2 要求深/浅 + 跟随系统，
所以配色必须**全部换成 ThemeResource**，任何硬编码颜色都会在深色模式下失效。

### 2.6 `tuf_gui.py` — 应用主体（2156 行，v2 必须拆解）

**类清单与职责**

| 类 | 行 | 职责 | v2 归属 |
|---|---|---|---|
| `PreviewLabel(QLabel)` | 90 | 预览控件：接受拖放 + 按住拖动改文本坐标 | DisplayPage 的预览交互 |
| `ScreenCaptureThread(QThread)` | 138 | dxcam 桌面截屏 → 适配 → 叠加 → 旋转 → 编码 → USB；内建前馈画质（目标 60fps） | CaptureSource（本期暂缓） |
| `SMTCCaptureThread(QThread)` | 368 | winsdk 轮询 SMTC 封面/标题；无媒体发黑帧；封面未变时只重发已编码 JPEG 做心跳（2s 周期） | CaptureSource（本期暂缓） |
| `PhotoStreamThread(QThread)` | 458 | 静态图按 `photo_fps` 循环重发（JPEG 质量固定 `PHOTO_JPEG_Q=95`） | StaticImageSource |
| `VideoStreamThread(QThread)` | 522 | 视频解码 + 时间轴/丢帧/回卷 + 动态画质 + 列表循环通知 | VideoSource + PlaybackController |
| `AddAssetThread(QThread)` | 682 | 批量素材入库，进度映射（文件级 × 文件内转码进度，分母 1000） | AssetLibrary + 后台任务 |
| `SettingsDialog(QDialog)` | 718 | 独立设置窗口：常规/电源操作/高级选项(照片频率 + SMTC 过滤) | **SettingsPage（改为应用内页面）** |
| `MainWindow(QMainWindow)` | 820 | 全部编排 + 全部 UI + 配置读写 + 电源消息 | 拆成 Views + Services |

**MainWindow 承担了 6 种互相独立的职责**（这是 v1 最需要被打散的地方）：
1. UI 构建与可见性互斥（`_build_ui` / `_update_param_visibility` / `_sync_overlay_cards`）
2. 设备生命周期（`_init_device` / `closeEvent`）
3. 播放编排（`start_photo` / `start_video` / `start_capture` / `stop_display` / `_on_video_finished`）
4. 素材库运维（`_refresh_lists` / `_add_asset` / `_delete_selected` / `_rename_selected` / 右键菜单）
5. 配置持久化（散落在近 30 个 `config.set()` 调用点）
6. 系统集成（`_apply_autostart` 写注册表 Run 键、`nativeEvent` 电源消息）

**关键"逐帧读取函数指针"模式**（v2 应改为状态订阅）
四个线程构造函数都接收 `get_mode / get_rotation / get_overlay / get_fps / get_quality / get_screen` 这些
**getter 回调**，每帧调用一次以读取最新 UI 状态。好处是"改参数不用重启线程"（视频不闪、位置不跳），
代价是 UI 状态与线程强耦合、无法测试。v2 用不可变 `RenderParams` 快照 + 原子交换能同时拿到两者的好处。

**信号流（线程 → UI）**
`preview(object)` / `fps_report(float)` / `blocks_report(int)` / `error(str)` / `video_finished()`
其中 `preview` 携带的是 320×320 BGR ndarray，**每帧都跨线程投递**（PySide 会拷贝并进事件队列），
`_show_preview` 里再做一次 `np.ascontiguousarray` + `QImage.copy()` —— 每帧两次全幅拷贝，是 v1 的主要 CPU 浪费点之一。

**编码的"断电消息"注释（第 1590–1612 行）**
v1 作者自己写了一段给后续开发者的说明：电源状态识别依赖 `nativeEvent` 捕获
`WM_QUERYENDSESSION(0x0011)` / `WM_ENDSESSION(0x0016)` / `WM_POWERBROADCAST(0x0218)+PBT_APMSUSPEND(0x0004)`，
并明确列出 4 个可靠性缺口，且**明确建议在 C++/WinUI 里改用**
`RegisterSuspendResumeNotification` / `WTSRegisterSessionNotification` / 任务计划程序兜底。
这段注释是 v2 的现成需求说明（但电源联动属于本期暂缓，只需保留接口位置）。

### 2.7 `tuf_direct.py` — 协议探针（非应用组成部分）

只开图像接口、零控制命令，验证"纯 JPEG 流能否独立驱动屏幕"。它是**协议结论的证据工具**，
v2 不需要移植，但它证明了一条重要的产品行为：**画面显示与控制命令完全解耦**，
所以 v2 可以把"发帧"和"发控制命令"放在两个独立组件里，互不阻塞。

### 2.8 调用关系总图

```
config_store ──(读)──> MainWindow ──(写)──> config_store
                          │
        ┌─────────────────┼──────────────────┬────────────────┐
        ▼                 ▼                  ▼                ▼
  AddAssetThread   *StreamThread      SettingsDialog    tuf_hid.TufCooler
        │                 │                                 ▲
        ▼                 ▼                                 │
    asset_lib.add_*   asset_lib.fit_frame                   │
                      asset_lib.rotate_frame ───────────────┘
                      asset_lib.bgr_to_jpeg
                      overlay.draw_overlay
                      asset_lib.QUALITY_TARGETS
```
依赖方向是**单向**的：`tuf_hid` / `asset_lib` / `overlay` / `config_store` 都不反向依赖 GUI，
只有 `tuf_gui` 依赖它们全体。这个分层是干净的，v2 直接按这个方向切组件即可。
唯一的例外是 `theme.py`：底层模块（`asset_lib` 无关，但 `tuf_gui` 各处）在 UI 里到处引用 `theme.TOK`。

---

## 3. HID 协议核心机制（v2 必须逐字复刻）

### 3.1 设备与端点
| 项 | 值 |
|---|---|
| VID / PID | `0x0B05` / `0x1C7B`（路径含 `vid_0b05&pid_1c7b`） |
| 接口发现 | HID 类 GUID + `DIGCF_PRESENT \| DIGCF_DEVICEINTERFACE`，逐项取 DevicePath |
| 接口分类依据 | `HidP_GetCaps` 的 `OutputReportByteLength` |
| 控制接口 MI_00 | caps 441（wire 440）—— 亮度、开关 |
| 图像接口 MI_01 | caps 1025（wire 1024）—— JPEG 块流 |
| 打开方式 | `CreateFileW(path, GENERIC_READ\|GENERIC_WRITE, FILE_SHARE_READ\|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL)`，同步 I/O |
| 屏幕原生分辨率 | **320×320** |
| 刷新上限 | 0ms 块间延迟实测 559 fps 无花屏（无实际意义） |

### 3.2 报文结构（三条硬规则）

**规则一：WriteFile 必须写 caps 长度，首字节是报告 ID 0x00**
```
控制: [0x00][5 字节命令][0x00 × 435]  = 441 字节
图像: [0x00][4 字节块头][1020 字节载荷] = 1025 字节
只写 440 / 1024 → WriteFile 返回 ERROR_INVALID_PARAMETER (87)
```
> C++ 陷阱预警：WinRT `HidDevice::SendOutputReport` 会**自己**管理报告 ID，
> 若照搬 Python 的"手工前置 0x00"会变成双重报告 ID。v2 要么用 Win32 `WriteFile` 保持
> 与已验证实现逐位一致，要么用 WinRT 并去掉手工前置并实测——**这个必须实测确认，不能假设**。

**规则二：控制命令 = 小端 32 位命令 ID + 1 字节参数**
```
偏移 0..3: cmd_id (LE)      偏移 4: value      偏移 5..439: 0x00
亮度  0x80000112 → 12 01 00 80 <V>     V ∈ 0x00..0x64 (0–100)
开关  0x80000110 → 10 01 00 80 <V>     V: 0x00=开, 0x01=关
亮度 0 不是黑屏，只是最低亮度；黑屏必须用开关命令
```

**规则三：图像帧 = 连续 JPEG 流，按 1020 字节分块**
```
N = ceil(len(jpeg) / 1020)
首块   : [0x08][N]    [0x00][0x80] + jpeg[0:1020]        ← 块号位置放的是"总块数"
后续块 i: [0x08][i]    [0x00][0x00] + jpeg[i*1020:(i+1)*1020]
末块不足 1020 → 用 0x00 填满
```
**易错点**：首块第 1 字节是**总块数 N**（不是序号 0），第 3 字节标志位 `0x80` 才是"帧起始"标记。
N 直接决定单帧传输时间：质量 50 ≈ 5–8 块 / 85 ≈ 10–15 块 / 100 ≈ 20–30 块。

### 3.3 颜色格式（不可协商）
- 设备端 JPEG 解码期望 **BGR 字节序**。
- `cv2.imencode('.jpg', bgr)` 直接用 BGR 帧编码即可。
- **一旦插入 BGR→RGB 转换，红蓝互换**（红显深蓝、蓝显红、黄显浅蓝）。
- v2 里这意味着：从 WinUI/WIC/D2D 拿到 BGRA8 后，**不要**做通道交换，直接交给编码器。

### 3.4 设备行为特性（决定架构）
| 特性 | 后果 |
|---|---|
| 固件不保留最后一帧 | "显示" = 持续发流；静态图也必须循环重发 |
| 约 3 秒无数据自动回自带动画 | 停止发送即自动恢复；静态图可用低频心跳（1–2 Hz）维持 |
| 发帧与控制命令相互独立 | 可放两个独立组件，互不阻塞 |
| InfoHub 与本程序争抢同一 HID 接口 | 必须检测并提示用户关闭 InfoHub |

### 3.5 异常处理（v1 现状与 v2 要求）
**v1 做法**
- `_write` 返回 `ok != 0 and written == olen`，任何不满足即判失败。
- `send_jpeg_frame` 每块重试 3 次、间隔 5ms；单块最终失败只把 `ok_all` 置假，**继续发剩余块**。
- `send_ctrl` 系列失败抛 `RuntimeError`；上层线程 `error.emit` → `stop_display()` + `QMessageBox`。
- 无掉线重连、无热插拔感知、无错误分类。

**v2 要求**
1. 传输层内聚重试（单块重试 N 次 + 帧级重试 + 指数退避），业务层只收到"帧已投递/设备掉线"两类结果。
2. 错误分类：`ERROR_DEVICE_NOT_CONNECTED` / `ERROR_ACCESS_DENIED`（多为 InfoHub 占用）/ `ERROR_INVALID_PARAMETER`（协议参数错）/ 超时。
3. 设备掉线 → 自动降级为"未连接"状态，热插拔恢复后自动重开接口并继续播放（v1 完全做不到）。
4. 连续 K 帧发送失败才判定掉线，避免偶发 USB 抖动打断播放。
5. 所有帧发送失败**不得**弹模态对话框阻塞 UI（v1 的 `QMessageBox` 会卡住用户）。

---

## 4. 配置系统与设置页组织

### 4.1 config.json 结构（22 个键，按功能分组）

| 组 | 键 | 类型 / 取值 | 默认值(代码) | 当前文件值 | v2 归属 |
|---|---|---|---|---|---|
| 屏幕 | `screen_on` | bool | `True` | `true` | 主控页 + 设置页 |
| 屏幕 | `brightness` | int 0–100 | `50` | `100` | 主控页 + 设置页 |
| 屏幕 | `fit_mode` | contain/cover/stretch | `contain` | `contain` | 主控页 |
| 屏幕 | `rotation` | 0/90/180/270 | `0` | `270` | 主控页 |
| 内容 | `display_type` | photo/video/screen/smtc/null | `None` | `video` | 应用状态（内部） |
| 内容 | `photo_path` | 相对/绝对路径/null | `None` | `null` | 应用状态（内部） |
| 内容 | `video_path` | 相对/绝对路径/null | `None` | `assets/videos/...mp4` | 应用状态（内部） |
| 内容 | `playback_mode` | single/list | `single` | `single` | 主控页 |
| 播放 | `photo_fps` | float 0.1–30 | `10.0` | `1.0` | 设置页 |
| 播放 | `video_quality` | low/medium/high | `medium` | `high` | 主控页 |
| 启动 | `auto_start` | bool | `False` | `false` | 设置页（写 HKCU Run） |
| 电源 | `power_actions` | bool（总开关） | `True` | `true` | 设置页（**本期暂缓**） |
| 电源 | `power_close_app` | bool | `True` | `true` | 同上 |
| 电源 | `power_shutdown` | bool | `False` | `true` | 同上 |
| 电源 | `power_logout` | bool | `False` | `true` | 同上 |
| 电源 | `power_sleep` | bool | `False` | `true` | 同上 |
| 叠加 | `overlay_type` | none/clock/custom（旧值 hwinfo 归一到 none） | `none` | `none` | 主控页 |
| 叠加 | `clock_style` | text/analog | `text` | `text` | 主控页 |
| 叠加 | `clock_text` | `{x,y,alpha,scale,thick,color[B,G,R],bg[B,G,R]}` | `x0 y0 α0.6 s0.6 t1 白/黑` | 同 | 主控页 |
| 叠加 | `custom_texts` | 数组，元素同上 + `text` | `[]` | `[]` | 主控页 |
| 采集 | `smtc_filter_mode` | all/whitelist/blacklist | `all` | `all` | 设置页（**本期暂缓**） |
| 采集 | `smtc_filter_apps` | string[] | `[]` | `[]` | 同上 |

**注意两处默认值不一致**：`brightness`（代码 50 / 文件 100）、`photo_fps`（代码 10.0 / 文件 1.0）、
`video_quality`（medium / high）、`rotation`（0 / 270）。**代码默认值是"首次运行"值，文件值是用户改过的**，
v2 必须原样沿用代码默认值，否则老用户升级后观感会变。
另外 `clock_text` 在代码里的默认是 `x=0.0, y=0.0`，而 `MainWindow.__init__` 读取时回退值是
`x=0.85, y=0.12`（右上角）——存在两套默认，v2 要统一（建议以右上角为准）。

### 4.2 读写流程与路径规约
```
启动:  ConfigStore(path) → load() → DEFAULT_CONFIG ∪ 文件 → self.data
读取:  get(key, default)
写入:  set(key, value) → data[key]=value → save() → json.dump(全量, ensure_ascii=False, indent=2)
路径:  素材库内文件 → os.path.relpath(p, APP_DIR).replace('\\','/') 存入
       外部文件     → 存绝对路径
       读出时相对路径拼回 APP_DIR（os.path.normpath）
启动恢复: display_type 指向的素材若已不存在 → 清空该路径并把 display_type 置 None
```
**v2 改进点**：写入去抖（200–500ms 合并）、原子写（临时文件 + `ReplaceFile`/`MoveFileEx` 原子替换）、
加 `schema_version` 字段、控件绑定不直接触发磁盘 I/O。

**✅ 已确认的兼容性方案：配置迁到 `%LOCALAPPDATA%`，并实现 v1→v2 迁移**

```
v1 位置（只读，不改动）: D:\asus-aio-screen-control\config.json
v2 位置（唯一写入方）:   %LOCALAPPDATA%\AsusAioScreenControl\config.json
```
迁移逻辑（v2 首次启动时执行一次）：
1. 若 `%LOCALAPPDATA%\...\config.json` 已存在 → 直接用，迁移结束。
2. 否则若仓库根 `config.json` 存在 → 读取（复用 v1 的**合并式加载**：`DEFAULT_CONFIG` 打底 + 文件覆盖 +
   **保留全部未知键**，其中 `hwinfo` 等旧值做归一再写入）。
3. **路径重定位（迁移的关键，必须做）**：v1 的 `photo_path` / `video_path` 是相对
   `APP_DIR = D:\asus-aio-screen-control` 的相对路径（如 `assets/videos/xxx.mp4`）。
   配置基目录变化后这些相对路径会失效，迁移时必须
   **全部转成绝对路径**：`%(V1_REPO_ROOT)%\<相对路径>` → 绝对路径后再写入 v2 配置；
   已是绝对路径的（外部文件）原样保留。
4. 素材库目录在 v2 里**仍指向仓库内的 `assets\photos` / `assets\videos`**（素材是用户数据、体积大，
   不迁到 `%LOCALAPPDATA%`；`tools\ffmpeg.exe` 同样仍从仓库定位，并回退系统 PATH）。
5. 迁移只在 v2 侧新增文件，**绝不移动/删除/修改 v1 的 `config.json`** —— 这样用户可随时回退到 v1，
   且完全符合"不修改与本任务无关文件"的约定。
6. v2 的 `AppConfig` 增加 `schema_version`，后续版本变更走版本化升级；v2 自己**不再回写** v1 的 config.json，
   因此不存在"v1/v2 互相覆盖"的风险。

### 4.3 设置页（SettingsPage）的配置项组织

v1 的 `SettingsDialog` 只有三组：常规（开机自启）/ 电源操作 / 高级选项（照片频率、SMTC 过滤），
而**亮度、图像适应、旋转、视频质量、播放模式、叠加层**这些同样属于"设置"的项散落在主控页。
v2 按 WinUI 设置页惯例重新组织，同时不破坏"常用项在主控页也能直接改"的体验（两边绑定同一份配置）：

**设置页分组（本期）**
| 分组 | 设置项 | 控件 | 绑定键 |
|---|---|---|---|
| 常规 | 开机自启 | `ToggleSwitch` | `auto_start` |
| 常规 | 界面主题（跟随系统/浅色/深色） | `RadioButtons` | **新增键** `app_theme` |
| 屏幕 | 屏幕开关 | `ToggleSwitch` | `screen_on` |
| 屏幕 | 亮度 | `Slider` 0–100 | `brightness` |
| 画面 | 图像适应 | `ComboBox`(contain/cover/stretch) | `fit_mode` |
| 画面 | 画面旋转 | `ComboBox`(0/90/180/270) | `rotation` |
| 播放 | 照片发送频率 | `NumberBox` 0.1–30 Hz + 说明"过低会触发固件 3s 超时" | `photo_fps` |
| 播放 | 视频质量档 | `ComboBox`(低/中/高) | `video_quality` |
| 播放 | 播放模式 | `ComboBox`(单曲/列表循环) | `playback_mode` |
| 关于 | 版本、适配机型、开源声明 | `HyperlinkButton` + 文本 | — |

**设置页分组（占位，标注"暂缓"）**
| 分组 | 设置项 | 绑定键 | 状态 |
|---|---|---|---|
| 电源联动 | 总开关 + 关程序/关机/注销/睡眠 | `power_*` 5 键 | 灰显 + "暂不可用" |
| 采集 | SMTC 来源过滤模式 / 应用名列表 | `smtc_filter_mode` / `smtc_filter_apps` | 灰显（待确认见 §7） |
| 显示内容捕获 | 目标显示器 | 未持久化 | 灰显 |

**设置页实现要求**
- 一个 `SettingsViewModel`（或 `SettingsPagePresenter`）：构造时从 config 快照填充控件，
  控件变化 → 写 VM → 去抖 300ms → 落盘 + 广播 `ConfigChanged`。
- 设置页与主控页共用同一个配置服务，**避免两处状态不同步**（v1 的 `slider_fps` 就是"隐藏控件做逻辑中转"的补丁）。
- `auto_start` 落地实现：`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`，
  值名沿用 `TUFScreenControl`，命令改为当前 exe 绝对路径（便于迁移 v1 的既有自启注册项）。
- 主题项（新增 `app_theme`）：需要一个持久化字段存放 跟随系统/浅色/深色，已确认新增 `app_theme` 键
  （v1 会忽略未知键，安全）。
- **配置位置**：v2 的配置读写 `%LOCALAPPDATA%\AsusAioScreenControl\config.json`，
  首次启动从仓库根 `config.json` 迁移（含路径重定位），详见 §4.2。

---

## 5. 本期暂缓逻辑清单（保留/标注，不实现）

| 功能 | v1 位置 | 处理方式 |
|---|---|---|
| 温度 / 硬件信息监测 | `overlay.draw_overlay` 的 `hub`/`placements` 参数；`custom_texts` 曾支持 `hwinfo` 类型；`overlay_type=='hwinfo'` 归一逻辑 | v2 **完全不引入**。叠加层只保留 clock / custom text 两类；`overlay_type` 的 `hwinfo` 归一逻辑保留在迁移函数里（读到旧值→none），避免老配置报错 |
| 系统信息采集 | 无独立实现（v1 已移除） | 不实现 |
| 电源联动 | `tuf_gui.py` 1589–1644（`_maybe_power_off` / `closeEvent` / `nativeEvent`）+ 5 个配置键 + 设置页电源分组 | 配置键读写**照旧保留**（兼容 config.json），设置项在设置页**灰显标注"暂缓"**；`AppServices` 预留 `PowerWatcher` 接口但 `AutoStart` **本期实现**（它不属于暂缓项）。v2 正解按 v1 注释建议：`RegisterSuspendResumeNotification` + `WTSRegisterSessionNotification` |
| 自定义主题 | `theme.py` 整文件（`TOK` / `app_qss()` / 各类 helper / `apply_light_titlebar`） | **不迁移**。v2 只用 WinUI 原生 ThemeResource + 深/浅/跟随系统。`TOK→WinUI 画刷` 的注释表留作配色参考 |
| 悬浮窗 | `overlay.py` 只做帧内叠加，无独立悬浮窗；`README` 提到的"悬浮窗"在 v1 代码里**并未实现**（README 与代码不一致，需在 README 修订时一并处理） | 不实现 |
| 桌面实时投射 | `ScreenCaptureThread` + `dxcam` 依赖 + 显示器选择 | 本期暂缓（见 §7 待确认） |
| SMTC 媒体封面 | `SMTCCaptureThread` + `winsdk` 依赖 + 过滤设置 | 本期暂缓（见 §7 待确认） |

> 补充发现：`tuf_gui.py` 的 `_make_cam()` 里仍保留 `region == 'square_center'` 分支，
> 但调用处传的是 `region=None`，属**死代码**；`tuf_gui.py` 第 3064–3092 行附近的
> `_maybe_power_off` 与 `nativeEvent` 之外的电源注释块也需在 v2 精简。
> `README.md` 的功能列表（"硬件信息显示"、"悬浮窗/主题自定义"、"电源联动"）与 v1 实际实现有偏差，
> 属于 v1 历史遗留描述，v2 完成时应同步修订。

---

## 6. C++/WinUI3 重构规划

### 6.1 设计原则

1. **按功能内聚切分，不按文件对应类**。7 个 Python 文件 → **3 个项目 / 约 12 个内聚组件**，
   其中 `tuf_gui.py` 一个文件就要拆成 7–8 个组件。
2. **依赖方向与 v1 保持一致**：`Device` / `Imaging` / `Core` 不依赖 WinUI；
   只有 `Views` / `ViewModels` 依赖 WinUI。这样协议与图像层可以单测、可以在无 UI 下跑。
3. **热路径零分配**：帧缓冲、JPEG 输出缓冲、HID 报文缓冲全部预分配复用。
4. **UI 线程绝不阻塞**：设备枚举、转码、发帧、缩略图解码全在后台。
5. **状态显式化**：用不可变 `RenderParams` 快照替代 v1 的 getter 回调；用 `DeviceState` 替代散落的 HANDLE。

### 6.2 目录结构（`D:\asus-aio-screen-control\v2\`）

```
v2\
├─ AsusAioScreenControl.sln
├─ .gitignore                       ← v2 专属: x64/ Debug/ Release/ Generated Files/ *.user .vs/ out/ packages/
├─ Directory.Build.props            ← 统一 C++20 / /W4 / WinUI3 公共属性 (可选)
├─ README.md                        ← v2 构建说明 (VS2022 17.14 + Windows App SDK)
│
├─ src\
│  ├─ ScreenControl.Device\         ← 静态库: HID 协议层 (对应 tuf_hid.py)
│  │   ├─ HidEnumerator.h/.cpp          SetupDi 枚举 + HidP_GetCaps 分类, 返回 {ctrlPath, imgPath}
│  │   ├─ HidTransport.h/.cpp           CreateFile/WriteFile 封装; 唯一的 _Write 出口; 重试策略
│  │   ├─ ControlProtocol.h/.cpp        0x80000112 亮度 / 0x80000110 开关 的报文构造
│  │   ├─ FrameChunker.h/.cpp           1020B 分块 + 首块 [08][N][00][80] 头构造 (纯函数, 可单测)
│  │   └─ ScreenDevice.h/.cpp           对外门面: Open/Close/SetBrightness/SetScreen/SendFrame
│  │                                    + DeviceState + 掉线/热插拔 + 重连
│  │
│  ├─ ScreenControl.Imaging\        ← 静态库: 帧处理 (对应 asset_lib 的运行时部分 + overlay)
│  │   ├─ FrameBuffer.h                 预分配的 BGRA8/BGR 帧容器 (320×320)
│  │   ├─ FrameGeometry.h/.cpp          FitMode(contain/cover/stretch) + Rotate90 的 SIMD 实现
│  │   ├─ JpegEncoder.h/.cpp            WIC 编码, BGR 直入, 质量参数, 复用 IStream/缓冲
│  │   ├─ OverlayRenderer.h/.cpp        时钟/文本合成 (对应 overlay.py, 本期只用 clock/text)
│  │   └─ RenderParams.h                不可变参数快照 {fitMode, rotation, jpegQuality, overlay}
│  │
│  ├─ ScreenControl.Core\           ← 静态库: 应用逻辑 (无 WinUI 依赖)
│  │   ├─ AppConfig.h/.cpp              22+ 键的强类型结构 + 默认值 + 合并式读 + 原子去抖写 (config_store.py)
│  │   ├─ ConfigMigration.h/.cpp        v1 仓库根 config.json → %LOCALAPPDATA% 的一次性迁移
│  │   │                                 (含 photo_path/video_path 相对→绝对路径重定位) 
│  │   ├─ AssetLibrary.h/.cpp           素材库枚举/入库/删除/重命名 + ffmpeg 管线 (asset_lib 的库部分)
│  │   ├─ MediaTranscoder.h/.cpp        ffmpeg 子进程封装 + 进度解析 + 编码器探测 (异步)
│  │   ├─ FrameSource.h                 抽象接口: Start/Stop/SetParams/FrameAvailable
│  │   ├─ StaticImageSource.h/.cpp      静态图 (对应 PhotoStreamThread)
│  │   ├─ VideoFrameSource.h/.cpp       MF SourceReader 解码 + 时间轴 + 丢帧 (对应 VideoStreamThread)
│  │   ├─ PlaybackController.h/.cpp     编排: 持有的源 + 渲染器 + 设备, 单点负责启停/切源/心跳
│  │   └─ QualityController.h/.cpp      前馈动态质量 (QUALITY_TARGETS 的 C++ 版, 可复用于所有源)
│  │
│  ├─ ScreenControl.App\            ← WinUI3 应用 (C++/WinRT)
│  │   ├─ App.xaml/.h/.cpp              Application, 主题初始化, 服务组装(组合根)
│  │   ├─ AppServices.h/.cpp            服务容器: Config/Device/Playback/Assets/Theme
│  │   ├─ MainWindow.xaml/.h/.cpp       NavigationView + Frame + Mica + 自定义标题栏
│  │   ├─ Views\
│  │   │   ├─ DisplayPage.xaml/.h/.cpp      主控页: 预览 + 屏幕开关/亮度 + 素材库 + 源选择
│  │   │   ├─ SettingsPage.xaml/.h/.cpp     设置页 (承载配置系统, 应用内页面)
│  │   │   └─ AboutPage.xaml (可选)
│  │   ├─ ViewModels\
│  │   │   ├─ DisplayViewModel.h/.cpp       预览位图/状态/命令绑定
│  │   │   ├─ SettingsViewModel.h/.cpp      配置双向绑定 + 去抖保存
│  │   │   ├─ MediaLibraryViewModel.h/.cpp  素材项集合 + 缩略图异步加载
│  │   │   └─ ObservableObject.h            轻量 INotifyPropertyChanged 基类
│  │   ├─ Controls\
│  │   │   ├─ PreviewPane.xaml/.cpp         预览控件 (SoftwareBitmapSource)
│  │   │   ├─ MediaGridView.xaml/.cpp       素材网格 (GridView + 右键命令)
│  │   │   └─ SettingCard.xaml/.cpp         设置项卡片 (标题/描述/右侧控件)
│  │   ├─ Services\
│  │   │   ├─ ThemeService.h/.cpp           深/浅/跟随系统 + Mica backdrop 同步
│  │   │   ├─ AutoStartService.h/.cpp       HKCU Run 键读写
│  │   │   └─ DispatcherService.h/.cpp      后台线程 → UI 线程的封送
│  │   ├─ Resources\
│  │   │   └─ Tokens.xaml                   (可选) 少量自定义画刷, 其余全用系统 ThemeResource
│  │   ├─ Package.appxmanifest / app.manifest
│  │   └─ ScreenControl.App.vcxproj
│  │
│  └─ tests\ (可选, 建议至少覆盖两个纯逻辑组件)
│      ├─ FrameChunkerTests.cpp             ← 分块头字节的黄金用例 (>1020 / =1020 / <1020 / 多块)
│      └─ AppConfigTests.cpp                ← 默认值合并 / 未知键保留 / 旧值归一
└─ docs\
   └─ PROJECT_UNDERSTANDING.md              ← 本文档迁入
```

**为什么这样切**：`Device` 与 `Imaging` 是纯逻辑，可以脱离 UI 单测（这是 v1 最缺的）；
`Core` 承载"帧泵"编排，`App` 只做呈现。**没有一个类与 Python 文件一一对应**——
例如 `tuf_gui.py` 的四个线程变成 `FrameSource` 的一个接口 + 2 个实现 + 1 个编排器 + 1 个质量控制器。

### 6.3 核心类设计

```cpp
// ---- Device 层 ----
enum class DeviceState { Disconnected, Ready, ControlOnly, Error };

struct FrameChunkHeader {           // 纯函数, 便于单测
    static constexpr uint32_t kJpegChunkSize = 1020;
    static constexpr uint32_t kImageReportLen = 1025;
    static constexpr uint32_t kControlReportLen = 441;
    static void BuildFirst(uint8_t* block, uint8_t totalBlocks);   // 08 N 00 80
    static void BuildNext (uint8_t* block, uint8_t index);         // 08 i 00 00
};

class ScreenDevice {                                              // 对应 TufCooler
public:
    winrt::fire_and_forget OpenAsync();                           // 后台枚举, 不阻塞 UI
    void Close() noexcept;
    DeviceState State() const noexcept;
    winrt::event_token StateChanged(...);

    bool SetBrightness(uint8_t percent);                          // 0x80000112, 钳位 0-100
    bool SetScreen(bool on);                                      // 0x80000110, 0=开/1=关
    SendResult SendFrame(std::span<const uint8_t> jpeg,           // 分块 + 重试, 不抛异常
                         uint32_t blockDelayUs = 0);
private:
    bool WriteControl(std::span<const uint8_t> payload);          // 唯一写出口: 补零 + 前置 0x00 + WriteFile(441)
    bool WriteImageBlock(const uint8_t* block1024);               // WriteFile(1025)
    // 预分配缓冲: m_ctrlBuf[441], m_imgBuf[1025]; 掉线计数; 重连状态
};

// ---- Imaging 层 ----
struct RenderParams {                 // 不可变快照, 替代 v1 的 6 个 getter
    FitMode   fitMode   = FitMode::Contain;
    Rotation  rotation  = Rotation::Deg270;
    uint8_t   jpegQuality = 95;
    bool      overlayEnabled = false;
    OverlaySpec overlay{};             // clock / custom text
};

class FramePipeline {                  // 一次调用完成 fit->overlay->rotate->encode
public:
    // 输入: 任意尺寸 BGR/BGRA 帧; 输出: 指向内部复用的 JPEG 缓冲 (零分配)
    std::span<const uint8_t> Render(std::span<const uint8_t> src, int w, int h,
                                    const RenderParams& p, uint32_t* outJpegBytes);
private:
    FrameBuffer m_canvas;              // 320x320 预分配
    JpegEncoder m_encoder;             // WIC, 复用 stream
};

// ---- Core 层 ----
class IFrameSource {                                                    // 统一四种源
public:
    virtual ~IFrameSource() = default;
    virtual winrt::fire_and_forget Start(const RenderParams&) = 0;
    virtual void Stop() = 0;
    virtual void UpdateParams(const RenderParams&) = 0;   // 热更新, 不重启
    virtual void SetFrameSink(std::function<void(FrameView)>) = 0;
};

class PlaybackController {              // v1 的 start_photo/start_video/stop_display/_init_device 合并点
public:
    void AttachDevice(std::shared_ptr<ScreenDevice>);
    winrt::fire_and_forget PlayPhoto(std::filesystem::path);
    winrt::fire_and_forget PlayVideo(std::filesystem::path, PlaybackMode);
    void Stop();
    void ApplyParams(const RenderParams&);        // fit/rotation/quality/overlay 一次性下发
    void SetPreviewSink(PreviewSink);             // 已节流的 UI 预览回调
};
```

**`PlaybackController` 是 v1→v2 最关键的收敛**：v1 里"启动/停止/切源/参数变更/心跳/列表循环/错误恢复"
分散在 MainWindow 的十余个方法里，v2 全部收进这一个类，UI 只发命令、只收状态。

### 6.4 WinUI3 UI 层设计

#### 6.4.1 窗体骨架：NavigationView + Frame

```xml
<Window ...>
  <Grid>
    <Grid.RowDefinitions>
      <RowDefinition Height="Auto"/>   <!-- 标题栏行 (拖动区 + 标题 + 右对齐按钮) -->
      <RowDefinition Height="*"/>
    </Grid.RowDefinitions>

    <!-- 自定义标题栏 (内容扩展到标题栏) -->
    <Grid x:Name="AppTitleBar" Grid.Row="0" Height="48">
      <StackPanel Orientation="Horizontal" Margin="16,0,0,0" VerticalAlignment="Center">
        <FontIcon Glyph="&#xE9D9;" FontSize="16"/>
        <TextBlock x:Name="AppTitleText" Text="ASUS AIO Screen Control"
                   Margin="12,0,0,0" VerticalAlignment="Center"/>
      </StackPanel>
    </Grid>

    <NavigationView x:Name="Nav" Grid.Row="0" Grid.RowSpan="2"
                    IsBackButtonVisible="Collapsed" IsSettingsVisible="False"
                    PaneDisplayMode="Left"
                    Background="Transparent"
                    SelectionChanged="Nav_SelectionChanged">
      <NavigationView.MenuItems>
        <NavigationViewItem Content="主控" Tag="display"><IconSourceElement .../></NavigationViewItem>
        <NavigationViewItem Content="设置" Tag="settings"><IconSourceElement .../></NavigationViewItem>
      </NavigationView.MenuItems>
      <!-- 标题栏右侧留出系统按钮区域: PaneCustomContent / 顶部 Padding 由 TitleBar 高度撑开 -->
      <Frame x:Name="ContentFrame" Background="Transparent"/>
    </NavigationView>
  </Grid>
</Window>
```

**Mica + 内容扩展标题栏落地步骤（代码层）**
```cpp
// MainWindow 构造函数中:
ExtendsContentIntoTitleBar(true);
SetTitleBar(AppTitleBar());                       // 拖动区改为 XAML 元素

if (auto appWindow = this->AppWindow()) {
    appWindow.TitleBar().ButtonBackgroundColor(Microsoft::UI::Colors::Transparent());
    appWindow.TitleBar().ButtonInactiveBackgroundColor(Microsoft::UI::Colors::Transparent());
    appWindow.TitleBar().PreferredHeightOption(
        winrt::Windows::UI::Windowing::TitleBarHeightOption::Tall);
}

// Mica 背衬 (Windows 11)
SystemBackdrop(winrt::Microsoft::UI::Xaml::Media::MicaBackdrop{
    winrt::Microsoft::UI::Composition::SystemBackdrops::MicaKind::BaseAlt });
```
**必须注意的三个坑**
1. **`SetTitleBar` 的交互元素要排除**：标题栏行里若放了按钮（如"设备状态"指示器、刷新按钮），
   要调用 `InputNonClientPointerSource::SetRegionRects(NonClientRegionKind::Passthrough, rects)`
   把这些矩形声明为"客户端区域"，否则点不到。
2. **Mica 会被不透明背景盖住**：`NavigationView` 和每个 `Page` 的根容器都必须
   `Background="Transparent"`，否则看到的是 `ApplicationPageBackgroundThemeBrush`（实心色）而非 Mica。
   内容卡片则用 `CardBackgroundFillColorDefaultBrush`（本来就带半透明，Mica 能透出来）。
3. **Mica 在 Windows 10 / 远程桌面 / 透明效果关闭时不可用**：`MicaBackdrop` 会自动降级为
   纯色 `DesktopAcrylicBackdrop` 或实色，**不需要写代码分支**，但视觉上要能接受降级效果。

#### 6.4.2 深色 / 浅色 / 跟随系统

```cpp
class ThemeService {
public:
    void Apply(AppTheme theme);   // System / Light / Dark, 来自 config 的 app_theme
private:
    void ApplyToRoot(winrt::Microsoft::UI::Xaml::ElementTheme t) {
        if (auto root = m_window.Content().as<FrameworkElement>())
            root.RequestedTheme(t);           // Default = 跟随系统
    }
};
```
- `ElementTheme::Default` = 跟随系统（WinUI 原生能力，自动响应系统主题变化）。
- 显式浅/深：在**窗口根元素**上设 `RequestedTheme`，整棵树（含 `NavigationView`、
  所有 `ThemeResource` 画刷）会一起切换，无需手动改任何颜色。
- **Mica 背衬必须同步**：`MicaBackdrop` 内部会用 `SystemBackdropConfiguration` 监听
  `Theme` / `IsInputActive` / `IsHighContrast`，XAML 的 `RequestedTheme` 变化会驱动它，
  所以深浅切换时 Mica 的色调会自动跟着变——这正是"深色与浅色下 Mica 均有正确表现"的落地方式。
- 所有配色**只用 `ThemeResource`**：`TextFillColorPrimary/Secondary/Tertiary`、
  `CardBackgroundFillColorDefaultBrush`、`CardStrokeColorDefaultBrush`、`AccentFillColorDefaultBrush`、
  `ControlFillColorDefault`、`SystemFillColorCritical` 等（`theme.py` 的 `TOK` 注释已给出对应关系）。
  **任何硬编码 `#RRGGBB` 都会在深色模式下出错**，这是 v1 到 v2 最容易踩的视觉回归。
- 设置页提供三选项（跟随系统 / 浅色 / 深色），持久化到 `app_theme`（新增键，向后兼容）。

#### 6.4.3 主控页（DisplayPage）布局

```
┌ 预览卡 (320×320 原生比例, SoftwareBitmapSource 显示) ─┐  ┌ 屏幕控制卡 ────────────┐
│  预览位图 + "未在显示" 空状态                          │  │ [ToggleSwitch] 屏幕开关  │
└───────────────────────────────────────────────────────┘  │ 亮度 [Slider] 100%      │
┌ 显示内容 (SelectorBar: 照片 | 视频 | 捕获) ───────────┐  │ 图像适应 [ComboBox]     │
│  照片: GridView 缩略图网格 + "添加照片" 按钮            │  │ 画面旋转 [ComboBox]     │
│  视频: 播放模式 + GridView + "添加视频" + 转码进度条     │  │ 视频质量 [ComboBox]     │
│  捕获: (本期灰显占位)                                  │  │ [状态芯片● 已连接]      │
└───────────────────────────────────────────────────────┘  └────────────────────────┘
┌ 叠加卡: 无 / 时钟 / 自定义文本 + 样式编辑 (可折叠 Expander) ─┐
```
- 预览用 `SoftwareBitmapSource` + `Image`；后台线程用 `DispatcherQueue.TryEnqueue` 投递，
  **必须节流到 20–30fps**（v1 是每帧投递，见 §7）。
- 页面切换用 `Frame.Navigate`，用 `NavigationCacheMode::Required` 缓存主控页状态，避免来回切丢预览。

#### 6.4.4 设置页（SettingsPage）承载配置系统

```xml
<Page ...><ScrollViewer><StackPanel Spacing="4">
  <TextBlock Text="常规" Style="{StaticResource TitleTextBlockStyle}"/>
  <local:SettingCard Header="开机自启" Description="登录 Windows 时自动运行">
    <ToggleSwitch IsOn="{x:Bind VM.AutoStart, Mode=TwoWay}"/>
  </local:SettingCard>
  <local:SettingCard Header="界面主题" Description="仅深色/浅色，跟随系统">
    <RadioButtons SelectedIndex="{x:Bind VM.ThemeIndex, Mode=TwoWay}">
      <RadioButton Content="跟随系统"/><RadioButton Content="浅色"/><RadioButton Content="深色"/>
    </RadioButtons>
  </local:SettingCard>
  <TextBlock Text="屏幕" .../>
  ... (亮度 / 适应 / 旋转 / 照片频率 / 视频质量 / 播放模式)
  <TextBlock Text="电源联动（暂缓）" .../>
  <local:SettingCard IsEnabled="False" Description="本期暂未实现">...</local:SettingCard>
</StackPanel></ScrollViewer></Page>
```
- **保存/加载流程**：`SettingsViewModel` 构造时 `AppConfig` 快照 → 填控件；
  `Mode=TwoWay` 绑定回 VM → VM 内 300ms 去抖 → `ConfigService.SaveAsync()`（原子写）。
- 主控页与设置页**读写同一个 `ConfigService`**，靠 `ConfigChanged` 事件保持同步
  （比如设置页改亮度，主控页滑杆同步移动）。
- 设置页必须是 `Page`（`Frame` 导航目标），**不是 `ContentDialog`、不是独立 `Window`**——这是硬约束。

### 6.5 性能 / 稳定性：v1 哪些是原型级、必须在 C++ 重写

| # | v1 原型级设计 | 问题 | v2 方案 |
|---|---|---|---|
| 1 | `fit_frame` 每帧都做 resize（即使源帧静止） | 静态图/桌面无变化时也反复缩放 | 输入尺寸+参数未变则复用上次画布（脏标记） |
| 2 | `overlay.draw_overlay` 每帧 `bgr.copy()` + 每文本 `out.copy()`+`addWeighted` | 静态图时纯浪费 | 叠加层离屏缓存；时钟仅在秒数变化时重绘 |
| 3 | JPEG 编码在发帧线程同步执行 | 编码时间直接挤占帧预算 | 编码放到独立工作线程/+ 可选 jpeg-turbo（SIMD） |
| 4 | 每帧 `send_jpeg_frame` 逐块 `WriteFile` + `time.sleep(block_delay)` | 帧间有 4ms×N 的固定延迟，帧率上不去 | HID 写搬到专用高优先级线程 + **OVERLAPPED 异步 I/O 队列**，块间不 sleep |
| 5 | 静态图按 `photo_fps`(默认10，最低可到 0.1) 全速重发 | 静止画面也在做完整编码+USB 传输 | **心跳降频**：内容未变时降到 1–2 Hz 维持（避开固件 3s 超时），变化时立即恢复全速 |
| 6 | `preview` 信号每帧跨线程投递 ndarray，UI 侧再 `ascontiguousarray`+`QImage.copy()` | 60fps 下事件队列洪泛、GC 抖动、界面发滞 | 预览**节流到 20–30fps** + 单槽"最新帧"交接（丢旧帧而非排队）+ 复用 `SoftwareBitmap` |
| 7 | `VideoStreamThread` 顺序解码 + 跳帧追赶，落后无法快速 seek | 卡顿后恢复慢 | Media Foundation `SourceReader`（D3D11/DXVA 硬解）+ `SetCurrentPosition` 精确 seek + 时间戳驱动的呈现时钟 |
| 8 | `ScreenCaptureThread` 依赖第三方 `dxcam` | 额外依赖、多屏/独占场景出错 | `Windows.Graphics.Capture` + `GraphicsCaptureItem`（原生、支持 Win11 窗口捕获） |
| 9 | `SMTCCaptureThread` 用 `asyncio.run()` + `wait_for(2s)` 每 2 秒轮询 | 每轮新建事件循环；winsdk 卡死靠超时兜底 | C++/WinRT 原生异步 `GlobalSystemMediaTransportControlsSessionManager` + `MediaPropertiesChanged` 事件订阅（推送而非轮询） |
| 10 | 参数靠 6 个 getter 回调逐帧读取 UI 控件 | UI 与线程强耦合、无法测试 | 不可变 `RenderParams` 快照 + 原子交换 |
| 11 | 控件变化 → `config.set()` → **立即整文件 json.dump** | 拖滑杆时疯狂写盘 | 去抖 300ms + 原子写（临时文件 + `MoveFileEx` 替换） |
| 12 | `stop_display()` 对每个线程 `wait(3000)` | UI 线程最多阻塞 3 秒 | 异步停止（`fire_and_forget` + 完成回调），UI 永不阻塞 |
| 13 | 发帧失败 → `stop_display()` + **模态对话框** | 一次 USB 抖动就打断播放并要求用户手动恢复 | 传输层重试 + 连续 K 帧失败才降级为"未连接"；非模态 InfoBar 提示；热插拔后自动恢复播放 |
| 14 | 无掉线/热插拔处理（拔掉 USB 即彻底失败） | 必须重启程序 | `CM_Register_Notification` 监听设备接口变化，自动重建接口并续播 |
| 15 | 素材缩略图每次刷新都用 OpenCV 全解码图片/视频首帧 | 素材多时列表卡顿、内存尖峰 | WIC 缩略图（`IWICBitmapScaler`）+ 后台批量 + LRU 缓存 |
| 16 | `tuf_hid.open()` 对同一接口 CreateFile 两次 | 句柄浪费、竞态 | 一次 CreateFile，用该句柄查 caps |
| 17 | 全部错误处理 = `print()` / 弹窗，无日志 | 用户无法自助排查 | 结构化日志（`%LOCALAPPDATA%` 或 `v2\logs\`）+ 设置页"打开日志"按钮 |
| 18 | 转码用 `subprocess` + 轮询进度文件 | 进程管理粗糙、无取消 | `CreateProcess` + 作业对象（Job Object）保证 ffmpeg 子进程随应用退出 + 管道读进度 |

### 6.6 v2 实施顺序建议（增量可验证）

1. **骨架 + 空壳**：解决方案、三个项目、`NavigationView + Frame + Mica + 自定义标题栏 + 主题切换`。
   验证点：深浅模式切换 Mica 正确、标题栏可拖动、两个页面可导航。
2. **Device 层**：`HidEnumerator` + `HidTransport` + `FrameChunker`（含单测用例）。
   验证点：能枚举出 441/1025 两个接口；用一张预置 JPEG 直接发帧，屏幕出图；亮度/开关命令有效。
3. **Imaging 层**：`FramePipeline`（fit/rotate/WIC 编码），复用 ② 的发送路径。
   验证点：三种适配模式 + 四向旋转 + 颜色正确（无红蓝互换）。
4. **Core 层**：`AppConfig` + `StaticImageSource` + `VideoFrameSource` + `PlaybackController`。
   验证点：照片/视频播放、切源无闪烁、参数热更新、掉线重连。
5. **UI 完善**：主控页（预览节流 + 素材网格 + 转码进度）、设置页（全部本期配置项）、关于页。
6. **收尾**：`README` 修订、`v2\.gitignore`、日志、单测、打 `v2.0` tag。

---

## 7. Python v1 的当前局限（v2 的明确目标）

### 7.1 启动速度
- 冷启动要导入 `cv2`、`numpy`、`PySide6`（含 Qt 插件扫描）、`dxcam`，PySide6+OpenCV 的 DLL 加载常在数百毫秒到 1 秒以上。
- 之后在**主线程同步**做：HID 枚举（对每个候选接口 CreateFile 两次 + HidP_GetCaps）、
  读配置、**恢复显示内容时 `imread` 或 `VideoCapture` 打开素材**、生成预览。
- 首帧前还会做一次 `set_screen` + `set_brightness`，其中 `wake()` 里有 `time.sleep(0.1)`（v1 内部路径）。
- **估算：冷启动到首帧约 2–4 秒，期间窗口无响应**（本机未实测，因 shell 不可用）。
- **v2 目标**：窗口 300ms 内可见（先呈现 UI，设备枚举/素材加载异步），**首帧 < 1s**，
  主线程全程不阻塞。

### 7.2 内存
- OpenCV + numpy + PySide6/Qt 常驻，进程基线通常在 150–300MB 量级。
- 素材缩略图对每张照片 `imread` 全解码、对每个视频 `VideoCapture` 读首帧，
  素材多时（本仓库 `assets/photos` 已有 40+ 张）内存与时间都会爬升。
- 视频解码缓冲 + 每帧 ndarray 分配/释放造成常态性 GC。
- **v2 目标**：常驻工作集 < 80MB（不含视频解码器/转码子进程），缩略图 LRU 上限 + WIC 缩放解码。

### 7.3 渲染性能
- 每帧固定开销链：`fit_frame` resize（`INTER_LINEAR`，缩小时不如下采样专用的 `INTER_AREA`）
  → `draw_overlay` 多次 `copy()`+`addWeighted` → `rotate_frame` → `imencode`（单线程 CPU）
  → 跨线程投递 ndarray → UI 侧 `ascontiguousarray` + `QImage.copy` → `setPixmap`（又一次上传）。
  即"每帧 3–4 次全幅拷贝 + 1 次 JPEG 编码 + 1 次 Qt 图像上传"，且全部在 Python 层调度。
- `send_jpeg_frame` 的块间 `time.sleep` 进一步压低可达帧率（视频路径 `block_delay=0.0` 已优化，但照片路径是 0.001）。
- 目标 60fps 时 CPU 占用高，且帧率随 JPEG 质量（块数）波动。
- **v2 目标**：视频稳定 60fps 且 CPU 占用显著下降；几何变换 SIMD/GPU；JPEG 编码多线程；
  预览节流；静止画面心跳降频（这是最大的能耗收益点）。

### 7.4 稳定性
1. **无掉线恢复**：USB 抖动 / 设备重插 / InfoHub 抢占 → 线程 `return` + 弹窗 + 停止显示，必须手动重启。
2. **发帧失败即中断**：`send_jpeg_frame` 单块重试 3 次后仍失败也只是置假，上层看 `ok_all` 为假立刻终止。
3. **UI 阻塞**：`stop_display()` 逐线程 `wait(3000)`；错误用模态 `QMessageBox`。
4. **配置写入无节流、非原子**：拖滑杆触发高频全量写；崩溃可能产生半截 JSON
   （`load()` 捕获异常后回退默认值，等于**静默丢配置**）。
5. **依赖未声明**：`README` 的依赖段只列了 Python/OpenCV/FFmpeg，实际还硬依赖
   **PySide6、numpy、dxcam、winsdk**，而仓库里**没有 `requirements.txt`**
   （`README` 却写 `pip install -r requirements.txt`）。
6. **启动脚本路径失效**：`启动GUI.bat` 里 `cd /d "D:\TUFScreenControl\PyTufGUI"`
   指向旧项目位置，在仓库根目录下运行会 `cd` 到不存在或过期的目录。
7. **协议报告编号瑕疵**：`PROTOCOL_REVERSE_REPORT.md` 的 §4.2 与 §4.3 编号重复、
   正文引用"见 4.3"实际指的是自己，属文档整理问题（不改结论，v2 文档期一并修正）。
8. **无日志**：全部错误走 `print()` 或弹窗，用户侧无法自助定位。
9. **无测试**：`FrameChunker` 这类纯逻辑（分块头字节）没有任何黄金用例保护，
   而它是整个项目最容易写错、错了又最难察觉的地方。

**v2 稳定性目标**：设备掉线自动重连并续播；帧发送失败有分层重试与降级，不弹模态框、不中断播放；
配置原子写 + 去抖；所有错误有日志；协议分块逻辑有单测。

---

## 8. 已确认的决策与剩余阻塞项

### 8.1 已确认（你已拍板，规划按此执行）

| 项 | 决策 |
|---|---|
| 本期范围 | **桌面实时投射 + SMTC 媒体封面捕获均暂缓**，UI 灰显占位；本期只做 HID 通信 / 视频图片播放 / 自定义显示（文本、时钟、素材）/ 配置系统 |
| 视频解码 | **Media Foundation**（系统原生 + D3D11/DXVA 硬解，无第三方运行时依赖） |
| HID 实现 | **沿用 Win32 `WriteFile` 路径**（与已验证的 Python 实现逐位一致，规避 WinRT `SendOutputReport` 的报告 ID 语义差异），封装成 C++ 类 |
| 深浅色持久化 | **新增 `app_theme` 键**（System / Light / Dark） |
| 配置存储 | **改为 `%LOCALAPPDATA%\AsusAioScreenControl\config.json`**，并实现 **v1→v2 迁移逻辑**（含相对路径→绝对路径重定位），v1 的 config.json 保持只读不动 |
| 单元测试 | **建测试工程**，至少覆盖 `FrameChunker`（协议分块黄金用例）与 `AppConfig`（默认值合并 / 未知键保留 / 旧值归一 / v1 迁移） |

### 8.2 ⚠️ 剩余阻塞项：shell 与 git 不可用（**必须先解决，否则无法开工**）

本次分析期间，**每一条 `pwsh` 调用都在沙箱建立阶段失败**：

```
Error: SetNamedSecurityInfoW failed (Win32 5): grantWrite(D:\asus-aio-screen-control)
```

即 DSH 无法为工作区授予写 ACL（Win32 5 = ACCESS_DENIED）。连带影响：

1. **无法执行开工前环境验证** —— `vcvars64.bat`、MSVC 14.44、Windows SDK 10.0.26100、
   Windows App SDK / WinUI3 C++ 组件与项目模板，一个都没能实测确认。
   这一项是你在任务书里明确要求的，且"发现缺失要立即报告、不擅自安装或改用替代"。
2. **无法执行任何 git 操作** —— `git status` / `add` / `commit` / `push` 全部不可用，
   而工作规范要求每步先看 `git status`、每个有意义的改动都要提交推送。
   在此项修复前，v2 骨架即使写出来也无法提交，会直接违反规范。
3. 文件读写工具（read / write / glob / grep）正常，所以本文档得以完成。
4. `diagnose-windows-sandbox-acl` 技能本次也加载失败：
   `ENOENT: ...\Temp\dsh-acl-skill-b1pfxW\SKILL.md`（技能载荷文件缺失），无法用来自动修复 ACL。

**请先处理这一项**（任一即可）：
- 重启一次 DSH 会话让沙箱重建 ACL；或
- 调整工作区权限设置；或
- 在下次让我执行环境验证命令时，批准一次 `danger-full-access` 提权重试。

修复后我会**立即先做环境验证**并在任何缺失/不匹配出现时报告，验证通过再进入 §6.6 的步骤 1。

---

*文档状态：只读分析产出，未修改仓库中任何既有文件，未创建任何 C++ 代码。*
