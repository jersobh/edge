#include "xfsr_controls.hpp"

#include "common.hpp"

#if !defined(_WIN32)
#include <cerrno>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <linux/input.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace xfsr_ctrl {

std::atomic<uint64_t> g_presentFrames{0};

namespace {

// --- 8×8 ASCII font (32..127), same glyph set as demo HudOverlay ---
static const uint8_t kFont8[96][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00},
    {0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00},
    {0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00},
    {0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00},
    {0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00},
    {0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00},
    {0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00},
    {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00},
    {0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06},
    {0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00},
    {0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00},
    {0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00},
    {0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00},
    {0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00},
    {0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00},
    {0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00},
    {0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00},
    {0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00},
    {0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00},
    {0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00},
    {0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00},
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00},
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06},
    {0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00},
    {0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00},
    {0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00},
    {0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00},
    {0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00},
    {0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00},
    {0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00},
    {0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00},
    {0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00},
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00},
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00},
    {0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00},
    {0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00},
    {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    {0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00},
    {0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00},
    {0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00},
    {0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00},
    {0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00},
    {0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00},
    {0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00},
    {0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00},
    {0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00},
    {0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00},
    {0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00},
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},
    {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00},
    {0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00},
    {0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00},
    {0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00},
    {0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00},
    {0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00},
    {0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00},
    {0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF},
    {0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00},
    {0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00},
    {0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00},
    {0x38, 0x30, 0x30, 0x3e, 0x33, 0x33, 0x6E, 0x00},
    {0x00, 0x00, 0x1E, 0x33, 0x3f, 0x03, 0x1E, 0x00},
    {0x1C, 0x36, 0x06, 0x0f, 0x06, 0x06, 0x0F, 0x00},
    {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F},
    {0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00},
    {0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    {0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E},
    {0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00},
    {0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    {0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00},
    {0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00},
    {0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00},
    {0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F},
    {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78},
    {0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00},
    {0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00},
    {0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00},
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00},
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},
    {0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00},
    {0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00},
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F},
    {0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00},
    {0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00},
    {0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00},
    {0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00},
    {0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
};

constexpr int kPresetCount = 4;
const char* kPresetNames[kPresetCount] = {"quality", "balanced", "performance", "ultra"};
constexpr float kPresetScales[kPresetCount] = {1.5f, 1.7f, 2.0f, 2.5f};

inline const char* presetLabel(float scale, int idx) {
    if (scale < 1.35f) return "in-game";
    return kPresetNames[idx];
}

// X11 keysyms (avoid X11 headers — dlopen only).
constexpr unsigned long kXK_F1 = 0xffbe;
constexpr unsigned long kXK_F2 = 0xffbf;
constexpr unsigned long kXK_F3 = 0xffc0;
constexpr unsigned long kXK_F4 = 0xffc1;
constexpr unsigned long kXK_F5 = 0xffc2;
constexpr unsigned long kXK_F6 = 0xffc3;
constexpr unsigned long kXK_F10 = 0xffc7;
constexpr unsigned long kXK_Shift_L = 0xffe1;
constexpr unsigned long kXK_Shift_R = 0xffe2;
constexpr unsigned long kXK_Control_L = 0xffe3;
constexpr unsigned long kXK_Control_R = 0xffe4;
constexpr unsigned long kXK_Alt_L = 0xffe9;
constexpr unsigned long kXK_Alt_R = 0xffea;
constexpr unsigned long kXK_Insert = 0xff63;
constexpr unsigned long kXK_Delete = 0xffff;
constexpr unsigned long kXK_Home = 0xff50;
constexpr unsigned long kXK_End = 0xff57;
constexpr unsigned long kXK_Page_Up = 0xff55;
constexpr unsigned long kXK_Page_Down = 0xff56;
constexpr unsigned long kXK_Pause = 0xff13;

struct KeyBits {
    bool f1 = false, f2 = false, f3 = false, f4 = false, f5 = false, f6 = false, f10 = false;
    bool ins = false, del = false, home = false, end = false, pgup = false, pgdn = false, pause = false;
    bool shift = false, ctrl = false, alt = false;
};

struct Runtime {
    std::mutex mu;
    bool inited = false;
    bool hudOn = true;
    HotkeyMode hotkeyMode = HotkeyMode::Shift; // Safe default: Shift+F* (no in-game F1/F2 conflict)
    int presetIdx = 0;
    float scale = 1.5f;
    bool rcas = true;
    Upscaler upscaler = Upscaler::Fsr1;
    float edgeSharpness = 0.60f;
    bool edgePerf = false;
    int edgeMode = 0; // 0=quality, 1=perf, 2=nearest
    FanMode fanMode = FanMode::Auto;
    int fanPct = -1; // -1 for auto, 0..100 for fixed/custom
    std::chrono::steady_clock::time_point lastAutoOptChange{};
    bool offload = false;
    bool oneWay = true;
    bool oneWayForce = false;
    bool overlayPinned = false;
    bool recreate = false;
    bool reconfigureOffload = false;
    bool retryOffload = false;
    // ROADMAP #1 — dynamic resolution (continuous float scale).
    bool adaptive = false;
    float targetFps = 60.f;
    float emaFtMs = 0.f; // present-to-present EMA
    bool haveEma = false;
    uint32_t lastDisplayW = 0;
    uint32_t lastDisplayH = 0;
    uint32_t lastRenderW = 0;
    uint32_t lastRenderH = 0;
    char presentMode[32] = "—"; // identity | proxy-edge | hybrid-edge | …
    char dgpuName[64] = "";
    char igpuName[64] = "";
    bool hasDgpu = false;
    bool hasIgpu = false;
    bool dgpuIsDiscrete = true;
    std::chrono::steady_clock::time_point lastPresent{};
    std::chrono::steady_clock::time_point lastScaleChange{};
    std::chrono::steady_clock::time_point toastUntil{};
    char toast[160]{};
    char controlPath[512]{};
    char statusPath[512]{};
    time_t controlMtime = 0;
    bool loggedKeys = false;
    bool loggedAdaptive = false;
};

const char* upscalerName(Upscaler u) {
    switch (u) {
    case Upscaler::Blit:
        return "blit";
    case Upscaler::Nis:
        return "nis";
    case Upscaler::Edge:
        return "edge";
    case Upscaler::Nearest:
        return "nearest";
    default:
        return "fsr1";
    }
}

Upscaler parseUpscaler(const char* s) {
    if (!s || !s[0]) return Upscaler::Fsr1;
    if (std::strcmp(s, "blit") == 0 || std::strcmp(s, "0") == 0) return Upscaler::Blit;
    if (std::strcmp(s, "nis") == 0 || std::strcmp(s, "2") == 0) return Upscaler::Nis;
    if (std::strcmp(s, "edge") == 0 || std::strcmp(s, "3") == 0) return Upscaler::Edge;
    if (std::strcmp(s, "nearest") == 0 || std::strcmp(s, "nn") == 0 || std::strcmp(s, "pixel") == 0 ||
        std::strcmp(s, "4") == 0)
        return Upscaler::Nearest;
    if (std::strcmp(s, "fsr1") == 0 || std::strcmp(s, "fsr") == 0 || std::strcmp(s, "1") == 0)
        return Upscaler::Fsr1;
    return Upscaler::Fsr1;
}

uint32_t renderDim(uint32_t display, float scale) {
    const float s = (scale < 0.125f || (scale >= 0.99f && scale <= 1.01f)) ? 1.f : scale;
    uint32_t r = static_cast<uint32_t>(static_cast<float>(display) / s + 0.5f);
    if (r < 64) r = 64;
    return r;
}

Runtime g_rt;

#if !defined(_WIN32)
using XOpenDisplay_fn = void* (*)(const char*);
using XCloseDisplay_fn = int (*)(void*);
using XQueryKeymap_fn = int (*)(void*, char[32]);
using XKeysymToKeycode_fn = unsigned char (*)(void*, unsigned long);

struct X11 {
    void* lib = nullptr;
    void* dpy = nullptr;
    XOpenDisplay_fn OpenDisplay = nullptr;
    XCloseDisplay_fn CloseDisplay = nullptr;
    XQueryKeymap_fn QueryKeymap = nullptr;
    XKeysymToKeycode_fn KeysymToKeycode = nullptr;
    unsigned char kcF1 = 0, kcF2 = 0, kcF3 = 0, kcF4 = 0, kcF5 = 0, kcF6 = 0, kcF10 = 0;
    unsigned char kcShiftL = 0, kcShiftR = 0;
    unsigned char kcCtrlL = 0, kcCtrlR = 0;
    unsigned char kcAltL = 0, kcAltR = 0;
    unsigned char kcIns = 0, kcDel = 0, kcHome = 0, kcEnd = 0, kcPgUp = 0, kcPgDn = 0, kcPause = 0;
    bool ready = false;
    bool gaveUp = false;
    int failTries = 0;
};

X11 g_x11;

struct Evdev {
    std::vector<int> fds;
    bool probed = false;
    bool anyOpen = false;
};

Evdev g_evdev;
#endif
KeyBits g_prevKeys{};

int presetIndexFromName(const char* p) {
    if (!p) return 0;
    if (std::strcmp(p, "ultra") == 0 || std::strcmp(p, "U") == 0 ||
        std::strcmp(p, "extreme") == 0)
        return 3;
    if (std::strcmp(p, "performance") == 0 || std::strcmp(p, "perf") == 0 ||
        std::strcmp(p, "E") == 0)
        return 2;
    if (std::strcmp(p, "balanced") == 0 || std::strcmp(p, "W") == 0) return 1;
    return 0;
}

int presetIndexFromScale(float s) {
    int best = 0;
    float bestD = 1e9f;
    for (int i = 0; i < kPresetCount; ++i) {
        float d = s - kPresetScales[i];
        if (d < 0) d = -d;
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

void trimInPlace(char* s) {
    if (!s) return;
    char* a = s;
    while (*a == ' ' || *a == '\t' || *a == '\r' || *a == '\n') ++a;
    if (a != s) std::memmove(s, a, std::strlen(a) + 1);
    size_t n = std::strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
}

struct FanWorker {
    std::mutex mu;
    std::condition_variable cv;
    bool hasNbfc = false;
    bool probed = false;
    bool running = false;
    bool stop = false;
    bool dirty = false;
    int targetPct = -1; // -1: auto, 0..100: percentage
    int lastAppliedPct = -999;
    std::thread worker;

    void probe() {
        if (probed) return;
        probed = true;
#if !defined(_WIN32)
        hasNbfc = (::access("/usr/bin/nbfc", X_OK) == 0);
        if (!hasNbfc) {
            FILE* p = ::popen("which nbfc 2>/dev/null", "r");
            if (p) {
                char buf[128]{};
                if (std::fgets(buf, sizeof(buf), p)) {
                    trimInPlace(buf);
                    if (buf[0]) hasNbfc = true;
                }
                ::pclose(p);
            }
        }
#endif
        if (hasNbfc && !running) {
            running = true;
            worker = std::thread([this]() {
                std::unique_lock<std::mutex> lock(mu);
                while (!stop) {
                    cv.wait(lock, [this]() { return stop || dirty; });
                    if (stop) break;
                    dirty = false;
                    const int pct = targetPct;
                    lock.unlock();

                    if (pct != lastAppliedPct) {
                        lastAppliedPct = pct;
                        char cmd[160]{};
                        if (pct < 0) {
                            std::snprintf(cmd, sizeof(cmd),
                                          "nbfc set -f 0 -a >/dev/null 2>&1 && nbfc set -f 1 -a >/dev/null 2>&1");
                        } else {
                            const int speed = std::clamp(pct, 10, 100);
                            std::snprintf(cmd, sizeof(cmd),
                                          "nbfc set -f 0 -s %d >/dev/null 2>&1 && nbfc set -f 1 -s %d >/dev/null 2>&1",
                                          speed, speed);
                        }
                        ::system(cmd);
                    }

                    lock.lock();
                }
            });
        }
    }

    void apply(int pct) {
        if (!probed) probe();
        if (!hasNbfc) return;
        std::lock_guard<std::mutex> lock(mu);
        targetPct = pct;
        dirty = true;
        cv.notify_one();
    }

    void shutdown() {
        if (!running) return;
        {
            std::lock_guard<std::mutex> lock(mu);
            stop = true;
            dirty = true;
            cv.notify_one();
        }
        if (worker.joinable()) worker.join();
        running = false;
    }

    ~FanWorker() {
        shutdown();
    }
};

static FanWorker g_fanWorker;

struct GpuMetric {
    char tag[16] = "";     // "dGPU" or "iGPU"
    char name[64] = "";    // "GeForce GTX 1650" or "Radeon Vega 10"
    int loadPct = -1;
    int tempC = -1;
    int vramUsedMb = -1;
    bool isNvidia = false;
    char busyPath[320] = "";
    char tempPath[660] = "";
    char vramPath[320] = "";
};

struct HudMetrics {
    double fps = 0;
    int cpu = -1;
    int cpuTempC = -1;
    char cpuTempPath[384]{};
    GpuMetric gpus[2];
    int gpuN = 0;
};

#if !defined(_WIN32)
struct NvmlContext {
    void* lib = nullptr;
    using Init_fn = int (*)();
    using Shutdown_fn = int (*)();
    using GetHandle_fn = int (*)(unsigned int, void**);
    using GetName_fn = int (*)(void*, char*, unsigned int);
    using GetTemp_fn = int (*)(void*, int, unsigned int*);
    struct Rates { unsigned int gpu; unsigned int memory; };
    using GetRates_fn = int (*)(void*, Rates*);
    struct Mem { unsigned long long total; unsigned long long free; unsigned long long used; };
    using GetMem_fn = int (*)(void*, Mem*);

    Init_fn init = nullptr;
    Shutdown_fn shutdown = nullptr;
    GetHandle_fn getHandle = nullptr;
    GetName_fn getName = nullptr;
    GetTemp_fn getTemp = nullptr;
    GetRates_fn getRates = nullptr;
    GetMem_fn getMem = nullptr;

    void* dev = nullptr;
    char name[64] = "";
    bool ready = false;
    bool attempted = false;

    void probe() {
        if (attempted) return;
        attempted = true;
        lib = dlopen("libnvidia-ml.so.1", RTLD_NOW);
        if (!lib) lib = dlopen("libnvidia-ml.so", RTLD_NOW);
        if (!lib) return;
        init = (Init_fn)dlsym(lib, "nvmlInit_v2");
        if (!init) init = (Init_fn)dlsym(lib, "nvmlInit");
        shutdown = (Shutdown_fn)dlsym(lib, "nvmlShutdown");
        getHandle = (GetHandle_fn)dlsym(lib, "nvmlDeviceGetHandleByIndex_v2");
        if (!getHandle) getHandle = (GetHandle_fn)dlsym(lib, "nvmlDeviceGetHandleByIndex");
        getName = (GetName_fn)dlsym(lib, "nvmlDeviceGetName");
        getTemp = (GetTemp_fn)dlsym(lib, "nvmlDeviceGetTemperature");
        getRates = (GetRates_fn)dlsym(lib, "nvmlDeviceGetUtilizationRates");
        getMem = (GetMem_fn)dlsym(lib, "nvmlDeviceGetMemoryInfo");

        if (init && init() == 0 && getHandle && getHandle(0, &dev) == 0 && dev) {
            ready = true;
            char fullName[64] = {0};
            if (getName && getName(dev, fullName, sizeof(fullName)) == 0) {
                const char* src = fullName;
                if (std::strncmp(src, "NVIDIA GeForce ", 15) == 0) src += 15;
                else if (std::strncmp(src, "NVIDIA ", 7) == 0) src += 7;
                std::snprintf(name, sizeof(name), "%s", src);
            } else {
                std::snprintf(name, sizeof(name), "GTX/RTX GPU");
            }
        }
    }

    bool sample(int& outLoad, int& outTemp, int& outUsedMb) {
        if (!ready || !dev) return false;
        if (getRates) {
            Rates r{};
            if (getRates(dev, &r) == 0) outLoad = static_cast<int>(r.gpu);
        }
        if (getTemp) {
            unsigned int t = 0;
            if (getTemp(dev, 0, &t) == 0) outTemp = static_cast<int>(t);
        }
        if (getMem) {
            Mem m{};
            if (getMem(dev, &m) == 0) {
                outUsedMb = static_cast<int>(m.used / (1024ULL * 1024ULL));
            }
        }
        return true;
    }
};

static NvmlContext g_nvml;
#endif

HudMetrics g_metrics;
uint64_t g_presentMark = 0;
std::chrono::steady_clock::time_point g_metricAt{};
bool g_metricInited = false;

int readSysInt(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (!f) return -1;
    int v = -1;
    if (std::fscanf(f, "%d", &v) != 1) v = -1;
    std::fclose(f);
    return v;
}

unsigned long long readSysUll(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (!f) return 0;
    unsigned long long v = 0;
    if (std::fscanf(f, "%llu", &v) != 1) v = 0;
    std::fclose(f);
    return v;
}

void discoverGpus() {
#if !defined(_WIN32)
    if (g_metrics.gpuN > 0) return;

    // 1. Probe NVIDIA dGPU via NVML
    g_nvml.probe();
    if (g_nvml.ready && g_metrics.gpuN < 2) {
        GpuMetric& g = g_metrics.gpus[g_metrics.gpuN++];
        std::snprintf(g.tag, sizeof(g.tag), "dGPU");
        std::snprintf(g.name, sizeof(g.name), "%s", g_nvml.name);
        g.isNvidia = true;
    }

    // 2. Discover DRM cards from sysfs
    DIR* dir = opendir("/sys/class/drm");
    if (dir) {
        while (g_metrics.gpuN < 2) {
            dirent* ent = readdir(dir);
            if (!ent) break;
            const char* name = ent->d_name;
            if (std::strncmp(name, "card", 4) != 0) continue;
            if (std::strchr(name, '-')) continue;

            char vendorPath[320];
            std::snprintf(vendorPath, sizeof(vendorPath), "/sys/class/drm/%s/device/vendor", name);
            FILE* vf = std::fopen(vendorPath, "r");
            unsigned vendor = 0;
            if (!vf) continue;
            if (std::fscanf(vf, "%x", &vendor) != 1) vendor = 0;
            std::fclose(vf);

            // Skip NVIDIA DRM if already handled via NVML
            if (vendor == 0x10deu && g_nvml.ready) continue;

            GpuMetric& g = g_metrics.gpus[g_metrics.gpuN];
            g.isNvidia = (vendor == 0x10deu);

            if (vendor == 0x1002u) {
                char vramTotPath[340];
                std::snprintf(vramTotPath, sizeof(vramTotPath),
                              "/sys/class/drm/%s/device/mem_info_vram_total", name);
                const unsigned long long vramTot = readSysUll(vramTotPath);
                const bool isApu = (vramTot > 0 && vramTot <= (2ULL << 30));
                if (g_metrics.gpuN > 0 || isApu) {
                    std::snprintf(g.tag, sizeof(g.tag), "iGPU");
                    std::snprintf(g.name, sizeof(g.name), "AMD Vega/Radeon");
                } else {
                    std::snprintf(g.tag, sizeof(g.tag), "dGPU");
                    std::snprintf(g.name, sizeof(g.name), "AMD Radeon");
                }
            } else if (vendor == 0x8086u) {
                std::snprintf(g.tag, sizeof(g.tag), "iGPU");
                std::snprintf(g.name, sizeof(g.name), "Intel HD/Iris");
            } else if (vendor == 0x10deu) {
                std::snprintf(g.tag, sizeof(g.tag), "dGPU");
                std::snprintf(g.name, sizeof(g.name),
                              g_nvml.ready ? "GeForce GPU" : "GeForce GPU (driver offline)");
            } else {
                std::snprintf(g.tag, sizeof(g.tag), "GPU");
                std::snprintf(g.name, sizeof(g.name), "Generic GPU");
            }

            std::snprintf(g.busyPath, sizeof(g.busyPath),
                          "/sys/class/drm/%s/device/gpu_busy_percent", name);
            std::snprintf(g.vramPath, sizeof(g.vramPath),
                          "/sys/class/drm/%s/device/mem_info_vram_used", name);

            char hwmon[384];
            std::snprintf(hwmon, sizeof(hwmon), "/sys/class/drm/%s/device/hwmon", name);
            DIR* hd = opendir(hwmon);
            if (hd) {
                while (dirent* he = readdir(hd)) {
                    if (he->d_name[0] == '.') continue;
                    char tp[660];
                    std::snprintf(tp, sizeof(tp), "%s/%s/temp1_input", hwmon, he->d_name);
                    const int milli = readSysInt(tp);
                    if (milli > 0) {
                        std::snprintf(g.tempPath, sizeof(g.tempPath), "%s", tp);
                        break;
                    }
                }
                closedir(hd);
            }
            ++g_metrics.gpuN;
        }
        closedir(dir);
    }
#endif
}

int sampleCpuPercent() {
#if !defined(_WIN32)
    static unsigned long long prevIdle = 0, prevTotal = 0;
    static bool have = false;
    FILE* f = std::fopen("/proc/stat", "r");
    if (!f) return -1;
    char label[16];
    unsigned long long user = 0, nice = 0, sys = 0, idle = 0, iow = 0, irq = 0, sirq = 0;
    const int n = std::fscanf(f, "%15s %llu %llu %llu %llu %llu %llu %llu", label, &user, &nice,
                               &sys, &idle, &iow, &irq, &sirq);
    std::fclose(f);
    if (n < 5) return -1;
    const unsigned long long idleAll = idle + iow;
    const unsigned long long total = user + nice + sys + idle + iow + irq + sirq;
    int pct = -1;
    if (have && total > prevTotal) {
        const unsigned long long dTot = total - prevTotal;
        const unsigned long long dIdle = idleAll - prevIdle;
        if (dTot > 0) pct = static_cast<int>((dTot - dIdle) * 100ull / dTot);
    }
    prevIdle = idleAll;
    prevTotal = total;
    have = true;
    return pct;
#else
    return -1;
#endif
}

void discoverCpuTemp() {
#if !defined(_WIN32)
    if (g_metrics.cpuTempPath[0]) return;
    DIR* dir = opendir("/sys/class/hwmon");
    if (!dir) return;

    char fallbackPath[384]{};
    while (dirent* ent = readdir(dir)) {
        if (std::strncmp(ent->d_name, "hwmon", 5) != 0) continue;
        char namePath[384];
        std::snprintf(namePath, sizeof(namePath), "/sys/class/hwmon/%s/name", ent->d_name);
        FILE* nf = std::fopen(namePath, "r");
        if (!nf) continue;
        char drvName[64]{};
        if (std::fscanf(nf, "%63s", drvName) != 1) drvName[0] = '\0';
        std::fclose(nf);

        char temp1[384];
        std::snprintf(temp1, sizeof(temp1), "/sys/class/hwmon/%s/temp1_input", ent->d_name);
        if (::access(temp1, R_OK) == 0) {
            if (fallbackPath[0] == '\0') {
                std::snprintf(fallbackPath, sizeof(fallbackPath), "%s", temp1);
            }
            if (std::strstr(drvName, "k10temp") || std::strstr(drvName, "coretemp") ||
                std::strstr(drvName, "zenpower") || std::strstr(drvName, "cpu_thermal") ||
                std::strstr(drvName, "soc_thermal")) {
                std::snprintf(g_metrics.cpuTempPath, sizeof(g_metrics.cpuTempPath), "%s", temp1);
                break;
            }
        }
    }
    closedir(dir);

    if (g_metrics.cpuTempPath[0] == '\0' && fallbackPath[0] != '\0') {
        std::snprintf(g_metrics.cpuTempPath, sizeof(g_metrics.cpuTempPath), "%s", fallbackPath);
    }
#endif
}

void sampleMetricsLocked() {
    const auto now = std::chrono::steady_clock::now();
    if (!g_metricInited) {
        g_metricInited = true;
        g_metricAt = now;
        g_presentMark = g_presentFrames.load(std::memory_order_relaxed);
        discoverGpus();
        discoverCpuTemp();
        return;
    }
    const double dt = std::chrono::duration<double>(now - g_metricAt).count();
    if (dt < 0.5) return;
    const uint64_t frames = g_presentFrames.load(std::memory_order_relaxed);
    g_metrics.fps = static_cast<double>(frames - g_presentMark) / dt;
    g_presentMark = frames;
    g_metricAt = now;
    g_metrics.cpu = sampleCpuPercent();

    discoverCpuTemp();
    if (g_metrics.cpuTempPath[0]) {
        const int milli = readSysInt(g_metrics.cpuTempPath);
        if (milli > 0) g_metrics.cpuTempC = milli / 1000;
    }

#if !defined(_WIN32)
    for (int i = 0; i < g_metrics.gpuN; ++i) {
        auto& g = g_metrics.gpus[i];
        if (g.isNvidia) {
            g_nvml.sample(g.loadPct, g.tempC, g.vramUsedMb);
        } else {
            if (g.busyPath[0]) {
                const int pct = readSysInt(g.busyPath);
                if (pct >= 0) g.loadPct = pct;
            }
            if (g.tempPath[0]) {
                const int milli = readSysInt(g.tempPath);
                if (milli > 0) g.tempC = milli / 1000;
            }
            if (g.vramPath[0]) {
                const unsigned long long v = readSysUll(g.vramPath);
                if (v > 0) g.vramUsedMb = static_cast<int>(v / (1024ULL * 1024ULL));
            }
        }
    }
#endif

    // Auto-Optimized fan control: react to peak temperature across CPU and all GPUs
    if (g_rt.fanMode == FanMode::AutoOpt && g_fanWorker.hasNbfc) {
        int maxTemp = g_metrics.cpuTempC;
        for (int i = 0; i < g_metrics.gpuN; ++i) {
            if (g_metrics.gpus[i].tempC > maxTemp) maxTemp = g_metrics.gpus[i].tempC;
        }
        int target = -1;
        if (maxTemp >= 78) target = 100;
        else if (maxTemp >= 70) target = 85;
        else if (maxTemp >= 62) target = 70;
        else if (maxTemp > 0) target = 50;

        const auto elapsedSec =
            std::chrono::duration_cast<std::chrono::seconds>(now - g_rt.lastAutoOptChange).count();
        if (target > 0 && (target != g_rt.fanPct) && (elapsedSec >= 5 || maxTemp >= 78)) {
            g_rt.lastAutoOptChange = now;
            g_rt.fanPct = target;
            g_fanWorker.apply(target);
        }
    }
}

void cleanGpuName(char* out, size_t outSize, const char* in) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    if (!in || !in[0]) return;

    const char* p = in;
    const char* prefixes[] = {
        "NVIDIA GeForce ", "NVIDIA ", "GeForce ",
        "AMD Radeon(TM) ", "AMD Radeon ", "AMD ",
        "Intel(R) ", "Intel "
    };
    for (const char* pre : prefixes) {
        const size_t len = std::strlen(pre);
        if (std::strncmp(p, pre, len) == 0) {
            p += len;
            break;
        }
    }

    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", p);

    // Strip bracket/paren driver details like (RADV RAVEN) or (ACO)
    char* paren = std::strchr(buf, '(');
    if (paren) {
        if (paren > buf && *(paren - 1) == ' ') *(paren - 1) = '\0';
        else *paren = '\0';
    }

    const char* suffixes[] = {
        "with Max-Q Design", "Laptop GPU", "Graphics", "Mobile"
    };
    for (const char* suf : suffixes) {
        char* loc = std::strstr(buf, suf);
        if (loc) {
            const size_t slen = std::strlen(suf);
            std::memmove(loc, loc + slen, std::strlen(loc + slen) + 1);
        }
    }
    trimInPlace(buf);
    const char* chosen = buf[0] ? buf : p;
    std::snprintf(out, outSize, "%.*s", static_cast<int>(outSize - 1), chosen);
}

void formatStatusLocked(char* out, size_t n) {
    sampleMetricsLocked();
    char cpu[16] = "--";
    if (g_metrics.cpu >= 0) std::snprintf(cpu, sizeof(cpu), "%d%%", g_metrics.cpu);

    auto tempColor = [](int tempC) -> const char* {
        if (tempC <= 0) return "\006"; // muted
        if (tempC < 65) return "\003"; // green
        if (tempC < 76) return "\004"; // amber
        return "\005";                 // red
    };

    // --- Line 1: Mode, Upscaler, and Resolutions ---
    const bool isPassthrough =
        g_rt.lastRenderW && g_rt.lastDisplayW &&
        g_rt.lastRenderW == g_rt.lastDisplayW &&
        g_rt.lastRenderH == g_rt.lastDisplayH;
    const char* modeLabel = "SAME-GPU";
    if (std::strcmp(g_rt.presentMode, "gamescope") == 0) {
        modeLabel = "GAMESCOPE";
    } else if (!g_rt.hasIgpu && !g_rt.dgpuIsDiscrete) {
        modeLabel = isPassthrough ? "PASSTHROUGH" : "SAME-GPU";
    } else if (g_rt.offload && (g_rt.hasIgpu || !g_rt.hasDgpu)) {
        if (g_rt.oneWay) {
            modeLabel = (g_rt.upscaler == Upscaler::Edge) ? "HYBRID" : "OFFLOAD 1-WAY";
        } else {
            modeLabel = "OFFLOAD 2-WAY";
        }
    } else if (g_rt.lastRenderW == 3840 && g_rt.lastDisplayW == 1920) {
        modeLabel = "DSR 4K";
    } else if (isPassthrough) {
        modeLabel = "PASSTHROUGH";
    }

    char upscalerBuf[64] = "";
    if (g_rt.upscaler == Upscaler::Edge) {
        const char* em = (g_rt.edgeMode == 3) ? "comp" :
                         ((g_rt.edgeMode == 2) ? "nn" : (g_rt.edgePerf ? "perf" : "qual"));
        std::snprintf(upscalerBuf, sizeof(upscalerBuf), "edge:%s (CAS:%.2f)", em, g_rt.edgeSharpness);
    } else if (g_rt.upscaler == Upscaler::Nis) {
        std::snprintf(upscalerBuf, sizeof(upscalerBuf), "NIS");
    } else if (g_rt.upscaler == Upscaler::Fsr1) {
        std::snprintf(upscalerBuf, sizeof(upscalerBuf), "FSR1 (RCAS:%s)", g_rt.rcas ? "on" : "off");
    } else if (g_rt.upscaler == Upscaler::Nearest) {
        std::snprintf(upscalerBuf, sizeof(upscalerBuf), "nearest");
    } else {
        std::snprintf(upscalerBuf, sizeof(upscalerBuf), "blit");
    }

    char resBuf[64] = "";
    if (g_rt.lastRenderW && g_rt.lastDisplayW) {
        const float effScale =
            static_cast<float>(g_rt.lastDisplayW) / static_cast<float>(g_rt.lastRenderW);
        if (isPassthrough && (g_rt.scale > 1.01f || g_rt.scale < 0.99f)) {
            std::snprintf(resBuf, sizeof(resBuf), "%ux%u -> %ux%u (1.00x, tgt %.2fx)",
                          g_rt.lastRenderW, g_rt.lastRenderH,
                          g_rt.lastDisplayW, g_rt.lastDisplayH, g_rt.scale);
        } else {
            std::snprintf(resBuf, sizeof(resBuf), "%ux%u -> %ux%u (%.2fx)",
                          g_rt.lastRenderW, g_rt.lastRenderH,
                          g_rt.lastDisplayW, g_rt.lastDisplayH, effScale);
        }
    } else {
        std::snprintf(resBuf, sizeof(resBuf), "%.2fx %s", g_rt.scale, presetLabel(g_rt.scale, g_rt.presetIdx));
    }

    // --- Line 2: FPS, CPU, Temp & Fan ---
    char fanBuf[48] = "";
    if (g_fanWorker.hasNbfc) {
        if (g_rt.fanMode == FanMode::Auto) {
            std::snprintf(fanBuf, sizeof(fanBuf), " \006FAN \002AUTO");
        } else if (g_rt.fanMode == FanMode::AutoOpt) {
            std::snprintf(fanBuf, sizeof(fanBuf), " \006FAN \002OPT \006(%d%%)",
                          g_rt.fanPct > 0 ? g_rt.fanPct : 50);
        } else {
            std::snprintf(fanBuf, sizeof(fanBuf), " \006FAN \002%s \006(%d%%)",
                          fanModeName(g_rt.fanMode), g_rt.fanPct);
        }
    }

    char cpuBuf[48] = "";
    if (g_metrics.cpuTempC > 0) {
        std::snprintf(cpuBuf, sizeof(cpuBuf), " \006CPU \001%s %s%dC", cpu,
                      tempColor(g_metrics.cpuTempC), g_metrics.cpuTempC);
    } else {
        std::snprintf(cpuBuf, sizeof(cpuBuf), " \006CPU \001%s", cpu);
    }

    char line2[160] = "";
    if (g_rt.adaptive) {
        std::snprintf(line2, sizeof(line2), "\006FPS \003%.0f \006(tgt:%.0f)%s%s",
                      g_metrics.fps, g_rt.targetFps, cpuBuf, fanBuf);
    } else {
        std::snprintf(line2, sizeof(line2), "\006FPS \003%.0f%s%s",
                      g_metrics.fps, cpuBuf, fanBuf);
    }

    // --- Line 3: Both GPUs (dGPU and iGPU with names, load, temp, VRAM) ---
    char gpuBuf[256] = "";
    for (int i = 0; i < g_metrics.gpuN; ++i) {
        auto& g = g_metrics.gpus[i];
        const char* tag = g.tag;
        char cleanName[64] = "";
        if (std::strcmp(g.tag, "dGPU") == 0) {
            const char* s = (g_rt.dgpuIsDiscrete && g_rt.dgpuName[0]) ? g_rt.dgpuName : g.name;
            cleanGpuName(cleanName, sizeof(cleanName), s);
        } else if (std::strcmp(g.tag, "iGPU") == 0) {
            const char* s = g_rt.igpuName[0]
                                ? g_rt.igpuName
                                : ((!g_rt.dgpuIsDiscrete && g_rt.dgpuName[0]) ? g_rt.dgpuName : g.name);
            cleanGpuName(cleanName, sizeof(cleanName), s);
        } else {
            cleanGpuName(cleanName, sizeof(cleanName), g.name);
        }
        const char* name = cleanName;

        char piece[256] = "";
        char metricsPart[128] = "";
        const char* tc = tempColor(g.tempC);
        if (g.loadPct >= 0 && g.tempC >= 0 && g.vramUsedMb >= 0) {
            std::snprintf(metricsPart, sizeof(metricsPart), ": \001%d%% %s%dC \006%dMB",
                          g.loadPct, tc, g.tempC, g.vramUsedMb);
        } else if (g.loadPct >= 0 && g.tempC >= 0) {
            std::snprintf(metricsPart, sizeof(metricsPart), ": \001%d%% %s%dC",
                          g.loadPct, tc, g.tempC);
        } else if (g.loadPct >= 0 && g.vramUsedMb >= 0) {
            std::snprintf(metricsPart, sizeof(metricsPart), ": \001%d%% \006%dMB",
                          g.loadPct, g.vramUsedMb);
        } else if (g.loadPct >= 0) {
            std::snprintf(metricsPart, sizeof(metricsPart), ": \001%d%%", g.loadPct);
        } else if (g.tempC >= 0) {
            std::snprintf(metricsPart, sizeof(metricsPart), ": %s%dC", tc, g.tempC);
        }

        if (i > 0) std::strncat(gpuBuf, " \006| ", sizeof(gpuBuf) - std::strlen(gpuBuf) - 1);
        std::snprintf(piece, sizeof(piece), "%s%s \001[%.24s]%.60s",
                      (std::strcmp(tag, "dGPU") == 0 ? "\002" : "\007"), tag, name, metricsPart);
        std::strncat(gpuBuf, piece, sizeof(gpuBuf) - std::strlen(gpuBuf) - 1);
    }

    if (gpuBuf[0]) {
        std::snprintf(out, n, "\002EdgeScale \007[%s] \002%s \006| \001%s\n%s\n%s",
                      modeLabel, upscalerBuf, resBuf, line2, gpuBuf);
    } else {
        std::snprintf(out, n, "\002EdgeScale \007[%s] \002%s \006| \001%s\n%s",
                      modeLabel, upscalerBuf, resBuf, line2);
    }
}

void armToastLocked(const char* msg) {
    if (msg && msg[0]) {
        std::snprintf(g_rt.toast, sizeof(g_rt.toast), "%s", msg);
    } else {
        formatStatusLocked(g_rt.toast, sizeof(g_rt.toast));
    }
    g_rt.toastUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::fprintf(stderr, "[xfsr] %s\n", g_rt.toast);
}

void setFanModeLocked(FanMode mode, int customPct = -1) {
    g_rt.fanMode = mode;
    int target = -1;
    char toast[96]{};
    switch (mode) {
    case FanMode::Auto:
        target = -1;
        g_rt.fanPct = -1;
        std::snprintf(toast, sizeof(toast), "fan: AUTO (EC default)");
        break;
    case FanMode::AutoOpt:
        target = -1;
        g_rt.fanPct = -1;
        std::snprintf(toast, sizeof(toast), "fan: AUTO-OPT (temp-reactive)");
        break;
    case FanMode::Balanced:
        target = (customPct > 0) ? customPct : 75;
        g_rt.fanPct = target;
        std::snprintf(toast, sizeof(toast), "fan: BALANCED (%d%%)", target);
        break;
    case FanMode::Max:
        target = 100;
        g_rt.fanPct = 100;
        std::snprintf(toast, sizeof(toast), "fan: MAX (100%% - no throttle)");
        break;
    case FanMode::Quiet:
        target = (customPct > 0) ? customPct : 50;
        g_rt.fanPct = target;
        std::snprintf(toast, sizeof(toast), "fan: QUIET (%d%%)", target);
        break;
    case FanMode::Custom:
        target = std::clamp(customPct, 10, 100);
        g_rt.fanPct = target;
        std::snprintf(toast, sizeof(toast), "fan: %d%%", target);
        break;
    }
    if (mode != FanMode::AutoOpt) {
        g_fanWorker.apply(target);
    }
    armToastLocked(toast);
}

void cycleFanModeLocked() {
    if (!g_fanWorker.hasNbfc) {
        armToastLocked("fan control: nbfc not found");
        return;
    }
    switch (g_rt.fanMode) {
    case FanMode::Auto:
        setFanModeLocked(FanMode::AutoOpt, -1);
        break;
    case FanMode::AutoOpt:
        setFanModeLocked(FanMode::Balanced, 75);
        break;
    case FanMode::Balanced:
        setFanModeLocked(FanMode::Max, 100);
        break;
    case FanMode::Max:
        setFanModeLocked(FanMode::Quiet, 50);
        break;
    case FanMode::Quiet:
    case FanMode::Custom:
    default:
        setFanModeLocked(FanMode::Auto, -1);
        break;
    }
}

void setOffloadLocked(bool on) {
    const char* envOff = xfsrEnv("XFSR_OFFLOAD", "CROSSVULKAN_OFFLOAD");
    if (on && envOff && envOff[0] == '0') {
        armToastLocked("offload locked off by profile");
        return;
    }
    g_rt.offload = on;
    if (!on) g_rt.oneWay = false;
    setenv("XFSR_OFFLOAD", on ? "1" : "0", 1);
    // Live: same-GPU ↔ round-trip (layer keeps dGPU WSI). One-way still restart-only.
    g_rt.reconfigureOffload = true;
    armToastLocked(on ? "offload on (round-trip)" : "offload off (same-GPU)");
}

void setOneWayLocked(bool on) {
    const char* envOff = xfsrEnv("XFSR_OFFLOAD", "CROSSVULKAN_OFFLOAD");
    if (on && envOff && envOff[0] == '0') {
        armToastLocked("offload locked off by profile");
        return;
    }
    if (on) g_rt.offload = true; // --one-way implies --offload
    g_rt.oneWay = on;
    setenv("XFSR_OFFLOAD", g_rt.offload ? "1" : "0", 1);
    if (on) {
        setenv("XFSR_IGPU_PRESENT", g_rt.oneWayForce ? "force" : "1", 1);
    } else {
        setenv("XFSR_IGPU_PRESENT", "0", 1);
    }
    // Moving WSI to/from iGPU mid-run SIGSEGVs with MangoHud — preference only.
    g_rt.reconfigureOffload = false;
    if (on)
        armToastLocked(g_rt.oneWayForce ? "one-way on (force) - restart to apply"
                                        : "one-way on - restart to apply");
    else
        armToastLocked("one-way off - restart to apply");
}

void syncGlShimScaleEnvLocked() {
    // Same process as LD_PRELOAD libxfsr_gl_shim — keep XFSR_SCALE in sync so
    // viewport remap tracks F1 (drawable-first path is primary; env is fallback).
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", g_rt.scale);
    setenv("XFSR_SCALE", buf, 1);
    setenv("EDGESCALE_SCALE", buf, 1);
    setenv("CROSSVULKAN_SCALE", buf, 1);
    // Drop absolute render hints — they were for launch display size and go stale.
    unsetenv("XFSR_RENDER_W");
    unsetenv("XFSR_RENDER_H");
    unsetenv("EDGESCALE_RENDER_W");
    unsetenv("EDGESCALE_RENDER_H");
    unsetenv("CROSSVULKAN_RENDER_W");
    unsetenv("CROSSVULKAN_RENDER_H");
}

// Continuous float scale. Recreate only when render pixels move ≥ hysteresis.
#if !defined(_WIN32)
// True when the main EXE DT_NEEDED-links libX11 (Godot 3). Godot 4 sowrap does not.
bool exeLinksLibX11() {
    static int cached = -1;
    if (cached >= 0) return cached == 1;
    cached = 0;
    const int fd = ::open("/proc/self/exe", O_RDONLY);
    if (fd < 0) return false;
    Elf64_Ehdr eh{};
    if (::read(fd, &eh, sizeof(eh)) != static_cast<ssize_t>(sizeof(eh)) ||
        std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64) {
        ::close(fd);
        return false;
    }
    if (::lseek(fd, static_cast<off_t>(eh.e_phoff), SEEK_SET) < 0) {
        ::close(fd);
        return false;
    }
    for (uint16_t i = 0; i < eh.e_phnum; ++i) {
        Elf64_Phdr ph{};
        if (::read(fd, &ph, sizeof(ph)) != static_cast<ssize_t>(sizeof(ph))) break;
        if (ph.p_type != PT_DYNAMIC) continue;
        const size_t n = static_cast<size_t>(ph.p_filesz / sizeof(Elf64_Dyn));
        std::vector<Elf64_Dyn> dyn(n);
        if (::lseek(fd, static_cast<off_t>(ph.p_offset), SEEK_SET) < 0 ||
            ::read(fd, dyn.data(), ph.p_filesz) != static_cast<ssize_t>(ph.p_filesz))
            break;
        Elf64_Addr strtab = 0;
        size_t strsz = 0;
        for (const auto& d : dyn) {
            if (d.d_tag == DT_STRTAB) strtab = d.d_un.d_ptr;
            if (d.d_tag == DT_STRSZ) strsz = static_cast<size_t>(d.d_un.d_val);
        }
        Elf64_Off stroff = 0;
        int found = 0;
        if (::lseek(fd, static_cast<off_t>(eh.e_phoff), SEEK_SET) >= 0) {
            for (uint16_t k = 0; k < eh.e_phnum; ++k) {
                Elf64_Phdr lp{};
                if (::read(fd, &lp, sizeof(lp)) != static_cast<ssize_t>(sizeof(lp))) break;
                if (lp.p_type != PT_LOAD) continue;
                if (strtab >= lp.p_vaddr && strtab < lp.p_vaddr + lp.p_filesz) {
                    stroff = lp.p_offset + (strtab - lp.p_vaddr);
                    found = 1;
                    break;
                }
            }
        }
        if (found && strsz > 0 && strsz < (1u << 20)) {
            std::vector<char> strs(strsz);
            if (::lseek(fd, static_cast<off_t>(stroff), SEEK_SET) >= 0 &&
                ::read(fd, strs.data(), strsz) == static_cast<ssize_t>(strsz)) {
                for (const auto& d : dyn) {
                    if (d.d_tag != DT_NEEDED) continue;
                    const size_t off = static_cast<size_t>(d.d_un.d_val);
                    if (off < strsz && std::strstr(strs.data() + off, "libX11.so")) {
                        cached = 1;
                        break;
                    }
                }
            }
        }
        break;
    }
    ::close(fd);
    return cached == 1;
}
#endif

// Godot 4 / sowrap + Zink (MMX): block live scale. Upscaler/RCAS still switch live.
bool x11LieBlocksLiveScale() {
    const char* e = std::getenv("XFSR_X11_LIE");
    if (e && e[0] == '0') return false;
    if (e && e[0] == '1') return true;
#if defined(_WIN32)
    return false;
#else
    const char* z = std::getenv("MESA_LOADER_DRIVER_OVERRIDE");
    const int zink = z && (std::strcmp(z, "zink") == 0 || std::strncmp(z, "zink,", 5) == 0);
    if (!zink) return false; // native Vulkan — live F1 is fine
    return !exeLinksLibX11();
#endif
}

void setScaleLocked(float newScale, bool requestRecreate, bool fromAdaptive) {
    constexpr float kMin = 1.05f;
    constexpr float kMax = 4.0f; // enough for 1080p→4K and low-res→4K pixel-art scales
    if (newScale < kMin) newScale = kMin;
    if (newScale > kMax) newScale = kMax;
    if (std::fabs(newScale - g_rt.scale) < 0.008f) return;

    if (requestRecreate && x11LieBlocksLiveScale()) {
        armToastLocked("preset: restart edgerun to change");
        return;
    }

    const uint32_t dw = g_rt.lastDisplayW ? g_rt.lastDisplayW : 1920u;
    const uint32_t dh = g_rt.lastDisplayH ? g_rt.lastDisplayH : 1080u;
    const uint32_t oldRw = g_rt.lastRenderW ? g_rt.lastRenderW : renderDim(dw, g_rt.scale);
    const uint32_t oldRh = g_rt.lastRenderH ? g_rt.lastRenderH : renderDim(dh, g_rt.scale);
    const uint32_t newRw = renderDim(dw, newScale);
    const uint32_t newRh = renderDim(dh, newScale);
    const int dpx = std::abs(static_cast<int>(newRw) - static_cast<int>(oldRw)) +
                    std::abs(static_cast<int>(newRh) - static_cast<int>(oldRh));

    g_rt.scale = newScale;
    g_rt.presetIdx = presetIndexFromScale(newScale);
    syncGlShimScaleEnvLocked();

    // Need a meaningful pixel change before kicking OUT_OF_DATE (avoid thrash).
    constexpr int kHysteresisPx = 12;
    if (requestRecreate && dpx >= kHysteresisPx) {
        g_rt.recreate = true;
        g_rt.lastScaleChange = std::chrono::steady_clock::now();
        char buf[160];
        if (fromAdaptive)
            std::snprintf(buf, sizeof(buf), "adaptive %.2fx (%ux%u)", g_rt.scale, newRw, newRh);
        else
            std::snprintf(buf, sizeof(buf), "scale %.2fx (%ux%u)", g_rt.scale, newRw, newRh);
        armToastLocked(buf);
    }
}

void applyPresetIdxLocked(int idx, bool requestRecreate, bool fromAdaptive = false) {
    if (idx < 0) idx = 0;
    if (idx >= kPresetCount) idx = kPresetCount - 1;
    setScaleLocked(kPresetScales[idx], requestRecreate, fromAdaptive);
    g_rt.presetIdx = idx;
}

// Present-to-present EMA → continuous scale toward XFSR_TARGET_FPS.
void tickAdaptiveLocked() {
    if (!g_rt.adaptive || g_rt.recreate) return;
    const auto now = std::chrono::steady_clock::now();
    if (g_rt.lastPresent.time_since_epoch().count() == 0) {
        g_rt.lastPresent = now;
        return;
    }
    const float dtMs =
        std::chrono::duration<float, std::milli>(now - g_rt.lastPresent).count();
    g_rt.lastPresent = now;
    // Ignore stalls / hitch outliers that would yank the scale.
    if (dtMs < 0.5f || dtMs > 100.f) return;

    constexpr float kAlpha = 0.12f;
    if (!g_rt.haveEma) {
        g_rt.emaFtMs = dtMs;
        g_rt.haveEma = true;
    } else {
        g_rt.emaFtMs = g_rt.emaFtMs * (1.f - kAlpha) + dtMs * kAlpha;
    }

    const float targetMs = 1000.f / (g_rt.targetFps > 1.f ? g_rt.targetFps : 60.f);
    const float upThresh = targetMs * 1.12f;   // miss target → lower res
    const auto sinceChange =
        std::chrono::duration<float>(now - g_rt.lastScaleChange).count();
    const float cooldown = (g_rt.emaFtMs > upThresh) ? 0.35f : 0.90f;
    if (sinceChange < cooldown) return;

    const float err = (g_rt.emaFtMs - targetMs) / targetMs;
    // Deadband — already near target.
    if (std::fabs(err) < 0.06f) return;
    // Asymmetric: drop resolution faster than we restore quality.
    float step = err * 0.18f;
    if (step > 0.12f) step = 0.12f;
    if (step < -0.06f) step = -0.06f;
    setScaleLocked(g_rt.scale + step, true, true);
}

#if defined(_WIN32)
KeyBits readKeysWin32() {
    KeyBits k{};
    k.f1 = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
    k.f2 = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
    k.f3 = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
    k.f4 = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
    k.f5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    k.f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    k.f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    k.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    k.ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    k.alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    k.ins = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    k.del = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
    k.home = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
    k.end = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
    k.pgup = (GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0;
    k.pgdn = (GetAsyncKeyState(VK_NEXT) & 0x8000) != 0;
    k.pause = (GetAsyncKeyState(VK_PAUSE) & 0x8000) != 0;
    return k;
}
#else
bool ensureX11() {
    if (g_x11.ready) return true;
    if (g_x11.gaveUp) return false;
    if (!std::getenv("DISPLAY")) {
        // Wayland-only: retry a few times in case DISPLAY appears later (Xwayland).
        if (++g_x11.failTries > 120) g_x11.gaveUp = true;
        return false;
    }
    if (!g_x11.lib) {
        g_x11.lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        if (!g_x11.lib) g_x11.lib = dlopen("libX11.so", RTLD_NOW | RTLD_LOCAL);
        if (!g_x11.lib) {
            if (++g_x11.failTries > 30) g_x11.gaveUp = true;
            return false;
        }
        g_x11.OpenDisplay = reinterpret_cast<XOpenDisplay_fn>(dlsym(g_x11.lib, "XOpenDisplay"));
        g_x11.CloseDisplay = reinterpret_cast<XCloseDisplay_fn>(dlsym(g_x11.lib, "XCloseDisplay"));
        g_x11.QueryKeymap = reinterpret_cast<XQueryKeymap_fn>(dlsym(g_x11.lib, "XQueryKeymap"));
        g_x11.KeysymToKeycode =
            reinterpret_cast<XKeysymToKeycode_fn>(dlsym(g_x11.lib, "XKeysymToKeycode"));
        if (!g_x11.OpenDisplay || !g_x11.QueryKeymap || !g_x11.KeysymToKeycode) {
            g_x11.gaveUp = true;
            return false;
        }
    }
    g_x11.dpy = g_x11.OpenDisplay(nullptr);
    if (!g_x11.dpy) {
        if (++g_x11.failTries > 60) g_x11.gaveUp = true;
        return false;
    }
    g_x11.kcF1 = g_x11.KeysymToKeycode(g_x11.dpy, kXK_F1);
    g_x11.kcF2 = g_x11.KeysymToKeycode(g_x11.dpy, kXK_F2);
    g_x11.kcF3 = g_x11.KeysymToKeycode(g_x11.dpy, kXK_F3);
    g_x11.kcF4 = g_x11.KeysymToKeycode(g_x11.dpy, kXK_F4);
    g_x11.kcF5 = g_x11.KeysymToKeycode(g_x11.dpy, kXK_F5);
    g_x11.kcF6 = g_x11.KeysymToKeycode(g_x11.dpy, kXK_F6);
    g_x11.kcF10 = g_x11.KeysymToKeycode(g_x11.dpy, kXK_F10);
    g_x11.kcShiftL = g_x11.KeysymToKeycode(g_x11.dpy, kXK_Shift_L);
    g_x11.kcShiftR = g_x11.KeysymToKeycode(g_x11.dpy, kXK_Shift_R);
    g_x11.kcCtrlL = g_x11.KeysymToKeycode(g_x11.dpy, kXK_Control_L);
    g_x11.kcCtrlR = g_x11.KeysymToKeycode(g_x11.dpy, kXK_Control_R);
    g_x11.kcAltL = g_x11.KeysymToKeycode(g_x11.dpy, kXK_Alt_L);
    g_x11.kcAltR = g_x11.KeysymToKeycode(g_x11.dpy, kXK_Alt_R);
    g_x11.kcIns = g_x11.KeysymToKeycode(g_x11.dpy, kXK_Insert);
    g_x11.kcDel = g_x11.KeysymToKeycode(g_x11.dpy, kXK_Delete);
    g_x11.kcHome = g_x11.KeysymToKeycode(g_x11.dpy, kXK_Home);
    g_x11.kcEnd = g_x11.KeysymToKeycode(g_x11.dpy, kXK_End);
    g_x11.kcPgUp = g_x11.KeysymToKeycode(g_x11.dpy, kXK_Page_Up);
    g_x11.kcPgDn = g_x11.KeysymToKeycode(g_x11.dpy, kXK_Page_Down);
    g_x11.kcPause = g_x11.KeysymToKeycode(g_x11.dpy, kXK_Pause);
    g_x11.ready = true;
    return true;
}

bool xKeyDown(const char keys[32], unsigned char kc) {
    if (!kc) return false;
    return (keys[kc / 8] & (1 << (kc % 8))) != 0;
}

bool evKeyDown(const unsigned char* bits, int key) {
    if (key < 0) return false;
    const unsigned idx = static_cast<unsigned>(key);
    return (bits[idx / 8] & (1u << (idx % 8))) != 0;
}

void ensureEvdev() {
    if (g_evdev.probed) return;
    g_evdev.probed = true;
    DIR* dir = opendir("/dev/input");
    if (!dir) return;
    while (dirent* ent = readdir(dir)) {
        if (std::strncmp(ent->d_name, "event", 5) != 0) continue;
        char path[280];
        std::snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long evBits = 0;
        if (ioctl(fd, EVIOCGBIT(0, sizeof(evBits)), &evBits) < 0 || !(evBits & (1u << EV_KEY))) {
            close(fd);
            continue;
        }
        // Prefer devices that look like keyboards (have KEY_A or KEY_F1).
        unsigned char keyBits[(KEY_MAX + 7) / 8]{};
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits) < 0) {
            close(fd);
            continue;
        }
        const bool hasF1 = evKeyDown(keyBits, KEY_F1);
        const bool hasA = evKeyDown(keyBits, KEY_A);
        if (!hasF1 && !hasA) {
            close(fd);
            continue;
        }
        g_evdev.fds.push_back(fd);
        g_evdev.anyOpen = true;
    }
    closedir(dir);
}

KeyBits readKeysX11() {
    KeyBits k{};
    if (!ensureX11()) return k;
    char keys[32]{};
    g_x11.QueryKeymap(g_x11.dpy, keys);
    k.f1 = xKeyDown(keys, g_x11.kcF1);
    k.f2 = xKeyDown(keys, g_x11.kcF2);
    k.f3 = xKeyDown(keys, g_x11.kcF3);
    k.f4 = xKeyDown(keys, g_x11.kcF4);
    k.f5 = xKeyDown(keys, g_x11.kcF5);
    k.f6 = xKeyDown(keys, g_x11.kcF6);
    k.f10 = xKeyDown(keys, g_x11.kcF10);
    k.shift = xKeyDown(keys, g_x11.kcShiftL) || xKeyDown(keys, g_x11.kcShiftR);
    k.ctrl = xKeyDown(keys, g_x11.kcCtrlL) || xKeyDown(keys, g_x11.kcCtrlR);
    k.alt = xKeyDown(keys, g_x11.kcAltL) || xKeyDown(keys, g_x11.kcAltR);
    k.ins = xKeyDown(keys, g_x11.kcIns);
    k.del = xKeyDown(keys, g_x11.kcDel);
    k.home = xKeyDown(keys, g_x11.kcHome);
    k.end = xKeyDown(keys, g_x11.kcEnd);
    k.pgup = xKeyDown(keys, g_x11.kcPgUp);
    k.pgdn = xKeyDown(keys, g_x11.kcPgDn);
    k.pause = xKeyDown(keys, g_x11.kcPause);
    return k;
}

KeyBits readKeysEvdev() {
    KeyBits k{};
    ensureEvdev();
    if (!g_evdev.anyOpen) return k;
    unsigned char bits[(KEY_MAX + 7) / 8]{};
    for (int fd : g_evdev.fds) {
        unsigned char one[(KEY_MAX + 7) / 8]{};
        if (ioctl(fd, EVIOCGKEY(sizeof(one)), one) < 0) continue;
        for (size_t i = 0; i < sizeof(bits); ++i) bits[i] |= one[i];
    }
    k.f1 = evKeyDown(bits, KEY_F1);
    k.f2 = evKeyDown(bits, KEY_F2);
    k.f3 = evKeyDown(bits, KEY_F3);
    k.f4 = evKeyDown(bits, KEY_F4);
    k.f5 = evKeyDown(bits, KEY_F5);
    k.f6 = evKeyDown(bits, KEY_F6);
    k.f10 = evKeyDown(bits, KEY_F10);
    k.shift = evKeyDown(bits, KEY_LEFTSHIFT) || evKeyDown(bits, KEY_RIGHTSHIFT);
    k.ctrl = evKeyDown(bits, KEY_LEFTCTRL) || evKeyDown(bits, KEY_RIGHTCTRL);
    k.alt = evKeyDown(bits, KEY_LEFTALT) || evKeyDown(bits, KEY_RIGHTALT);
    k.ins = evKeyDown(bits, KEY_INSERT);
    k.del = evKeyDown(bits, KEY_DELETE);
    k.home = evKeyDown(bits, KEY_HOME);
    k.end = evKeyDown(bits, KEY_END);
    k.pgup = evKeyDown(bits, KEY_PAGEUP);
    k.pgdn = evKeyDown(bits, KEY_PAGEDOWN);
    k.pause = evKeyDown(bits, KEY_PAUSE);
    return k;
}

KeyBits mergeKeys(const KeyBits& a, const KeyBits& b) {
    KeyBits k;
    k.f1 = a.f1 || b.f1;
    k.f2 = a.f2 || b.f2;
    k.f3 = a.f3 || b.f3;
    k.f4 = a.f4 || b.f4;
    k.f5 = a.f5 || b.f5;
    k.f6 = a.f6 || b.f6;
    k.f10 = a.f10 || b.f10;
    k.shift = a.shift || b.shift;
    k.ctrl = a.ctrl || b.ctrl;
    k.alt = a.alt || b.alt;
    k.ins = a.ins || b.ins;
    k.del = a.del || b.del;
    k.home = a.home || b.home;
    k.end = a.end || b.end;
    k.pgup = a.pgup || b.pgup;
    k.pgdn = a.pgdn || b.pgdn;
    k.pause = a.pause || b.pause;
    return k;
}
#endif

void applyHotkeyActions(const KeyBits& cur, const KeyBits& prev, HotkeyMode mode) {
    if (mode == HotkeyMode::None) return;
    auto rise = [](bool now, bool was) { return now && !was; };

    // Navigation cluster mode: Insert/Home/PgUp/Del/End/PgDn/Pause
    if (mode == HotkeyMode::Nav) {
        if (rise(cur.ins, prev.ins)) {
            applyPresetIdxLocked((g_rt.presetIdx + 1) % kPresetCount, true);
        }
        if (rise(cur.home, prev.home)) {
            if (g_rt.upscaler == Upscaler::Edge) {
                g_rt.edgeMode = (g_rt.edgeMode + 1) % 4;
                g_rt.edgePerf = (g_rt.edgeMode == 1);
                const char* mname = (g_rt.edgeMode == 3) ? "competitive" :
                                    ((g_rt.edgeMode == 2) ? "nearest" : (g_rt.edgeMode == 1 ? "perf" : "quality"));
                setenv("EDGESCALE_EDGE_MODE", mname, 1);
                setenv("XFSR_EDGE_MODE", mname, 1);
                const char* label = (g_rt.edgeMode == 3) ? "edge competitive (lowest latency)" :
                                    ((g_rt.edgeMode == 2) ? "edge nearest (pixel art)" :
                                    (g_rt.edgeMode == 1 ? "edge perf (lite)" : "edge quality"));
                armToastLocked(label);
            } else {
                g_rt.rcas = !g_rt.rcas;
                armToastLocked(g_rt.rcas ? "RCAS on" : "RCAS off (EASU only)");
            }
        }
        if (rise(cur.pgup, prev.pgup)) {
            if (g_rt.upscaler == Upscaler::Fsr1)
                g_rt.upscaler = Upscaler::Blit;
            else if (g_rt.upscaler == Upscaler::Blit)
                g_rt.upscaler = Upscaler::Nearest;
            else if (g_rt.upscaler == Upscaler::Nearest)
                g_rt.upscaler = Upscaler::Nis;
            else if (g_rt.upscaler == Upscaler::Nis)
                g_rt.upscaler = Upscaler::Edge;
            else
                g_rt.upscaler = Upscaler::Fsr1;
            setenv("XFSR_UPSCALER", upscalerName(g_rt.upscaler), 1);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "upscaler %s", upscalerName(g_rt.upscaler));
            armToastLocked(buf);
        }
        if (rise(cur.del, prev.del)) {
            setOffloadLocked(!g_rt.offload);
        }
        if (rise(cur.end, prev.end)) {
            setOneWayLocked(!g_rt.oneWay);
        }
        if (rise(cur.pgdn, prev.pgdn)) {
            cycleFanModeLocked();
        }
        if (rise(cur.pause, prev.pause)) {
            g_rt.overlayPinned = !g_rt.overlayPinned;
            armToastLocked(g_rt.overlayPinned ? "overlay pinned" : "overlay unpinned");
        }
        return;
    }

    bool modOk = false;
    switch (mode) {
    case HotkeyMode::Shift:
        modOk = cur.shift;
        break;
    case HotkeyMode::Alt:
        modOk = cur.alt;
        break;
    case HotkeyMode::Ctrl:
        modOk = cur.ctrl;
        break;
    case HotkeyMode::CtrlAlt:
        modOk = cur.ctrl && cur.alt;
        break;
    case HotkeyMode::Bare:
        modOk = true;
        break;
    default:
        break;
    }
    if (!modOk) return;

    if (rise(cur.f1, prev.f1)) {
        applyPresetIdxLocked((g_rt.presetIdx + 1) % kPresetCount, true);
    }
    if (rise(cur.f2, prev.f2)) {
        if (g_rt.upscaler == Upscaler::Edge) {
            g_rt.edgeMode = (g_rt.edgeMode + 1) % 4;
            g_rt.edgePerf = (g_rt.edgeMode == 1);
            const char* mname = (g_rt.edgeMode == 3) ? "competitive" :
                                ((g_rt.edgeMode == 2) ? "nearest" : (g_rt.edgeMode == 1 ? "perf" : "quality"));
            setenv("EDGESCALE_EDGE_MODE", mname, 1);
            setenv("XFSR_EDGE_MODE", mname, 1);
            const char* label = (g_rt.edgeMode == 3) ? "edge competitive (lowest latency)" :
                                ((g_rt.edgeMode == 2) ? "edge nearest (pixel art)" :
                                (g_rt.edgeMode == 1 ? "edge perf (lite)" : "edge quality"));
            armToastLocked(label);
        } else {
            g_rt.rcas = !g_rt.rcas;
            armToastLocked(g_rt.rcas ? "RCAS on" : "RCAS off (EASU only)");
        }
    }
    if (rise(cur.f3, prev.f3)) {
        // Cycle: fsr1 → blit → nearest → nis → edge → fsr1 (live when passes exist).
        if (g_rt.upscaler == Upscaler::Fsr1)
            g_rt.upscaler = Upscaler::Blit;
        else if (g_rt.upscaler == Upscaler::Blit)
            g_rt.upscaler = Upscaler::Nearest;
        else if (g_rt.upscaler == Upscaler::Nearest)
            g_rt.upscaler = Upscaler::Nis;
        else if (g_rt.upscaler == Upscaler::Nis)
            g_rt.upscaler = Upscaler::Edge;
        else
            g_rt.upscaler = Upscaler::Fsr1;
        setenv("XFSR_UPSCALER", upscalerName(g_rt.upscaler), 1);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "upscaler %s", upscalerName(g_rt.upscaler));
        armToastLocked(buf);
    }
    if (rise(cur.f4, prev.f4)) {
        setOffloadLocked(!g_rt.offload);
    }
    if (rise(cur.f5, prev.f5)) {
        setOneWayLocked(!g_rt.oneWay);
    }
    if (rise(cur.f6, prev.f6)) {
        cycleFanModeLocked();
    }
    if (rise(cur.f10, prev.f10)) {
        g_rt.overlayPinned = !g_rt.overlayPinned;
        armToastLocked(g_rt.overlayPinned ? "overlay pinned" : "overlay unpinned");
    }
}

void pollHotkeysLocked() {
    if (g_rt.hotkeyMode == HotkeyMode::None) return;
#if defined(_WIN32)
    const KeyBits cur = readKeysWin32();
    if (!g_rt.loggedKeys) {
        g_rt.loggedKeys = true;
        std::fprintf(stderr,
                     "[edgescale] hotkeys: win32=yes mode=[%s]\n",
                     hotkeyModeDescription(g_rt.hotkeyMode));
    }
#else
    const KeyBits x11 = readKeysX11();
    const KeyBits ev = readKeysEvdev();
    const KeyBits cur = mergeKeys(x11, ev);
    if (!g_rt.loggedKeys) {
        g_rt.loggedKeys = true;
        std::fprintf(stderr,
                     "[edgescale] hotkeys: x11=%s evdev=%s mode=[%s]%s\n",
                     g_x11.ready ? "yes" : "no", g_evdev.anyOpen ? "yes" : "no",
                     hotkeyModeDescription(g_rt.hotkeyMode),
                     (!g_x11.ready && !g_evdev.anyOpen)
                         ? " - use control file if keys fail"
                         : "");
    }
#endif
    applyHotkeyActions(cur, g_prevKeys, g_rt.hotkeyMode);
    g_prevKeys = cur;
}

void handleControlLineLocked(char* line) {
    trimInPlace(line);
    if (!line[0] || line[0] == '#') return;
    char cmd[64]{};
    char arg[64]{};
    // Simple "cmd arg" split
    const char* sp = line;
    size_t i = 0;
    while (*sp && *sp != ' ' && *sp != '\t' && i + 1 < sizeof(cmd)) cmd[i++] = *sp++;
    cmd[i] = '\0';
    while (*sp == ' ' || *sp == '\t') ++sp;
    i = 0;
    while (*sp && i + 1 < sizeof(arg)) arg[i++] = *sp++;
    arg[i] = '\0';
    trimInPlace(arg);

    if (std::strcmp(cmd, "preset") == 0) {
        applyPresetIdxLocked(presetIndexFromName(arg), true);
    } else if (std::strcmp(cmd, "cycle_preset") == 0 || std::strcmp(cmd, "cycle") == 0) {
        applyPresetIdxLocked((g_rt.presetIdx + 1) % kPresetCount, true);
    } else if (std::strcmp(cmd, "rcas") == 0) {
        if (arg[0] == '0')
            g_rt.rcas = false;
        else if (arg[0] == '1')
            g_rt.rcas = true;
        else
            g_rt.rcas = !g_rt.rcas;
        armToastLocked(g_rt.rcas ? "RCAS on" : "RCAS off");
    } else if (std::strcmp(cmd, "edge_sharp") == 0 || std::strcmp(cmd, "edge_sharpness") == 0) {
        float v = 0.f, frac = 0.f, place = 0.1f;
        const char* p = arg;
        while (*p == ' ' || *p == '\t') ++p;
        while (*p >= '0' && *p <= '9') {
            v = v * 10.f + static_cast<float>(*p - '0');
            ++p;
        }
        if (*p == '.') {
            ++p;
            while (*p >= '0' && *p <= '9') {
                frac += static_cast<float>(*p - '0') * place;
                place *= 0.1f;
                ++p;
            }
        }
        v += frac;
        if (v >= 0.f && v <= 1.f) {
            g_rt.edgeSharpness = v;
            char env[32];
            std::snprintf(env, sizeof(env), "%.3f", v);
            setenv("XFSR_EDGE_SHARPNESS", env, 1);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "edge sharp %.2f", v);
            armToastLocked(buf);
        }
    } else if (std::strcmp(cmd, "edge_mode") == 0) {
        bool set = false;
        if (!arg[0] || std::strcmp(arg, "toggle") == 0) {
            g_rt.edgeMode = (g_rt.edgeMode + 1) % 4;
            g_rt.edgePerf = (g_rt.edgeMode == 1);
            set = true;
        } else if (std::strcmp(arg, "competitive") == 0 || std::strcmp(arg, "comp") == 0 ||
                   std::strcmp(arg, "esports") == 0 || std::strcmp(arg, "3") == 0) {
            g_rt.edgeMode = 3;
            g_rt.edgePerf = false;
            set = true;
        } else if (std::strcmp(arg, "nearest") == 0 || std::strcmp(arg, "nn") == 0 ||
                   std::strcmp(arg, "pixel") == 0 || std::strcmp(arg, "2") == 0) {
            g_rt.edgeMode = 2;
            g_rt.edgePerf = false;
            set = true;
        } else if (arg[0] == '1' || std::strcmp(arg, "perf") == 0 || std::strcmp(arg, "lite") == 0 ||
                   std::strcmp(arg, "edge-lite") == 0) {
            g_rt.edgeMode = 1;
            g_rt.edgePerf = true;
            set = true;
        } else if (arg[0] == '0' || std::strcmp(arg, "quality") == 0 || std::strcmp(arg, "hq") == 0) {
            g_rt.edgeMode = 0;
            g_rt.edgePerf = false;
            set = true;
        }
        if (set) {
            const char* mname = (g_rt.edgeMode == 3) ? "competitive" :
                                ((g_rt.edgeMode == 2) ? "nearest" : (g_rt.edgeMode == 1 ? "perf" : "quality"));
            setenv("EDGESCALE_EDGE_MODE", mname, 1);
            setenv("XFSR_EDGE_MODE", mname, 1);
            const char* label = (g_rt.edgeMode == 3) ? "edge competitive (lowest latency)" :
                                ((g_rt.edgeMode == 2) ? "edge nearest (pixel art)" :
                                (g_rt.edgeMode == 1 ? "edge perf (lite)" : "edge quality"));
            armToastLocked(label);
        }
    } else if (std::strcmp(cmd, "fsr") == 0) {
        if (arg[0] == '0')
            g_rt.upscaler = Upscaler::Blit;
        else if (arg[0] == '1')
            g_rt.upscaler = Upscaler::Fsr1;
        else
            g_rt.upscaler =
                (g_rt.upscaler == Upscaler::Fsr1) ? Upscaler::Blit : Upscaler::Fsr1;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "upscaler %s", upscalerName(g_rt.upscaler));
        armToastLocked(buf);
    } else if (std::strcmp(cmd, "upscaler") == 0) {
        if (arg[0])
            g_rt.upscaler = parseUpscaler(arg);
        else if (g_rt.upscaler == Upscaler::Fsr1)
            g_rt.upscaler = Upscaler::Blit;
        else if (g_rt.upscaler == Upscaler::Blit)
            g_rt.upscaler = Upscaler::Nearest;
        else if (g_rt.upscaler == Upscaler::Nearest)
            g_rt.upscaler = Upscaler::Nis;
        else if (g_rt.upscaler == Upscaler::Nis)
            g_rt.upscaler = Upscaler::Edge;
        else
            g_rt.upscaler = Upscaler::Fsr1;
        setenv("XFSR_UPSCALER", upscalerName(g_rt.upscaler), 1);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "upscaler %s", upscalerName(g_rt.upscaler));
        armToastLocked(buf);
    } else if (std::strcmp(cmd, "scale") == 0) {
        float v = 0.f, frac = 0.f, place = 0.1f;
        const char* p = arg;
        while (*p >= '0' && *p <= '9') {
            v = v * 10.f + static_cast<float>(*p - '0');
            ++p;
        }
        if (*p == '.') {
            ++p;
            while (*p >= '0' && *p <= '9') {
                frac += static_cast<float>(*p - '0') * place;
                place *= 0.1f;
                ++p;
            }
        }
        v += frac;
        if (v >= 1.05f && v <= 4.0f) setScaleLocked(v, true, false);
    } else if (std::strcmp(cmd, "overlay") == 0 || std::strcmp(cmd, "pin") == 0) {
        if (arg[0] == '0')
            g_rt.overlayPinned = false;
        else if (arg[0] == '1')
            g_rt.overlayPinned = true;
        else
            g_rt.overlayPinned = !g_rt.overlayPinned;
        armToastLocked(g_rt.overlayPinned ? "overlay pinned" : "overlay unpinned");
    } else if (std::strcmp(cmd, "offload") == 0) {
        bool next = g_rt.offload;
        if (arg[0] == '0')
            next = false;
        else if (arg[0] == '1')
            next = true;
        else
            next = !g_rt.offload;
        setOffloadLocked(next);
    } else if (std::strcmp(cmd, "one-way") == 0 || std::strcmp(cmd, "oneway") == 0) {
        bool next = g_rt.oneWay;
        if (arg[0] == '0')
            next = false;
        else if (arg[0] == '1')
            next = true;
        else
            next = !g_rt.oneWay;
        setOneWayLocked(next);
    } else if (std::strcmp(cmd, "adaptive") == 0) {
        if (arg[0] == '0')
            g_rt.adaptive = false;
        else if (arg[0] == '1')
            g_rt.adaptive = true;
        else
            g_rt.adaptive = !g_rt.adaptive;
        setenv("XFSR_ADAPTIVE", g_rt.adaptive ? "1" : "0", 1);
        armToastLocked(g_rt.adaptive ? "adaptive on" : "adaptive off");
    } else if (std::strcmp(cmd, "target_fps") == 0 || std::strcmp(cmd, "target") == 0) {
        float v = 0.f;
        const char* p = arg;
        while (*p >= '0' && *p <= '9') {
            v = v * 10.f + static_cast<float>(*p - '0');
            ++p;
        }
        if (v >= 1.f) {
            if (v < 20.f) v = 20.f;
            if (v > 1000.f) v = 1000.f;
            g_rt.targetFps = v;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.0f", v);
            setenv("XFSR_TARGET_FPS", buf, 1);
            char toast[64];
            std::snprintf(toast, sizeof(toast), "target %.0f FPS", v);
            armToastLocked(toast);
        }
    } else if (std::strcmp(cmd, "fan") == 0 || std::strcmp(cmd, "fan_mode") == 0) {
        if (!arg[0] || std::strcmp(arg, "cycle") == 0 || std::strcmp(arg, "toggle") == 0) {
            cycleFanModeLocked();
        } else if (std::strcmp(arg, "auto") == 0 || std::strcmp(arg, "ec") == 0 || std::strcmp(arg, "0") == 0) {
            setFanModeLocked(FanMode::Auto, -1);
        } else if (std::strcmp(arg, "auto-opt") == 0 || std::strcmp(arg, "opt") == 0 ||
                   std::strcmp(arg, "optimized") == 0 || std::strcmp(arg, "reactive") == 0) {
            setFanModeLocked(FanMode::AutoOpt, -1);
        } else if (std::strcmp(arg, "max") == 0 || std::strcmp(arg, "100") == 0 || std::strcmp(arg, "turbo") == 0) {
            setFanModeLocked(FanMode::Max, 100);
        } else if (std::strcmp(arg, "balanced") == 0 || std::strcmp(arg, "game") == 0 || std::strcmp(arg, "75") == 0) {
            setFanModeLocked(FanMode::Balanced, 75);
        } else if (std::strcmp(arg, "quiet") == 0 || std::strcmp(arg, "silent") == 0 || std::strcmp(arg, "50") == 0) {
            setFanModeLocked(FanMode::Quiet, 50);
        } else {
            int pct = std::atoi(arg);
            if (pct >= 10 && pct <= 100) {
                setFanModeLocked(FanMode::Custom, pct);
            }
        }
    } else if (std::strcmp(cmd, "hotkeys") == 0) {
        if (std::strcmp(arg, "none") == 0 || std::strcmp(arg, "off") == 0 || std::strcmp(arg, "0") == 0) {
            g_rt.hotkeyMode = HotkeyMode::None;
        } else if (std::strcmp(arg, "alt") == 0) {
            g_rt.hotkeyMode = HotkeyMode::Alt;
        } else if (std::strcmp(arg, "ctrl") == 0) {
            g_rt.hotkeyMode = HotkeyMode::Ctrl;
        } else if (std::strcmp(arg, "ctrl_alt") == 0 || std::strcmp(arg, "ctrlalt") == 0) {
            g_rt.hotkeyMode = HotkeyMode::CtrlAlt;
        } else if (std::strcmp(arg, "nav") == 0) {
            g_rt.hotkeyMode = HotkeyMode::Nav;
        } else if (std::strcmp(arg, "bare") == 0) {
            g_rt.hotkeyMode = HotkeyMode::Bare;
        } else {
            g_rt.hotkeyMode = HotkeyMode::Shift;
        }
        char b[64];
        std::snprintf(b, sizeof(b), "hotkeys: %s", hotkeyModeDescription(g_rt.hotkeyMode));
        armToastLocked(b);
    } else if (std::strcmp(cmd, "status") == 0) {
        armToastLocked(nullptr);
    }
}

void pollControlFileLocked() {
    if (!g_rt.controlPath[0]) return;
    struct stat st {};
    if (stat(g_rt.controlPath, &st) != 0) return;
    if (st.st_mtime == g_rt.controlMtime && st.st_size > 0) {
        // Still process if size changed without mtime granularity — use size+mtime.
    }
    static off_t lastSize = -1;
    if (st.st_mtime == g_rt.controlMtime && st.st_size == lastSize) return;
    g_rt.controlMtime = st.st_mtime;
    lastSize = st.st_size;
    if (st.st_size == 0) return;

    std::ifstream in(g_rt.controlPath);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s", line.c_str());
        handleControlLineLocked(buf);
    }
    in.close();
    // Truncate so the next write is a fresh command.
    std::ofstream trunc(g_rt.controlPath, std::ios::trunc);
}

void ensureOverlayGpu(OverlayGpu* ov, DeviceContext* ctx, uint32_t w, uint32_t h) {
    if (!ov || !ctx) return;
    if (ov->ctx == ctx && ov->stagingMapped && ov->width == w && ov->height == h && ov->image.image)
        return;
    destroyOverlay(ov);
    ov->ctx = ctx;
    ov->width = w;
    ov->height = h;
    ctx->loadDispatch();
    auto& d = ctx->vk;
    try {
        ov->image = createImage(ctx, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_IMAGE_TILING_OPTIMAL, false);
    } catch (...) {
        ov->ctx = nullptr;
        return;
    }
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4u;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (d.CreateBuffer(ctx->device, &bci, nullptr, &ov->staging) != VK_SUCCESS) {
        destroyOverlay(ov);
        return;
    }
    VkMemoryRequirements req{};
    d.GetBufferMemoryRequirements(ctx->device, ov->staging, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = findMemoryType(ctx->physical, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (d.AllocateMemory(ctx->device, &mai, nullptr, &ov->stagingMem) != VK_SUCCESS) {
        destroyOverlay(ov);
        return;
    }
    d.BindBufferMemory(ctx->device, ov->staging, ov->stagingMem, 0);
    if (d.MapMemory(ctx->device, ov->stagingMem, 0, bytes, 0, &ov->stagingMapped) != VK_SUCCESS) {
        destroyOverlay(ov);
        return;
    }
    ov->lastText[0] = '\0';
    ov->lastWrapW = 0;
}

struct TextLayout {
    uint32_t boxW = 0;
    uint32_t boxH = 0;
};

TextLayout computeLayout(const char* text, uint32_t maxWidth, uint32_t maxHeight) {
    TextLayout l{};
    const int scale = (maxWidth < 900) ? 1 : 2;
    const int charW = 8 * scale + (scale == 1 ? 1 : 2);
    const int lineH = 8 * scale + (scale == 1 ? 4 : 6);
    const int startX = (scale == 1) ? 10 : 14;
    const int startY = (scale == 1) ? 8 : 10;
    int curX = startX;
    uint32_t curLineW = 0;
    uint32_t maxLineW = 0;
    uint32_t physicalLines = 1;

    for (const char* t = text; *t; ++t) {
        const unsigned char uc = static_cast<unsigned char>(*t);
        if (uc >= 1 && uc <= 7) continue; // color escape byte
        if (*t == '\n') {
            if (curLineW > maxLineW) maxLineW = curLineW;
            curLineW = 0;
            curX = startX;
            physicalLines++;
            continue;
        }
        if (curX + 8 * scale >= static_cast<int>(maxWidth)) {
            // Soft wrap
            if (curLineW > maxLineW) maxLineW = curLineW;
            curLineW = 0;
            curX = startX;
            physicalLines++;
        }
        curX += charW;
        curLineW += charW;
    }
    if (curLineW > maxLineW) maxLineW = curLineW;

    l.boxW = std::min(maxWidth, maxLineW + static_cast<uint32_t>(startX * 2 + 4));
    l.boxH = std::min(maxHeight, physicalLines * static_cast<uint32_t>(lineH) + static_cast<uint32_t>(startY * 2 + 8));
    return l;
}

void rasterizeText(uint8_t* pixels, uint32_t width, uint32_t height, const char* text, uint32_t wrapWidth = 0) {
    const size_t n = static_cast<size_t>(width) * height * 4u;
    std::memset(pixels, 0, n);

    const uint32_t maxW = (wrapWidth > 0 && wrapWidth < width) ? wrapWidth : width;
    const TextLayout layout = computeLayout(text, maxW, height);
    const uint32_t boxW = layout.boxW;
    const uint32_t boxH = layout.boxH;
    constexpr int R = 6;

    // Background: Dark obsidian slate glass card with rounded corners and glowing top accent bar
    for (uint32_t y = 0; y < boxH; ++y) {
        for (uint32_t x = 0; x < boxW; ++x) {
            // Check rounded corners
            int cx = (x < R) ? (R - x) : ((x >= boxW - R) ? (x - (boxW - R - 1)) : 0);
            int cy = (y < R) ? (R - y) : ((y >= boxH - R) ? (y - (boxH - R - 1)) : 0);
            if (cx > 0 && cy > 0 && (cx * cx + cy * cy > R * R)) {
                continue; // outside rounded corner: transparent
            }

            uint8_t* p = pixels + (static_cast<size_t>(y) * width + x) * 4;

            // Top glowing header line: horizontal gradient from Cyan (#22d3ee) to Violet (#a855f7)
            if (y <= 1) {
                float t = (boxW > 1) ? static_cast<float>(x) / static_cast<float>(boxW - 1) : 0.f;
                p[0] = static_cast<uint8_t>(34.f * (1.f - t) + 168.f * t + 0.5f);
                p[1] = static_cast<uint8_t>(211.f * (1.f - t) + 85.f * t + 0.5f);
                p[2] = static_cast<uint8_t>(238.f * (1.f - t) + 247.f * t + 0.5f);
                p[3] = 255;
            } else if (x == 0 || x == boxW - 1 || y == boxH - 1) {
                // Subtle 1px crisp card border
                p[0] = 56;
                p[1] = 189;
                p[2] = 248;
                p[3] = 65;
            } else {
                // Obsidian slate glass card
                p[0] = 12;
                p[1] = 16;
                p[2] = 24;
                p[3] = 235;
            }
        }
    }

    struct ColorRgb { uint8_t r, g, b; };
    constexpr ColorRgb kColors[8] = {
        {248, 250, 252}, // 0: fallback white
        {248, 250, 252}, // 1: white
        {56,  189, 248}, // 2: electric cyan
        {74,  222, 128}, // 3: mint green
        {251, 191, 36 }, // 4: warm amber
        {248, 113, 113}, // 5: coral red
        {148, 163, 184}, // 6: slate muted
        {192, 132, 252}, // 7: neon violet
    };
    ColorRgb curColor = kColors[1];

    const int scale = (maxW < 900) ? 1 : 2;
    const int charW = 8 * scale + (scale == 1 ? 1 : 2);
    const int lineH = 8 * scale + (scale == 1 ? 4 : 6);
    const int startX = (scale == 1) ? 10 : 14;
    const int startY = (scale == 1) ? 8 : 10;
    int x = startX;
    int y = startY;

    for (const char* t = text; *t; ++t) {
        char c = *t;
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc >= 1 && uc <= 7) {
            curColor = kColors[uc];
            continue;
        }
        if (c == '\n') {
            x = startX;
            y += lineH;
            continue;
        }
        if (x + 8 * scale >= static_cast<int>(maxW)) {
            x = startX;
            y += lineH;
        }
        if (uc < 32 || uc > 127) c = '?';
        const uint8_t* glyph = kFont8[static_cast<unsigned char>(c) - 32];
        for (int row = 0; row < 8; ++row) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; ++col) {
                if (!(bits & (1u << col))) continue;
                for (int sy = 0; sy < scale; ++sy) {
                    for (int sx = 0; sx < scale; ++sx) {
                        int px = x + col * scale + sx;
                        int py = y + row * scale + sy;

                        // 1. Drop shadow offset by (+1, +1)
                        int spx = px + 1;
                        int spy = py + 1;
                        if (spx >= 0 && spy >= 0 && spx < static_cast<int>(width) &&
                            spy < static_cast<int>(height)) {
                            uint8_t* sp = pixels + (static_cast<size_t>(spy) * width + static_cast<size_t>(spx)) * 4;
                            if (sp[3] < 250) {
                                sp[0] = 0;
                                sp[1] = 0;
                                sp[2] = 0;
                                sp[3] = 220;
                            }
                        }

                        // 2. Foreground glyph pixel
                        if (px >= 0 && py >= 0 && px < static_cast<int>(width) &&
                            py < static_cast<int>(height)) {
                            uint8_t* p = pixels + (static_cast<size_t>(py) * width + static_cast<size_t>(px)) * 4;
                            p[0] = curColor.r;
                            p[1] = curColor.g;
                            p[2] = curColor.b;
                            p[3] = 255;
                        }
                    }
                }
            }
        }
        x += charW;
    }
}

void transition(DeviceContext* ctx, VkCommandBuffer cmd, VkImage image, VkImageLayout oldL,
                VkImageLayout newL, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = oldL;
    b.newLayout = newL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    ctx->vk.CmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

} // namespace

void notePresented() {
    g_presentFrames.fetch_add(1, std::memory_order_relaxed);
    // Adaptive may request recreate — don't block the present path long.
    if (std::unique_lock<std::mutex> lock(g_rt.mu, std::try_to_lock); lock.owns_lock()) {
        if (g_rt.adaptive) tickAdaptiveLocked();
        else g_rt.lastPresent = std::chrono::steady_clock::now();
    }
}

void publishPresentStatus(const char* mode, uint32_t renderW, uint32_t renderH,
                          uint32_t displayW, uint32_t displayH, uint32_t presents) {
    initFromEnv();
    std::lock_guard<std::mutex> lock(g_rt.mu);
    if (displayW) g_rt.lastDisplayW = displayW;
    if (displayH) g_rt.lastDisplayH = displayH;
    if (renderW) g_rt.lastRenderW = renderW;
    if (renderH) g_rt.lastRenderH = renderH;
    if (mode && mode[0]) {
        std::snprintf(g_rt.presentMode, sizeof(g_rt.presentMode), "%s", mode);
    }
    // Only mirror the effective ratio into the preset when caps-shrink is off (in-game
    // resolution defines scale). With caps-shrink on, --preset / F1 is authoritative —
    // an app that ignores the clamp (e.g. windowed 1280x720 on 1080p) must not flip
    // "performance" to "quality". Never clobber a pending F1 change either.
    const char* cs = std::getenv("XFSR_CAPS_SHRINK");
    const bool capsShrinkOff = cs && cs[0] == '0';
    if (capsShrinkOff && !g_rt.recreate && renderW && displayW && renderW < displayW) {
        g_rt.scale = static_cast<float>(displayW) / static_cast<float>(renderW);
        g_rt.presetIdx = presetIndexFromScale(g_rt.scale);
    }
    if (!g_rt.statusPath[0]) return;
    FILE* f = std::fopen(g_rt.statusPath, "w");
    if (!f) return;
    std::fprintf(f,
                 "mode=%s\nrender=%ux%u\ndisplay=%ux%u\npresents=%u\nscale=%.3f\npreset=%s\n"
                 "upscaler=%s\nadaptive=%d\ntarget_fps=%.1f\nema_ft_ms=%.2f\n",
                 mode && mode[0] ? mode : "unknown", renderW, renderH, displayW, displayH, presents,
                 g_rt.scale, presetLabel(g_rt.scale, g_rt.presetIdx), upscalerName(g_rt.upscaler),
                 g_rt.adaptive ? 1 : 0, g_rt.targetFps, g_rt.haveEma ? g_rt.emaFtMs : 0.f);
    std::fclose(f);
}

void setDeviceInfo(const char* dgpuName, const char* igpuName, bool dgpuIsDiscrete, bool hasIgpu) {
    initFromEnv();
    std::lock_guard<std::mutex> lock(g_rt.mu);
    g_rt.dgpuIsDiscrete = dgpuIsDiscrete;
    if (dgpuName && dgpuName[0]) {
        std::snprintf(g_rt.dgpuName, sizeof(g_rt.dgpuName), "%s", dgpuName);
        g_rt.hasDgpu = true;
    }
    if (igpuName && igpuName[0]) {
        std::snprintf(g_rt.igpuName, sizeof(g_rt.igpuName), "%s", igpuName);
        g_rt.hasIgpu = hasIgpu;
    }
}

void initFromEnv() {
    std::lock_guard<std::mutex> lock(g_rt.mu);
    if (g_rt.inited) return;
    g_rt.inited = true;

    const char* hud = std::getenv("XFSR_HUD");
    if (!hud) hud = std::getenv("CROSSVULKAN_HUD");
    // Default ON; XFSR_HUD=0 disables.
    g_rt.hudOn = !(hud && hud[0] == '0');

    // Hotkeys mode: shift (default) | alt | ctrl | ctrl_alt | nav | bare | none
    const char* hk = std::getenv("EDGESCALE_HOTKEYS");
    if (!hk) hk = std::getenv("XFSR_HOTKEYS");
    const char* mod = std::getenv("EDGESCALE_HUD_MOD");
    if (!mod) mod = std::getenv("XFSR_HUD_MOD");

    if (hk && hk[0]) {
        if (std::strcmp(hk, "none") == 0 || std::strcmp(hk, "off") == 0 || std::strcmp(hk, "0") == 0) {
            g_rt.hotkeyMode = HotkeyMode::None;
        } else if (std::strcmp(hk, "alt") == 0) {
            g_rt.hotkeyMode = HotkeyMode::Alt;
        } else if (std::strcmp(hk, "ctrl") == 0) {
            g_rt.hotkeyMode = HotkeyMode::Ctrl;
        } else if (std::strcmp(hk, "ctrl_alt") == 0 || std::strcmp(hk, "ctrlalt") == 0) {
            g_rt.hotkeyMode = HotkeyMode::CtrlAlt;
        } else if (std::strcmp(hk, "nav") == 0) {
            g_rt.hotkeyMode = HotkeyMode::Nav;
        } else if (std::strcmp(hk, "bare") == 0) {
            g_rt.hotkeyMode = HotkeyMode::Bare;
        } else {
            g_rt.hotkeyMode = HotkeyMode::Shift;
        }
    } else if (mod && mod[0]) {
        if (mod[0] == '0') {
            g_rt.hotkeyMode = HotkeyMode::Bare;
        } else {
            g_rt.hotkeyMode = HotkeyMode::Shift;
        }
    } else {
        // Safe default: require Shift so in-game F1/F2 voting in games like CS2 never triggers!
        g_rt.hotkeyMode = HotkeyMode::Shift;
    }

    // Pin status strip at launch (Proton/Steam often eat F10). XFSR_OVERLAY=1 / --overlay.
    const char* ov = std::getenv("XFSR_OVERLAY");
    if (!ov) ov = std::getenv("CROSSVULKAN_OVERLAY");
    if (ov && ov[0] && ov[0] != '0') g_rt.overlayPinned = true;

    const char* preset = std::getenv("XFSR_PRESET");
    if (!preset || !preset[0]) preset = std::getenv("CROSSVULKAN_PRESET_MODE");
    g_rt.presetIdx = presetIndexFromName(preset);

    const char* scaleEnv = std::getenv("XFSR_SCALE");
    if (scaleEnv && scaleEnv[0]) {
        // Minimal float parse (no strtof — Proton glibc).
        float v = 0.f, frac = 0.f, place = 0.1f;
        const char* p = scaleEnv;
        while (*p == ' ' || *p == '\t') ++p;
        while (*p >= '0' && *p <= '9') {
            v = v * 10.f + static_cast<float>(*p - '0');
            ++p;
        }
        if (*p == '.') {
            ++p;
            while (*p >= '0' && *p <= '9') {
                frac += static_cast<float>(*p - '0') * place;
                place *= 0.1f;
                ++p;
            }
        }
        v += frac;
        if (v >= 0.125f && v <= 8.f) {
            g_rt.scale = v;
            g_rt.presetIdx = presetIndexFromScale(v);
        } else {
            g_rt.scale = kPresetScales[g_rt.presetIdx];
        }
    } else {
        g_rt.scale = kPresetScales[g_rt.presetIdx];
    }

    const char* rcas = std::getenv("XFSR_RCAS");
    if (!rcas) rcas = std::getenv("CROSSVULKAN_RCAS");
    g_rt.rcas = !(rcas && rcas[0] == '0');

    if (const char* es = std::getenv("XFSR_EDGE_SHARPNESS")) {
        float v = 0.f, frac = 0.f, place = 0.1f;
        const char* p = es;
        while (*p == ' ' || *p == '\t') ++p;
        while (*p >= '0' && *p <= '9') {
            v = v * 10.f + static_cast<float>(*p - '0');
            ++p;
        }
        if (*p == '.') {
            ++p;
            while (*p >= '0' && *p <= '9') {
                frac += static_cast<float>(*p - '0') * place;
                place *= 0.1f;
                ++p;
            }
        }
        v += frac;
        if (v >= 0.f && v <= 1.f) g_rt.edgeSharpness = v;
    }
    if (const char* em = xfsrEnv("XFSR_EDGE_MODE", "CROSSVULKAN_EDGE_MODE")) {
        if (std::strcmp(em, "competitive") == 0 || std::strcmp(em, "comp") == 0 ||
            std::strcmp(em, "esports") == 0 || std::strcmp(em, "3") == 0) {
            g_rt.edgeMode = 3;
            g_rt.edgePerf = false;
        } else if (std::strcmp(em, "nearest") == 0 || std::strcmp(em, "nn") == 0 ||
            std::strcmp(em, "pixel") == 0 || std::strcmp(em, "2") == 0) {
            g_rt.edgeMode = 2;
            g_rt.edgePerf = false;
        } else if (em[0] == '1' || std::strcmp(em, "perf") == 0 || std::strcmp(em, "lite") == 0 ||
                   std::strcmp(em, "edge-lite") == 0) {
            g_rt.edgeMode = 1;
            g_rt.edgePerf = true;
        } else if (em[0] == '0' || std::strcmp(em, "quality") == 0 || std::strcmp(em, "hq") == 0) {
            g_rt.edgeMode = 0;
            g_rt.edgePerf = false;
        }
    }

    if (const char* ups = xfsrEnv("XFSR_UPSCALER", "CROSSVULKAN_UPSCALER")) {
        g_rt.upscaler = parseUpscaler(ups);
    } else {
        const char* fsr = xfsrEnv("XFSR_LAYER_FSR");
        if (!fsr || !fsr[0]) fsr = xfsrEnv("XFSR_FSR", "CROSSVULKAN_FSR");
        g_rt.upscaler = (fsr && fsr[0] == '0') ? Upscaler::Blit : Upscaler::Fsr1;
    }

    const char* off = std::getenv("XFSR_OFFLOAD");
    if (!off) off = std::getenv("CROSSVULKAN_OFFLOAD");
    // Default ON (offload to iGPU); explicit XFSR_OFFLOAD=0 disables.
    const bool offloadExplicitOff = off && off[0] == '0';
    g_rt.offload = !offloadExplicitOff;

    const char* ow = std::getenv("XFSR_IGPU_PRESENT");
    if (!ow) ow = std::getenv("CROSSVULKAN_IGPU_PRESENT");
    g_rt.oneWayForce = false;
    if (ow && (std::strcmp(ow, "force") == 0 || ow[0] == '2')) {
        g_rt.oneWay = true;
        g_rt.oneWayForce = true;
        if (!offloadExplicitOff) g_rt.offload = true;
    } else if (ow && ow[0] == '0') {
        g_rt.oneWay = false;
    } else if (ow && ow[0] == '1') {
        g_rt.oneWay = true;
        if (!offloadExplicitOff) g_rt.offload = true;
    } else {
        // Layer default: prefer one-way when offload is on (any vendor; MangoHud still gates).
        g_rt.oneWay = g_rt.offload;
    }

    const char* adaptive = std::getenv("XFSR_ADAPTIVE");
    if (!adaptive) adaptive = std::getenv("CROSSVULKAN_ADAPTIVE");
    g_rt.adaptive = (adaptive && adaptive[0] && adaptive[0] != '0');

    const char* tfps = std::getenv("XFSR_TARGET_FPS");
    if (!tfps) tfps = std::getenv("CROSSVULKAN_TARGET_FPS");
    if (tfps && tfps[0]) {
        float v = 0.f;
        const char* p = tfps;
        while (*p == ' ' || *p == '\t') ++p;
        while (*p >= '0' && *p <= '9') {
            v = v * 10.f + static_cast<float>(*p - '0');
            ++p;
        }
        if (*p == '.') {
            ++p;
            float place = 0.1f;
            while (*p >= '0' && *p <= '9') {
                v += static_cast<float>(*p - '0') * place;
                place *= 0.1f;
                ++p;
            }
        }
        if (v < 20.f) v = 20.f;
        if (v > 1000.f) v = 1000.f;
        g_rt.targetFps = v;
    }

    const char* cpath = xfsrEnv("XFSR_CONTROL", "CROSSVULKAN_CONTROL");
    if (cpath && cpath[0]) {
        std::snprintf(g_rt.controlPath, sizeof(g_rt.controlPath), "%s", cpath);
    } else {
#if defined(_WIN32)
        const char* tmp = std::getenv("TEMP");
        if (!tmp || !tmp[0]) tmp = std::getenv("TMP");
        if (!tmp || !tmp[0]) tmp = "C:\\Windows\\Temp";
        std::snprintf(g_rt.controlPath, sizeof(g_rt.controlPath), "%s\\edgescale-control-%d.txt",
                      tmp, static_cast<int>(getpid()));
#else
        const char* rt = std::getenv("XDG_RUNTIME_DIR");
        if (rt && rt[0])
            std::snprintf(g_rt.controlPath, sizeof(g_rt.controlPath), "%s/edgescale-control", rt);
        else
            std::snprintf(g_rt.controlPath, sizeof(g_rt.controlPath), "/tmp/edgescale-control-%d",
                          static_cast<int>(getpid()));
#endif
    }
    {
#if defined(_WIN32)
        const char* tmp = std::getenv("TEMP");
        if (!tmp || !tmp[0]) tmp = std::getenv("TMP");
        if (!tmp || !tmp[0]) tmp = "C:\\Windows\\Temp";
        std::snprintf(g_rt.statusPath, sizeof(g_rt.statusPath), "%s\\edgescale-status-%d.txt",
                      tmp, static_cast<int>(getpid()));
#else
        const char* rt = std::getenv("XDG_RUNTIME_DIR");
        if (rt && rt[0])
            std::snprintf(g_rt.statusPath, sizeof(g_rt.statusPath), "%s/edgescale-status", rt);
        else
            std::snprintf(g_rt.statusPath, sizeof(g_rt.statusPath), "/tmp/edgescale-status-%d",
                          static_cast<int>(getpid()));
#endif
    }

    g_rt.lastScaleChange = std::chrono::steady_clock::now();

    g_fanWorker.probe();
    if (const char* fanEnv = xfsrEnv("EDGESCALE_FAN", "XFSR_FAN")) {
        if (std::strcmp(fanEnv, "max") == 0 || std::strcmp(fanEnv, "100") == 0) {
            setFanModeLocked(FanMode::Max, 100);
        } else if (std::strcmp(fanEnv, "balanced") == 0 || std::strcmp(fanEnv, "75") == 0) {
            setFanModeLocked(FanMode::Balanced, 75);
        } else if (std::strcmp(fanEnv, "quiet") == 0 || std::strcmp(fanEnv, "50") == 0) {
            setFanModeLocked(FanMode::Quiet, 50);
        } else if (std::strcmp(fanEnv, "opt") == 0 || std::strcmp(fanEnv, "auto-opt") == 0) {
            setFanModeLocked(FanMode::AutoOpt, -1);
        } else if (std::strcmp(fanEnv, "auto") == 0 || std::strcmp(fanEnv, "ec") == 0) {
            setFanModeLocked(FanMode::Auto, -1);
        } else {
            int pct = std::atoi(fanEnv);
            if (pct >= 10 && pct <= 100) {
                setFanModeLocked(FanMode::Custom, pct);
            }
        }
    }

    if (g_rt.hudOn) {
        std::fprintf(stderr,
                     "[edgescale] hotkeys: %s%s\n"
                     "[edgescale] control file: %s (preset|edge_mode|fsr|offload|one-way|fan|overlay|adaptive|hotkeys|status)\n",
                     hotkeyModeDescription(g_rt.hotkeyMode),
                     g_rt.overlayPinned ? " [overlay pinned]" : "", g_rt.controlPath);
        // Touch the control file so the path always exists for scripting.
        std::ofstream touch(g_rt.controlPath, std::ios::app);
#if !defined(_WIN32)
        const char* rt = std::getenv("XDG_RUNTIME_DIR");
        if (rt && rt[0]) {
            char legacyCtrl[512];
            std::snprintf(legacyCtrl, sizeof(legacyCtrl), "%s/xfsr-control", rt);
            if (::symlink(g_rt.controlPath, legacyCtrl) != 0 && errno != EEXIST) {
                /* best-effort legacy path */
            }
            char legacyStatus[512];
            std::snprintf(legacyStatus, sizeof(legacyStatus), "%s/xfsr-status", rt);
            if (::symlink(g_rt.statusPath, legacyStatus) != 0 && errno != EEXIST) {
                /* best-effort legacy path */
            }
        }
#endif
    }
    if (g_rt.adaptive && !g_rt.loggedAdaptive) {
        g_rt.loggedAdaptive = true;
        std::fprintf(stderr, "[xfsr] adaptive FSR on — target %.0f FPS (continuous scale)\n",
                     g_rt.targetFps);
    }
}

bool enabled() {
    initFromEnv();
    std::lock_guard<std::mutex> lock(g_rt.mu);
    return g_rt.hudOn;
}

Snapshot poll() {
    initFromEnv();
    Snapshot s{};
    std::lock_guard<std::mutex> lock(g_rt.mu);
    if (!g_rt.hudOn) {
        s.scale = g_rt.scale;
        s.preset = kPresetNames[g_rt.presetIdx];
        s.rcas = g_rt.rcas;
        s.upscaler = g_rt.upscaler;
        s.edgeSharpness = g_rt.edgeSharpness;
        s.edgePerf = g_rt.edgePerf;
        s.edgeMode = g_rt.edgeMode;
        s.fanMode = g_rt.fanMode;
        s.fanPct = g_rt.fanPct;
        s.cpuTempC = g_metrics.cpuTempC;
        s.fsr = (g_rt.upscaler == Upscaler::Fsr1);
        s.offload = g_rt.offload;
        s.oneWay = g_rt.oneWay;
        s.oneWayForce = g_rt.oneWayForce;
        formatStatusLocked(s.status, sizeof(s.status));
        return s;
    }
    static auto s_lastPoll = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - s_lastPoll >= std::chrono::milliseconds(16)) {
        s_lastPoll = now;
        pollHotkeysLocked();
        pollControlFileLocked();
    }

    s.scale = g_rt.scale;
    s.preset = kPresetNames[g_rt.presetIdx];
    s.rcas = g_rt.rcas;
    s.upscaler = g_rt.upscaler;
    s.edgeSharpness = g_rt.edgeSharpness;
    s.edgePerf = g_rt.edgePerf;
    s.edgeMode = g_rt.edgeMode;
    s.fanMode = g_rt.fanMode;
    s.fanPct = g_rt.fanPct;
    s.cpuTempC = g_metrics.cpuTempC;
    s.fsr = (g_rt.upscaler == Upscaler::Fsr1);
    s.offload = g_rt.offload;
    s.oneWay = g_rt.oneWay;
    s.oneWayForce = g_rt.oneWayForce;
    s.overlayPinned = g_rt.overlayPinned;
    s.recreateSwapchain = g_rt.recreate;
    s.reconfigureOffload = g_rt.reconfigureOffload;
    s.retryOffload = g_rt.retryOffload;
    const bool toastLive = std::chrono::steady_clock::now() < g_rt.toastUntil;
    s.showOverlay = g_rt.overlayPinned || toastLive;
    if (g_rt.overlayPinned)
        formatStatusLocked(s.status, sizeof(s.status));
    else if (toastLive)
        std::snprintf(s.status, sizeof(s.status), "%s", g_rt.toast);
    else
        s.status[0] = '\0';
    return s;
}

Snapshot peek() {
    initFromEnv();
    Snapshot s{};
    std::lock_guard<std::mutex> lock(g_rt.mu);
    s.scale = g_rt.scale;
    s.preset = kPresetNames[g_rt.presetIdx];
    s.rcas = g_rt.rcas;
    s.upscaler = g_rt.upscaler;
    s.edgeSharpness = g_rt.edgeSharpness;
    s.edgePerf = g_rt.edgePerf;
    s.edgeMode = g_rt.edgeMode;
    s.fanMode = g_rt.fanMode;
    s.fanPct = g_rt.fanPct;
    s.cpuTempC = g_metrics.cpuTempC;
    s.fsr = (g_rt.upscaler == Upscaler::Fsr1);
    s.offload = g_rt.offload;
    s.oneWay = g_rt.oneWay;
    s.oneWayForce = g_rt.oneWayForce;
    s.overlayPinned = g_rt.overlayPinned;
    s.recreateSwapchain = g_rt.recreate;
    s.reconfigureOffload = g_rt.reconfigureOffload;
    s.retryOffload = g_rt.retryOffload;
    formatStatusLocked(s.status, sizeof(s.status));
    return s;
}

void setFanMode(FanMode mode, int customPct) {
    initFromEnv();
    std::lock_guard<std::mutex> lock(g_rt.mu);
    setFanModeLocked(mode, customPct);
}

void clearRecreateRequest() {
    std::lock_guard<std::mutex> lock(g_rt.mu);
    g_rt.recreate = false;
}

void clearReconfigureOffload() {
    std::lock_guard<std::mutex> lock(g_rt.mu);
    g_rt.reconfigureOffload = false;
}

void clearRetryOffload() {
    std::lock_guard<std::mutex> lock(g_rt.mu);
    g_rt.retryOffload = false;
}

void showToast(const char* msg) {
    initFromEnv();
    std::lock_guard<std::mutex> lock(g_rt.mu);
    armToastLocked(msg);
}

void destroyOverlay(OverlayGpu* ov) {
    if (!ov) return;
    if (ov->ctx && ov->ctx->device) {
        ov->ctx->loadDispatch();
        auto& d = ov->ctx->vk;
        if (ov->stagingMapped && d.UnmapMemory) {
            d.UnmapMemory(ov->ctx->device, ov->stagingMem);
            ov->stagingMapped = nullptr;
        }
        if (ov->staging && d.DestroyBuffer) d.DestroyBuffer(ov->ctx->device, ov->staging, nullptr);
        if (ov->stagingMem && d.FreeMemory) d.FreeMemory(ov->ctx->device, ov->stagingMem, nullptr);
    }
    ov->image.destroy();
    ov->staging = VK_NULL_HANDLE;
    ov->stagingMem = VK_NULL_HANDLE;
    ov->stagingMapped = nullptr;
    ov->width = ov->height = 0;
    ov->lastWrapW = 0;
    ov->lastText[0] = '\0';
    ov->ctx = nullptr;
}

bool recordOverlay(OverlayGpu* ov, DeviceContext* ctx, VkCommandBuffer cmd, VkImage swapImage,
                   uint32_t swapW, uint32_t swapH, const char* text) {
    if (!ov || !ctx || !cmd || !swapImage || !text || !text[0]) return false;
    if (swapW < 64 || swapH < 32) return false;

    constexpr uint32_t kOw = 1600;
    constexpr uint32_t kOh = 160;
    ensureOverlayGpu(ov, ctx, kOw, kOh);
    if (!ov->stagingMapped || !ov->image.image) return false;

    ctx->loadDispatch();
    auto& d = ctx->vk;
    if (!d.CmdCopyBufferToImage || !d.CmdBlitImage) return false;

    const uint32_t screenMaxW = (swapW > 24u) ? (swapW - 24u) : ov->width;
    const uint32_t wrapW = std::min(ov->width, screenMaxW);

    if (std::strncmp(ov->lastText, text, sizeof(ov->lastText)) != 0 || ov->lastWrapW != wrapW) {
        std::vector<uint8_t> pixels(static_cast<size_t>(ov->width) * ov->height * 4u);
        rasterizeText(pixels.data(), ov->width, ov->height, text, wrapW);
        std::memcpy(ov->stagingMapped, pixels.data(), pixels.size());
        std::snprintf(ov->lastText, sizeof(ov->lastText), "%s", text);
        ov->lastWrapW = wrapW;

        transition(ctx, cmd, ov->image.image, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);

        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {ov->width, ov->height, 1};
        d.CmdCopyBufferToImage(cmd, ov->staging, ov->image.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        transition(ctx, cmd, ov->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_TRANSFER_READ_BIT);
    }

    const TextLayout layout = computeLayout(text, wrapW, ov->height);
    const uint32_t contentW = layout.boxW;
    const uint32_t contentH = layout.boxH;

    // Bottom-left — top is usually MangoHud.
    const int32_t margin = 12;
    const int32_t dw =
        static_cast<int32_t>(std::min(contentW, swapW > 24u ? swapW - 24u : contentW));
    const int32_t dh =
        static_cast<int32_t>(std::min(contentH, swapH > 24u ? swapH - 24u : contentH));
    const int32_t dstY0 = std::max(0, static_cast<int32_t>(swapH) - dh - margin);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {dw, dh, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {margin, dstY0, 0};
    blit.dstOffsets[1] = {margin + dw, dstY0 + dh, 1};
    d.CmdBlitImage(cmd, ov->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapImage,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    return true;
}

const char* fanModeName(FanMode mode) {
    switch (mode) {
    case FanMode::Auto: return "AUTO";
    case FanMode::AutoOpt: return "AUTO-OPT";
    case FanMode::Balanced: return "BALANCED";
    case FanMode::Max: return "MAX";
    case FanMode::Quiet: return "QUIET";
    case FanMode::Custom: return "CUSTOM";
    default: return "AUTO";
    }
}

const char* hotkeyModeName(HotkeyMode mode) {
    switch (mode) {
    case HotkeyMode::Shift: return "shift";
    case HotkeyMode::Alt: return "alt";
    case HotkeyMode::Ctrl: return "ctrl";
    case HotkeyMode::CtrlAlt: return "ctrl_alt";
    case HotkeyMode::Nav: return "nav";
    case HotkeyMode::Bare: return "bare";
    case HotkeyMode::None: return "none";
    default: return "shift";
    }
}

const char* hotkeyModeDescription(HotkeyMode mode) {
    switch (mode) {
    case HotkeyMode::Shift: return "Shift+F1..F6, Shift+F10 (safe default)";
    case HotkeyMode::Alt: return "Alt+F1..F6, Alt+F10";
    case HotkeyMode::Ctrl: return "Ctrl+F1..F6, Ctrl+F10";
    case HotkeyMode::CtrlAlt: return "Ctrl+Alt+F1..F6, Ctrl+Alt+F10";
    case HotkeyMode::Nav: return "Insert/Home/PgUp/Del/End/PgDn/Pause (no F-keys)";
    case HotkeyMode::Bare: return "bare F1..F6, F10 (may conflict with in-game actions)";
    case HotkeyMode::None: return "disabled";
    default: return "Shift+F1..F6";
    }
}

} // namespace xfsr_ctrl
