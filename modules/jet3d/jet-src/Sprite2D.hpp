#ifndef JET_SPRITE2D_HPP
#define JET_SPRITE2D_HPP

/// @file Sprite2D.hpp
/// @brief Screen-space 2D overlay: solid rectangle or unscaled upright sprite.
///
/// A Sprite2D is owned by the caller and registered with Scene::addSprite().
/// After registration it is drawn automatically at the end of every
/// Scene::render() call, after all 3D geometry and PostFX, so it always
/// composites on top of the world.
///
/// Rendering rules:
///   - `material->diffuseMap != nullptr`  →  blit the texture at (x, y);
///     pixels equal to `diffuseMap->alphaColor` are skipped (colour-key)
///     when `diffuseMap->hasAlpha` is true.
///   - `material->diffuseMap == nullptr`  →  fill a `width × height` solid
///     rectangle with `material->color`.
///
/// The effective alpha is `material->alpha * alpha / 255`.  At 255 the blit
/// is a straight copy (fastest path).  Any other value blends against the
/// existing framebuffer content with a fast 5-bit lerp.
///
/// `zOrder` controls draw order within the sprite pass: lower values are
/// drawn first (further back).  There is no framebuffer depth test — sprites
/// always paint over everything rendered before them.

#include <cstdint>
#include "Material.hpp"
#include "Object.hpp"   // for BlendMode enum

namespace Renderer {

/// @brief Screen-space 2D overlay registered with a Scene.
struct Sprite2D {
    int         x        = 0;       ///< Left edge in pixels (screen space, 0 = left).
    int         y        = 0;       ///< Top edge in pixels (screen space, 0 = top).
    int         width    = 0;       ///< Width in pixels.  Ignored when diffuseMap is set (texture dimensions are used).
    int         height   = 0;       ///< Height in pixels.  Ignored when diffuseMap is set.
    Material*   material = nullptr; ///< Surface description: color + optional diffuseMap.  Required.
    uint8_t     alpha    = 255;     ///< Per-sprite alpha multiplied with material->alpha.
    int         zOrder   = 0;       ///< Draw order within the 2D pass; lower = drawn first (behind).
    bool        enabled  = true;    ///< When false the sprite is skipped entirely.
    /// @brief Blend equation used when writing this sprite onto the framebuffer.
    ///
    /// BLEND_REPLACE — normal alpha-lerp composite (default).
    /// BLEND_ADD     — saturating additive: dst = clamp(dst + src * alpha).
    ///                 Ideal for glows, coronas and lens-flare elements drawn as
    ///                 grayscale soft-edged textures; produces bright halos that
    ///                 never over-darken the scene.
    BlendMode   blendMode = BlendMode::BLEND_REPLACE;
    /// @brief Integer upscale factor applied at blit time (1 = native size).
    ///
    /// When > 1, the sprite is rendered at texture_size × scale pixels using
    /// bilinear interpolation. Solid-colour (no texture) sprites scale their
    /// width/height instead. Useful for soft glow textures that would look too
    /// small at 1:1 — at 2× or 3× the smooth falloff still reads well.
    int         scale     = 1;
};

} // namespace Renderer

#endif // JET_SPRITE2D_HPP
