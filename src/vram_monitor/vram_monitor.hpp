#pragma once

#include <cstdint>
#include <string>

namespace edgescale {

struct NvidiaMetrics {
    bool available = false;
    std::string deviceName;
    uint64_t vramTotalBytes = 0;
    uint64_t vramUsedBytes = 0;
    uint64_t vramFreeBytes = 0;
    uint32_t gpuUtilPercent = 0;
    uint32_t memUtilPercent = 0;
    uint32_t pcieTxKbSec = 0;
    uint32_t pcieRxKbSec = 0;
    uint32_t tempC = 0;
};

struct AmdMetrics {
    bool available = false;
    std::string deviceName;
    uint64_t vramTotalBytes = 0;
    uint64_t vramUsedBytes = 0;
    uint64_t gttTotalBytes = 0;
    uint64_t gttUsedBytes = 0;
};

struct HostMemoryMetrics {
    uint64_t totalBytes = 0;
    uint64_t availableBytes = 0;
    uint64_t freeBytes = 0;
    uint64_t cachedBytes = 0;
    uint64_t swapTotalBytes = 0;
    uint64_t swapFreeBytes = 0;
};

struct DiskStorageMetrics {
    uint64_t totalBytes = 0;
    uint64_t availableBytes = 0;
    std::string path;
};

struct TierSnapshot {
    NvidiaMetrics nvidia;
    AmdMetrics amd;
    HostMemoryMetrics host;
    DiskStorageMetrics disk;
};

struct ResidencyParams {
    double accessFreq = 1.0;     // F_i: Recent access frequency (calls/sec or norm)
    double reuseProb = 1.0;       // R_i: Probability of imminent reuse (0.0 .. 1.0)
    double reconstructCost = 1.0; // C_i: Cost of reconstructing or reload (norm)
    uint64_t sizeBytes = 1048576; // B_i: Resource size in bytes
    double transferCost = 0.5;    // A_i: PCIe / bus transfer cost penalty (norm)
};

// S_i = (F_i * R_i * C_i) / ( (B_i / 1MB) * (1.0 + A_i) )
double calculateResidencyScore(const ResidencyParams& p);

const char* recommendResidencyTier(double score, const TierSnapshot& snap, uint64_t resourceBytes);

class MemoryMonitor {
public:
    MemoryMonitor();
    ~MemoryMonitor();

    bool init();
    void shutdown();

    TierSnapshot poll();
    std::string toJson(const TierSnapshot& snap) const;

private:
    bool m_nvmlInitialized = false;
    void* m_nvmlDevice = nullptr;
};

} // namespace edgescale

