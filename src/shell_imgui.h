// AIO Graphics Test - Dear ImGui single-window shell (Phase 0 de-risk scaffold).
//
// Additive, reversible entry point. cube.c (compiled as C) includes this header to
// dispatch the --imgui CLI flag to aio_run_imgui_shell(). The implementation lives
// in shell_imgui.cpp (compiled as C++). The declaration uses C linkage under a C++
// compiler so the C caller and the C++ definition agree on the symbol name.
#ifndef AIO_SHELL_IMGUI_H
#define AIO_SHELL_IMGUI_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opens the single-window Dear ImGui / Direct3D 11 shell and runs it until the
// window is closed (or ESC). Returns a process exit code. This is the Phase 0
// scaffold: it proves the C++ / ImGui / DX11 path builds and launches; it does
// NOT replace the existing shell.
int aio_run_imgui_shell(HINSTANCE hInstance);

#ifdef __cplusplus
}
#endif

#endif  // AIO_SHELL_IMGUI_H
