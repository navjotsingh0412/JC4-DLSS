// JC4 DLSS (v1.0) - ReShade add-on, D3D11.
//
// Runs DLSS (DLAA, native resolution, HDR input) inside Just Cause 4's own temporal-AA compute pass,
// bypasses the game's FXAA blur, optionally hides the motion-blur effect (the game must keep its
// Motion Blur option ON so it keeps producing velocity data), and optionally sharpens the result.
//
// UI: open the ReShade overlay (Home key) -> "JC4 DLSS" tab.  Hotkey: F6 = DLSS on/off.
// Settings are saved to jc4_dlss.ini next to JustCause4.exe.  Log: jc4_dlss.log.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#define ImTextureID unsigned long long
#include <imgui.h>
#include <reshade.hpp>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_defs.h"
#include "nvsdk_ngx_params.h"
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_helpers_d3d.h"

using namespace reshade;
using namespace reshade::api;

extern "C" __declspec(dllexport) const char *NAME = "JC4 DLSS";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "DLSS (DLAA) for Just Cause 4: replaces the game's TAA/FXAA, optional sharpening and texture filtering. F6 toggles.";

namespace {

struct Settings {
	bool enabled = true;
	bool hide_motion_blur = true;
	bool bypass_fxaa = true;
	float sharpness = 0.35f;
	bool upgrade_aniso = true;      // force 16x anisotropic filtering on the game's trilinear/bilinear samplers (needs restart)
	float mip_bias = 0.0f;          // texture LOD bias added to the game's samplers (negative = sharper). Needs restart.
	bool compact = false;           // tighter panel layout
	bool explanations = true;       // show the grey help text under options
	float panel_opacity = 0.97f;    // card background opacity
	bool hud = false;               // small status strip (visible while ReShade's overlay / on-screen display is active)
} g_set;

struct CmdState {
	std::map<uint32_t, resource_view> srv;       // (stage<<8|slot)
	std::map<uint32_t, resource_view> uav;
	resource_view rtv0 = {};
};

std::mutex g_mutex;
std::unordered_map<uintptr_t, CmdState> g_state;
uint32_t g_bb_w = 0, g_bb_h = 0;
FILE *g_log = nullptr;
std::wstring g_dir;

bool g_key_prev = false;
bool g_ngx_tried = false, g_ngx_ok = false, g_failed = false;
NVSDK_NGX_Parameter *g_params = nullptr;
NVSDK_NGX_Handle *g_handle = nullptr;
ID3D11Texture2D *g_out = nullptr;
bool g_reset_next = true;
uint32_t g_taa_evals = 0, g_frames = 0, g_consec_fail = 0, g_fxaa_bypassed = 0, g_mb_bypassed = 0;
uint64_t g_pending_dst = 0, g_seq = 0, g_pending_seq = 0;
bool g_dlss_this_frame = false; double g_fps = 60.0, g_ms = 16.7;
bool g_taa_this_frame = false;   // did the game run its TAA this frame? (no TAA = pause menu / loading -> reset DLSS history afterwards)

// sharpen
bool g_sharp_tried = false, g_sharp_ok = false;
ID3D11ComputeShader *g_sharp_cs = nullptr;
ID3D11Buffer *g_sharp_cb = nullptr;
ID3D11Texture2D *g_sharp_tex = nullptr;
ID3D11UnorderedAccessView *g_sharp_uav = nullptr;

void logf(const char *fmt, ...)
{
	if (!g_log) return;
	va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
	fputc('\n', g_log); fflush(g_log);
}
void NVSDK_CONV ngx_log(const char *msg, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
	if (!g_log) return;
	size_t n = strlen(msg); fprintf(g_log, "[ngx] %s%s", msg, (n && msg[n - 1] == '\n') ? "" : "\n"); fflush(g_log);
}
std::string rstr(NVSDK_NGX_Result r) { char b[96]; snprintf(b, sizeof(b), "0x%08X (%ls)", (unsigned)r, GetNGXResultAsString(r)); return b; }

// ---- settings file -----------------------------------------------------------
std::wstring ini_path() { return g_dir + L"\\jc4_dlss.ini"; }
void load_settings()
{
	std::wstring p = ini_path();
	g_set.enabled = GetPrivateProfileIntW(L"JC4DLSS", L"Enabled", 1, p.c_str()) != 0;
	g_set.hide_motion_blur = GetPrivateProfileIntW(L"JC4DLSS", L"HideMotionBlur", 1, p.c_str()) != 0;
	g_set.bypass_fxaa = GetPrivateProfileIntW(L"JC4DLSS", L"BypassFXAA", 1, p.c_str()) != 0;
	g_set.sharpness = (float)GetPrivateProfileIntW(L"JC4DLSS", L"SharpnessPercent", 35, p.c_str()) / 100.f;
	if (g_set.sharpness < 0.f) g_set.sharpness = 0.f; if (g_set.sharpness > 1.f) g_set.sharpness = 1.f;
	g_set.upgrade_aniso = GetPrivateProfileIntW(L"JC4DLSS", L"UpgradeAniso", 1, p.c_str()) != 0;
	wchar_t mb[32] = {};
	GetPrivateProfileStringW(L"JC4DLSS", L"MipBias", L"0", mb, 32, p.c_str());
	g_set.mip_bias = (float)wcstod(mb, nullptr);
	if (g_set.mip_bias < -2.f) g_set.mip_bias = -2.f; if (g_set.mip_bias > 1.f) g_set.mip_bias = 1.f;
	g_set.compact = GetPrivateProfileIntW(L"JC4DLSS", L"Compact", 0, p.c_str()) != 0;
	g_set.explanations = GetPrivateProfileIntW(L"JC4DLSS", L"Explanations", 1, p.c_str()) != 0;
	g_set.panel_opacity = (float)GetPrivateProfileIntW(L"JC4DLSS", L"PanelOpacityPercent", 97, p.c_str()) / 100.f;
	if (g_set.panel_opacity < 0.3f) g_set.panel_opacity = 0.3f; if (g_set.panel_opacity > 1.f) g_set.panel_opacity = 1.f;
	g_set.hud = GetPrivateProfileIntW(L"JC4DLSS", L"Hud", 0, p.c_str()) != 0;
}
void save_settings()
{
	std::wstring p = ini_path(); wchar_t b[16];
	WritePrivateProfileStringW(L"JC4DLSS", L"Enabled", g_set.enabled ? L"1" : L"0", p.c_str());
	WritePrivateProfileStringW(L"JC4DLSS", L"HideMotionBlur", g_set.hide_motion_blur ? L"1" : L"0", p.c_str());
	WritePrivateProfileStringW(L"JC4DLSS", L"BypassFXAA", g_set.bypass_fxaa ? L"1" : L"0", p.c_str());
	swprintf(b, 16, L"%d", (int)(g_set.sharpness * 100.f + 0.5f));
	WritePrivateProfileStringW(L"JC4DLSS", L"SharpnessPercent", b, p.c_str());
	WritePrivateProfileStringW(L"JC4DLSS", L"UpgradeAniso", g_set.upgrade_aniso ? L"1" : L"0", p.c_str());
	swprintf(b, 16, L"%.2f", g_set.mip_bias);
	WritePrivateProfileStringW(L"JC4DLSS", L"MipBias", b, p.c_str());
	WritePrivateProfileStringW(L"JC4DLSS", L"Compact", g_set.compact ? L"1" : L"0", p.c_str());
	WritePrivateProfileStringW(L"JC4DLSS", L"Explanations", g_set.explanations ? L"1" : L"0", p.c_str());
	swprintf(b, 16, L"%d", (int)(g_set.panel_opacity * 100.f + 0.5f));
	WritePrivateProfileStringW(L"JC4DLSS", L"PanelOpacityPercent", b, p.c_str());
	WritePrivateProfileStringW(L"JC4DLSS", L"Hud", g_set.hud ? L"1" : L"0", p.c_str());
}

// ---- helpers -----------------------------------------------------------------
bool is8888(format f) { return f == format::r8g8b8a8_unorm || f == format::r8g8b8a8_unorm_srgb || f == format::r8g8b8a8_typeless; }

bool bb_tex(device *dev, resource r, format *f)
{
	if (!r.handle) return false;
	resource_desc d = dev->get_resource_desc(r);
	if (d.type != resource_type::texture_2d || d.texture.width != g_bb_w || d.texture.height != g_bb_h) return false;
	if (f) *f = d.texture.format;
	return true;
}

struct CsSaved {
	ID3D11ComputeShader *cs = nullptr; ID3D11ClassInstance *ci[256] = {}; UINT nci = 256;
	ID3D11ShaderResourceView *srv[128] = {}; ID3D11UnorderedAccessView *uav[8] = {};
	ID3D11Buffer *cb[14] = {}; ID3D11SamplerState *smp[16] = {};
};
void cs_save(ID3D11DeviceContext *c, CsSaved &s)
{
	c->CSGetShader(&s.cs, s.ci, &s.nci);
	c->CSGetShaderResources(0, 128, s.srv); c->CSGetUnorderedAccessViews(0, 8, s.uav);
	c->CSGetConstantBuffers(0, 14, s.cb); c->CSGetSamplers(0, 16, s.smp);
}
void cs_restore(ID3D11DeviceContext *c, CsSaved &s)
{
	UINT keep[8]; for (auto &k : keep) k = (UINT)-1;
	c->CSSetShader(s.cs, s.ci, s.nci);
	c->CSSetShaderResources(0, 128, s.srv); c->CSSetUnorderedAccessViews(0, 8, s.uav, keep);
	c->CSSetConstantBuffers(0, 14, s.cb); c->CSSetSamplers(0, 16, s.smp);
	if (s.cs) s.cs->Release();
	for (UINT i = 0; i < s.nci; ++i) if (s.ci[i]) s.ci[i]->Release();
	for (auto *p : s.srv) if (p) p->Release();
	for (auto *p : s.uav) if (p) p->Release();
	for (auto *p : s.cb) if (p) p->Release();
	for (auto *p : s.smp) if (p) p->Release();
}

// ---- NGX ---------------------------------------------------------------------
bool ensure_ngx(command_list *cl, device *rdev)
{
	if (g_ngx_ok) return true;
	if (g_ngx_tried) return false;
	g_ngx_tried = true;
	ID3D11Device *dev = (ID3D11Device *)rdev->get_native();
	ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)cl->get_native();
	if (ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) { logf("TAA dispatch is on a deferred context: unsupported"); return false; }

	std::wstring data = g_dir + L"\\ngx_data";
	CreateDirectoryW(data.c_str(), nullptr);
	const wchar_t *paths[1] = { g_dir.c_str() };
	NVSDK_NGX_FeatureCommonInfo info = {};
	info.PathListInfo.Path = paths; info.PathListInfo.Length = 1;
	info.LoggingInfo.LoggingCallback = ngx_log;
	info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
	info.LoggingInfo.DisableOtherLoggingSinks = true;
	NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID("a0b1c2d3-1111-2222-3333-444455556666", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", data.c_str(), dev, &info);
	logf("NGX Init -> %s", rstr(r).c_str());
	if (NVSDK_NGX_FAILED(r)) return false;
	r = NVSDK_NGX_D3D11_AllocateParameters(&g_params);
	if (NVSDK_NGX_FAILED(r) || !g_params) { logf("AllocateParameters -> %s", rstr(r).c_str()); return false; }
	NVSDK_NGX_DLSS_Create_Params cp = {};
	cp.Feature.InWidth = g_bb_w; cp.Feature.InHeight = g_bb_h;
	cp.Feature.InTargetWidth = g_bb_w; cp.Feature.InTargetHeight = g_bb_h;
	cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
	cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
	r = NGX_D3D11_CREATE_DLSS_EXT(ctx, &g_handle, g_params, &cp);
	logf("CreateFeature DLAA HDR %ux%u -> %s", g_bb_w, g_bb_h, rstr(r).c_str());
	if (NVSDK_NGX_FAILED(r) || !g_handle) return false;

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = g_bb_w; td.Height = g_bb_h; td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R11G11B10_FLOAT; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	HRESULT hr = dev->CreateTexture2D(&td, nullptr, &g_out);
	if (FAILED(hr)) { logf("create output texture failed 0x%08X", (unsigned)hr); return false; }
	g_ngx_ok = true;
	return true;
}

// ---- sharpen (contrast-adaptive, applied on the final LDR image) --------------
const char *kSharpHLSL = R"HLSL(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
cbuffer C : register(b0) { float sharp; uint w; uint h; uint pad; };
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= w || id.y >= h) return;
	int2 p = int2(id.xy); int2 mxp = int2(w - 1, h - 1);
	float4 c4 = src.Load(int3(p, 0));
	float3 c = c4.rgb;
	float3 a = src.Load(int3(clamp(p + int2(0, -1), 0, mxp), 0)).rgb;
	float3 b = src.Load(int3(clamp(p + int2(-1, 0), 0, mxp), 0)).rgb;
	float3 d = src.Load(int3(clamp(p + int2(1, 0), 0, mxp), 0)).rgb;
	float3 e = src.Load(int3(clamp(p + int2(0, 1), 0, mxp), 0)).rgb;
	float3 mn = min(min(min(a, b), min(d, e)), c);
	float3 mx = max(max(max(a, b), max(d, e)), c);
	float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
	float peak = -1.0 / lerp(8.0, 5.0, saturate(sharp));
	float3 wgt = amp * peak;
	float3 rcpW = 1.0 / (1.0 + 4.0 * wgt);
	float3 o = saturate((a * wgt + b * wgt + d * wgt + e * wgt + c) * rcpW);
	dst[id.xy] = float4(o, c4.a);
}
)HLSL";

bool ensure_sharpen(ID3D11Device *dev)
{
	if (g_sharp_ok) return true;
	if (g_sharp_tried) return false;
	g_sharp_tried = true;
	ID3DBlob *code = nullptr, *err = nullptr;
	HRESULT hr = D3DCompile(kSharpHLSL, strlen(kSharpHLSL), "jc4_sharpen", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err);
	if (FAILED(hr)) { logf("sharpen shader compile failed: %s", err ? (const char *)err->GetBufferPointer() : "?"); if (err) err->Release(); return false; }
	hr = dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_sharp_cs);
	code->Release(); if (err) err->Release();
	if (FAILED(hr)) { logf("CreateComputeShader failed 0x%08X", (unsigned)hr); return false; }
	D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_sharp_cb))) return false;
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = g_bb_w; td.Height = g_bb_h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_sharp_tex))) return false;
	if (FAILED(dev->CreateUnorderedAccessView(g_sharp_tex, nullptr, &g_sharp_uav))) return false;
	g_sharp_ok = true;
	logf("sharpen shader ready");
	return true;
}

// sharpen `src` (R8G8B8A8 typeless/unorm, bb size) into g_sharp_tex
bool run_sharpen(ID3D11DeviceContext *ctx, ID3D11Device *dev, ID3D11Resource *src)
{
	if (!ensure_sharpen(dev)) return false;
	D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
	ID3D11ShaderResourceView *srv = nullptr;
	if (FAILED(dev->CreateShaderResourceView(src, &sd, &srv))) return false;
	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(ctx->Map(g_sharp_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { srv->Release(); return false; }
	struct { float s; uint32_t w, h, p; } cbv = { g_set.sharpness, g_bb_w, g_bb_h, 0 };
	memcpy(m.pData, &cbv, sizeof(cbv)); ctx->Unmap(g_sharp_cb, 0);
	CsSaved saved; cs_save(ctx, saved);
	ID3D11ShaderResourceView *none[1] = { nullptr };
	ctx->CSSetShader(g_sharp_cs, nullptr, 0);
	ctx->CSSetShaderResources(0, 1, &srv); ctx->CSSetConstantBuffers(0, 1, &g_sharp_cb);
	UINT keep = (UINT)-1; ctx->CSSetUnorderedAccessViews(0, 1, &g_sharp_uav, &keep);
	ctx->Dispatch((g_bb_w + 7) / 8, (g_bb_h + 7) / 8, 1);
	ID3D11UnorderedAccessView *nu[1] = { nullptr };
	ctx->CSSetUnorderedAccessViews(0, 1, nu, &keep); ctx->CSSetShaderResources(0, 1, none);
	cs_restore(ctx, saved);
	srv->Release();
	return true;
}

// ---- texture filtering: upgrade samplers to anisotropic, optional LOD bias (applies to samplers created after load) ----
uint32_t g_smp_total = 0, g_smp_aniso = 0, g_smp_biased = 0;
bool on_create_sampler(device *, sampler_desc &desc)
{
	++g_smp_total;
	uint32_t f = (uint32_t)desc.filter;
	if (f & 0x80) return false;                         // comparison (shadow) samplers: never touch
	uint32_t base = f & 0x7F;
	bool changed = false;
	bool mips = desc.max_lod > 0.5f;
	if (g_set.upgrade_aniso && mips && (base == 0x15 || base == 0x14)) {   // trilinear / bilinear+mip-point -> anisotropic
		desc.filter = filter_mode::anisotropic;
		if (desc.max_anisotropy < 16.f) desc.max_anisotropy = 16.f;
		++g_smp_aniso; changed = true;
	}
	bool linearish = (base == 0x15 || base == 0x14 || base == 0x55 || base == 0x54 || base == 0x11 || base == 0x10);
	if (g_set.mip_bias != 0.f && mips && linearish) { desc.mip_lod_bias += g_set.mip_bias; ++g_smp_biased; changed = true; }
	return changed;
}

// ---- the hooks ---------------------------------------------------------------
bool active() { return g_set.enabled && !g_failed; }

void flush_pending(command_list *cl)
{
	if (!g_pending_dst || g_seq <= g_pending_seq) return;
	((ID3D11DeviceContext *)cl->get_native())->CopyResource((ID3D11Resource *)g_pending_dst, g_out);
	g_pending_dst = 0;
}

bool on_dispatch(command_list *cl, uint32_t gx, uint32_t gy, uint32_t gz)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	++g_seq;
	flush_pending(cl);
	if (!g_bb_w || !active() || gz != 1) return false;
	if (gx != (g_bb_w + 7) / 8 || gy != (g_bb_h + 7) / 8) return false;

	CmdState &s = g_state[(uintptr_t)cl];
	device *dev = cl->get_device();
	auto get_srv = [&](uint32_t slot, format *f) -> resource {
		auto it = s.srv.find((0x20u << 8) | slot);
		if (it == s.srv.end()) return resource{ 0 };
		resource r = dev->get_resource_from_view(it->second);
		return bb_tex(dev, r, f) ? r : resource{ 0 };
	};
	format fc = format::unknown, fd = format::unknown, fv = format::unknown, fo = format::unknown;
	resource color = get_srv(0, &fc), depth = get_srv(2, &fd), vel = get_srv(4, &fv);
	if (!color.handle || !depth.handle || !vel.handle) return false;
	if (fc != format::r11g11b10_float || fd != format::r32_float || fv != format::r16g16_float) return false;
	auto u0 = s.uav.find((0x20u << 8) | 0);
	if (u0 == s.uav.end()) return false;
	resource out = dev->get_resource_from_view(u0->second);
	if (!bb_tex(dev, out, &fo) || fo != format::r11g11b10_float) return false;

	++g_taa_evals; g_taa_this_frame = true;
	if (g_taa_evals == 1) logf("TAA dispatch identified (%ux%u groups)", gx, gy);
	if (!ensure_ngx(cl, dev)) { g_failed = true; logf("NGX setup failed: leaving the game's own AA untouched"); return false; }

	ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)cl->get_native();
	// game jitter alternates +-0.25 px on both axes; game velocity = current - previous (UV)
	float j = (g_taa_evals & 1) ? 0.25f : -0.25f;
	NVSDK_NGX_D3D11_DLSS_Eval_Params ep = {};
	ep.Feature.pInColor = (ID3D11Resource *)color.handle;
	ep.Feature.pInOutput = g_out;
	ep.pInDepth = (ID3D11Resource *)depth.handle;
	ep.pInMotionVectors = (ID3D11Resource *)vel.handle;
	ep.InJitterOffsetX = j; ep.InJitterOffsetY = j;
	ep.InRenderSubrectDimensions.Width = g_bb_w; ep.InRenderSubrectDimensions.Height = g_bb_h;
	ep.InReset = g_reset_next ? 1 : 0;
	ep.InMVScaleX = -(float)g_bb_w; ep.InMVScaleY = -(float)g_bb_h;

	CsSaved saved; cs_save(ctx, saved);
	NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, g_handle, g_params, &ep);
	cs_restore(ctx, saved);
	if (g_taa_evals <= 2 || NVSDK_NGX_FAILED(r)) logf("Evaluate #%u -> %s", g_taa_evals, rstr(r).c_str());
	if (NVSDK_NGX_FAILED(r)) {
		g_dlss_this_frame = false;
		if (++g_consec_fail >= 3) { g_failed = true; logf("3 failures in a row: DLSS disabled for this session"); }
		return false;
	}
	g_consec_fail = 0; g_reset_next = false; g_dlss_this_frame = true;
	g_pending_dst = out.handle; g_pending_seq = g_seq;   // overwrite the game's TAA result right after its dispatch
	return false;
}

// Game's motion-blur composite: pass the image through (the game still produced velocity before this).
bool maybe_bypass_motion_blur(command_list *cl, uint32_t vcount)
{
	if (vcount != 3 || !active() || !g_set.hide_motion_blur || !g_ngx_ok) return false;
	CmdState &s = g_state[(uintptr_t)cl];
	if (!s.rtv0.handle) return false;
	auto c = s.srv.find((0x10u << 8) | 2), n4 = s.srv.find((0x10u << 8) | 4), v5 = s.srv.find((0x10u << 8) | 5);
	if (c == s.srv.end() || n4 == s.srv.end() || v5 == s.srv.end()) return false;
	device *dev = cl->get_device();
	format fc = format::unknown, fv = format::unknown, frt = format::unknown;
	resource rc = dev->get_resource_from_view(c->second), rv = dev->get_resource_from_view(v5->second), rt = dev->get_resource_from_view(s.rtv0);
	if (!bb_tex(dev, rc, &fc) || !is8888(fc)) return false;
	if (!bb_tex(dev, rv, &fv) || fv != format::r8g8b8a8_unorm) return false;
	if (!bb_tex(dev, rt, &frt) || frt != format::r8g8b8a8_typeless || fc != format::r8g8b8a8_typeless) return false;
	((ID3D11DeviceContext *)cl->get_native())->CopyResource((ID3D11Resource *)rt.handle, (ID3D11Resource *)rc.handle);
	if (++g_mb_bypassed == 1) logf("motion-blur pass bypassed");
	return true;
}

// Game's final FXAA pass: while DLSS is active, pass the image through (optionally sharpened).
bool maybe_bypass_fxaa(command_list *cl, uint32_t vcount)
{
	if (!g_dlss_this_frame || vcount != 3 || !active() || !g_set.bypass_fxaa) return false;
	CmdState &s = g_state[(uintptr_t)cl];
	if (!s.rtv0.handle) return false;
	auto ps5 = s.srv.find((0x10u << 8) | 5), ps0 = s.srv.find((0x10u << 8) | 0);
	if (ps5 == s.srv.end() || ps0 == s.srv.end()) return false;
	device *dev = cl->get_device();
	format f5 = format::unknown, f0 = format::unknown, frt = format::unknown;
	resource v = dev->get_resource_from_view(ps5->second), c = dev->get_resource_from_view(ps0->second), rt = dev->get_resource_from_view(s.rtv0);
	if (!bb_tex(dev, v, &f5) || f5 != format::r16g16_float) return false;
	if (!bb_tex(dev, c, &f0) || !is8888(f0)) return false;
	if (!bb_tex(dev, rt, &frt) || frt != format::r8g8b8a8_unorm) return false;
	ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)cl->get_native();
	ID3D11Device *nd = (ID3D11Device *)dev->get_native();
	if (g_set.sharpness > 0.01f && run_sharpen(ctx, nd, (ID3D11Resource *)c.handle))
		ctx->CopyResource((ID3D11Resource *)rt.handle, g_sharp_tex);
	else
		ctx->CopyResource((ID3D11Resource *)rt.handle, (ID3D11Resource *)c.handle);
	if (++g_fxaa_bypassed == 1) logf("FXAA pass bypassed");
	g_dlss_this_frame = false;
	return true;
}

// ---- events ------------------------------------------------------------------
void on_init_swapchain(swapchain *sc, bool)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	resource_desc d = sc->get_device()->get_resource_desc(sc->get_back_buffer(0));
	g_bb_w = d.texture.width; g_bb_h = d.texture.height;
}
void on_bind_rt(command_list *cl, uint32_t count, const resource_view *rtvs, resource_view)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_state[(uintptr_t)cl].rtv0 = count ? rtvs[0] : resource_view{ 0 };
}
void on_push_descriptors(command_list *cl, shader_stage stages, pipeline_layout, uint32_t, const descriptor_table_update &u)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	CmdState &s = g_state[(uintptr_t)cl];
	uint32_t st = (uint32_t)stages & 0xFFFFFF;
	for (uint32_t i = 0; i < u.count; ++i) {
		uint32_t key = (st << 8) | ((u.binding + i) & 255);
		if (u.type == descriptor_type::shader_resource_view || u.type == descriptor_type::buffer_shader_resource_view) {
			resource_view v = static_cast<const resource_view *>(u.descriptors)[i];
			if (v.handle) s.srv[key] = v; else s.srv.erase(key);
		} else if (u.type == descriptor_type::unordered_access_view || u.type == descriptor_type::buffer_unordered_access_view) {
			resource_view v = static_cast<const resource_view *>(u.descriptors)[i];
			if (v.handle) s.uav[key] = v; else s.uav.erase(key);
		}
	}
}
bool on_draw(command_list *cl, uint32_t vc, uint32_t, uint32_t, uint32_t)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	++g_seq; flush_pending(cl);
	if (maybe_bypass_motion_blur(cl, vc)) return true;
	return maybe_bypass_fxaa(cl, vc);
}
bool on_draw_indexed(command_list *cl, uint32_t, uint32_t, uint32_t, int32_t, uint32_t)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	++g_seq; flush_pending(cl);
	return false;
}

void on_present(command_queue *, swapchain *, const rect *, const rect *, uint32_t, const rect *)
{
	bool k6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
	bool beep2 = false, beep1 = false;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		++g_frames; g_dlss_this_frame = false;
		{
			LARGE_INTEGER now, fq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
			static LARGE_INTEGER last = {};
			if (last.QuadPart) { double ms = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)fq.QuadPart; if (ms > 0.01 && ms < 1000.0) { g_ms = g_ms * 0.9 + ms * 0.1; g_fps = 1000.0 / g_ms; } }
			last = now;
		}
		if (!g_taa_this_frame) g_reset_next = true;   // paused, in a menu or loading: start DLSS history fresh afterwards
		g_taa_this_frame = false;
		if (k6 && !g_key_prev) { g_set.enabled = !g_set.enabled; g_reset_next = true; save_settings(); logf("F6: DLSS %s", g_set.enabled ? "ON" : "OFF"); (g_set.enabled ? beep2 : beep1) = true; }
		g_key_prev = k6;
		if (g_frames % 1800 == 0) logf("status: frames=%u dlss_frames=%u fxaa_bypassed=%u mb_bypassed=%u enabled=%d failed=%d samplers=%u/%u/%u", g_frames, g_taa_evals, g_fxaa_bypassed, g_mb_bypassed, (int)g_set.enabled, (int)g_failed, g_smp_total, g_smp_aniso, g_smp_biased);
	}
	if (beep1) Beep(700, 80);
	if (beep2) { Beep(900, 70); Beep(900, 70); }
}

// ---- overlay UI -------------------------------------------------------------------
float g_launch_bias = 0.f;
bool g_launch_aniso = true;

void tip(const char *text)
{
	if (ImGui::IsItemHovered(0)) { ImGui::BeginTooltip(); ImGui::TextUnformatted(text, nullptr); ImGui::EndTooltip(); }
}

void note(const char *text, const ImVec4 &grey)
{
	if (!g_set.explanations) return;
	ImGui::PushStyleColor(ImGuiCol_Text, grey);
	ImGui::TextWrapped("%s", text);
	ImGui::PopStyleColor(1);
}

// small status strip, drawn when ReShade builds an overlay frame (overlay open or its on-screen display active)
void on_reshade_overlay(effect_runtime *)
{
	if (!g_set.hud) return;
	bool on = g_set.enabled && g_ngx_ok && !g_failed;
	char buf[96];
	snprintf(buf, sizeof(buf), "%s    %.0f fps    %.1f ms", on ? "DLSS on" : "DLSS off", g_fps, g_ms);
	ImVec2 ts = ImGui::CalcTextSize(buf, nullptr, false, -1.f);
	ImGuiIO &io = ImGui::GetIO();
	ImVec2 p0(io.DisplaySize.x - ts.x - 26.f, 10.f), p1(io.DisplaySize.x - 10.f, 10.f + ts.y + 10.f);
	ImDrawList *dl = ImGui::GetForegroundDrawList(nullptr);
	dl->AddRectFilled(p0, p1, IM_COL32(10, 12, 16, 190), 6.f, 0);
	dl->AddText(ImVec2(p0.x + 8.f, p0.y + 5.f), on ? IM_COL32(61, 220, 132, 255) : IM_COL32(160, 165, 178, 255), buf, nullptr);
}

void draw_ui_full(effect_runtime *)
{
	const ImVec4 accent(0.31f, 0.55f, 1.00f, 1.f), accent_h(0.42f, 0.63f, 1.00f, 1.f), accent_a(0.25f, 0.47f, 0.92f, 1.f);
	const ImVec4 green(0.24f, 0.86f, 0.52f, 1.f), amber(1.00f, 0.75f, 0.24f, 1.f), red(1.00f, 0.40f, 0.30f, 1.f), grey(0.60f, 0.63f, 0.69f, 1.f);
	const ImVec4 card(0.114f, 0.125f, 0.157f, g_set.panel_opacity), frame(0.150f, 0.165f, 0.210f, 1.f), frame_h(0.200f, 0.220f, 0.285f, 1.f);
	const float gap = g_set.compact ? 3.f : 6.f;
	bool changed = false;

	ImGui::PushStyleColor(ImGuiCol_ChildBg, card);
	ImGui::PushStyleColor(ImGuiCol_FrameBg, frame);
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, frame_h);
	ImGui::PushStyleColor(ImGuiCol_FrameBgActive, frame_h);
	ImGui::PushStyleColor(ImGuiCol_CheckMark, accent);
	ImGui::PushStyleColor(ImGuiCol_SliderGrab, accent);
	ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, accent_h);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
	ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.f);
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.f);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f, gap));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f, g_set.compact ? 6.f : 9.f));

	// --- status pill + master switch ---
	ImVec4 pill = green; const char *label = "Active";
	if (g_failed) { pill = red; label = "Stopped"; }
	else if (!g_set.enabled) { pill = grey; label = "Off"; }
	else if (!g_ngx_ok) { pill = amber; label = "Waiting"; }
	{
		ImVec4 bg(pill.x * 0.26f, pill.y * 0.26f, pill.z * 0.26f, 1.f);
		ImGui::PushStyleColor(ImGuiCol_Button, bg); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg); ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg);
		ImGui::PushStyleColor(ImGuiCol_Text, pill);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 11.f);
		ImGui::Button(label, ImVec2(84.f, 0.f));
		ImGui::PopStyleVar(1); ImGui::PopStyleColor(4);
	}
	ImGui::SameLine(0.f, 10.f);
	if (g_failed) ImGui::TextColored(red, "see jc4_dlss.log");
	else if (!g_set.enabled) ImGui::TextColored(grey, "game's own anti-aliasing");
	else if (!g_ngx_ok) ImGui::TextColored(amber, "load into the world");
	else ImGui::TextColored(grey, "DLAA  %ux%u", g_bb_w, g_bb_h);
	ImGui::SameLine(0.f, 18.f);
	if (ImGui::Checkbox("Enable DLSS", &g_set.enabled)) { changed = true; g_reset_next = true; }
	tip("Turns the whole mod on or off. Off brings back the game's normal image. Hotkey: F6.");
	ImGui::Spacing();

	// --- quick look ---
	ImGui::BeginChild("jc4_card_quick", ImVec2(0.f, 0.f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY, 0);
	ImGui::TextColored(grey, "QUICK LOOK");
	{
		float avail = ImGui::GetContentRegionAvail().x, bw = (avail - 12.f) / 3.f;
		const char *names[3] = { "Natural", "Balanced", "Crisp" };
		const float vals[3] = { 0.15f, 0.35f, 0.60f };
		const char *tips[3] = { "Soft and film-like. Sharpness 15%.", "Recommended. Sharpness 35%.", "Very sharp. Sharpness 60%. Can look harsh on some scenes." };
		for (int i = 0; i < 3; ++i) {
			bool on = std::fabs(g_set.sharpness - vals[i]) < 0.01f;
			if (on) {
				ImGui::PushStyleColor(ImGuiCol_Button, accent); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accent_h);
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, accent_a); ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.04f, 0.07f, 0.13f, 1.f));
			} else {
				ImGui::PushStyleColor(ImGuiCol_Button, frame); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, frame_h); ImGui::PushStyleColor(ImGuiCol_ButtonActive, frame_h);
			}
			if (ImGui::Button(names[i], ImVec2(bw, 0.f))) { g_set.sharpness = vals[i]; changed = true; }
			ImGui::PopStyleColor(on ? 4 : 3);
			tip(tips[i]);
			if (i < 2) ImGui::SameLine(0.f, 6.f);
		}
	}
	ImGui::EndChild();
	if (!g_set.compact) ImGui::Spacing();

	// --- image ---
	ImGui::BeginChild("jc4_card_image", ImVec2(0.f, 0.f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY, 0);
	ImGui::TextColored(grey, "IMAGE");
	{
		float pct = g_set.sharpness * 100.f;
		ImGui::SetNextItemWidth(-110.f);
		if (ImGui::SliderFloat("Sharpness", &pct, 0.f, 100.f, "%.0f%%", 0)) { g_set.sharpness = pct / 100.f; changed = true; }
		tip("A light sharpening step applied after DLSS. 0 = off.\nOnly active while 'Replace the game's FXAA blur' is ticked.");
	}
	ImGui::EndChild();
	if (!g_set.compact) ImGui::Spacing();

	// --- compatibility ---
	ImGui::BeginChild("jc4_card_compat", ImVec2(0.f, 0.f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY, 0);
	ImGui::TextColored(grey, "GAME COMPATIBILITY");
	if (ImGui::Checkbox("Hide the game's motion blur", &g_set.hide_motion_blur)) changed = true;
	tip("Removes the motion-blur look.");
	note("Keep the game's own Motion Blur option ON: it supplies the motion data DLSS needs.", grey);
	if (ImGui::Checkbox("Replace the game's FXAA blur", &g_set.bypass_fxaa)) changed = true;
	tip("The game finishes with a blurry FXAA pass. With DLSS active it is not needed.");
	ImGui::EndChild();
	if (!g_set.compact) ImGui::Spacing();

	// --- textures ---
	ImGui::BeginChild("jc4_card_tex", ImVec2(0.f, 0.f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY, 0);
	ImGui::TextColored(grey, "TEXTURES");
	if (ImGui::Checkbox("Force 16x anisotropic filtering", &g_set.upgrade_aniso)) changed = true;
	tip("Sharper textures on slopes and at a distance (terrain, roads).");
	ImGui::SetNextItemWidth(-150.f);
	if (ImGui::SliderFloat("Texture detail", &g_set.mip_bias, -1.5f, 0.5f, "%.2f", 0)) changed = true;
	tip("LOD bias. 0 = game default. -0.5 to -1 = sharper textures (NVIDIA suggests about -1 with DLSS).\nToo low can add shimmer.");
	bool pending = (g_set.upgrade_aniso != g_launch_aniso) || (std::fabs(g_set.mip_bias - g_launch_bias) > 0.001f);
	if (pending) ImGui::TextColored(amber, "Restart the game to apply the texture changes.");
	else note("Texture changes take effect after a game restart.", grey);
	ImGui::EndChild();
	if (!g_set.compact) ImGui::Spacing();

	// --- display ---
	ImGui::BeginChild("jc4_card_display", ImVec2(0.f, 0.f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY, 0);
	ImGui::TextColored(grey, "DISPLAY");
	if (ImGui::Checkbox("Compact layout", &g_set.compact)) changed = true;
	if (ImGui::Checkbox("Show explanations", &g_set.explanations)) changed = true;
	if (ImGui::Checkbox("On-screen status strip", &g_set.hud)) changed = true;
	tip("A small 'DLSS on / fps' strip in the top-right corner.\nReShade only lets add-ons draw while its overlay or its own on-screen display\n(for example its FPS counter) is active, so turn that on to keep the strip visible while playing.");
	{
		float pct = g_set.panel_opacity * 100.f;
		ImGui::SetNextItemWidth(-130.f);
		if (ImGui::SliderFloat("Panel opacity", &pct, 30.f, 100.f, "%.0f%%", 0)) { g_set.panel_opacity = pct / 100.f; changed = true; }
	}
	ImGui::EndChild();
	if (!g_set.compact) ImGui::Spacing();

	// --- diagnostics / reset ---
	if (ImGui::CollapsingHeader("Diagnostics", 0)) {
		ImGui::Text("Frames processed by DLSS: %u", g_taa_evals);
		ImGui::Text("FXAA passes replaced: %u   motion-blur passes hidden: %u", g_fxaa_bypassed, g_mb_bypassed);
		ImGui::Text("Texture samplers: %u seen, %u made anisotropic, %u biased", g_smp_total, g_smp_aniso, g_smp_biased);
		ImGui::TextColored(grey, "Settings: jc4_dlss.ini     Log: jc4_dlss.log");
	}
	if (ImGui::Button("Reset all settings", ImVec2(150.f, 0.f))) {
		g_set.enabled = true; g_set.hide_motion_blur = true; g_set.bypass_fxaa = true; g_set.sharpness = 0.35f;
		g_set.upgrade_aniso = true; g_set.mip_bias = 0.f; g_set.compact = false; g_set.explanations = true; g_set.panel_opacity = 0.97f; g_set.hud = false;
		g_reset_next = true; changed = true;
	}
	tip("Back to the recommended defaults.");
	ImGui::SameLine(0.f, 12.f);
	ImGui::TextColored(grey, "v1.5");

	ImGui::PopStyleVar(6);
	ImGui::PopStyleColor(7);
	if (changed) save_settings();
}

// Compact entry shown inside ReShade's "Add-ons" tab; the full controls live in the "JC4 DLSS" tab.
void draw_ui_addons(effect_runtime *)
{
	const ImVec4 green(0.35f, 0.85f, 0.45f, 1.f), amber(1.f, 0.75f, 0.25f, 1.f), red(1.f, 0.4f, 0.3f, 1.f), grey(0.62f, 0.62f, 0.62f, 1.f);
	if (g_failed) ImGui::TextColored(red, "STATUS: DLSS stopped - see jc4_dlss.log");
	else if (!g_set.enabled) ImGui::TextColored(grey, "STATUS: Off - the game's own anti-aliasing is in use");
	else if (!g_ngx_ok) ImGui::TextColored(amber, "STATUS: Waiting - load into the world");
	else ImGui::TextColored(green, "STATUS: Active - DLSS (DLAA, native resolution)");
	if (ImGui::Checkbox("Enable DLSS", &g_set.enabled)) { g_reset_next = true; save_settings(); }
	ImGui::TextDisabled("All settings are in the \"JC4 DLSS\" tab. If it floats, drag its tab into the tab bar once.");
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
	switch (reason) {
	case DLL_PROCESS_ATTACH: {
		if (!reshade::register_addon(hModule)) return FALSE;
		wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH);
		std::wstring s(path); size_t sl = s.find_last_of(L"\\/");
		g_dir = sl == std::wstring::npos ? L"." : s.substr(0, sl);
		g_log = _wfopen((g_dir + L"\\jc4_dlss.log").c_str(), L"w");
		logf("JC4 DLSS v1.5 loaded (aniso upgrade %d, mip bias %.2f)", (int)g_set.upgrade_aniso, g_set.mip_bias);
		load_settings();
		g_launch_aniso = g_set.upgrade_aniso; g_launch_bias = g_set.mip_bias;
		reshade::register_overlay("JC4 DLSS", draw_ui_full);   // its own tab with all the controls
		reshade::register_overlay(nullptr, draw_ui_addons);    // short entry in the Add-ons tab
		reshade::register_event<addon_event::init_swapchain>(on_init_swapchain);
		reshade::register_event<addon_event::bind_render_targets_and_depth_stencil>(on_bind_rt);
		reshade::register_event<addon_event::push_descriptors>(on_push_descriptors);
		reshade::register_event<addon_event::draw>(on_draw);
		reshade::register_event<addon_event::draw_indexed>(on_draw_indexed);
		reshade::register_event<addon_event::dispatch>(on_dispatch);
		reshade::register_event<addon_event::reshade_overlay>(on_reshade_overlay);
		reshade::register_event<addon_event::create_sampler>(on_create_sampler);
		reshade::register_event<addon_event::present>(on_present);
		break; }
	case DLL_PROCESS_DETACH:
		reshade::unregister_addon(hModule);
		break;
	}
	return TRUE;
}
