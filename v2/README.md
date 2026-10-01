# ASUS AIO Screen Control — v2（C++ / WinUI 3）

> v1（Python / PySide6）的功能等价重构版。设计依据见仓库根目录的
> [`project_understanding_v2.md`](../project_understanding_v2.md)。

当前状态：**工程骨架阶段**（不含完整功能实现）。

---

## 工程结构

```
v2\
├─ AsusAioScreenControl.sln          解决方案（Debug|x64 / Release|x64）
├─ Directory.Build.props             共享属性（依赖版本、原生桌面框架口径）
├─ Directory.Build.targets           WinUI/WinAppSDK WinMD 元数据引用
├─ NuGet.config                      nuget.org 源
├─ .gitignore                        v2 构建产物忽略规则
└─ src\
   ├─ ScreenControl.Device\          HID 协议层（静态库，不依赖 WinUI，可单测）
   ├─ ScreenControl.Imaging\         帧处理层（静态库：几何变换 + JPEG 编码）
   ├─ ScreenControl.Core\            应用逻辑层（静态库：配置 / 素材 / 播放编排）
   └─ ScreenControl.App\             WinUI 3 应用（非打包，C++/WinRT）
```

依赖方向单向：`App → Core → {Device, Imaging}`，下层不反向依赖 UI。

---

## 构建

依赖（本机已就绪，无需安装）：

| 组件 | 版本 |
|---|---|
| Visual Studio 2022 Community | 17.14（`D:\Dev\VS2022`） |
| MSVC 编译器 | 14.44（`VC\Tools\MSVC\14.44.35207`） |
| Windows SDK | 10.0.26100 |
| Windows App SDK | 1.8.250916003（NuGet） |
| Microsoft.Windows.CppWinRT | 2.0.240111.5（NuGet） |
| Windows App Runtime | 1.8（系统已安装，非打包模式运行时需要） |

命令行构建：

```powershell
$msbuild = 'D:\Dev\VS2022\MSBuild\Current\Bin\MSBuild.exe'
$sln     = 'D:\asus-aio-screen-control\v2\AsusAioScreenControl.sln'

# 首次需要还原 NuGet 包
& $msbuild $sln /restore /p:Configuration=Debug /p:Platform=x64
```

输出统一落在 `v2\out\<Platform>\<Configuration>\`。

或用 Visual Studio 打开 `AsusAioScreenControl.sln`，选 **Debug x64** 直接生成。

---

## 用命令行构建 WinUI 3 C++ 工程时的关键坑位（已实测踩平，勿回退）

这些是让 `MSBuild` 能干净构建本解决方案的必要条件，改动前请先读 `Directory.Build.props`
和 `Directory.Build.targets` 里的注释。

1. **框架口径必须显式设成原生桌面。**
   WinUI 3 模板设 `<ApplicationType>Windows Store</ApplicationType>`，会让
   `Microsoft.NuGet.targets` 推断出 `TargetPlatformIdentifier=UAP` 并注入
   `_NuGetTargetFallbackMoniker=UAP,Version=v10.0`，而 WindowsAppSDK 的 NuGet 包不提供
   UAP 目标，于是报 `Your project does not reference "UAP,Version=v10.0" framework`。
   修正：`TargetPlatformIdentifier=Windows` +
   `TargetFrameworkIdentifier=native` / `TargetFrameworkVersion=v0.0` / `TargetFramework=native,Version=v0.0`。

2. **XAML 项必须「关闭默认项发现 + 显式声明 `<Page>`」**，两者缺一不可：
   * 只留默认项发现（`EnableDefaultPageItems=true` 且不写 `<Page>`）→ WinUI 的
     `CompileXaml` 任务虽然执行但**不产出代码**，`*.xaml.g.h` 不生成，
     编译报 `InitializeComponent 找不到标识符`。
   * 默认项 + 显式项同时存在 → 重复项，同样不产出代码。
   正确组合：`EnableDefaultPageItems=false`，然后在 vcxproj 里显式写
   `<Page Include="..." ><XamlRuntime>WinUI</XamlRuntime></Page>`。

3. **`Generated Files` 必须加入 `IncludePath`。**
   XAML 生成头（`*.g.h`）落在项目目录下的 `Generated Files\`，不在默认包含路径里。

4. **WinMD 元数据引用要放在 `Directory.Build.targets`，不能放 `.props`。**
   `NuGetPackageRoot` 是在 NuGet 的 `*.nuget.g.props` 里才赋值的，而它在
   `Directory.Build.props` **之后**导入；在 `.props` 里求值会得到
   `\microsoft.windowsappsdk...` 这种没有盘符的路径，`Reference` 静默失效，
   随后 MIDL 报 `unresolved type declaration`、cppwinrt 报
   `Type 'Microsoft.Web.WebView2.Core.CoreWebView2' could not be found`。

5. **框架相关标签要用 `$(MSBuildThisFileDirectory)` 相对表达，不要用 `$(Platform)`。**
   `Directory.Build.props` 求值时机早于 `Microsoft.Cpp.Default.props`，此时
   `$(Platform)` 可能为空，拼出 `out\obj\\\<Proj>\` 这类畸形路径。

6. **非打包运行需要系统安装 Windows App Runtime。**
   `WindowsAppSDKSelfContained=false` + `WindowsPackageType=None` 时，
   WindowsAppSDK 会自动注入 `MddBootstrapAutoInitializer.cpp`，**不要**再手写
   `MddBootstrapInitialize2`（会与自动初始化冲突，且自包含模式下会链接失败）。

7. **不要手动给 `Application` 赋 `ResourceDictionary`。**
   实测：在 `Application::Start` 回调里 `app.Resources(...)` 塞入
   `XamlControlsResources` 会直接让进程在 `Microsoft.UI.Xaml.dll` 中 fail-fast
   （退出码 `0xC000027B`，事件日志里的 stowed exception）。
   留空即可正常启动。

8. **App 类用纯 C++/WinRT 定义，不使用 XAML `Application`。**
   本解决方案没有 `App.xaml`：命令行构建下 XAML `Application` 的代码生成不可靠，
   改用 `ApplicationT<App>` 的纯 C++ App 类（`AppClass.h/.cpp`）后不再需要生成头。

---

## 部署模式

**当前：自包含（self-contained）+ 非打包。**

`WindowsAppSDKSelfContained=true` 让 MSBuild 把 WinUI / Windows App SDK 的运行库
完整复制到输出目录，程序从本地加载，**不依赖系统安装的 Windows App Runtime 版本**。
Debug x64 输出目录约 159 MB / 137 个文件，含 `Microsoft.UI.Xaml.dll`（14.7 MB）、
`Microsoft.WindowsAppRuntime.dll`、`CoreMessagingXP.dll`、
`Microsoft.UI.Xaml.Controls.dll` 等。

**v2.0 发布形态：MSIX 打包（packaged）**，运行库作为依赖由打包机制自动带上。

---

## 已知问题

1. **`warning MSB8027`：`WindowsAppRuntimeAutoInitializer.cpp` 重复项（假阳性）。**
   成因：WindowsAppSDK 1.8 自带目标同时经 `build\native` 与 `buildTransitive\native`
   两条导入路径把同一份文件加进 `ClCompile`，两条路径展开后指向同一文件。
   已在中 `Directory.Build.targets` 的 `V2DeduplicateGeneratedClCompile`
   （挂在 `SetTargetPath` 之前）去重，**实测产物中每个自动初始化文件只编译出一个 .obj**
   （`WindowsAppRuntimeAutoInitializer.obj`、`MddBootstrapAutoInitializer.obj` 等各一份），
   即去重已生效、运行时初始化代码只保留一份。
   该警告由 `Microsoft.CppBuild.targets` 的 `WarnCompileDuplicatedFilename` 目标
   在 `ObjectFileName` 元数据层面发出，去重后仍会触发，属 SDK 侧的假阳性；
   需要彻底消除可设 `IgnoreWarnCompileDuplicatedFilename=true`（当前未设，
   以便真实的重名冲突仍能被发现）。

2. **运行时启动崩溃：`Window.Content(...)` 触发访问冲突（`0xC0000005`）。**
   已用逐行日志把范围缩到最小可复现：

   ```
   [App] OnLaunched entered          <- Application::Start 回调已进入，OnLaunched 正常触发
   [App] config loaded
   [MainWindow] ctor enter
   [MainWindow] Window() ok          <- Window 创建成功
   [MainWindow] Grid() ok
   [MainWindow] TextBlock props ok
   [MainWindow] Children().Append ok
   （到此为止，进程消失）
   ```

   崩溃点在 `m_window.Content(root)`，事件日志确认为
   `AsusAioScreenControl.exe` 自身模块内 `0xc0000005`（访问冲突），
   且**不可被 `catch (hresult_error)` 捕获**（是原生层访问冲突，不是可传播的 HRESULT）。

   已排除（不是这些原因）：
   * `Application::Start` 未调用 —— 已调用，`OnLaunched` 确认触发；
   * 消息循环 / `DispatcherQueueController` 缺失 —— WinUI 3 由
     `Application::Start` 内部建立并驱动消息泵，不需要手工建；
   * `init_apartment` 冲突 —— 已移除手工调用；
   * 窗口未 Activate —— 崩溃发生在 `Activate()` 之前；
   * 施工顺序 —— 曾把 `Window()` 提到最前，仍崩在同一句；
   * 主题资源缺失 —— `Application::Current().Resources().Lookup(...)`
     已换成 `TryLookup`（不再抛异常），崩溃点未变。

   当前判断：**`Window` 缺少与该运行时类型的关联**。本工程用「普通 C++ 类 +
   `Window` 成员」的方式建窗口，没有把窗口声明为 MIDL runtimeclass。
   尝试补回 `runtimeclass MainWindow : Microsoft.UI.Xaml.Window` 时，
   C++/WinRT 生成的 `ScreenControl` 投影里 `MainWindow` 是**空的**
   （`WINRT_EXPORT namespace winrt::ScreenControl { }`），
   拿不到 `MainWindowT`，因此无法编译。

   **结论：这条路的根因在上游 —— 命令行 MSBuild 下 WinUI 3 的 C++ 投影/代码生成
   不完整（与下面第 3 条 WMC1007 同源）。规范解法是用 Visual Studio 打开
   `AsusAioScreenControl.sln`、用官方「WinUI 3 桌面应用 (原生)」模板生成工程后开发
   XAML 界面；命令行构建仅适合验证静态库层（Device / Imaging / Core 均已通过）。**

3. **WinUI 3 的 C++ XAML 代码生成在命令行 MSBuild 下不产出。**
   标记编译 pass 1 执行，pass 2 报
   `XamlCompiler error WMC1007: Cannot resolve metadata for WinUI types`。
   已逐项验证未解决：原生框架口径、默认/显式 `<Page>` 项、增加 `<ApplicationDefinition>`、
   向 XAML 编译器显式喂 WinMD 元数据。该路径依赖 Visual Studio 项目系统注入的状态。

   当前界面代码（`MainWindow.cpp`，含 NavigationView + Frame + Mica +
   内容扩展标题栏 + 深浅色跟随系统的完整实现）保留在仓库中，但**会在启动时崩溃**。
   仓库当前保留的是一份最小化的调试版本（只放一个 TextBlock），
   以便下一轮直接用日志定位；完整界面实现见本文件历史版本 / git 记录。

---

## 已实现 / 未实现

### 已实现（骨架层面，可编译验证）

| 位置 | 内容 |
|---|---|
| `Device/FrameChunker` | **完整实现**。JPEG 按 1020 字节分块、首块 `[08][总块数][00][80]`、后续块 `[08][序号][00][00]`、末块补零。协议层最容易写错的部分，已独立成类便于单测。 |
| `Device/HidProtocol` | 设备常量、控制命令 ID、发送状态与 Win32 错误归类（区分「设备掉了」和「临时抖动」）。 |
| `Device/ScreenController` | 设备枚举（SetupDi + `HidP_GetCaps` 按 `OutputReportByteLength` 分类控制/图像接口）、唯一写出口（补零 + 前置 `0x00` 报告 ID + 写满 caps 长度）、亮度/开关命令、发帧（逐块 3 次重试）。 |
| `Imaging/FrameGeometry` | `contain` / `cover` / `stretch` 适配 + 四向旋转，画布预分配。 |
| `Imaging/RenderParams` | 不可变渲染参数快照，替代 v1「6 个 getter 回调逐帧读 UI」的做法。 |
| `Core/MinimalJson` | 零依赖只读 JSON 解析器（配置迁移与加载用，不引入第三方库）。 |
| `Core/AppConfig` | v1 全部配置键、默认值沿用 v1、合并式加载（缺键补默认 / 未知键忽略）、**原子写**、**v1→v2 一次性迁移**（含素材路径相对→绝对重定位）。 |
| `App/MainWindow` | NavigationView + Frame 导航（主控 / 设置）、**Mica 系统背衬**、**内容扩展到标题栏 + 自定义拖动区**、主题应用。 |
| `App/DisplayPage` | 主控页骨架：预览占位 + 屏幕开关 + 亮度 + 适配/旋转。 |
| `App/SettingsPage` | 应用内设置页骨架（常规 / 画面与播放 / 暂缓功能灰显 / 配置现状回显）。 |

### 未实现（后续迭代）

* JPEG 编码（WIC）与帧管线 `FramePipeline`（当前 `FrameGeometry` 的缩放为最近邻骨架版）
* 视频解码（Media Foundation）与图片/视频播放编排 `PlaybackController`
* 素材库（枚举 / 入库 / ffmpeg 转码 / 缩略图）
* 覆盖层绘制（时钟 / 自定义文本）
* 设备掉线重连与热插拔监听
* 开机自启（HKCU Run 键）
* 单元测试工程（`FrameChunker` 黄金用例、`AppConfig` 迁移用例）
* 日志

### 本期暂缓（明确不做）

温度/硬件信息监测、系统信息采集、电源联动、自定义主题、悬浮窗、
桌面实时投射（`Windows.Graphics.Capture`）、SMTC 媒体封面捕获。
相关配置键在 `AppConfig` 中保留以兼容 v1 配置，设置页对应项灰显占位。

---

## 配置

* v2 配置位置：`%LOCALAPPDATA%\AsusAioScreenControl\config.json`
* 首次启动若该文件不存在而仓库根 `config.json` 存在，则执行一次性迁移：
  复用 v1 的合并式加载，并把 `photo_path` / `video_path` 由相对仓库根的路径
  **重定位为绝对路径**（配置基目录变了，相对路径会失效）。
* **绝不修改或删除 v1 的 `config.json`**，用户可随时回退到 v1。
* 素材库仍在仓库内（`assets\photos` / `assets\videos`），属用户数据不搬迁。

---

## 许可与声明

个人开源项目，与华硕（ASUS）官方无关，不包含官方 InfoHub 软件的任何代码。
