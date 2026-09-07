#include "json_lite.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <sstream>
#include <iomanip>
#include <limits>
#include <locale>

namespace jsonlite {

std::wstring fromUtf8(const std::string &value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::string toUtf8(const std::wstring &value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

namespace {

class Parser final {
public:
    explicit Parser(std::wstring text) : text_(std::move(text)) {}

    bool parse(Value &value, std::string &error) {
        skipSpace();
        if (!parseValue(value, error)) return false;
        skipSpace();
        if (pos_ != text_.size()) return fail(error, "trailing characters");
        return true;
    }

private:
    bool fail(std::string &error, const char *message) {
        error = message;
        return false;
    }

    void skipSpace() {
        while (pos_ < text_.size() && (text_[pos_] == L' ' || text_[pos_] == L'\t' ||
                                       text_[pos_] == L'\r' || text_[pos_] == L'\n')) {
            ++pos_;
        }
    }

    bool parseValue(Value &value, std::string &error) {
        skipSpace();
        if (pos_ >= text_.size()) return fail(error, "unexpected end");
        switch (text_[pos_]) {
        case L'{': return parseObject(value, error);
        case L'[': return parseArray(value, error);
        case L'"': {
            std::wstring string;
            if (!parseString(string, error)) return false;
            value = Value(std::move(string));
            return true;
        }
        case L't': return parseLiteral(L"true", Value(true), value, error);
        case L'f': return parseLiteral(L"false", Value(false), value, error);
        case L'n': return parseLiteral(L"null", Value(nullptr), value, error);
        default: return parseNumber(value, error);
        }
    }

    bool parseLiteral(const wchar_t *literal, Value result, Value &value, std::string &error) {
        const size_t length = wcslen(literal);
        if (text_.compare(pos_, length, literal) != 0) return fail(error, "invalid literal");
        pos_ += length;
        value = std::move(result);
        return true;
    }

    bool parseString(std::wstring &value, std::string &error) {
        if (pos_ >= text_.size() || text_[pos_] != L'"') return fail(error, "expected string");
        ++pos_;
        value.clear();
        while (pos_ < text_.size()) {
            const wchar_t ch = text_[pos_++];
            if (ch == L'"') return true;
            if (ch == L'\\') {
                if (pos_ >= text_.size()) return fail(error, "invalid escape");
                const wchar_t escaped = text_[pos_++];
                switch (escaped) {
                case L'"': value.push_back(L'"'); break;
                case L'\\': value.push_back(L'\\'); break;
                case L'/': value.push_back(L'/'); break;
                case L'b': value.push_back(L'\b'); break;
                case L'f': value.push_back(L'\f'); break;
                case L'n': value.push_back(L'\n'); break;
                case L'r': value.push_back(L'\r'); break;
                case L't': value.push_back(L'\t'); break;
                case L'u': {
                    if (pos_ + 4 > text_.size()) return fail(error, "short unicode escape");
                    unsigned int code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const wchar_t digit = text_[pos_++];
                        code <<= 4;
                        if (digit >= L'0' && digit <= L'9') code += digit - L'0';
                        else if (digit >= L'a' && digit <= L'f') code += digit - L'a' + 10;
                        else if (digit >= L'A' && digit <= L'F') code += digit - L'A' + 10;
                        else return fail(error, "invalid unicode escape");
                    }
                    value.push_back(static_cast<wchar_t>(code));
                    break;
                }
                default: return fail(error, "unknown escape");
                }
            }
            else {
                if (ch < 0x20) return fail(error, "control character in string");
                value.push_back(ch);
            }
        }
        return fail(error, "unterminated string");
    }

    bool parseObject(Value &value, std::string &error) {
        ++pos_;
        Value::Object object;
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == L'}') {
            ++pos_;
            value = Value(std::move(object));
            return true;
        }
        while (pos_ < text_.size()) {
            std::wstring key;
            if (!parseString(key, error)) return false;
            skipSpace();
            if (pos_ >= text_.size() || text_[pos_++] != L':') return fail(error, "expected colon");
            Value item;
            if (!parseValue(item, error)) return false;
            if (!object.emplace(std::move(key), std::move(item)).second) return fail(error, "duplicate object key");
            skipSpace();
            if (pos_ >= text_.size()) return fail(error, "unterminated object");
            if (text_[pos_] == L'}') {
                ++pos_;
                value = Value(std::move(object));
                return true;
            }
            if (text_[pos_++] != L',') return fail(error, "expected comma");
            skipSpace();
        }
        return fail(error, "unterminated object");
    }

    bool parseArray(Value &value, std::string &error) {
        ++pos_;
        Value::Array array;
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == L']') {
            ++pos_;
            value = Value(std::move(array));
            return true;
        }
        while (pos_ < text_.size()) {
            Value item;
            if (!parseValue(item, error)) return false;
            array.push_back(std::move(item));
            skipSpace();
            if (pos_ >= text_.size()) return fail(error, "unterminated array");
            if (text_[pos_] == L']') {
                ++pos_;
                value = Value(std::move(array));
                return true;
            }
            if (text_[pos_++] != L',') return fail(error, "expected comma");
            skipSpace();
        }
        return fail(error, "unterminated array");
    }

    bool parseNumber(Value &value, std::string &error) {
        const size_t start = pos_;
        if (text_[pos_] == L'-') ++pos_;
        if (pos_ >= text_.size()) return fail(error, "invalid number");
        if (text_[pos_] == L'0') ++pos_;
        else {
            if (text_[pos_] < L'1' || text_[pos_] > L'9') return fail(error, "invalid number");
            while (pos_ < text_.size() && text_[pos_] >= L'0' && text_[pos_] <= L'9') ++pos_;
        }
        if (pos_ < text_.size() && text_[pos_] == L'.') {
            ++pos_;
            const size_t fraction = pos_;
            while (pos_ < text_.size() && text_[pos_] >= L'0' && text_[pos_] <= L'9') ++pos_;
            if (fraction == pos_) return fail(error, "invalid fraction");
        }
        if (pos_ < text_.size() && (text_[pos_] == L'e' || text_[pos_] == L'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == L'+' || text_[pos_] == L'-')) ++pos_;
            const size_t exponent = pos_;
            while (pos_ < text_.size() && text_[pos_] >= L'0' && text_[pos_] <= L'9') ++pos_;
            if (exponent == pos_) return fail(error, "invalid exponent");
        }
        try {
            value = Value(std::stod(text_.substr(start, pos_ - start)));
            return true;
        }
        catch (...) {
            return fail(error, "invalid number");
        }
    }

    std::wstring text_;
    size_t pos_ = 0;
};

std::string escapeString(const std::wstring &value) {
    std::string result;
    for (const wchar_t ch : value) {
        switch (ch) {
        case L'"': result += "\\\""; break;
        case L'\\': result += "\\\\"; break;
        case L'\b': result += "\\b"; break;
        case L'\f': result += "\\f"; break;
        case L'\n': result += "\\n"; break;
        case L'\r': result += "\\r"; break;
        case L'\t': result += "\\t"; break;
        default:
            // UTF-16 代理项按转义输出，避免单独转 UTF-8 时丢失 emoji。
            if (ch < 0x20 || (ch >= 0xd800 && ch <= 0xdfff)) {
                char buffer[8]{};
                sprintf_s(buffer, "\\u%04x", static_cast<unsigned int>(ch));
                result += buffer;
            }
            else result += toUtf8(std::wstring(1, ch));
        }
    }
    return result;
}

void stringify(const Value &value, std::string &out) {
    if (value.isNull()) { out += "null"; return; }
    if (value.isBool()) { out += value.boolean() ? "true" : "false"; return; }
    if (value.isNumber()) {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value.number();
        out += stream.str();
        return;
    }
    if (value.isString()) {
        out.push_back('"');
        out += escapeString(value.string());
        out.push_back('"');
        return;
    }
    if (value.isArray()) {
        out.push_back('[');
        bool first = true;
        for (const auto &item : value.array()) {
            if (!first) out.push_back(',');
            first = false;
            stringify(item, out);
        }
        out.push_back(']');
        return;
    }
    out.push_back('{');
    bool first = true;
    for (const auto &[key, item] : value.object()) {
        if (!first) out.push_back(',');
        first = false;
        out.push_back('"');
        out += escapeString(key);
        out += "\":";
        stringify(item, out);
    }
    out.push_back('}');
}

} // namespace

bool parseUtf8(const std::string &text, Value &value, std::string &error) {
    const std::wstring wide = fromUtf8(text);
    if (text.size() > 0 && wide.empty()) {
        error = "invalid UTF-8";
        return false;
    }
    return Parser(wide).parse(value, error);
}

std::string stringifyUtf8(const Value &value) {
    std::string result;
    stringify(value, result);
    return result;
}

std::string escapeUtf8(const std::wstring &value) {
    return escapeString(value);
}

} // namespace jsonlite
