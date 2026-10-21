/*
 * CursorTest - checks full-colour mouse cursors without needing a game.
 *
 * Move the mouse over each tile; the cursor changes to that tile's test cursor. What you should
 * see is written in the tile. The strip at the bottom is plain GDI drawing (text, gradients,
 * alpha blending, stretching) that must look the same before and after a cursor change in the
 * emulator, as a quick check that 2D drawing was not affected.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <math.h>

#define COLS 3
#define ROWS 2
#define GDI_STRIP_H 190
#define ANIM_TIMER 1

typedef uint32_t (*pixel_fn)(int x, int y, int w, int h, int frame);

static HCURSOR g_cursors[COLS * ROWS];
static HCURSOR g_anim[6];
static int g_anim_frame;
static int g_hover = -1;

static const char *g_titles[COLS * ROWS] = {
    "1. Colour quadrants (32x32)",
    "2. Soft shadow / alpha (48x48)",
    "3. Large rainbow (64x64)",
    "4. Animated (changes every 0.3 s)",
    "5. Standard Windows arrow",
    "6. Hidden cursor",
};
static const char *g_expect[COLS * ROWS] = {
    "Expect: RED top-left, GREEN top-right,\nBLUE bottom-left, YELLOW bottom-right.\n"
    "Black and white = colour cursors not working.\nRed and blue swapped = channel order bug.",
    "Expect: a white dot with a soft grey\nshadow fading out around it.\n"
    "A hard black square or ring = alpha not working.",
    "Expect: a smooth rainbow square,\nhot spot in the centre.",
    "Expect: the cursor cycles red, orange,\nyellow, green, blue, purple.",
    "Expect: the normal arrow, as before.",
    "Expect: NO cursor while inside this tile.",
};

static uint32_t argb(int a, int r, int g, int b) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint32_t px_quadrants(int x, int y, int w, int h, int frame) {
    (void)frame;
    if (x == 0 || y == 0 || x == w - 1 || y == h - 1) return argb(255, 0, 0, 0);
    int right = x >= w / 2, bottom = y >= h / 2;
    if (!right && !bottom) return argb(255, 255, 0, 0);
    if (right && !bottom) return argb(255, 0, 255, 0);
    if (!right && bottom) return argb(255, 0, 0, 255);
    return argb(255, 255, 255, 0);
}

static uint32_t px_shadow(int x, int y, int w, int h, int frame) {
    (void)frame;
    double cx = w / 2.0, cy = h / 2.0, d = hypot(x + 0.5 - cx, y + 0.5 - cy);
    if (d < 8) return argb(255, 255, 255, 255);
    if (d < 9.5) return argb(255, 0, 0, 0);
    if (d < 22) return argb((int)(160 * (1.0 - (d - 9.5) / 12.5)), 0, 0, 0);
    return 0;
}

static uint32_t px_rainbow(int x, int y, int w, int h, int frame) {
    (void)frame;
    double t = (double)x / (w - 1) * 6.0;
    int seg = (int)t; double f = t - seg; int up = (int)(255 * f), dn = 255 - up;
    int r, g, b;
    switch (seg % 6) {
        case 0: r = 255; g = up; b = 0; break;
        case 1: r = dn; g = 255; b = 0; break;
        case 2: r = 0; g = 255; b = up; break;
        case 3: r = 0; g = dn; b = 255; break;
        case 4: r = up; g = 0; b = 255; break;
        default: r = 255; g = 0; b = dn; break;
    }
    double shade = 0.4 + 0.6 * (1.0 - (double)y / (h - 1));
    return argb(255, (int)(r * shade), (int)(g * shade), (int)(b * shade));
}

static uint32_t px_anim(int x, int y, int w, int h, int frame) {
    static const uint32_t colours[6] = {
        0xffff0000, 0xffff8000, 0xffffff00, 0xff00c000, 0xff0060ff, 0xffa000ff };
    double cx = w / 2.0, cy = h / 2.0, d = hypot(x + 0.5 - cx, y + 0.5 - cy);
    if (d < 11) return colours[frame % 6];
    if (d < 13) return argb(255, 0, 0, 0);
    return 0;
}

/* Build a 32-bit colour cursor (straight alpha) from a pixel function. */
static HCURSOR make_cursor(int w, int h, int hx, int hy, pixel_fn fn, int frame) {
    BITMAPV5HEADER bi = {0};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = w;
    bi.bV5Height = -h;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00ff0000;
    bi.bV5GreenMask = 0x0000ff00;
    bi.bV5BlueMask = 0x000000ff;
    bi.bV5AlphaMask = 0xff000000;

    void *bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP colour = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!colour || !bits) return LoadCursor(NULL, IDC_ARROW);

    uint32_t *p = (uint32_t *)bits;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            p[y * w + x] = fn(x, y, w, h, frame);

    HBITMAP mask = CreateBitmap(w, h, 1, 1, NULL);
    ICONINFO ii = {0};
    ii.fIcon = FALSE;
    ii.xHotspot = hx;
    ii.yHotspot = hy;
    ii.hbmMask = mask;
    ii.hbmColor = colour;
    HCURSOR c = (HCURSOR)CreateIconIndirect(&ii);
    DeleteObject(colour);
    DeleteObject(mask);
    return c ? c : LoadCursor(NULL, IDC_ARROW);
}

static RECT tile_rect(HWND hwnd, int i) {
    RECT rc; GetClientRect(hwnd, &rc);
    int tw = rc.right / COLS, th = (rc.bottom - GDI_STRIP_H) / ROWS;
    RECT r = { (i % COLS) * tw, (i / COLS) * th, (i % COLS + 1) * tw, (i / COLS + 1) * th };
    return r;
}

static int tile_at(HWND hwnd, POINT pt) {
    for (int i = 0; i < COLS * ROWS; i++) {
        RECT r = tile_rect(hwnd, i);
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

static HCURSOR cursor_for(int tile) {
    if (tile == 3) return g_anim[g_anim_frame % 6];
    if (tile == 5) return NULL;
    if (tile >= 0) return g_cursors[tile];
    return LoadCursor(NULL, IDC_ARROW);
}

static void text(HDC dc, int x, int y, const char *s) {
    TextOutA(dc, x, y, s, lstrlenA(s));
}

static void draw_gdi_strip(HDC dc, RECT strip) {
    HBRUSH bg = CreateSolidBrush(RGB(245, 245, 240));
    FillRect(dc, &strip, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    HFONT title = CreateFontA(-16, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    HFONT body = CreateFontA(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Tahoma");
    HFONT serif = CreateFontA(-20, 0, 0, 0, FW_NORMAL, TRUE, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Times New Roman");

    int x = strip.left + 12, y = strip.top + 8;
    SelectObject(dc, title);
    SetTextColor(dc, RGB(20, 20, 20));
    text(dc, x, y, "2D drawing check (must look the same as before the update)");
    SelectObject(dc, body);
    SetTextColor(dc, RGB(60, 60, 60));
    text(dc, x, y + 24, "Text, a gradient bar, a half-transparent box and a stretched checkerboard.");
    SelectObject(dc, serif);
    SetTextColor(dc, RGB(150, 30, 30));
    text(dc, x, y + 46, "Italic serif text - The quick brown fox");

    /* Gradient bar. */
    TRIVERTEX v[2] = {
        { x, y + 80, 0xff00, 0x0000, 0x0000, 0xff00 },
        { x + 360, y + 110, 0x0000, 0x0000, 0xff00, 0xff00 } };
    GRADIENT_RECT gr = { 0, 1 };
    GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_H);

    /* Half-transparent green box over the bar's right end. */
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, 1, 1);
    HGDIOBJ old = SelectObject(mem, bmp);
    SetPixel(mem, 0, 0, RGB(0, 200, 0));
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 128, 0 };
    AlphaBlend(dc, x + 260, y + 70, 160, 50, mem, 0, 0, 1, 1, bf);
    SelectObject(mem, old);
    DeleteObject(bmp);

    /* Stretched 4x4 checkerboard. */
    bmp = CreateCompatibleBitmap(dc, 4, 4);
    old = SelectObject(mem, bmp);
    for (int cy = 0; cy < 4; cy++)
        for (int cx = 0; cx < 4; cx++)
            SetPixel(mem, cx, cy, ((cx + cy) & 1) ? RGB(30, 30, 30) : RGB(230, 200, 60));
    StretchBlt(dc, x + 460, y + 30, 120, 120, mem, 0, 0, 4, 4, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);

    /* Outlined ellipse. */
    HPEN pen = CreatePen(PS_SOLID, 3, RGB(0, 90, 200));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, x + 610, y + 30, x + 760, y + 150);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);

    DeleteObject(title);
    DeleteObject(body);
    DeleteObject(serif);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
            g_hover = tile_at(hwnd, pt);
            SetCursor(cursor_for(g_hover));
            return TRUE;
        }
        break;
    case WM_TIMER:
        g_anim_frame++;
        if (g_hover == 3) SetCursor(cursor_for(3));
        return 0;
    case WM_SIZE:
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        static const COLORREF tints[COLS * ROWS] = {
            RGB(70, 70, 80), RGB(200, 200, 205), RGB(40, 60, 50),
            RGB(60, 50, 70), RGB(90, 90, 90), RGB(50, 70, 90) };
        HFONT big = CreateFontA(-18, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
        HFONT small = CreateFontA(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
        SetBkMode(dc, TRANSPARENT);
        for (int i = 0; i < COLS * ROWS; i++) {
            RECT r = tile_rect(hwnd, i);
            HBRUSH b = CreateSolidBrush(tints[i]);
            FillRect(dc, &r, b);
            DeleteObject(b);
            FrameRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
            COLORREF fg = i == 1 ? RGB(10, 10, 10) : RGB(240, 240, 240);
            SetTextColor(dc, fg);
            RECT t = { r.left + 10, r.top + 8, r.right - 8, r.bottom - 8 };
            SelectObject(dc, big);
            DrawTextA(dc, g_titles[i], -1, &t, DT_LEFT | DT_TOP | DT_WORDBREAK);
            t.top += 30;
            SelectObject(dc, small);
            DrawTextA(dc, g_expect[i], -1, &t, DT_LEFT | DT_TOP | DT_WORDBREAK);
        }
        DeleteObject(big);
        DeleteObject(small);
        RECT rc; GetClientRect(hwnd, &rc);
        RECT strip = { 0, rc.bottom - GDI_STRIP_H, rc.right, rc.bottom };
        draw_gdi_strip(dc, strip);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    (void)prev; (void)cmd;
    g_cursors[0] = make_cursor(32, 32, 0, 0, px_quadrants, 0);
    g_cursors[1] = make_cursor(48, 48, 24, 24, px_shadow, 0);
    g_cursors[2] = make_cursor(64, 64, 32, 32, px_rainbow, 0);
    for (int i = 0; i < 6; i++) g_anim[i] = make_cursor(32, 32, 16, 16, px_anim, i);
    g_cursors[3] = g_anim[0];
    g_cursors[4] = LoadCursor(NULL, IDC_ARROW);
    g_cursors[5] = NULL;

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "CursorTest";
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowA("CursorTest", "Cursor Test - hover each tile (Esc to quit)",
                              WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                              1024, 680, NULL, NULL, inst, NULL);
    ShowWindow(hwnd, SW_MAXIMIZE);
    SetTimer(hwnd, ANIM_TIMER, 300, NULL);

    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return 0;
}
