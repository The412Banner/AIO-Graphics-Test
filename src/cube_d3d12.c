// AIO Graphics Test - Direct3D 12 cube backend.
//
// A spinning, multi-colored cube rendered with Direct3D 12. Under Winlator this
// exercises the VKD3D-Proton (d3d12.dll -> Vulkan -> Turnip) translation path,
// the counterpart to the DXVK (D3D11) and native Vulkan/OpenGL cubes.
//
// d3d12.dll, dxgi.dll and d3dcompiler are all loaded dynamically, so the .exe
// has NO static dependency on them and still launches without VKD3D installed,
// showing a graceful notice instead.
//
// For simplicity (this is a diagnostic, not an engine) the frame loop waits for
// the GPU to go idle after each Present rather than pipelining frames.
//
// Copyright (c) 2026 The412Banner. Licensed under Apache-2.0 (see LICENSE).

#define COBJMACROS
// This mingw's dxguid lacks the D3D12 / newer-DXGI IIDs, so instantiate the
// GUIDs we reference directly in this TU.
#define INITGUID
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00  // Windows 10: D3D12 + GetTickCount64
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cube_d3d12.h"
#include "hud.h"
#include "bench.h"
#include "watchdog.h"

#define FRAME_COUNT 2

static int g_w = 640, g_h = 480;
static int g_quit;
// Pending window resize -> refresh g_w/g_h (which feed the projection aspect each
// frame), so the cube isn't stretched when the window is maximized/resized.
static volatile int g_resize_pending = 0;
static int g_resize_w = 0, g_resize_h = 0;

static LRESULT CALLBACK d3d12_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) {
                g_quit = 1;
                PostQuitMessage(0);
            }
            return 0;
        case WM_SIZE:
            if (w != SIZE_MINIMIZED) {
                int nw = LOWORD(l), nh = HIWORD(l);
                if (nw > 0 && nh > 0) { g_resize_w = nw; g_resize_h = nh; g_resize_pending = 1; }
            }
            return 0;
        case WM_CLOSE:
            g_quit = 1;
            PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcA(h, m, w, l);
}

// --- row-major / row-vector matrix math (matches the row_major cbuffer) ---
typedef struct {
    float m[16];
} Mat4;

static Mat4 mat_identity(void) {
    Mat4 r;
    memset(&r, 0, sizeof(r));
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}
static Mat4 mat_mul(Mat4 a, Mat4 b) {
    Mat4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}
static Mat4 mat_translate(float x, float y, float z) {
    Mat4 r = mat_identity();
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
}
static Mat4 mat_rotate(float ax, float ay, float az, float a) {
    float l = sqrtf(ax * ax + ay * ay + az * az);
    if (l > 0) { ax /= l; ay /= l; az /= l; }
    float c = cosf(a), s = sinf(a), t = 1.0f - c;
    Mat4 r = mat_identity();
    r.m[0] = c + ax * ax * t;       r.m[1] = ax * ay * t + az * s;  r.m[2] = ax * az * t - ay * s;
    r.m[4] = ay * ax * t - az * s;  r.m[5] = c + ay * ay * t;       r.m[6] = ay * az * t + ax * s;
    r.m[8] = az * ax * t + ay * s;  r.m[9] = az * ay * t - ax * s;  r.m[10] = c + az * az * t;
    return r;
}
static Mat4 mat_perspective(float fovy, float aspect, float zn, float zf) {
    float f = 1.0f / tanf(fovy * 0.5f);
    Mat4 r;
    memset(&r, 0, sizeof(r));
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = zf / (zn - zf);
    r.m[11] = -1.0f;
    r.m[14] = zn * zf / (zn - zf);
    return r;
}

typedef struct {
    float pos[3];
    float col[3];
} Vertex;

static void build_cube(Vertex *out) {
    static const float f[6][4][3] = {
        {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},
        {{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}},
        {{-1, 1, -1}, {-1, 1, 1}, {1, 1, 1}, {1, 1, -1}},
        {{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}},
        {{1, -1, -1}, {1, 1, -1}, {1, 1, 1}, {1, -1, 1}},
        {{-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}},
    };
    static const float col[6][3] = {
        {0.90f, 0.20f, 0.20f}, {0.20f, 0.80f, 0.30f}, {0.25f, 0.45f, 0.95f},
        {0.95f, 0.80f, 0.20f}, {0.85f, 0.40f, 0.90f}, {0.20f, 0.85f, 0.90f},
    };
    static const int idx[6] = {0, 1, 2, 0, 2, 3};
    int v = 0;
    for (int face = 0; face < 6; face++)
        for (int k = 0; k < 6; k++) {
            int ci = idx[k];
            out[v].pos[0] = f[face][ci][0];
            out[v].pos[1] = f[face][ci][1];
            out[v].pos[2] = f[face][ci][2];
            out[v].col[0] = col[face][0];
            out[v].col[1] = col[face][1];
            out[v].col[2] = col[face][2];
            v++;
        }
}

static const char *kHLSL =
    "cbuffer CB : register(b0) { row_major float4x4 mvp; };\n"
    "struct VSIn { float3 pos : POSITION; float3 col : COLOR; };\n"
    "struct VSOut { float4 pos : SV_POSITION; float3 col : COLOR; };\n"
    "VSOut VSMain(VSIn i) { VSOut o; o.pos = mul(float4(i.pos,1.0), mvp); o.col = i.col; return o; }\n"
    "float4 PSMain(VSOut i) : SV_TARGET { return float4(i.col, 1.0); }\n";

typedef HRESULT(WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
                                        ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **,
                                        ID3DBlob **);
typedef HRESULT(WINAPI *PFN_D3D12CreateDevice)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef HRESULT(WINAPI *PFN_D3D12SerializeRootSignature)(const D3D12_ROOT_SIGNATURE_DESC *,
                                                         D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **,
                                                         ID3DBlob **);
typedef HRESULT(WINAPI *PFN_CreateDXGIFactory2)(UINT, REFIID, void **);

static void fail_box(const char *msg) {
    MessageBoxA(NULL, msg, "AIO Graphics Test - Direct3D 12", MB_OK | MB_ICONERROR);
}

// mingw-w64 gates the COBJMACRO for GetCPUDescriptorHandleForHeapStart behind
// WIDL_C_INLINE_WRAPPERS (which mis-compiles on this toolchain). Its vtable uses
// the explicit-aggregate-return form (This, out*), so call that directly.
static D3D12_CPU_DESCRIPTOR_HANDLE cpu_heap_start(ID3D12DescriptorHeap *h) {
    D3D12_CPU_DESCRIPTOR_HANDLE out;
    h->lpVtbl->GetCPUDescriptorHandleForHeapStart(h, &out);
    return out;
}

static D3D12_HEAP_PROPERTIES heap_props(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES hp;
    memset(&hp, 0, sizeof(hp));
    hp.Type = type;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;
    return hp;
}

static void barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *res, D3D12_RESOURCE_STATES before,
                    D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0, sizeof(b));
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
}

int aio_run_d3d12_cube(HINSTANCE hinst) {
    const char *api = "Direct3D 12";
    const char *cls = "AIOD3D12Cube";

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = d3d12_wndproc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(hinst, MAKEINTRESOURCEA(1));
    wc.lpszClassName = cls;
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA(cls, "AIO Graphics Test  -  Direct3D 12",
                              WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 640,
                              480, NULL, NULL, hinst, NULL);
    if (!hwnd) return 1;
    RECT rc;
    GetClientRect(hwnd, &rc);
    g_w = rc.right - rc.left;
    g_h = rc.bottom - rc.top;
    if (g_w <= 0) g_w = 640;
    if (g_h <= 0) g_h = 480;

    // --- dynamic entry points ---
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    HMODULE d3dc = LoadLibraryA("d3dcompiler_47.dll");
    if (!d3dc) d3dc = LoadLibraryA("d3dcompiler_43.dll");
    PFN_D3D12CreateDevice p_create =
        d3d12 ? (PFN_D3D12CreateDevice)GetProcAddress(d3d12, "D3D12CreateDevice") : NULL;
    PFN_D3D12SerializeRootSignature p_serialize =
        d3d12 ? (PFN_D3D12SerializeRootSignature)GetProcAddress(d3d12, "D3D12SerializeRootSignature")
              : NULL;
    PFN_CreateDXGIFactory2 p_factory =
        dxgi ? (PFN_CreateDXGIFactory2)GetProcAddress(dxgi, "CreateDXGIFactory2") : NULL;
    PFN_D3DCompile p_compile = d3dc ? (PFN_D3DCompile)GetProcAddress(d3dc, "D3DCompile") : NULL;
    if (!p_create || !p_serialize || !p_factory || !p_compile) {
        fail_box(
            "Direct3D 12 is not available in this container.\n\n"
            "Could not load d3d12.dll / dxgi.dll / d3dcompiler (is VKD3D installed?).");
        DestroyWindow(hwnd);
        return 1;
    }

    ID3D12Device *dev = NULL;
    if (FAILED(p_create(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev)) || !dev) {
        fail_box(
            "Could not create a Direct3D 12 device.\n\n"
            "This container's GPU/driver doesn't expose D3D12 (VKD3D-Proton).");
        DestroyWindow(hwnd);
        return 1;
    }

    ID3D12CommandQueue *queue = NULL;
    D3D12_COMMAND_QUEUE_DESC qd;
    memset(&qd, 0, sizeof(qd));
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue);

    IDXGIFactory2 *factory = NULL;
    p_factory(0, &IID_IDXGIFactory2, (void **)&factory);
    DXGI_SWAP_CHAIN_DESC1 scd;
    memset(&scd, 0, sizeof(scd));
    scd.Width = g_w;
    scd.Height = g_h;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = FRAME_COUNT;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1 *swap1 = NULL;
    IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)queue, hwnd, &scd, NULL, NULL, &swap1);
    IDXGISwapChain3 *swap = NULL;
    if (swap1) IDXGISwapChain1_QueryInterface(swap1, &IID_IDXGISwapChain3, (void **)&swap);
    if (!swap) {
        fail_box("Could not create the Direct3D 12 swapchain.");
        DestroyWindow(hwnd);
        return 1;
    }

    // RTV heap (one per back buffer) + DSV heap.
    ID3D12DescriptorHeap *rtvHeap = NULL, *dsvHeap = NULL;
    D3D12_DESCRIPTOR_HEAP_DESC hd;
    memset(&hd, 0, sizeof(hd));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = FRAME_COUNT;
    ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvHeap);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 1;
    ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&dsvHeap);
    UINT rtvSize = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvStart = cpu_heap_start(rtvHeap);
    ID3D12Resource *rt[FRAME_COUNT];
    for (UINT i = 0; i < FRAME_COUNT; i++) {
        rt[i] = NULL;
        IDXGISwapChain3_GetBuffer(swap, i, &IID_ID3D12Resource, (void **)&rt[i]);
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvStart;
        h.ptr += (SIZE_T)i * rtvSize;
        ID3D12Device_CreateRenderTargetView(dev, rt[i], NULL, h);
    }

    // Depth buffer + DSV.
    ID3D12Resource *depth = NULL;
    {
        D3D12_HEAP_PROPERTIES hp = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC rd;
        memset(&rd, 0, sizeof(rd));
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = g_w;
        rd.Height = g_h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_D32_FLOAT;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv;
        memset(&cv, 0, sizeof(cv));
        cv.Format = DXGI_FORMAT_D32_FLOAT;
        cv.DepthStencil.Depth = 1.0f;
        ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                             &IID_ID3D12Resource, (void **)&depth);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = cpu_heap_start(dsvHeap);
    ID3D12Device_CreateDepthStencilView(dev, depth, NULL, dsvHandle);

    // Root signature: a single root CBV at b0.
    ID3D12RootSignature *rootsig = NULL;
    {
        D3D12_ROOT_PARAMETER rp;
        memset(&rp, 0, sizeof(rp));
        rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rp.Descriptor.ShaderRegister = 0;
        rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rs;
        memset(&rs, 0, sizeof(rs));
        rs.NumParameters = 1;
        rs.pParameters = &rp;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ID3DBlob *sig = NULL, *err = NULL;
        if (FAILED(p_serialize(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) || !sig) {
            fail_box("Failed to serialize the D3D12 root signature.");
            DestroyWindow(hwnd);
            return 1;
        }
        ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(sig),
                                         ID3D10Blob_GetBufferSize(sig), &IID_ID3D12RootSignature,
                                         (void **)&rootsig);
        ID3D10Blob_Release(sig);
        if (err) ID3D10Blob_Release(err);
    }

    // Compile shaders + build the PSO.
    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    if (FAILED(p_compile(kHLSL, strlen(kHLSL), "cube.hlsl", NULL, NULL, "VSMain", "vs_5_0", 0, 0,
                         &vsb, &err)) ||
        FAILED(p_compile(kHLSL, strlen(kHLSL), "cube.hlsl", NULL, NULL, "PSMain", "ps_5_0", 0, 0,
                         &psb, &err))) {
        fail_box("Failed to compile the Direct3D 12 cube shaders.");
        DestroyWindow(hwnd);
        return 1;
    }
    D3D12_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    ID3D12PipelineState *pso = NULL;
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
        memset(&pd, 0, sizeof(pd));
        pd.pRootSignature = rootsig;
        pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vsb);
        pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vsb);
        pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(psb);
        pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(psb);
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = 0xFFFFFFFFu;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        pd.InputLayout.pInputElementDescs = il;
        pd.InputLayout.NumElements = 2;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pd.SampleDesc.Count = 1;
        if (FAILED(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState,
                                                            (void **)&pso))) {
            fail_box("Failed to create the Direct3D 12 pipeline state.");
            DestroyWindow(hwnd);
            return 1;
        }
    }
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);

    // Vertex buffer (upload heap, read directly by the GPU).
    Vertex verts[36];
    build_cube(verts);
    ID3D12Resource *vbo = NULL;
    {
        D3D12_HEAP_PROPERTIES hp = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC rd;
        memset(&rd, 0, sizeof(rd));
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = sizeof(verts);
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                             &IID_ID3D12Resource, (void **)&vbo);
        void *p = NULL;
        D3D12_RANGE rr = {0, 0};
        ID3D12Resource_Map(vbo, 0, &rr, &p);
        memcpy(p, verts, sizeof(verts));
        ID3D12Resource_Unmap(vbo, 0, NULL);
    }
    D3D12_VERTEX_BUFFER_VIEW vbv;
    vbv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(vbo);
    vbv.SizeInBytes = sizeof(verts);
    vbv.StrideInBytes = sizeof(Vertex);

    // Constant buffer: one 256-byte slot per frame-in-flight, persistently mapped.
    // Separate slots so the CPU can write the next frame's MVP while the GPU is
    // still reading the previous frame's (required once the loop is pipelined).
    const SIZE_T CB_STRIDE = 256;
    ID3D12Resource *cbo = NULL;
    void *cbptr = NULL;
    {
        D3D12_HEAP_PROPERTIES hp = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC rd;
        memset(&rd, 0, sizeof(rd));
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = CB_STRIDE * FRAME_COUNT;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                             &IID_ID3D12Resource, (void **)&cbo);
        D3D12_RANGE rr = {0, 0};
        ID3D12Resource_Map(cbo, 0, &rr, &cbptr);
    }

    // One command allocator per frame-in-flight so we can record the next frame
    // while the GPU is still executing the previous one's allocator.
    ID3D12CommandAllocator *alloc[FRAME_COUNT] = {0};
    for (UINT i = 0; i < FRAME_COUNT; i++)
        ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            &IID_ID3D12CommandAllocator, (void **)&alloc[i]);
    ID3D12GraphicsCommandList *cl = NULL;
    ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0], NULL,
                                   &IID_ID3D12GraphicsCommandList, (void **)&cl);
    ID3D12GraphicsCommandList_Close(cl);

    ID3D12Fence *fence = NULL;
    UINT64 fenceVal = 0;
    ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    HANDLE fenceEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    // Fence value that retires each frame slot; 0 = slot never used yet.
    UINT64 frameFence[FRAME_COUNT] = {0};
    UINT cpuFrame = 0;  // rotating CPU slot 0..FRAME_COUNT-1 (allocator + CB + fence)

    D3D12_VIEWPORT vp = {0.0f, 0.0f, (float)g_w, (float)g_h, 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, g_w, g_h};
    const float clear[4] = {0.10f, 0.10f, 0.12f, 1.0f};

    aio_hud_create(hinst);
    aio_hud_update(hwnd, "Direct3D 12  -  measuring...");

    int bench_on = aio_bench_active();
    LARGE_INTEGER qpf, start, prev;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&start);
    prev = start;
    ULONGLONG last_ms = GetTickCount64();
    uint64_t frames = 0, last_frame = 0;

    MSG msg;
    aio_watchdog_start(&frames, 12);
    g_quit = 0;
    while (!g_quit) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_quit = 1; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (g_quit) break;
        if (g_resize_pending) {  // keep the cube round when the window is resized
            g_resize_pending = 0;
            if (g_resize_w > 0 && g_resize_h > 0) { g_w = g_resize_w; g_h = g_resize_h; }
        }

        // Throttle the CPU to FRAME_COUNT frames ahead: wait only for the slot
        // we are about to reuse (allocator + CB), not the frame just submitted.
        // This is what lets CPU recording overlap GPU execution (real pipelining).
        if (frameFence[cpuFrame] != 0 &&
            ID3D12Fence_GetCompletedValue(fence) < frameFence[cpuFrame]) {
            ID3D12Fence_SetEventOnCompletion(fence, frameFence[cpuFrame], fenceEvent);
            WaitForSingleObject(fenceEvent, INFINITE);
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double t = (double)(now.QuadPart - start.QuadPart) / (double)qpf.QuadPart;
        float a = (float)t * 0.6f;
        Mat4 model = mat_mul(mat_rotate(0, 1, 0, a), mat_rotate(1, 0, 0, 0.5f));
        Mat4 mvp = mat_mul(mat_mul(model, mat_translate(0, 0, -6.5f)),
                           mat_perspective(0.6f, (g_h > 0) ? (float)g_w / g_h : 1.0f, 0.1f, 100.0f));
        SIZE_T cbOff = (SIZE_T)cpuFrame * CB_STRIDE;
        memcpy((char *)cbptr + cbOff, mvp.m, sizeof(mvp.m));

        UINT idx = IDXGISwapChain3_GetCurrentBackBufferIndex(swap);
        ID3D12CommandAllocator_Reset(alloc[cpuFrame]);
        ID3D12GraphicsCommandList_Reset(cl, alloc[cpuFrame], pso);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, rootsig);
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(
            cl, 0, ID3D12Resource_GetGPUVirtualAddress(cbo) + cbOff);
        ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &scissor);
        barrier(cl, rt[idx], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvStart;
        rtv.ptr += (SIZE_T)idx * rtvSize;
        ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv, FALSE, &dsvHandle);
        ID3D12GraphicsCommandList_ClearRenderTargetView(cl, rtv, clear, 0, NULL);
        ID3D12GraphicsCommandList_ClearDepthStencilView(cl, dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f,
                                                        0, 0, NULL);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl,
                                                         D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_IASetVertexBuffers(cl, 0, 1, &vbv);
        ID3D12GraphicsCommandList_DrawInstanced(cl, 36, 1, 0, 0);
        barrier(cl, rt[idx], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        ID3D12GraphicsCommandList_Close(cl);

        ID3D12CommandList *lists[1] = {(ID3D12CommandList *)cl};
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
        IDXGISwapChain3_Present(swap, aio_vsync ? 1 : 0, 0);

        // Tag this slot with the fence value that will retire it, then advance.
        // We do NOT block here — the GPU runs this frame while the CPU records
        // the next one; the throttle at the top of the loop enforces the depth.
        fenceVal++;
        ID3D12CommandQueue_Signal(queue, fence, fenceVal);
        frameFence[cpuFrame] = fenceVal;
        cpuFrame = (cpuFrame + 1) % FRAME_COUNT;
        frames++;

        if (bench_on) {
            double dt_ms = (double)(now.QuadPart - prev.QuadPart) * 1000.0 / (double)qpf.QuadPart;
            aio_bench_add(dt_ms);
            prev = now;
            if (t >= (double)aio_bench_seconds()) g_quit = 1;
        }

        ULONGLONG now_ms = GetTickCount64();
        if (now_ms - last_ms >= 500) {
            double secs = (double)(now_ms - last_ms) / 1000.0;
            double fps = (secs > 0.0) ? (double)(frames - last_frame) / secs : 0.0;
            char hud[64], title[128];
            snprintf(hud, sizeof(hud), "%s   %.0f FPS", api, fps);
            aio_hud_update(hwnd, hud);
            snprintf(title, sizeof(title), "AIO Graphics Test  -  %s  -  %.0f FPS", api, fps);
            SetWindowTextA(hwnd, title);
            last_ms = now_ms;
            last_frame = frames;
        }
    }

    aio_watchdog_stop();

    if (bench_on) {
        QueryPerformanceCounter(&prev);
        double total = (double)(prev.QuadPart - start.QuadPart) / (double)qpf.QuadPart;
        char *res = aio_bench_finish(api, total);
        if (res) {
            aio_bench_show_result(res);
            free(res);
        }
    }

    // Drain the GPU before tearing down.
    fenceVal++;
    ID3D12CommandQueue_Signal(queue, fence, fenceVal);
    if (ID3D12Fence_GetCompletedValue(fence) < fenceVal) {
        ID3D12Fence_SetEventOnCompletion(fence, fenceVal, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    }
    aio_hud_destroy();

    if (fenceEvent) CloseHandle(fenceEvent);
    if (fence) ID3D12Fence_Release(fence);
    if (cl) ID3D12GraphicsCommandList_Release(cl);
    for (UINT i = 0; i < FRAME_COUNT; i++)
        if (alloc[i]) ID3D12CommandAllocator_Release(alloc[i]);
    if (cbo) ID3D12Resource_Release(cbo);
    if (vbo) ID3D12Resource_Release(vbo);
    if (pso) ID3D12PipelineState_Release(pso);
    if (rootsig) ID3D12RootSignature_Release(rootsig);
    if (depth) ID3D12Resource_Release(depth);
    for (UINT i = 0; i < FRAME_COUNT; i++)
        if (rt[i]) ID3D12Resource_Release(rt[i]);
    if (dsvHeap) ID3D12DescriptorHeap_Release(dsvHeap);
    if (rtvHeap) ID3D12DescriptorHeap_Release(rtvHeap);
    if (swap) IDXGISwapChain3_Release(swap);
    if (swap1) IDXGISwapChain1_Release(swap1);
    if (factory) IDXGIFactory2_Release(factory);
    if (queue) ID3D12CommandQueue_Release(queue);
    if (dev) ID3D12Device_Release(dev);
    DestroyWindow(hwnd);
    return 0;
}
