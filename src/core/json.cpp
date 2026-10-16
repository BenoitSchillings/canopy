// JSON value: RFC 8259 parser and serializer.
#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace canopy {

namespace {

const Json& static_null() {
    static const Json n;
    return n;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------
class Parser {
public:
    Parser(std::string_view t, std::string* err) : t_(t), err_(err) {}

    bool parse_document(Json* out) {
        skip_ws();
        if (!parse_value(out, 0)) return false;
        skip_ws();
        if (pos_ != t_.size()) return fail("trailing characters after JSON value");
        return true;
    }

private:
    std::string_view t_;
    size_t pos_ = 0;
    std::string* err_;
    static constexpr int kMaxDepth = 512;

    bool fail(const char* msg) {
        if (err_ && err_->empty()) {
            // Compute line/column (1-based) for a friendly message.
            size_t line = 1, col = 1;
            for (size_t i = 0; i < pos_ && i < t_.size(); i++) {
                if (t_[i] == '\n') { line++; col = 1; } else col++;
            }
            char buf[160];
            std::snprintf(buf, sizeof buf, "%s at offset %zu (line %zu, column %zu)", msg, pos_, line, col);
            *err_ = buf;
        }
        return false;
    }

    bool at_end() const { return pos_ >= t_.size(); }
    char peek() const { return at_end() ? '\0' : t_[pos_]; }

    void skip_ws() {
        while (!at_end()) {
            char c = t_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') pos_++;
            else break;
        }
    }

    bool parse_value(Json* out, int depth) {
        if (depth > kMaxDepth) return fail("nesting too deep");
        if (at_end()) return fail("unexpected end of input");
        char c = t_[pos_];
        switch (c) {
        case '{': return parse_object(out, depth);
        case '[': return parse_array(out, depth);
        case '"': {
            std::string s;
            if (!parse_string(&s)) return false;
            *out = Json(std::move(s));
            return true;
        }
        case 't':
            if (t_.substr(pos_, 4) == "true") { pos_ += 4; *out = Json(true); return true; }
            return fail("invalid literal");
        case 'f':
            if (t_.substr(pos_, 5) == "false") { pos_ += 5; *out = Json(false); return true; }
            return fail("invalid literal");
        case 'n':
            if (t_.substr(pos_, 4) == "null") { pos_ += 4; *out = Json(nullptr); return true; }
            return fail("invalid literal");
        default:
            if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out);
            return fail("unexpected character");
        }
    }

    bool parse_object(Json* out, int depth) {
        pos_++; // '{'
        Json::Object obj;
        skip_ws();
        if (peek() == '}') { pos_++; *out = Json(std::move(obj)); return true; }
        for (;;) {
            skip_ws();
            if (peek() != '"') return fail("expected string key");
            std::string key;
            if (!parse_string(&key)) return false;
            skip_ws();
            if (peek() != ':') return fail("expected ':'");
            pos_++;
            skip_ws();
            Json v;
            if (!parse_value(&v, depth + 1)) return false;
            obj[std::move(key)] = std::move(v);   // last duplicate wins
            skip_ws();
            char c = peek();
            if (c == ',') { pos_++; continue; }
            if (c == '}') { pos_++; break; }
            return fail(at_end() ? "unterminated object" : "expected ',' or '}'");
        }
        *out = Json(std::move(obj));
        return true;
    }

    bool parse_array(Json* out, int depth) {
        pos_++; // '['
        Json::Array arr;
        skip_ws();
        if (peek() == ']') { pos_++; *out = Json(std::move(arr)); return true; }
        for (;;) {
            skip_ws();
            Json v;
            if (!parse_value(&v, depth + 1)) return false;
            arr.push_back(std::move(v));
            skip_ws();
            char c = peek();
            if (c == ',') { pos_++; continue; }
            if (c == ']') { pos_++; break; }
            return fail(at_end() ? "unterminated array" : "expected ',' or ']'");
        }
        *out = Json(std::move(arr));
        return true;
    }

    static int hexval(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool parse_hex4(uint32_t* out) {
        if (pos_ + 4 > t_.size()) return fail("truncated \\u escape");
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            int h = hexval(t_[pos_ + i]);
            if (h < 0) return fail("invalid hex digit in \\u escape");
            v = (v << 4) | (uint32_t)h;
        }
        pos_ += 4;
        *out = v;
        return true;
    }

    static void append_utf8(std::string& s, uint32_t cp) {
        if (cp < 0x80) {
            s.push_back((char)cp);
        } else if (cp < 0x800) {
            s.push_back((char)(0xC0 | (cp >> 6)));
            s.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s.push_back((char)(0xE0 | (cp >> 12)));
            s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            s.push_back((char)(0xF0 | (cp >> 18)));
            s.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }

    bool parse_string(std::string* out) {
        pos_++; // opening quote
        std::string s;
        for (;;) {
            if (at_end()) return fail("unterminated string");
            // Fast path: copy a run of plain characters.
            size_t start = pos_;
            while (pos_ < t_.size()) {
                unsigned char c = (unsigned char)t_[pos_];
                if (c == '"' || c == '\\' || c < 0x20) break;
                pos_++;
            }
            s.append(t_.data() + start, pos_ - start);
            if (at_end()) return fail("unterminated string");
            unsigned char c = (unsigned char)t_[pos_];
            if (c == '"') { pos_++; break; }
            if (c < 0x20) return fail("unescaped control character in string");
            // backslash
            pos_++;
            if (at_end()) return fail("unterminated escape");
            char e = t_[pos_++];
            switch (e) {
            case '"': s.push_back('"'); break;
            case '\\': s.push_back('\\'); break;
            case '/': s.push_back('/'); break;
            case 'b': s.push_back('\b'); break;
            case 'f': s.push_back('\f'); break;
            case 'n': s.push_back('\n'); break;
            case 'r': s.push_back('\r'); break;
            case 't': s.push_back('\t'); break;
            case 'u': {
                uint32_t cp;
                if (!parse_hex4(&cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    // High surrogate: expect a low surrogate next.
                    if (pos_ + 6 <= t_.size() && t_[pos_] == '\\' && t_[pos_ + 1] == 'u') {
                        size_t save = pos_;
                        pos_ += 2;
                        uint32_t lo;
                        if (!parse_hex4(&lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            // Lone high surrogate: emit U+FFFD, re-parse the next escape normally.
                            pos_ = save;
                            cp = 0xFFFD;
                        }
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;  // lone low surrogate
                }
                append_utf8(s, cp);
                break;
            }
            default:
                pos_--;
                return fail("invalid escape sequence");
            }
        }
        *out = std::move(s);
        return true;
    }

    bool parse_number(Json* out) {
        size_t start = pos_;
        if (peek() == '-') pos_++;
        if (at_end()) return fail("invalid number");
        if (t_[pos_] == '0') {
            pos_++;
        } else if (t_[pos_] >= '1' && t_[pos_] <= '9') {
            while (!at_end() && t_[pos_] >= '0' && t_[pos_] <= '9') pos_++;
        } else {
            return fail("invalid number");
        }
        if (peek() == '.') {
            pos_++;
            if (at_end() || !(t_[pos_] >= '0' && t_[pos_] <= '9')) return fail("expected digit after '.'");
            while (!at_end() && t_[pos_] >= '0' && t_[pos_] <= '9') pos_++;
        }
        if (peek() == 'e' || peek() == 'E') {
            pos_++;
            if (peek() == '+' || peek() == '-') pos_++;
            if (at_end() || !(t_[pos_] >= '0' && t_[pos_] <= '9')) return fail("expected digit in exponent");
            while (!at_end() && t_[pos_] >= '0' && t_[pos_] <= '9') pos_++;
        }
        // strtod needs a NUL-terminated buffer.
        char buf[64];
        size_t len = pos_ - start;
        double v;
        if (len < sizeof buf) {
            std::memcpy(buf, t_.data() + start, len);
            buf[len] = 0;
            v = std::strtod(buf, nullptr);
        } else {
            std::string tmp(t_.data() + start, len);
            v = std::strtod(tmp.c_str(), nullptr);
        }
        *out = Json(v);
        return true;
    }
};

// ---------------------------------------------------------------------------
// Serializer
// ---------------------------------------------------------------------------
void dump_string(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20 || c == 0x7f) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out.push_back((char)c);   // non-ASCII UTF-8 passes through verbatim
            }
        }
    }
    out.push_back('"');
}

void dump_number(std::string& out, double v) {
    if (std::isnan(v) || std::isinf(v)) { out += "null"; return; }   // not representable in JSON
    const double kMaxExact = 9007199254740992.0;  // 2^53
    if (v == std::floor(v) && std::fabs(v) < kMaxExact) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%lld", (long long)v);
        out += buf;
        return;
    }
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    out += buf;
}

void dump_value(std::string& out, const Json& j, int indent, int level) {
    auto newline = [&](int lvl) {
        if (indent > 0) {
            out.push_back('\n');
            out.append((size_t)(lvl * indent), ' ');
        }
    };
    switch (j.type()) {
    case Json::Type::Null: out += "null"; break;
    case Json::Type::Bool: out += j.as_bool() ? "true" : "false"; break;
    case Json::Type::Number: dump_number(out, j.as_double()); break;
    case Json::Type::String: dump_string(out, j.as_string()); break;
    case Json::Type::Array: {
        const auto& a = j.as_array();
        if (a.empty()) { out += "[]"; break; }
        out.push_back('[');
        for (size_t i = 0; i < a.size(); i++) {
            if (i) out.push_back(',');
            newline(level + 1);
            dump_value(out, a[i], indent, level + 1);
        }
        newline(level);
        out.push_back(']');
        break;
    }
    case Json::Type::Object: {
        const auto& o = j.as_object();
        if (o.empty()) { out += "{}"; break; }
        out.push_back('{');
        bool first = true;
        for (const auto& [k, v] : o) {
            if (!first) out.push_back(',');
            first = false;
            newline(level + 1);
            dump_string(out, k);
            out.push_back(':');
            if (indent > 0) out.push_back(' ');
            dump_value(out, v, indent, level + 1);
        }
        newline(level);
        out.push_back('}');
        break;
    }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Json members
// ---------------------------------------------------------------------------
const std::string& Json::as_string() const {
    static const std::string empty;
    return is_string() ? s_ : empty;
}

const Json::Array& Json::as_array() const {
    static const Array empty;
    return is_array() ? a_ : empty;
}

Json::Array& Json::as_array() {
    if (is_null()) type_ = Type::Array;
    if (!is_array()) { *this = Json(Array{}); }
    return a_;
}

const Json::Object& Json::as_object() const {
    static const Object empty;
    return is_object() ? o_ : empty;
}

Json::Object& Json::as_object() {
    if (is_null()) type_ = Type::Object;
    if (!is_object()) { *this = Json(Object{}); }
    return o_;
}

const Json& Json::operator[](const std::string& key) const {
    if (!is_object()) return static_null();
    auto it = o_.find(key);
    return it == o_.end() ? static_null() : it->second;
}

Json& Json::operator[](const std::string& key) {
    if (is_null()) type_ = Type::Object;
    if (!is_object()) { *this = Json(Object{}); }
    return o_[key];
}

const Json& Json::operator[](size_t i) const {
    if (!is_array() || i >= a_.size()) return static_null();
    return a_[i];
}

Json& Json::operator[](size_t i) {
    if (is_null()) type_ = Type::Array;
    if (!is_array()) { *this = Json(Array{}); }
    if (i >= a_.size()) a_.resize(i + 1);
    return a_[i];
}

bool Json::has(const std::string& key) const {
    return is_object() && o_.find(key) != o_.end();
}

size_t Json::size() const {
    if (is_array()) return a_.size();
    if (is_object()) return o_.size();
    if (is_string()) return s_.size();
    return 0;
}

void Json::push_back(Json v) {
    if (is_null()) type_ = Type::Array;
    if (!is_array()) { *this = Json(Array{}); }
    a_.push_back(std::move(v));
}

std::optional<Json> Json::parse(std::string_view text, std::string* err) {
    if (err) err->clear();
    Parser p(text, err);
    Json out;
    if (!p.parse_document(&out)) return std::nullopt;
    return out;
}

std::string Json::dump(int indent) const {
    std::string out;
    dump_value(out, *this, indent < 0 ? 0 : indent, 0);
    return out;
}

} // namespace canopy
