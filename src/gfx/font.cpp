// FreeType rasterization into one RGBA8 atlas, fontconfig for family resolution and
// per-code-point fallback (CJK, symbols, color emoji).
//
// Atlas layout: mask glyphs store coverage in .a with rgb = 1 (shader: color * tex.a);
// color glyphs store premultiplied RGBA (shader: tex.rgba * color.a). Glyph::color selects.
// The atlas starts at 1024^2 and grows (2048, 4096) by re-uploading; glyph atlas rects
// (ax, ay, aw, ah) stay valid across growth, normalized UVs are recomputed.
#include "font.h"
#include "gl.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_SYNTHESIS_H
#include FT_TRUETYPE_TABLES_H
#include FT_BITMAP_H
#include <fontconfig/fontconfig.h>
#include <fontconfig/fcfreetype.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace canopy::gfx {

namespace {

constexpr int kAtlasStart = 1024;
constexpr int kAtlasMax = 4096;
constexpr int kPad = 1;              // transparent texels around each glyph (each side)
constexpr int kMaxFallbackFaces = 16;

struct FaceRef {
    FT_Face face = nullptr;
    std::string path;
    int index = 0;
    bool synth_bold = false;
    bool synth_italic = false;
    bool color = false;              // bitmap color font (CBDT), use FT_LOAD_COLOR + strike
    float scale = 1.f;               // extra scale applied to bitmaps (color strikes)
    FcCharSet* charset = nullptr;    // coverage (fallback faces), owned
};

// Code points that default to emoji presentation (color fallback preferred).
bool is_emoji_presentation(char32_t cp) {
    return (cp >= 0x1F300 && cp <= 0x1FAFF) || (cp >= 0x1F1E6 && cp <= 0x1F1FF) ||
           (cp >= 0x2600 && cp <= 0x27BF && (cp == 0x2614 || cp == 0x2615 || (cp >= 0x2648 && cp <= 0x2653) || cp == 0x267F ||
                                             cp == 0x2693 || cp == 0x26A1 || cp == 0x26AA || cp == 0x26AB || cp == 0x26BD || cp == 0x26BE ||
                                             cp == 0x26C4 || cp == 0x26C5 || cp == 0x26CE || cp == 0x26D4 || cp == 0x26EA || cp == 0x26F2 ||
                                             cp == 0x26F3 || cp == 0x26F5 || cp == 0x26FA || cp == 0x26FD || cp == 0x2705 || cp == 0x270A ||
                                             cp == 0x270B || cp == 0x2728 || cp == 0x274C || cp == 0x274E || (cp >= 0x2753 && cp <= 0x2755) ||
                                             cp == 0x2757 || (cp >= 0x2795 && cp <= 0x2797) || cp == 0x27B0 || cp == 0x27BF)) ||
           cp == 0x231A || cp == 0x231B || cp == 0x23E9 || cp == 0x23EA || cp == 0x23EB || cp == 0x23EC || cp == 0x23F0 || cp == 0x23F3 ||
           cp == 0x25FD || cp == 0x25FE || cp == 0x2B1B || cp == 0x2B1C || cp == 0x2B50 || cp == 0x2B55 || cp == 0x1F004 || cp == 0x1F0CF ||
           cp == 0x1F18E || (cp >= 0x1F191 && cp <= 0x1F19A) || cp == 0x1F201 || cp == 0x1F21A || cp == 0x1F22F || (cp >= 0x1F232 && cp <= 0x1F236) ||
           (cp >= 0x1F238 && cp <= 0x1F23A) || cp == 0x1F250 || cp == 0x1F251;
}

// Zero-width code points that must not produce a visible glyph (joiners, variation selectors).
bool is_zero_width(char32_t cp) {
    return (cp >= 0x200B && cp <= 0x200F) || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF || (cp >= 0xE0100 && cp <= 0xE01EF) ||
           cp == 0x2060 || cp == 0x00AD;
}

uint64_t glyph_key(char32_t cp, GlyphStyle st) { return (uint64_t)cp | ((uint64_t)st << 32); }

FcPattern* make_pattern(const std::string& family, bool bold, bool italic) {
    FcPattern* p = FcPatternCreate();
    if (!family.empty()) FcPatternAddString(p, FC_FAMILY, (const FcChar8*)family.c_str());
    else {
        FcPatternAddString(p, FC_FAMILY, (const FcChar8*)"monospace");
        FcPatternAddInteger(p, FC_SPACING, FC_MONO);
    }
    FcPatternAddInteger(p, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
    FcPatternAddInteger(p, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
    FcPatternAddBool(p, FC_SCALABLE, FcTrue);
    return p;
}

// want_family: when non-empty, require the match to actually be that family (fontconfig
// otherwise silently substitutes sans-serif for unknown families).
bool match_pattern(FcPattern* p, std::string* path, int* index, FcLangSet** langs, const std::string& want_family = "") {
    FcConfigSubstitute(nullptr, p, FcMatchPattern);
    FcDefaultSubstitute(p);
    FcResult res;
    FcPattern* m = FcFontMatch(nullptr, p, &res);
    if (!m) return false;
    FcChar8* file = nullptr;
    bool ok = FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch && file;
    if (ok && !want_family.empty()) {
        bool found = false;
        FcChar8* fam = nullptr;
        for (int i = 0; FcPatternGetString(m, FC_FAMILY, i, &fam) == FcResultMatch; ++i)
            if (FcStrCmpIgnoreCase(fam, (const FcChar8*)want_family.c_str()) == 0) { found = true; break; }
        if (!found) ok = false;
    }
    if (ok) {
        *path = (const char*)file;
        int idx = 0;
        if (FcPatternGetInteger(m, FC_INDEX, 0, &idx) != FcResultMatch) idx = 0;
        *index = idx;
        if (langs) {
            FcLangSet* ls = nullptr;
            if (FcPatternGetLangSet(m, FC_LANG, 0, &ls) == FcResultMatch && ls) *langs = FcLangSetCopy(ls);
        }
    }
    FcPatternDestroy(m);
    return ok;
}

} // namespace

struct Font::Impl {
    FT_Library lib = nullptr;
    std::string family;
    int pad_x = 0, pad_y = 0;
    FcLangSet* langs = nullptr;

    FaceRef styles[4];                       // regular, bold, italic, bold-italic
    std::vector<FT_Face> owned_faces;        // distinct FT_Face objects to destroy
    std::vector<FaceRef> fallbacks;
    int fallback_rr = 0;                     // round-robin replacement slot when full

    std::unordered_map<uint64_t, Glyph> cache;
    Glyph notdef_glyph;                      // returned when nothing can be drawn

    // Atlas
    std::vector<uint8_t> pixels;             // RGBA8, atlas_size^2
    int atlas_size = 0;
    int shelf_x = 0, shelf_y = 0, shelf_h = 0;
    unsigned tex = 0;
    bool tex_valid = false;                  // texture holds the full CPU buffer
    bool dirty = false;
    bool needs_full_upload = false;

    ~Impl() {
        for (FaceRef& f : fallbacks) if (f.charset) FcCharSetDestroy(f.charset);
        for (FT_Face f : owned_faces) FT_Done_Face(f);
        if (lib) FT_Done_FreeType(lib);
        if (langs) FcLangSetDestroy(langs);
        if (tex && glDeleteTextures) glDeleteTextures(1, &tex);
    }

    // ---- atlas -------------------------------------------------------------------
    void atlas_reset(int size) {
        atlas_size = size;
        pixels.assign((size_t)size * size * 4, 0);
        shelf_x = shelf_y = shelf_h = 0;
        needs_full_upload = true;
        dirty = true;
    }

    // Allocates a w x h rectangle (plus padding). Returns false if the atlas must grow/clear.
    bool atlas_alloc(int w, int h, int* x, int* y) {
        int pw = w + 2 * kPad, ph = h + 2 * kPad;
        if (pw > atlas_size || ph > atlas_size) return false;
        if (shelf_x + pw > atlas_size) {              // new shelf
            shelf_y += shelf_h;
            shelf_x = 0;
            shelf_h = 0;
        }
        if (shelf_y + ph > atlas_size) return false;
        *x = shelf_x + kPad;
        *y = shelf_y + kPad;
        shelf_x += pw;
        shelf_h = std::max(shelf_h, ph);
        return true;
    }

    bool atlas_grow() {
        if (atlas_size >= kAtlasMax) return false;
        int ns = atlas_size * 2;
        std::vector<uint8_t> np((size_t)ns * ns * 4, 0);
        for (int y = 0; y < atlas_size; ++y)
            std::memcpy(&np[(size_t)y * ns * 4], &pixels[(size_t)y * atlas_size * 4], (size_t)atlas_size * 4);
        pixels.swap(np);
        // Shelves continue below the old content (the current shelf keeps its position; the
        // remaining width of the old rows is not reused, which is fine for 2-3 growths).
        atlas_size = ns;
        needs_full_upload = true;
        dirty = true;
        float inv = 1.f / ns;
        for (auto& kv : cache) {
            Glyph& g = kv.second;
            g.u0 = g.ax * inv; g.v0 = g.ay * inv;
            g.u1 = (g.ax + g.aw) * inv; g.v1 = (g.ay + g.ah) * inv;
        }
        return true;
    }

    void ensure_texture() {
        if (!glGenTextures) return;    // no GL yet; upload happens lazily later
        if (!tex) {
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            needs_full_upload = true;
        }
        if (needs_full_upload && atlas_size > 0) {
            glBindTexture(GL_TEXTURE_2D, tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, atlas_size, atlas_size, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            needs_full_upload = false;
            tex_valid = true;
            dirty = true;
        }
    }

    void upload_rect(int x, int y, int w, int h) {
        ensure_texture();
        if (!tex || !tex_valid || w <= 0 || h <= 0) return;
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, atlas_size);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, &pixels[((size_t)y * atlas_size + x) * 4]);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        dirty = true;
    }

    // Copies an RGBA8 image (premultiplied / mask form already applied) into the atlas.
    // Grows or clears the atlas as needed. Returns false if the image cannot fit at all.
    bool atlas_put(const std::vector<uint8_t>& rgba, int w, int h, int* ax, int* ay) {
        int x = 0, y = 0;
        while (!atlas_alloc(w, h, &x, &y)) {
            if (!atlas_grow()) {
                // 4096^2 exhausted: start over. References into the cache become stale, which
                // is acceptable for this rare case (callers consume glyphs immediately).
                std::fprintf(stderr, "font: glyph atlas full, resetting\n");
                cache.clear();
                atlas_reset(kAtlasMax);
                if (!atlas_alloc(w, h, &x, &y)) return false;
                break;
            }
        }
        for (int row = 0; row < h; ++row)
            std::memcpy(&pixels[((size_t)(y + row) * atlas_size + x) * 4], &rgba[(size_t)row * w * 4], (size_t)w * 4);
        if (needs_full_upload) ensure_texture();
        else upload_rect(x, y, w, h);
        *ax = x; *ay = y;
        return true;
    }

    // ---- faces -------------------------------------------------------------------
    FT_Face open_face(const std::string& path, int index) {
        FT_Face face = nullptr;
        if (FT_New_Face(lib, path.c_str(), index, &face) != 0) return nullptr;
        owned_faces.push_back(face);
        return face;
    }

    // Configures the pixel size of a face for our nominal size. For bitmap-only color
    // fonts, selects the best strike and computes the scale factor to our size.
    void size_face(FaceRef& fr, float size_px) {
        FT_Face face = fr.face;
        if (FT_IS_SCALABLE(face)) {
            FT_Set_Char_Size(face, 0, (FT_F26Dot6)std::lround(size_px * 64.f), 72, 72);
            fr.scale = 1.f;
            fr.color = FT_HAS_COLOR(face) != 0;
            return;
        }
        if (face->num_fixed_sizes > 0) {
            int best = 0;
            int want = (int)std::lround(size_px);
            for (int i = 1; i < face->num_fixed_sizes; ++i) {
                int hb = face->available_sizes[best].height, hi = face->available_sizes[i].height;
                bool best_ok = hb >= want, i_ok = hi >= want;
                if ((i_ok && (!best_ok || hi < hb)) || (!i_ok && !best_ok && hi > hb)) best = i;
            }
            FT_Select_Size(face, best);
            float ppem = (float)(face->size->metrics.y_ppem > 0 ? face->size->metrics.y_ppem : face->available_sizes[best].height);
            fr.scale = size_px / ppem;
            fr.color = FT_HAS_COLOR(face) != 0;
        }
    }
};

// ------------------------------------------------------------------------------------

Font::Font() : impl_(new Impl) {}
Font::~Font() = default;

bool Font::resolve_family(const std::string& family, bool bold, bool italic, std::string* path, int* index) {
    if (!FcInit()) return false;
    FcPattern* p = make_pattern(family, bold, italic);
    bool generic = family.empty() || family == "monospace" || family == "mono";
    bool ok = match_pattern(p, path, index, nullptr, generic ? "" : family);
    FcPatternDestroy(p);
    return ok;
}

bool Font::is_box_drawing(char32_t cp) {
    return (cp >= 0x2500 && cp <= 0x259F) || (cp >= 0xE0B0 && cp <= 0xE0BF);
}

unsigned Font::atlas_texture() const {
    impl_->ensure_texture();
    return impl_->tex;
}

int Font::atlas_size() const { return impl_->atlas_size; }

bool Font::take_atlas_dirty() {
    impl_->ensure_texture();
    bool d = impl_->dirty;
    impl_->dirty = false;
    return d;
}

bool Font::load(const std::string& family, float size_px, int pad_x, int pad_y, std::string* err) {
    Impl& im = *impl_;
    if (!im.lib && FT_Init_FreeType(&im.lib) != 0) {
        if (err) *err = "FT_Init_FreeType failed";
        return false;
    }
    if (!FcInit()) {
        if (err) *err = "fontconfig init failed";
        return false;
    }
    im.family = family;
    im.pad_x = pad_x;
    im.pad_y = pad_y;
    return set_size(size_px);
}

bool Font::set_size(float size_px) {
    Impl& im = *impl_;
    if (!im.lib) return false;
    size_px_ = size_px;

    // Drop everything that depends on the size.
    im.cache.clear();
    for (FaceRef& f : im.fallbacks) if (f.charset) FcCharSetDestroy(f.charset);
    im.fallbacks.clear();
    for (FT_Face f : im.owned_faces) FT_Done_Face(f);
    im.owned_faces.clear();
    for (FaceRef& f : im.styles) f = FaceRef{};
    if (im.langs) { FcLangSetDestroy(im.langs); im.langs = nullptr; }
    im.atlas_reset(im.atlas_size > 0 ? im.atlas_size : kAtlasStart);

    // Primary (regular) face.
    std::string path;
    int index = 0;
    {
        FcPattern* p = make_pattern(im.family, false, false);
        // Generic aliases (monospace, ...) legitimately resolve to another family name.
        bool generic = im.family.empty() || im.family == "monospace" || im.family == "mono";
        bool ok = match_pattern(p, &path, &index, &im.langs, generic ? "" : im.family);
        FcPatternDestroy(p);
        if (!ok) {
            std::fprintf(stderr, "font: fontconfig has no family '%s'\n", im.family.c_str());
            return false;
        }
    }
    FT_Face reg = im.open_face(path, index);
    if (!reg) {
        std::fprintf(stderr, "font: cannot open %s\n", path.c_str());
        return false;
    }
    im.styles[STYLE_REGULAR].face = reg;
    im.styles[STYLE_REGULAR].path = path;
    im.styles[STYLE_REGULAR].index = index;
    im.size_face(im.styles[STYLE_REGULAR], size_px);

    // Bold / italic / bold-italic: real faces when fontconfig finds distinct files,
    // otherwise synthesized from the regular face.
    for (int st = 1; st < 4; ++st) {
        bool bold = st & STYLE_BOLD, italic = st & STYLE_ITALIC;
        FaceRef& fr = im.styles[st];
        std::string spath;
        int sindex = 0;
        FcPattern* p = make_pattern(im.family, bold, italic);
        bool ok = match_pattern(p, &spath, &sindex, nullptr);
        FcPatternDestroy(p);
        FT_Face face = nullptr;
        bool distinct = ok && !(spath == path && sindex == index);
        if (distinct) {
            // Reuse an already opened style face with the same file.
            for (int o = 0; o < st && !face; ++o)
                if (im.styles[o].face && im.styles[o].path == spath && im.styles[o].index == sindex && !im.styles[o].synth_bold && !im.styles[o].synth_italic)
                    face = im.styles[o].face;
            if (!face) face = im.open_face(spath, sindex);
        }
        if (face) {
            fr.face = face;
            fr.path = spath;
            fr.index = sindex;
            im.size_face(fr, size_px);
            // A distinct file may still only cover one of the two axes (e.g. only Bold exists
            // for Bold Italic). Check style flags and synthesize the missing axis.
            bool has_bold = (face->style_flags & FT_STYLE_FLAG_BOLD) != 0;
            bool has_italic = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
            fr.synth_bold = bold && !has_bold;
            fr.synth_italic = italic && !has_italic;
        } else {
            fr = im.styles[STYLE_REGULAR];
            fr.synth_bold = bold;
            fr.synth_italic = italic;
        }
    }

    // Metrics from the regular face.
    FT_Size_Metrics& m = reg->size->metrics;
    float scale = (float)m.y_scale / 65536.f / 64.f;   // font units -> pixels
    float adv = 0.f;
    FT_UInt gi = FT_Get_Char_Index(reg, 'M');
    if (gi && FT_Load_Glyph(reg, gi, FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING) == 0)
        adv = (float)reg->glyph->advance.x / 64.f;
    if (adv <= 0.f) adv = (float)m.max_advance / 64.f;
    if (FT_IS_FIXED_WIDTH(reg)) adv = std::max(adv, (float)m.max_advance / 64.f);
    int ascender = (int)std::lround((float)m.ascender / 64.f);
    int descender = (int)std::lround((float)-m.descender / 64.f);
    int height = (int)std::lround((float)m.height / 64.f);
    if (height < ascender + descender) height = ascender + descender;

    metrics_.cell_w = (int)std::lround(adv) + im.pad_x;
    metrics_.cell_h = height + im.pad_y;
    int extra = metrics_.cell_h - (ascender + descender);   // line gap + padding, split around the glyph box
    metrics_.ascent = ascender + extra / 2;
    metrics_.descent = metrics_.cell_h - metrics_.ascent;
    float ul_pos = reg->underline_position * scale;          // negative = below baseline
    float ul_th = reg->underline_thickness * scale;
    metrics_.underline_thickness = std::max(1, (int)std::lround(ul_th));
    int ul = metrics_.ascent + (int)std::lround(-ul_pos);
    if (ul_pos == 0.f) ul = metrics_.ascent + std::max(1, descender / 2);
    metrics_.underline_pos = std::min(ul, metrics_.cell_h - metrics_.underline_thickness);
    float x_height = 0.f;
    if (auto* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(reg, FT_SFNT_OS2)); os2 && os2->sxHeight > 0)
        x_height = os2->sxHeight * scale;
    if (x_height <= 0.f) x_height = ascender * 0.5f;
    metrics_.strike_pos = metrics_.ascent - (int)std::lround(x_height * 0.5f);

    // .notdef for the "nothing works" case.
    im.notdef_glyph = Glyph{};
    im.notdef_glyph.advance = (float)adv;
    return true;
}

// Rasterizes glyph index gi from face fr into an RGBA buffer. Returns false on failure.
static bool render_glyph(FaceRef& fr, FT_UInt gi, float size_px, const FontMetrics& metrics,
                         std::vector<uint8_t>& out, int* w, int* h, int* bx, int* by, float* advance, bool* is_color) {
    FT_Face face = fr.face;
    FT_Int32 flags = FT_LOAD_TARGET_LIGHT;
    if (face->face_flags & FT_FACE_FLAG_HINTER) flags |= FT_LOAD_NO_AUTOHINT;
    if (fr.color) flags |= FT_LOAD_COLOR;
    if (fr.synth_italic) {
        FT_Matrix shear{0x10000L, 0x4000L, 0, 0x10000L};   // ~14 degrees
        FT_Set_Transform(face, &shear, nullptr);
    }
    FT_Error e = FT_Load_Glyph(face, gi, flags);
    if (fr.synth_italic) FT_Set_Transform(face, nullptr, nullptr);
    if (e != 0) return false;
    FT_GlyphSlot slot = face->glyph;
    *advance = (float)slot->advance.x / 64.f * fr.scale;
    if (fr.synth_bold && slot->format == FT_GLYPH_FORMAT_OUTLINE) {
        FT_Pos strength = FT_MulFix(face->units_per_EM, face->size->metrics.y_scale) / 28;
        FT_Outline_EmboldenXY(&slot->outline, strength, strength / 2);
        FT_Outline_Translate(&slot->outline, -strength / 4, 0);
    }
    if (slot->format != FT_GLYPH_FORMAT_BITMAP) {
        if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) return false;
    }
    FT_Bitmap& bm = slot->bitmap;
    int sw = (int)bm.width, sh = (int)bm.rows;
    *is_color = false;
    if (sw <= 0 || sh <= 0) { *w = *h = 0; *bx = *by = 0; return true; }

    if (bm.pixel_mode == FT_PIXEL_MODE_BGRA) {
        *is_color = true;
        // Scale the strike to our size, then make sure it fits the cell (2 cells wide max).
        float s = fr.scale;
        float max_h = (float)std::max(1, metrics.cell_h - 0);
        float max_w = (float)std::max(1, metrics.cell_w * 2);
        if (sh * s > max_h) s = max_h / sh;
        if (sw * s > max_w) s = max_w / sw;
        int dw = std::max(1, (int)std::lround(sw * s)), dh = std::max(1, (int)std::lround(sh * s));
        out.assign((size_t)dw * dh * 4, 0);
        float fx = (float)sw / dw, fy = (float)sh / dh;   // src pixels per dst pixel
        for (int y = 0; y < dh; ++y) {
            float y0 = y * fy, y1 = (y + 1) * fy;
            int iy0 = (int)std::floor(y0), iy1 = std::min(sh, (int)std::ceil(y1));
            for (int x = 0; x < dw; ++x) {
                float x0 = x * fx, x1 = (x + 1) * fx;
                int ix0 = (int)std::floor(x0), ix1 = std::min(sw, (int)std::ceil(x1));
                float acc[4] = {0, 0, 0, 0}, wsum = 0.f;
                for (int sy = iy0; sy < iy1; ++sy) {
                    float wy = std::min(y1, (float)sy + 1) - std::max(y0, (float)sy);
                    if (wy <= 0) continue;
                    const uint8_t* row = bm.buffer + (size_t)sy * bm.pitch;
                    for (int sx = ix0; sx < ix1; ++sx) {
                        float wx = std::min(x1, (float)sx + 1) - std::max(x0, (float)sx);
                        if (wx <= 0) continue;
                        float wgt = wx * wy;
                        const uint8_t* p = row + sx * 4;   // B G R A premultiplied
                        acc[0] += p[2] * wgt; acc[1] += p[1] * wgt; acc[2] += p[0] * wgt; acc[3] += p[3] * wgt;
                        wsum += wgt;
                    }
                }
                uint8_t* d = &out[((size_t)y * dw + x) * 4];
                if (wsum > 0) for (int c = 0; c < 4; ++c) d[c] = (uint8_t)std::min(255.f, acc[c] / wsum + 0.5f);
            }
        }
        *w = dw; *h = dh;
        *bx = (int)std::lround(slot->bitmap_left * s);
        *by = (int)std::lround(slot->bitmap_top * s);
        *advance = (float)slot->advance.x / 64.f * s;
        return true;
    }

    // Grayscale / mono coverage -> rgb = 255, a = coverage.
    out.assign((size_t)sw * sh * 4, 255);
    for (int y = 0; y < sh; ++y) {
        const uint8_t* row = bm.buffer + (size_t)y * bm.pitch;
        uint8_t* d = &out[(size_t)y * sw * 4];
        if (bm.pixel_mode == FT_PIXEL_MODE_MONO) {
            for (int x = 0; x < sw; ++x) d[x * 4 + 3] = (row[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0;
        } else if (bm.pixel_mode == FT_PIXEL_MODE_GRAY) {
            int ng = bm.num_grays > 1 ? bm.num_grays - 1 : 255;
            for (int x = 0; x < sw; ++x) d[x * 4 + 3] = ng == 255 ? row[x] : (uint8_t)(row[x] * 255 / ng);
        } else {
            for (int x = 0; x < sw; ++x) d[x * 4 + 3] = row[x];
        }
    }
    *w = sw; *h = sh;
    *bx = slot->bitmap_left;
    *by = slot->bitmap_top;
    (void)size_px;
    return true;
}

const Glyph& Font::glyph(char32_t cp, GlyphStyle style) {
    Impl& im = *impl_;
    uint64_t key = glyph_key(cp, style);
    auto it = im.cache.find(key);
    if (it != im.cache.end()) return it->second;
    if (!im.styles[STYLE_REGULAR].face) return im.notdef_glyph;
    if (is_zero_width(cp)) {
        Glyph z;
        z.valid = true;
        return im.cache.emplace(key, z).first->second;
    }

    FaceRef* fr = &im.styles[style & 3];
    FT_UInt gi = FT_Get_Char_Index(fr->face, cp);
    if (!gi && fr->face != im.styles[STYLE_REGULAR].face) {
        // Styled face lacks it; try the regular face with synthesis.
        gi = FT_Get_Char_Index(im.styles[STYLE_REGULAR].face, cp);
        if (gi) {
            static thread_local FaceRef tmp;
            tmp = im.styles[STYLE_REGULAR];
            tmp.synth_bold = style & STYLE_BOLD;
            tmp.synth_italic = style & STYLE_ITALIC;
            fr = &tmp;
        }
    }
    bool valid = gi != 0;

    if (!gi) {
        // Fallback through already loaded faces first.
        for (FaceRef& f : im.fallbacks) {
            if (f.charset && !FcCharSetHasChar(f.charset, cp)) continue;
            FT_UInt g2 = FT_Get_Char_Index(f.face, cp);
            if (g2) { fr = &f; gi = g2; valid = true; break; }
        }
    }
    if (!gi) {
        // Ask fontconfig for a face covering this code point.
        FcPattern* p = make_pattern(im.family, style & STYLE_BOLD, style & STYLE_ITALIC);
        FcPatternDel(p, FC_SPACING);
        FcPatternDel(p, FC_SCALABLE);
        FcCharSet* cs = FcCharSetCreate();
        FcCharSetAddChar(cs, cp);
        FcPatternAddCharSet(p, FC_CHARSET, cs);
        if (is_emoji_presentation(cp)) FcPatternAddBool(p, FC_COLOR, FcTrue);   // prefer Noto Color Emoji
        if (im.langs) FcPatternAddLangSet(p, FC_LANG, im.langs);
        std::string fpath;
        int findex = 0;
        bool ok = match_pattern(p, &fpath, &findex, nullptr);
        FcCharSetDestroy(cs);
        FcPatternDestroy(p);
        if (ok) {
            FaceRef* found = nullptr;
            for (FaceRef& f : im.fallbacks)
                if (f.path == fpath && f.index == findex) { found = &f; break; }
            if (!found) {
                FT_Face face = im.open_face(fpath, findex);
                if (face) {
                    FaceRef nf;
                    nf.face = face;
                    nf.path = fpath;
                    nf.index = findex;
                    im.size_face(nf, size_px_);
                    FcCharSet* fcs = nullptr;
                    if (FT_Get_Char_Index(face, cp)) {
                        // Coverage set for cheap future checks.
                        fcs = FcFreeTypeCharSet(face, nullptr);
                    }
                    nf.charset = fcs;
                    if ((int)im.fallbacks.size() < kMaxFallbackFaces) {
                        im.fallbacks.push_back(nf);
                        found = &im.fallbacks.back();
                    } else {
                        FaceRef& victim = im.fallbacks[im.fallback_rr++ % kMaxFallbackFaces];
                        if (victim.charset) FcCharSetDestroy(victim.charset);
                        victim = nf;   // old FT_Face stays open in owned_faces until set_size
                        found = &victim;
                    }
                }
            }
            if (found) {
                FT_UInt g2 = FT_Get_Char_Index(found->face, cp);
                if (g2) { fr = found; gi = g2; valid = true; }
            }
        }
    }
    if (!gi) {
        fr = &im.styles[style & 3];
        gi = 0;   // .notdef of the primary style face
    }

    Glyph g;
    g.valid = valid;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0, bx = 0, by = 0;
    float adv = 0.f;
    bool is_color = false;
    if (render_glyph(*fr, gi, size_px_, metrics_, rgba, &w, &h, &bx, &by, &adv, &is_color)) {
        g.advance = adv;
        g.color = is_color;
        g.bearing_x = bx;
        g.bearing_y = by;
        if (w > 0 && h > 0) {
            int ax = 0, ay = 0;
            if (im.atlas_put(rgba, w, h, &ax, &ay)) {
                g.ax = ax; g.ay = ay; g.aw = w; g.ah = h;
                float inv = 1.f / im.atlas_size;
                g.u0 = ax * inv; g.v0 = ay * inv; g.u1 = (ax + w) * inv; g.v1 = (ay + h) * inv;
            } else {
                g.aw = g.ah = 0;
            }
        }
    } else {
        g.advance = (float)(metrics_.cell_w - im.pad_x);
        g.valid = false;
    }
    if (g.advance <= 0.f && !valid) g.advance = (float)(metrics_.cell_w - im.pad_x);
    auto res = im.cache.emplace(key, g);
    return res.first->second;
}

} // namespace canopy::gfx
