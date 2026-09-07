#pragma once

#include <map>
#include <string>
#include <variant>
#include <vector>

namespace jsonlite {

struct Value {
    using Object = std::map<std::wstring, Value>;
    using Array = std::vector<Value>;
    using Storage = std::variant<std::nullptr_t, bool, double, std::wstring, Array, Object>;

    Storage data;

    Value() : data(nullptr) {}
    Value(std::nullptr_t) : data(nullptr) {}
    Value(bool value) : data(value) {}
    Value(double value) : data(value) {}
    Value(int value) : data(static_cast<double>(value)) {}
    Value(const wchar_t *value) : data(std::wstring(value ? value : L"")) {}
    Value(std::wstring value) : data(std::move(value)) {}
    Value(Array value) : data(std::move(value)) {}
    Value(Object value) : data(std::move(value)) {}

    bool isNull() const { return std::holds_alternative<std::nullptr_t>(data); }
    bool isBool() const { return std::holds_alternative<bool>(data); }
    bool isNumber() const { return std::holds_alternative<double>(data); }
    bool isString() const { return std::holds_alternative<std::wstring>(data); }
    bool isArray() const { return std::holds_alternative<Array>(data); }
    bool isObject() const { return std::holds_alternative<Object>(data); }

    bool boolean(bool fallback = false) const {
        return isBool() ? std::get<bool>(data) : fallback;
    }
    double number(double fallback = 0) const {
        return isNumber() ? std::get<double>(data) : fallback;
    }
    std::wstring string(const std::wstring &fallback = {}) const {
        return isString() ? std::get<std::wstring>(data) : fallback;
    }
    const Array &array() const {
        static const Array empty;
        return isArray() ? std::get<Array>(data) : empty;
    }
    const Object &object() const {
        static const Object empty;
        return isObject() ? std::get<Object>(data) : empty;
    }
    const Value *get(const std::wstring &key) const {
        if (!isObject()) return nullptr;
        const auto &map = std::get<Object>(data);
        const auto it = map.find(key);
        return it == map.end() ? nullptr : &it->second;
    }
};

bool parseUtf8(const std::string &text, Value &value, std::string &error);
std::string stringifyUtf8(const Value &value);
std::string toUtf8(const std::wstring &value);
std::wstring fromUtf8(const std::string &value);
std::string escapeUtf8(const std::wstring &value);

} // namespace jsonlite
