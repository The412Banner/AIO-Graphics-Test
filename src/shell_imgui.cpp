// AIO Graphics Test - Dear ImGui single-window shell (Phase 0 de-risk scaffold).
//
// Purpose: prove the C++ / Dear ImGui / Direct3D 11 path builds green in CI and
// launches under the FEX / arm64ec Proton-9 container, where mingw C++ codegen has
// previously hit illegal-instruction (c000001d) crashes. It is deliberately
// self-contained (one .cpp) and additive: reachable ONLY via the --imgui flag.
//
// Renderer notes (repo lesson): d3d11.dll and dxgi are resolved DYNAMICALLY so the
// exe still LAUNCHES on a container without DXVK (it just reports D3D11 missing).
// We never import D3D11CreateDeviceAndSwapChain at link time. Dear ImGui's DX11
// backend needs D3DCompile() to build its own shaders; rather than statically link
// -ld3dcompiler (which would add a load-time dependency on d3dcompiler_47.dll and
// defeat the "must launch without the DXVK DLL set" rule), we provide a forwarding
// D3DCompile below that lazily LoadLibrary()s the real compiler at first use. That
// keeps d3d11 / dxgi / d3dcompiler ALL dynamic; the only extra static import ImGui's
// Win32 backend adds is imm32 (a core DLL always present under Wine).

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <cstdint>
#include <cstdio>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include "shell_imgui.h"

// ---------------------------------------------------------------------------
// D3DCompile forwarding shim (keeps d3dcompiler dynamic - see header comment).
// The mingw-w64 d3dcompiler.h prototype is a plain (non-dllimport) declaration,
// so this definition satisfies imgui_impl_dx11.o's reference to D3DCompile and we
// link NO import lib for it. The real compiler is loaded lazily at first call.
// ---------------------------------------------------------------------------
extern "C" HRESULT WINAPI D3DCompile(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
                                     const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude,
                                     LPCSTR pEntrypoint, LPCSTR pTarget, UINT Flags1, UINT Flags2,
                                     ID3DBlob **ppCode, ID3DBlob **ppErrorMsgs) {
    static pD3DCompile s_fn = nullptr;
    static bool s_tried = false;
    if (!s_tried) {
        s_tried = true;
        const char *dlls[] = {"d3dcompiler_47.dll", "d3dcompiler_46.dll", "d3dcompiler_43.dll"};
        for (int i = 0; i < 3 && !s_fn; ++i) {
            HMODULE h = LoadLibraryA(dlls[i]);
            if (h) s_fn = (pD3DCompile)GetProcAddress(h, "D3DCompile");
        }
    }
    if (!s_fn) {
        if (ppCode) *ppCode = nullptr;
        if (ppErrorMsgs) *ppErrorMsgs = nullptr;
        return E_NOTIMPL;
    }
    return s_fn(pSrcData, SrcDataSize, pSourceName, pDefines, pInclude, pEntrypoint, pTarget, Flags1,
                Flags2, ppCode, ppErrorMsgs);
}

// ImGui's Win32 backend WndProc handler (declared in imgui_impl_win32.h).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

// ---------------------------------------------------------------------------
// Direct3D 11 device / swapchain (created via dynamic d3d11.dll load).
// ---------------------------------------------------------------------------
typedef HRESULT(WINAPI *PFN_D3D11CreateDeviceAndSwapChain_t)(
    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
    const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *,
    ID3D11DeviceContext **);

static ID3D11Device *g_dev = nullptr;
static ID3D11DeviceContext *g_ctx = nullptr;
static IDXGISwapChain *g_swap = nullptr;
static ID3D11RenderTargetView *g_rtv = nullptr;

static void create_rtv() {
    ID3D11Texture2D *back = nullptr;
    g_swap->GetBuffer(0, IID_ID3D11Texture2D, (void **)&back);
    if (back) {
        g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

static void release_rtv() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

// Returns true on success. On failure the caller shows a message and exits cleanly
// (the whole point of the scaffold is that a missing DXVK does not crash the exe).
static bool create_device(HWND hwnd) {
    HMODULE d3d11 = LoadLibraryA("d3d11.dll");
    PFN_D3D11CreateDeviceAndSwapChain_t create =
        d3d11 ? (PFN_D3D11CreateDeviceAndSwapChain_t)GetProcAddress(d3d11,
                                                                    "D3D11CreateDeviceAndSwapChain")
              : nullptr;
    if (!create) return false;

    DXGI_SWAP_CHAIN_DESC scd;
    ZeroMemory(&scd, sizeof(scd));
    scd.BufferCount = 2;
    scd.BufferDesc.Width = 0;
    scd.BufferDesc.Height = 0;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                      D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want,
                        (UINT)(sizeof(want) / sizeof(want[0])), D3D11_SDK_VERSION, &scd, &g_swap,
                        &g_dev, &got, &g_ctx);
    if (FAILED(hr)) return false;
    create_rtv();
    return true;
}

static void destroy_device() {
    release_rtv();
    if (g_swap) {
        g_swap->Release();
        g_swap = nullptr;
    }
    if (g_ctx) {
        g_ctx->Release();
        g_ctx = nullptr;
    }
    if (g_dev) {
        g_dev->Release();
        g_dev = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Window proc.
// ---------------------------------------------------------------------------
static LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (g_dev && wp != SIZE_MINIMIZED) {
                release_rtv();
                g_swap->ResizeBuffers(0, (UINT)LOWORD(lp), (UINT)HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
                create_rtv();
            }
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                PostMessage(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// The scaffold UI. Foreshadows the target mockup: a top toolbar with a List/Grid
// segmented control (visual stub), a right-docked test list, and a left viewport
// with a clear-color fill + an overlay reading "Direct3D 11  -  NNNN FPS" and a
// real frametime sparkline.
// ---------------------------------------------------------------------------
static const char *k_tests[] = {"Vulkan",       "OpenGL",     "Direct3D 12", "Direct3D 11",
                                 "Direct3D 10",  "Direct3D 9", "Direct3D 8",  "DirectDraw (DX7)"};

static void draw_toolbar(int *view_mode) {
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, 44.0f));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("##toolbar", nullptr, flags)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("AIO Graphics Test");
        ImGui::SameLine(0.0f, 24.0f);
        // Segmented List / Grid control (visual stub).
        const char *modes[] = {"List", "Grid"};
        for (int i = 0; i < 2; ++i) {
            bool active = (*view_mode == i);
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.26f, 0.45f, 0.80f, 1.0f));
            if (ImGui::Button(modes[i], ImVec2(70.0f, 0.0f))) *view_mode = i;
            if (active) ImGui::PopStyleColor();
            if (i == 0) ImGui::SameLine(0.0f, 2.0f);
        }
    }
    ImGui::End();
}

static void draw_test_list(int *selected) {
    ImGuiViewport *vp = ImGui::GetMainViewport();
    const float panel_w = 240.0f;
    const float top = vp->WorkPos.y + 44.0f;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - panel_w, top));
    ImGui::SetNextWindowSize(ImVec2(panel_w, vp->WorkSize.y - 44.0f));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Tests", nullptr, flags)) {
        for (int i = 0; i < (int)(sizeof(k_tests) / sizeof(k_tests[0])); ++i) {
            if (ImGui::Selectable(k_tests[i], *selected == i)) *selected = i;
        }
    }
    ImGui::End();
}

static void draw_viewport_overlay(float fps, const float *frametimes, int ft_count, int ft_offset) {
    ImGuiViewport *vp = ImGui::GetMainViewport();
    const float panel_w = 240.0f;
    const float top = vp->WorkPos.y + 44.0f;
    ImVec2 pos(vp->WorkPos.x + 12.0f, top + 12.0f);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing;
    (void)panel_w;
    if (ImGui::Begin("##overlay", nullptr, flags)) {
        ImGui::Text("Direct3D 11  -  %.0f FPS", fps);
        char label[64];
        snprintf(label, sizeof(label), "%.2f ms", fps > 0.0f ? 1000.0f / fps : 0.0f);
        ImGui::PlotLines("##frametime", frametimes, ft_count, ft_offset, label, 0.0f, 40.0f,
                         ImVec2(220.0f, 60.0f));
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Entry point.
// ---------------------------------------------------------------------------
extern "C" int aio_run_imgui_shell(HINSTANCE hInstance) {
    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "AIOImGuiShell";
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowA(wc.lpszClassName, "AIO Graphics Test", WS_OVERLAPPEDWINDOW, 100, 100,
                              1100, 680, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        UnregisterClassA(wc.lpszClassName, hInstance);
        return 1;
    }

    if (!create_device(hwnd)) {
        destroy_device();
        DestroyWindow(hwnd);
        UnregisterClassA(wc.lpszClassName, hInstance);
        MessageBoxA(nullptr,
                    "Direct3D 11 is not available in this container.\n\n"
                    "Could not create a D3D11 device + swapchain (no d3d11.dll / DXVK?).\n"
                    "The ImGui shell requires the container's DXVK DLL set.",
                    "AIO Graphics Test", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;  // scaffold: do not persist layout
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    // Real frametime ring buffer for the sparkline.
    static float frametimes[120];
    for (int i = 0; i < 120; ++i) frametimes[i] = 0.0f;
    int ft_offset = 0;
    LARGE_INTEGER freq, prev;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);

    int view_mode = 0;   // 0 = List, 1 = Grid (stub)
    int selected = 3;    // Direct3D 11 preselected (matches the overlay label)
    float fps_smooth = 0.0f;

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // Measure the real frame interval.
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt_ms = (double)(now.QuadPart - prev.QuadPart) * 1000.0 / (double)freq.QuadPart;
        prev = now;
        if (dt_ms < 0.0) dt_ms = 0.0;
        frametimes[ft_offset] = (float)dt_ms;
        ft_offset = (ft_offset + 1) % 120;
        float inst_fps = dt_ms > 0.0 ? (float)(1000.0 / dt_ms) : 0.0f;
        fps_smooth = fps_smooth <= 0.0f ? inst_fps : fps_smooth * 0.9f + inst_fps * 0.1f;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        draw_toolbar(&view_mode);
        draw_test_list(&selected);
        draw_viewport_overlay(fps_smooth, frametimes, 120, ft_offset);

        ImGui::Render();
        // Left viewport region: clear color fill (the game/test render target lives
        // here in the real product). ImGui panels composite on top.
        const float clear[4] = {0.08f, 0.10f, 0.13f, 1.0f};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroy_device();
    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, hInstance);
    return 0;
}
