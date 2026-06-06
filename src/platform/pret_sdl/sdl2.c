#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <errno.h>
#include <signal.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <xinput.h>
#endif

#ifdef __PSP__
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspgu.h>
#endif

#include <SDL.h>

#include "global.h"
#include "core.h"
#include "lib/agb_flash/flash_internal.h"
#include "platform/shared/dma.h"
#include "platform/shared/input.h"
#include "platform/shared/video/gpsp_renderer.h"

#if ENABLE_AUDIO
#include "platform/shared/audio/cgb_audio.h"
#endif

#ifndef SA2_IOS
#define SA2_IOS 0
#endif

ALIGNED(256) uint16_t gameImage[DISPLAY_WIDTH * DISPLAY_HEIGHT];

#if ENABLE_VRAM_VIEW
uint16_t vramBuffer[VRAM_VIEW_WIDTH * VRAM_VIEW_HEIGHT];
#endif

SDL_Window *sdlWindow;
SDL_Renderer *sdlRenderer;
SDL_Texture *sdlTexture;
#if ENABLE_VRAM_VIEW
SDL_Window *vramWindow;
SDL_Renderer *vramRenderer;
SDL_Texture *vramTexture;
#endif
#define INITIAL_VIDEO_SCALE 1
unsigned int videoScale = INITIAL_VIDEO_SCALE;
unsigned int preFullscreenVideoScale = INITIAL_VIDEO_SCALE;

bool speedUp = false;
bool videoScaleChanged = false;
bool isRunning = true;
bool paused = false;
bool stepOneFrame = false;
bool headless = false;

#ifdef __PSP__
static SDL_Joystick *joystick = NULL;
static SDL_Rect pspDestRect;
#endif

double lastGameTime = 0;
double curGameTime = 0;
double fixedTimestep = 1.0 / 60.0; // 16.666667ms
double timeScale = 1.0;
double accumulator = 0.0;

static FILE *sSaveFile = NULL;

extern void AgbMain(void);
void DoSoftReset(void) {};

void ProcessSDLEvents(void);
void VDraw(SDL_Texture *texture);
void VramDraw(SDL_Texture *texture);

static void ReadSaveFile(char *path);
static void StoreSaveFile(void);
static void CloseSaveFile(void);

u16 Platform_GetKeyInput(void);

#if SA2_IOS
static void IosInitDiagnosticsAndSavePath(void);
static void IosLog(const char *fmt, ...);
static void IosInstallSignalHandlers(void);
static void IosHandleTouchEvent(const SDL_Event *event);
static void IosDrawTouchOverlay(void);
static u16 IosGetTouchKeys(void);
static void IosHeartbeat(void);
#endif

#ifdef _WIN32
void *Platform_malloc(size_t numBytes) { return HeapAlloc(GetProcessHeap(), HEAP_GENERATE_EXCEPTIONS | HEAP_ZERO_MEMORY, numBytes); }
void Platform_free(void *ptr) { HeapFree(GetProcessHeap(), 0, ptr); }
#endif

#if SA2_IOS
#define IOS_MAX_TOUCHES 12
#define IOS_LOG_PATH_MAX 1024

typedef struct {
    SDL_FingerID fingerId;
    float x;
    float y;
    bool active;
} IosTouch;

typedef struct {
    const char *label;
    float x;
    float y;
    float radius;
    u16 key;
} IosButton;

static IosTouch sIosTouches[IOS_MAX_TOUCHES];
static FILE *sIosLogFile = NULL;
static char sIosSavePath[IOS_LOG_PATH_MAX] = "sa2.sav";
static char sIosDiagnosticsDir[IOS_LOG_PATH_MAX] = "";
static Uint32 sIosLaunchTicks = 0;
static Uint32 sIosLastHeartbeatTicks = 0;
static u32 sIosFrameCounter = 0;
static u32 sIosHeartbeatCounter = 0;
static u16 sIosTouchKeys = 0;
static float sIosJoyKnobX = 74.0f;
static float sIosJoyKnobY = 172.0f;

static const IosButton sIosButtons[] = {
    { "A", 356.0f, 170.0f, 20.0f, A_BUTTON },
    { "B", 392.0f, 132.0f, 20.0f, B_BUTTON },
    { "L", 322.0f, 132.0f, 18.0f, L_BUTTON },
    { "R", 356.0f, 94.0f, 18.0f, R_BUTTON },
    { "ST", 386.0f, 32.0f, 15.0f, START_BUTTON },
    { "SE", 346.0f, 32.0f, 15.0f, SELECT_BUTTON },
};

static void IosEnsureDir(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return;
    }
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        SDL_Log("SA2 iOS: mkdir failed for %s: %s", path, strerror(errno));
    }
}

static void IosJoinPath(char *out, size_t outSize, const char *left, const char *right)
{
    if (left == NULL || left[0] == '\0') {
        snprintf(out, outSize, "%s", right);
        return;
    }

    size_t len = strlen(left);
    const char *slash = (len > 0 && left[len - 1] == '/') ? "" : "/";
    snprintf(out, outSize, "%s%s%s", left, slash, right);
}

static void IosWriteLatestCrashReport(int signalNumber)
{
    char reportPath[IOS_LOG_PATH_MAX];
    IosJoinPath(reportPath, sizeof(reportPath), sIosDiagnosticsDir, "LATEST_CRASH_OR_ABRUPT_EXIT_REPORT.txt");

    FILE *report = fopen(reportPath, "w");
    if (report == NULL) {
        return;
    }

    Uint32 ticks = SDL_GetTicks();
    fprintf(report, "build=sonic-advance-2-ios-initial\n");
    fprintf(report, "signal=%d\n", signalNumber);
    fprintf(report, "ticks=%u\n", ticks);
    fprintf(report, "runtime_ms=%u\n", ticks - sIosLaunchTicks);
    fprintf(report, "frame_counter=%u\n", sIosFrameCounter);
    fprintf(report, "heartbeat_counter=%u\n", sIosHeartbeatCounter);
    fprintf(report, "touch_keys=0x%04x\n", sIosTouchKeys);
    fprintf(report, "save_path=%s\n", sIosSavePath);
    fprintf(report, "diagnostics_dir=%s\n", sIosDiagnosticsDir);
    fprintf(report, "note=reopen the app once after a crash, then copy this file from Files > On My iPhone > SonicAdvance2 > SA2_DIAGNOSTICS\n");
    fclose(report);
}

static void IosSignalHandler(int signalNumber)
{
    IosWriteLatestCrashReport(signalNumber);
    if (sIosLogFile != NULL) {
        fprintf(sIosLogFile, "signal=%d ticks=%u frame_counter=%u heartbeat_counter=%u touch_keys=0x%04x\n", signalNumber,
                SDL_GetTicks(), sIosFrameCounter, sIosHeartbeatCounter, sIosTouchKeys);
        fflush(sIosLogFile);
    }
    signal(signalNumber, SIG_DFL);
    raise(signalNumber);
}

static void IosInstallSignalHandlers(void)
{
    signal(SIGABRT, IosSignalHandler);
    signal(SIGBUS, IosSignalHandler);
    signal(SIGFPE, IosSignalHandler);
    signal(SIGILL, IosSignalHandler);
    signal(SIGSEGV, IosSignalHandler);
}

static void IosLog(const char *fmt, ...)
{
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    SDL_Log("SA2 iOS: %s", line);
    if (sIosLogFile != NULL) {
        fprintf(sIosLogFile, "%s\n", line);
        fflush(sIosLogFile);
    }
}

static void IosInitDiagnosticsAndSavePath(void)
{
    sIosLaunchTicks = SDL_GetTicks();
    sIosLastHeartbeatTicks = sIosLaunchTicks;

    const char *home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        char documentsPath[IOS_LOG_PATH_MAX];
        IosJoinPath(documentsPath, sizeof(documentsPath), home, "Documents");
        IosEnsureDir(documentsPath);
        IosJoinPath(sIosDiagnosticsDir, sizeof(sIosDiagnosticsDir), documentsPath, "SA2_DIAGNOSTICS");
        IosEnsureDir(sIosDiagnosticsDir);
    } else {
        snprintf(sIosDiagnosticsDir, sizeof(sIosDiagnosticsDir), ".");
    }

    char currentPath[IOS_LOG_PATH_MAX];
    char previousPath[IOS_LOG_PATH_MAX];
    IosJoinPath(currentPath, sizeof(currentPath), sIosDiagnosticsDir, "CURRENT_SESSION_RUNTIME_LOG.txt");
    IosJoinPath(previousPath, sizeof(previousPath), sIosDiagnosticsDir, "PREVIOUS_SESSION_RUNTIME_LOG.txt");
    remove(previousPath);
    rename(currentPath, previousPath);
    sIosLogFile = fopen(currentPath, "w");

    char *prefPath = SDL_GetPrefPath("UlisesEstrad", "SonicAdvance2IOS");
    if (prefPath != NULL) {
        IosJoinPath(sIosSavePath, sizeof(sIosSavePath), prefPath, "sa2.sav");
        SDL_free(prefPath);
    } else {
        snprintf(sIosSavePath, sizeof(sIosSavePath), "sa2.sav");
    }

    IosLog("===== SA2 IOS RUN ticks=%u =====", sIosLaunchTicks);
    IosLog("build=sonic-advance-2-ios-initial");
    IosLog("save_path=%s", sIosSavePath);
    IosLog("diagnostics_dir=%s", sIosDiagnosticsDir);
}

static IosTouch *IosFindTouch(SDL_FingerID fingerId)
{
    for (int i = 0; i < IOS_MAX_TOUCHES; i++) {
        if (sIosTouches[i].active && sIosTouches[i].fingerId == fingerId) {
            return &sIosTouches[i];
        }
    }
    return NULL;
}

static IosTouch *IosAllocTouch(SDL_FingerID fingerId)
{
    IosTouch *touch = IosFindTouch(fingerId);
    if (touch != NULL) {
        return touch;
    }

    for (int i = 0; i < IOS_MAX_TOUCHES; i++) {
        if (!sIosTouches[i].active) {
            sIosTouches[i].active = true;
            sIosTouches[i].fingerId = fingerId;
            return &sIosTouches[i];
        }
    }
    return NULL;
}

static bool IosPointInButton(float x, float y, const IosButton *button)
{
    float dx = x - button->x;
    float dy = y - button->y;
    return (dx * dx + dy * dy) <= (button->radius * button->radius);
}

static void IosRecomputeTouchKeys(void)
{
    u16 oldKeys = sIosTouchKeys;
    u16 newKeys = 0;
    bool joystickActive = false;
    const float joyCenterX = 74.0f;
    const float joyCenterY = 172.0f;
    const float joyRadius = 48.0f;
    const float joyDeadzone = 12.0f;
    sIosJoyKnobX = joyCenterX;
    sIosJoyKnobY = joyCenterY;

    for (int i = 0; i < IOS_MAX_TOUCHES; i++) {
        if (!sIosTouches[i].active) {
            continue;
        }

        float x = sIosTouches[i].x;
        float y = sIosTouches[i].y;
        float dx = x - joyCenterX;
        float dy = y - joyCenterY;
        float dist2 = dx * dx + dy * dy;

        if (dist2 <= joyRadius * joyRadius) {
            joystickActive = true;
            if (dx > joyDeadzone) {
                newKeys |= DPAD_RIGHT;
            } else if (dx < -joyDeadzone) {
                newKeys |= DPAD_LEFT;
            }
            if (dy > joyDeadzone) {
                newKeys |= DPAD_DOWN;
            } else if (dy < -joyDeadzone) {
                newKeys |= DPAD_UP;
            }
            sIosJoyKnobX = x;
            sIosJoyKnobY = y;
        }

        for (unsigned int b = 0; b < ARRAY_COUNT(sIosButtons); b++) {
            if (IosPointInButton(x, y, &sIosButtons[b])) {
                newKeys |= sIosButtons[b].key;
            }
        }
    }

    if (!joystickActive) {
        sIosJoyKnobX = joyCenterX;
        sIosJoyKnobY = joyCenterY;
    }

    sIosTouchKeys = newKeys;
    if (oldKeys != newKeys) {
        IosLog("touch_keys old=0x%04x new=0x%04x", oldKeys, newKeys);
    }
}

static void IosHandleTouchEvent(const SDL_Event *event)
{
    if (event->type == SDL_FINGERDOWN || event->type == SDL_FINGERMOTION) {
        IosTouch *touch = IosAllocTouch(event->tfinger.fingerId);
        if (touch != NULL) {
            touch->x = event->tfinger.x * DISPLAY_WIDTH;
            touch->y = event->tfinger.y * DISPLAY_HEIGHT;
            IosLog("touch_%s id=%lld x=%.1f y=%.1f", event->type == SDL_FINGERDOWN ? "down" : "motion",
                   (long long)event->tfinger.fingerId, touch->x, touch->y);
        }
    } else if (event->type == SDL_FINGERUP) {
        IosTouch *touch = IosFindTouch(event->tfinger.fingerId);
        if (touch != NULL) {
            IosLog("touch_up id=%lld x=%.1f y=%.1f", (long long)event->tfinger.fingerId, touch->x, touch->y);
            touch->active = false;
        }
    }

    IosRecomputeTouchKeys();
}

static u16 IosGetTouchKeys(void) { return sIosTouchKeys; }

static const uint8_t *IosGlyphRows(char ch)
{
    static const uint8_t A[] = { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 };
    static const uint8_t B[] = { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E };
    static const uint8_t L[] = { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F };
    static const uint8_t R[] = { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 };
    static const uint8_t S[] = { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E };
    static const uint8_t T[] = { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 };
    static const uint8_t E[] = { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F };

    switch (ch) {
        case 'A': return A;
        case 'B': return B;
        case 'L': return L;
        case 'R': return R;
        case 'S': return S;
        case 'T': return T;
        case 'E': return E;
        default: return NULL;
    }
}

static void IosDrawText(SDL_Renderer *renderer, const char *text, int x, int y, int scale)
{
    SDL_Rect rect;
    rect.w = scale;
    rect.h = scale;

    for (int c = 0; text[c] != '\0'; c++) {
        const uint8_t *rows = IosGlyphRows(text[c]);
        if (rows != NULL) {
            for (int row = 0; row < 7; row++) {
                for (int col = 0; col < 5; col++) {
                    if (rows[row] & (1 << (4 - col))) {
                        rect.x = x + c * 6 * scale + col * scale;
                        rect.y = y + row * scale;
                        SDL_RenderFillRect(renderer, &rect);
                    }
                }
            }
        }
    }
}

static void IosDrawCircle(SDL_Renderer *renderer, int cx, int cy, int radius, bool filled)
{
    int r2 = radius * radius;
    for (int y = -radius; y <= radius; y++) {
        for (int x = -radius; x <= radius; x++) {
            int d2 = x * x + y * y;
            if ((filled && d2 <= r2) || (!filled && d2 <= r2 && d2 >= (radius - 2) * (radius - 2))) {
                SDL_RenderDrawPoint(renderer, cx + x, cy + y);
            }
        }
    }
}

static void IosDrawTouchOverlay(void)
{
    SDL_BlendMode oldBlendMode;
    SDL_GetRenderDrawBlendMode(sdlRenderer, &oldBlendMode);
    SDL_SetRenderDrawBlendMode(sdlRenderer, SDL_BLENDMODE_BLEND);

    SDL_SetRenderDrawColor(sdlRenderer, 0, 0, 0, 110);
    IosDrawCircle(sdlRenderer, 74, 172, 48, true);
    SDL_SetRenderDrawColor(sdlRenderer, 255, 255, 255, 180);
    IosDrawCircle(sdlRenderer, 74, 172, 48, false);
    SDL_SetRenderDrawColor(sdlRenderer, 255, 255, 255, 120);
    SDL_RenderDrawLine(sdlRenderer, 34, 172, 114, 172);
    SDL_RenderDrawLine(sdlRenderer, 74, 132, 74, 212);
    SDL_SetRenderDrawColor(sdlRenderer, 80, 180, 255, 190);
    IosDrawCircle(sdlRenderer, (int)sIosJoyKnobX, (int)sIosJoyKnobY, 16, true);

    for (unsigned int i = 0; i < ARRAY_COUNT(sIosButtons); i++) {
        const IosButton *button = &sIosButtons[i];
        SDL_SetRenderDrawColor(sdlRenderer, 0, 0, 0, 130);
        IosDrawCircle(sdlRenderer, (int)button->x, (int)button->y, (int)button->radius, true);
        SDL_SetRenderDrawColor(sdlRenderer, 255, 255, 255, 200);
        IosDrawCircle(sdlRenderer, (int)button->x, (int)button->y, (int)button->radius, false);
        int labelWidth = (int)strlen(button->label) * 12 - 2;
        IosDrawText(sdlRenderer, button->label, (int)(button->x - labelWidth / 2), (int)(button->y - 7), 2);
    }

    SDL_SetRenderDrawBlendMode(sdlRenderer, oldBlendMode);
}

static void IosHeartbeat(void)
{
    sIosFrameCounter++;
    Uint32 ticks = SDL_GetTicks();
    if (ticks - sIosLastHeartbeatTicks >= 5000) {
        sIosHeartbeatCounter++;
        IosLog("heartbeat=%u ticks=%u runtime_ms=%u frames=%u touch_keys=0x%04x audio_queue=%u", sIosHeartbeatCounter, ticks,
               ticks - sIosLaunchTicks, sIosFrameCounter, sIosTouchKeys, SDL_GetQueuedAudioSize(1));
        sIosLastHeartbeatTicks = ticks;
    }
}
#endif

#ifdef __PSP__
PSP_MODULE_INFO("SonicAdvance2", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

unsigned int sce_newlib_stack_size = 512 * 1024;

extern bool isRunning;

int exitCallback(int arg1, int arg2, void *common)
{
    (void)arg1;
    (void)arg2;
    (void)common;
    isRunning = false;
    return 0;
}

int callbackThread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    int cbid = sceKernelCreateCallback("Exit Callback", exitCallback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

int setupPspCallbacks(void)
{
    int thid = sceKernelCreateThread("update_thread", callbackThread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0) {
        sceKernelStartThread(thid, 0, 0);
    }
    return thid;
}
#endif

int main(int argc, char **argv)
{
#ifdef __PSP__
    setupPspCallbacks();
#endif

    const char *headlessEnv = getenv("HEADLESS");

    if (headlessEnv && strcmp(headlessEnv, "true") == 0) {
        headless = true;
    }

    const char *parentEnv = getenv("SIO_PARENT");

    if (parentEnv && strcmp(parentEnv, "true") == 0) {
        SIO_MULTI_CNT->id = 0;
        SIO_MULTI_CNT->si = 1;
        SIO_MULTI_CNT->sd = 1;
        SIO_MULTI_CNT->enable = false;
    }

    // Open an output console on Windows
#if (defined _WIN32) && (DEBUG != 0)
    AllocConsole();
    AttachConsole(GetCurrentProcessId());
    freopen("CON", "w", stdout);
#endif

#if !SA2_IOS
    ReadSaveFile("sa2.sav");
#endif

    // Prevent the multiplayer screen from being drawn ( see core.c:EngineInit() )
    REG_RCNT = 0x8000;
    REG_KEYINPUT = 0x3FF;

    if (headless) {
#if ENABLE_AUDIO
        // Required or it makes an infinite loop
        cgb_audio_init(48000);
#endif
        AgbMain();
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0) {
        fprintf(stderr, "SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }
#if SA2_IOS
    IosInitDiagnosticsAndSavePath();
    IosInstallSignalHandlers();
    IosLog("SDL_Init ok");
    ReadSaveFile(sIosSavePath);
#endif

#ifdef __PSP__
    if (SDL_NumJoysticks() > 0) {
        joystick = SDL_JoystickOpen(0);
    }
#endif

#ifdef TITLE_BAR
    const char *title = STR(TITLE_BAR);
#else
    const char *title = "SAT-R sa2";
#endif

#ifdef __PSP__
    sdlWindow = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 480, 272, SDL_WINDOW_SHOWN);
#else
    sdlWindow = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, DISPLAY_WIDTH * videoScale,
                                 DISPLAY_HEIGHT * videoScale, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
#endif
    if (sdlWindow == NULL) {
        fprintf(stderr, "Window could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }
#if SA2_IOS
    IosLog("SDL_CreateWindow success display=%dx%d", DISPLAY_WIDTH, DISPLAY_HEIGHT);
#endif

#if ENABLE_VRAM_VIEW
    int mainWindowX;
    int mainWindowWidth;
    SDL_GetWindowPosition(sdlWindow, &mainWindowX, NULL);
    SDL_GetWindowSize(sdlWindow, &mainWindowWidth, NULL);
    int vramWindowX = mainWindowX + mainWindowWidth;
    u16 vramWindowWidth = VRAM_VIEW_WIDTH;
    u16 vramWindowHeight = VRAM_VIEW_HEIGHT;
    vramWindow = SDL_CreateWindow("VRAM View", vramWindowX, SDL_WINDOWPOS_CENTERED, vramWindowWidth, vramWindowHeight,
                                  SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (vramWindow == NULL) {
        fprintf(stderr, "VRAM Window could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }
#endif

#ifdef __PSP__
    sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (sdlRenderer == NULL)
        sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, SDL_RENDERER_ACCELERATED);
    if (sdlRenderer == NULL)
        sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, 0);
#else
    sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, SDL_RENDERER_PRESENTVSYNC);
#endif
    if (sdlRenderer == NULL) {
        fprintf(stderr, "Renderer could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }
#if SA2_IOS
    IosLog("SDL_CreateRenderer success");
#endif

#if ENABLE_VRAM_VIEW
    vramRenderer = SDL_CreateRenderer(vramWindow, -1, SDL_RENDERER_PRESENTVSYNC);
    if (vramRenderer == NULL) {
        fprintf(stderr, "VRAM Renderer could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }
#endif

    SDL_SetRenderDrawColor(sdlRenderer, 0, 0, 0, 255);
    SDL_RenderClear(sdlRenderer);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
#ifdef __PSP__
    // SDL_RenderSetLogicalSize is broken on PSP, stretch to fill manually
    pspDestRect = (SDL_Rect) { 0, 0, GU_SCR_WIDTH, GU_SCR_HEIGHT };
#else
    SDL_RenderSetLogicalSize(sdlRenderer, DISPLAY_WIDTH, DISPLAY_HEIGHT);
#endif
#if ENABLE_VRAM_VIEW
    SDL_SetRenderDrawColor(vramRenderer, 0, 0, 0, 255);
    SDL_RenderClear(vramRenderer);
    SDL_RenderSetLogicalSize(vramRenderer, vramWindowWidth, vramWindowHeight);
#endif

    sdlTexture = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_ABGR1555, SDL_TEXTUREACCESS_STREAMING, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (sdlTexture == NULL) {
        fprintf(stderr, "Texture could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }
#if SA2_IOS
    IosLog("SDL_CreateTexture success");
#endif

#if ENABLE_VRAM_VIEW
    vramTexture = SDL_CreateTexture(vramRenderer, SDL_PIXELFORMAT_ABGR1555, SDL_TEXTUREACCESS_STREAMING, vramWindowWidth, vramWindowHeight);
    if (vramTexture == NULL) {
        fprintf(stderr, "Texture could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }
#endif

#if ENABLE_AUDIO
    SDL_AudioSpec want;

    SDL_memset(&want, 0, sizeof(want)); /* or SDL_zero(want) */
    want.freq = 48000;
    want.format = AUDIO_S16;
    want.channels = 2;
    want.samples = (want.freq / 60);
    cgb_audio_init(want.freq);

    if (SDL_OpenAudio(&want, 0) < 0) {
        SDL_Log("Failed to open audio: %s", SDL_GetError());
    } else {
        if (want.format != AUDIO_S16) /* we let this one thing change. */
            SDL_Log("We didn't get S16 audio format.");
        SDL_PauseAudio(0);
    }
#if SA2_IOS
    IosLog("audio init requested freq=%d channels=%d samples=%d queued=%u", want.freq, want.channels, want.samples, SDL_GetQueuedAudioSize(1));
#endif
#endif

    VDraw(sdlTexture);
#if ENABLE_VRAM_VIEW
    VramDraw(vramTexture);
#endif
#if SA2_IOS
    IosLog("runtime handoff start");
#endif
    AgbMain();

    return 0;
}

bool newFrameRequested = FALSE;

// called every gba frame. we process sdl events and render as many times
// as vsync needs, then return when a new game frame is needed.
void VBlankIntrWait(void)
{
#define HANDLE_VBLANK_INTRS()                                                                                                              \
    ({                                                                                                                                     \
        REG_DISPSTAT |= INTR_FLAG_VBLANK;                                                                                                  \
        RunDMAs(DMA_VBLANK);                                                                                                               \
        if (REG_DISPSTAT & DISPSTAT_VBLANK_INTR)                                                                                           \
            gIntrTable[INTR_INDEX_VBLANK]();                                                                                               \
        REG_DISPSTAT &= ~INTR_FLAG_VBLANK;                                                                                                 \
    })

    if (headless) {
        REG_VCOUNT = DISPLAY_HEIGHT + 1;
        HANDLE_VBLANK_INTRS();
        return;
    }

    bool frameAvailable = TRUE;
    bool frameDrawn = false;

    while (isRunning) {
#ifndef __PSP__
        ProcessSDLEvents();
#endif

        if (!paused || stepOneFrame) {
            double dt = fixedTimestep / timeScale; // TODO: Fix speedup

            // don't accumulate time if we already requested a new frame
            // this frame cycle (emulates threaded sdl behavior)
            if (!newFrameRequested) {
                double deltaTime = 0;

                curGameTime = SDL_GetPerformanceCounter();
                if (stepOneFrame) {
                    deltaTime = dt;
                } else {
                    deltaTime = (double)((curGameTime - lastGameTime) / (double)SDL_GetPerformanceFrequency());
                    if (deltaTime > (dt * 5))
                        deltaTime = dt * 5;
                }
                lastGameTime = curGameTime;

                accumulator += deltaTime;
            } else {
                newFrameRequested = FALSE;
            }

            while (accumulator >= dt) {
                REG_KEYINPUT = KEYS_MASK ^ Platform_GetKeyInput();
                if (frameAvailable) {
                    VDraw(sdlTexture);
                    frameAvailable = FALSE;
                    frameDrawn = true;

                    HANDLE_VBLANK_INTRS();

                    accumulator -= dt;
                } else {
                    newFrameRequested = TRUE;
                    return;
                }
            }

            if (paused && stepOneFrame) {
                stepOneFrame = false;
            }
        }

        // present
#ifdef __PSP__
        // manual blit since SDL_RenderSetLogicalSize doesn't work on psp
        SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, &pspDestRect);
        SDL_RenderPresent(sdlRenderer);
#else
        SDL_RenderClear(sdlRenderer);
        SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, NULL);
#if SA2_IOS
        IosDrawTouchOverlay();
        IosHeartbeat();
#endif

#if ENABLE_VRAM_VIEW
        VramDraw(vramTexture);
        SDL_RenderClear(vramRenderer);
        SDL_RenderCopy(vramRenderer, vramTexture, NULL, NULL);
#endif
        if (videoScaleChanged) {
            SDL_SetWindowSize(sdlWindow, DISPLAY_WIDTH * videoScale, DISPLAY_HEIGHT * videoScale);
            videoScaleChanged = false;
        }

        SDL_RenderPresent(sdlRenderer);
#if ENABLE_VRAM_VIEW
        SDL_RenderPresent(vramRenderer);
#endif
#endif
    }

#if SA2_IOS
    IosLog("clean shutdown frames=%u heartbeats=%u", sIosFrameCounter, sIosHeartbeatCounter);
#endif
    CloseSaveFile();

#if SA2_IOS
    if (sIosLogFile != NULL) {
        fclose(sIosLogFile);
        sIosLogFile = NULL;
    }
#endif

    SDL_DestroyWindow(sdlWindow);
    SDL_Quit();
#ifdef __PSP__
    sceKernelExitGame();
#endif
    exit(0);
#undef HANDLE_VBLANK_INTRS
}

static void ReadSaveFile(char *path)
{
    // Check whether the saveFile exists, and create it if not
    sSaveFile = fopen(path, "r+b");
    if (sSaveFile == NULL) {
        sSaveFile = fopen(path, "w+b");
    }

    fseek(sSaveFile, 0, SEEK_END);
    int fileSize = ftell(sSaveFile);
    fseek(sSaveFile, 0, SEEK_SET);

    // Only read as many bytes as fit inside the buffer
    // or as many bytes as are in the file
    int bytesToRead = (fileSize < sizeof(FLASH_BASE)) ? fileSize : sizeof(FLASH_BASE);

    int bytesRead = fread(FLASH_BASE, 1, bytesToRead, sSaveFile);

    // Fill the buffer if the savefile was just created or smaller than the buffer itself
    for (int i = bytesRead; i < sizeof(FLASH_BASE); i++) {
        FLASH_BASE[i] = 0xFF;
    }
}

static void StoreSaveFile()
{
    if (sSaveFile != NULL) {
        fseek(sSaveFile, 0, SEEK_SET);
        fwrite(FLASH_BASE, 1, sizeof(FLASH_BASE), sSaveFile);
    }
}

void Platform_StoreSaveFile(void) { StoreSaveFile(); }

static void CloseSaveFile()
{
    if (sSaveFile != NULL) {
        fclose(sSaveFile);
    }
}

static u16 keys;

// Key mappings
#define KEY_A_BUTTON      SDLK_c
#define KEY_B_BUTTON      SDLK_x
#define KEY_START_BUTTON  SDLK_RETURN
#define KEY_SELECT_BUTTON SDLK_BACKSLASH
#define KEY_L_BUTTON      SDLK_s
#define KEY_R_BUTTON      SDLK_d
#define KEY_DPAD_UP       SDLK_UP
#define KEY_DPAD_DOWN     SDLK_DOWN
#define KEY_DPAD_LEFT     SDLK_LEFT
#define KEY_DPAD_RIGHT    SDLK_RIGHT

#define HANDLE_KEYUP(key)                                                                                                                  \
    case KEY_##key:                                                                                                                        \
        keys &= ~key;                                                                                                                      \
        break;

#define HANDLE_KEYDOWN(key)                                                                                                                \
    case KEY_##key:                                                                                                                        \
        keys |= key;                                                                                                                       \
        break;

#ifdef __PSP__
#define BTN_TRIANGLE 0
#define BTN_CIRCLE   1
#define BTN_CROSS    2
#define BTN_SQUARE   3
#define BTN_LTRIGGER 4
#define BTN_RTRIGGER 5
#define BTN_DOWN     6
#define BTN_LEFT     7
#define BTN_UP       8
#define BTN_RIGHT    9
#define BTN_SELECT   10
#define BTN_START    11

static u16 PollJoystickButtons(void)
{
    u16 newKeys = 0;
    if (joystick == NULL)
        return newKeys;

    SDL_JoystickUpdate();

    if (SDL_JoystickGetButton(joystick, BTN_CROSS))
        newKeys |= A_BUTTON;
    if (SDL_JoystickGetButton(joystick, BTN_CIRCLE))
        newKeys |= B_BUTTON;
    if (SDL_JoystickGetButton(joystick, BTN_SQUARE))
        newKeys |= B_BUTTON; // Square also B
    if (SDL_JoystickGetButton(joystick, BTN_START))
        newKeys |= START_BUTTON;
    if (SDL_JoystickGetButton(joystick, BTN_SELECT))
        newKeys |= SELECT_BUTTON;
    if (SDL_JoystickGetButton(joystick, BTN_LTRIGGER))
        newKeys |= L_BUTTON;
    if (SDL_JoystickGetButton(joystick, BTN_RTRIGGER))
        newKeys |= R_BUTTON;
    if (SDL_JoystickGetButton(joystick, BTN_UP))
        newKeys |= DPAD_UP;
    if (SDL_JoystickGetButton(joystick, BTN_DOWN))
        newKeys |= DPAD_DOWN;
    if (SDL_JoystickGetButton(joystick, BTN_LEFT))
        newKeys |= DPAD_LEFT;
    if (SDL_JoystickGetButton(joystick, BTN_RIGHT))
        newKeys |= DPAD_RIGHT;

    return newKeys;
}

#endif

u32 fullScreenFlags = 0;
static SDL_DisplayMode sdlDispMode = { 0 };

void Platform_QueueAudio(const s16 *data, uint32_t bytesCount)
{
    if (headless) {
        return;
    }
    // Reset the audio buffer if we are 10 frames out of sync
    // If this happens it suggests there was some OS level lag
    // in playing audio. The queue length should remain stable at < 10 otherwise
    if (SDL_GetQueuedAudioSize(1) > (bytesCount * 10)) {
        SDL_ClearQueuedAudio(1);
    }

    SDL_QueueAudio(1, data, bytesCount);
    // printf("Queueing %d\n, QueueSize %d\n", bytesCount, SDL_GetQueuedAudioSize(1));
}

void ProcessSDLEvents(void)
{
    SDL_Event event;

    while (SDL_PollEvent(&event)) {
        SDL_Keycode keyCode = event.key.keysym.sym;
        Uint16 keyMod = event.key.keysym.mod;

        switch (event.type) {
            case SDL_QUIT:
#if SA2_IOS
                IosLog("event=SDL_QUIT");
#endif
                isRunning = false;
                break;
#if SA2_IOS
            case SDL_APP_WILLENTERBACKGROUND:
                IosLog("event=SDL_APP_WILLENTERBACKGROUND");
                break;
            case SDL_APP_DIDENTERBACKGROUND:
                IosLog("event=SDL_APP_DIDENTERBACKGROUND");
                break;
            case SDL_APP_WILLENTERFOREGROUND:
                IosLog("event=SDL_APP_WILLENTERFOREGROUND");
                break;
            case SDL_APP_DIDENTERFOREGROUND:
                IosLog("event=SDL_APP_DIDENTERFOREGROUND");
                break;
            case SDL_APP_TERMINATING:
                IosLog("event=SDL_APP_TERMINATING");
                break;
            case SDL_FINGERDOWN:
            case SDL_FINGERMOTION:
            case SDL_FINGERUP:
                IosHandleTouchEvent(&event);
                break;
#endif
            case SDL_KEYUP:
                switch (event.key.keysym.sym) {
                    HANDLE_KEYUP(A_BUTTON)
                    HANDLE_KEYUP(B_BUTTON)
                    HANDLE_KEYUP(START_BUTTON)
                    HANDLE_KEYUP(SELECT_BUTTON)
                    HANDLE_KEYUP(L_BUTTON)
                    HANDLE_KEYUP(R_BUTTON)
                    HANDLE_KEYUP(DPAD_UP)
                    HANDLE_KEYUP(DPAD_DOWN)
                    HANDLE_KEYUP(DPAD_LEFT)
                    HANDLE_KEYUP(DPAD_RIGHT)
                    case SDLK_SPACE:
                        if (speedUp) {
                            speedUp = false;
                            timeScale = 1.0;
                            SDL_ClearQueuedAudio(1);
                            SDL_PauseAudio(0);
                        }
                        break;
                }
                break;
            case SDL_KEYDOWN:
                if (keyCode == SDLK_RETURN && (keyMod & KMOD_ALT)) {
                    fullScreenFlags ^= SDL_WINDOW_FULLSCREEN_DESKTOP;
                    if (fullScreenFlags & SDL_WINDOW_FULLSCREEN_DESKTOP) {
                        SDL_GetWindowDisplayMode(sdlWindow, &sdlDispMode);
                        preFullscreenVideoScale = videoScale;
                    } else {
                        SDL_SetWindowDisplayMode(sdlWindow, &sdlDispMode);
                        videoScale = preFullscreenVideoScale;
                    }
                    SDL_SetWindowFullscreen(sdlWindow, fullScreenFlags);

                    SDL_SetWindowSize(sdlWindow, DISPLAY_WIDTH * videoScale, DISPLAY_HEIGHT * videoScale);
                    videoScaleChanged = FALSE;
                } else
                    switch (event.key.keysym.sym) {
                        HANDLE_KEYDOWN(A_BUTTON)
                        HANDLE_KEYDOWN(B_BUTTON)
                        HANDLE_KEYDOWN(START_BUTTON)
                        HANDLE_KEYDOWN(SELECT_BUTTON)
                        HANDLE_KEYDOWN(L_BUTTON)
                        HANDLE_KEYDOWN(R_BUTTON)
                        HANDLE_KEYDOWN(DPAD_UP)
                        HANDLE_KEYDOWN(DPAD_DOWN)
                        HANDLE_KEYDOWN(DPAD_LEFT)
                        HANDLE_KEYDOWN(DPAD_RIGHT)
                        case SDLK_r:
                            if (event.key.keysym.mod & (KMOD_LCTRL | KMOD_RCTRL)) {
                                DoSoftReset();
                            }
                            break;
                        case SDLK_p:
                            if (event.key.keysym.mod & (KMOD_LCTRL | KMOD_RCTRL)) {
                                paused = !paused;
                            }
                            break;
                        case SDLK_SPACE:
                            if (!speedUp) {
                                speedUp = true;
                                timeScale = SPEEDUP_SCALE;
                                SDL_PauseAudio(1);
                            }
                            break;
                        case SDLK_F10:
                            paused = true;
                            stepOneFrame = true;
                            break;
                    }
                break;
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    unsigned int w = event.window.data1;
                    unsigned int h = event.window.data2;

                    videoScale = 0;
                    if (w / DISPLAY_WIDTH > videoScale)
                        videoScale = w / DISPLAY_WIDTH;
                    if (h / DISPLAY_HEIGHT > videoScale)
                        videoScale = h / DISPLAY_HEIGHT;
                    if (videoScale < 1)
                        videoScale = 1;

                    videoScaleChanged = true;
                }
                break;
        }
    }
}

u16 Platform_GetKeyInput(void)
{
#ifdef _WIN32
    SharedKeys gamepadKeys = GetXInputKeys();

    speedUp = (gamepadKeys & KEY_SPEEDUP) ? true : false;

    if (speedUp) {
        timeScale = SPEEDUP_SCALE;
        SDL_PauseAudio(1);
    } else {
        timeScale = 1.0f;
        SDL_PauseAudio(0);
    }

    return (gamepadKeys != 0) ? gamepadKeys : keys;
#endif

#ifdef __PSP__
    return keys | PollJoystickButtons();
#endif

#if SA2_IOS
    return keys | IosGetTouchKeys();
#endif

    return keys;
}

#if ENABLE_VRAM_VIEW
void VramDraw(SDL_Texture *texture)
{
    memset(vramBuffer, 0, sizeof(vramBuffer));
    gpsp_draw_vram_view(vramBuffer);
    SDL_UpdateTexture(texture, NULL, vramBuffer, VRAM_VIEW_WIDTH * sizeof(Uint16));
}
#endif

void VDraw(SDL_Texture *texture)
{
    gpsp_draw_frame(gameImage);
    SDL_UpdateTexture(texture, NULL, gameImage, DISPLAY_WIDTH * sizeof(Uint16));
    REG_VCOUNT = DISPLAY_HEIGHT + 1; // prep for being in VBlank period
}
