// AIO Graphics Test - in-app start menu.
// On launch the .exe shows a menu (app-name header + buttons) to pick a test.
#ifndef AIO_MENU_H
#define AIO_MENU_H

#include <windows.h>

// Current release version, shown in the bottom-right of the shell. Bump on release.
#define AIO_VERSION "v1.6.1"

enum AioMode {
    AIO_MODE_EXIT = 0,
    AIO_MODE_CUBE_VK,
    AIO_MODE_CUBE_GL,
    AIO_MODE_CUBE_DDRAW,  // DirectDraw / legacy Direct3D (DX7 cube + 2D blit)
    AIO_MODE_CUBE_DX8,
    AIO_MODE_CUBE_DX9,
    AIO_MODE_CUBE_DX10,
    AIO_MODE_CUBE_DX11,
    AIO_MODE_CUBE_DX12,
    AIO_MODE_GPUINFO,
    AIO_MODE_BENCH,
    AIO_MODE_SEMAPHORE,
    AIO_MODE_DISK  // disk (drive) read/write speed test
};

// Runs the app shell: a persistent left sidebar (the menu, always visible) plus
// a content pane on the right where each test opens in-frame. Blocks until the
// window is closed. Returns a process exit code.
int aio_run_shell(HINSTANCE hInstance);

#endif  // AIO_MENU_H
