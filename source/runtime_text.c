/* runtime text: preserved standalone implementation. */
#define _GNU_SOURCE 1
#include "runtime.h"


/* ---- cocos text ---- */
#pragma push_macro("COCOS_TEXT_MAX_DIM")
#pragma push_macro("COCOS_TEXT_MAX_LINES")


#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>
#include <ft2build.h>
#include FT_FREETYPE_H


#endif

#define COCOS_TEXT_MAX_DIM 4096
#define COCOS_TEXT_MAX_LINES 512u

typedef struct cocos_text_CocosTextLine {
    size_t begin;
    size_t end;
    int width;
} cocos_text_CocosTextLine;

static float cocos_text_clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static uint8_t cocos_text_color8(float v) {
    const float c = cocos_text_clamp01(v) * 255.0f;
    return (uint8_t)(c + 0.5f);
}

static bool cocos_text_text_request_valid(const Etd2xCocosTextRequest *r) {
    if (!r || !r->text || r->font_size <= 0 || r->font_size > COCOS_TEXT_MAX_DIM) return false;
    if (r->width < 0 || r->height < 0) return false;
    if (r->width > COCOS_TEXT_MAX_DIM || r->height > COCOS_TEXT_MAX_DIM) return false;
    if (!isfinite(r->tint_r) || !isfinite(r->tint_g) || !isfinite(r->tint_b) || !isfinite(r->tint_a) ||
        !isfinite(r->line_spacing) || fabsf(r->line_spacing) > COCOS_TEXT_MAX_DIM ||
        !isfinite(r->shadow_dx) || !isfinite(r->shadow_dy) || !isfinite(r->shadow_blur) ||
        fabsf(r->shadow_dx) > COCOS_TEXT_MAX_DIM || fabsf(r->shadow_dy) > COCOS_TEXT_MAX_DIM ||
        !isfinite(r->shadow_opacity) ||
        !isfinite(r->stroke_r) || !isfinite(r->stroke_g) || !isfinite(r->stroke_b) ||
        !isfinite(r->stroke_a) || !isfinite(r->stroke_size)) return false;
    return true;
}

typedef bool (*cocos_text_CocosShrinkMeasure)(void *, const Etd2xCocosTextRequest *, int, int, int *, int *);
/* Cocos 3.17/4.0 measure with the previous Paint size, then decrement the
 * size by one. No-wrap height uses the candidate size; wrapped height uses
 * the previous size. A zero bound skips fitting; exhausting sizes restores
 * the original. Line spacing is applied only to the final layout. */
static int cocos_text_shrink_font_size(const Etd2xCocosTextRequest *r, cocos_text_CocosShrinkMeasure measure, void *ctx) {
    if (!r->shrink || !r->width || !r->height) return r->font_size;
    int paint_size = r->font_size;
    for (int candidate = r->font_size; candidate > 0; --candidate) {
        int width = 0, height = 0;
        if (!measure(ctx, r, paint_size, candidate, &width, &height)) return r->font_size;
        paint_size = candidate;
        if (width <= r->width && height <= r->height) return candidate;
    }
    return r->font_size;
}

#ifdef __SWITCH__
static size_t cocos_text_utf8_decode(const char *s, size_t remaining, uint32_t *out_cp) {
    if (!s || !remaining || !out_cp) return 0;
    const uint8_t c0 = (uint8_t)s[0];
    if (c0 < 0x80u) { *out_cp = c0; return 1; }
    if ((c0 & 0xe0u) == 0xc0u && remaining >= 2u) {
        const uint8_t c1 = (uint8_t)s[1];
        if ((c1 & 0xc0u) != 0x80u) return 0;
        const uint32_t cp = ((uint32_t)(c0 & 0x1fu) << 6) | (uint32_t)(c1 & 0x3fu);
        if (cp < 0x80u) return 0;
        *out_cp = cp; return 2;
    }
    if ((c0 & 0xf0u) == 0xe0u && remaining >= 3u) {
        const uint8_t c1 = (uint8_t)s[1], c2 = (uint8_t)s[2];
        if ((c1 & 0xc0u) != 0x80u || (c2 & 0xc0u) != 0x80u) return 0;
        const uint32_t cp = ((uint32_t)(c0 & 0x0fu) << 12) |
                            ((uint32_t)(c1 & 0x3fu) << 6) | (uint32_t)(c2 & 0x3fu);
        if (cp < 0x800u || (cp >= 0xd800u && cp <= 0xdfffu)) return 0;
        *out_cp = cp; return 3;
    }
    if ((c0 & 0xf8u) == 0xf0u && remaining >= 4u) {
        const uint8_t c1 = (uint8_t)s[1], c2 = (uint8_t)s[2], c3 = (uint8_t)s[3];
        if ((c1 & 0xc0u) != 0x80u || (c2 & 0xc0u) != 0x80u || (c3 & 0xc0u) != 0x80u) return 0;
        const uint32_t cp = ((uint32_t)(c0 & 0x07u) << 18) |
                            ((uint32_t)(c1 & 0x3fu) << 12) |
                            ((uint32_t)(c2 & 0x3fu) << 6) | (uint32_t)(c3 & 0x3fu);
        if (cp < 0x10000u || cp > 0x10ffffu) return 0;
        *out_cp = cp; return 4;
    }
    /* Match Java's forgiving String conversion closely enough for malformed
     * guest bytes: consume one byte and emit U+FFFD instead of aborting. */
    *out_cp = 0xfffdu;
    return 1;
}

#endif

static void cocos_text_bitmap_reset(Etd2xCocosTextBitmap *b) {
    if (b) memset(b, 0, sizeof(*b));
}

void etd2x_cocos_text_bitmap_free(Etd2xCocosTextBitmap *bitmap) {
    if (!bitmap) return;
    free(bitmap->pixels);
    cocos_text_bitmap_reset(bitmap);
}

static void cocos_text_blend_pixel_rgba(uint8_t *dst, uint8_t r, uint8_t g, uint8_t b,
                             uint8_t base_alpha, uint8_t coverage) {
    if (!dst || coverage == 0 || base_alpha == 0) return;
    const unsigned sa = ((unsigned)base_alpha * coverage + 127u) / 255u;
    if (!sa) return;
    const unsigned inv = 255u - sa;
    /* Android text bitmaps are premultiplied. Keep the compatibility bitmap
     * premultiplied so Cocos' existing Android native conversion sees the same
     * kind of source data it gets from Bitmap.copyPixelsToBuffer(). */
    const unsigned sr = ((unsigned)r * sa + 127u) / 255u;
    const unsigned sg = ((unsigned)g * sa + 127u) / 255u;
    const unsigned sb = ((unsigned)b * sa + 127u) / 255u;
    dst[0] = (uint8_t)(sr + ((unsigned)dst[0] * inv + 127u) / 255u);
    dst[1] = (uint8_t)(sg + ((unsigned)dst[1] * inv + 127u) / 255u);
    dst[2] = (uint8_t)(sb + ((unsigned)dst[2] * inv + 127u) / 255u);
    dst[3] = (uint8_t)(sa + ((unsigned)dst[3] * inv + 127u) / 255u);
}

#ifdef __SWITCH__

static FT_Library cocos_text_g_ft_library;
static bool cocos_text_g_ft_ready;
static bool cocos_text_g_pl_ready;
static PlFontData cocos_text_g_shared_font;
static bool cocos_text_g_shared_font_ready;

static Etd2xResult cocos_text_ensure_freetype(void) {
    if (cocos_text_g_ft_ready) return ETD2X_OK;
    if (FT_Init_FreeType(&cocos_text_g_ft_library) != 0) return ETD2X_ERR_APK_UNSUPPORTED;
    cocos_text_g_ft_ready = true;
    return ETD2X_OK;
}

static Etd2xResult cocos_text_ensure_shared_font(void) {
    Etd2xResult rc = cocos_text_ensure_freetype();
    if (rc != ETD2X_OK) return rc;
    if (!cocos_text_g_pl_ready) {
        const Result prc = plInitialize(PlServiceType_User);
        if (R_FAILED(prc)) {
            ETD2X_LOGCW("COCOS-TEXT", "plInitialize failed rc=0x%x", prc);
            return ETD2X_ERR_APK_UNSUPPORTED;
        }
        cocos_text_g_pl_ready = true;
    }
    if (!cocos_text_g_shared_font_ready) {
        const Result prc = plGetSharedFontByType(&cocos_text_g_shared_font, PlSharedFontType_Standard);
        if (R_FAILED(prc) || !cocos_text_g_shared_font.address || !cocos_text_g_shared_font.size) {
            ETD2X_LOGCW("COCOS-TEXT", "plGetSharedFontByType failed rc=0x%x", prc);
            return ETD2X_ERR_APK_UNSUPPORTED;
        }
        cocos_text_g_shared_font_ready = true;
    }
    return ETD2X_OK;
}

static bool cocos_text_suffix_case_equal(const char *s, const char *suffix) {
    if (!s || !suffix) return false;
    const size_t sn = strlen(s), nn = strlen(suffix);
    if (sn < nn) return false;
    s += sn - nn;
    for (size_t i = 0; i < nn; ++i)
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)suffix[i])) return false;
    return true;
}

typedef struct cocos_text_FontFaceOwner {
    FT_Face face;
    Etd2xAsset *asset;
} cocos_text_FontFaceOwner;

static void cocos_text_font_face_close(cocos_text_FontFaceOwner *owner) {
    if (!owner) return;
    if (owner->face) FT_Done_Face(owner->face);
    if (owner->asset) etd2x_asset_close(owner->asset);
    memset(owner, 0, sizeof(*owner));
}

static bool cocos_text_try_asset_face_name(const char *asset_name, cocos_text_FontFaceOwner *owner) {
    if (!asset_name || !asset_name[0] || !owner || !etd2x_assets_manager()) return false;
    Etd2xAsset *asset = etd2x_asset_open(etd2x_assets_manager(), asset_name, ETD2X_AASSET_MODE_BUFFER);
    if (!asset) return false;
    const int64_t len64 = etd2x_asset_length64(asset);
    const void *data = etd2x_asset_buffer(asset);
    if (!data || len64 <= 0 || (uint64_t)len64 > (uint64_t)0x7fffffff) {
        etd2x_asset_close(asset);
        return false;
    }
    FT_Face face = NULL;
    if (FT_New_Memory_Face(cocos_text_g_ft_library, (const FT_Byte *)data, (FT_Long)len64, 0, &face) != 0) {
        etd2x_asset_close(asset);
        return false;
    }
    owner->face = face;
    owner->asset = asset;
    return true;
}

static Etd2xResult cocos_text_open_font_face(const char *font_name, int font_size,
                                  cocos_text_FontFaceOwner *owner, char *source, size_t source_cap) {
    if (!owner || !source || source_cap == 0 || font_size <= 0) return ETD2X_ERR_ARGUMENT;
    memset(owner, 0, sizeof(*owner));
    source[0] = '\0';
    Etd2xResult rc = cocos_text_ensure_freetype();
    if (rc != ETD2X_OK) return rc;

    if (font_name && font_name[0] &&
        (cocos_text_suffix_case_equal(font_name, ".ttf") || cocos_text_suffix_case_equal(font_name, ".otf"))) {
        const char *asset_name = font_name;
        if (strncmp(asset_name, "assets/", 7) == 0) asset_name += 7;
        if (cocos_text_try_asset_face_name(asset_name, owner)) {
            snprintf(source, source_cap, "asset");
        } else if (!strchr(asset_name, '/')) {
            char candidate[512];
            const int candidate_len = snprintf(candidate, sizeof(candidate), "fonts/%s", asset_name);
            if (candidate_len > 0 && (size_t)candidate_len < sizeof(candidate) &&
                cocos_text_try_asset_face_name(candidate, owner)) {
                snprintf(source, source_cap, "asset-fonts");
            }
        }
    }

    if (!owner->face) {
        rc = cocos_text_ensure_shared_font();
        if (rc != ETD2X_OK) return rc;
        if (FT_New_Memory_Face(cocos_text_g_ft_library, (const FT_Byte *)cocos_text_g_shared_font.address,
                               (FT_Long)cocos_text_g_shared_font.size, 0, &owner->face) != 0)
            return ETD2X_ERR_APK_UNSUPPORTED;
        snprintf(source, source_cap, "switch-shared");
    }
    if (FT_Set_Pixel_Sizes(owner->face, 0, (FT_UInt)font_size) != 0) {
        cocos_text_font_face_close(owner);
        return ETD2X_ERR_APK_UNSUPPORTED;
    }
    return ETD2X_OK;
}

/* FreeType library-level calls (face creation/destruction) are not thread-safe;
 * Paint queries can run on any Java thread next to Cocos rasterization. */
static pthread_mutex_t cocos_text_g_ft_lock = PTHREAD_MUTEX_INITIALIZER;

static Etd2xResult cocos_text_measure_switch(const char *text, float font_size, float *out_width) {
    cocos_text_FontFaceOwner owner = {0};
    char source[32];
    const int pixels = (int)ceilf(font_size);
    Etd2xResult rc = cocos_text_open_font_face(NULL, pixels > 0 ? pixels : 1, &owner, source, sizeof(source));
    if (rc != ETD2X_OK) return rc;
    (void)FT_Set_Char_Size(owner.face, 0, (FT_F26Dot6)(font_size * 64.0f), 72, 72);
    double width = 0.0;
    const size_t len = strlen(text);
    for (size_t p = 0; p < len;) {
        uint32_t cp = 0;
        const size_t n = cocos_text_utf8_decode(text + p, len - p, &cp);
        if (!n) break;
        p += n;
        const FT_UInt glyph = FT_Get_Char_Index(owner.face, (FT_ULong)cp);
        if (FT_Load_Glyph(owner.face, glyph, FT_LOAD_NO_HINTING) == 0)
            width += (double)owner.face->glyph->linearHoriAdvance / 65536.0;
    }
    cocos_text_font_face_close(&owner);
    *out_width = (float)width;
    return ETD2X_OK;
}

static Etd2xResult cocos_text_has_glyphs_switch(const char *text, bool *out_has) {
    cocos_text_FontFaceOwner owner = {0};
    char source[32];
    Etd2xResult rc = cocos_text_open_font_face(NULL, 16, &owner, source, sizeof(source));
    if (rc != ETD2X_OK) return rc;
    bool has = true;
    const size_t len = strlen(text);
    for (size_t p = 0; p < len && has;) {
        uint32_t cp = 0;
        const size_t n = cocos_text_utf8_decode(text + p, len - p, &cp);
        if (!n) { has = false; break; }
        p += n;
        /* Variation selectors and joiners combine with a base glyph. */
        if (cp == 0x200Du || (cp >= 0xFE00u && cp <= 0xFE0Fu) || (cp >= 0xE0100u && cp <= 0xE01EFu)) continue;
        has = FT_Get_Char_Index(owner.face, (FT_ULong)cp) != 0;
    }
    cocos_text_font_face_close(&owner);
    *out_has = has;
    return ETD2X_OK;
}

static int cocos_text_glyph_advance(FT_Face face, uint32_t cp, FT_UInt *io_previous) {
    const FT_UInt glyph = FT_Get_Char_Index(face, (FT_ULong)cp);
    int advance = 0;
    if (FT_HAS_KERNING(face) && io_previous && *io_previous && glyph) {
        FT_Vector kern = {0, 0};
        if (FT_Get_Kerning(face, *io_previous, glyph, FT_KERNING_DEFAULT, &kern) == 0)
            advance += (int)(kern.x >> 6);
    }
    if (FT_Load_Glyph(face, glyph, FT_LOAD_DEFAULT) == 0)
        advance += (int)(face->glyph->advance.x >> 6);
    if (io_previous) *io_previous = glyph;
    return advance > 0 ? advance : 1;
}

static int cocos_text_measure_span(FT_Face face, const char *text, size_t begin, size_t end) {
    int width = 0;
    FT_UInt previous = 0;
    for (size_t p = begin; p < end;) {
        uint32_t cp = 0;
        const size_t n = cocos_text_utf8_decode(text + p, end - p, &cp);
        if (!n) break;
        p += n;
        if (cp == '\r') continue;
        width += cocos_text_glyph_advance(face, cp, &previous);
    }
    return width;
}

static bool cocos_text_push_line(cocos_text_CocosTextLine *lines, size_t *count, size_t begin, size_t end, int width,
                      size_t max_lines) {
    if (!lines || !count || *count >= COCOS_TEXT_MAX_LINES || *count >= max_lines) return false;
    lines[*count] = (cocos_text_CocosTextLine){.begin = begin, .end = end, .width = width};
    (*count)++;
    return true;
}

static size_t cocos_text_build_lines(FT_Face face, const char *text, int max_width, int max_height,
                          int line_height, cocos_text_CocosTextLine *lines, int *out_max_width) {
    const size_t text_len = strlen(text);
    size_t count = 0;
    int widest = 0;
    size_t max_lines = COCOS_TEXT_MAX_LINES;
    if (max_height > 0 && line_height > 0) {
        const int bounded = max_height / line_height;
        if (bounded > 0 && (size_t)bounded < max_lines) max_lines = (size_t)bounded;
    }

    size_t segment_begin = 0;
    while (segment_begin <= text_len && count < max_lines) {
        size_t segment_end = segment_begin;
        while (segment_end < text_len && text[segment_end] != '\n') segment_end++;

        if (max_width <= 0) {
            const int w = cocos_text_measure_span(face, text, segment_begin, segment_end);
            if (!cocos_text_push_line(lines, &count, segment_begin, segment_end, w, max_lines)) break;
            if (w > widest) widest = w;
        } else if (segment_begin == segment_end) {
            if (!cocos_text_push_line(lines, &count, segment_begin, segment_end, 0, max_lines)) break;
        } else {
            size_t line_begin = segment_begin;
            while (line_begin < segment_end && count < max_lines) {
                size_t p = line_begin, best_end = line_begin, last_space_end = 0;
                int width = 0, width_at_space = 0;
                FT_UInt previous = 0;
                while (p < segment_end) {
                    uint32_t cp = 0;
                    const size_t n = cocos_text_utf8_decode(text + p, segment_end - p, &cp);
                    if (!n) break;
                    const int adv = cocos_text_glyph_advance(face, cp, &previous);
                    if (width + adv > max_width && best_end > line_begin) break;
                    width += adv;
                    p += n;
                    best_end = p;
                    if (cp == ' ' || cp == '\t') {
                        last_space_end = p;
                        width_at_space = width;
                    }
                    if (width >= max_width) break;
                }
                size_t line_end = best_end;
                int line_width = width;
                if (best_end < segment_end && last_space_end > line_begin) {
                    line_end = last_space_end;
                    line_width = width_at_space;
                }
                if (line_end <= line_begin) {
                    uint32_t cp = 0;
                    const size_t n = cocos_text_utf8_decode(text + line_begin, segment_end - line_begin, &cp);
                    line_end = line_begin + (n ? n : 1u);
                    line_width = cocos_text_measure_span(face, text, line_begin, line_end);
                }
                size_t trim_end = line_end;
                while (trim_end > line_begin && (text[trim_end - 1u] == ' ' || text[trim_end - 1u] == '\t'))
                    trim_end--;
                line_width = cocos_text_measure_span(face, text, line_begin, trim_end);
                if (!cocos_text_push_line(lines, &count, line_begin, trim_end, line_width, max_lines)) break;
                if (line_width > widest) widest = line_width;
                line_begin = line_end;
                while (line_begin < segment_end && (text[line_begin] == ' ' || text[line_begin] == '\t')) line_begin++;
            }
        }

        if (segment_end == text_len) break;
        segment_begin = segment_end + 1u;
    }
    if (count == 0) (void)cocos_text_push_line(lines, &count, 0, 0, 0, max_lines);
    if (max_width > 0) widest = max_width;
    if (out_max_width) *out_max_width = widest;
    return count;
}

static void cocos_text_draw_ft_glyph_bitmap(Etd2xCocosTextBitmap *bitmap, const FT_Bitmap *glyph,
                                 int dst_x, int dst_y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!bitmap || !bitmap->pixels || !glyph || glyph->pixel_mode != FT_PIXEL_MODE_GRAY) return;
    for (unsigned gy = 0; gy < glyph->rows; ++gy) {
        const int y = dst_y + (int)gy;
        if (y < 0 || y >= bitmap->height) continue;
        const uint8_t *row = glyph->pitch >= 0
            ? glyph->buffer + (ptrdiff_t)gy * glyph->pitch
            : glyph->buffer + (ptrdiff_t)(glyph->rows - 1u - gy) * (-(ptrdiff_t)glyph->pitch);
        for (unsigned gx = 0; gx < glyph->width; ++gx) {
            const int x = dst_x + (int)gx;
            if (x < 0 || x >= bitmap->width) continue;
            const uint8_t coverage = row[gx];
            cocos_text_blend_pixel_rgba(bitmap->pixels + ((size_t)y * (size_t)bitmap->width + (size_t)x) * 4u,
                             r, g, b, a, coverage);
        }
    }
}

static void cocos_text_draw_ft_span(FT_Face face, Etd2xCocosTextBitmap *bitmap, const char *text,
                         size_t begin, size_t end, int start_x, int baseline_y,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t a, int offset_x, int offset_y) {
    int pen_x = start_x;
    FT_UInt previous = 0;
    for (size_t p = begin; p < end;) {
        uint32_t cp = 0;
        const size_t n = cocos_text_utf8_decode(text + p, end - p, &cp);
        if (!n) break;
        p += n;
        if (cp == '\r') continue;
        const FT_UInt glyph_index = FT_Get_Char_Index(face, (FT_ULong)cp);
        if (FT_HAS_KERNING(face) && previous && glyph_index) {
            FT_Vector kern = {0, 0};
            if (FT_Get_Kerning(face, previous, glyph_index, FT_KERNING_DEFAULT, &kern) == 0)
                pen_x += (int)(kern.x >> 6);
        }
        if (FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT) != 0) { previous = glyph_index; continue; }
        const int advance = (int)(face->glyph->advance.x >> 6);
        if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) == 0) {
            const int gx = pen_x + face->glyph->bitmap_left + offset_x;
            const int gy = baseline_y - face->glyph->bitmap_top + offset_y;
            cocos_text_draw_ft_glyph_bitmap(bitmap, &face->glyph->bitmap, gx, gy, r, g, b, a);
        }
        pen_x += advance > 0 ? advance : 1;
        previous = glyph_index;
    }
}

static int cocos_text_line_start_x(const cocos_text_CocosTextLine *line, int max_width, int horizontal) {
    if (!line) return 0;
    if (horizontal == 3) return max_width / 2 - line->width / 2;
    if (horizontal == 2) return max_width - line->width;
    return 0;
}

static bool cocos_text_measure_shrink_switch(void *ctx, const Etd2xCocosTextRequest *r, int paint_size,
                                  int candidate, int *width, int *height) {
    FT_Face face = ctx;
    if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)paint_size)) return false;
    int line_height = (int)((face->size->metrics.height + 63) >> 6);
    if (line_height <= 0) line_height = paint_size;
    cocos_text_CocosTextLine lines[COCOS_TEXT_MAX_LINES];
    const size_t count = cocos_text_build_lines(face, r->text, r->shrink_wrap ? r->width : 0,
                                      0, line_height, lines, width);
    if (!r->shrink_wrap) {
        if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)candidate)) return false;
        line_height = (int)((face->size->metrics.ascender - face->size->metrics.descender + 63) >> 6);
        if (line_height <= 0) line_height = candidate;
    }
    *height = count >= COCOS_TEXT_MAX_LINES ? COCOS_TEXT_MAX_DIM + 1 : (int)count * line_height;
    return true;
}

static Etd2xResult cocos_text_rasterize_switch(const Etd2xCocosTextRequest *r, Etd2xCocosTextBitmap *out) {
    cocos_text_FontFaceOwner owner = {0};
    char font_source[32] = {0};
    Etd2xResult rc = cocos_text_open_font_face(r->font_name, r->font_size, &owner, font_source, sizeof(font_source));
    if (rc != ETD2X_OK) return rc;
    FT_Face face = owner.face;
    const int fitted_size = cocos_text_shrink_font_size(r, cocos_text_measure_shrink_switch, face);
    if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)fitted_size)) {
        cocos_text_font_face_close(&owner); return ETD2X_ERR_APK_UNSUPPORTED;
    }

    int line_height = (int)((face->size->metrics.height + 63) >> 6);
    int ascender = (int)((face->size->metrics.ascender + 63) >> 6);
    if (line_height <= 0) line_height = fitted_size;
    if (ascender <= 0) ascender = fitted_size;
    const int line_step = (int)fmaxf(1.0f, line_height + r->line_spacing);

    cocos_text_CocosTextLine lines[COCOS_TEXT_MAX_LINES];
    int max_content_width = 0;
    const size_t line_count = cocos_text_build_lines(face, r->text, r->no_wrap ? 0 : r->width, r->height, line_step,
                                          lines, &max_content_width);
    if (r->no_wrap && r->width > 0) max_content_width = r->width;
    const int total_height = line_height + (int)(line_count - 1u) * line_step;
    const int bitmap_text_height = r->height == 0 ? total_height : r->height;
    const int pad_x = r->shadow ? (int)fabsf(r->shadow_dx) : 0;
    const int pad_y = r->shadow ? (int)fabsf(r->shadow_dy) : 0;
    const int width = max_content_width + pad_x;
    const int height = bitmap_text_height + pad_y;
    if (width <= 0 || height <= 0 || width > COCOS_TEXT_MAX_DIM || height > COCOS_TEXT_MAX_DIM) {
        cocos_text_font_face_close(&owner);
        return ETD2X_ERR_APK_UNSUPPORTED;
    }
    const size_t pixel_count = (size_t)width * (size_t)height;
    if (pixel_count > SIZE_MAX / 4u) { cocos_text_font_face_close(&owner); return ETD2X_ERR_NO_MEMORY; }
    uint8_t *pixels = (uint8_t *)calloc(pixel_count, 4u);
    if (!pixels) { cocos_text_font_face_close(&owner); return ETD2X_ERR_NO_MEMORY; }

    out->width = width;
    out->height = height;
    out->pixels = pixels;
    out->byte_count = pixel_count * 4u;
    out->line_count = line_count;
    out->font_size = fitted_size;
    snprintf(out->font_source, sizeof(out->font_source), "%s", font_source);

    const int horizontal = r->alignment & 0x0f;
    const int vertical = (r->alignment >> 4) & 0x0f;
    int baseline = ascender;
    if (r->height > total_height) {
        if (vertical == 3) baseline += (r->height - total_height) / 2;
        else if (vertical == 2) baseline += r->height - total_height;
    }
    const int render_dx = r->shadow && r->shadow_dx < 0.0f ? pad_x : 0;
    const int render_dy = r->shadow && r->shadow_dy < 0.0f ? pad_y : 0;

    const uint8_t tr = cocos_text_color8(r->tint_r), tg = cocos_text_color8(r->tint_g), tb = cocos_text_color8(r->tint_b);
    const uint8_t sr = cocos_text_color8(r->stroke_r), sg = cocos_text_color8(r->stroke_g), sb = cocos_text_color8(r->stroke_b);
    const uint8_t ta = cocos_text_color8(r->tint_a), sa = cocos_text_color8(r->stroke_a);
    const uint8_t shadow_alpha = cocos_text_color8(r->shadow_opacity);
    const int shadow_x = (int)lroundf(r->shadow_dx);
    const int shadow_y = (int)lroundf(r->shadow_dy);
    int stroke_radius = r->stroke ? (int)ceilf(fabsf(r->stroke_size) * 0.5f) : 0;
    if (stroke_radius > 8) stroke_radius = 8;

    for (size_t i = 0; i < line_count; ++i) {
        const int x = cocos_text_line_start_x(&lines[i], max_content_width, horizontal) + render_dx;
        const int y = baseline + (int)i * line_step + render_dy;
        if (r->shadow) {
            /* Android Paint applies blur; a direct shadow offset keeps the core
             * compatibility behavior without turning text creation into a hot
             * convolution path. */
            cocos_text_draw_ft_span(face, out, r->text, lines[i].begin, lines[i].end,
                         x, y, 125u, 125u, 125u, shadow_alpha, shadow_x, shadow_y);
        }
        if (r->stroke && stroke_radius > 0) {
            for (int oy = -stroke_radius; oy <= stroke_radius; ++oy) {
                for (int ox = -stroke_radius; ox <= stroke_radius; ++ox) {
                    if (ox == 0 && oy == 0) continue;
                    if (ox * ox + oy * oy > stroke_radius * stroke_radius + stroke_radius) continue;
                    cocos_text_draw_ft_span(face, out, r->text, lines[i].begin, lines[i].end,
                                 x, y, sr, sg, sb, sa, ox, oy);
                }
            }
        }
        cocos_text_draw_ft_span(face, out, r->text, lines[i].begin, lines[i].end,
                     x, y, tr, tg, tb, ta, 0, 0);
    }

    ETD2X_LOGCI("COCOS-TEXT",
                "bitmap text bytes=%zu font=%s size=%d fitted=%d shrink=%u canvas=%dx%d lines=%zu source=%s shadow=%u blur=%.2f stroke=%u",
                strlen(r->text), r->font_name && r->font_name[0] ? r->font_name : "<default>",
                r->font_size, fitted_size, r->shrink ? 1u : 0u, out->width, out->height, out->line_count, out->font_source,
                r->shadow ? 1u : 0u, (double)r->shadow_blur, r->stroke ? 1u : 0u);
    cocos_text_font_face_close(&owner);
    return ETD2X_OK;
}

#else

/* Host-only deterministic fallback. It is deliberately not a visual substitute
 * for FreeType; it validates the generic Cocos bridge, bounds, layout and pixel
 * transport in retained regressions. */
static bool cocos_text_measure_shrink_host(void *ctx, const Etd2xCocosTextRequest *r, int paint_size,
                                int candidate, int *width, int *height) {
    (void)ctx;
    const int cell_h = paint_size > 2 ? paint_size : 3;
    const int cell_w = cell_h > 3 ? cell_h * 3 / 5 : 2;
    size_t count = 1, columns = 0, widest = 0;
    const size_t limit = r->shrink_wrap ? (size_t)(r->width / cell_w > 0 ? r->width / cell_w : 1) : SIZE_MAX;
    for (const unsigned char *p = (const unsigned char *)r->text; *p; ++p) {
        if (*p == '\n') { if (columns > widest) widest = columns; columns = 0; count++; }
        else if ((*p & 0xc0u) != 0x80u && *p != '\r') {
            if (columns == limit) { if (columns > widest) widest = columns; columns = 0; count++; }
            columns++;
        }
    }
    if (columns > widest) widest = columns;
    *width = r->shrink_wrap ? r->width : (int)widest * cell_w;
    *height = (int)count * (r->shrink_wrap ? cell_h : (candidate > 2 ? candidate : 3));
    return true;
}
static Etd2xResult cocos_text_rasterize_host(const Etd2xCocosTextRequest *r, Etd2xCocosTextBitmap *out) {
    const int fitted_size = cocos_text_shrink_font_size(r, cocos_text_measure_shrink_host, NULL);
    const int cell_h = fitted_size > 2 ? fitted_size : 3;
    const int cell_w = cell_h > 3 ? (cell_h * 3) / 5 : 2;
    const int line_step = (int)fmaxf(1.0f, cell_h + r->line_spacing);
    size_t lines = 1, columns = 0, max_columns = 0;
    for (const unsigned char *p = (const unsigned char *)r->text; *p; ++p) {
        if (*p == '\n') { if (columns > max_columns) max_columns = columns; columns = 0; lines++; }
        else if ((*p & 0xc0u) != 0x80u) columns++;
    }
    if (columns > max_columns) max_columns = columns;
    int width = r->width > 0 ? r->width : (int)(max_columns ? max_columns : 1u) * cell_w;
    int height = r->height > 0 ? r->height : cell_h + (int)(lines - 1u) * line_step;
    if (r->shadow) { width += (int)fabsf(r->shadow_dx); height += (int)fabsf(r->shadow_dy); }
    if (width <= 0 || height <= 0 || width > COCOS_TEXT_MAX_DIM || height > COCOS_TEXT_MAX_DIM)
        return ETD2X_ERR_APK_UNSUPPORTED;
    const size_t bytes = (size_t)width * (size_t)height * 4u;
    uint8_t *pixels = (uint8_t *)calloc(1, bytes);
    if (!pixels) return ETD2X_ERR_NO_MEMORY;
    const uint8_t tr = cocos_text_color8(r->tint_r), tg = cocos_text_color8(r->tint_g), tb = cocos_text_color8(r->tint_b);
    const uint8_t ta = cocos_text_color8(r->tint_a);
    int x = 0, y = 0;
    for (const unsigned char *p = (const unsigned char *)r->text; *p && y < height; ++p) {
        if (*p == '\n') { x = 0; y += line_step; continue; }
        if ((*p & 0xc0u) == 0x80u) continue;
        if (*p != ' ' && *p != '\t') {
            for (int yy = 1; yy < cell_h - 1 && y + yy < height; ++yy)
                for (int xx = 1; xx < cell_w - 1 && x + xx < width; ++xx)
                    cocos_text_blend_pixel_rgba(pixels + ((size_t)(y + yy) * (size_t)width + (size_t)(x + xx)) * 4u,
                                     tr, tg, tb, ta, 220u);
        }
        x += cell_w;
        if (!r->no_wrap && r->width > 0 && x + cell_w > r->width) { x = 0; y += line_step; }
    }
    out->width = width; out->height = height; out->pixels = pixels; out->byte_count = bytes;
    out->line_count = lines;
    out->font_size = fitted_size;
    snprintf(out->font_source, sizeof(out->font_source), "host-fixture");
    return ETD2X_OK;
}

#endif

Etd2xResult etd2x_cocos_text_rasterize(const Etd2xCocosTextRequest *request,
                                       Etd2xCocosTextBitmap *out_bitmap) {
    if (!out_bitmap) return ETD2X_ERR_ARGUMENT;
    cocos_text_bitmap_reset(out_bitmap);
    if (!cocos_text_text_request_valid(request)) return ETD2X_ERR_ARGUMENT;
#ifdef __SWITCH__
    pthread_mutex_lock(&cocos_text_g_ft_lock);
    const Etd2xResult rc = cocos_text_rasterize_switch(request, out_bitmap);
    pthread_mutex_unlock(&cocos_text_g_ft_lock);
    return rc;
#else
    return cocos_text_rasterize_host(request, out_bitmap);
#endif
}

Etd2xResult etd2x_cocos_text_measure(const char *text, float font_size, float *out_width) {
    if (!text || !out_width || !(font_size > 0.0f) || font_size > (float)COCOS_TEXT_MAX_DIM) return ETD2X_ERR_ARGUMENT;
    *out_width = 0.0f;
    if (!text[0]) return ETD2X_OK;
#ifdef __SWITCH__
    pthread_mutex_lock(&cocos_text_g_ft_lock);
    const Etd2xResult rc = cocos_text_measure_switch(text, font_size, out_width);
    pthread_mutex_unlock(&cocos_text_g_ft_lock);
    return rc;
#else
    /* Host fixture metrics match rasterize_host: 3/5 em per code point. */
    size_t points = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) if ((*p & 0xc0u) != 0x80u) ++points;
    *out_width = (float)points * font_size * 0.6f;
    return ETD2X_OK;
#endif
}

Etd2xResult etd2x_cocos_text_has_glyphs(const char *text, bool *out_has) {
    if (!text || !out_has) return ETD2X_ERR_ARGUMENT;
    *out_has = false;
    if (!text[0]) return ETD2X_OK;
#ifdef __SWITCH__
    pthread_mutex_lock(&cocos_text_g_ft_lock);
    const Etd2xResult rc = cocos_text_has_glyphs_switch(text, out_has);
    pthread_mutex_unlock(&cocos_text_g_ft_lock);
    return rc;
#else
    *out_has = true;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) if (*p < 0x20u) *out_has = false;
    return ETD2X_OK;
#endif
}

#pragma pop_macro("COCOS_TEXT_MAX_LINES")
#pragma pop_macro("COCOS_TEXT_MAX_DIM")
