// AIO Graphics Test - GPU info / report (GL + Vulkan adapter dump).
//
// Builds a text report by (a) opening its own VkInstance to enumerate Vulkan
// adapters and (b) spinning up a throwaway WGL OpenGL context to read
// GL_VENDOR/RENDERER/VERSION/GLSL/EXTENSIONS. The report can be returned as a
// string (for the in-app GPU Info window) or written to file + console (CLI).
// Replaces GPUInfo.exe.
//
// Copyright (c) 2026 The412Banner. Licensed under Apache-2.0 (see LICENSE).

#include <windows.h>
#include <GL/gl.h>
#include <vulkan/vulkan.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpuinfo.h"

#ifndef GL_SHADING_LANGUAGE_VERSION
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#endif

#define AIO_REPORT_FILE "AIO-Graphics-Test_report.txt"

// ----------------------------------------------------------- growable string
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} StrBuf;

static void sb_init(StrBuf *sb) {
    sb->cap = 4096;
    sb->len = 0;
    sb->buf = (char *)malloc(sb->cap);
    if (sb->buf) sb->buf[0] = '\0';
}

static void sb_appendf(StrBuf *sb, const char *fmt, ...) {
    if (!sb->buf) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (sb->len + (size_t)n + 1 > sb->cap) {
        size_t ncap = sb->cap * 2;
        while (sb->len + (size_t)n + 1 > ncap) ncap *= 2;
        char *nb = (char *)realloc(sb->buf, ncap);
        if (!nb) return;
        sb->buf = nb;
        sb->cap = ncap;
    }
    va_start(ap, fmt);
    vsnprintf(sb->buf + sb->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb->len += (size_t)n;
}

static const char *vk_device_type_str(VkPhysicalDeviceType t) {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "Integrated GPU";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "Discrete GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "Virtual GPU";
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU";
        default:                                     return "Other";
    }
}

// ------------------------------------------------------------- OpenGL section
static void report_opengl(StrBuf *sb) {
    sb_appendf(sb, "\r\n==================== OpenGL ====================\r\n");

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "AIOGpuInfoGLWindow";
    RegisterClassA(&wc);  // ok if already registered

    HWND hwnd = CreateWindowA(wc.lpszClassName, "gl", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, NULL, NULL,
                              wc.hInstance, NULL);
    if (!hwnd) {
        sb_appendf(sb, "OpenGL: could not create helper window.\r\n");
        return;
    }

    HDC dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int pf = ChoosePixelFormat(dc, &pfd);
    if (pf == 0 || !SetPixelFormat(dc, pf, &pfd)) {
        sb_appendf(sb, "OpenGL: no compatible pixel format (GL may be unavailable here).\r\n");
        ReleaseDC(hwnd, dc);
        DestroyWindow(hwnd);
        return;
    }

    HGLRC rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) {
        sb_appendf(sb, "OpenGL: could not create/make-current a WGL context.\r\n");
        if (rc) wglDeleteContext(rc);
        ReleaseDC(hwnd, dc);
        DestroyWindow(hwnd);
        return;
    }

    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    const char *glsl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
    const char *exts = (const char *)glGetString(GL_EXTENSIONS);

    sb_appendf(sb, "GL_VENDOR                   : %s\r\n", vendor ? vendor : "(null)");
    sb_appendf(sb, "GL_RENDERER                 : %s\r\n", renderer ? renderer : "(null)");
    sb_appendf(sb, "GL_VERSION                  : %s\r\n", version ? version : "(null)");
    sb_appendf(sb, "GL_SHADING_LANGUAGE_VERSION : %s\r\n", glsl ? glsl : "(null)");
    if (exts) {
        sb_appendf(sb, "GL_EXTENSIONS               :\r\n%s\r\n", exts);
    } else {
        sb_appendf(sb, "GL_EXTENSIONS               : (null / core profile)\r\n");
    }

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    ReleaseDC(hwnd, dc);
    DestroyWindow(hwnd);
}

// ------------------------------------------------------------- Vulkan section
static void report_vulkan(StrBuf *sb) {
    sb_appendf(sb, "\r\n==================== Vulkan ====================\r\n");

    VkApplicationInfo app;
    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "AIO Graphics Test";
    app.pEngineName = "AIO Graphics Test";
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
        sb_appendf(sb, "Vulkan: vkCreateInstance failed - no Vulkan ICD reachable.\r\n");
        return;
    }

    uint32_t inst_ext_count = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &inst_ext_count, NULL);
    if (inst_ext_count > 0) {
        VkExtensionProperties *ie = malloc(sizeof(VkExtensionProperties) * inst_ext_count);
        if (ie) {
            vkEnumerateInstanceExtensionProperties(NULL, &inst_ext_count, ie);
            sb_appendf(sb, "Instance extensions (%u):\r\n", inst_ext_count);
            for (uint32_t i = 0; i < inst_ext_count; i++) sb_appendf(sb, "  %s\r\n", ie[i].extensionName);
            free(ie);
        }
    }

    uint32_t gpu_count = 0;
    vkEnumeratePhysicalDevices(inst, &gpu_count, NULL);
    if (gpu_count == 0) {
        sb_appendf(sb, "Vulkan: no physical devices found.\r\n");
        vkDestroyInstance(inst, NULL);
        return;
    }
    VkPhysicalDevice *gpus = malloc(sizeof(VkPhysicalDevice) * gpu_count);
    if (!gpus) {
        vkDestroyInstance(inst, NULL);
        return;
    }
    vkEnumeratePhysicalDevices(inst, &gpu_count, gpus);
    sb_appendf(sb, "\r\nPhysical devices: %u\r\n", gpu_count);

    for (uint32_t g = 0; g < gpu_count; g++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(gpus[g], &p);

        sb_appendf(sb, "\r\n--- GPU %u ---\r\n", g);
        sb_appendf(sb, "deviceName    : %s\r\n", p.deviceName);
        sb_appendf(sb, "deviceType    : %s\r\n", vk_device_type_str(p.deviceType));
        sb_appendf(sb, "apiVersion    : %u.%u.%u (0x%08x)\r\n", VK_API_VERSION_MAJOR(p.apiVersion),
                   VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), p.apiVersion);
        sb_appendf(sb, "driverVersion : %u.%u.%u (0x%08x raw)\r\n", VK_API_VERSION_MAJOR(p.driverVersion),
                   VK_API_VERSION_MINOR(p.driverVersion), VK_API_VERSION_PATCH(p.driverVersion),
                   p.driverVersion);
        sb_appendf(sb, "vendorID      : 0x%04x\r\n", p.vendorID);
        sb_appendf(sb, "deviceID      : 0x%04x\r\n", p.deviceID);

        VkPhysicalDeviceFeatures feat;
        vkGetPhysicalDeviceFeatures(gpus[g], &feat);
        sb_appendf(sb, "Features:\r\n");
#define AIO_F(name) sb_appendf(sb, "  %-28s: %s\r\n", #name, feat.name ? "yes" : "no")
        AIO_F(robustBufferAccess);
        AIO_F(fullDrawIndexUint32);
        AIO_F(imageCubeArray);
        AIO_F(geometryShader);
        AIO_F(tessellationShader);
        AIO_F(multiDrawIndirect);
        AIO_F(multiViewport);
        AIO_F(samplerAnisotropy);
        AIO_F(textureCompressionETC2);
        AIO_F(textureCompressionASTC_LDR);
        AIO_F(textureCompressionBC);
        AIO_F(shaderInt64);
        AIO_F(shaderInt16);
        AIO_F(sparseBinding);
        AIO_F(fragmentStoresAndAtomics);
#undef AIO_F

        VkPhysicalDeviceMemoryProperties mem;
        vkGetPhysicalDeviceMemoryProperties(gpus[g], &mem);
        sb_appendf(sb, "memoryHeaps   : %u\r\n", mem.memoryHeapCount);
        for (uint32_t h = 0; h < mem.memoryHeapCount; h++) {
            double mb = (double)mem.memoryHeaps[h].size / (1024.0 * 1024.0);
            int dl = (mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? 1 : 0;
            sb_appendf(sb, "  heap %u: %.0f MB%s\r\n", h, mb, dl ? " (DEVICE_LOCAL)" : "");
        }

        uint32_t qf_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[g], &qf_count, NULL);
        if (qf_count > 0) {
            VkQueueFamilyProperties *qf = malloc(sizeof(VkQueueFamilyProperties) * qf_count);
            if (qf) {
                vkGetPhysicalDeviceQueueFamilyProperties(gpus[g], &qf_count, qf);
                sb_appendf(sb, "queueFamilies : %u\r\n", qf_count);
                for (uint32_t q = 0; q < qf_count; q++) {
                    VkQueueFlags f = qf[q].queueFlags;
                    sb_appendf(sb, "  family %u: count=%u%s%s%s%s\r\n", q, qf[q].queueCount,
                               (f & VK_QUEUE_GRAPHICS_BIT) ? " GRAPHICS" : "",
                               (f & VK_QUEUE_COMPUTE_BIT) ? " COMPUTE" : "",
                               (f & VK_QUEUE_TRANSFER_BIT) ? " TRANSFER" : "",
                               (f & VK_QUEUE_SPARSE_BINDING_BIT) ? " SPARSE" : "");
                }
                free(qf);
            }
        }

        uint32_t dev_ext_count = 0;
        vkEnumerateDeviceExtensionProperties(gpus[g], NULL, &dev_ext_count, NULL);
        if (dev_ext_count > 0) {
            VkExtensionProperties *de = malloc(sizeof(VkExtensionProperties) * dev_ext_count);
            if (de) {
                vkEnumerateDeviceExtensionProperties(gpus[g], NULL, &dev_ext_count, de);
                sb_appendf(sb, "deviceExtensions (%u):\r\n", dev_ext_count);
                for (uint32_t i = 0; i < dev_ext_count; i++) sb_appendf(sb, "  %s\r\n", de[i].extensionName);
                free(de);
            }
        }
    }

    free(gpus);
    vkDestroyInstance(inst, NULL);
}

// --------------------------------------------------------- structured queries
static const char *vk_vendor_str(uint32_t id) {
    switch (id) {
        case 0x1002: return "AMD";
        case 0x1010: return "ImgTec";
        case 0x10DE: return "NVIDIA";
        case 0x13B5: return "ARM";
        case 0x5143: return "Qualcomm";
        case 0x8086: return "Intel";
        case 0x10005: return "Mesa";
        default:      return "Unknown";
    }
}

void aio_gpuinfo_query_vk(AioVkInfo *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    VkApplicationInfo app;
    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "AIO Graphics Test";
    app.pEngineName = "AIO Graphics Test";
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) return;

    uint32_t gpu_count = 0;
    vkEnumeratePhysicalDevices(inst, &gpu_count, NULL);
    if (gpu_count == 0) { vkDestroyInstance(inst, NULL); return; }
    VkPhysicalDevice *gpus = malloc(sizeof(VkPhysicalDevice) * gpu_count);
    if (!gpus) { vkDestroyInstance(inst, NULL); return; }
    vkEnumeratePhysicalDevices(inst, &gpu_count, gpus);

    // Prefer the first non-CPU device (the real GPU), else device 0.
    uint32_t sel = 0;
    for (uint32_t g = 0; g < gpu_count; g++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(gpus[g], &p);
        if (p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) { sel = g; break; }
    }

    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(gpus[sel], &p);
    snprintf(out->device, sizeof(out->device), "%s", p.deviceName);
    snprintf(out->api, sizeof(out->api), "%u.%u.%u", VK_API_VERSION_MAJOR(p.apiVersion),
             VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
    snprintf(out->driver, sizeof(out->driver), "%u.%u.%u", VK_API_VERSION_MAJOR(p.driverVersion),
             VK_API_VERSION_MINOR(p.driverVersion), VK_API_VERSION_PATCH(p.driverVersion));
    snprintf(out->vendor, sizeof(out->vendor), "%s (0x%04x)", vk_vendor_str(p.vendorID), p.vendorID);
    snprintf(out->type, sizeof(out->type), "%s", vk_device_type_str(p.deviceType));

    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(gpus[sel], &mem);
    VkDeviceSize best = 0;
    int best_local = 0;
    for (uint32_t h = 0; h < mem.memoryHeapCount; h++) {
        if (mem.memoryHeaps[h].size > best) {
            best = mem.memoryHeaps[h].size;
            best_local = (mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? 1 : 0;
        }
    }
    snprintf(out->memory, sizeof(out->memory), "%.1f GB %s",
             (double)best / (1024.0 * 1024.0 * 1024.0), best_local ? "device-local" : "shared");

    VkPhysicalDeviceFeatures feat;
    vkGetPhysicalDeviceFeatures(gpus[sel], &feat);
    out->f_geometry = feat.geometryShader ? 1 : 0;
    out->f_tessellation = feat.tessellationShader ? 1 : 0;
    out->f_samplerAniso = feat.samplerAnisotropy ? 1 : 0;
    out->f_multiDrawIndirect = feat.multiDrawIndirect ? 1 : 0;
    out->f_shaderInt64 = feat.shaderInt64 ? 1 : 0;
    out->f_textureBC = feat.textureCompressionBC ? 1 : 0;
    out->f_sparseBinding = feat.sparseBinding ? 1 : 0;
    out->f_fragStoresAtomics = feat.fragmentStoresAndAtomics ? 1 : 0;
    out->ok = 1;

    free(gpus);
    vkDestroyInstance(inst, NULL);
}

void aio_gpuinfo_query_gl(AioGlInfo *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "AIOGpuInfoGLWindow";
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowA(wc.lpszClassName, "gl", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, NULL, NULL,
                              wc.hInstance, NULL);
    if (!hwnd) return;
    HDC dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(dc, &pfd);
    if (pf == 0 || !SetPixelFormat(dc, pf, &pfd)) {
        ReleaseDC(hwnd, dc);
        DestroyWindow(hwnd);
        return;
    }
    HGLRC rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) {
        if (rc) wglDeleteContext(rc);
        ReleaseDC(hwnd, dc);
        DestroyWindow(hwnd);
        return;
    }

    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    const char *glsl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    snprintf(out->renderer, sizeof(out->renderer), "%s", renderer ? renderer : "(null)");
    snprintf(out->version, sizeof(out->version), "%s", version ? version : "(null)");
    snprintf(out->glsl, sizeof(out->glsl), "%s", glsl ? glsl : "(null)");
    snprintf(out->vendor, sizeof(out->vendor), "%s", vendor ? vendor : "(null)");
    GLint maxtex = 0, maxsamp = 0;
    glGetIntegerv(0x0D33 /* GL_MAX_TEXTURE_SIZE */, &maxtex);
    glGetIntegerv(0x8D57 /* GL_MAX_SAMPLES */, &maxsamp);
    snprintf(out->max_texture, sizeof(out->max_texture), "%d", (int)maxtex);
    if (maxsamp > 0) snprintf(out->max_samples, sizeof(out->max_samples), "%d", (int)maxsamp);
    else            snprintf(out->max_samples, sizeof(out->max_samples), "-");
    out->ok = 1;

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    ReleaseDC(hwnd, dc);
    DestroyWindow(hwnd);
}

// ----------------------------------------------------------------- public API
// OpenGL-only report (for the OpenGL tab). Caller frees.
char *aio_gpuinfo_build_gl_text(void) {
    StrBuf sb;
    sb_init(&sb);
    report_opengl(&sb);
    return sb.buf;
}

// Vulkan-only report (for the Vulkan tab). Caller frees.
char *aio_gpuinfo_build_vk_text(void) {
    StrBuf sb;
    sb_init(&sb);
    report_vulkan(&sb);
    return sb.buf;
}

char *aio_gpuinfo_build_text(void) {
    StrBuf sb;
    sb_init(&sb);
    sb_appendf(&sb, "AIO Graphics Test - GPU info report\r\n");
    sb_appendf(&sb, "(OpenGL + Vulkan adapter dump; replaces GPUInfo.exe)\r\n");
    report_opengl(&sb);
    report_vulkan(&sb);
    return sb.buf;  // caller frees
}

int aio_run_gpuinfo(void) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
    }
    char *text = aio_gpuinfo_build_text();
    if (text) {
        FILE *f = fopen(AIO_REPORT_FILE, "w");
        if (f) {
            fputs(text, f);
            fclose(f);
        }
        fputs(text, stdout);
        printf("\nReport written to %s\n", AIO_REPORT_FILE);
        free(text);
    }
    return 0;
}
