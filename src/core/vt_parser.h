#pragma once
// VT500-style escape sequence state machine (after Paul Williams' DEC parser), with UTF-8
// decoding folded in. The parser owns no screen state; it dispatches to a VtHandler.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace canopy {

struct CsiParams {
    static constexpr int MAX = 32;
    int vals[MAX];
    uint8_t sub[MAX];       // 1 if this param was separated from the previous by ':' (sub-parameter)
    bool present[MAX];      // whether a digit was seen for this param
    int count = 0;          // number of params (>=1 after any digit/separator)

    void clear() { count = 0; }
    // Value of param i, or def if absent/zero-length.
    int get(int i, int def = 0) const { return (i < count && present[i]) ? vals[i] : def; }
    // Like get() but returns def when the value is 0 as well (most CSI commands treat 0 as 1).
    int get1(int i, int def = 1) const { int v = get(i, def); return v == 0 ? def : v; }
    bool is_sub(int i) const { return i < count && sub[i]; }
};

class VtHandler {
public:
    virtual ~VtHandler() = default;
    virtual void print(const char32_t* cps, size_t n) = 0;
    virtual void execute(uint8_t c) = 0;                     // C0 control or C1 (0x80..0x9F)
    virtual void esc_dispatch(std::string_view intermediates, uint8_t final) = 0;
    virtual void csi_dispatch(const CsiParams& p, std::string_view intermediates, uint8_t final) = 0;
    virtual void osc_dispatch(std::string_view payload) = 0;
    virtual void dcs_hook(const CsiParams& p, std::string_view intermediates, uint8_t final) = 0;
    virtual void dcs_put(std::string_view data) = 0;
    virtual void dcs_unhook() = 0;
    // SOS / PM / APC string bodies (kind = 'X', '^', '_').
    virtual void string_dispatch(char kind, std::string_view payload) = 0;
};

class VtParser {
public:
    enum class State : uint8_t {
        Ground, Escape, EscapeIntermediate,
        CsiEntry, CsiParam, CsiIntermediate, CsiIgnore,
        DcsEntry, DcsParam, DcsIntermediate, DcsIgnore, DcsPassthrough,
        OscString, SosPmApcString,
    };
    explicit VtParser(VtHandler& h) : h_(h) {}
    void feed(const char* data, size_t len);
    void reset();
    State state() const { return state_; }

private:
    void byte(uint8_t b);                 // one fully-decoded ASCII byte or C1 control (as 0x80..0x9F)
    void codepoint(char32_t cp);          // one decoded non-ASCII code point (>= 0xA0)
    void flush_print();
    void clear_seq();
    void enter(State s);
    void param_digit(int d);
    void param_sep(bool colon);

    VtHandler& h_;
    State state_ = State::Ground;
    CsiParams params_;
    std::string intermediates_;
    std::string str_;                     // OSC / DCS / APC body
    char str_kind_ = 0;
    bool dcs_hooked_ = false;
    // UTF-8 decoder state
    char32_t u_cp_ = 0; int u_need_ = 0; char32_t u_min_ = 0;
    // print batching
    char32_t pbuf_[512]; size_t pn_ = 0;
};

} // namespace canopy
