// FT812 graphics engine: renders a display list into the framebuffer.
//
// Geometry follows the EVE model: LINE_WIDTH and POINT_SIZE are radii in 1/16 px, RECTS
// get corners rounded by the line width, and edges are antialiased. Vertex coordinates name
// pixel centres, so a 1 px wide rectangle from (x0, y0) to (x1, y1) covers both corner
// pixels, which is how the UI code uses it (it passes inclusive end coordinates).
//
// Not modelled: stencil, alpha test, tag buffer, bilinear filtering, edge strips.

#include "EveChip.h"

#include "FT81x.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace eve
{
namespace
{
struct Context
{
    uint8_t r = 255, g = 255, b = 255, a = 255;
    uint8_t clearR = 0, clearG = 0, clearB = 0, clearA = 0;
    int lineWidth = 16; // 1/16 px, radius
    int pointSize = 16; // 1/16 px, radius
    int scissorX = 0, scissorY = 0, scissorW = 2048, scissorH = 2048;
    int blendSrc = SRC_ALPHA, blendDst = ONE_MINUS_SRC_ALPHA;
    int handle = 0, cell = 0;
    int vertexFormat = 4;
    int translateX = 0, translateY = 0; // 1/16 px
    int32_t transform[6] = { 256, 0, 0, 0, 256, 0 }; // A..F, 8.8 fixed (C, F in 1/256 px)
    bool maskR = true, maskG = true, maskB = true, maskA = true;
    uint32_t paletteSource = 0;
};

int32_t signExtend(uint32_t value, int bits)
{
    const uint32_t m = 1u << (bits - 1);
    value &= (1u << bits) - 1;
    return static_cast<int32_t>((value ^ m) - m);
}

uint8_t expand5(uint32_t v) { return static_cast<uint8_t>((v << 3) | (v >> 2)); }
uint8_t expand6(uint32_t v) { return static_cast<uint8_t>((v << 2) | (v >> 4)); }
uint8_t expand4(uint32_t v) { return static_cast<uint8_t>(v * 17); }

struct Rgba
{
    uint8_t r, g, b, a;
};

class Raster
{
  public:
    Raster(std::vector<uint32_t>& fb, const Context& ctx) : fb(fb), ctx(ctx)
    {
        clipX0 = std::max(0, ctx.scissorX);
        clipY0 = std::max(0, ctx.scissorY);
        clipX1 = std::min(kScreenWidth, ctx.scissorX + ctx.scissorW);
        clipY1 = std::min(kScreenHeight, ctx.scissorY + ctx.scissorH);
    }

    bool empty() const { return clipX0 >= clipX1 || clipY0 >= clipY1; }

    // Clamp a float bounding box to the clip rectangle; false if nothing is left.
    bool bounds(float x0, float y0, float x1, float y1, int& ix0, int& iy0, int& ix1, int& iy1) const
    {
        ix0 = std::max(clipX0, static_cast<int>(std::floor(x0)));
        iy0 = std::max(clipY0, static_cast<int>(std::floor(y0)));
        ix1 = std::min(clipX1, static_cast<int>(std::ceil(x1)) + 1);
        iy1 = std::min(clipY1, static_cast<int>(std::ceil(y1)) + 1);
        return ix0 < ix1 && iy0 < iy1;
    }

    void blend(int x, int y, Rgba src)
    {
        if (src.a == 0 && ctx.blendSrc == SRC_ALPHA && ctx.blendDst == ONE_MINUS_SRC_ALPHA)
        {
            return;
        }
        uint32_t& pixel = fb[static_cast<size_t>(y) * kScreenWidth + x];
        const int dr = (pixel >> 16) & 0xFF, dg = (pixel >> 8) & 0xFF, db = pixel & 0xFF;
        const int da = (pixel >> 24) & 0xFF;

        auto factor = [&](int f) {
            switch (f)
            {
                case ZERO: return 0;
                case ONE: return 255;
                case SRC_ALPHA: return static_cast<int>(src.a);
                case DST_ALPHA: return da;
                case ONE_MINUS_SRC_ALPHA: return 255 - src.a;
                case ONE_MINUS_DST_ALPHA: return 255 - da;
                default: return 255;
            }
        };
        const int sf = factor(ctx.blendSrc);
        const int df = factor(ctx.blendDst);
        auto mix = [&](int s, int d) { return std::min(255, (s * sf + d * df + 127) / 255); };

        const int nr = ctx.maskR ? mix(src.r, dr) : dr;
        const int ng = ctx.maskG ? mix(src.g, dg) : dg;
        const int nb = ctx.maskB ? mix(src.b, db) : db;
        const int na = ctx.maskA ? mix(src.a, da) : da;
        pixel = (static_cast<uint32_t>(na) << 24) | (nr << 16) | (ng << 8) | nb;
    }

    void plot(int x, int y, float coverage)
    {
        if (coverage <= 0.0f)
        {
            return;
        }
        const int alpha = static_cast<int>(ctx.a * std::min(coverage, 1.0f) + 0.5f);
        blend(x, y, { ctx.r, ctx.g, ctx.b, static_cast<uint8_t>(alpha) });
    }

    std::vector<uint32_t>& fb;
    const Context& ctx;
    int clipX0, clipY0, clipX1, clipY1;
};

float coverageFor(float radius, float distance)
{
    return std::clamp(radius + 0.5f - distance, 0.0f, 1.0f);
}

// Radius of the outline drawn around lines and rectangles. The core geometry runs through
// pixel centres, so half a pixel less than LINE_WIDTH keeps a 1 px line exactly 1 px wide.
float outlineRadius(int lineWidth) { return std::max(lineWidth / 16.0f - 0.5f, 0.5f); }

float segmentDistance(float px, float py, float ax, float ay, float bx, float by)
{
    const float dx = bx - ax, dy = by - ay;
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 0.0f ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float ex = px - (ax + t * dx), ey = py - (ay + t * dy);
    return std::sqrt(ex * ex + ey * ey);
}

void drawPoint(Raster& raster, float cx, float cy, float radius)
{
    int x0, y0, x1, y1;
    if (!raster.bounds(cx - radius - 1, cy - radius - 1, cx + radius + 1, cy + radius + 1, x0, y0, x1, y1))
    {
        return;
    }
    for (int y = y0; y < y1; y++)
    {
        for (int x = x0; x < x1; x++)
        {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            raster.plot(x, y, coverageFor(radius, std::sqrt(dx * dx + dy * dy)));
        }
    }
}

void drawRect(Raster& raster, float ax, float ay, float bx, float by, float radius)
{
    const float minX = std::min(ax, bx), maxX = std::max(ax, bx);
    const float minY = std::min(ay, by), maxY = std::max(ay, by);
    int x0, y0, x1, y1;
    if (!raster.bounds(minX - radius - 1, minY - radius - 1, maxX + radius + 1, maxY + radius + 1, x0, y0, x1, y1))
    {
        return;
    }
    for (int y = y0; y < y1; y++)
    {
        const float py = y + 0.5f;
        const float dy = std::max({ minY - py, 0.0f, py - maxY });
        for (int x = x0; x < x1; x++)
        {
            const float px = x + 0.5f;
            const float dx = std::max({ minX - px, 0.0f, px - maxX });
            raster.plot(x, y, coverageFor(radius, std::sqrt(dx * dx + dy * dy)));
        }
    }
}

// Lines and strips: union of capsules, blended once per pixel so joints do not double up.
void drawPolyline(Raster& raster, const std::vector<float>& points, bool strip, float radius)
{
    const size_t count = points.size() / 2;
    if (count < 2)
    {
        return;
    }
    float minX = points[0], maxX = points[0], minY = points[1], maxY = points[1];
    for (size_t i = 1; i < count; i++)
    {
        minX = std::min(minX, points[2 * i]);
        maxX = std::max(maxX, points[2 * i]);
        minY = std::min(minY, points[2 * i + 1]);
        maxY = std::max(maxY, points[2 * i + 1]);
    }
    int x0, y0, x1, y1;
    if (!raster.bounds(minX - radius - 1, minY - radius - 1, maxX + radius + 1, maxY + radius + 1, x0, y0, x1, y1))
    {
        return;
    }
    const size_t step = strip ? 1 : 2;
    for (int y = y0; y < y1; y++)
    {
        for (int x = x0; x < x1; x++)
        {
            float best = 1e9f;
            for (size_t i = 0; i + 1 < count; i += step)
            {
                best = std::min(best, segmentDistance(x + 0.5f, y + 0.5f, points[2 * i], points[2 * i + 1],
                                                      points[2 * i + 2], points[2 * i + 3]));
            }
            raster.plot(x, y, coverageFor(radius, best));
        }
    }
}

void drawGradient(Raster& raster, const uint32_t* params)
{
    const float x0 = static_cast<int16_t>(params[0] & 0xFFFF), y0 = static_cast<int16_t>(params[0] >> 16);
    const float x1 = static_cast<int16_t>(params[2] & 0xFFFF), y1 = static_cast<int16_t>(params[2] >> 16);
    const uint32_t c0 = params[1], c1 = params[3];
    const float dx = x1 - x0, dy = y1 - y0;
    const float len2 = dx * dx + dy * dy;

    for (int y = raster.clipY0; y < raster.clipY1; y++)
    {
        for (int x = raster.clipX0; x < raster.clipX1; x++)
        {
            float t = len2 > 0.0f ? ((x + 0.5f - x0) * dx + (y + 0.5f - y0) * dy) / len2 : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            auto lerp = [&](int shift) {
                const float a = (c0 >> shift) & 0xFF, b = (c1 >> shift) & 0xFF;
                return static_cast<uint8_t>(a + (b - a) * t + 0.5f);
            };
            raster.blend(x, y, { lerp(16), lerp(8), lerp(0), raster.ctx.a });
        }
    }
}
} // namespace

void Chip::renderDisplayList()
{
    swaps++;
    if (traceEnabled())
    {
        const uint32_t* w = reinterpret_cast<const uint32_t*>(ramDL.data());
        std::fprintf(stderr, "swap %llu:", (unsigned long long)swaps);
        for (uint32_t i = 0; i < kRamDLSize / 4; i++)
        {
            std::fprintf(stderr, " %08x", w[i]);
            if (w[i] == 0) break;
        }
        std::fprintf(stderr, "\n");
    }
    Context ctx;
    Context stack[4];
    int stackDepth = 0;
    uint32_t callStack[4];
    int callDepth = 0;

    int primitive = -1;
    std::vector<float> polyline; // pending LINES / LINE_STRIP vertices
    float rectX = 0, rectY = 0;
    bool haveRectCorner = false;

    auto flushPolyline = [&]() {
        if (polyline.size() >= 4)
        {
            Raster raster(framebuffer, ctx);
            drawPolyline(raster, polyline, primitive == LINE_STRIP, outlineRadius(ctx.lineWidth));
        }
        polyline.clear();
    };

    auto drawBitmap = [&](int x, int y, int handleIndex, int cellIndex) {
        const BitmapHandle& h = handles[handleIndex & 31];
        const int width = h.width ? static_cast<int>(h.width) : 2048;
        const int height = h.height ? static_cast<int>(h.height) : 2048;
        Raster raster(framebuffer, ctx);
        int x0, y0, x1, y1;
        if (!raster.bounds(static_cast<float>(x), static_cast<float>(y), static_cast<float>(x + width - 1),
                           static_cast<float>(y + height - 1), x0, y0, x1, y1))
        {
            return;
        }
        x1 = std::min(x1, x + width);
        y1 = std::min(y1, y + height);

        uint32_t bpp;
        switch (h.format)
        {
            case L1: bpp = 1; break;
            case L2: bpp = 2; break;
            case L4: bpp = 4; break;
            case L8:
            case RGB332:
            case ARGB2:
            case PALETTED4444:
            case PALETTED565:
            case PALETTED8: bpp = 8; break;
            default: bpp = 16; break;
        }
        const int texWidth = h.lineStride ? static_cast<int>(h.lineStride * 8 / bpp) : 0;
        const int texHeight = static_cast<int>(h.layoutHeight ? h.layoutHeight : 2048);
        if (texWidth == 0)
        {
            return;
        }
        const uint32_t base = h.source + static_cast<uint32_t>(cellIndex) * h.lineStride * texHeight;
        const int32_t* m = ctx.transform;

        for (int py = y0; py < y1; py++)
        {
            for (int px = x0; px < x1; px++)
            {
                // Pixel centre relative to the vertex, through the inverse bitmap transform.
                const float lx = px - x + 0.5f, ly = py - y + 0.5f;
                int tx = static_cast<int>(std::floor((m[0] * lx + m[1] * ly + m[2]) / 256.0f));
                int ty = static_cast<int>(std::floor((m[3] * lx + m[4] * ly + m[5]) / 256.0f));
                if (h.wrapX == REPEAT)
                {
                    tx = ((tx % texWidth) + texWidth) % texWidth;
                }
                if (h.wrapY == REPEAT)
                {
                    ty = ((ty % texHeight) + texHeight) % texHeight;
                }
                if (tx < 0 || ty < 0 || tx >= texWidth || ty >= texHeight)
                {
                    continue;
                }

                const uint32_t row = base + static_cast<uint32_t>(ty) * h.lineStride;
                Rgba texel{ 255, 255, 255, 255 };
                switch (h.format)
                {
                    case L1:
                        texel.a = ((readByte(row + tx / 8) >> (7 - tx % 8)) & 1) ? 255 : 0;
                        break;
                    case L2:
                        texel.a = static_cast<uint8_t>(((readByte(row + tx / 4) >> (6 - 2 * (tx % 4))) & 3) * 85);
                        break;
                    case L4:
                        texel.a = expand4((readByte(row + tx / 2) >> (tx % 2 ? 0 : 4)) & 0xF);
                        break;
                    case L8: texel.a = readByte(row + tx); break;
                    case RGB332:
                    {
                        const uint8_t v = readByte(row + tx);
                        texel = { static_cast<uint8_t>(((v >> 5) & 7) * 255 / 7),
                                  static_cast<uint8_t>(((v >> 2) & 7) * 255 / 7),
                                  static_cast<uint8_t>((v & 3) * 85), 255 };
                        break;
                    }
                    case ARGB2:
                    {
                        const uint8_t v = readByte(row + tx);
                        texel = { static_cast<uint8_t>(((v >> 4) & 3) * 85), static_cast<uint8_t>(((v >> 2) & 3) * 85),
                                  static_cast<uint8_t>((v & 3) * 85), static_cast<uint8_t>(((v >> 6) & 3) * 85) };
                        break;
                    }
                    case RGB565:
                    {
                        const uint32_t v = readByte(row + 2 * tx) | (readByte(row + 2 * tx + 1) << 8);
                        texel = { expand5(v >> 11), expand6((v >> 5) & 63), expand5(v & 31), 255 };
                        break;
                    }
                    case ARGB1555:
                    {
                        const uint32_t v = readByte(row + 2 * tx) | (readByte(row + 2 * tx + 1) << 8);
                        texel = { expand5((v >> 10) & 31), expand5((v >> 5) & 31), expand5(v & 31),
                                  static_cast<uint8_t>((v & 0x8000) ? 255 : 0) };
                        break;
                    }
                    case ARGB4:
                    {
                        const uint32_t v = readByte(row + 2 * tx) | (readByte(row + 2 * tx + 1) << 8);
                        texel = { expand4((v >> 8) & 15), expand4((v >> 4) & 15), expand4(v & 15),
                                  expand4((v >> 12) & 15) };
                        break;
                    }
                    case PALETTED4444:
                    {
                        const uint32_t i = readByte(row + tx);
                        const uint32_t v = readByte(ctx.paletteSource + 2 * i) |
                                           (readByte(ctx.paletteSource + 2 * i + 1) << 8);
                        texel = { expand4((v >> 8) & 15), expand4((v >> 4) & 15), expand4(v & 15),
                                  expand4((v >> 12) & 15) };
                        break;
                    }
                    case PALETTED565:
                    {
                        const uint32_t i = readByte(row + tx);
                        const uint32_t v = readByte(ctx.paletteSource + 2 * i) |
                                           (readByte(ctx.paletteSource + 2 * i + 1) << 8);
                        texel = { expand5(v >> 11), expand6((v >> 5) & 63), expand5(v & 31), 255 };
                        break;
                    }
                    case PALETTED8:
                    {
                        const uint32_t v = readWord(ctx.paletteSource + 4 * readByte(row + tx));
                        texel = { static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8),
                                  static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 24) };
                        break;
                    }
                    default: continue;
                }

                raster.blend(px, py,
                             { static_cast<uint8_t>(texel.r * ctx.r / 255), static_cast<uint8_t>(texel.g * ctx.g / 255),
                               static_cast<uint8_t>(texel.b * ctx.b / 255), static_cast<uint8_t>(texel.a * ctx.a / 255) });
            }
        }
    };

    auto vertex = [&](float x, float y, bool hasHandle, int handleIndex, int cellIndex, int rawX, int rawY) {
        switch (primitive)
        {
            case BITMAPS:
                drawBitmap(rawX, rawY, hasHandle ? handleIndex : ctx.handle, hasHandle ? cellIndex : ctx.cell);
                break;
            case FTPOINTS:
            {
                Raster raster(framebuffer, ctx);
                drawPoint(raster, x, y, ctx.pointSize / 16.0f);
                break;
            }
            case LINES:
                polyline.push_back(x);
                polyline.push_back(y);
                break;
            case LINE_STRIP:
                polyline.push_back(x);
                polyline.push_back(y);
                break;
            case RECTS:
                if (!haveRectCorner)
                {
                    rectX = x;
                    rectY = y;
                    haveRectCorner = true;
                }
                else
                {
                    Raster raster(framebuffer, ctx);
                    drawRect(raster, rectX, rectY, x, y, outlineRadius(ctx.lineWidth));
                    haveRectCorner = false;
                }
                break;
            default: break;
        }
    };

    const uint32_t* dl = reinterpret_cast<const uint32_t*>(ramDL.data());
    const uint32_t dlWords = kRamDLSize / 4;
    uint32_t pc = 0;
    int guard = 0;

    while (pc < dlWords && guard++ < 1 << 20)
    {
        const uint32_t word = dl[pc++];
        const uint32_t top2 = word >> 30;

        if (top2 == 1) // VERTEX2F
        {
            const float scale = 1.0f / static_cast<float>(1 << ctx.vertexFormat);
            const int32_t vx = signExtend(word >> 15, 15), vy = signExtend(word, 15);
            const float x = vx * scale + ctx.translateX / 16.0f;
            const float y = vy * scale + ctx.translateY / 16.0f;
            vertex(x + 0.5f, y + 0.5f, false, 0, 0, static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)));
            continue;
        }
        if (top2 == 2) // VERTEX2II
        {
            const int x = static_cast<int>((word >> 21) & 511), y = static_cast<int>((word >> 12) & 511);
            const float fx = x + ctx.translateX / 16.0f, fy = y + ctx.translateY / 16.0f;
            vertex(fx + 0.5f, fy + 0.5f, true, static_cast<int>((word >> 7) & 31), static_cast<int>(word & 127),
                   static_cast<int>(std::floor(fx)), static_cast<int>(std::floor(fy)));
            continue;
        }

        const uint32_t op = word >> 24;
        switch (op)
        {
            case 0x00: // DISPLAY
                pc = dlWords;
                break;
            case 0x01: handles[ctx.handle].source = word & 0x3FFFFF; break;
            case 0x02: // CLEAR_COLOR_RGB
                ctx.clearR = (word >> 16) & 0xFF;
                ctx.clearG = (word >> 8) & 0xFF;
                ctx.clearB = word & 0xFF;
                break;
            case 0x03: break; // TAG
            case 0x04: // COLOR_RGB
                ctx.r = (word >> 16) & 0xFF;
                ctx.g = (word >> 8) & 0xFF;
                ctx.b = word & 0xFF;
                break;
            case 0x05: ctx.handle = word & 31; break;
            case 0x06: ctx.cell = word & 127; break;
            case 0x07: // BITMAP_LAYOUT
            {
                BitmapHandle& h = handles[ctx.handle];
                h.format = (word >> 19) & 31;
                h.lineStride = (h.lineStride & ~1023u) | ((word >> 9) & 1023);
                h.layoutHeight = (h.layoutHeight & ~511u) | (word & 511);
                break;
            }
            case 0x08: // BITMAP_SIZE
            {
                BitmapHandle& h = handles[ctx.handle];
                h.filter = (word >> 20) & 1;
                h.wrapX = (word >> 19) & 1;
                h.wrapY = (word >> 18) & 1;
                h.width = (h.width & ~511u) | ((word >> 9) & 511);
                h.height = (h.height & ~511u) | (word & 511);
                break;
            }
            case 0x09: break; // ALPHA_FUNC
            case 0x0A: break; // STENCIL_FUNC
            case 0x0B:
                ctx.blendSrc = (word >> 3) & 7;
                ctx.blendDst = word & 7;
                break;
            case 0x0C: break; // STENCIL_OP
            case 0x0D: ctx.pointSize = word & 8191; break;
            case 0x0E: ctx.lineWidth = word & 4095; break;
            case 0x0F: ctx.clearA = word & 0xFF; break;
            case 0x10: ctx.a = word & 0xFF; break;
            case 0x11: break; // CLEAR_STENCIL
            case 0x12: break; // CLEAR_TAG
            case 0x13: break; // STENCIL_MASK
            case 0x14: break; // TAG_MASK
            case 0x15: ctx.transform[0] = signExtend(word, 17); break;
            case 0x16: ctx.transform[1] = signExtend(word, 17); break;
            case 0x17: ctx.transform[2] = signExtend(word, 24); break;
            case 0x18: ctx.transform[3] = signExtend(word, 17); break;
            case 0x19: ctx.transform[4] = signExtend(word, 17); break;
            case 0x1A: ctx.transform[5] = signExtend(word, 24); break;
            case 0x1B: // SCISSOR_XY
                ctx.scissorX = static_cast<int>((word >> 11) & 2047);
                ctx.scissorY = static_cast<int>(word & 2047);
                break;
            case 0x1C: // SCISSOR_SIZE
                ctx.scissorW = static_cast<int>((word >> 12) & 4095);
                ctx.scissorH = static_cast<int>(word & 4095);
                break;
            case 0x1D: // CALL
                if (callDepth < 4)
                {
                    callStack[callDepth++] = pc;
                    pc = word & 0xFFFF;
                }
                break;
            case 0x1E: pc = word & 0xFFFF; break; // JUMP
            case 0x1F: // BEGIN
                flushPolyline();
                primitive = static_cast<int>(word & 15);
                haveRectCorner = false;
                break;
            case 0x20: // COLOR_MASK
                ctx.maskR = (word >> 3) & 1;
                ctx.maskG = (word >> 2) & 1;
                ctx.maskB = (word >> 1) & 1;
                ctx.maskA = word & 1;
                break;
            case 0x21: // END
                flushPolyline();
                primitive = -1;
                haveRectCorner = false;
                break;
            case 0x22: // SAVE_CONTEXT
                if (stackDepth < 4)
                {
                    stack[stackDepth++] = ctx;
                }
                break;
            case 0x23: // RESTORE_CONTEXT
                if (stackDepth > 0)
                {
                    flushPolyline();
                    ctx = stack[--stackDepth];
                }
                break;
            case 0x24: // RETURN
                if (callDepth > 0)
                {
                    pc = callStack[--callDepth];
                }
                break;
            case 0x25: break; // MACRO
            case 0x26: // CLEAR
                if (word & 4)
                {
                    Raster raster(framebuffer, ctx);
                    const uint32_t clear = (static_cast<uint32_t>(ctx.clearA) << 24) | (ctx.clearR << 16) |
                                           (ctx.clearG << 8) | ctx.clearB;
                    for (int y = raster.clipY0; y < raster.clipY1; y++)
                    {
                        std::fill(&framebuffer[static_cast<size_t>(y) * kScreenWidth + raster.clipX0],
                                  &framebuffer[static_cast<size_t>(y) * kScreenWidth + raster.clipX1], clear);
                    }
                }
                break;
            case 0x27: ctx.vertexFormat = word & 7; break;
            case 0x28: // BITMAP_LAYOUT_H
            {
                BitmapHandle& h = handles[ctx.handle];
                h.lineStride = (h.lineStride & 1023) | (((word >> 2) & 3) << 10);
                h.layoutHeight = (h.layoutHeight & 511) | ((word & 3) << 9);
                break;
            }
            case 0x29: // BITMAP_SIZE_H
            {
                BitmapHandle& h = handles[ctx.handle];
                h.width = (h.width & 511) | (((word >> 2) & 3) << 9);
                h.height = (h.height & 511) | ((word & 3) << 9);
                break;
            }
            case 0x2A: ctx.paletteSource = word & 0x3FFFFF; break;
            case 0x2B: ctx.translateX = signExtend(word, 17); break;
            case 0x2C: ctx.translateY = signExtend(word, 17); break;
            case 0x2D: break; // NOP

            case kOpGlyph:
            {
                if (pc >= dlWords)
                {
                    break;
                }
                const uint32_t position = dl[pc++];
                drawBitmap(static_cast<int16_t>(position & 0xFFFF), static_cast<int16_t>(position >> 16),
                           static_cast<int>((word >> 8) & 31), static_cast<int>(word & 0xFF));
                break;
            }
            case kOpGradient:
            {
                if (pc + 4 > dlWords)
                {
                    pc = dlWords;
                    break;
                }
                Raster raster(framebuffer, ctx);
                drawGradient(raster, &dl[pc]);
                pc += 4;
                break;
            }
            default: break;
        }
    }
    flushPolyline();
}

void Chip::snapshot(uint32_t format, uint32_t address, int x, int y, int width, int height)
{
    // 0x20 is ARGB8, whose width the chip takes in units of half pixels.
    const bool argb8 = format == 0x20;
    const int pixels = argb8 ? width / 2 : width;
    for (int row = 0; row < height; row++)
    {
        for (int col = 0; col < pixels; col++)
        {
            const int sx = x + col, sy = y + row;
            uint32_t pixel = 0;
            if (sx >= 0 && sy >= 0 && sx < kScreenWidth && sy < kScreenHeight)
            {
                pixel = framebuffer[static_cast<size_t>(sy) * kScreenWidth + sx];
            }
            if (argb8)
            {
                writeWord(address, pixel | 0xFF000000u);
                address += 4;
            }
            else
            {
                const uint32_t r = (pixel >> 19) & 31, g = (pixel >> 10) & 63, b = (pixel >> 3) & 31;
                const uint32_t v = (r << 11) | (g << 5) | b;
                writeByte(address, static_cast<uint8_t>(v));
                writeByte(address + 1, static_cast<uint8_t>(v >> 8));
                address += 2;
            }
        }
    }
}
} // namespace eve
