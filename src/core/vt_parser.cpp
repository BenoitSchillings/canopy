#include "vt_parser.h"
#include "unicode.h"

namespace canopy {

static constexpr size_t MAX_STRING = 1 << 20;   // cap OSC/DCS bodies at 1 MiB

void VtParser::reset() {
    state_ = State::Ground;
    clear_seq();
    str_.clear();
    dcs_hooked_ = false;
    u_need_ = 0;
    pn_ = 0;
}

void VtParser::clear_seq() {
    params_.clear();
    intermediates_.clear();
}

void VtParser::enter(State s) {
    // Entry actions.
    switch (s) {
    case State::Escape: case State::CsiEntry: case State::DcsEntry: clear_seq(); break;
    case State::OscString: case State::SosPmApcString: str_.clear(); break;
    case State::DcsPassthrough: str_.clear(); break;
    default: break;
    }
    state_ = s;
}

void VtParser::flush_print() {
    if (pn_) { h_.print(pbuf_, pn_); pn_ = 0; }
}

void VtParser::param_digit(int d) {
    if (params_.count == 0) { params_.count = 1; params_.vals[0] = 0; params_.sub[0] = 0; params_.present[0] = false; }
    int i = params_.count - 1;
    if (i >= CsiParams::MAX) return;
    int v = params_.vals[i] * 10 + d;
    params_.vals[i] = v > 65535 ? 65535 : v;
    params_.present[i] = true;
}

void VtParser::param_sep(bool colon) {
    if (params_.count == 0) { params_.count = 1; params_.vals[0] = 0; params_.sub[0] = 0; params_.present[0] = false; }
    if (params_.count >= CsiParams::MAX) return;   // extra params are ignored
    int i = params_.count++;
    params_.vals[i] = 0; params_.sub[i] = colon ? 1 : 0; params_.present[i] = false;
}

void VtParser::feed(const char* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    const uint8_t* end = p + len;
    while (p < end) {
        uint8_t b = *p;
        if (u_need_) {
            // Continuation of a multi-byte UTF-8 sequence.
            if ((b & 0xC0) == 0x80) {
                u_cp_ = (u_cp_ << 6) | (b & 0x3F);
                ++p;
                if (--u_need_ == 0) {
                    char32_t cp = u_cp_;
                    if (cp < u_min_ || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
                    if (cp < 0x80) byte((uint8_t)cp);
                    else if (cp < 0xA0) byte((uint8_t)cp);     // C1 control
                    else codepoint(cp);
                }
                continue;
            }
            // Invalid continuation: emit U+FFFD and reprocess this byte.
            u_need_ = 0;
            codepoint(0xFFFD);
            continue;
        }
        if (b < 0x80) {
            if (state_ == State::Ground && b >= 0x20 && b < 0x7F) {
                // Fast path: run of printable ASCII.
                while (p < end && *p >= 0x20 && *p < 0x7F) {
                    pbuf_[pn_++] = *p++;
                    if (pn_ == sizeof(pbuf_) / sizeof(pbuf_[0])) flush_print();
                }
                continue;
            }
            byte(b);
            ++p;
            continue;
        }
        // Lead byte of a multi-byte sequence (or an invalid byte).
        ++p;
        if ((b & 0xE0) == 0xC0) { u_need_ = 1; u_cp_ = b & 0x1F; u_min_ = 0x80; }
        else if ((b & 0xF0) == 0xE0) { u_need_ = 2; u_cp_ = b & 0x0F; u_min_ = 0x800; }
        else if ((b & 0xF8) == 0xF0) { u_need_ = 3; u_cp_ = b & 0x07; u_min_ = 0x10000; }
        else { codepoint(0xFFFD); }
    }
    flush_print();
}

void VtParser::codepoint(char32_t cp) {
    switch (state_) {
    case State::Ground:
        pbuf_[pn_++] = cp;
        if (pn_ == sizeof(pbuf_) / sizeof(pbuf_[0])) flush_print();
        return;
    case State::OscString: case State::SosPmApcString:
        if (str_.size() < MAX_STRING) utf8_append(str_, cp);
        return;
    case State::DcsPassthrough:
        if (str_.size() < MAX_STRING) utf8_append(str_, cp);
        return;
    default:
        // A printable character inside an escape sequence: abort the sequence and print it.
        state_ = State::Ground;
        pbuf_[pn_++] = cp;
        if (pn_ == sizeof(pbuf_) / sizeof(pbuf_[0])) flush_print();
        return;
    }
}

void VtParser::byte(uint8_t b) {
    // ---- "anywhere" transitions ----
    if (b == 0x18 || b == 0x1A) {               // CAN, SUB
        if (state_ == State::DcsPassthrough && dcs_hooked_) { h_.dcs_put(str_); str_.clear(); h_.dcs_unhook(); dcs_hooked_ = false; }
        flush_print(); h_.execute(b); state_ = State::Ground; return;
    }
    if (b >= 0x80) {                            // C1 controls
        switch (b) {
        case 0x9B: flush_print(); enter(State::CsiEntry); return;
        case 0x9D: flush_print(); enter(State::OscString); return;
        case 0x90: flush_print(); enter(State::DcsEntry); return;
        case 0x98: case 0x9E: case 0x9F: flush_print(); str_kind_ = b == 0x98 ? 'X' : b == 0x9E ? '^' : '_'; enter(State::SosPmApcString); return;
        case 0x9C: // ST
            if (state_ == State::OscString) { h_.osc_dispatch(str_); }
            else if (state_ == State::SosPmApcString) { h_.string_dispatch(str_kind_, str_); }
            else if (state_ == State::DcsPassthrough && dcs_hooked_) { h_.dcs_put(str_); str_.clear(); h_.dcs_unhook(); dcs_hooked_ = false; }
            state_ = State::Ground; return;
        default:
            if (state_ == State::OscString || state_ == State::SosPmApcString || state_ == State::DcsPassthrough) return;
            flush_print(); h_.execute(b); state_ = State::Ground; return;
        }
    }
    if (b == 0x1B) {                            // ESC
        switch (state_) {
        case State::OscString: h_.osc_dispatch(str_); break;
        case State::SosPmApcString: h_.string_dispatch(str_kind_, str_); break;
        case State::DcsPassthrough: if (dcs_hooked_) { h_.dcs_put(str_); str_.clear(); h_.dcs_unhook(); dcs_hooked_ = false; } break;
        default: break;
        }
        flush_print();
        enter(State::Escape);
        return;
    }

    switch (state_) {
    case State::Ground:
        if (b < 0x20) { flush_print(); h_.execute(b); }
        else if (b == 0x7F) { /* DEL ignored */ }
        else { pbuf_[pn_++] = b; if (pn_ == sizeof(pbuf_) / sizeof(pbuf_[0])) flush_print(); }
        return;

    case State::Escape:
        if (b < 0x20) { h_.execute(b); return; }
        if (b >= 0x20 && b <= 0x2F) { intermediates_.push_back((char)b); state_ = State::EscapeIntermediate; return; }
        if (b == 0x7F) return;
        switch (b) {
        case '[': enter(State::CsiEntry); return;
        case ']': enter(State::OscString); return;
        case 'P': enter(State::DcsEntry); return;
        case 'X': str_kind_ = 'X'; enter(State::SosPmApcString); return;
        case '^': str_kind_ = '^'; enter(State::SosPmApcString); return;
        case '_': str_kind_ = '_'; enter(State::SosPmApcString); return;
        default:
            h_.esc_dispatch(intermediates_, b);
            state_ = State::Ground;
            return;
        }

    case State::EscapeIntermediate:
        if (b < 0x20) { h_.execute(b); return; }
        if (b <= 0x2F) { intermediates_.push_back((char)b); return; }
        if (b == 0x7F) return;
        h_.esc_dispatch(intermediates_, b);
        state_ = State::Ground;
        return;

    case State::CsiEntry:
        if (b < 0x20) { h_.execute(b); return; }
        if (b == 0x7F) return;
        if (b >= 0x30 && b <= 0x39) { param_digit(b - '0'); state_ = State::CsiParam; return; }
        if (b == ';') { param_sep(false); state_ = State::CsiParam; return; }
        if (b == ':') { param_sep(true); state_ = State::CsiParam; return; }
        if (b >= 0x3C && b <= 0x3F) { intermediates_.push_back((char)b); state_ = State::CsiParam; return; }
        if (b >= 0x20 && b <= 0x2F) { intermediates_.push_back((char)b); state_ = State::CsiIntermediate; return; }
        h_.csi_dispatch(params_, intermediates_, b);
        state_ = State::Ground;
        return;

    case State::CsiParam:
        if (b < 0x20) { h_.execute(b); return; }
        if (b == 0x7F) return;
        if (b >= 0x30 && b <= 0x39) { param_digit(b - '0'); return; }
        if (b == ';') { param_sep(false); return; }
        if (b == ':') { param_sep(true); return; }
        if (b >= 0x3C && b <= 0x3F) { state_ = State::CsiIgnore; return; }
        if (b >= 0x20 && b <= 0x2F) { intermediates_.push_back((char)b); state_ = State::CsiIntermediate; return; }
        h_.csi_dispatch(params_, intermediates_, b);
        state_ = State::Ground;
        return;

    case State::CsiIntermediate:
        if (b < 0x20) { h_.execute(b); return; }
        if (b == 0x7F) return;
        if (b >= 0x20 && b <= 0x2F) { intermediates_.push_back((char)b); return; }
        if (b >= 0x30 && b <= 0x3F) { state_ = State::CsiIgnore; return; }
        h_.csi_dispatch(params_, intermediates_, b);
        state_ = State::Ground;
        return;

    case State::CsiIgnore:
        if (b < 0x20) { h_.execute(b); return; }
        if (b >= 0x40 && b <= 0x7E) state_ = State::Ground;
        return;

    case State::DcsEntry:
        if (b < 0x20 || b == 0x7F) return;
        if (b >= 0x30 && b <= 0x39) { param_digit(b - '0'); state_ = State::DcsParam; return; }
        if (b == ';') { param_sep(false); state_ = State::DcsParam; return; }
        if (b == ':') { param_sep(true); state_ = State::DcsParam; return; }
        if (b >= 0x3C && b <= 0x3F) { intermediates_.push_back((char)b); state_ = State::DcsParam; return; }
        if (b >= 0x20 && b <= 0x2F) { intermediates_.push_back((char)b); state_ = State::DcsIntermediate; return; }
        h_.dcs_hook(params_, intermediates_, b); dcs_hooked_ = true;
        enter(State::DcsPassthrough);
        return;

    case State::DcsParam:
        if (b < 0x20 || b == 0x7F) return;
        if (b >= 0x30 && b <= 0x39) { param_digit(b - '0'); return; }
        if (b == ';') { param_sep(false); return; }
        if (b == ':') { param_sep(true); return; }
        if (b >= 0x3C && b <= 0x3F) { state_ = State::DcsIgnore; return; }
        if (b >= 0x20 && b <= 0x2F) { intermediates_.push_back((char)b); state_ = State::DcsIntermediate; return; }
        h_.dcs_hook(params_, intermediates_, b); dcs_hooked_ = true;
        enter(State::DcsPassthrough);
        return;

    case State::DcsIntermediate:
        if (b < 0x20 || b == 0x7F) return;
        if (b >= 0x20 && b <= 0x2F) { intermediates_.push_back((char)b); return; }
        if (b >= 0x30 && b <= 0x3F) { state_ = State::DcsIgnore; return; }
        h_.dcs_hook(params_, intermediates_, b); dcs_hooked_ = true;
        enter(State::DcsPassthrough);
        return;

    case State::DcsIgnore:
        return;   // until ST / ESC

    case State::DcsPassthrough:
        if (b == 0x7F) return;
        if (str_.size() < MAX_STRING) str_.push_back((char)b);
        return;

    case State::OscString:
        if (b == 0x07) { h_.osc_dispatch(str_); state_ = State::Ground; return; }
        if (b < 0x20) return;   // other controls ignored
        if (str_.size() < MAX_STRING) str_.push_back((char)b);
        return;

    case State::SosPmApcString:
        if (b == 0x07) { h_.string_dispatch(str_kind_, str_); state_ = State::Ground; return; }
        if (b < 0x20) return;
        if (str_.size() < MAX_STRING) str_.push_back((char)b);
        return;
    }
}

} // namespace canopy
