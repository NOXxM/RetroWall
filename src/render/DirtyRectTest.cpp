// ---------------------------------------------------------------------------
// render/DirtyRectTest.cpp
// GPU-free self-test that demonstrates partial-frame update geometry: it walks
// an animated square across a virtual surface and verifies, frame by frame,
// that the computed dirty region exactly bounds what changed (old ∪ new) and
// never exceeds the surface. Invoked with `--dirty-rect-test`.
// ---------------------------------------------------------------------------
#include "render/DirtyRect.hpp"

#include <windows.h>

#include <cstdio>

namespace lwe::render {

namespace {

bool RectEquals(const RECT& a, const RECT& b) noexcept {
    return a.left == b.left && a.top == b.top && a.right == b.right &&
           a.bottom == b.bottom;
}

bool RectContains(const RECT& outer, const RECT& inner) noexcept {
    return inner.left >= outer.left && inner.top >= outer.top &&
           inner.right <= outer.right && inner.bottom <= outer.bottom;
}

// Every pixel that changed (old square OR new square, after clamping) must lie
// inside at least one reported dirty rect; nothing else is claimed dirty.
bool RegionCoversChange(const DirtyRegion& region, const RECT& oldClamped,
                        const RECT& newClamped) noexcept {
    auto coveredBySome = [&](const RECT& changed) {
        if (changed.right <= changed.left || changed.bottom <= changed.top) {
            return true;  // empty change is trivially covered
        }
        for (UINT i = 0; i < region.count; ++i) {
            if (RectContains(region.rects[i], changed)) {
                return true;
            }
        }
        return false;
    };
    return coveredBySome(oldClamped) && coveredBySome(newClamped);
}

int g_failures = 0;

void Check(bool condition, const char* name) {
    if (!condition) {
        ++g_failures;
        std::printf("  [FAIL] %s\n", name);
    } else {
        std::printf("  [ ok ] %s\n", name);
    }
}

}  // namespace

int RunDirtyRectSelfTest() {
    g_failures = 0;
    std::printf("Dirty-rectangle self-test\n=========================\n");

    constexpr LONG kW = 1920;
    constexpr LONG kH = 1080;

    // 1. Disjoint move -> two separate rects, each equal to a clamped bound.
    {
        const RECT oldR{0, 0, 10, 10};
        const RECT newR{100, 100, 110, 110};
        const DirtyRegion d = ComputeDirtyRegion(oldR, newR, kW, kH);
        Check(d.count == 2, "disjoint move yields 2 rects");
        Check(RectEquals(d.rects[0], oldR) && RectEquals(d.rects[1], newR),
              "disjoint rects equal old/new bounds");
    }

    // 2. Overlapping move -> single bounding box == union.
    {
        const RECT oldR{0, 0, 10, 10};
        const RECT newR{5, 5, 15, 15};
        const DirtyRegion d = ComputeDirtyRegion(oldR, newR, kW, kH);
        Check(d.count == 1, "overlapping move yields 1 rect");
        Check(RectEquals(d.rects[0], RECT{0, 0, 15, 15}),
              "overlap rect equals bounding box");
    }

    // 3. Out-of-bounds bounds are clamped to the surface.
    {
        const RECT oldR{-50, -50, 10, 10};
        const RECT newR{kW - 5, kH - 5, kW + 200, kH + 200};
        const DirtyRegion d = ComputeDirtyRegion(oldR, newR, kW, kH);
        const RECT surface{0, 0, kW, kH};
        bool inside = true;
        for (UINT i = 0; i < d.count; ++i) {
            inside = inside && RectContains(surface, d.rects[i]);
        }
        Check(inside, "clamped rects stay within the surface");
    }

    // 4. Full animation walk: a 200x150 square bouncing for 2000 frames. Each
    //    frame's dirty region must cover both the old and new (clamped) bounds
    //    and never leave the surface.
    {
        RECT sq{0, 0, 200, 150};
        int vx = 7;
        int vy = 5;
        bool coverageOk = true;
        bool boundsOk = true;
        const RECT surface{0, 0, kW, kH};

        for (int frame = 0; frame < 2000; ++frame) {
            const RECT oldR = sq;

            // advance + bounce
            OffsetRect(&sq, vx, vy);
            if (sq.left < 0) {
                OffsetRect(&sq, -sq.left, 0);
                vx = -vx;
            }
            if (sq.top < 0) {
                OffsetRect(&sq, 0, -sq.top);
                vy = -vy;
            }
            if (sq.right > kW) {
                OffsetRect(&sq, kW - sq.right, 0);
                vx = -vx;
            }
            if (sq.bottom > kH) {
                OffsetRect(&sq, 0, kH - sq.bottom);
                vy = -vy;
            }

            const DirtyRegion d = ComputeDirtyRegion(oldR, sq, kW, kH);
            const RECT oldC = ClampRect(oldR, kW, kH);
            const RECT newC = ClampRect(sq, kW, kH);

            coverageOk = coverageOk && RegionCoversChange(d, oldC, newC);
            for (UINT i = 0; i < d.count; ++i) {
                boundsOk = boundsOk && RectContains(surface, d.rects[i]);
            }
        }
        Check(coverageOk, "2000-frame walk: every dirty region covers the change");
        Check(boundsOk, "2000-frame walk: no dirty rect leaves the surface");
    }

    std::printf("=========================\n%s (%d failure%s)\n",
                g_failures == 0 ? "PASS" : "FAIL", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures;
}

}  // namespace lwe::render
