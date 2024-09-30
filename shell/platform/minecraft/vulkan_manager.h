#pragma once

#include <vulkan/vulkan.hpp>

template <typename T>
using UniquePtr = vk::UniqueHandle<T, vk::DispatchLoaderDynamic>;

class ExportTexture final {
  struct OpenGLFunctions* opengl;

 public:
  UniquePtr<vk::Image> image;
  UniquePtr<vk::DeviceMemory> memory;
  uint32_t texture;
  uint32_t memory_object;
#ifdef _WIN32
  HANDLE handle;
#else
  int fd;
#endif

  ExportTexture(const class VulkanManager*, int width, int height);
  ~ExportTexture();
};

class VulkanManager final {
  friend ExportTexture;
  vk::DispatchLoaderDynamic dsym;
  OpenGLFunctions* opengl;
  ExportTexture* texture;

  uint32_t FindMemoryType(uint32_t, vk::MemoryPropertyFlagBits) const;

 public:
  vk::Instance instance;
  vk::PhysicalDevice physical_device;
  vk::Device device;
  vk::Queue queue;
  uint32_t queue_family_index;
  uint32_t device_extensions_count;
  uint32_t result;
  const char** device_extensions;

  VulkanManager();
  ~VulkanManager();
  void* GetProcAddress(VkInstance instance, const char* name) const;
  void Resize(int width, int height);
  VkImage GetImage() const;
  uint32_t GetTexture() const;
  static void Init(void* f4);
};
