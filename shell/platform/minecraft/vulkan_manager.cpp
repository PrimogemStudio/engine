#include "vulkan_manager.h"

#ifdef _WIN32
#include <vulkan/vulkan_win32.h>
#else
#include <unistd.h>
#endif

static void* (*GetOpenGLProc)(const char*);

enum : uint16_t {
  GL_TEXTURE_2D = 0xDE1,
  GL_LINEAR = 0x2601,
  GL_TEXTURE_MAG_FILTER = 0x2800,
  GL_TEXTURE_MIN_FILTER = 0x2801,
  GL_RGBA8 = 0x8058,
  GL_HANDLE_TYPE_OPAQUE_FD_EXT = 0x9586,
  GL_HANDLE_TYPE_OPAQUE_WIN32_EXT = 0x9587
};

struct OpenGLFunctions {
  void (*glGenTextures)(int count, uint32_t* textures);
  void (*glDeleteTextures)(int count, uint32_t* textures);
  void (*glBindTexture)(int target, uint32_t texture);
  void (*glTexParameteri)(int target, int name, int param);
  void (*glCreateMemoryObjectsEXT)(int count, uint32_t* objects);
  void (*glDeleteMemoryObjectsEXT)(int count, uint32_t* objects);
  void (*glImportMemoryWin32HandleEXT)(uint32_t memory,
                                       uint64_t size,
                                       uint32_t handleType,
                                       void* handle);
  void (*glImportMemoryFdEXT)(uint32_t memory,
                              uint64_t size,
                              uint32_t handleType,
                              int fd);
  void (*glTextureStorageMem2DEXT)(uint32_t texture,
                                   int levels,
                                   uint32_t internalFormat,
                                   int width,
                                   int height,
                                   uint32_t memory,
                                   uint64_t offset);
  OpenGLFunctions();
};

uint32_t VulkanManager::FindMemoryType(
    const uint32_t memoryTypeBits,
    const vk::MemoryPropertyFlagBits properties) const {
  auto props = physical_device.getMemoryProperties(dsym);
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    if (memoryTypeBits & 1 << i &&
        (props.memoryTypes[i].propertyFlags & properties) == properties)
      return i;
  }
  return -1;
}

VulkanManager::VulkanManager()
    : opengl(new OpenGLFunctions),
      texture(nullptr),
      queue_family_index(0),
      result(0) {
  static const char* extensions[]{
      VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
#ifdef _WIN32
      VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
#else
      VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
#endif
  };
  device_extensions_count = std::size(extensions);
  device_extensions = extensions;
  VkApplicationInfo app_info{
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pNext = nullptr,
      .pApplicationName = "Flutter",
      .applicationVersion = VK_MAKE_API_VERSION(0, 3, 24, 3),
      .pEngineName = "Minecraft",
      .engineVersion = VK_MAKE_API_VERSION(0, 1, 21, 1),
      .apiVersion = VK_HEADER_VERSION_COMPLETE,
  };
  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  info.flags = 0;
  info.pApplicationInfo = &app_info;
  dsym.init();
  dsym.vkCreateInstance(&info, nullptr, (VkInstance*)&instance);
  dsym.init(instance);
  for (auto& device : instance.enumeratePhysicalDevices(dsym)) {
    if (device.getProperties(dsym).deviceType ==
        vk::PhysicalDeviceType::eDiscreteGpu) {
      physical_device = device;
      for (auto& prop : device.getQueueFamilyProperties(dsym)) {
        if (prop.queueFlags & vk::QueueFlagBits::eGraphics)
          break;
        queue_family_index++;
      }
      break;
    }
  }
  VkPhysicalDeviceFeatures device_features{};
  VkDeviceQueueCreateInfo graphics_queue{};
  graphics_queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  graphics_queue.queueFamilyIndex = queue_family_index;
  graphics_queue.queueCount = 1;
  float priority = 1.0f;
  graphics_queue.pQueuePriorities = &priority;
  VkDeviceCreateInfo device_info{};
  device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_info.enabledExtensionCount = device_extensions_count;
  device_info.ppEnabledExtensionNames = device_extensions;
  device_info.pEnabledFeatures = &device_features;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &graphics_queue;
  device = physical_device.createDevice(device_info, nullptr, dsym);
  queue = device.getQueue(queue_family_index, 0, dsym);
}

ExportTexture::ExportTexture(const VulkanManager* vulkan,
                             const int width,
                             const int height)
    : opengl(vulkan->opengl) {
  VkImageCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = VK_FORMAT_R8G8B8A8_UNORM;
  info.extent.width = width;
  info.extent.height = height;
  info.extent.depth = 1;
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  image = vulkan->device.createImageUnique(info, nullptr, vulkan->dsym);
  auto req =
      vulkan->device.getImageMemoryRequirements(image.get(), vulkan->dsym);
  VkExportMemoryAllocateInfo export_alloc{
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .pNext = nullptr,
#ifdef WIN32
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
#else
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
#endif
  };
  VkMemoryAllocateInfo alloc;
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.pNext = &export_alloc;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = vulkan->FindMemoryType(
      req.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
  memory = vulkan->device.allocateMemoryUnique(alloc, nullptr, vulkan->dsym);
  vulkan->device.bindImageMemory(image.get(), memory.get(), 0, vulkan->dsym);
  opengl->glCreateMemoryObjectsEXT(1, &memory_object);
#ifdef _WIN32
  VkMemoryGetWin32HandleInfoKHR getInfo{
      .sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
      .pNext = nullptr,
      .memory = memory.get(),
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT_KHR,
  };
  handle = vulkan->device.getMemoryWin32HandleKHR(getInfo, vulkan->dsym);
  opengl->glImportMemoryWin32HandleEXT(memory_object, req.size,
                                       GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);
#else
  VkMemoryGetFdInfoKHR getInfo{
      .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
      .pNext = nullptr,
      .memory = memory.get(),
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR,
  };
  fd = vulkan->device.getMemoryFdKHR(getInfo, vulkan->dsym);
  opengl->glImportMemoryFdEXT(memory_object, req.size,
                              GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);
#endif
  opengl->glGenTextures(1, &texture);
  opengl->glBindTexture(GL_TEXTURE_2D, texture);
  opengl->glTextureStorageMem2DEXT(texture, 1, GL_RGBA8, width, height,
                                   memory_object, 0);
  opengl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  opengl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

ExportTexture::~ExportTexture() {
  opengl->glDeleteTextures(1, &texture);
#ifdef _WIN32
  CloseHandle(handle);
#else
  close(fd);
#endif
  opengl->glDeleteMemoryObjectsEXT(1, &memory_object);
}

VulkanManager::~VulkanManager() {
  delete texture;
  delete opengl;
  device.destroy(nullptr, dsym);
  instance.destroy(nullptr, dsym);
}

void* VulkanManager::GetProcAddress(const VkInstance instance,
                                    const char* name) const {
  return (void*)dsym.vkGetInstanceProcAddr(instance, name);
}

void VulkanManager::Resize(const int width, const int height) {
  delete texture;
  texture = new ExportTexture(this, width, height);
  result = texture->texture;
}

VkImage VulkanManager::GetImage() const {
  return texture->image.get();
}

uint32_t VulkanManager::GetTexture() const {
  return result;
}

void VulkanManager::Init(void* f4) {
  GetOpenGLProc = (decltype(GetOpenGLProc))f4;
}

OpenGLFunctions::OpenGLFunctions() {
#define GET_PROC(F) F = (decltype(F))GetOpenGLProc(#F)
  GET_PROC(glGenTextures);
  GET_PROC(glDeleteTextures);
  GET_PROC(glBindTexture);
  GET_PROC(glTexParameteri);
  GET_PROC(glCreateMemoryObjectsEXT);
  GET_PROC(glDeleteMemoryObjectsEXT);
  GET_PROC(glImportMemoryWin32HandleEXT);
  GET_PROC(glImportMemoryFdEXT);
  GET_PROC(glTextureStorageMem2DEXT);
#undef GET_PROC
}
