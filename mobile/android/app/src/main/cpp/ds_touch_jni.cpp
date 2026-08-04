#include <jni.h>

#include "common/ds_touch_mapping.hpp"

#include <cstdint>

namespace {

jfloatArray make_rect_array(JNIEnv* env, const archstreamer::ViewRectF& rect) {
    if (!rect.valid()) {
        return nullptr;
    }
    jfloatArray out = env->NewFloatArray(4);
    if (out == nullptr) {
        return nullptr;
    }
    const jfloat values[4] = {rect.x, rect.y, rect.w, rect.h};
    env->SetFloatArrayRegion(out, 0, 4, values);
    return out;
}

archstreamer::DsScreenRects make_bot_layout(
    jint window_w,
    jint window_h,
    jboolean has_bot,
    jint bot_x,
    jint bot_y,
    jint bot_w,
    jint bot_h) {
    archstreamer::DsScreenRects layout;
    layout.window_w = static_cast<std::uint16_t>(window_w);
    layout.window_h = static_cast<std::uint16_t>(window_h);
    layout.has_bot = has_bot != JNI_FALSE;
    layout.bot_x = static_cast<std::int16_t>(bot_x);
    layout.bot_y = static_cast<std::int16_t>(bot_y);
    layout.bot_w = static_cast<std::int16_t>(bot_w);
    layout.bot_h = static_cast<std::int16_t>(bot_h);
    return layout;
}

archstreamer::DsScreenRects make_top_layout(
    jint window_w,
    jint window_h,
    jboolean has_top,
    jint top_x,
    jint top_y,
    jint top_w,
    jint top_h) {
    archstreamer::DsScreenRects layout;
    layout.window_w = static_cast<std::uint16_t>(window_w);
    layout.window_h = static_cast<std::uint16_t>(window_h);
    layout.has_top = has_top != JNI_FALSE;
    layout.top_x = static_cast<std::int16_t>(top_x);
    layout.top_y = static_cast<std::int16_t>(top_y);
    layout.top_w = static_cast<std::int16_t>(top_w);
    layout.top_h = static_cast<std::int16_t>(top_h);
    return layout;
}

} // namespace

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_archstreamer_client_ui_DsTouchMapping_nativeClientBottomScreenHitRect(
    JNIEnv* env,
    jclass,
    jfloat view_w,
    jfloat view_h,
    jboolean portrait_stack,
    jfloat stream_aspect,
    jfloat hybrid_ratio,
    jboolean emph_bottom,
    jboolean screens_swapped) {
    const auto hit = archstreamer::client_bottom_screen_hit_rect(
        view_w,
        view_h,
        portrait_stack != JNI_FALSE,
        stream_aspect,
        hybrid_ratio,
        emph_bottom != JNI_FALSE,
        screens_swapped != JNI_FALSE);
    if (!hit.valid()) {
        return nullptr;
    }
    return make_rect_array(env, hit);
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_archstreamer_client_ui_DsTouchMapping_nativeHorizontalEmphFracsFromLayout(
    JNIEnv* env,
    jclass,
    jint window_w,
    jint window_h,
    jboolean has_top,
    jint top_x,
    jint top_y,
    jint top_w,
    jint top_h,
    jboolean has_bot,
    jint bot_x,
    jint bot_y,
    jint bot_w,
    jint bot_h) {
    archstreamer::DsScreenRects layout;
    layout.window_w = static_cast<std::uint16_t>(window_w);
    layout.window_h = static_cast<std::uint16_t>(window_h);
    layout.has_top = has_top != JNI_FALSE;
    layout.top_x = static_cast<std::int16_t>(top_x);
    layout.top_y = static_cast<std::int16_t>(top_y);
    layout.top_w = static_cast<std::int16_t>(top_w);
    layout.top_h = static_cast<std::int16_t>(top_h);
    layout.has_bot = has_bot != JNI_FALSE;
    layout.bot_x = static_cast<std::int16_t>(bot_x);
    layout.bot_y = static_cast<std::int16_t>(bot_y);
    layout.bot_w = static_cast<std::int16_t>(bot_w);
    layout.bot_h = static_cast<std::int16_t>(bot_h);
    const auto fr = archstreamer::horizontal_emph_fracs_from_layout(layout);
    if (!fr.has_value()) {
        return nullptr;
    }
    jfloatArray out = env->NewFloatArray(8);
    if (out == nullptr) {
        return nullptr;
    }
    const jfloat values[8] = {
        fr->left_u0,
        fr->left_u1,
        fr->right_u0,
        fr->right_u1,
        fr->left_v0,
        fr->left_v1,
        fr->right_v0,
        fr->right_v1,
    };
    env->SetFloatArrayRegion(out, 0, 8, values);
    return out;
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_archstreamer_client_ui_DsTouchMapping_nativeHorizontalEmphFracs(
    JNIEnv* env,
    jclass,
    jfloat hybrid_ratio,
    jboolean emph_bottom) {
    const auto fr = archstreamer::horizontal_emph_fracs(
        hybrid_ratio, emph_bottom != JNI_FALSE);
    jfloatArray out = env->NewFloatArray(8);
    if (out == nullptr) {
        return nullptr;
    }
    const jfloat values[8] = {
        fr.left_u0,
        fr.left_u1,
        fr.right_u0,
        fr.right_u1,
        fr.left_v0,
        fr.left_v1,
        fr.right_v0,
        fr.right_v1,
    };
    env->SetFloatArrayRegion(out, 0, 8, values);
    return out;
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_archstreamer_client_ui_DsTouchMapping_nativeBottomScreenHitRect(
    JNIEnv* env,
    jclass,
    jfloat view_w,
    jfloat view_h,
    jint window_w,
    jint window_h,
    jboolean has_bot,
    jint bot_x,
    jint bot_y,
    jint bot_w,
    jint bot_h) {
    const auto layout = make_bot_layout(
        window_w, window_h, has_bot, bot_x, bot_y, bot_w, bot_h);
    const auto hit = archstreamer::bottom_screen_hit_rect(view_w, view_h, layout);
    if (!hit.has_value()) {
        return nullptr;
    }
    return make_rect_array(env, *hit);
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_archstreamer_client_ui_DsTouchMapping_nativeTopScreenHitRect(
    JNIEnv* env,
    jclass,
    jfloat view_w,
    jfloat view_h,
    jint window_w,
    jint window_h,
    jboolean has_top,
    jint top_x,
    jint top_y,
    jint top_w,
    jint top_h) {
    const auto layout = make_top_layout(
        window_w, window_h, has_top, top_x, top_y, top_w, top_h);
    const auto hit = archstreamer::top_screen_hit_rect(view_w, view_h, layout);
    if (!hit.has_value()) {
        return nullptr;
    }
    return make_rect_array(env, *hit);
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_archstreamer_client_ui_DsTouchMapping_nativeLocalPointToNormalized(
    JNIEnv* env,
    jclass,
    jfloat local_x,
    jfloat local_y,
    jfloat local_w,
    jfloat local_h) {
    float nx = 0.f;
    float ny = 0.f;
    if (!archstreamer::local_point_to_normalized(local_x, local_y, local_w, local_h, nx, ny)) {
        return nullptr;
    }
    jfloatArray out = env->NewFloatArray(2);
    if (out == nullptr) {
        return nullptr;
    }
    const jfloat values[2] = {nx, ny};
    env->SetFloatArrayRegion(out, 0, 2, values);
    return out;
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_archstreamer_client_ui_DsTouchMapping_nativeEncodeNormalizedU16(
    JNIEnv* env,
    jclass,
    jfloat nx,
    jfloat ny) {
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    archstreamer::encode_normalized_u16(nx, ny, x, y);
    jintArray out = env->NewIntArray(2);
    if (out == nullptr) {
        return nullptr;
    }
    const jint values[2] = {static_cast<jint>(x), static_cast<jint>(y)};
    env->SetIntArrayRegion(out, 0, 2, values);
    return out;
}
