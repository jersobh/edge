#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cmath>

namespace edgescale::scheduler {

enum class Vendor {
    NVIDIA,
    AMD,
    INTEL,
    UNKNOWN
};

enum class MemoryTier {
    Tier1_DedicatedVRAM,     // Local GPU VRAM (fastest, private GDDR/HBM)
    Tier2_SecondaryGPU_VRAM, // Distributed compute on secondary device
    Tier3_HostRAM,           // System memory fallback (spill/staging via PCIe)
    Tier4_NVMeStorage        // Cold assets / model storage
};

struct GPUDevice {
    uint32_t device_id = 0;
    std::string name;
    Vendor vendor = Vendor::UNKNOWN;

    // Capacities (in bytes)
    uint64_t total_vram = 0;
    uint64_t available_vram = 0;

    // Measured Bandwidths (in bytes per second)
    uint64_t memory_bandwidth_bytes_per_sec = 0;      // e.g. 128 GB/s for GTX 1650 GDDR5
    uint64_t host_memory_bandwidth_bytes_per_sec = 0; // e.g. 8-12 GB/s over PCIe 3.0

    // Host Memory Visibility
    uint64_t free_host_visible_device_memory = 0;     // BAR 1 / ReBAR aperture
    uint64_t free_host_system_memory = 0;             // System DDR4 RAM pool

    // Driver Capabilities
    bool supports_peer_transfer = false;              // Evaluated per device-pair in topology
    bool supports_external_memory = false;            // DMA-BUF / VK_KHR_external_memory_fd
    bool supports_timeline_semaphore = false;         // VK_KHR_timeline_semaphore

    // Measured Performance Profiles (Empirical telemetry)
    float measured_compute_throughput = 0.0f;         // TFLOPs or prompt t/s
    float measured_transfer_bandwidth = 0.0f;         // GB/s across host/bus
    float measured_transfer_latency = 0.0f;           // Microseconds per dispatch
};

struct PeerTransferRoute {
    uint32_t src_device_id = 0;
    uint32_t dst_device_id = 0;
    bool has_direct_p2p = false;
    uint64_t bandwidth_bytes_per_sec = 0;
    float latency_us = 0.0f;
};

struct LayerWorkload {
    uint32_t layer_index = 0;
    uint64_t parameter_bytes = 0;
    uint64_t activation_bytes = 0;
    uint64_t flops_per_token = 0;
    bool is_decode_critical = true;
};

// Cost-Aware Placement Evaluation
struct PlacementDecision {
    uint32_t target_device_id = 0;
    MemoryTier target_tier = MemoryTier::Tier1_DedicatedVRAM;
    double estimated_latency_ms = 0.0;
    double cost_score = 0.0;
    std::string justification;
};

class CostAwareScheduler {
public:
    CostAwareScheduler() = default;

    void registerDevice(const GPUDevice& dev) {
        m_devices[dev.device_id] = dev;
    }

    void setPeerRoute(const PeerTransferRoute& route) {
        uint64_t key = (static_cast<uint64_t>(route.src_device_id) << 32) | route.dst_device_id;
        m_routes[key] = route;
    }

    // Evaluates placement cost:
    // Cost = TransferCost(src -> dst) + (ParamBytes / MemoryBW) + (FLOPs / ComputeThroughput)
    PlacementDecision scheduleLayer(const LayerWorkload& layer, uint32_t current_src_device) const {
        PlacementDecision bestDecision{};
        double minCost = 1e18;

        for (const auto& [devId, dev] : m_devices) {
            // Check VRAM headroom
            bool fitsInVram = (dev.available_vram >= layer.parameter_bytes);

            // Compute memory fetch time (decode is bandwidth bound)
            double memBw = static_cast<double>(dev.memory_bandwidth_bytes_per_sec);
            if (!fitsInVram) {
                // Must spill to Host RAM over PCIe
                memBw = static_cast<double>(dev.host_memory_bandwidth_bytes_per_sec);
            }
            if (memBw <= 0.0) memBw = 1.0;

            double fetchTimeMs = (static_cast<double>(layer.parameter_bytes) / memBw) * 1000.0;

            // Compute transfer time between devices if pipeline stage moves
            double transferTimeMs = 0.0;
            if (current_src_device != devId) {
                uint64_t key = (static_cast<uint64_t>(current_src_device) << 32) | devId;
                auto it = m_routes.find(key);
                if (it != m_routes.end() && it->second.bandwidth_bytes_per_sec > 0) {
                    transferTimeMs = (static_cast<double>(layer.activation_bytes) / it->second.bandwidth_bytes_per_sec) * 1000.0;
                    transferTimeMs += (it->second.latency_us / 1000.0);
                } else {
                    // Host-staged cross-vendor fallback (e.g. DMA-BUF over system RAM)
                    transferTimeMs = (static_cast<double>(layer.activation_bytes) / 8e9) * 1000.0 + 0.05;
                }
            }

            // Total predicted execution cost
            double totalCost = fetchTimeMs + transferTimeMs;

            if (totalCost < minCost) {
                minCost = totalCost;
                bestDecision.target_device_id = devId;
                bestDecision.target_tier = fitsInVram ? (devId == 0 ? MemoryTier::Tier1_DedicatedVRAM : MemoryTier::Tier2_SecondaryGPU_VRAM)
                                                      : MemoryTier::Tier3_HostRAM;
                bestDecision.estimated_latency_ms = totalCost;
                bestDecision.cost_score = totalCost;
                if (fitsInVram) {
                    bestDecision.justification = "Pinned in dedicated VRAM (high bandwidth)";
                } else {
                    bestDecision.justification = "Host RAM fallback (PCIe-bound spill)";
                }
            }
        }

        return bestDecision;
    }

private:
    std::unordered_map<uint32_t, GPUDevice> m_devices;
    std::unordered_map<uint64_t, PeerTransferRoute> m_routes;
};

} // namespace edgescale::scheduler

