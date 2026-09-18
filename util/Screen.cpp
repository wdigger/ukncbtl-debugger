// Screen.cpp -- the machine's screen, in a window (see Screen.h)

#include "stdafx.h"
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "Emulator.h"
#include "Screen.h"

//////////////////////////////////////////////////////////////////////
// The picture as a file
//
// Nothing here is SDL's: the machine's screen is drawn from its own
// memory (Emulator_PrepareScreenRGB32) and written out as the simplest
// thing every viewer reads.  A build without SDL3 has this much.

namespace {

void AppendWord(std::string& out, uint16_t value)
{
    out.push_back((char)(value & 0xff));
    out.push_back((char)(value >> 8));
}

void AppendLong(std::string& out, uint32_t value)
{
    AppendWord(out, (uint16_t)(value & 0xffff));
    AppendWord(out, (uint16_t)(value >> 16));
}

}  // namespace

bool Screen_Save(const char* path)
{
    const int width = UKNC_SCREEN_WIDTH;
    const int height = UKNC_SCREEN_HEIGHT;
    // Three bytes a pixel, and a BMP row is padded to four -- 1920 is a
    // multiple of four already, but the padding is written rather than
    // assumed away, since the screen's width is not this file's to fix.
    const int stride = ((width * 3) + 3) & ~3;

    std::vector<uint32_t> pixels((size_t)width * height);
    Emulator_PrepareScreenRGB32(pixels.data(), Emulator_GetPalette());

    std::string header;
    const uint32_t pixelsOffset = 14 + 40;
    const uint32_t fileSize = pixelsOffset + (uint32_t)stride * height;

    header.push_back('B');
    header.push_back('M');
    AppendLong(header, fileSize);
    AppendLong(header, 0);
    AppendLong(header, pixelsOffset);

    AppendLong(header, 40);              // The header that follows
    AppendLong(header, (uint32_t)width);
    AppendLong(header, (uint32_t)height);
    AppendWord(header, 1);               // Planes
    AppendWord(header, 24);              // Bits per pixel
    AppendLong(header, 0);               // Not compressed
    AppendLong(header, (uint32_t)stride * height);
    AppendLong(header, 2835);            // Pixels per metre, both ways:
    AppendLong(header, 2835);            // 72 dpi, which nothing reads
    AppendLong(header, 0);               // Colours used
    AppendLong(header, 0);               // Colours that matter

    FILE* file = fopen(path, "wb");
    if (file == nullptr)
        return false;

    bool okWritten = fwrite(header.data(), header.size(), 1, file) == 1;

    // Bottom row first, and blue before red: a BMP is written the way a
    // screen is scanned out, upside down.
    std::vector<uint8_t> row((size_t)stride, 0);
    for (int y = height - 1; okWritten && y >= 0; y--)
    {
        const uint32_t* line = pixels.data() + (size_t)y * width;
        for (int x = 0; x < width; x++)
        {
            row[(size_t)x * 3 + 0] = (uint8_t)(line[x] & 0xff);
            row[(size_t)x * 3 + 1] = (uint8_t)((line[x] >> 8) & 0xff);
            row[(size_t)x * 3 + 2] = (uint8_t)((line[x] >> 16) & 0xff);
        }
        okWritten = fwrite(row.data(), row.size(), 1, file) == 1;
    }

    if (fclose(file) != 0)
        okWritten = false;
    return okWritten;
}

//////////////////////////////////////////////////////////////////////
// The picture in a window

#ifdef HAVE_SDL3

#include <SDL3/SDL.h>

namespace {

// The machine's 640 x 288 pixels are not square -- they are taller than
// they are wide -- so the window is 960 x 576: one and a half times
// across and twice down, the proportions UKNCBTL's own windows show
// them in.  The renderer stretches the picture into that and keeps its
// shape whatever the window is then resized to.
const int kWindowWidth = 960;
const int kWindowHeight = 576;

// What the machine's own screen runs at, and so what a window showing
// it has to run at: 50 frames a second, 20 milliseconds each.
const uint64_t kFrameNanoseconds = 20 * 1000 * 1000;

// Set when the window is closed from the desktop; read through
// Screen_QuitRequested() by everything that waits or runs.
bool g_okQuitRequested = false;

SDL_Window* g_window = nullptr;
SDL_Renderer* g_renderer = nullptr;
SDL_Texture* g_texture = nullptr;

uint32_t g_pixels[UKNC_SCREEN_WIDTH * UKNC_SCREEN_HEIGHT];

// When the next frame is due, so that the machine runs at the speed its
// screen implies rather than as fast as this computer can manage.
uint64_t g_nextFrameAt = 0;

// And when the last one was actually drawn.  A stretch that is not
// paced -- bringing the operating system up, mostly -- runs through
// emulated frames far faster than any screen could show them, and
// drawing one that is replaced a moment later costs real time for
// nothing: on this machine a thousand frames of booting took ten
// seconds to show and one second to run.  So frames are drawn at a
// hundred a second at most, which no eye and no paced run can tell
// from every one.
uint64_t g_lastFrameAt = 0;
const uint64_t kMinFrameNanoseconds = 10 * 1000 * 1000;

// Whatever the window manager has to say.  Closing the window is the
// one thing it can say that means anything here, and what it means is
// "enough": the machine stops, and whoever waits or runs sees that
// through Screen_QuitRequested() (see Screen.h).
void HandleEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        if (event.type == SDL_EVENT_QUIT
            || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        {
            std::wcout << L"Screen closed; stopping." << std::endl;
            g_okQuitRequested = true;
            Screen_Close();
            return;
        }
    }
}

}  // namespace

bool Screen_Open()
{
    if (g_window != nullptr)
        return true;

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::cout << "ukncbtldebug: no screen: " << SDL_GetError() << std::endl;
        return false;
    }

    // Resizable, and the renderer keeps the shape of what it shows, so
    // how big the picture is left to whoever is looking at it.
    g_window = SDL_CreateWindow("\xd0\xa3\xd0\x9a\xd0\x9d\xd0\xa6",  // УКНЦ
                                kWindowWidth, kWindowHeight,
                                SDL_WINDOW_RESIZABLE);
    if (g_window != nullptr)
        g_renderer = SDL_CreateRenderer(g_window, nullptr);
    if (g_renderer != nullptr)
    {
        // Streaming, because every frame is a fresh one: the machine's
        // screen is a list of tags walked from scratch each time, not a
        // bitmap with a few pixels changed (see Emulator.cpp).
        g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_XRGB8888,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      UKNC_SCREEN_WIDTH, UKNC_SCREEN_HEIGHT);
    }
    if (g_texture == nullptr)
    {
        std::cout << "ukncbtldebug: no screen: " << SDL_GetError() << std::endl;
        Screen_Close();
        return false;
    }

    // Nearest, not smoothed: these pixels are meant to be seen as
    // pixels, and text drawn eight dots wide turns to mush otherwise.
    SDL_SetTextureScaleMode(g_texture, SDL_SCALEMODE_NEAREST);
    // Not waiting for the display's own refresh: the machine's frame is
    // 1/50 second and Screen_Pace() is what keeps to it, while a stretch
    // that is deliberately not paced -- bringing the operating system
    // up -- should go as fast as it can rather than at the speed of
    // whatever monitor happens to be attached.
    SDL_SetRenderVSync(g_renderer, 0);
    // The renderer works in the window's own shape and letterboxes what
    // is left over, so the picture keeps its proportions at any size.
    SDL_SetRenderLogicalPresentation(g_renderer, kWindowWidth, kWindowHeight,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);

    g_nextFrameAt = SDL_GetTicksNS();
    Screen_Frame();
    return true;
}

void Screen_Frame()
{
    if (g_window == nullptr)
        return;

    uint64_t now = SDL_GetTicksNS();
    if (now - g_lastFrameAt < kMinFrameNanoseconds)
        return;
    g_lastFrameAt = now;

    Emulator_PrepareScreenRGB32(g_pixels, Emulator_GetPalette());
    SDL_UpdateTexture(g_texture, nullptr, g_pixels,
                      UKNC_SCREEN_WIDTH * (int)sizeof (uint32_t));

    const SDL_FRect whole = { 0.0f, 0.0f, (float)kWindowWidth,
                              (float)kWindowHeight };
    SDL_RenderClear(g_renderer);
    SDL_RenderTexture(g_renderer, g_texture, nullptr, &whole);
    SDL_RenderPresent(g_renderer);

    HandleEvents();
}

void Screen_Pace()
{
    if (g_window == nullptr)
        return;

    // A machine running at twenty times life size is no use to watch,
    // and a window is the only reason to hold it back -- so this is
    // where it is held.  Falling behind is not caught up on: the clock
    // is reset instead, so a slow stretch costs what it costs and
    // nothing races afterwards to make up for it.
    uint64_t now = SDL_GetTicksNS();
    if (now < g_nextFrameAt)
    {
        SDL_DelayNS(g_nextFrameAt - now);
        g_nextFrameAt += kFrameNanoseconds;
    }
    else
    {
        g_nextFrameAt = now + kFrameNanoseconds;
    }
}

void Screen_Idle()
{
    if (g_window == nullptr)
        return;

    HandleEvents();
    if (g_window == nullptr)
        return;

    // The picture has not changed -- nothing is running -- but it has to
    // be presented again all the same: a window that is uncovered, or
    // moved to another display, is redrawn from what it was last given.
    SDL_RenderPresent(g_renderer);
    g_nextFrameAt = SDL_GetTicksNS();
}

bool Screen_IsOpen()
{
    return g_window != nullptr;
}

bool Screen_QuitRequested()
{
    return g_okQuitRequested;
}

void Screen_Close()
{
    if (g_texture != nullptr)
    {
        SDL_DestroyTexture(g_texture);
        g_texture = nullptr;
    }
    if (g_renderer != nullptr)
    {
        SDL_DestroyRenderer(g_renderer);
        g_renderer = nullptr;
    }
    if (g_window != nullptr)
    {
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
    }
    SDL_Quit();
}

#else  // HAVE_SDL3

// Built without SDL3: the machine runs, and nobody watches it.

bool Screen_Open()
{
    std::cout << "ukncbtldebug: built without SDL3, so there is no screen"
              << std::endl;
    return false;
}

void Screen_Frame() {}
void Screen_Pace() {}
void Screen_Idle() {}
bool Screen_IsOpen() { return false; }
bool Screen_QuitRequested() { return false; }
void Screen_Close() {}

#endif  // HAVE_SDL3
