#include "Texture.hpp"
#include <stdlib.h>

namespace Renderer
{

  Texture::Texture(int w, int h, uint16_t *data, bool hasAlpha, uint16_t alphaColor, bool screenSpace, TextureAddressMode addressMode, uint16_t *palette)
      : width(w), height(h), data(data),
        hasAlpha(hasAlpha), alphaColor(alphaColor), screenSpace(screenSpace),
        addressMode(addressMode), palette(palette)
  {
    // Level 0 always aliases `data`, so an un-mipped texture still resolves
    // through the same lookup with no special case in the sampler.
    mipCount   = 1;
    mipData[0] = data;
    mipW[0]    = w;
    mipH[0]    = h;
  }

  // MESHPUNK: box-filter the chain. See Texture::buildMips in the header.
  void Texture::buildMips()
  {
    // Paletted data is indices — averaging two indices is meaningless. Non
    // power-of-two dimensions would need a resampler rather than a 2x2 fold.
    if (palette || !data) return;
    if (width < 2 || height < 2) return;
    if ((width & (width - 1)) || (height & (height - 1))) return;

    mipCount   = 1;
    mipData[0] = data;
    mipW[0]    = width;
    mipH[0]    = height;

    int w = width, h = height;
    const uint16_t* src = data;
    for (int lvl = 1; lvl < MAX_MIPS && w >= 2 && h >= 2; ++lvl) {
      const int nw = w >> 1, nh = h >> 1;
      uint16_t* dst = (uint16_t*)malloc((size_t)nw * (size_t)nh * 2u);
      if (!dst) break;                      // keep whatever chain we have
      for (int y = 0; y < nh; ++y) {
        const uint16_t* r0 = src + (size_t)(y * 2)     * w;
        const uint16_t* r1 = src + (size_t)(y * 2 + 1) * w;
        for (int x = 0; x < nw; ++x) {
          const uint16_t a = r0[x*2], b = r0[x*2+1];
          const uint16_t c = r1[x*2], d = r1[x*2+1];
          // Average per RGB565 channel. Summing the packed words would carry
          // between fields and mix red into green.
          const int r = (((a>>11)&0x1F) + ((b>>11)&0x1F) + ((c>>11)&0x1F) + ((d>>11)&0x1F)) >> 2;
          const int g = (((a>>5)&0x3F)  + ((b>>5)&0x3F)  + ((c>>5)&0x3F)  + ((d>>5)&0x3F))  >> 2;
          const int bl= ((a&0x1F)       + (b&0x1F)       + (c&0x1F)       + (d&0x1F))       >> 2;
          dst[y*nw + x] = (uint16_t)((r<<11) | (g<<5) | bl);
        }
      }
      mipData[lvl] = dst;
      mipW[lvl]    = nw;
      mipH[lvl]    = nh;
      mipCount     = lvl + 1;
      src = dst; w = nw; h = nh;
    }
  }

uint16_t Texture::getPixel(int u, int v)
{
    return getPixel(u, v, 0);
}

uint16_t Texture::getPixel(int u, int v, int level)
{
    if (level < 0) level = 0;
    if (level >= mipCount) level = mipCount - 1;

    switch (addressMode)
    {
    case WRAP:
        // MESHPUNK: mask, not modulo.
        //
        // This ran `((u % S) + S) % S` on each axis — FOUR signed integer
        // divisions per textured pixel. Xtensa LX7 has no single-cycle divide,
        // and the compiler cannot reduce a SIGNED % to a mask because the sign
        // of the result is defined to follow the dividend.
        //
        // FIXED_POINT_SCALE is 1024, a power of two, so on two's complement
        // `u & (S - 1)` produces exactly the same non-negative result for both
        // signs — the identity the original expression was spelling out the
        // long way. static_assert keeps that true if the scale ever changes.
        static_assert((FIXED_POINT_SCALE & (FIXED_POINT_SCALE - 1)) == 0,
                      "WRAP masking requires a power-of-two FIXED_POINT_SCALE");
        u &= (FIXED_POINT_SCALE - 1);
        v &= (FIXED_POINT_SCALE - 1);
        break;
    case CLAMP:
        // Clamp texture coordinates
        u = std::min(std::max(u, 0), FIXED_POINT_SCALE - 1);
        v = std::min(std::max(v, 0), FIXED_POINT_SCALE - 1);
        break;
    case ZERO:
        // Return a default color if out of bounds
        if (u < 0 || u >= FIXED_POINT_SCALE || v < 0 || v >= FIXED_POINT_SCALE)
        {
            return 0; // Or use a predefined default color
        }
        break;
    }

    #if BILINEAR_FILTER
    // Compute scaled texture coordinates
    uint32_t scaledU = u * (width - 1);
    uint32_t scaledV = v * (height - 1);

    int x0 = scaledU / FIXED_POINT_SCALE;
    int y0 = scaledV / FIXED_POINT_SCALE;

    int x1 = x0 + 1;
    int y1 = y0 + 1;

    // Clamp coordinates to texture dimensions
    if (x1 >= width) x1 = width - 1;
    if (y1 >= height) y1 = height - 1;

    uint32_t u_ratio = scaledU % FIXED_POINT_SCALE;
    uint32_t v_ratio = scaledV % FIXED_POINT_SCALE;
    uint32_t u_opposite = FIXED_POINT_SCALE - u_ratio;
    uint32_t v_opposite = FIXED_POINT_SCALE - v_ratio;

    // Retrieve colors at the four surrounding pixels
    uint16_t c00 = data[y0 * width + x0];
    uint16_t c10 = data[y0 * width + x1];
    uint16_t c01 = data[y1 * width + x0];
    uint16_t c11 = data[y1 * width + x1];

    // Decompose colors into RGB components (5 bits Red, 6 bits Green, 5 bits Blue)
    uint8_t r00 = (c00 >> 11) & 0x1F;
    uint8_t g00 = (c00 >> 5) & 0x3F;
    uint8_t b00 = c00 & 0x1F;

    uint8_t r10 = (c10 >> 11) & 0x1F;
    uint8_t g10 = (c10 >> 5) & 0x3F;
    uint8_t b10 = c10 & 0x1F;

    uint8_t r01 = (c01 >> 11) & 0x1F;
    uint8_t g01 = (c01 >> 5) & 0x3F;
    uint8_t b01 = c01 & 0x1F;

    uint8_t r11 = (c11 >> 11) & 0x1F;
    uint8_t g11 = (c11 >> 5) & 0x3F;
    uint8_t b11 = c11 & 0x1F;

    // Perform bilinear interpolation
    uint32_t r = (r00 * u_opposite * v_opposite +
            r10 * u_ratio * v_opposite +
            r01 * u_opposite * v_ratio +
            r11 * u_ratio * v_ratio) / (FIXED_POINT_SCALE * FIXED_POINT_SCALE);
    uint32_t g = (g00 * u_opposite * v_opposite +
            g10 * u_ratio * v_opposite +
            g01 * u_opposite * v_ratio +
            g11 * u_ratio * v_ratio) / (FIXED_POINT_SCALE * FIXED_POINT_SCALE);
    uint32_t b = (b00 * u_opposite * v_opposite +
            b10 * u_ratio * v_opposite +
            b01 * u_opposite * v_ratio +
            b11 * u_ratio * v_ratio) / (FIXED_POINT_SCALE * FIXED_POINT_SCALE);

    // Recombine RGB components into a 16-bit color
    uint16_t color = ((r & 0x1F) << 11) | ((g & 0x3F) << 5) | (b & 0x1F);
    #else
    // Scale down the fixed-point UV coordinates to the texture dimensions
    // MESHPUNK: sample the SELECTED MIP, and unsigned so this is a shift
    // rather than a signed divide. Every address mode above leaves u and v
    // inside [0, FIXED_POINT_SCALE-1] — WRAP masks, CLAMP clamps, ZERO returns
    // early — but the compiler cannot prove that from an `int`, so it was
    // emitting the sign-correcting sequence for a signed division on every
    // textured pixel.
    const int       width  = mipW[level];
    const int       height = mipH[level];
    const uint16_t* data   = mipData[level];
    u = (int)(((uint32_t)u * (uint32_t)width)  / (uint32_t)FIXED_POINT_SCALE);
    v = (int)(((uint32_t)v * (uint32_t)height) / (uint32_t)FIXED_POINT_SCALE);

    // Retrieve the color from the texture data
    uint16_t color = 0;
    if (palette) {
        // If a palette is provided, use it to get the color.
        // paletteSize>0 means animated: offset the index so the palette
        // appears to scroll without touching the texture data (Sonic trick).
        uint8_t colorIndex = ((uint8_t*)data)[v * width + u];
        if (paletteSize > 0) {
            colorIndex = (colorIndex + paletteOffset) % paletteSize;
        }
        color = palette[colorIndex];
    }
    else {
        color = data[v * width + u];
    }
    #endif

    return color;
}

} // namespace Renderer
