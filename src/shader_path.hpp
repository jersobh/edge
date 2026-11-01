#pragma once

#include <cstdlib>
#include <string>
#include <sys/stat.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

// SPIR-V search order:
//   1. XFSR_SHADER_DIR env var
//   2. <module_dir>/shaders (build tree alongside libVkLayer_XFSR.so / xfsr-demo)
//   3. <module_dir>/../share/xfsr/shaders (installed prefix, e.g. ~/.local or /usr)
//   4. $XDG_DATA_HOME/xfsr/shaders or $HOME/.local/share/xfsr/shaders
//   5. ./shaders
inline std::string xfsrShaderDir() {
    auto dirHasShaders = [](const std::string& d) -> bool {
        if (d.empty()) return false;
        struct stat st{};
        std::string probe = d + "/fsr_easu_fp32.comp.spv";
        if (::stat(probe.c_str(), &st) == 0) return true;
        probe = d + "/edge_pass.comp.spv";
        return ::stat(probe.c_str(), &st) == 0;
    };

    if (const char* e = std::getenv("XFSR_SHADER_DIR")) {
        if (e[0]) return e;
    }

#if defined(_WIN32)
    HMODULE hm = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&xfsrShaderDir, &hm)) {
        char path[MAX_PATH];
        if (GetModuleFileNameA(hm, path, sizeof(path)) > 0) {
            std::string p(path);
            auto slash = p.find_last_of("\\/");
            if (slash != std::string::npos) {
                std::string modDir = p.substr(0, slash);
                if (dirHasShaders(modDir + "\\shaders")) return modDir + "\\shaders";
                if (dirHasShaders(modDir + "/shaders")) return modDir + "/shaders";
                if (dirHasShaders(modDir)) return modDir;
            }
        }
    }
    if (const char* appdata = std::getenv("LOCALAPPDATA")) {
        if (appdata[0] && dirHasShaders(std::string(appdata) + "\\EdgeScale\\shaders")) {
            return std::string(appdata) + "\\EdgeScale\\shaders";
        }
    }
#elif defined(__linux__) || defined(__APPLE__)
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&xfsrShaderDir), &info) != 0 && info.dli_fname) {
        std::string p(info.dli_fname);
        auto slash = p.find_last_of('/');
        if (slash != std::string::npos) {
            std::string modDir = p.substr(0, slash);
            if (dirHasShaders(modDir + "/shaders")) return modDir + "/shaders";
            if (dirHasShaders(modDir + "/../share/xfsr/shaders")) return modDir + "/../share/xfsr/shaders";
        }
    }
#endif

    if (const char* xdg = std::getenv("XDG_DATA_HOME")) {
        if (xdg[0] && dirHasShaders(std::string(xdg) + "/xfsr/shaders")) {
            return std::string(xdg) + "/xfsr/shaders";
        }
    }
    if (const char* home = std::getenv("HOME")) {
        if (home[0] && dirHasShaders(std::string(home) + "/.local/share/xfsr/shaders")) {
            return std::string(home) + "/.local/share/xfsr/shaders";
        }
    }
    if (dirHasShaders("/usr/local/share/xfsr/shaders")) return "/usr/local/share/xfsr/shaders";
    if (dirHasShaders("/usr/share/xfsr/shaders")) return "/usr/share/xfsr/shaders";

#ifdef SHADER_DIR
    return SHADER_DIR;
#else
    return "shaders";
#endif
}

inline std::string xfsrShaderPath(const char* file) {
    return xfsrShaderDir() + "/" + file;
}
