module;
#include <Windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

module patch.post_process:renderer;

import std;
import imgui_hook;
import :utils;
import :settings;
import :shader;

namespace patch::post_process {

class PostProcessRenderer {
private:
  // Two independent resource sets: slot 0 = main world, slot 1 = minimap.
  // Keeping them separate avoids re-creating textures every frame when the
  // minimap renders into a differently sized target than the main view.
  struct Slot {
    ComPtr<IDirect3DTexture9> texture;
    ComPtr<IDirect3DSurface9> surface;
    ComPtr<IDirect3DVertexBuffer9> vb;
    ComPtr<IDirect3DStateBlock9> state;
    UINT width{0};
    UINT height{0};
  };
  std::array<Slot, 2> slots_;
  ComPtr<IDirect3DPixelShader9> pixel_shader_;
  bool shader_compilation_failed_{false};
  UINT frame_count_{0};

  struct ScreenVertex {
    float x{0.0f}, y{0.0f}, z{0.0f}, rhw{1.0f};
    float u{0.0f}, v{0.0f};
  };

  struct alignas(16) ShaderConstants {
    std::array<float, 4> scale_bias; // c0: scale, bias, saturation, gamma
    std::array<float, 4>
        color_temp_tint;          // c1: temp_r, temp_g, temp_b, tonemap_mode
    std::array<float, 4> effects; // c2: sharpening, fxaa, vignette, grain
    std::array<float, 4> screen_size; // c3: 1/w, 1/h, w, h
    std::array<float, 4>
        shadows; // c4: shadow_r, shadow_g, shadow_b, split_intensity
    std::array<float, 4>
        highlights; // c5: highlight_r, highlight_g, highlight_b, split_balance
    std::array<float, 4>
        extra; // c6: vibrance, bleach_bypass, black_level, s_curve
    std::array<float, 4> film; // c7: technicolor, dpx_film, frame_seed, 0

    [[nodiscard]] static ShaderConstants
    FromSettings(const Settings &s, float width, float height,
                 float frame_seed) noexcept {
      const float scale = std::exp2(s.exposure) * s.contrast;
      const float bias = (s.brightness - 0.5f) * s.contrast + 0.5f;

      const float temp_r = s.color_temp * 0.1f + s.tint * 0.05f;
      const float temp_g = -s.tint * 0.1f;
      const float temp_b = -s.color_temp * 0.1f + s.tint * 0.05f;

      return ShaderConstants{
          .scale_bias = {scale, bias, s.saturation, s.gamma},
          .color_temp_tint = {temp_r, temp_g, temp_b,
                              static_cast<float>(
                                  std::to_underlying(s.tonemap_mode))},
          .effects = {s.sharpening, s.fxaa_enabled ? 1.0f : 0.0f,
                      s.vignette_intensity, s.film_grain},
          .screen_size = {1.0f / width, 1.0f / height, width, height},
          .shadows = {s.shadow_tint[0], s.shadow_tint[1], s.shadow_tint[2],
                      s.split_intensity},
          .highlights = {s.highlight_tint[0], s.highlight_tint[1],
                         s.highlight_tint[2], s.split_balance},
          .extra = {s.vibrance, s.bleach_bypass, s.black_level, s.s_curve},
          .film = {s.technicolor, s.dpx_film, frame_seed, 0.0f},
      };
    }

    [[nodiscard]] const float *data() const noexcept {
      return scale_bias.data();
    }

    [[nodiscard]] static constexpr UINT register_count() noexcept {
      return sizeof(ShaderConstants) / sizeof(float[4]);
    }
  };

  static constexpr DWORD kFVF = D3DFVF_XYZRHW | D3DFVF_TEX1;

  [[nodiscard]] bool CompileShader(IDirect3DDevice9 *device) {
    if (pixel_shader_)
      return true;
    if (shader_compilation_failed_)
      return false;

    auto d3d_compile = D3DCompiler::GetCompileFunction();
    if (!d3d_compile) {
      shader_compilation_failed_ = true;
      return false;
    }

    ComPtr<ID3DBlob> code_blob;
    ComPtr<ID3DBlob> error_blob;
    const HRESULT hr = d3d_compile(kShaderSource.data(), kShaderSource.size(),
                                   nullptr, nullptr, nullptr, "main", "ps_3_0",
                                   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                   code_blob.ReleaseAndGetAddressOf(),
                                   error_blob.ReleaseAndGetAddressOf());

    if (FAILED(hr) || !code_blob) {
      shader_compilation_failed_ = true;
      return false;
    }

    const HRESULT ps_hr = device->CreatePixelShader(
        reinterpret_cast<const DWORD *>(code_blob->GetBufferPointer()),
        pixel_shader_.ReleaseAndGetAddressOf());

    return SUCCEEDED(ps_hr) && pixel_shader_;
  }

  [[nodiscard]] bool EnsureResources(Slot &s, IDirect3DDevice9 *device,
                                     UINT width, UINT height,
                                     D3DFORMAT format) {
    if (s.texture && s.surface && s.vb &&
        s.state && s.width == width && s.height == height) {
      return true;
    }

    ReleaseSlot(s);

    // 1. Create render target texture
    if (FAILED(device->CreateTexture(
            width, height, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT,
            s.texture.ReleaseAndGetAddressOf(), nullptr)) ||
        !s.texture) {
      return false;
    }

    if (FAILED(s.texture->GetSurfaceLevel(
            0, s.surface.ReleaseAndGetAddressOf())) ||
        !s.surface) {
      ReleaseSlot(s);
      return false;
    }

    // 2. Create static GPU vertex buffer for fullscreen triangle
    if (FAILED(device->CreateVertexBuffer(
            sizeof(ScreenVertex) * 3, D3DUSAGE_WRITEONLY, kFVF, D3DPOOL_DEFAULT,
            s.vb.ReleaseAndGetAddressOf(), nullptr)) ||
        !s.vb) {
      ReleaseSlot(s);
      return false;
    }

    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    const ScreenVertex triangle[3] = {
        {-0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f},
        {2.0f * w - 0.5f, -0.5f, 0.0f, 1.0f, 2.0f, 0.0f},
        {-0.5f, 2.0f * h - 0.5f, 0.0f, 1.0f, 0.0f, 2.0f},
    };

    if (ScopedVertexBufferLock lock(s.vb.Get(), 0, sizeof(triangle),
                                    0);
        lock) {
      std::memcpy(lock.data(), triangle, sizeof(triangle));
    } else {
      ReleaseSlot(s);
      return false;
    }

    // 3. Create persistent D3D9 StateBlock
    if (FAILED(device->CreateStateBlock(
            D3DSBT_ALL, s.state.ReleaseAndGetAddressOf())) ||
        !s.state) {
      ReleaseSlot(s);
      return false;
    }

    s.width = width;
    s.height = height;
    return true;
  }

  void SetupRenderStates(Slot &s, IDirect3DDevice9 *device,
                         IDirect3DSurface9 *backbuffer, UINT width,
                         UINT height) noexcept {
    device->SetRenderTarget(0, backbuffer);
    device->SetTexture(0, s.texture.Get());

    // Explicitly set viewport to cover the entire render target surface
    const D3DVIEWPORT9 vp{
        .X = 0,
        .Y = 0,
        .Width = width,
        .Height = height,
        .MinZ = 0.0f,
        .MaxZ = 1.0f,
    };
    device->SetViewport(&vp);

    // Disable programmable vertex shader so FVF (XYZRHW) fixed-function path is
    // used
    device->SetVertexShader(nullptr);

    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE,
                           D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                               D3DCOLORWRITEENABLE_BLUE |
                               D3DCOLORWRITEENABLE_ALPHA);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
  }

public:
  [[nodiscard]] static PostProcessRenderer &Instance() noexcept {
    static PostProcessRenderer instance;
    return instance;
  }

  static void ReleaseSlot(Slot &s) noexcept {
    s.state.Reset();
    s.vb.Reset();
    s.surface.Reset();
    s.texture.Reset();
    s.width = 0;
    s.height = 0;
  }

  void ReleaseResources() noexcept {
    for (auto &slot : slots_) {
      ReleaseSlot(slot);
    }
  }

  void OnReset() noexcept {
    ReleaseResources();
    pixel_shader_.Reset();
    shader_compilation_failed_ = false;
  }

  // Builds shader settings for the minimap: only brightness/saturation/tint
  // style adjustments, no AA / sharpening / vignette / grain.
  [[nodiscard]] static Settings MakeMinimapSettings() noexcept {
    Settings s{};
    s.enabled = true;
    s.fxaa_enabled = false;
    s.exposure = std::log2(std::clamp(g_minimap.brightness, 0.02f, 2.0f));
    s.contrast = g_minimap.contrast;
    s.saturation = g_minimap.saturation;
    s.color_temp = g_minimap.color_temp;
    s.gamma = g_minimap.gamma;
    s.black_level = g_minimap.black_level;
    return s;
  }

  // minimap == false: called from the main world hook.
  // minimap == true:  called from the minimap (Cartographic) render path.
  void Render(IDirect3DDevice9 *device, bool minimap) {
    if (!device || (minimap ? !g_minimap.enabled : !g_settings.enabled))
      return;

    if (device->TestCooperativeLevel() != D3D_OK)
      return;

    ComPtr<IDirect3DSurface9> target;
    if (FAILED(device->GetRenderTarget(0, target.ReleaseAndGetAddressOf())) ||
        !target) {
      return;
    }

    D3DSURFACE_DESC desc{};
    if (FAILED(target->GetDesc(&desc))) {
      return;
    }

    // Size of the real window backbuffer, used to tell the main world view
    // from smaller views (minimap).
    UINT main_w = desc.Width;
    UINT main_h = desc.Height;
    {
      ComPtr<IDirect3DSurface9> main_bb;
      D3DSURFACE_DESC main_desc{};
      if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO,
                                          main_bb.ReleaseAndGetAddressOf())) &&
          main_bb && SUCCEEDED(main_bb->GetDesc(&main_desc))) {
        main_w = main_desc.Width;
        main_h = main_desc.Height;
      }
    }

    D3DVIEWPORT9 vp{};
    device->GetViewport(&vp);

    const bool is_minimap = minimap;
    const bool own_small_target = desc.Width < main_w || desc.Height < main_h;
    const double vp_area = static_cast<double>(vp.Width) * vp.Height;
    const double rt_area = static_cast<double>(desc.Width) * desc.Height;

    if (is_minimap) {
      g_minimap_debug = {.rt_w = desc.Width, .rt_h = desc.Height,
                         .vp_x = vp.X,       .vp_y = vp.Y,
                         .vp_w = vp.Width,   .vp_h = vp.Height,
                         .hits = g_minimap_debug.hits + 1};
      // Minimap shares the full-screen target and we cannot isolate it from
      // the viewport: do nothing rather than darken the whole screen.
      if (!own_small_target && vp_area >= rt_area * 0.9)
        return;
    }

    const Settings active = is_minimap ? MakeMinimapSettings() : g_settings;

    // Region to process: whole target for the main view or an own minimap
    // target, viewport rectangle if the minimap shares the backbuffer.
    RECT rect{0, 0, static_cast<LONG>(desc.Width),
              static_cast<LONG>(desc.Height)};
    if (is_minimap && !own_small_target) {
      rect.left = std::clamp<LONG>(vp.X, 0, desc.Width);
      rect.top = std::clamp<LONG>(vp.Y, 0, desc.Height);
      rect.right = std::clamp<LONG>(vp.X + vp.Width, 0, desc.Width);
      rect.bottom = std::clamp<LONG>(vp.Y + vp.Height, 0, desc.Height);
      if (rect.right <= rect.left || rect.bottom <= rect.top)
        return;
    }

    Slot &s = slots_[is_minimap ? 1 : 0];
    if (!EnsureResources(s, device, desc.Width, desc.Height, desc.Format) ||
        !CompileShader(device)) {
      return;
    }

    // 1. Capture entire D3D9 pipeline state BEFORE any modifications
    s.state->Capture();

    // 2. Copy the already rendered region into the intermediate texture
    if (FAILED(device->StretchRect(target.Get(), &rect, s.surface.Get(), &rect,
                                   D3DTEXF_NONE))) {
      s.state->Apply();
      return;
    }

    // 3. Setup render states for the post-process blit
    SetupRenderStates(s, device, target.Get(), desc.Width, desc.Height);

    // Limit drawing to the minimap rectangle so nothing else is touched
    if (is_minimap && !own_small_target) {
      device->SetScissorRect(&rect);
      device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    }

    // 4. Precompute linear math on CPU & pack into constant buffer
    const auto constants = ShaderConstants::FromSettings(
        active, static_cast<float>(desc.Width),
        static_cast<float>(desc.Height), static_cast<float>(frame_count_++));

    device->SetPixelShader(pixel_shader_.Get());
    device->SetPixelShaderConstantF(0, constants.data(),
                                    constants.register_count());

    // 5. Draw single fullscreen triangle from GPU VRAM
    device->SetStreamSource(0, s.vb.Get(), 0, sizeof(ScreenVertex));
    device->SetFVF(kFVF);
    device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);

    // 6. Restore original pipeline state
    s.state->Apply();
  }
};

} // namespace patch::post_process
