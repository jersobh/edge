#include "vram_monitor.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/statvfs.h>
#include <glob.h>
#include <dlfcn.h>

#if defined(__has_include)
#  if __has_include(<nvml.h>)
#    include <nvml.h>
#    define HAS_NVML_HEADERS 1
#  endif
#endif

namespace edgescale {

double calculateResidencyScore(const ResidencyParams& p) {
    if (p.sizeBytes == 0) return 0.0;
    // Normalize size to Megabytes for clean readable scores
    double sizeMb = static_cast<double>(p.sizeBytes) / (1024.0 * 1024.0);
    if (sizeMb < 0.001) sizeMb = 0.001;
    double denom = sizeMb * (1.0 + (p.transferCost > 0.0 ? p.transferCost : 0.0));
    return (p.accessFreq * p.reuseProb * p.reconstructCost) / denom;
}

const char* recommendResidencyTier(double score, const TierSnapshot& snap, uint64_t resourceBytes) {
    // If resource is small (< 16MB) and score is high (> 2.0) and NVIDIA VRAM has space -> Tier 1 (VRAM)
    // If score is moderate (0.2 .. 2.0) or VRAM is near capacity -> Tier 2 (System RAM)
    // If score is low (< 0.2) and resource is large or inactive -> Tier 3 (NVMe SSD Compressed Cache)
    const uint64_t vramMargin = 256ULL * 1024ULL * 1024ULL; // keep 256MB free in VRAM
    bool vramHasSpace = (snap.nvidia.available && (snap.nvidia.vramFreeBytes > resourceBytes + vramMargin));

    if (score >= 1.5 && vramHasSpace) {
        return "Tier 1 (NVIDIA VRAM / Device Local)";
    } else if (score >= 0.2) {
        return "Tier 2 (Host System RAM / Pinned UVM)";
    } else {
        return "Tier 3 (NVMe SSD Compressed Cache)";
    }
}

MemoryMonitor::MemoryMonitor() = default;

MemoryMonitor::~MemoryMonitor() {
    shutdown();
}

bool MemoryMonitor::init() {
#if defined(HAS_NVML_HEADERS)
    nvmlReturn_t r = nvmlInit();
    if (r == NVML_SUCCESS) {
        m_nvmlInitialized = true;
        nvmlDevice_t dev = nullptr;
        if (nvmlDeviceGetHandleByIndex(0, &dev) == NVML_SUCCESS) {
            m_nvmlDevice = dev;
        }
        return true;
    }
#endif
    return false;
}

void MemoryMonitor::shutdown() {
#if defined(HAS_NVML_HEADERS)
    if (m_nvmlInitialized) {
        nvmlShutdown();
        m_nvmlInitialized = false;
        m_nvmlDevice = nullptr;
    }
#endif
}

static HostMemoryMetrics readHostMemory() {
    HostMemoryMetrics m{};
    std::ifstream f("/proc/meminfo");
    if (!f.is_open()) return m;

    std::string key;
    uint64_t val;
    std::string unit;
    while (f >> key >> val) {
        f >> unit;
        uint64_t bytes = val * 1024ULL;
        if (key == "MemTotal:") m.totalBytes = bytes;
        else if (key == "MemFree:") m.freeBytes = bytes;
        else if (key == "MemAvailable:") m.availableBytes = bytes;
        else if (key == "Cached:") m.cachedBytes = bytes;
        else if (key == "SwapTotal:") m.swapTotalBytes = bytes;
        else if (key == "SwapFree:") m.swapFreeBytes = bytes;
    }
    return m;
}

static AmdMetrics readAmdMemory() {
    AmdMetrics m{};
    // Check drm sysfs for AMD GPU cards
    glob_t g{};
    if (glob("/sys/class/drm/card*/device/mem_info_vram_total", 0, nullptr, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; ++i) {
            std::string basePath = g.gl_pathv[i];
            size_t idx = basePath.rfind("/mem_info_vram_total");
            if (idx == std::string::npos) continue;
            std::string dir = basePath.substr(0, idx);

            auto readVal = [](const std::string& path) -> uint64_t {
                std::ifstream f(path);
                uint64_t v = 0;
                if (f >> v) return v;
                return 0;
            };

            m.vramTotalBytes = readVal(dir + "/mem_info_vram_total");
            m.vramUsedBytes = readVal(dir + "/mem_info_vram_used");
            m.gttTotalBytes = readVal(dir + "/mem_info_gtt_total");
            m.gttUsedBytes = readVal(dir + "/mem_info_gtt_used");
            m.available = (m.vramTotalBytes > 0);
            m.deviceName = "AMD Integrated GPU (amdgpu)";
            break;
        }
        globfree(&g);
    }
    return m;
}

static DiskStorageMetrics readDiskStorage(const char* mountPath) {
    DiskStorageMetrics m{};
    m.path = mountPath ? mountPath : "/";
    struct statvfs s{};
    if (statvfs(m.path.c_str(), &s) == 0) {
        m.totalBytes = static_cast<uint64_t>(s.f_blocks) * s.f_frsize;
        m.availableBytes = static_cast<uint64_t>(s.f_bavail) * s.f_frsize;
    }
    return m;
}

TierSnapshot MemoryMonitor::poll() {
    TierSnapshot snap{};
    snap.host = readHostMemory();
    snap.amd = readAmdMemory();
    snap.disk = readDiskStorage("/home/jeff");

#if defined(HAS_NVML_HEADERS)
    if (m_nvmlInitialized && m_nvmlDevice) {
        nvmlDevice_t dev = static_cast<nvmlDevice_t>(m_nvmlDevice);
        char name[96]{};
        if (nvmlDeviceGetName(dev, name, sizeof(name)) == NVML_SUCCESS) {
            snap.nvidia.deviceName = name;
        }
        nvmlMemory_t mem{};
        if (nvmlDeviceGetMemoryInfo(dev, &mem) == NVML_SUCCESS) {
            snap.nvidia.vramTotalBytes = mem.total;
            snap.nvidia.vramFreeBytes = mem.free;
            snap.nvidia.vramUsedBytes = mem.used;
            snap.nvidia.available = true;
        }
        nvmlUtilization_t util{};
        if (nvmlDeviceGetUtilizationRates(dev, &util) == NVML_SUCCESS) {
            snap.nvidia.gpuUtilPercent = util.gpu;
            snap.nvidia.memUtilPercent = util.memory;
        }
        unsigned int tx = 0, rx = 0;
        if (nvmlDeviceGetPcieThroughput(dev, NVML_PCIE_UTIL_TX_BYTES, &tx) == NVML_SUCCESS) {
            snap.nvidia.pcieTxKbSec = tx;
        }
        if (nvmlDeviceGetPcieThroughput(dev, NVML_PCIE_UTIL_RX_BYTES, &rx) == NVML_SUCCESS) {
            snap.nvidia.pcieRxKbSec = rx;
        }
        unsigned int temp = 0;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        if (nvmlDeviceGetTemperature(dev, NVML_TEMPERATURE_GPU, &temp) == NVML_SUCCESS) {
            snap.nvidia.tempC = temp;
        }
#pragma GCC diagnostic pop
    }
#endif

    return snap;
}

std::string MemoryMonitor::toJson(const TierSnapshot& snap) const {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"tier1_nvidia\": {\n";
    ss << "    \"available\": " << (snap.nvidia.available ? "true" : "false") << ",\n";
    ss << "    \"device\": \"" << snap.nvidia.deviceName << "\",\n";
    ss << "    \"vram_total_mb\": " << (snap.nvidia.vramTotalBytes / (1024 * 1024)) << ",\n";
    ss << "    \"vram_used_mb\": " << (snap.nvidia.vramUsedBytes / (1024 * 1024)) << ",\n";
    ss << "    \"vram_free_mb\": " << (snap.nvidia.vramFreeBytes / (1024 * 1024)) << ",\n";
    ss << "    \"gpu_util_percent\": " << snap.nvidia.gpuUtilPercent << ",\n";
    ss << "    \"mem_util_percent\": " << snap.nvidia.memUtilPercent << ",\n";
    ss << "    \"pcie_tx_kb_s\": " << snap.nvidia.pcieTxKbSec << ",\n";
    ss << "    \"pcie_rx_kb_s\": " << snap.nvidia.pcieRxKbSec << ",\n";
    ss << "    \"temperature_c\": " << snap.nvidia.tempC << "\n";
    ss << "  },\n";
    ss << "  \"tier2_system_ram\": {\n";
    ss << "    \"total_mb\": " << (snap.host.totalBytes / (1024 * 1024)) << ",\n";
    ss << "    \"available_mb\": " << (snap.host.availableBytes / (1024 * 1024)) << ",\n";
    ss << "    \"free_mb\": " << (snap.host.freeBytes / (1024 * 1024)) << ",\n";
    ss << "    \"cached_mb\": " << (snap.host.cachedBytes / (1024 * 1024)) << ",\n";
    ss << "    \"swap_total_mb\": " << (snap.host.swapTotalBytes / (1024 * 1024)) << ",\n";
    ss << "    \"swap_free_mb\": " << (snap.host.swapFreeBytes / (1024 * 1024)) << "\n";
    ss << "  },\n";
    ss << "  \"amd_igpu_companion\": {\n";
    ss << "    \"available\": " << (snap.amd.available ? "true" : "false") << ",\n";
    ss << "    \"vram_total_mb\": " << (snap.amd.vramTotalBytes / (1024 * 1024)) << ",\n";
    ss << "    \"vram_used_mb\": " << (snap.amd.vramUsedBytes / (1024 * 1024)) << ",\n";
    ss << "    \"gtt_total_mb\": " << (snap.amd.gttTotalBytes / (1024 * 1024)) << ",\n";
    ss << "    \"gtt_used_mb\": " << (snap.amd.gttUsedBytes / (1024 * 1024)) << "\n";
    ss << "  },\n";
    ss << "  \"tier3_nvme_cache\": {\n";
    ss << "    \"mount\": \"" << snap.disk.path << "\",\n";
    ss << "    \"total_gb\": " << (snap.disk.totalBytes / (1024 * 1024 * 1024)) << ",\n";
    ss << "    \"available_gb\": " << (snap.disk.availableBytes / (1024 * 1024 * 1024)) << "\n";
    ss << "  }\n";
    ss << "}\n";
    return ss.str();
}

} // namespace edgescale
