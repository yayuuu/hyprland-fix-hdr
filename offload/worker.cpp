#include "protocol.hpp"
#include "supervisor.hpp"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <compare>
#include <memory>
#include <sys/resource.h>
#include <chrono>
#include <poll.h>

extern "C" {
    extern const unsigned char hdrfix_shader_start[];
    extern const unsigned char hdrfix_shader_end[];
}
using namespace Offload;
static void vkcheck(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + ": " + std::to_string(r));
}
struct Image {
    VkDevice device{};
    VkImage image{};
    VkImageView view{};
    VkDeviceMemory memory{};
    FD fd;
    uint32_t offset = 0, stride = 0;
    bool deviceLocal = false;
    ~Image() {
        if (view) vkDestroyImageView(device, view, nullptr);
        if (image) vkDestroyImage(device, image, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
    }
};
struct SourceKey {
    uint64_t identity;
    uint32_t width, height, stride, offset, format;
    bool native = false;
    auto operator<=>(const SourceKey&) const = default;
};
struct Slot {
    std::unique_ptr<Image> image;
    std::unique_ptr<Image> localInput, localOutput;
    VkCommandBuffer command{};
    static constexpr unsigned MAX_BANDS = 8;
    std::array<VkCommandBuffer, MAX_BANDS> bandUpload{}, bandCompute{}, bandDownload{};
    std::array<VkSemaphore, MAX_BANDS> bandUploaded{}, bandComputed{};
    VkCommandBuffer upload{}, download{};
    VkDescriptorSet descriptors{};
    VkFence fence{};
    VkSemaphore complete{};
    VkSemaphore uploaded{}, computed{};
    std::vector<VkSemaphore> waits;
    bool initialized = false;
    std::optional<SourceKey> source;
    VkQueryPool queries{};
    uint64_t profileSerial = 0;
    unsigned profileBands = 0;
    bool sourceReady = false, reuseReady = false;
    double cpuSubmitMs = 0, slotWaitMs = 0;
};
class Worker {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    VkQueue copyQueue{}, downloadQueue{};
    uint32_t family = 0, externalFamily = VK_QUEUE_FAMILY_EXTERNAL;
    uint32_t copyFamily = 0;
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkCommandPool commands{};
    VkCommandPool copyCommands{};
    VkDescriptorSetLayout descriptorLayout{};
    VkDescriptorPool descriptorPool{};
    VkPipelineLayout pipelineLayout{};
    VkPipeline pipeline{};
    VkSampler sampler{};
    PFN_vkGetMemoryFdKHR getMemoryFd{};
    PFN_vkGetMemoryFdPropertiesKHR getMemoryFdProperties{};
    PFN_vkImportSemaphoreFdKHR importSemaphoreFd{};
    PFN_vkGetSemaphoreFdKHR getSemaphoreFd{};
    std::array<Slot, 3> slots;
    std::map<SourceKey, std::unique_ptr<Image>> sources;
    uint32_t width = 0, height = 0;
    bool profile = false;
    bool timestampsSupported = false;
    bool directInput = false;
    bool pipelined = false;
    bool preferLocalInput = false;
    float timestampPeriod = 1;
    unsigned profileRemaining = 72;

    void report(Slot& slot) {
        if (!slot.profileSerial) return;
        uint64_t t[6 + 2 * Slot::MAX_BANDS]{};
        const auto count = 6 + 2 * slot.profileBands;
        const auto result = vkGetQueryPoolResults(device, slot.queries, 0, count, count * sizeof(uint64_t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (result == VK_SUCCESS) {
            auto ms = [&](unsigned a, unsigned b) { return double(int64_t(t[b]) - int64_t(t[a])) * timestampPeriod / 1e6; };
            bool ordered = true;
            double minBandReady = 0;
            for (unsigned band = 0; band < slot.profileBands; ++band) {
                const auto ready = ms(6 + 2 * band, 7 + 2 * band);
                ordered &= ready >= 0 && t[5] >= t[7 + 2 * band];
                if (!band || ready < minBandReady) minBandReady = ready;
            }
            std::printf("[hdr-profile] serial=%llu upload_ms=%.6f compute_ms=%.6f download_ms=%.6f span_ms=%.6f gaps_ms=%.6f source_ready=%d reuse_ready=%d cpu_submit_ms=%.6f slot_wait_ms=%.6f bands=%u band_order_ok=%d min_band_ready_ms=%.6f\n",
                (unsigned long long)slot.profileSerial, ms(0,1), ms(2,3), ms(4,5), ms(0,5), ms(1,2)+ms(3,4), slot.sourceReady, slot.reuseReady, slot.cpuSubmitMs, slot.slotWaitMs, slot.profileBands, ordered, minBandReady);
        } else std::printf("[hdr-profile] query result=%d\n", result);
        slot.profileSerial = 0;
    }

    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags required, bool forbidLocal = false) {
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            const auto f = memoryProperties.memoryTypes[i].propertyFlags;
            if ((bits & (1u << i)) && (f & required) == required && (!forbidLocal || !(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))) return i;
        }
        throw std::runtime_error("No compatible memory type");
    }
    std::unique_ptr<Image> makeImage(const Packet* source, FD* input, bool local = false, bool fp16 = false) {
        auto result = std::make_unique<Image>(); result->device = device;
        const bool optimalSource = source && (source->fdFlags & NATIVE_INPUT);
        if (optimalSource && source->offset) throw std::runtime_error("Unsupported native input offset");
        const uint64_t linear = 0;
        VkImageDrmFormatModifierListCreateInfoEXT list{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};
        list.drmFormatModifierCount = 1; list.pDrmFormatModifiers = &linear;
        VkSubresourceLayout layout{};
        VkImageDrmFormatModifierExplicitCreateInfoEXT explicitLayout{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
        if (source) {
            // DMA-BUF import types describe compatible mappings, not the
            // producer's physical allocation. In particular, NVIDIA can map
            // its own device-local DMA-BUF through the generic type 0/heap 1.
            std::ifstream fdinfo("/proc/self/fdinfo/" + std::to_string(input->value));
            for (std::string line; std::getline(fdinfo, line); ) {
                if (line.starts_with("exp_name:") || line.starts_with("size:") || line.starts_with("name:"))
                    std::printf("[hdr-offload] input fd %s\n", line.c_str());
            }
            layout.offset = source->offset; layout.rowPitch = source->stride;
            explicitLayout.drmFormatModifier = 0; explicitLayout.drmFormatModifierPlaneCount = 1; explicitLayout.pPlaneLayouts = &layout;
        }
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        external.pNext = optimalSource ? nullptr : source ? static_cast<void*>(&explicitLayout) : static_cast<void*>(&list);
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.pNext = local ? nullptr : &external; info.imageType = VK_IMAGE_TYPE_2D;
        info.format = source || fp16 ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        info.extent = {source ? source->width : width, source ? source->height : height, 1}; info.mipLevels = 1; info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = (local || optimalSource) ? VK_IMAGE_TILING_OPTIMAL : VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        info.usage = source ? VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT :
            (fp16 ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        const uint32_t sharedFamilies[]{family, copyFamily};
        info.sharingMode = local && pipelined ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
        if (info.sharingMode == VK_SHARING_MODE_CONCURRENT) { info.queueFamilyIndexCount = 2; info.pQueueFamilyIndices = sharedFamilies; }
        vkcheck(vkCreateImage(device, &info, nullptr, &result->image), "create image");
        VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device, result->image, &requirements);
        VkImportMemoryFdInfoKHR imported{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
        VkExportMemoryAllocateInfo exported{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = result->image;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.pNext = &dedicated; allocation.allocationSize = requirements.size;
        if (source) {
            VkMemoryFdPropertiesKHR properties{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
            vkcheck(getMemoryFdProperties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, input->value, &properties), "input fd properties");
            const auto bits = properties.memoryTypeBits & requirements.memoryTypeBits;
            std::printf("[hdr-offload] input import compatible types=0x%x\n", bits);
            allocation.memoryTypeIndex = memoryType(bits, 0);
            if (preferLocalInput) {
                for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
                    if ((bits & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                        allocation.memoryTypeIndex = i;
                        break;
                    }
                }
            }
            imported.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; imported.fd = input->value;
            dedicated.pNext = &imported;
        } else if (local) {
            allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        } else {
            // Critical: HOST_VISIBLE without DEVICE_LOCAL. An NVIDIA VRAM
            // DMA-BUF makes i915 execbuf fail with ENOMEM on this machine.
            const auto required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            try { allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits, required | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, true); }
            catch (...) { allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits, required, true); }
            exported.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; dedicated.pNext = &exported;
        }
        if (optimalSource && !(memoryProperties.memoryTypes[allocation.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            throw std::runtime_error("Native input requires NVIDIA-local memory");
        if (source) {
            const auto bytes = lseek(input->value, 0, SEEK_END);
            if (bytes < 0 || uint64_t(bytes) < requirements.size) throw std::runtime_error("Input allocation smaller than image layout");
        }
        vkcheck(vkAllocateMemory(device, &allocation, nullptr, &result->memory), "allocate image memory");
        result->deviceLocal = memoryProperties.memoryTypes[allocation.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        if (source) std::printf("[hdr-offload] input memory: type=%u flags=0x%x heap=%u bytes=%llu stride=%u optimal=%d\n",
            allocation.memoryTypeIndex, memoryProperties.memoryTypes[allocation.memoryTypeIndex].propertyFlags,
            memoryProperties.memoryTypes[allocation.memoryTypeIndex].heapIndex,
            (unsigned long long)allocation.allocationSize, source->stride, optimalSource);
        if (source) input->release(); // Vulkan owns the imported descriptor now.
        vkcheck(vkBindImageMemory(device, result->image, result->memory, 0), "bind image memory");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = result->image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = info.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkcheck(vkCreateImageView(device, &view, nullptr, &result->view), "create image view");
        if (!source && !local) {
            VkMemoryGetFdInfoKHR fdInfo{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
            fdInfo.memory = result->memory; fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
            int fd = -1; vkcheck(getMemoryFd(device, &fdInfo, &fd), "export RAM image"); result->fd.reset(fd);
            VkImageSubresource sub{VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0};
            VkSubresourceLayout outLayout; vkGetImageSubresourceLayout(device, result->image, &sub, &outLayout);
            if (outLayout.offset > UINT32_MAX || outLayout.rowPitch > UINT32_MAX) throw std::runtime_error("Image layout overflow");
            result->offset = outLayout.offset; result->stride = outLayout.rowPitch;
        }
        return result;
    }
    VkSemaphore waitSemaphore(FD& fd) {
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore semaphore; vkcheck(vkCreateSemaphore(device, &info, nullptr, &semaphore), "create wait semaphore");
        VkImportSemaphoreFdInfoKHR imported{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
        imported.semaphore = semaphore; imported.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
        imported.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT; imported.fd = fd.value;
        auto r = importSemaphoreFd(device, &imported);
        if (r != VK_SUCCESS) { vkDestroySemaphore(device, semaphore, nullptr); vkcheck(r, "import wait fence"); }
        fd.release(); return semaphore;
    }
  public:
    Worker(uint32_t w, uint32_t h, uint32_t options = 0) : width(w), height(h) {
        if (!width || !height || uint64_t(width) * height > 16777216) throw std::runtime_error("Invalid output size");
        preferLocalInput = options & PREFER_LOCAL_INPUT;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName = "hyprland-hdr-offload-worker"; app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; create.pApplicationInfo = &app;
        vkcheck(vkCreateInstance(&create, nullptr, &instance), "create instance");
        uint32_t count = 0; vkcheck(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate devices");
        std::vector<VkPhysicalDevice> devices(count); vkcheck(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate devices");
        for (auto p : devices) { VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(p, &properties); if (properties.vendorID == 0x10de) { physical = p; std::printf("[hdr-offload] GPU: %s\n", properties.deviceName); break; } }
        if (!physical) throw std::runtime_error("NVIDIA unavailable; leave device binding unchanged");
        uint32_t n = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, nullptr);
        std::vector<VkQueueFamilyProperties> families(n); vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, families.data());
        family = n;
        for (uint32_t i = 0; i < n; ++i) if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) break; }
        if (family == n) throw std::runtime_error("No compute queue");
        copyFamily = family;
        for (uint32_t i = 0; i < n; ++i)
            if ((families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) && !(families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) { copyFamily = i; break; }
        VkPhysicalDeviceProperties gpuProperties{}; vkGetPhysicalDeviceProperties(physical, &gpuProperties);
        timestampPeriod = gpuProperties.limits.timestampPeriod;
        timestampsSupported = families[family].timestampValidBits == 64 && families[copyFamily].timestampValidBits == 64;
        profile = std::getenv("HDR_OFFLOAD_PROFILE") && std::strcmp(std::getenv("HDR_OFFLOAD_PROFILE"), "1") == 0 && timestampsSupported;
        std::printf("[hdr-offload] timestamp bits: compute=%u copy=%u period=%g profile=%d\n",
            families[family].timestampValidBits, families[copyFamily].timestampValidBits, timestampPeriod, profile);
        directInput = std::getenv("HDR_OFFLOAD_DIRECT_INPUT") && std::strcmp(std::getenv("HDR_OFFLOAD_DIRECT_INPUT"), "1") == 0;
        std::printf("[hdr-offload] direct input sampling=%d\n", directInput);
        pipelined = (options & PIPELINED) && !directInput && copyFamily != family && families[copyFamily].queueCount >= 2;
        std::printf("[hdr-offload] pipelined transfer=%d bands=%u\n", pipelined, pipelined ? Slot::MAX_BANDS : 1);
        const float priority[]{1, 1};
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = priority;
        std::vector<VkDeviceQueueCreateInfo> queueInfos{qi};
        if (copyFamily != family) { auto ci = qi; ci.queueFamilyIndex = copyFamily; ci.queueCount = pipelined ? 2 : 1; queueInfos.push_back(ci); }
        std::vector<const char*> extensions{VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
        uint32_t ec = 0; vkEnumerateDeviceExtensionProperties(physical, nullptr, &ec, nullptr);
        std::vector<VkExtensionProperties> available(ec); vkEnumerateDeviceExtensionProperties(physical, nullptr, &ec, available.data());
        for (const auto& e : available) if (std::strcmp(e.extensionName, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME) == 0) { extensions.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME); externalFamily = VK_QUEUE_FAMILY_FOREIGN_EXT; }
        VkPhysicalDeviceFeatures supported; vkGetPhysicalDeviceFeatures(physical, &supported);
        if (!supported.shaderStorageImageExtendedFormats) throw std::runtime_error("Packed 10-bit storage unsupported");
        VkPhysicalDeviceFeatures features{}; features.shaderStorageImageExtendedFormats = true;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dc.queueCreateInfoCount = queueInfos.size(); dc.pQueueCreateInfos = queueInfos.data();
        dc.enabledExtensionCount = extensions.size(); dc.ppEnabledExtensionNames = extensions.data(); dc.pEnabledFeatures = &features;
        vkcheck(vkCreateDevice(physical, &dc, nullptr, &device), "create device"); vkGetDeviceQueue(device, family, 0, &queue);
        vkGetDeviceQueue(device, copyFamily, 0, &copyQueue);
        vkGetDeviceQueue(device, copyFamily, pipelined ? 1 : 0, &downloadQueue);
        std::printf("[hdr-offload] queues: compute=%u copy=%u\n", family, copyFamily);
        vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
        getMemoryFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR"));
        getMemoryFdProperties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR"));
        importSemaphoreFd = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(vkGetDeviceProcAddr(device, "vkImportSemaphoreFdKHR"));
        getSemaphoreFd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR"));
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cp.queueFamilyIndex = family; cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkcheck(vkCreateCommandPool(device, &cp, nullptr, &commands), "create command pool");
        cp.queueFamilyIndex = copyFamily;
        vkcheck(vkCreateCommandPool(device, &cp, nullptr, &copyCommands), "create copy command pool");
        VkDescriptorSetLayoutBinding bindings[2]{{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount = 2; dl.pBindings = bindings;
        vkcheck(vkCreateDescriptorSetLayout(device, &dl, nullptr, &descriptorLayout), "create descriptor layout");
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 64};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount = 1; pl.pSetLayouts = &descriptorLayout; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &range;
        vkcheck(vkCreatePipelineLayout(device, &pl, nullptr, &pipelineLayout), "create pipeline layout");
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sm.codeSize = reinterpret_cast<uintptr_t>(hdrfix_shader_end) - reinterpret_cast<uintptr_t>(hdrfix_shader_start); sm.pCode = reinterpret_cast<const uint32_t*>(hdrfix_shader_start);
        VkShaderModule module; vkcheck(vkCreateShaderModule(device, &sm, nullptr, &module), "create shader module");
        VkComputePipelineCreateInfo pc{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; pc.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr}; pc.layout = pipelineLayout;
        auto pipelineResult = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pc, nullptr, &pipeline); vkDestroyShaderModule(device, module, nullptr); vkcheck(pipelineResult, "create compute pipeline");
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkcheck(vkCreateSampler(device, &si, nullptr, &sampler), "create sampler");
        VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3}};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets = 3; dp.poolSizeCount = 2; dp.pPoolSizes = sizes;
        vkcheck(vkCreateDescriptorPool(device, &dp, nullptr, &descriptorPool), "create descriptor pool");
        for (auto& slot : slots) {
            if (profile) {
                VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; q.queryType = VK_QUERY_TYPE_TIMESTAMP; q.queryCount = 6 + 2 * Slot::MAX_BANDS;
                vkcheck(vkCreateQueryPool(device, &q, nullptr, &slot.queries), "create timestamp pool");
            }
            slot.image = makeImage(nullptr, nullptr);
            slot.localInput = makeImage(nullptr, nullptr, true, true);
            slot.localOutput = makeImage(nullptr, nullptr, true, false);
            VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = commands; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
            vkcheck(vkAllocateCommandBuffers(device, &ca, &slot.command), "allocate command");
            ca.commandPool = copyCommands;
            vkcheck(vkAllocateCommandBuffers(device, &ca, &slot.upload), "allocate upload command");
            vkcheck(vkAllocateCommandBuffers(device, &ca, &slot.download), "allocate download command");
            VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; da.descriptorPool = descriptorPool; da.descriptorSetCount = 1; da.pSetLayouts = &descriptorLayout;
            vkcheck(vkAllocateDescriptorSets(device, &da, &slot.descriptors), "allocate descriptors");
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkcheck(vkCreateFence(device, &fi, nullptr, &slot.fence), "create fence");
            VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO}; exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
            VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; sc.pNext = &exportInfo;
            vkcheck(vkCreateSemaphore(device, &sc, nullptr, &slot.complete), "create completion semaphore");
            sc.pNext = nullptr;
            vkcheck(vkCreateSemaphore(device, &sc, nullptr, &slot.uploaded), "create upload semaphore");
            vkcheck(vkCreateSemaphore(device, &sc, nullptr, &slot.computed), "create compute semaphore");
            if (pipelined) for (unsigned i=0;i<Slot::MAX_BANDS;++i) {
                ca.commandPool=copyCommands;vkcheck(vkAllocateCommandBuffers(device,&ca,&slot.bandUpload[i]),"allocate band upload");
                vkcheck(vkAllocateCommandBuffers(device,&ca,&slot.bandDownload[i]),"allocate band download");
                ca.commandPool=commands;vkcheck(vkAllocateCommandBuffers(device,&ca,&slot.bandCompute[i]),"allocate band compute");
                vkcheck(vkCreateSemaphore(device,&sc,nullptr,&slot.bandUploaded[i]),"create band upload semaphore");
                vkcheck(vkCreateSemaphore(device,&sc,nullptr,&slot.bandComputed[i]),"create band compute semaphore");
            }
        }
        std::printf("[hdr-offload] ready: %ux%u, packed PQ output in system RAM\n", width, height);
    }
    uint32_t flags() const { return pipelined ? uint32_t(PIPELINED) : 0; }
    void inspect(int socket, const Packet& query, const std::vector<FD>& descriptors) {
        if (descriptors.size() != 1) throw std::runtime_error("invalid source inspection");
        VkMemoryFdPropertiesKHR properties{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        vkcheck(getMemoryFdProperties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, descriptors[0].value, &properties), "inspect input memory");
        Packet result{}; result.type = RESULT; result.serial = query.serial;
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
            if ((properties.memoryTypeBits & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                result.fdFlags |= SOURCE_DEVICE_LOCAL;
        if (!send(socket, result)) throw std::runtime_error("cannot send source inspection");
    }
    ~Worker() {
        if (device) {
            vkDeviceWaitIdle(device);
            for (auto& slot : slots) { for (auto sem : slot.bandUploaded) if (sem) vkDestroySemaphore(device, sem, nullptr); for (auto sem : slot.bandComputed) if (sem) vkDestroySemaphore(device, sem, nullptr); report(slot); if (slot.queries) vkDestroyQueryPool(device, slot.queries, nullptr); for (auto s : slot.waits) vkDestroySemaphore(device, s, nullptr); if (slot.complete) vkDestroySemaphore(device, slot.complete, nullptr); if (slot.uploaded) vkDestroySemaphore(device, slot.uploaded, nullptr); if (slot.computed) vkDestroySemaphore(device, slot.computed, nullptr); if (slot.fence) vkDestroyFence(device, slot.fence, nullptr); slot.image.reset(); slot.localInput.reset(); slot.localOutput.reset(); }
            sources.clear();
            if (sampler) vkDestroySampler(device, sampler, nullptr);
            if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            if (descriptorLayout) vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
            if (commands) vkDestroyCommandPool(device, commands, nullptr);
            if (copyCommands) vkDestroyCommandPool(device, copyCommands, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
    void convert(int sock, const Packet& job, std::vector<FD>& fds) {
        if (!job.width || !job.height || job.width > width || job.height > height || job.slot >= slots.size() || job.modifier != 0 ||
            (job.format != 0x48344258 && job.format != 0x48344241) || job.color[15] <= 0)
            throw std::runtime_error("Unsupported input or color transform");
        const size_t expected = 1 + bool(job.fdFlags & ACQUIRE) + bool(job.fdFlags & REUSE);
        if (fds.size() != expected) throw std::runtime_error("Invalid descriptor count");
        const auto cpuStart = std::chrono::steady_clock::now();
        auto& slot = slots[job.slot];
        vkcheck(vkWaitForFences(device, 1, &slot.fence, true, 100000000), "previous compute job");
        const auto waitEnd = std::chrono::steady_clock::now();
        report(slot);
        const bool timestamp = timestampsSupported && ((profile && profileRemaining > 0) || (job.fdFlags & PROFILE));
        if (timestamp) {
            if (profileRemaining) --profileRemaining;
            if (!slot.queries) {
                VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; q.queryType = VK_QUERY_TYPE_TIMESTAMP; q.queryCount = 6 + 2 * Slot::MAX_BANDS;
                vkcheck(vkCreateQueryPool(device, &q, nullptr, &slot.queries), "create timestamp pool");
            }
            slot.profileSerial = job.serial;
            slot.profileBands = pipelined ? std::min(Slot::MAX_BANDS, job.height) : 0;
            auto ready = [&](size_t i) { pollfd p{fds[i].value, POLLIN, 0}; return poll(&p, 1, 0) > 0; };
            slot.sourceReady = !(job.fdFlags & ACQUIRE) || ready(1);
            slot.reuseReady = !(job.fdFlags & REUSE) || ready(1 + bool(job.fdFlags & ACQUIRE));
            slot.slotWaitMs = std::chrono::duration<double, std::milli>(waitEnd-cpuStart).count();
        }
        for (auto s : slot.waits) vkDestroySemaphore(device, s, nullptr);
        slot.waits.clear();
        const SourceKey sourceKey{job.source, job.width, job.height, job.stride, job.offset, job.format, bool(job.fdFlags & NATIVE_INPUT)};
        if (!sources.contains(sourceKey)) {
            if (sources.size() >= 16) {
                auto unused = std::ranges::find_if(sources, [&](const auto& source) {
                    return std::ranges::none_of(slots, [&](const auto& active) {
                        return active.source == source.first && vkGetFenceStatus(device, active.fence) != VK_SUCCESS;
                    });
                });
                if (unused == sources.end()) throw std::runtime_error("No retired input buffer to evict");
                sources.erase(unused);
            }
            sources.emplace(sourceKey, makeImage(&job, &fds[0]));
        }
        auto& input = *sources.at(sourceKey);
        size_t index = 1;
        if (job.fdFlags & ACQUIRE) slot.waits.push_back(waitSemaphore(fds[index++]));
        if (job.fdFlags & REUSE) slot.waits.push_back(waitSemaphore(fds[index++]));
        VkDescriptorImageInfo images[]{{sampler, directInput ? input.view : slot.localInput->view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, slot.localOutput->view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[2]{};
        for (int i = 0; i < 2; ++i) { writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = slot.descriptors; writes[i].dstBinding = i; writes[i].descriptorCount = 1; writes[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[i].pImageInfo = &images[i]; }
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
        auto begin = [&](VkCommandBuffer command) {
            vkcheck(vkResetCommandBuffer(command, 0), "reset staged command");
            VkCommandBufferBeginInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; cb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkcheck(vkBeginCommandBuffer(command, &cb), "begin staged command");
        };
        auto barrier = [&](VkCommandBuffer command, VkImage image, VkImageLayout old, uint32_t from, uint32_t to, VkAccessFlags read, VkAccessFlags write) {
            // Semaphores provide visibility between the internal queues. Concurrent
            // images need only their initial layout transition, no ownership handoff.
            if (pipelined && (image == slot.localInput->image || image == slot.localOutput->image)) {
                if (old != VK_IMAGE_LAYOUT_UNDEFINED) return;
                from = to = VK_QUEUE_FAMILY_IGNORED;
            }
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.image = image; b.oldLayout = old; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = from; b.dstQueueFamilyIndex = to; b.srcAccessMask = read; b.dstAccessMask = write;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        };
        if (pipelined) {
            const auto old = slot.initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
            const unsigned bands = std::min(Slot::MAX_BANDS, job.height);
            vkcheck(vkResetFences(device,1,&slot.fence),"reset fence");
            for(unsigned band=0;band<bands;++band) {
                const unsigned y=job.height*band/bands,end=job.height*(band+1)/bands;
                VkImageCopy region{};region.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.dstSubresource=region.srcSubresource;
                region.srcOffset.y=region.dstOffset.y=y;region.extent={job.width,end-y,1};
                auto up=slot.bandUpload[band],comp=slot.bandCompute[band],down=slot.bandDownload[band];
                begin(up);
                if(band==0) {
                    if(timestamp) {vkCmdResetQueryPool(up,slot.queries,0,6 + 2 * bands);vkCmdWriteTimestamp(up,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,slot.queries,0);}
                    barrier(up,input.image,VK_IMAGE_LAYOUT_GENERAL,externalFamily,copyFamily,0,VK_ACCESS_TRANSFER_READ_BIT);
                    barrier(up,slot.localInput->image,old,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,0,VK_ACCESS_TRANSFER_WRITE_BIT);
                }
                vkCmdCopyImage(up,input.image,VK_IMAGE_LAYOUT_GENERAL,slot.localInput->image,VK_IMAGE_LAYOUT_GENERAL,1,&region);
                if(band+1==bands) {
                    barrier(up,input.image,VK_IMAGE_LAYOUT_GENERAL,copyFamily,externalFamily,VK_ACCESS_TRANSFER_READ_BIT,0);
                    if(timestamp)vkCmdWriteTimestamp(up,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,slot.queries,1);
                }
                vkcheck(vkEndCommandBuffer(up),"end band upload");
                begin(comp);
                if(band==0) {
                    if(timestamp)vkCmdWriteTimestamp(comp,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,slot.queries,2);
                    barrier(comp,slot.localOutput->image,old,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,0,VK_ACCESS_SHADER_WRITE_BIT);
                }
                vkCmdBindPipeline(comp,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
                vkCmdBindDescriptorSets(comp,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&slot.descriptors,0,nullptr);
                auto conversion=job.color;conversion[3]=float(job.width);conversion[7]=float(end);conversion[11]=float(y);
                vkCmdPushConstants(comp,pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(conversion),conversion.data());
                vkCmdDispatch(comp,(job.width+15)/16,(end-y+15)/16,1);
                if (timestamp) vkCmdWriteTimestamp(comp, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.queries, 6 + 2 * band);
                if(timestamp && band+1==bands)vkCmdWriteTimestamp(comp,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,slot.queries,3);
                vkcheck(vkEndCommandBuffer(comp),"end band compute");
                begin(down);
                if(band==0) {
                    if(timestamp)vkCmdWriteTimestamp(down,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,slot.queries,4);
                    barrier(down,slot.image->image,old,slot.initialized?externalFamily:VK_QUEUE_FAMILY_IGNORED,slot.initialized?copyFamily:VK_QUEUE_FAMILY_IGNORED,0,VK_ACCESS_TRANSFER_WRITE_BIT);
                }
                if (timestamp) vkCmdWriteTimestamp(down, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, slot.queries, 7 + 2 * band);
                vkCmdCopyImage(down,slot.localOutput->image,VK_IMAGE_LAYOUT_GENERAL,slot.image->image,VK_IMAGE_LAYOUT_GENERAL,1,&region);
                if(band+1==bands) {
                    barrier(down,slot.image->image,VK_IMAGE_LAYOUT_GENERAL,copyFamily,externalFamily,VK_ACCESS_TRANSFER_WRITE_BIT,0);
                    if(timestamp)vkCmdWriteTimestamp(down,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,slot.queries,5);
                }
                vkcheck(vkEndCommandBuffer(down),"end band download");
        }
            // Batch all bands for each queue in one call. Individual submit entries
            // keep per-band semaphore dependencies without a syscall for every band.
            std::array<VkSubmitInfo, Slot::MAX_BANDS> uploads{}, computes{}, downloads{};
            std::array<std::vector<VkSemaphore>, Slot::MAX_BANDS> uploadWaits, downloadWaits;
            const VkPipelineStageFlags stages[]{VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
            for (unsigned band = 0; band < bands; ++band) {
                if (band == 0 && (job.fdFlags & ACQUIRE)) uploadWaits[band].push_back(slot.waits[0]);
                downloadWaits[band].push_back(slot.bandComputed[band]);
                if (band == 0 && (job.fdFlags & REUSE)) downloadWaits[band].push_back(slot.waits[bool(job.fdFlags & ACQUIRE)]);
                auto init = [&](VkSubmitInfo& info, VkCommandBuffer& command, unsigned waitCount, const VkSemaphore* waits, const VkSemaphore* signal) {
                    info = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; info.waitSemaphoreCount = waitCount;
                    info.pWaitSemaphores = waits; info.pWaitDstStageMask = stages;
                    info.commandBufferCount = 1; info.pCommandBuffers = &command;
                    info.signalSemaphoreCount = signal ? 1 : 0; info.pSignalSemaphores = signal;
                };
                init(uploads[band], slot.bandUpload[band], uploadWaits[band].size(), uploadWaits[band].data(), &slot.bandUploaded[band]);
                init(computes[band], slot.bandCompute[band], 1, &slot.bandUploaded[band], &slot.bandComputed[band]);
                init(downloads[band], slot.bandDownload[band], downloadWaits[band].size(), downloadWaits[band].data(), band + 1 == bands ? &slot.complete : nullptr);
        }
            vkcheck(vkQueueSubmit(copyQueue, bands, uploads.data(), VK_NULL_HANDLE), "submit pipelined uploads");
            vkcheck(vkQueueSubmit(queue, bands, computes.data(), VK_NULL_HANDLE), "submit pipelined conversion");
            vkcheck(vkQueueSubmit(downloadQueue, bands, downloads.data(), slot.fence), "submit pipelined downloads");
        } else {
            VkImageCopy region{}; region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; region.dstSubresource = region.srcSubresource; region.extent = {job.width, job.height, 1};
            const auto old = slot.initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
            const auto firstFrom = slot.initialized ? family : VK_QUEUE_FAMILY_IGNORED;
            const auto firstTo = slot.initialized ? copyFamily : VK_QUEUE_FAMILY_IGNORED;
            begin(slot.upload);
            if (timestamp) { vkCmdResetQueryPool(slot.upload, slot.queries, 0, 6); vkCmdWriteTimestamp(slot.upload, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, slot.queries, 0); }
            barrier(slot.upload, input.image, VK_IMAGE_LAYOUT_GENERAL, externalFamily, copyFamily, 0, VK_ACCESS_TRANSFER_READ_BIT);
            barrier(slot.upload, slot.localInput->image, old, firstFrom, firstTo, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
            vkCmdCopyImage(slot.upload, input.image, VK_IMAGE_LAYOUT_GENERAL, slot.localInput->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
            barrier(slot.upload, input.image, VK_IMAGE_LAYOUT_GENERAL, copyFamily, externalFamily, VK_ACCESS_TRANSFER_READ_BIT, 0);
            barrier(slot.upload, slot.localInput->image, VK_IMAGE_LAYOUT_GENERAL, copyFamily, family, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
            if (timestamp) vkCmdWriteTimestamp(slot.upload, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.queries, 1);
            vkcheck(vkEndCommandBuffer(slot.upload), "end upload command");
            begin(slot.command);
            if (timestamp && directInput) {
                vkCmdResetQueryPool(slot.command, slot.queries, 0, 6);
                vkCmdWriteTimestamp(slot.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, slot.queries, 0);
                vkCmdWriteTimestamp(slot.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, slot.queries, 1);
        }
            if (timestamp) vkCmdWriteTimestamp(slot.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, slot.queries, 2);
            barrier(slot.command, directInput ? input.image : slot.localInput->image, VK_IMAGE_LAYOUT_GENERAL,
                directInput ? externalFamily : copyFamily, family, 0, VK_ACCESS_SHADER_READ_BIT);
            barrier(slot.command, slot.localOutput->image, old, slot.initialized ? copyFamily : VK_QUEUE_FAMILY_IGNORED, slot.initialized ? family : VK_QUEUE_FAMILY_IGNORED, 0, VK_ACCESS_SHADER_WRITE_BIT);
            vkCmdBindPipeline(slot.command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(slot.command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &slot.descriptors, 0, nullptr);
            auto conversion = job.color;
            conversion[3] = float(job.width); conversion[7] = float(job.height);
            vkCmdPushConstants(slot.command, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(conversion), conversion.data());
            vkCmdDispatch(slot.command, (job.width + 15) / 16, (job.height + 15) / 16, 1);
            barrier(slot.command, directInput ? input.image : slot.localInput->image, VK_IMAGE_LAYOUT_GENERAL,
                family, directInput ? externalFamily : copyFamily, VK_ACCESS_SHADER_READ_BIT, 0);
            barrier(slot.command, slot.localOutput->image, VK_IMAGE_LAYOUT_GENERAL, family, copyFamily, VK_ACCESS_SHADER_WRITE_BIT, 0);
            if (timestamp) vkCmdWriteTimestamp(slot.command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.queries, 3);
            vkcheck(vkEndCommandBuffer(slot.command), "end compute command");
            begin(slot.download);
            if (timestamp) vkCmdWriteTimestamp(slot.download, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, slot.queries, 4);
            barrier(slot.download, slot.localOutput->image, VK_IMAGE_LAYOUT_GENERAL, family, copyFamily, 0, VK_ACCESS_TRANSFER_READ_BIT);
            barrier(slot.download, slot.image->image, old, slot.initialized ? externalFamily : VK_QUEUE_FAMILY_IGNORED, slot.initialized ? copyFamily : VK_QUEUE_FAMILY_IGNORED, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
            vkCmdCopyImage(slot.download, slot.localOutput->image, VK_IMAGE_LAYOUT_GENERAL, slot.image->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
            barrier(slot.download, slot.localOutput->image, VK_IMAGE_LAYOUT_GENERAL, copyFamily, family, VK_ACCESS_TRANSFER_READ_BIT, 0);
            barrier(slot.download, slot.image->image, VK_IMAGE_LAYOUT_GENERAL, copyFamily, externalFamily, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
            if (timestamp) vkCmdWriteTimestamp(slot.download, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.queries, 5);
            vkcheck(vkEndCommandBuffer(slot.download), "end download command");
            auto submit = [&](VkQueue target, VkCommandBuffer command, const std::vector<VkSemaphore>& waits, VkSemaphore signal, VkFence fence) {
                std::vector<VkPipelineStageFlags> stages(waits.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO}; info.waitSemaphoreCount = waits.size(); info.pWaitSemaphores = waits.data(); info.pWaitDstStageMask = stages.data();
                info.commandBufferCount = 1; info.pCommandBuffers = &command; info.signalSemaphoreCount = 1; info.pSignalSemaphores = &signal;
                vkcheck(vkQueueSubmit(target, 1, &info, fence), "submit staged conversion");
            };
            std::vector<VkSemaphore> uploadWaits, computeWaits, downloadWaits;
            if (!directInput) computeWaits.push_back(slot.uploaded);
            downloadWaits.push_back(slot.computed);
            size_t waitIndex = 0;
            if (job.fdFlags & ACQUIRE) { if (directInput) computeWaits.push_back(slot.waits[waitIndex++]); else uploadWaits.push_back(slot.waits[waitIndex++]); }
            if (job.fdFlags & REUSE) downloadWaits.push_back(slot.waits[waitIndex++]);
            vkcheck(vkResetFences(device, 1, &slot.fence), "reset fence");
            if (!directInput) submit(copyQueue, slot.upload, uploadWaits, slot.uploaded, VK_NULL_HANDLE);
            submit(queue, slot.command, computeWaits, slot.computed, VK_NULL_HANDLE);
            submit(copyQueue, slot.download, downloadWaits, slot.complete, slot.fence);
        }
        slot.initialized = true; slot.source = sourceKey;
        VkSemaphoreGetFdInfoKHR exportInfo{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR}; exportInfo.semaphore = slot.complete; exportInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        int completion = -1; vkcheck(getSemaphoreFd(device, &exportInfo, &completion), "export conversion completion"); FD completionFD(completion);
        Packet result{}; result.type = RESULT; result.width = job.width; result.height = job.height; result.slot = job.slot; result.serial = job.serial;
        if (input.deviceLocal) result.fdFlags |= SOURCE_DEVICE_LOCAL;
        result.format = 0x30334258; result.stride = slot.image->stride; result.offset = slot.image->offset;
        std::vector<int> descriptors{slot.image->fd.value};
        if (completion >= 0) { result.fdFlags |= COMPLETE; descriptors.push_back(completion); }
        if (timestamp) slot.cpuSubmitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-waitEnd).count();
        if (!send(sock, result, descriptors)) throw std::runtime_error("Cannot send converted buffer");
    }
};

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const rlimit noCore{0, 0}; setrlimit(RLIMIT_CORE, &noCore);
    if (!std::getenv("__NV_PRIME_RENDER_OFFLOAD") || std::strcmp(std::getenv("__NV_PRIME_RENDER_OFFLOAD"), "1") != 0) {
        std::fprintf(stderr, "[hdr-offload] render offload flag required; no GPU opened\n"); return 2;
    }
    if (argc == 4 && std::strcmp(argv[1], "--supervise") == 0 && std::strcmp(argv[2], "3") == 0 && std::strcmp(argv[3], "--embedded") == 0)
        return superviseWorker(argv[2], argv[3]);
    const bool embedded = argc == 3 && std::strcmp(argv[2], "--embedded") == 0;
    const int sock = (argc == 2 || embedded) ? std::atoi(argv[1]) : -1;
    if (embedded) close(4); // The executable is already mapped; retain only IPC.

    if (sock < 0) return 2;
    try {
        Packet hello; std::vector<FD> fds;
        if (!receive(sock, hello, fds) || hello.type != HELLO || !fds.empty()) return 2;
        Worker worker(hello.width, hello.height, hello.fdFlags);
        Packet ready{}; ready.type = READY; ready.pid = getpid(); ready.width = hello.width; ready.height = hello.height; ready.fdFlags = worker.flags();
        if (!send(sock, ready)) return 2;
        for (;;) {
            Packet job; std::vector<FD> descriptors;
            if (!receive(sock, job, descriptors)) break;
            if (job.type == INSPECT) { worker.inspect(sock, job, descriptors); continue; }
            if (job.type != CONVERT) throw std::runtime_error("Unexpected request");
            worker.convert(sock, job, descriptors);
        }
        std::printf("[hdr-offload] stopped; Vulkan resources released\n");
        return 0;
    } catch (const std::exception& e) {
        Packet error{}; error.type = ERROR; std::snprintf(error.error, sizeof(error.error), "%s", e.what()); send(sock, error);
        std::fprintf(stderr, "[hdr-offload] %s\n", e.what()); return 1;
    }
}
