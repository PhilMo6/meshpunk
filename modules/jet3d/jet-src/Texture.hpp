#ifndef TEXTURE_HPP
#define TEXTURE_HPP

#include <cstdint>
#include <vector>
#include "Shader.hpp"
#include "JetConfig.hpp"

namespace Renderer
{
    /// @brief How out-of-range UVs are resolved.
    enum TextureAddressMode
    {
        WRAP,   ///< UVs wrap (tile).
        CLAMP,  ///< UVs are clamped to the [0,1) range.
        ZERO    ///< Out-of-range UVs return colour 0.
    };

    /// @brief Static 2D image used for diffuse / reflection / screen-space sampling.
    class Texture
    {
    public:
        int width;                          ///< Width in pixels.
        int height;                         ///< Height in pixels.
        uint16_t *data;                     ///< Pixel data; RGB565 unless `palette` is set, in which case it is paletted indices.
        bool hasAlpha;                      ///< When true, pixels equal to `alphaColor` are treated as transparent.
        uint16_t alphaColor;                ///< Colour key sampled as fully transparent.
        bool screenSpace;                   ///< When true, the texture is sampled in screen space rather than UV space.
        TextureAddressMode addressMode;     ///< UV addressing mode.
        uint16_t *palette;                  ///< Optional palette; when non-null `data` is treated as indices.
        int       paletteSize  = 0;         ///< Number of entries in palette (0 = non-animated / full 256).
        int       paletteOffset = 0;        ///< Current animation offset; added to every index before lookup.

        bool reflectionMap = false;         ///< When true, sampled via reflected view direction instead of UV.
        char* name = nullptr;               ///< Optional name for asset lookup.

        /// @name MESHPUNK: mip chain
        /// @brief Progressively half-sized, box-filtered copies, selected per
        /// TRIANGLE by the rasteriser from its UV-per-screen-pixel rate.
        ///
        /// Nearest sampling with no mip chain means a pixel covering N texels
        /// picks exactly one of them, so the choice flips with sub-pixel camera
        /// motion and the surface crawls and beats into moiré. Measured on the
        /// terrain: 1.8 texels/pixel at 60,000 units out, 9.2 at 300,000 — the
        /// "texture warping" that survived every interpolation fix, because
        /// interpolation error was already down at 0.12 texels.
        ///
        /// This is the same idea texlod already implements as a crude two-level
        /// mip (textured or flat) chosen by per-triangle depth; it just fills in
        /// the levels between.
        ///
        /// Level 0 aliases `data` and is never freed here. Costs 1/3 extra
        /// texels for the whole chain.
        /// @{
        static const int MAX_MIPS = 4;
        int       mipCount = 1;                 ///< Levels present, including level 0.
        uint16_t* mipData[MAX_MIPS] = { nullptr, nullptr, nullptr, nullptr };
        int       mipW[MAX_MIPS] = { 0, 0, 0, 0 };
        int       mipH[MAX_MIPS] = { 0, 0, 0, 0 };

        /// @brief Box-filter `data` down to at most MAX_MIPS levels.
        ///        No-op for paletted textures (indices cannot be averaged) or
        ///        when either dimension is not a power of two.
        void buildMips();
        /// @}

        /// @brief Construct a texture.
        /// @param w Width in pixels.
        /// @param h Height in pixels.
        /// @param data Pixel data (caller-owned).
        /// @param hasAlpha Enable colour-key transparency.
        /// @param alphaColor Colour to treat as transparent when `hasAlpha` is true.
        /// @param screenSpace Sample in screen space instead of UV.
        /// @param addressMode UV addressing mode (WRAP/CLAMP/ZERO).
        /// @param palette Optional palette (caller-owned).
        Texture(int w, int h, uint16_t *data, bool hasAlpha = false, uint16_t alphaColor = 0, bool screenSpace = false, TextureAddressMode addressMode = WRAP, uint16_t *palette = nullptr);

        /// @brief Sample the texture at a UV coordinate.
        /// @param u Fixed-point U coordinate.
        /// @param v Fixed-point V coordinate.
        /// @return Sampled RGB565 colour.
        uint16_t getPixel(int u, int v);

        /// @brief Sample a specific mip level. Clamped to the available chain.
        uint16_t getPixel(int u, int v, int level);

        /// @brief Advance the palette animation by `dt` seconds at `fps` palette-steps per second.
        ///        No-op if `paletteSize` is 0. Call once per game frame.
        /// @param dt      Seconds since last call.
        /// @param fps     How many palette entries to advance per second.
        inline void advancePalette(float dt, float fps) {
            if (paletteSize <= 0) return;
            paletteOffset = (paletteOffset + (int)(dt * fps + 0.5f)) % paletteSize;
        }
    };

} // namespace Renderer

#endif // TEXTURE_HPP
