#pragma once

// ---------------------------------------------------------------------------
// MinimalJson.h — 极简 JSON 读取器（只读，够用即可）
//
// 为什么不用第三方 JSON 库：v2 只需要「读取 v1 配置里的已知键」这一件事。
// nlohmann/json 会引入一个大头文件依赖，Windows.Data.Json 会把配置层绑到 WinRT。
// 这个实现只做只读解析（对象/数组/字符串/数字/布尔/null），约 150 行，
// 足以完成 v1 -> v2 的配置迁移与加载。
//
// 不做的事：写入（保存时按固定格式手写）、转义序列之外的高级 JSON 特性。
// ---------------------------------------------------------------------------

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ScreenControl::Core::Json
{
    enum class Type
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    class Value
    {
    public:
        Type type = Type::Null;
        bool boolean = false;
        double number = 0.0;
        std::wstring text;
        std::vector<Value> items;
        std::map<std::wstring, Value> members;

        bool IsNull() const noexcept { return type == Type::Null; }
        bool IsObject() const noexcept { return type == Type::Object; }
        bool IsArray() const noexcept { return type == Type::Array; }
        bool IsNumber() const noexcept { return type == Type::Number; }
        bool IsString() const noexcept { return type == Type::String; }
        bool IsBool() const noexcept { return type == Type::Bool; }

        /// 取成员；不存在时返回一个静态的 Null 值。
        Value const& operator[](std::wstring const& key) const;

        /// 便捷取值：类型不符时返回 fallback。
        bool AsBool(bool fallback) const noexcept;
        int AsInt(int fallback) const noexcept;
        double AsDouble(double fallback) const noexcept;
        std::wstring AsString(std::wstring const& fallback) const;
        std::vector<std::wstring> AsStringArray() const;
    };

    /// 解析 UTF-8 JSON 文本。失败时返回 type == Null 的值。
    Value Parse(std::string const& utf8);
}
