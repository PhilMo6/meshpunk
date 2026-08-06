#ifndef POSTFX_HPP
#define POSTFX_HPP

#include <cstdint>
#include "JetConfig.hpp"

namespace Renderer {

/// @brief Collection of in-place post-processing effects on an RGB565 framebuffer.
///
/// Each effect compiles to a no-op when its corresponding POSTFX_* config
/// flag is zero. Working buffers are allocated once at construction and
/// reused every frame.
class PostFX {
public:
    /// @brief Construct a PostFX instance sized for a specific framebuffer.
    /// @param screenWidth Framebuffer width in pixels.
    /// @param screenHeight Framebuffer height in pixels.
    PostFX(int screenWidth, int screenHeight);
    ~PostFX();

    /// @brief Apply a 5-tap luminance-driven anti-aliasing pass in place.
    /// @param framebuffer RGB565 framebuffer of size screenWidth*screenHeight.
    void applyFXAA(uint16_t* framebuffer);

    /// @brief Apply a separable box-blur bloom pass in place.
    /// @param framebuffer RGB565 framebuffer of size screenWidth*screenHeight.
    void applyBloom(uint16_t* framebuffer);

    /// @brief Apply a CRT scanline darkening effect in place.
    /// @param framebuffer RGB565 framebuffer of size screenWidth*screenHeight.
    void applyCRT(uint16_t* framebuffer);

    /// @brief Blend the current frame with the previous frame to produce motion blur.
    /// @param framebuffer RGB565 framebuffer of size screenWidth*screenHeight.
    void applyMotionBlur(uint16_t* framebuffer);

    /// @brief Apply a chromatic aberration channel-shift effect in place.
    /// @param framebuffer RGB565 framebuffer of size screenWidth*screenHeight.
    void applyChromatic(uint16_t* framebuffer);

    /// @brief Quantize the framebuffer to PIXELATE_SIZE x PIXELATE_SIZE blocks in place.
    /// @param framebuffer RGB565 framebuffer of size screenWidth*screenHeight.
    void applyPixelate(uint16_t* framebuffer);

private:
    int screenWidth;
    int screenHeight;
    #if POSTFX_BLOOM
    uint16_t* bloomBuffer;
    uint16_t* bloomTemp;
    #endif
    #if POSTFX_MOTION_BLUR
    uint16_t* previousFrame;
    #endif
    #if POSTFX_ANTIALIASING
    // Cached unpacked channels + luminance for the previous (top) row.
    // Layout: 4 * screenWidth int16s, indexed [x*4 + {0=r,1=g,2=b,3=lum}].
    int16_t* fxaaTopRow;
    #endif
};

} // namespace Renderer

#endif // POSTFX_HPP
