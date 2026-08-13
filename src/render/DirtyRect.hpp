#pragma once
// ---------------------------------------------------------------------------
// render/DirtyRect.hpp
// Dirty-rectangle geometry for partial-frame updates. Pure, header-only, and
// unit-testable (no D3D/DXGI dependency) so the logic can be validated on any
// platform — see RunDirtyRectSelfTest().
// ---------------------------------------------------------------------------
#include <windows.h>

#include <array>
#include <cstdint>

namespace lwe::render {

// Up to two dirty rectangles describing what changed since the previous frame:
// classically the element's old bounds (to erase) and new bounds (to draw).
// When those overlap they collapse to a single bounding box — cheaper for the
// compositor and simpler for the coherent-copy step.
struct DirtyRegion {
    std::array<RECT, 2> rects{};
    UINT count = 0;
};

[[nodiscard]] inline bool RectsIntersect(const RECT& a, const RECT& b) noexcept {
    return a.left < b.right && b.left < a.right && a.top < b.bottom &&
           b.top < a.bottom;
}

[[nodiscard]] inline RECT BoundingBox(const RECT& a, const RECT& b) noexcept {
    RECT r;
    r.left = (a.left < b.left) ? a.left : b.left;
    r.top = (a.top < b.top) ? a.top : b.top;
    r.right = (a.right > b.right) ? a.right : b.right;
    r.bottom = (a.bottom > b.bottom) ? a.bottom : b.bottom;
    return r;
}

// Clamp to [0,w] x [0,h] and normalize to a non-inverted rect.
[[nodiscard]] inline RECT ClampRect(RECT r, LONG w, LONG h) noexcept {
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > w) r.right = w;
    if (r.bottom > h) r.bottom = h;
    if (r.right < r.left) r.right = r.left;
    if (r.bottom < r.top) r.bottom = r.top;
    return r;
}

// Minimal dirty region for an element that moved from `oldRect` to `newRect`,
// clamped to the surface. Overlapping bounds merge into one rectangle.
[[nodiscard]] inline DirtyRegion ComputeDirtyRegion(const RECT& oldRect,
                                                    const RECT& newRect,
                                                    LONG surfaceW,
                                                    LONG surfaceH) noexcept {
    const RECT o = ClampRect(oldRect, surfaceW, surfaceH);
    const RECT n = ClampRect(newRect, surfaceW, surfaceH);

    DirtyRegion region;
    if (RectsIntersect(o, n)) {
        region.rects[0] = BoundingBox(o, n);
        region.count = 1;
    } else {
        region.rects[0] = o;
        region.rects[1] = n;
        region.count = 2;
    }
    return region;
}

// Deterministic, GPU-free self-test of the dirty-rect logic. Returns 0 on pass,
// non-zero (count of failures) otherwise. Defined in DirtyRectTest.cpp so it can
// print to a console when invoked via `LiveWallpaperEngine.exe --dirty-rect-test`.
[[nodiscard]] int RunDirtyRectSelfTest();

}  // namespace lwe::render
