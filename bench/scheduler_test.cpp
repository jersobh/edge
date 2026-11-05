#include "gpu_scheduler.hpp"
#include <cstdio>

int main() {
    using namespace edgescale::scheduler;

    CostAwareScheduler scheduler;

    // Register Device 1: NVIDIA GTX 1650 (Tier 1)
    GPUDevice d1{};
    d1.device_id = 1;
    d1.name = "NVIDIA GeForce GTX 1650";
    d1.vendor = Vendor::NVIDIA;
    d1.total_vram = 4ULL * 1024 * 1024 * 1024;
    d1.available_vram = 3500ULL * 1024 * 1024; // ~3.5 GB free
    d1.memory_bandwidth_bytes_per_sec = 128ULL * 1024 * 1024 * 1024; // 128 GB/s
    d1.host_memory_bandwidth_bytes_per_sec = 8ULL * 1024 * 1024 * 1024; // 8 GB/s over PCIe
    scheduler.registerDevice(d1);

    // Register Device 0: AMD Radeon Vega 10 (Tier 2)
    GPUDevice d0{};
    d0.device_id = 0;
    d0.name = "AMD Radeon Vega 10 (iGPU)";
    d0.vendor = Vendor::AMD;
    d0.total_vram = 16ULL * 1024 * 1024 * 1024;
    d0.available_vram = 14ULL * 1024 * 1024 * 1024; // 14 GB available in system RAM
    d0.memory_bandwidth_bytes_per_sec = 35ULL * 1024 * 1024 * 1024; // 35 GB/s shared DDR4
    d0.host_memory_bandwidth_bytes_per_sec = 35ULL * 1024 * 1024 * 1024;
    scheduler.registerDevice(d0);

    // Register cross-device route
    PeerTransferRoute route{};
    route.src_device_id = 1;
    route.dst_device_id = 0;
    route.has_direct_p2p = false;
    route.bandwidth_bytes_per_sec = 8ULL * 1024 * 1024 * 1024;
    route.latency_us = 50.0f;
    scheduler.setPeerRoute(route);

    // Evaluate layer scheduling for a 120 MB transformer block
    LayerWorkload layer{};
    layer.layer_index = 0;
    layer.parameter_bytes = 120ULL * 1024 * 1024;
    layer.activation_bytes = 8ULL * 1024 * 1024;

    auto decision = scheduler.scheduleLayer(layer, 1);
    std::printf("=================================================================\n");
    std::printf("  EdgeScale Cost-Aware Scheduler Evaluation\n");
    std::printf("=================================================================\n");
    std::printf("  Target Device : Device %u\n", decision.target_device_id);
    std::printf("  Selected Tier : %d\n", static_cast<int>(decision.target_tier));
    std::printf("  Estimated Time: %.3f ms\n", decision.estimated_latency_ms);
    std::printf("  Justification : %s\n", decision.justification.c_str());
    std::printf("=================================================================\n");

    return 0;
}

