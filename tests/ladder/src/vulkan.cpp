// vulkan: instance, physical device, device and queue through vulkan-1.dll (winevulkan), then
// transfer-queue work read back through host-visible memory: vkCmdClearColorImage,
// vkCmdCopyBufferToImage into a sub-rectangle with a row length, vkCmdCopyImageToBuffer,
// vkCmdFillBuffer + vkCmdCopyBuffer, and 50 submissions reusing one fence. No shaders, so it
// needs no SPIR-V; it checks the Vulkan path itself (winevulkan -> MoltenVK/KosmicKrisp).
//
// vulkan-1.dll is loaded at run time. The declarations below are a minimal subset of
// vulkan_core.h; build.sh checks their layout against the real header with -DLADDER_VK_CHECK.
#include "gfx.h"
#include <stddef.h>

namespace lvk {
#define VKAPI_PTR __stdcall
typedef uint32_t VkFlags, VkBool32;
typedef uint64_t VkDeviceSize;
typedef int32_t VkStructureType, VkResult, VkFormat, VkImageType, VkImageTiling, VkSharingMode, VkImageLayout, VkPhysicalDeviceType,
    VkSampleCountFlagBits, VkCommandBufferLevel;
typedef VkFlags VkInstanceCreateFlags, VkDeviceCreateFlags, VkDeviceQueueCreateFlags, VkQueueFlags, VkImageCreateFlags,
    VkImageUsageFlags, VkBufferCreateFlags, VkBufferUsageFlags, VkMemoryPropertyFlags, VkMemoryHeapFlags, VkMemoryMapFlags,
    VkCommandPoolCreateFlags, VkCommandBufferUsageFlags, VkFenceCreateFlags, VkPipelineStageFlags, VkAccessFlags, VkImageAspectFlags,
    VkDependencyFlags;
typedef struct VkInstance_T *VkInstance; typedef struct VkPhysicalDevice_T *VkPhysicalDevice; typedef struct VkDevice_T *VkDevice;
typedef struct VkQueue_T *VkQueue; typedef struct VkCommandBuffer_T *VkCommandBuffer;
typedef uint64_t VkImage, VkBuffer, VkDeviceMemory, VkCommandPool, VkFence, VkSemaphore;
struct VkAllocationCallbacks; struct VkPhysicalDeviceFeatures; struct VkCommandBufferInheritanceInfo; struct VkMemoryBarrier;
struct VkBufferMemoryBarrier;
typedef void(VKAPI_PTR *PFN_vkVoidFunction)(void);

typedef struct VkApplicationInfo { VkStructureType sType; const void *pNext; const char *pApplicationName; uint32_t applicationVersion; const char *pEngineName; uint32_t engineVersion; uint32_t apiVersion; } VkApplicationInfo;
typedef struct VkInstanceCreateInfo { VkStructureType sType; const void *pNext; VkInstanceCreateFlags flags; const VkApplicationInfo *pApplicationInfo; uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames; uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames; } VkInstanceCreateInfo;
typedef struct VkPhysicalDeviceLimits { uint64_t opaque[sizeof(void *) == 8 ? 63 : 62]; } VkPhysicalDeviceLimits;   // 504/496 bytes, unused
typedef struct VkPhysicalDeviceSparseProperties { VkBool32 b[5]; } VkPhysicalDeviceSparseProperties;
typedef struct VkPhysicalDeviceProperties { uint32_t apiVersion; uint32_t driverVersion; uint32_t vendorID; uint32_t deviceID; VkPhysicalDeviceType deviceType; char deviceName[256]; uint8_t pipelineCacheUUID[16]; VkPhysicalDeviceLimits limits; VkPhysicalDeviceSparseProperties sparseProperties; } VkPhysicalDeviceProperties;
typedef struct VkExtent3D { uint32_t width, height, depth; } VkExtent3D;
typedef struct VkOffset3D { int32_t x, y, z; } VkOffset3D;
typedef struct VkQueueFamilyProperties { VkQueueFlags queueFlags; uint32_t queueCount; uint32_t timestampValidBits; VkExtent3D minImageTransferGranularity; } VkQueueFamilyProperties;
typedef struct VkMemoryType { VkMemoryPropertyFlags propertyFlags; uint32_t heapIndex; } VkMemoryType;
typedef struct VkMemoryHeap { VkDeviceSize size; VkMemoryHeapFlags flags; } VkMemoryHeap;
typedef struct VkPhysicalDeviceMemoryProperties { uint32_t memoryTypeCount; VkMemoryType memoryTypes[32]; uint32_t memoryHeapCount; VkMemoryHeap memoryHeaps[16]; } VkPhysicalDeviceMemoryProperties;
typedef struct VkDeviceQueueCreateInfo { VkStructureType sType; const void *pNext; VkDeviceQueueCreateFlags flags; uint32_t queueFamilyIndex; uint32_t queueCount; const float *pQueuePriorities; } VkDeviceQueueCreateInfo;
typedef struct VkDeviceCreateInfo { VkStructureType sType; const void *pNext; VkDeviceCreateFlags flags; uint32_t queueCreateInfoCount; const VkDeviceQueueCreateInfo *pQueueCreateInfos; uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames; uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames; const VkPhysicalDeviceFeatures *pEnabledFeatures; } VkDeviceCreateInfo;
typedef struct VkImageCreateInfo { VkStructureType sType; const void *pNext; VkImageCreateFlags flags; VkImageType imageType; VkFormat format; VkExtent3D extent; uint32_t mipLevels; uint32_t arrayLayers; VkSampleCountFlagBits samples; VkImageTiling tiling; VkImageUsageFlags usage; VkSharingMode sharingMode; uint32_t queueFamilyIndexCount; const uint32_t *pQueueFamilyIndices; VkImageLayout initialLayout; } VkImageCreateInfo;
typedef struct VkBufferCreateInfo { VkStructureType sType; const void *pNext; VkBufferCreateFlags flags; VkDeviceSize size; VkBufferUsageFlags usage; VkSharingMode sharingMode; uint32_t queueFamilyIndexCount; const uint32_t *pQueueFamilyIndices; } VkBufferCreateInfo;
typedef struct VkMemoryRequirements { VkDeviceSize size; VkDeviceSize alignment; uint32_t memoryTypeBits; } VkMemoryRequirements;
typedef struct VkMemoryAllocateInfo { VkStructureType sType; const void *pNext; VkDeviceSize allocationSize; uint32_t memoryTypeIndex; } VkMemoryAllocateInfo;
typedef struct VkCommandPoolCreateInfo { VkStructureType sType; const void *pNext; VkCommandPoolCreateFlags flags; uint32_t queueFamilyIndex; } VkCommandPoolCreateInfo;
typedef struct VkCommandBufferAllocateInfo { VkStructureType sType; const void *pNext; VkCommandPool commandPool; VkCommandBufferLevel level; uint32_t commandBufferCount; } VkCommandBufferAllocateInfo;
typedef struct VkCommandBufferBeginInfo { VkStructureType sType; const void *pNext; VkCommandBufferUsageFlags flags; const VkCommandBufferInheritanceInfo *pInheritanceInfo; } VkCommandBufferBeginInfo;
typedef struct VkImageSubresourceRange { VkImageAspectFlags aspectMask; uint32_t baseMipLevel; uint32_t levelCount; uint32_t baseArrayLayer; uint32_t layerCount; } VkImageSubresourceRange;
typedef struct VkImageSubresourceLayers { VkImageAspectFlags aspectMask; uint32_t mipLevel; uint32_t baseArrayLayer; uint32_t layerCount; } VkImageSubresourceLayers;
typedef struct VkImageMemoryBarrier { VkStructureType sType; const void *pNext; VkAccessFlags srcAccessMask; VkAccessFlags dstAccessMask; VkImageLayout oldLayout; VkImageLayout newLayout; uint32_t srcQueueFamilyIndex; uint32_t dstQueueFamilyIndex; VkImage image; VkImageSubresourceRange subresourceRange; } VkImageMemoryBarrier;
typedef union VkClearColorValue { float float32[4]; int32_t int32[4]; uint32_t uint32[4]; } VkClearColorValue;
typedef struct VkBufferImageCopy { VkDeviceSize bufferOffset; uint32_t bufferRowLength; uint32_t bufferImageHeight; VkImageSubresourceLayers imageSubresource; VkOffset3D imageOffset; VkExtent3D imageExtent; } VkBufferImageCopy;
typedef struct VkBufferCopy { VkDeviceSize srcOffset; VkDeviceSize dstOffset; VkDeviceSize size; } VkBufferCopy;
typedef struct VkSubmitInfo { VkStructureType sType; const void *pNext; uint32_t waitSemaphoreCount; const VkSemaphore *pWaitSemaphores; const VkPipelineStageFlags *pWaitDstStageMask; uint32_t commandBufferCount; const VkCommandBuffer *pCommandBuffers; uint32_t signalSemaphoreCount; const VkSemaphore *pSignalSemaphores; } VkSubmitInfo;
typedef struct VkFenceCreateInfo { VkStructureType sType; const void *pNext; VkFenceCreateFlags flags; } VkFenceCreateInfo;

enum : int32_t {
    VK_STRUCTURE_TYPE_APPLICATION_INFO = 0, VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1, VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2,
    VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3, VK_STRUCTURE_TYPE_SUBMIT_INFO = 4, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5,
    VK_STRUCTURE_TYPE_FENCE_CREATE_INFO = 8, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO = 12, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO = 14,
    VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO = 39, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 40,
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO = 42, VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER = 45,
    VK_FORMAT_R8G8B8A8_UNORM = 37, VK_IMAGE_TYPE_2D = 1, VK_IMAGE_TILING_OPTIMAL = 0, VK_SHARING_MODE_EXCLUSIVE = 0,
    VK_IMAGE_LAYOUT_UNDEFINED = 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL = 6, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL = 7,
    VK_SAMPLE_COUNT_1_BIT = 1, VK_COMMAND_BUFFER_LEVEL_PRIMARY = 0,
};
static const uint32_t VK_QUEUE_GRAPHICS_BIT = 1, VK_IMAGE_USAGE_TRANSFER_SRC_BIT = 1,
    VK_IMAGE_USAGE_TRANSFER_DST_BIT = 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT = 1, VK_BUFFER_USAGE_TRANSFER_DST_BIT = 2,
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT = 1, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 2, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 4,
    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT = 2, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT = 1,
    VK_IMAGE_ASPECT_COLOR_BIT = 1, VK_ACCESS_TRANSFER_READ_BIT = 0x800, VK_ACCESS_TRANSFER_WRITE_BIT = 0x1000,
    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT = 1, VK_PIPELINE_STAGE_TRANSFER_BIT = 0x1000, VK_QUEUE_FAMILY_IGNORED = ~0u;
}   // namespace lvk

#ifdef LADDER_VK_CHECK
#include <vulkan/vulkan_core.h>
#define VKC_SZ(T) static_assert(sizeof(lvk::T) == sizeof(::T) && alignof(lvk::T) == alignof(::T), #T);
VKC_SZ(VkApplicationInfo) VKC_SZ(VkInstanceCreateInfo) VKC_SZ(VkPhysicalDeviceLimits) VKC_SZ(VkPhysicalDeviceSparseProperties)
VKC_SZ(VkPhysicalDeviceProperties) VKC_SZ(VkQueueFamilyProperties) VKC_SZ(VkPhysicalDeviceMemoryProperties)
VKC_SZ(VkDeviceQueueCreateInfo) VKC_SZ(VkDeviceCreateInfo) VKC_SZ(VkImageCreateInfo) VKC_SZ(VkBufferCreateInfo)
VKC_SZ(VkMemoryRequirements) VKC_SZ(VkMemoryAllocateInfo) VKC_SZ(VkCommandPoolCreateInfo) VKC_SZ(VkCommandBufferAllocateInfo)
VKC_SZ(VkCommandBufferBeginInfo) VKC_SZ(VkImageSubresourceRange) VKC_SZ(VkImageSubresourceLayers) VKC_SZ(VkImageMemoryBarrier)
VKC_SZ(VkClearColorValue) VKC_SZ(VkBufferImageCopy) VKC_SZ(VkBufferCopy) VKC_SZ(VkSubmitInfo) VKC_SZ(VkFenceCreateInfo)
static_assert(offsetof(lvk::VkPhysicalDeviceProperties, sparseProperties) == offsetof(::VkPhysicalDeviceProperties, sparseProperties), "sparse");
static_assert(offsetof(lvk::VkImageCreateInfo, initialLayout) == offsetof(::VkImageCreateInfo, initialLayout), "layout");
static_assert(offsetof(lvk::VkBufferImageCopy, imageExtent) == offsetof(::VkBufferImageCopy, imageExtent), "extent");
static_assert(lvk::VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER == ::VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, "sType");
static_assert(lvk::VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO == ::VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, "sType");
static_assert(lvk::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL == ::VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, "layout");
static_assert(lvk::VK_ACCESS_TRANSFER_WRITE_BIT == ::VK_ACCESS_TRANSFER_WRITE_BIT, "access");
static_assert(lvk::VK_PIPELINE_STAGE_TRANSFER_BIT == ::VK_PIPELINE_STAGE_TRANSFER_BIT, "stage");
int main() { return 0; }
#else

using namespace lvk;

#define VK_FUNCS(X)                                                                                                         \
    X(vkCreateInstance, VkResult, (const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *))              \
    X(vkDestroyInstance, void, (VkInstance, const VkAllocationCallbacks *))                                                \
    X(vkEnumeratePhysicalDevices, VkResult, (VkInstance, uint32_t *, VkPhysicalDevice *))                                  \
    X(vkGetPhysicalDeviceProperties, void, (VkPhysicalDevice, VkPhysicalDeviceProperties *))                                \
    X(vkGetPhysicalDeviceQueueFamilyProperties, void, (VkPhysicalDevice, uint32_t *, VkQueueFamilyProperties *))           \
    X(vkGetPhysicalDeviceMemoryProperties, void, (VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *))                    \
    X(vkCreateDevice, VkResult, (VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *)) \
    X(vkDestroyDevice, void, (VkDevice, const VkAllocationCallbacks *))                                                    \
    X(vkGetDeviceQueue, void, (VkDevice, uint32_t, uint32_t, VkQueue *))                                                    \
    X(vkCreateImage, VkResult, (VkDevice, const VkImageCreateInfo *, const VkAllocationCallbacks *, VkImage *))             \
    X(vkDestroyImage, void, (VkDevice, VkImage, const VkAllocationCallbacks *))                                            \
    X(vkGetImageMemoryRequirements, void, (VkDevice, VkImage, VkMemoryRequirements *))                                      \
    X(vkBindImageMemory, VkResult, (VkDevice, VkImage, VkDeviceMemory, VkDeviceSize))                                      \
    X(vkCreateBuffer, VkResult, (VkDevice, const VkBufferCreateInfo *, const VkAllocationCallbacks *, VkBuffer *))          \
    X(vkDestroyBuffer, void, (VkDevice, VkBuffer, const VkAllocationCallbacks *))                                          \
    X(vkGetBufferMemoryRequirements, void, (VkDevice, VkBuffer, VkMemoryRequirements *))                                    \
    X(vkBindBufferMemory, VkResult, (VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize))                                    \
    X(vkAllocateMemory, VkResult, (VkDevice, const VkMemoryAllocateInfo *, const VkAllocationCallbacks *, VkDeviceMemory *)) \
    X(vkFreeMemory, void, (VkDevice, VkDeviceMemory, const VkAllocationCallbacks *))                                       \
    X(vkMapMemory, VkResult, (VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void **))            \
    X(vkUnmapMemory, void, (VkDevice, VkDeviceMemory))                                                                     \
    X(vkCreateCommandPool, VkResult, (VkDevice, const VkCommandPoolCreateInfo *, const VkAllocationCallbacks *, VkCommandPool *)) \
    X(vkDestroyCommandPool, void, (VkDevice, VkCommandPool, const VkAllocationCallbacks *))                                \
    X(vkAllocateCommandBuffers, VkResult, (VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *))              \
    X(vkBeginCommandBuffer, VkResult, (VkCommandBuffer, const VkCommandBufferBeginInfo *))                                 \
    X(vkEndCommandBuffer, VkResult, (VkCommandBuffer))                                                                     \
    X(vkCmdPipelineBarrier, void, (VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t, \
                                   const VkMemoryBarrier *, uint32_t, const VkBufferMemoryBarrier *, uint32_t, const VkImageMemoryBarrier *)) \
    X(vkCmdClearColorImage, void, (VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue *, uint32_t, const VkImageSubresourceRange *)) \
    X(vkCmdCopyImageToBuffer, void, (VkCommandBuffer, VkImage, VkImageLayout, VkBuffer, uint32_t, const VkBufferImageCopy *)) \
    X(vkCmdCopyBufferToImage, void, (VkCommandBuffer, VkBuffer, VkImage, VkImageLayout, uint32_t, const VkBufferImageCopy *)) \
    X(vkCmdFillBuffer, void, (VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, uint32_t))                            \
    X(vkCmdCopyBuffer, void, (VkCommandBuffer, VkBuffer, VkBuffer, uint32_t, const VkBufferCopy *))                        \
    X(vkCreateFence, VkResult, (VkDevice, const VkFenceCreateInfo *, const VkAllocationCallbacks *, VkFence *))             \
    X(vkDestroyFence, void, (VkDevice, VkFence, const VkAllocationCallbacks *))                                            \
    X(vkResetFences, VkResult, (VkDevice, uint32_t, const VkFence *))                                                      \
    X(vkWaitForFences, VkResult, (VkDevice, uint32_t, const VkFence *, VkBool32, uint64_t))                                \
    X(vkQueueSubmit, VkResult, (VkQueue, uint32_t, const VkSubmitInfo *, VkFence))                                         \
    X(vkDeviceWaitIdle, VkResult, (VkDevice))
#define DECL(name, ret, args) typedef ret(VKAPI_PTR *PFN_##name) args; static PFN_##name name;
VK_FUNCS(DECL)
typedef PFN_vkVoidFunction(VKAPI_PTR *PFN_vkGetInstanceProcAddr)(VkInstance, const char *);

static VkDevice dev;
static VkQueue queue;
static VkCommandBuffer cmd;
static VkFence fence;
static VkPhysicalDeviceMemoryProperties memprops;
static char g_err[160];

#define VK(x) do { VkResult r_ = (x); if (r_ < 0) { snprintf(g_err, sizeof g_err, "%s: VkResult %d", #x, (int)r_); return false; } } while (0)

static int mem_type(uint32_t bits, uint32_t want) {
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

struct HostBuf { VkBuffer buf; VkDeviceMemory mem; uint8_t *ptr; };
static bool host_buffer(VkDeviceSize size, HostBuf *b) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, size,
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};
    VK(vkCreateBuffer(dev, &bi, nullptr, &b->buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, b->buf, &req);
    int t = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t < 0) { snprintf(g_err, sizeof g_err, "no host-visible coherent memory type"); return false; }
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, (uint32_t)t};
    VK(vkAllocateMemory(dev, &ai, nullptr, &b->mem));
    VK(vkBindBufferMemory(dev, b->buf, b->mem, 0));
    VK(vkMapMemory(dev, b->mem, 0, size, 0, (void **)&b->ptr));
    return true;
}
static void free_host_buffer(HostBuf *b) {
    vkUnmapMemory(dev, b->mem); vkDestroyBuffer(dev, b->buf, nullptr); vkFreeMemory(dev, b->mem, nullptr);
}
static bool begin() {
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    VK(vkBeginCommandBuffer(cmd, &bi));
    return true;
}
static bool submit_wait() {
    VK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cmd, 0, nullptr};
    VK(vkResetFences(dev, 1, &fence));
    VK(vkQueueSubmit(queue, 1, &si, fence));
    VK(vkWaitForFences(dev, 1, &fence, 1, 10ull * 1000 * 1000 * 1000));
    return true;
}
static void image_barrier(VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, src, dst, from, to, VK_QUEUE_FAMILY_IGNORED,
                              VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
}

static VkImage img;
static VkDeviceMemory img_mem;
static bool image_tests() {
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nullptr, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {W, H, 1}, 1, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr,
                            VK_IMAGE_LAYOUT_UNDEFINED};
    VK(vkCreateImage(dev, &ii, nullptr, &img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, img, &req);
    int t = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t < 0) t = mem_type(req.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, (uint32_t)t};
    VK(vkAllocateMemory(dev, &ai, nullptr, &img_mem));
    VK(vkBindImageMemory(dev, img, img_mem, 0));
    HostBuf rb = {}, up = {};
    if (!host_buffer(W * H * 4, &rb) || !host_buffer(W * H * 4, &up)) return false;
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkBufferImageCopy whole = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};

    // clear -> copy to buffer
    if (!begin()) return false;
    image_barrier(img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue cc;
    memcpy(cc.float32, k_clear_color, sizeof cc.float32);
    vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &range);
    image_barrier(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &whole);
    if (!submit_wait()) return false;
    check_pixels("clear", rb.ptr, W * 4, [](int, int) { return k_clear; }, false, "vkCmdClearColorImage -> vkCmdCopyImageToBuffer");

    // upload the pattern into the 32x32 centre only (buffer offset + row length), read it all back
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { RGBA c = pattern(x, y); memcpy(up.ptr + (y * W + x) * 4, &c, 4); }
    VkBufferImageCopy centre = {(VkDeviceSize)(16 * W + 16) * 4, W, H, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {16, 16, 0}, {32, 32, 1}};
    if (!begin()) return false;
    image_barrier(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(cmd, up.buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &centre);
    image_barrier(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &whole);
    if (!submit_wait()) return false;
    check_pixels("copy_region", rb.ptr, W * 4, [](int x, int y) { return in_quad(x, y) ? pattern(x, y) : k_clear; }, false,
                 "buffer -> 32x32 image region with row length, then image -> buffer");
    free_host_buffer(&rb); free_host_buffer(&up);
    return true;
}

static bool buffer_tests() {
    HostBuf a = {}, b = {};
    if (!host_buffer(4096, &a) || !host_buffer(4096, &b)) return false;
    memset(b.ptr, 0x11, 4096);
    if (!begin()) return false;
    vkCmdFillBuffer(cmd, a.buf, 0, 4096, 0);
    vkCmdFillBuffer(cmd, a.buf, 256, 1024, 0xDEADBEEF);
    VkBufferCopy region = {0, 1024, 2048};   // a[0..2048) -> b[1024..3072)
    // a's fills must land before the copy reads them
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 0, nullptr);
    vkCmdCopyBuffer(cmd, a.buf, b.buf, 1, &region);
    if (!submit_wait()) return false;
    int bad = 0;
    const uint32_t *w = (const uint32_t *)b.ptr;
    for (int i = 0; i < 1024; i++) {
        uint32_t want_;
        if (i < 256 || i >= 768) want_ = 0x11111111;               // outside the copy
        else if (i >= 256 + 64 && i < 256 + 64 + 256) want_ = 0xDEADBEEF;   // a[256..1280) landed at b[1280..2304)
        else want_ = 0;
        bad += w[i] != want_;
    }
    check(bad == 0, "fill_copy_buffer", "vkCmdFillBuffer x2 + vkCmdCopyBuffer at offsets: %d of 1024 words wrong", bad);

    // 50 submissions reusing one fence and one command buffer
    int wrong = 0;
    for (uint32_t i = 1; i <= 50; i++) {
        if (!begin()) return false;
        vkCmdFillBuffer(cmd, a.buf, 0, 64, i * 0x01010101u);
        if (!submit_wait()) return false;
        wrong += ((const uint32_t *)a.ptr)[15] != i * 0x01010101u;
    }
    check(wrong == 0, "fence_reuse", "50 fill + submit + wait cycles on one fence: %d saw stale data", wrong);
    free_host_buffer(&a); free_host_buffer(&b);
    return true;
}

int main(int argc, char **argv) {
    ladder_begin("vulkan", argc, argv);
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    auto gipa = lib ? (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(lib, "vkGetInstanceProcAddr") : nullptr;
    if (!gipa) { skip("instance", "no vulkan-1.dll"); return ladder_done(); }
    vkCreateInstance = (PFN_vkCreateInstance)(void *)gipa(nullptr, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "ladder", 1, "ladder", 1, (1u << 22) | (1u << 12)};   // 1.1
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, 0, nullptr, 0, nullptr};
    VkInstance inst = nullptr;
    VkResult r = vkCreateInstance ? vkCreateInstance(&ici, nullptr, &inst) : -1;
    if (!check(r == 0, "instance", "vkCreateInstance: %d", (int)r)) return ladder_done();
    int missing = 0;
#define LOAD(name, ret, args) name = (PFN_##name)(void *)gipa(inst, #name); missing += !name;
    VK_FUNCS(LOAD)
    if (!check(missing == 0, "entry_points", "%d functions missing from vkGetInstanceProcAddr", missing)) return ladder_done();

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    VkPhysicalDevice pds[8];
    if (n > 8) n = 8;
    vkEnumeratePhysicalDevices(inst, &n, pds);
    if (!check(n > 0, "physical_device", "%u physical device(s)", n)) return ladder_done();
    VkPhysicalDeviceProperties props = {};
    vkGetPhysicalDeviceProperties(pds[0], &props);
    info("device", "%s, Vulkan %u.%u.%u, driver %08x, vendor %04x", props.deviceName, props.apiVersion >> 22,
         (props.apiVersion >> 12) & 0x3ff, props.apiVersion & 0xfff, props.driverVersion, props.vendorID);
    vkGetPhysicalDeviceMemoryProperties(pds[0], &memprops);

    uint32_t nq = 0;
    VkQueueFamilyProperties qf[16];
    vkGetPhysicalDeviceQueueFamilyProperties(pds[0], &nq, nullptr);
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pds[0], &nq, qf);
    uint32_t family = ~0u;
    for (uint32_t i = 0; i < nq && family == ~0u; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) family = i;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, family, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &qci, 0, nullptr, 0, nullptr, nullptr};
    r = family == ~0u ? -1 : vkCreateDevice(pds[0], &dci, nullptr, &dev);
    if (!check(r == 0, "device", "graphics queue family %d, vkCreateDevice: %d", (int)family, (int)r)) return ladder_done();
    vkGetDeviceQueue(dev, family, 0, &queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
    VkCommandPool pool = 0;
    vkCreateCommandPool(dev, &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    vkAllocateCommandBuffers(dev, &cai, &cmd);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0};
    vkCreateFence(dev, &fci, nullptr, &fence);

    if (!image_tests()) fail("image", "%s", g_err);
    if (!buffer_tests()) fail("buffer", "%s", g_err);

    vkDeviceWaitIdle(dev);
    if (img) vkDestroyImage(dev, img, nullptr);
    if (img_mem) vkFreeMemory(dev, img_mem, nullptr);
    vkDestroyFence(dev, fence, nullptr);
    vkDestroyCommandPool(dev, pool, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    pass("teardown", "device and instance destroyed");
    return ladder_done();
}
#endif
