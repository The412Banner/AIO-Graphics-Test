// AIO Graphics Test - GPU info / report mode (--gpuinfo / --report).
// Self-contained GL + Vulkan adapter dump; replaces GPUInfo.exe.
#ifndef AIO_GPUINFO_H
#define AIO_GPUINFO_H

// Per-API reports for the GPU Info tabs (caller frees). CRLF-terminated.
char *aio_gpuinfo_build_gl_text(void);
char *aio_gpuinfo_build_vk_text(void);

// ---------------------------------------------------------------------------
// Structured accessors (for the ImGui shell's GPU Info datapane). These do the
// SAME enumeration as the text reports but fill fixed structs so a card UI can
// lay out Device/Driver/API/... rows and green/red feature flags without parsing
// text. Every value is REAL (queried from the live Vulkan / GL driver); nothing
// is fabricated. Both are self-contained (open + tear down their own instance /
// context) and are blocking - call once off the UI thread and cache.
// ---------------------------------------------------------------------------
typedef struct {
    int ok;             // 1 = a Vulkan physical device was enumerated
    char device[256];   // VkPhysicalDeviceProperties.deviceName
    char driver[96];    // decoded driverVersion (x.y.z)
    char api[32];       // apiVersion "major.minor.patch"
    char vendor[64];    // "Qualcomm (0x5143)" etc.
    char type[32];      // "Integrated GPU" / "Discrete GPU" / ...
    char memory[48];    // largest heap, "11.4 GB device-local" / "... shared"
    // Real VkPhysicalDeviceFeatures bits shown as yes/no rows.
    int f_geometry, f_tessellation, f_samplerAniso, f_multiDrawIndirect;
    int f_shaderInt64, f_textureBC, f_sparseBinding, f_fragStoresAtomics;
} AioVkInfo;

typedef struct {
    int ok;                 // 1 = a WGL context came up and strings read
    char renderer[256];     // GL_RENDERER
    char version[128];      // GL_VERSION
    char glsl[64];          // GL_SHADING_LANGUAGE_VERSION
    char vendor[128];       // GL_VENDOR
    char max_texture[24];   // GL_MAX_TEXTURE_SIZE
    char max_samples[24];   // GL_MAX_SAMPLES ("-" if unavailable)
} AioGlInfo;

// Fill *out (zeroed first). Safe to call with a NULL-driver container: sets ok=0.
void aio_gpuinfo_query_vk(AioVkInfo *out);
void aio_gpuinfo_query_gl(AioGlInfo *out);

// Builds the full GL + Vulkan adapter report as a heap string (caller frees).
// Lines are CRLF-terminated so it displays correctly in a Win32 EDIT control.
char *aio_gpuinfo_build_text(void);

// CLI path: builds the report and writes it to the console and to
// "AIO-Graphics-Test_report.txt". Returns a process exit code.
int aio_run_gpuinfo(void);

#endif  // AIO_GPUINFO_H
