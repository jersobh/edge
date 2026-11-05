#pragma once

#include <cuda_runtime.h>
#include <cstdint>
#include <vector>
#include <string>

namespace edgescale::ai {

struct ManagedTensor {
    void* ptr = nullptr;
    size_t sizeBytes = 0;
    int preferredDevice = 0;
};

// Stream-conscious layer-wise memory virtualization manager
class ManagedLayerEngine {
public:
    explicit ManagedLayerEngine(int deviceId = 0);
    ~ManagedLayerEngine();

    bool init();
    void shutdown();

    // Allocate memory tier backed by HMM Unified Memory
    ManagedTensor allocateLayerBuffer(size_t sizeBytes);
    void freeLayerBuffer(ManagedTensor& tensor);

    // Asynchronously stage layer to GPU VRAM
    cudaError_t prefetchToGpu(const ManagedTensor& tensor, cudaStream_t stream = nullptr);

    // Asynchronously evict layer back to Host System RAM (Tier 2)
    cudaError_t evictToHost(const ManagedTensor& tensor, cudaStream_t stream = nullptr);

private:
    int m_deviceId = 0;
    cudaMemLocation m_locDev{};
    cudaMemLocation m_locHost{};
};

inline ManagedLayerEngine::ManagedLayerEngine(int deviceId) : m_deviceId(deviceId) {
    m_locDev.type = cudaMemLocationTypeDevice;
    m_locDev.id = m_deviceId;
    m_locHost.type = cudaMemLocationTypeHost;
    m_locHost.id = 0;
}

inline ManagedLayerEngine::~ManagedLayerEngine() {
    shutdown();
}

inline bool ManagedLayerEngine::init() {
    return (cudaSetDevice(m_deviceId) == cudaSuccess);
}

inline void ManagedLayerEngine::shutdown() {
}

inline ManagedTensor ManagedLayerEngine::allocateLayerBuffer(size_t sizeBytes) {
    ManagedTensor t{};
    t.sizeBytes = sizeBytes;
    t.preferredDevice = m_deviceId;
    if (cudaMallocManaged(&t.ptr, sizeBytes) == cudaSuccess) {
        cudaMemAdvise(t.ptr, sizeBytes, cudaMemAdviseSetPreferredLocation, m_locDev);
        cudaMemAdvise(t.ptr, sizeBytes, cudaMemAdviseSetAccessedBy, m_locDev);
    }
    return t;
}

inline void ManagedLayerEngine::freeLayerBuffer(ManagedTensor& tensor) {
    if (tensor.ptr) {
        cudaFree(tensor.ptr);
        tensor.ptr = nullptr;
        tensor.sizeBytes = 0;
    }
}

inline cudaError_t ManagedLayerEngine::prefetchToGpu(const ManagedTensor& tensor, cudaStream_t stream) {
    if (!tensor.ptr) return cudaErrorInvalidValue;
    return cudaMemPrefetchAsync(tensor.ptr, tensor.sizeBytes, m_locDev, 0, stream);
}

inline cudaError_t ManagedLayerEngine::evictToHost(const ManagedTensor& tensor, cudaStream_t stream) {
    if (!tensor.ptr) return cudaErrorInvalidValue;
    return cudaMemPrefetchAsync(tensor.ptr, tensor.sizeBytes, m_locHost, 0, stream);
}

} // namespace edgescale::ai

