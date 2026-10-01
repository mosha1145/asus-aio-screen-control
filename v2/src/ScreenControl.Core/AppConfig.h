// ============================================================================
//  AppConfig.h — 应用配置（对应 v1 config_store.py）
// ----------------------------------------------------------------------------
//  v1 行为（必须兼容）：
//    * DEFAULT_CONFIG 打底 + 文件值覆盖，未知键保留 -> 天然向前兼容
//    * set() = 改内存 + 立即整文件重写（滑杆拖动时疯狂写盘，v2 要改成去抖 + 原子写）
//    * 素材路径相对仓库根存储，软件目录搬移后仍有效
//
//  v2 变更（已确认）：
//    * 配置位置改为 %LOCALAPPDATA%\AsusAioScreenControl\config.json
//    * 首次启动从仓库根 config.json 迁移，并把相对路径重定位为绝对路径
//      （因为配置基目录变了，v1 存储的相对路径会失效）
//    * 绝不修改/删除 v1 的 config.json，用户可随时回退到 v1
// ============================================================================
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ScreenControl::Core::Json
{
    class Value;
}

namespace ScreenControl::Core
{
    /// 与 v1 config_store.DEFAULT_CONFIG 一一对应。
    /// 注意：默认值必须沿用 v1 的「代码默认值」，否则老用户升级后观感会变。
    struct AppConfig
    {
        // ---- 屏幕 ----
        bool screenOn = true;
        int brightness = 50;                 ///< 0-100（v1 代码默认 50）
        std::wstring fitMode = L"contain";   ///< contain / cover / stretch
        int rotation = 0;                    ///< 0 / 90 / 180 / 270

        // ---- 显示内容（内部状态） ----
        std::wstring displayType;            ///< photo / video / screen / smtc / 空
        std::wstring photoPath;              ///< 迁移后为绝对路径
        std::wstring videoPath;              ///< 迁移后为绝对路径
        std::wstring playbackMode = L"single";   ///< single / list

        // ---- 播放 ----
        double photoFps = 10.0;              ///< 0.1 - 30
        std::wstring videoQuality = L"medium";   ///< low / medium / high

        // ---- 启动 ----
        bool autoStart = false;

        // ---- 界面（v2 新增） ----
        std::wstring appTheme = L"system";   ///< system / light / dark

        // ---- 电源联动（本期暂缓，键保留以兼容 v1 配置） ----
        bool powerActions = true;
        bool powerCloseApp = true;
        bool powerShutdown = false;
        bool powerLogout = false;
        bool powerSleep = false;

        // ---- 覆盖层 ----
        std::wstring overlayType = L"none";  ///< none / clock / custom（旧值 hwinfo 归一为 none）
        std::wstring clockStyle = L"text";   ///< text / analog

        // ---- 采集（本期暂缓，键保留） ----
        std::wstring smtcFilterMode = L"all";
        std::vector<std::wstring> smtcFilterApps;

        /// 配置结构版本，用于将来做版本化升级。
        int schemaVersion = 2;
    };

    /// 配置存储：加载 / 保存 / 迁移。
    class ConfigStore
    {
    public:
        /// v1 配置位置（仓库根，只读，不修改）。
        static std::filesystem::path DefaultLegacyConfigPath();

        /// v2 配置位置（%LOCALAPPDATA%\AsusAioScreenControl\config.json）。
        static std::filesystem::path DefaultConfigPath();

        explicit ConfigStore(std::filesystem::path configPath = DefaultConfigPath());

        /// 加载配置。若 v2 配置不存在而 v1 配置存在，则执行一次性迁移。
        /// 返回值表示是否发生了迁移。
        bool Load();

        /// 保存配置（原子写：临时文件 + 替换）。
        bool Save() const;

        AppConfig& Data() noexcept { return m_config; }
        AppConfig const& Data() const noexcept { return m_config; }

        /// 当前配置文件的完整路径。
        std::filesystem::path const& Path() const noexcept { return m_path; }

        /// 素材库根目录（v2 仍指向仓库内的 assets\，素材是用户数据不搬走）。
        std::filesystem::path AssetRoot() const;

        /// 把配置里的相对素材路径重定位为绝对路径（迁移时使用）。
        static std::wstring ResolveMediaPath(std::wstring const& stored,
                                             std::filesystem::path const& legacyRoot);

    private:
        bool MigrateFromLegacy(std::filesystem::path const& legacyPath);

        /// 把解析好的 JSON 合并进 m_config（缺键保持默认值 -> 向前兼容）。
        void ApplyJson(Json::Value const& root);

        std::filesystem::path m_path;
        AppConfig m_config;
    };
}
