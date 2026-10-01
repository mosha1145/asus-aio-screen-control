#include "pch.h"
#include "AppConfig.h"
#include "MinimalJson.h"

#include <shlobj.h>

#include <fstream>
#include <sstream>

#pragma comment(lib, "shell32.lib")

namespace ScreenControl::Core
{
    namespace
    {
        /// v1 仓库根的判定标志：同时存在这些文件才认为是仓库根。
        bool LooksLikeRepoRoot(std::filesystem::path const& dir)
        {
            std::error_code ec;
            return std::filesystem::exists(dir / L"tuf_gui.py", ec) &&
                   std::filesystem::exists(dir / L"config_store.py", ec);
        }

        /// 从当前模块位置向上找仓库根（v2 构建产物在 <repo>\v2\out\...，需要上溯 3 层）。
        std::filesystem::path FindRepoRoot()
        {
            wchar_t modulePath[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) == 0)
            {
                return {};
            }
            std::filesystem::path dir = std::filesystem::path(modulePath).parent_path();
            for (int depth = 0; depth < 8 && !dir.empty(); ++depth)
            {
                if (LooksLikeRepoRoot(dir))
                {
                    return dir;
                }
                const std::filesystem::path parent = dir.parent_path();
                if (parent == dir)
                {
                    break;
                }
                dir = parent;
            }
            return {};
        }

        std::string ReadAllBytes(std::filesystem::path const& path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                return {};
            }
            std::ostringstream buffer;
            buffer << in.rdbuf();
            return buffer.str();
        }

        /// UTF-8 -> UTF-16
        std::wstring WidenUtf8(std::string const& text)
        {
            if (text.empty())
            {
                return {};
            }
            const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                                   static_cast<int>(text.size()), nullptr, 0);
            std::wstring out(static_cast<std::size_t>(needed > 0 ? needed : 0), L'\0');
            if (needed > 0)
            {
                MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                    out.data(), needed);
            }
            return out;
        }

        /// UTF-16 -> UTF-8
        std::string NarrowUtf8(std::wstring const& text)
        {
            if (text.empty())
            {
                return {};
            }
            const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                                   static_cast<int>(text.size()), nullptr, 0,
                                                   nullptr, nullptr);
            std::string out(static_cast<std::size_t>(needed > 0 ? needed : 0), '\0');
            if (needed > 0)
            {
                WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                    out.data(), needed, nullptr, nullptr);
            }
            return out;
        }

        std::string JsonEscape(std::string const& text)
        {
            std::string out;
            out.reserve(text.size() + 8);
            for (char const c : text)
            {
                switch (c)
                {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        char buf[8]{};
                        sprintf_s(buf, "\\u%04x", static_cast<unsigned char>(c));
                        out += buf;
                    }
                    else
                    {
                        out.push_back(c);
                    }
                    break;
                }
            }
            return out;
        }

        std::wstring ToLower(std::wstring value)
        {
            for (wchar_t& c : value)
            {
                c = static_cast<wchar_t>(::towlower(c));
            }
            return value;
        }
    }

    std::filesystem::path ConfigStore::DefaultConfigPath()
    {
        PWSTR localAppData = nullptr;
        std::filesystem::path base;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) &&
            localAppData != nullptr)
        {
            base = localAppData;
            CoTaskMemFree(localAppData);
        }
        if (base.empty())
        {
            base = std::filesystem::temp_directory_path();
        }
        return base / L"AsusAioScreenControl" / L"config.json";
    }

    std::filesystem::path ConfigStore::DefaultLegacyConfigPath()
    {
        // 优先从模块位置推导仓库根，避免硬编码绝对路径（目录改名/移动后仍可用）。
        const std::filesystem::path repoRoot = FindRepoRoot();
        if (!repoRoot.empty())
        {
            return repoRoot / L"config.json";
        }
        return {};   // 找不到就当作没有 v1 配置
    }

    ConfigStore::ConfigStore(std::filesystem::path configPath)
        : m_path(std::move(configPath))
    {
    }

    std::filesystem::path ConfigStore::AssetRoot() const
    {
        const std::filesystem::path repoRoot = FindRepoRoot();
        return repoRoot.empty() ? std::filesystem::path{} : repoRoot / L"assets";
    }

    std::wstring ConfigStore::ResolveMediaPath(std::wstring const& stored,
                                               std::filesystem::path const& legacyRoot)
    {
        if (stored.empty())
        {
            return {};
        }
        std::filesystem::path p(stored);
        if (p.is_absolute())
        {
            return stored;   // 外部文件：原样保留
        }
        // v1 存的是相对仓库根的路径，配置基目录变了以后必须重定位成绝对路径。
        if (legacyRoot.empty())
        {
            return stored;
        }
        return (legacyRoot / p).lexically_normal().wstring();
    }

    bool ConfigStore::Load()
    {
        std::error_code ec;

        if (std::filesystem::exists(m_path, ec))
        {
            const Json::Value root = Json::Parse(ReadAllBytes(m_path));
            if (root.IsObject())
            {
                ApplyJson(root);
                return false;
            }
            // 配置损坏：保留一份 .broken 便于排查，然后退回默认值。
            std::filesystem::path broken = m_path;
            broken += L".broken";
            std::filesystem::copy_file(m_path, broken,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            return false;
        }

        const std::filesystem::path legacy = DefaultLegacyConfigPath();
        if (!legacy.empty() && std::filesystem::exists(legacy, ec))
        {
            return MigrateFromLegacy(legacy);
        }
        return false;
    }

    bool ConfigStore::MigrateFromLegacy(std::filesystem::path const& legacyPath)
    {
        const Json::Value root = Json::Parse(ReadAllBytes(legacyPath));
        if (!root.IsObject())
        {
            return false;
        }

        ApplyJson(root);

        // 关键一步：v1 的素材路径是相对仓库根的，配置基目录变了就会失效，
        // 因此迁移时统一重定位为绝对路径。
        const std::filesystem::path legacyRoot = legacyPath.parent_path();
        if (!m_config.photoPath.empty())
        {
            m_config.photoPath = ResolveMediaPath(m_config.photoPath, legacyRoot);
        }
        if (!m_config.videoPath.empty())
        {
            m_config.videoPath = ResolveMediaPath(m_config.videoPath, legacyRoot);
        }

        // 注意：绝不删除/修改 v1 的 config.json —— 用户可随时回退到 v1。
        Save();
        return true;
    }

    void ConfigStore::ApplyJson(Json::Value const& root)
    {
        // screen_on / brightness 等默认值沿用 v1 代码默认值，缺键就不动。
        m_config.screenOn = root[L"screen_on"].AsBool(m_config.screenOn);
        m_config.brightness = root[L"brightness"].AsInt(m_config.brightness);
        m_config.fitMode = root[L"fit_mode"].AsString(m_config.fitMode);
        m_config.rotation = root[L"rotation"].AsInt(m_config.rotation);

        m_config.displayType = root[L"display_type"].AsString(m_config.displayType);
        m_config.photoPath = root[L"photo_path"].AsString(m_config.photoPath);
        m_config.videoPath = root[L"video_path"].AsString(m_config.videoPath);
        m_config.playbackMode = root[L"playback_mode"].AsString(m_config.playbackMode);

        m_config.photoFps = root[L"photo_fps"].AsDouble(m_config.photoFps);
        m_config.videoQuality = root[L"video_quality"].AsString(m_config.videoQuality);

        m_config.autoStart = root[L"auto_start"].AsBool(m_config.autoStart);
        m_config.appTheme = ToLower(root[L"app_theme"].AsString(m_config.appTheme));

        m_config.powerActions = root[L"power_actions"].AsBool(m_config.powerActions);
        m_config.powerCloseApp = root[L"power_close_app"].AsBool(m_config.powerCloseApp);
        m_config.powerShutdown = root[L"power_shutdown"].AsBool(m_config.powerShutdown);
        m_config.powerLogout = root[L"power_logout"].AsBool(m_config.powerLogout);
        m_config.powerSleep = root[L"power_sleep"].AsBool(m_config.powerSleep);

        // v1 曾有过 overlay_type == "hwinfo"（硬件信息叠加，已移除），归一到 none。
        std::wstring overlay = ToLower(root[L"overlay_type"].AsString(m_config.overlayType));
        if (overlay == L"hwinfo")
        {
            overlay = L"none";
        }
        m_config.overlayType = overlay;
        m_config.clockStyle = ToLower(root[L"clock_style"].AsString(m_config.clockStyle));

        m_config.smtcFilterMode = root[L"smtc_filter_mode"].AsString(m_config.smtcFilterMode);
        m_config.smtcFilterApps = root[L"smtc_filter_apps"].AsStringArray();

        m_config.schemaVersion = root[L"schema_version"].AsInt(m_config.schemaVersion);
    }

    bool ConfigStore::Save() const
    {
        std::error_code ec;

        if (!m_path.parent_path().empty())
        {
            std::filesystem::create_directories(m_path.parent_path(), ec);
        }

        std::ostringstream out;
        out << "{\n";
        out << "  \"schema_version\": " << m_config.schemaVersion << ",\n";
        out << "  \"screen_on\": " << (m_config.screenOn ? "true" : "false") << ",\n";
        out << "  \"brightness\": " << m_config.brightness << ",\n";
        out << "  \"display_type\": \"" << JsonEscape(NarrowUtf8(m_config.displayType)) << "\",\n";
        out << "  \"photo_path\": " << (m_config.photoPath.empty()
                                           ? "null"
                                           : "\"" + JsonEscape(NarrowUtf8(m_config.photoPath)) + "\"") << ",\n";
        out << "  \"video_path\": " << (m_config.videoPath.empty()
                                           ? "null"
                                           : "\"" + JsonEscape(NarrowUtf8(m_config.videoPath)) + "\"") << ",\n";
        out << "  \"fit_mode\": \"" << JsonEscape(NarrowUtf8(m_config.fitMode)) << "\",\n";
        out << "  \"photo_fps\": " << m_config.photoFps << ",\n";
        out << "  \"video_quality\": \"" << JsonEscape(NarrowUtf8(m_config.videoQuality)) << "\",\n";
        out << "  \"rotation\": " << m_config.rotation << ",\n";
        out << "  \"playback_mode\": \"" << JsonEscape(NarrowUtf8(m_config.playbackMode)) << "\",\n";
        out << "  \"auto_start\": " << (m_config.autoStart ? "true" : "false") << ",\n";
        out << "  \"app_theme\": \"" << JsonEscape(NarrowUtf8(m_config.appTheme)) << "\",\n";
        out << "  \"power_actions\": " << (m_config.powerActions ? "true" : "false") << ",\n";
        out << "  \"power_close_app\": " << (m_config.powerCloseApp ? "true" : "false") << ",\n";
        out << "  \"power_shutdown\": " << (m_config.powerShutdown ? "true" : "false") << ",\n";
        out << "  \"power_logout\": " << (m_config.powerLogout ? "true" : "false") << ",\n";
        out << "  \"power_sleep\": " << (m_config.powerSleep ? "true" : "false") << ",\n";
        out << "  \"overlay_type\": \"" << JsonEscape(NarrowUtf8(m_config.overlayType)) << "\",\n";
        out << "  \"clock_style\": \"" << JsonEscape(NarrowUtf8(m_config.clockStyle)) << "\",\n";
        out << "  \"smtc_filter_mode\": \"" << JsonEscape(NarrowUtf8(m_config.smtcFilterMode)) << "\",\n";
        out << "  \"smtc_filter_apps\": [";
        for (std::size_t i = 0; i < m_config.smtcFilterApps.size(); ++i)
        {
            out << (i ? ", " : "") << "\"" << JsonEscape(NarrowUtf8(m_config.smtcFilterApps[i])) << "\"";
        }
        out << "]\n";
        out << "}\n";

        // 原子写：先写临时文件，再替换目标，避免崩溃留下半截 JSON（v1 的隐患）。
        std::filesystem::path temp = m_path;
        temp += L".tmp";
        {
            std::ofstream file(temp, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                return false;
            }
            const std::string text = out.str();
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!file)
            {
                return false;
            }
        }

        if (!MoveFileExW(temp.c_str(), m_path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            std::filesystem::remove(temp, ec);
            return false;
        }
        return true;
    }
}
