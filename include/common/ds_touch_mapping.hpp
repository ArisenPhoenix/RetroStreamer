#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace archstreamer {

/** Native DS touchscreen resolution (stylus space after host conversion). */
constexpr int DsTouchPixelWidth = 256;
constexpr int DsTouchPixelHeight = 192;

/** TouchInput x/y are this many ticks for 0..1 within the bottom screen. */
constexpr std::uint16_t DsTouchNormMax = 65535;

struct ViewRectF {
    float x = 0.f;
    float y = 0.f;
    float w = 0.f;
    float h = 0.f;

    bool valid() const { return w > 0.f && h > 0.f; }
};

/**
 * Screen panes in host-window pixels (same fields as protocol DsScreenLayout).
 * Kept free of protocol.hpp so Android NDK can compile this header alone.
 */
struct DsScreenRects {
    std::uint16_t window_w = 0;
    std::uint16_t window_h = 0;
    bool has_top = false;
    std::int16_t top_x = 0;
    std::int16_t top_y = 0;
    std::int16_t top_w = 0;
    std::int16_t top_h = 0;
    bool has_bot = false;
    std::int16_t bot_x = 0;
    std::int16_t bot_y = 0;
    std::int16_t bot_w = 0;
    std::int16_t bot_h = 0;
};

/** Letterbox a content aspect into a view (same math as video paint paths). */
inline ViewRectF letterbox_content_rect(
    float view_w,
    float view_h,
    float content_w,
    float content_h) {
    ViewRectF out;
    if (view_w <= 0.f || view_h <= 0.f || content_w <= 0.f || content_h <= 0.f) {
        return out;
    }
    const float view_aspect = view_w / view_h;
    const float content_aspect = content_w / content_h;
    if (view_aspect > content_aspect) {
        out.h = view_h;
        out.w = view_h * content_aspect;
    } else {
        out.w = view_w;
        out.h = view_w / content_aspect;
    }
    out.x = (view_w - out.w) * 0.5f;
    out.y = (view_h - out.h) * 0.5f;
    return out;
}

/**
 * Map a host-window pixel rect into the client's view, assuming the streamed
 * frame matches the host window aspect and is letterboxed.
 */
inline ViewRectF map_host_rect_to_view(
    float view_w,
    float view_h,
    float window_w,
    float window_h,
    float host_x,
    float host_y,
    float host_w,
    float host_h) {
    ViewRectF out;
    if (window_w <= 0.f || window_h <= 0.f) {
        return out;
    }
    const ViewRectF box = letterbox_content_rect(view_w, view_h, window_w, window_h);
    if (!box.valid()) {
        return out;
    }
    out.x = box.x + box.w * (host_x / window_w);
    out.y = box.y + box.h * (host_y / window_h);
    out.w = box.w * (host_w / window_w);
    out.h = box.h * (host_h / window_h);
    return out;
}

inline std::optional<ViewRectF> top_screen_hit_rect(
    float view_w,
    float view_h,
    const DsScreenRects& layout) {
    if (!layout.has_top || layout.window_w == 0 || layout.window_h == 0) {
        return std::nullopt;
    }
    const ViewRectF r = map_host_rect_to_view(
        view_w,
        view_h,
        static_cast<float>(layout.window_w),
        static_cast<float>(layout.window_h),
        static_cast<float>(layout.top_x),
        static_cast<float>(layout.top_y),
        static_cast<float>(layout.top_w),
        static_cast<float>(layout.top_h));
    if (!r.valid()) {
        return std::nullopt;
    }
    return r;
}

inline std::optional<ViewRectF> bottom_screen_hit_rect(
    float view_w,
    float view_h,
    const DsScreenRects& layout) {
    if (!layout.has_bot || layout.window_w == 0 || layout.window_h == 0) {
        return std::nullopt;
    }
    const ViewRectF r = map_host_rect_to_view(
        view_w,
        view_h,
        static_cast<float>(layout.window_w),
        static_cast<float>(layout.window_h),
        static_cast<float>(layout.bot_x),
        static_cast<float>(layout.bot_y),
        static_cast<float>(layout.bot_w),
        static_cast<float>(layout.bot_h));
    if (!r.valid()) {
        return std::nullopt;
    }
    return r;
}

inline float clamp01(float v) {
    return std::clamp(v, 0.f, 1.f);
}

/**
 * Horizontal melonDS layout (ArchStreamer policy):
 * - Top screen always left, bottom screen always right (SwapScreenEmphasis only
 *   swaps sizes — positions stay put).
 * - Emphasized screen fills the content height; the other is ~1/ratio tall and
 *   vertically centered in its column (ratio 3 → small ≈ ⅓ of large).
 */
struct DsHorizontalEmphFracs {
    float left_u0 = 0.f;
    float left_u1 = 0.75f;
    float right_u0 = 0.75f;
    float right_u1 = 1.f;
    float left_v0 = 0.f;
    float left_v1 = 1.f;
    float right_v0 = 0.f;
    float right_v1 = 1.f;
    bool emph_bottom = false;
};

inline DsHorizontalEmphFracs horizontal_emph_fracs(
    float hybrid_ratio = 3.f,
    bool emph_bottom = false) {
    DsHorizontalEmphFracs out;
    out.emph_bottom = emph_bottom;
    const float r = std::clamp(hybrid_ratio, 2.f, 4.f);
    // Match melonDS Horizontal+Emph window split (e.g. 960+320 in 1280): large:small = r:1.
    // Do not use Hybrid buffer gaps (2·R) — that under-cuts the large pane (~0.746 vs 0.75)
    // and leaves a sliver of the top screen in the bottom crop.
    const float large_u = std::clamp(r / (r + 1.f), 0.5f, 0.9f);
    const float small_u = 1.f - large_u;
    const float small_v = clamp01(1.f / r); // ~⅓ when r=3
    const float small_v0 = (1.f - small_v) * 0.5f;
    const float small_v1 = small_v0 + small_v;

    if (!emph_bottom) {
        // EmphTop: large left (full height), small centered right.
        out.left_u0 = 0.f;
        out.left_u1 = large_u;
        out.right_u0 = large_u;
        out.right_u1 = 1.f;
        out.left_v0 = 0.f;
        out.left_v1 = 1.f;
        out.right_v0 = small_v0;
        out.right_v1 = small_v1;
    } else {
        // EmphBot: small centered left, large right (full height).
        out.left_u0 = 0.f;
        out.left_u1 = small_u;
        out.right_u0 = small_u;
        out.right_u1 = 1.f;
        out.left_v0 = small_v0;
        out.left_v1 = small_v1;
        out.right_v0 = 0.f;
        out.right_v1 = 1.f;
    }
    return out;
}

/** True when host SCREENS shows the bottom pane taller than the top (EmphBot). */
inline bool layout_emphasizes_bottom(const DsScreenRects& layout) {
    return layout.has_top && layout.has_bot && layout.bot_h > layout.top_h;
}

/**
 * Reject obviously broken SCREENS AABBs (e.g. old melonDS matrix bug that put
 * both panes at x=0 with overlapping/nonsense sizes).
 */
inline bool layout_rects_look_sane(const DsScreenRects& layout) {
    if (!layout.has_bot || layout.window_w == 0 || layout.window_h == 0) {
        return false;
    }
    if (layout.bot_w <= 0 || layout.bot_h <= 0) {
        return false;
    }
    // Horizontal policy: bottom stays on the right — center should not be in the left third.
    const float bot_cx =
        static_cast<float>(layout.bot_x) + static_cast<float>(layout.bot_w) * 0.5f;
    if (bot_cx < static_cast<float>(layout.window_w) * 0.35f) {
        return false;
    }
    const float aspect =
        static_cast<float>(layout.bot_w) / static_cast<float>(layout.bot_h);
    if (aspect < 0.45f || aspect > 3.5f) {
        return false;
    }
    return true;
}

/** UV fracs from host SCREENS AABBs (preferred when sane). */
inline std::optional<DsHorizontalEmphFracs> horizontal_emph_fracs_from_layout(
    const DsScreenRects& layout) {
    if (!layout_rects_look_sane(layout) || !layout.has_top || layout.window_w == 0 ||
        layout.window_h == 0) {
        return std::nullopt;
    }
    const float ww = static_cast<float>(layout.window_w);
    const float wh = static_cast<float>(layout.window_h);
    DsHorizontalEmphFracs out;
    out.emph_bottom = layout_emphasizes_bottom(layout);
    out.left_u0 = clamp01(static_cast<float>(layout.top_x) / ww);
    out.left_u1 = clamp01(static_cast<float>(layout.top_x + layout.top_w) / ww);
    out.left_v0 = clamp01(static_cast<float>(layout.top_y) / wh);
    out.left_v1 = clamp01(static_cast<float>(layout.top_y + layout.top_h) / wh);
    out.right_u0 = clamp01(static_cast<float>(layout.bot_x) / ww);
    out.right_u1 = clamp01(static_cast<float>(layout.bot_x + layout.bot_w) / ww);
    out.right_v0 = clamp01(static_cast<float>(layout.bot_y) / wh);
    out.right_v1 = clamp01(static_cast<float>(layout.bot_y + layout.bot_h) / wh);
    if (out.left_u1 <= out.left_u0 || out.right_u1 <= out.right_u0) {
        return std::nullopt;
    }
    return out;
}

/**
 * Client-local estimate of where the DS bottom pane is drawn in the stream.
 * Portrait stack: lower phone pane. Landscape: right column (bottom), either
 * full-height (EmphBot) or ~⅓-height centered (EmphTop). Prefer host layout
 * via bottom_screen_hit_rect when available.
 */
inline ViewRectF client_bottom_screen_hit_rect(
    float view_w,
    float view_h,
    bool portrait_stack,
    float stream_aspect = 16.f / 9.f,
    float hybrid_ratio = 3.f,
    bool emph_bottom = false,
    bool screens_swapped = false) {
    ViewRectF out;
    if (view_w <= 0.f || view_h <= 0.f) {
        return out;
    }

    if (portrait_stack && stream_aspect > 1.15f) {
        // Two equal 4:3 panes stacked. DS bottom is the lower pane unless the
        // mobile client swapped pane assignment (R2 local swap).
        const float pane_h = view_w * 0.75f;
        const float total_h = pane_h * 2.f;
        const float origin_y = ((view_h - total_h) * 0.5f);
        const float used = (total_h > view_h) ? (view_h * 0.5f) : pane_h;
        const float base_y = origin_y > 0.f ? origin_y : 0.f;
        out.x = 0.f;
        out.y = screens_swapped ? base_y : (base_y + used);
        out.w = view_w;
        out.h = used;
        return out;
    }

    const float aspect = stream_aspect > 0.1f ? stream_aspect : (16.f / 9.f);
    const ViewRectF box = letterbox_content_rect(view_w, view_h, aspect, 1.f);
    if (!box.valid()) {
        return out;
    }

    const auto fr = horizontal_emph_fracs(hybrid_ratio, emph_bottom);
    // Stream content: DS bottom is always the right column (host positions fixed).
    out.x = box.x + box.w * fr.right_u0;
    out.y = box.y + box.h * fr.right_v0;
    out.w = box.w * (fr.right_u1 - fr.right_u0);
    out.h = box.h * (fr.right_v1 - fr.right_v0);
    (void)screens_swapped;
    return out;
}

/** Prefer host SCREENS rects; fall back to shared Horizontal+Emph heuristic. */
inline ViewRectF resolve_bottom_screen_hit_rect(
    float view_w,
    float view_h,
    bool portrait_stack,
    const DsScreenRects* layout,
    float stream_aspect = 16.f / 9.f,
    float hybrid_ratio = 3.f,
    bool screens_swapped = false) {
    const bool layout_ok = layout != nullptr && layout_rects_look_sane(*layout);
    // Portrait dual-pane: client owns pane geometry (incl. local R2 swap).
    if (portrait_stack) {
        const bool emph_bot = layout_ok && layout_emphasizes_bottom(*layout);
        return client_bottom_screen_hit_rect(
            view_w,
            view_h,
            true,
            stream_aspect,
            hybrid_ratio,
            emph_bot,
            screens_swapped);
    }
    if (layout_ok) {
        if (const auto hit = bottom_screen_hit_rect(view_w, view_h, *layout)) {
            return *hit;
        }
    }
    const bool emph_bot = layout_ok && layout_emphasizes_bottom(*layout);
    return client_bottom_screen_hit_rect(
        view_w, view_h, false, stream_aspect, hybrid_ratio, emph_bot, false);
}

/** Point in view space → normalized 0..1 within @p rect (false if outside / invalid). */
inline bool view_point_to_normalized(
    float view_x,
    float view_y,
    const ViewRectF& rect,
    float& out_nx,
    float& out_ny) {
    if (!rect.valid()) {
        return false;
    }
    out_nx = (view_x - rect.x) / rect.w;
    out_ny = (view_y - rect.y) / rect.h;
    if (out_nx < 0.f || out_nx > 1.f || out_ny < 0.f || out_ny > 1.f) {
        return false;
    }
    return true;
}

/** Local coords inside a hit target sized @p local_w × @p local_h → 0..1. */
inline bool local_point_to_normalized(
    float local_x,
    float local_y,
    float local_w,
    float local_h,
    float& out_nx,
    float& out_ny) {
    if (local_w <= 0.f || local_h <= 0.f) {
        return false;
    }
    out_nx = clamp01(local_x / local_w);
    out_ny = clamp01(local_y / local_h);
    return true;
}

inline void encode_normalized_u16(float nx, float ny, std::uint16_t& out_x, std::uint16_t& out_y) {
    out_x = static_cast<std::uint16_t>(std::lround(clamp01(nx) * static_cast<float>(DsTouchNormMax)));
    out_y = static_cast<std::uint16_t>(std::lround(clamp01(ny) * static_cast<float>(DsTouchNormMax)));
}

inline void decode_normalized_u16(std::uint16_t in_x, std::uint16_t in_y, float& out_nx, float& out_ny) {
    out_nx = static_cast<float>(in_x) / static_cast<float>(DsTouchNormMax);
    out_ny = static_cast<float>(in_y) / static_cast<float>(DsTouchNormMax);
}

/**
 * Host-side: normalized bottom-screen fractions → absolute DS stylus pixels.
 * Matches melonDS touchScreen space (0..255 × 0..191).
 */
inline void ds_coords_from_normalized(
    float nx,
    float ny,
    std::uint16_t& out_x,
    std::uint16_t& out_y) {
    const int x = static_cast<int>(std::lround(clamp01(nx) * static_cast<float>(DsTouchPixelWidth - 1)));
    const int y = static_cast<int>(std::lround(clamp01(ny) * static_cast<float>(DsTouchPixelHeight - 1)));
    out_x = static_cast<std::uint16_t>(std::clamp(x, 0, DsTouchPixelWidth - 1));
    out_y = static_cast<std::uint16_t>(std::clamp(y, 0, DsTouchPixelHeight - 1));
}

inline void ds_coords_from_normalized_u16(
    std::uint16_t norm_x,
    std::uint16_t norm_y,
    std::uint16_t& out_x,
    std::uint16_t& out_y) {
    float nx = 0.f;
    float ny = 0.f;
    decode_normalized_u16(norm_x, norm_y, nx, ny);
    ds_coords_from_normalized(nx, ny, out_x, out_y);
}

} // namespace archstreamer
