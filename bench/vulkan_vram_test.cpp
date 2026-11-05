#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <volk.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    bool testOversubscribe = false;
    uint32_t targetMb = 2048;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--oversubscribe" || a == "-o") {
            testOversubscribe = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') targetMb = std::atoi(argv[++i]);
        } else if (a == "--target-mb" && i + 1 < argc) {
            targetMb = std::atoi(argv[++i]);
        } else if (a == "--help" || a == "-h") {
            std::printf("Usage: %s [--oversubscribe] [--target-mb <MB>]\n", argv[0]);
            std::printf("Vulkan memory budget, heap spoofing and oversubscription verification tool\n");
            return 0;
        }
    }

    if (volkInitialize() != VK_SUCCESS) {
        std::fprintf(stderr, "Failed to initialize Volk\n");
        return 1;
    }

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "vulkan_vram_test";
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &appInfo;

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        std::fprintf(stderr, "vkCreateInstance failed\n");
        return 1;
    }
    volkLoadInstance(instance);

    uint32_t pdCount = 0;
    vkEnumeratePhysicalDevices(instance, &pdCount, nullptr);
    if (!pdCount) {
        std::fprintf(stderr, "No physical devices found\n");
        return 1;
    }
    std::vector<VkPhysicalDevice> pds(pdCount);
    vkEnumeratePhysicalDevices(instance, &pdCount, pds.data());

    VkPhysicalDevice phys = pds[0];
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);

    // Pick discrete GPU if multiple
    for (auto p : pds) {
        VkPhysicalDeviceProperties pr{};
        vkGetPhysicalDeviceProperties(p, &pr);
        if (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            phys = p;
            props = pr;
            break;
        }
    }

    std::printf("================================================================================\n");
    std::printf("  Vulkan Memory Hierarchy & Budget Verification\n");
    std::printf("================================================================================\n");
    std::printf("  Selected GPU: %s (Type: %d, Driver: 0x%x)\n", props.deviceName, props.deviceType, props.driverVersion);

    // Query memory properties and budget extension
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budgetProps{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 memProps2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    memProps2.pNext = &budgetProps;

    vkGetPhysicalDeviceMemoryProperties2(phys, &memProps2);
    const auto& mp = memProps2.memoryProperties;

    std::printf("  Memory Types: %u, Heaps: %u\n", mp.memoryTypeCount, mp.memoryHeapCount);
    uint32_t deviceLocalHeapIdx = 0;
    uint32_t deviceLocalTypeIdx = 0;
    bool foundDeviceLocal = false;

    for (uint32_t h = 0; h < mp.memoryHeapCount; ++h) {
        bool isDeviceLocal = (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
        uint64_t heapMb = mp.memoryHeaps[h].size / (1024 * 1024);
        uint64_t budgetMb = budgetProps.heapBudget[h] / (1024 * 1024);
        uint64_t usageMb = budgetProps.heapUsage[h] / (1024 * 1024);
        std::printf("  [Heap %u] %5llu MB | Flags: 0x%02x %s | Budget: %5llu MB | Usage: %5llu MB\n",
                    h, (unsigned long long)heapMb, mp.memoryHeaps[h].flags,
                    isDeviceLocal ? "(DEVICE_LOCAL)" : "(HOST_SYSTEM)",
                    (unsigned long long)budgetMb, (unsigned long long)usageMb);
        if (isDeviceLocal && !foundDeviceLocal) {
            deviceLocalHeapIdx = h;
            foundDeviceLocal = true;
        }
    }

    for (uint32_t t = 0; t < mp.memoryTypeCount; ++t) {
        if (mp.memoryTypes[t].heapIndex == deviceLocalHeapIdx &&
            (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            deviceLocalTypeIdx = t;
            break;
        }
    }

    // Create logical device to test memory allocation
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &queuePriority;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;

    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS) {
        std::fprintf(stderr, "vkCreateDevice failed\n");
        return 1;
    }
    volkLoadDevice(device);

    std::printf("--------------------------------------------------------------------------------\n");
    std::printf("  Allocating test buffers on deviceLocalTypeIndex=%u (Target: %u MB)\n", deviceLocalTypeIdx, targetMb);

    const VkDeviceSize chunkSize = 512ULL * 1024ULL * 1024ULL; // 512 MB
    uint32_t numChunks = (targetMb + 511) / 512;
    std::vector<VkDeviceMemory> allocs;
    uint64_t totalAllocatedMb = 0;

    for (uint32_t c = 0; c < numChunks; ++c) {
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = chunkSize;
        mai.memoryTypeIndex = deviceLocalTypeIdx;

        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkResult r = vkAllocateMemory(device, &mai, nullptr, &mem);
        if (r == VK_SUCCESS) {
            allocs.push_back(mem);
            totalAllocatedMb += (chunkSize / (1024 * 1024));
            std::printf("   Allocated chunk %u: 512 MB (Cumulative: %llu MB) - VK_SUCCESS\n",
                        c + 1, (unsigned long long)totalAllocatedMb);
        } else {
            std::printf("   Chunk %u allocation returned %d at %llu MB\n",
                        c + 1, (int)r, (unsigned long long)totalAllocatedMb);
            if (!testOversubscribe) break;
        }
    }

    std::printf("--------------------------------------------------------------------------------\n");
    std::printf("  Total allocated: %llu MB across %zu chunks\n",
                (unsigned long long)totalAllocatedMb, allocs.size());
    std::printf("  Freeing test memory...\n");

    for (auto m : allocs) {
        vkFreeMemory(device, m, nullptr);
    }
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    std::printf("  SUCCESS: Vulkan memory operations completed cleanly\n");
    std::printf("================================================================================\n");
    return 0;
}
