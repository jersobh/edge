#include "vram_monitor.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

static void printUsage(const char* prog) {
    std::printf("Usage: %s [options]\n", prog);
    std::printf("Hybrid VRAM & Memory Hierarchy Observability Tool\n\n");
    std::printf("Options:\n");
    std::printf("  --status, --once         Print single snapshot table and exit (default)\n");
    std::printf("  --json                   Dump full JSON metrics and exit\n");
    std::printf("  --daemon, --watch        Continuously monitor memory tiers every second\n");
    std::printf("  --score                  Evaluate residency score for an allocation:\n");
    std::printf("      --freq <F>           Recent access frequency (default: 1.0)\n");
    std::printf("      --reuse <R>          Probability of reuse 0..1 (default: 1.0)\n");
    std::printf("      --cost <C>           Reconstruction / reload cost (default: 1.0)\n");
    std::printf("      --mb <MB>            Resource size in Megabytes (default: 64)\n");
    std::printf("      --xfer <A>           Transfer cost penalty (default: 0.5)\n");
    std::printf("  --help                   Show this help\n");
}

int main(int argc, char** argv) {
    bool jsonOutput = false;
    bool daemonMode = false;
    bool evalScore = false;
    edgescale::ResidencyParams resParams{};
    resParams.sizeBytes = 64ULL * 1024ULL * 1024ULL; // 64 MB default

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--json") jsonOutput = true;
        else if (a == "--daemon" || a == "--watch") daemonMode = true;
        else if (a == "--score") evalScore = true;
        else if (a == "--freq" && i + 1 < argc) { evalScore = true; resParams.accessFreq = std::atof(argv[++i]); }
        else if (a == "--reuse" && i + 1 < argc) { evalScore = true; resParams.reuseProb = std::atof(argv[++i]); }
        else if (a == "--cost" && i + 1 < argc) { evalScore = true; resParams.reconstructCost = std::atof(argv[++i]); }
        else if (a == "--mb" && i + 1 < argc) { evalScore = true; resParams.sizeBytes = (uint64_t)(std::atof(argv[++i]) * 1024.0 * 1024.0); }
        else if (a == "--xfer" && i + 1 < argc) { evalScore = true; resParams.transferCost = std::atof(argv[++i]); }
        else if (a == "--status" || a == "--once") {}
        else if (a == "--help" || a == "-h") {
            printUsage(argv[0]);
            return 0;
        }
    }

    edgescale::MemoryMonitor mon;
    mon.init();

    if (jsonOutput) {
        auto snap = mon.poll();
        std::printf("%s\n", mon.toJson(snap).c_str());
        return 0;
    }

    if (evalScore) {
        auto snap = mon.poll();
        double score = edgescale::calculateResidencyScore(resParams);
        const char* tier = edgescale::recommendResidencyTier(score, snap, resParams.sizeBytes);
        std::printf("=================================================================\n");
        std::printf("  Hybrid VRAM Manager: Weighted Residency Evaluation\n");
        std::printf("=================================================================\n");
        std::printf("  Formula: S_i = (F_i * R_i * C_i) / (Size_MB * (1 + Transfer_cost))\n");
        std::printf("  Access Frequency (F) : %.2f\n", resParams.accessFreq);
        std::printf("  Reuse Probability (R): %.2f\n", resParams.reuseProb);
        std::printf("  Reconstruct Cost (C) : %.2f\n", resParams.reconstructCost);
        std::printf("  Resource Size (B)    : %.2f MB\n", resParams.sizeBytes / (1024.0 * 1024.0));
        std::printf("  Transfer Penalty (A) : %.2f\n", resParams.transferCost);
        std::printf("-----------------------------------------------------------------\n");
        std::printf("  Calculated Score (S) : %.4f\n", score);
        std::printf("  Recommended Placement: %s\n", tier);
        std::printf("=================================================================\n");
        return 0;
    }

    auto printSnapshot = [](const edgescale::TierSnapshot& s) {
        std::printf("\033[H\033[J"); // Clear screen for watch mode
        std::printf("================================================================================\n");
        std::printf("                  Hybrid VRAM & Memory Hierarchy Telemetry                      \n");
        std::printf("================================================================================\n");
        
        // Tier 1: NVIDIA dGPU VRAM
        std::printf(" [Tier 1: Dedicated VRAM] %s\n", s.nvidia.available ? s.nvidia.deviceName.c_str() : "Not Detected");
        if (s.nvidia.available) {
            uint64_t totalMb = s.nvidia.vramTotalBytes / (1024 * 1024);
            uint64_t usedMb = s.nvidia.vramUsedBytes / (1024 * 1024);
            uint64_t freeMb = s.nvidia.vramFreeBytes / (1024 * 1024);
            std::printf("   VRAM Usage    : %5llu MB / %5llu MB (%3llu%% used, %5llu MB free)\n",
                        (unsigned long long)usedMb, (unsigned long long)totalMb,
                        totalMb ? (unsigned long long)(usedMb * 100 / totalMb) : 0ULL,
                        (unsigned long long)freeMb);
            std::printf("   GPU / Mem Util: %3u%% / %3u%%  | Temp: %u°C\n",
                        s.nvidia.gpuUtilPercent, s.nvidia.memUtilPercent, s.nvidia.tempC);
            std::printf("   PCIe Bandwidth: TX: %6u KB/s | RX: %6u KB/s\n",
                        s.nvidia.pcieTxKbSec, s.nvidia.pcieRxKbSec);
        } else {
            std::printf("   NVIDIA GPU telemetry offline or non-privileged.\n");
        }
        std::printf("--------------------------------------------------------------------------------\n");

        // Tier 2: System RAM
        uint64_t hostTotMb = s.host.totalBytes / (1024 * 1024);
        uint64_t hostAvailMb = s.host.availableBytes / (1024 * 1024);
        uint64_t hostUsedMb = (hostTotMb > hostAvailMb) ? (hostTotMb - hostAvailMb) : 0;
        std::printf(" [Tier 2: System RAM (Overflow / UVM Staging)]\n");
        std::printf("   RAM Capacity  : %5llu MB / %5llu MB (%3llu%% in use, %5llu MB available)\n",
                    (unsigned long long)hostUsedMb, (unsigned long long)hostTotMb,
                    hostTotMb ? (unsigned long long)(hostUsedMb * 100 / hostTotMb) : 0ULL,
                    (unsigned long long)hostAvailMb);
        std::printf("   Swap Free     : %5llu MB / %5llu MB\n",
                    (unsigned long long)(s.host.swapFreeBytes / (1024 * 1024)),
                    (unsigned long long)(s.host.swapTotalBytes / (1024 * 1024)));
        std::printf("--------------------------------------------------------------------------------\n");

        // Optional: AMD iGPU Companion
        std::printf(" [Companion iGPU: Display & Secondary Engine] %s\n", s.amd.available ? s.amd.deviceName.c_str() : "Not Detected");
        if (s.amd.available) {
            std::printf("   Dedicated VRAM: %5llu MB / %5llu MB\n",
                        (unsigned long long)(s.amd.vramUsedBytes / (1024 * 1024)),
                        (unsigned long long)(s.amd.vramTotalBytes / (1024 * 1024)));
            std::printf("   GTT (Host RAM): %5llu MB / %5llu MB\n",
                        (unsigned long long)(s.amd.gttUsedBytes / (1024 * 1024)),
                        (unsigned long long)(s.amd.gttTotalBytes / (1024 * 1024)));
        }
        std::printf("--------------------------------------------------------------------------------\n");

        // Tier 3: Storage
        std::printf(" [Tier 3: Storage Compressed Cache] Mount: %s\n", s.disk.path.c_str());
        std::printf("   NVMe Capacity : %5llu GB / %5llu GB available\n",
                    (unsigned long long)(s.disk.availableBytes / (1024 * 1024 * 1024)),
                    (unsigned long long)(s.disk.totalBytes / (1024 * 1024 * 1024)));
        std::printf("================================================================================\n");
    };

    if (daemonMode) {
        while (true) {
            auto snap = mon.poll();
            printSnapshot(snap);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    } else {
        auto snap = mon.poll();
        printSnapshot(snap);
    }

    return 0;
}

