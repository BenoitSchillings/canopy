#pragma once
// Minimal JSON value with parse/serialize. Enough for persistence and config.
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <optional>

namespace canopy {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;   // sorted keys => deterministic output

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), b_(b) {}
    Json(int v) : type_(Type::Number), n_((double)v) {}
    Json(int64_t v) : type_(Type::Number), n_((double)v) {}
    Json(uint64_t v) : type_(Type::Number), n_((double)v) {}
    Json(double v) : type_(Type::Number), n_(v) {}
    Json(const char* s) : type_(Type::String), s_(s) {}
    Json(std::string s) : type_(Type::String), s_(std::move(s)) {}
    Json(Array a) : type_(Type::Array), a_(std::move(a)) {}
    Json(Object o) : type_(Type::Object), o_(std::move(o)) {}

    static Json array() { return Json(Array{}); }
    static Json object() { return Json(Object{}); }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_string() const { return type_ == Type::String; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_bool() const { return type_ == Type::Bool; }

    bool as_bool(bool def = false) const { return is_bool() ? b_ : def; }
    double as_double(double def = 0) const { return is_number() ? n_ : def; }
    int64_t as_int(int64_t def = 0) const { return is_number() ? (int64_t)n_ : def; }
    const std::string& as_string() const;              // empty string if not a string
    std::string as_string(const std::string& def) const { return is_string() ? s_ : def; }
    const Array& as_array() const;                     // empty if not an array
    Array& as_array();                                 // converts to array if null
    const Object& as_object() const;
    Object& as_object();                               // converts to object if null

    // Object access. const operator[] returns a static null for missing keys.
    const Json& operator[](const std::string& key) const;
    Json& operator[](const std::string& key);          // converts to object if null
    const Json& operator[](size_t i) const;
    Json& operator[](size_t i);
    bool has(const std::string& key) const;
    size_t size() const;
    void push_back(Json v);

    static std::optional<Json> parse(std::string_view text, std::string* err = nullptr);
    std::string dump(int indent = 0) const;            // indent 0 => compact

private:
    Type type_ = Type::Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    Array a_;
    Object o_;
};

} // namespace canopy
