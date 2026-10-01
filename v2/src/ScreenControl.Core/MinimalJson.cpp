#include "pch.h"
#include "MinimalJson.h"

#include <cstdlib>

namespace ScreenControl::Core::Json
{
    namespace
    {
        const Value kNullValue{};

        /// 把 UTF-8 字节串转成宽字符串。
        std::wstring Widen(std::string const& utf8)
        {
            if (utf8.empty())
            {
                return {};
            }
            const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                                   static_cast<int>(utf8.size()), nullptr, 0);
            if (needed <= 0)
            {
                return {};
            }
            std::wstring out(static_cast<std::size_t>(needed), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                out.data(), needed);
            return out;
        }

        class Parser
        {
        public:
            explicit Parser(std::string const& text) : m_text(text) {}

            Value Parse()
            {
                SkipWhitespace();
                Value value = ParseValue();
                return value;
            }

        private:
            void SkipWhitespace()
            {
                while (m_pos < m_text.size())
                {
                    const char c = m_text[m_pos];
                    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                    {
                        ++m_pos;
                    }
                    else
                    {
                        break;
                    }
                }
            }

            bool Consume(char expected)
            {
                SkipWhitespace();
                if (m_pos < m_text.size() && m_text[m_pos] == expected)
                {
                    ++m_pos;
                    return true;
                }
                return false;
            }

            Value ParseValue()
            {
                SkipWhitespace();
                if (m_pos >= m_text.size())
                {
                    return {};
                }

                switch (m_text[m_pos])
                {
                case '{': return ParseObject();
                case '[': return ParseArray();
                case '"':
                {
                    Value v;
                    v.type = Type::String;
                    v.text = ParseString();
                    return v;
                }
                case 't':
                case 'f':
                {
                    Value v;
                    v.type = Type::Bool;
                    v.boolean = ParseLiteralBool();
                    return v;
                }
                case 'n':
                    ParseLiteralNull();
                    return {};
                default:
                {
                    Value v;
                    v.type = Type::Number;
                    v.number = ParseNumber();
                    return v;
                }
                }
            }

            Value ParseObject()
            {
                Value object;
                object.type = Type::Object;
                if (!Consume('{'))
                {
                    return object;
                }
                SkipWhitespace();
                if (Consume('}'))
                {
                    return object;
                }
                for (;;)
                {
                    SkipWhitespace();
                    if (m_pos >= m_text.size() || m_text[m_pos] != '"')
                    {
                        break;
                    }
                    std::wstring key = ParseString();
                    if (!Consume(':'))
                    {
                        break;
                    }
                    object.members[key] = ParseValue();
                    if (Consume(','))
                    {
                        continue;
                    }
                    Consume('}');
                    break;
                }
                return object;
            }

            Value ParseArray()
            {
                Value array;
                array.type = Type::Array;
                if (!Consume('['))
                {
                    return array;
                }
                SkipWhitespace();
                if (Consume(']'))
                {
                    return array;
                }
                for (;;)
                {
                    array.items.push_back(ParseValue());
                    if (Consume(','))
                    {
                        continue;
                    }
                    Consume(']');
                    break;
                }
                return array;
            }

            std::wstring ParseString()
            {
                std::string raw;
                if (m_pos >= m_text.size() || m_text[m_pos] != '"')
                {
                    return {};
                }
                ++m_pos; // 开引号
                while (m_pos < m_text.size())
                {
                    const char c = m_text[m_pos++];
                    if (c == '"')
                    {
                        break;
                    }
                    if (c == '\\' && m_pos < m_text.size())
                    {
                        const char esc = m_text[m_pos++];
                        switch (esc)
                        {
                        case 'n': raw.push_back('\n'); break;
                        case 't': raw.push_back('\t'); break;
                        case 'r': raw.push_back('\r'); break;
                        case 'b': raw.push_back('\b'); break;
                        case 'f': raw.push_back('\f'); break;
                        case '/': raw.push_back('/'); break;
                        case '\\': raw.push_back('\\'); break;
                        case '"': raw.push_back('"'); break;
                        case 'u':
                        {
                            // 只处理基本多文种平面外的代理对与 BMP 字符。
                            auto readHex4 = [this](unsigned& out) -> bool
                            {
                                if (m_pos + 4 > m_text.size())
                                {
                                    return false;
                                }
                                out = 0;
                                for (int i = 0; i < 4; ++i)
                                {
                                    const char h = m_text[m_pos++];
                                    out <<= 4;
                                    if (h >= '0' && h <= '9') out |= static_cast<unsigned>(h - '0');
                                    else if (h >= 'a' && h <= 'f') out |= static_cast<unsigned>(h - 'a' + 10);
                                    else if (h >= 'A' && h <= 'F') out |= static_cast<unsigned>(h - 'A' + 10);
                                    else return false;
                                }
                                return true;
                            };

                            unsigned cp = 0;
                            if (!readHex4(cp))
                            {
                                break;
                            }
                            // UTF-16 代理对
                            if (cp >= 0xD800 && cp <= 0xDBFF && m_pos + 6 <= m_text.size() &&
                                m_text[m_pos] == '\\' && m_text[m_pos + 1] == 'u')
                            {
                                const std::size_t save = m_pos;
                                m_pos += 2;
                                unsigned low = 0;
                                if (readHex4(low) && low >= 0xDC00 && low <= 0xDFFF)
                                {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                                }
                                else
                                {
                                    m_pos = save;
                                }
                            }
                            // 编码为 UTF-8
                            if (cp < 0x80)
                            {
                                raw.push_back(static_cast<char>(cp));
                            }
                            else if (cp < 0x800)
                            {
                                raw.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                                raw.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                            }
                            else if (cp < 0x10000)
                            {
                                raw.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                                raw.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                                raw.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                            }
                            else
                            {
                                raw.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                                raw.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                                raw.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                                raw.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                            }
                            break;
                        }
                        default:
                            raw.push_back(esc);
                            break;
                        }
                        continue;
                    }
                    raw.push_back(c);
                }
                return Widen(raw);
            }

            double ParseNumber()
            {
                const std::size_t start = m_pos;
                while (m_pos < m_text.size())
                {
                    const char c = m_text[m_pos];
                    if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
                        c == 'e' || c == 'E')
                    {
                        ++m_pos;
                    }
                    else
                    {
                        break;
                    }
                }
                const std::string token = m_text.substr(start, m_pos - start);
                return token.empty() ? 0.0 : std::strtod(token.c_str(), nullptr);
            }

            bool ParseLiteralBool()
            {
                if (m_text.compare(m_pos, 4, "true") == 0)
                {
                    m_pos += 4;
                    return true;
                }
                if (m_text.compare(m_pos, 5, "false") == 0)
                {
                    m_pos += 5;
                    return false;
                }
                ++m_pos;
                return false;
            }

            void ParseLiteralNull()
            {
                m_pos += (m_text.compare(m_pos, 4, "null") == 0) ? 4 : 1;
            }

            std::string const& m_text;
            std::size_t m_pos = 0;
        };
    }

    Value const& Value::operator[](std::wstring const& key) const
    {
        const auto it = members.find(key);
        return (it == members.end()) ? kNullValue : it->second;
    }

    bool Value::AsBool(bool fallback) const noexcept
    {
        if (type == Type::Bool)
        {
            return boolean;
        }
        if (type == Type::Number)
        {
            return number != 0.0;
        }
        return fallback;
    }

    int Value::AsInt(int fallback) const noexcept
    {
        if (type == Type::Number)
        {
            return static_cast<int>(number);
        }
        if (type == Type::Bool)
        {
            return boolean ? 1 : 0;
        }
        return fallback;
    }

    double Value::AsDouble(double fallback) const noexcept
    {
        if (type == Type::Number)
        {
            return number;
        }
        return fallback;
    }

    std::wstring Value::AsString(std::wstring const& fallback) const
    {
        return (type == Type::String) ? text : fallback;
    }

    std::vector<std::wstring> Value::AsStringArray() const
    {
        std::vector<std::wstring> out;
        if (type != Type::Array)
        {
            return out;
        }
        out.reserve(items.size());
        for (Value const& item : items)
        {
            if (item.type == Type::String)
            {
                out.push_back(item.text);
            }
        }
        return out;
    }

    Value Parse(std::string const& utf8)
    {
        Parser parser(utf8);
        return parser.Parse();
    }
}
