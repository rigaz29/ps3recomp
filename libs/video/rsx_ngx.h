/*
 * ps3recomp - the DLSS-G provider interface, as a host calls it (rsx_anr.cpp)
 *
 * A frame generation DLL such as ANR FG's nvngx_dlssg.dll exports NGX's D3D12 entry points and
 * reads its inputs from a parameter map the host hands it. These are the declarations a host
 * needs for that, written here for interoperability: the calling convention, the result codes,
 * the API version and feature values, the map's virtual functions in their order (the DLL calls
 * them through the vtable), and the names it reads. No file of NVIDIA's SDK is used.
 */
#ifndef RSX_NGX_H
#define RSX_NGX_H

#ifdef _WIN32

#include <d3d11.h>
#include <d3d12.h>

#define NGX_CALL __cdecl

typedef unsigned int ngx_result;
static const ngx_result ngx_ok = 0x1u;
static const ngx_result ngx_fail = 0xBAD00000u;            /* failures: this in the top 12 bits */
static inline bool ngx_failed(ngx_result r) { return (r & 0xFFF00000u) == ngx_fail; }

static const unsigned int ngx_version_api = 0x15u;          /* the 1.5 host interface */
static const int ngx_feature_frame_generation = 11;

struct ngx_handle;                                          /* the DLL's, opaque */
struct ngx_feature_common_info;                             /* passed as NULL */
typedef void(NGX_CALL* ngx_progress_fn)(float progress, bool& cancel);

/* The parameter map. The DLL calls these through the vtable, so their order is the interface. */
struct ngx_parameter {
    virtual void Set(const char* name, unsigned long long v) = 0;
    virtual void Set(const char* name, float v) = 0;
    virtual void Set(const char* name, double v) = 0;
    virtual void Set(const char* name, unsigned int v) = 0;
    virtual void Set(const char* name, int v) = 0;
    virtual void Set(const char* name, ID3D11Resource* v) = 0;
    virtual void Set(const char* name, ID3D12Resource* v) = 0;
    virtual void Set(const char* name, void* v) = 0;
    virtual ngx_result Get(const char* name, unsigned long long* v) const = 0;
    virtual ngx_result Get(const char* name, float* v) const = 0;
    virtual ngx_result Get(const char* name, double* v) const = 0;
    virtual ngx_result Get(const char* name, unsigned int* v) const = 0;
    virtual ngx_result Get(const char* name, int* v) const = 0;
    virtual ngx_result Get(const char* name, ID3D11Resource** v) const = 0;
    virtual ngx_result Get(const char* name, ID3D12Resource** v) const = 0;
    virtual ngx_result Get(const char* name, void** v) const = 0;
    virtual void Reset() = 0;
};

/* The entry points, by the names the DLL exports them under (rsx_anr.cpp's GetProcAddress). */
typedef ngx_result(NGX_CALL* ngx_init_fn)(unsigned long long app_id, const wchar_t* data_path, ID3D12Device* device,
                                          unsigned int version, const ngx_feature_common_info* info);
typedef ngx_result(NGX_CALL* ngx_populate_fn)(ngx_parameter* params);
typedef ngx_result(NGX_CALL* ngx_create_fn)(ID3D12GraphicsCommandList* cl, int feature, ngx_parameter* params,
                                            ngx_handle** out);
typedef ngx_result(NGX_CALL* ngx_evaluate_fn)(ID3D12GraphicsCommandList* cl, const ngx_handle* feature,
                                              ngx_parameter* params, ngx_progress_fn progress);
typedef ngx_result(NGX_CALL* ngx_release_fn)(ngx_handle* feature);

/* The names the map is read by. */
namespace ngx_p {
    constexpr const char* backbuffer = "DLSSG.Backbuffer";
    constexpr const char* backbuffer_format = "DLSSG.BackbufferFormat";
    constexpr const char* backbuffer_subrect_base_x = "DLSSG.BackbufferSubrectBaseX";
    constexpr const char* backbuffer_subrect_base_y = "DLSSG.BackbufferSubrectBaseY";
    constexpr const char* backbuffer_subrect_height = "DLSSG.BackbufferSubrectHeight";
    constexpr const char* backbuffer_subrect_width = "DLSSG.BackbufferSubrectWidth";
    constexpr const char* camera_aspect_ratio = "DLSSG.CameraAspectRatio";
    constexpr const char* camera_far = "DLSSG.CameraFar";
    constexpr const char* camera_fov = "DLSSG.CameraFOV";
    constexpr const char* camera_fwd_x = "DLSSG.CameraFwdX";
    constexpr const char* camera_fwd_y = "DLSSG.CameraFwdY";
    constexpr const char* camera_fwd_z = "DLSSG.CameraFwdZ";
    constexpr const char* camera_motion_included = "DLSSG.CameraMotionIncluded";
    constexpr const char* camera_near = "DLSSG.CameraNear";
    constexpr const char* camera_pinhole_offset_x = "DLSSG.CameraPinholeOffsetX";
    constexpr const char* camera_pinhole_offset_y = "DLSSG.CameraPinholeOffsetY";
    constexpr const char* camera_pos_x = "DLSSG.CameraPosX";
    constexpr const char* camera_pos_y = "DLSSG.CameraPosY";
    constexpr const char* camera_pos_z = "DLSSG.CameraPosZ";
    constexpr const char* camera_right_x = "DLSSG.CameraRightX";
    constexpr const char* camera_right_y = "DLSSG.CameraRightY";
    constexpr const char* camera_right_z = "DLSSG.CameraRightZ";
    constexpr const char* camera_up_x = "DLSSG.CameraUpX";
    constexpr const char* camera_up_y = "DLSSG.CameraUpY";
    constexpr const char* camera_up_z = "DLSSG.CameraUpZ";
    constexpr const char* camera_view_to_clip = "DLSSG.CameraViewToClip";
    constexpr const char* clip_to_camera_view = "DLSSG.ClipToCameraView";
    constexpr const char* clip_to_lens_clip = "DLSSG.ClipToLensClip";
    constexpr const char* clip_to_prev_clip = "DLSSG.ClipToPrevClip";
    constexpr const char* color_buffers_hdr = "DLSSG.ColorBuffersHDR";
    constexpr const char* creation_node_mask = "CreationNodeMask";
    constexpr const char* depth = "DLSSG.Depth";
    constexpr const char* depth_inverted = "DLSSG.DepthInverted";
    constexpr const char* depth_subrect_base_x = "DLSSG.DepthSubrectBaseX";
    constexpr const char* depth_subrect_base_y = "DLSSG.DepthSubrectBaseY";
    constexpr const char* depth_subrect_height = "DLSSG.DepthSubrectHeight";
    constexpr const char* depth_subrect_width = "DLSSG.DepthSubrectWidth";
    constexpr const char* dynamic_resolution = "DLSSG.DynamicResolution";
    constexpr const char* eval_flags = "DLSSG.EvalFlags";
    constexpr const char* height = "DLSSG.Height";
    constexpr const char* hud_less = "DLSSG.HUDLess";
    constexpr const char* hud_less_subrect_base_x = "DLSSG.HUDLessSubrectBaseX";
    constexpr const char* hud_less_subrect_base_y = "DLSSG.HUDLessSubrectBaseY";
    constexpr const char* hud_less_subrect_height = "DLSSG.HUDLessSubrectHeight";
    constexpr const char* hud_less_subrect_width = "DLSSG.HUDLessSubrectWidth";
    constexpr const char* internal_height = "DLSSG.InternalHeight";
    constexpr const char* internal_width = "DLSSG.InternalWidth";
    constexpr const char* jitter_offset_x = "DLSSG.JitterOffsetX";
    constexpr const char* jitter_offset_y = "DLSSG.JitterOffsetY";
    constexpr const char* menu_detection_enabled = "DLSSG.MenuDetectionEnabled";
    constexpr const char* min_relative_linear_depth_object_separation = "DLSSG.MinRelativeLinearDepthObjectSeparation";
    constexpr const char* multi_frame_count = "DLSSG.MultiFrameCount";
    constexpr const char* multi_frame_count_max = "DLSSG.MultiFrameCountMax";
    constexpr const char* multi_frame_index = "DLSSG.MultiFrameIndex";
    constexpr const char* mvec_dilated = "DLSSG.MvecDilated";
    constexpr const char* mvec_invalid_value = "DLSSG.MvecInvalidValue";
    constexpr const char* mvec_jittered = "DLSSG.MvecJittered";
    constexpr const char* mvec_scale_x = "DLSSG.MvecScaleX";
    constexpr const char* mvec_scale_y = "DLSSG.MvecScaleY";
    constexpr const char* mvecs = "DLSSG.MVecs";
    constexpr const char* mvecs_subrect_base_x = "DLSSG.MVecsSubrectBaseX";
    constexpr const char* mvecs_subrect_base_y = "DLSSG.MVecsSubrectBaseY";
    constexpr const char* mvecs_subrect_height = "DLSSG.MVecsSubrectHeight";
    constexpr const char* mvecs_subrect_width = "DLSSG.MVecsSubrectWidth";
    constexpr const char* ngx_height = "Height";
    constexpr const char* ngx_width = "Width";
    constexpr const char* not_rendering_game_frames = "DLSSG.NotRenderingGameFrames";
    constexpr const char* ortho_projection = "DLSSG.OrthoProjection";
    constexpr const char* output_interpolated = "DLSSG.OutputInterpolated";
    constexpr const char* output_real = "DLSSG.OutputReal";
    constexpr const char* prev_clip_to_clip = "DLSSG.PrevClipToClip";
    constexpr const char* reset = "DLSSG.Reset";
    constexpr const char* ui = "DLSSG.UI";
    constexpr const char* ui_alpha = "DLSSG.UIAlpha";
    constexpr const char* user_interface_recomposition_enabled = "DLSSG.UserInterfaceRecompositionEnabled";
    constexpr const char* visibility_node_mask = "VisibilityNodeMask";
    constexpr const char* width = "DLSSG.Width";
}  // namespace ngx_p

#endif /* _WIN32 */
#endif /* RSX_NGX_H */
