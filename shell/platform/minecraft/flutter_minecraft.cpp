#include <assets/jar_asset_bundle.h>
#include <embedder.h>
#include <flutter_plugin_registrar.h>
#include <shell/platform/common/client_wrapper/include/flutter/plugin_registrar.h>
#include <shell/platform/common/incoming_message_dispatcher.h>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "headless_event_loop.h"
#include "key_event_handler.h"
#include "keyboard_hook_handler.h"
#include "platform_handler.h"
#include "text_input_plugin.h"
#include "vulkan_manager.h"

using namespace flutter;

struct AOTDataDeleter {
  void operator()(const FlutterEngineAOTData aot_data) const {
    FlutterEngineCollectAOTData(aot_data);
  }
};

using UniqueAotDataPtr = std::unique_ptr<_FlutterEngineAOTData, AOTDataDeleter>;

static UniqueAotDataPtr LoadAotData(const std::filesystem::path& aot_data_path,
                                    const JarAssetBundle* assets) {
  if (aot_data_path.empty()) {
    std::cerr
        << "Attempted to load AOT data, but no aot_data_path was provided."
        << std::endl;
    return nullptr;
  }
  auto path_string = aot_data_path.string();
  auto flag = JarAssetBundle::IsJarPath(path_string);
  if (!flag && !exists(aot_data_path)) {
    std::cerr << "Can't load AOT data from " << path_string << "; no such file."
              << std::endl;
    return nullptr;
  }
  FlutterEngineAOTDataSource source{};
  source.type = flag ? kFlutterEngineAOTDataSourceTypeElfMemory
                     : kFlutterEngineAOTDataSourceTypeElfPath;
  source.elf_path = path_string.c_str();
  std::unique_ptr<fml::Mapping> memory = nullptr;
  if (flag) {
    memory = assets->GetAsMapping(path_string);
    source.size = memory->GetSize();
    source.data = memory->GetMapping();
  }
  FlutterEngineAOTData data = nullptr;
  auto result = FlutterEngineCreateAOTData(&source, &data);
  if (result != kSuccess) {
    std::cerr << "Failed to load AOT data from: " << path_string << std::endl;
    return nullptr;
  }
  return UniqueAotDataPtr(data);
}

using FlutterDesktopMessengerReferenceOwner =
    std::unique_ptr<FlutterDesktopMessenger,
                    decltype(&FlutterDesktopMessengerRelease)>;

struct FlutterMinecraftInstance {
  FLUTTER_API_SYMBOL(FlutterEngine) engine;
  UniqueAotDataPtr aot_data;
  std::unique_ptr<FlutterDesktopPluginRegistrar> plugin_registrar;
  std::unique_ptr<PluginRegistrar> internal_plugin_registrar;
  std::vector<std::unique_ptr<KeyboardHookHandler>> keyboard_hook_handlers;
  std::unique_ptr<IncomingMessageDispatcher> message_dispatcher;
  FlutterDesktopMessengerReferenceOwner messenger = {
      nullptr, [](FlutterDesktopMessengerRef) {}};
  BinaryMessenger* binary_messenger;
  std::unique_ptr<EventLoop> event_loop;
  std::unique_ptr<VulkanManager> vulkan;
  bool resize;
};

static FlutterMinecraftInstance* Cast(void* ptr) {
  return (FlutterMinecraftInstance*)ptr;
}

static void ConfigurePlatformTaskRunner(
    FlutterTaskRunnerDescription& task_runner) {
  task_runner.struct_size = sizeof(FlutterTaskRunnerDescription);
  task_runner.runs_task_on_current_thread_callback =
      [](void* instance) -> bool {
    return Cast(instance)->event_loop->RunsTasksOnCurrentThread();
  };
  task_runner.post_task_callback = [](const FlutterTask task,
                                      const uint64_t target_time_nanos,
                                      void* instance) -> void {
    Cast(instance)->event_loop->PostTask(task, target_time_nanos);
  };
}

struct FlutterDesktopPluginRegistrar {
  FlutterMinecraftInstance* instance;
  FlutterDesktopOnPluginRegistrarDestroyed destruction_handler;
};

struct FlutterDesktopMessenger {
  FlutterDesktopMessenger() = default;

  void AddRef() { ref_count_.fetch_add(1); }

  void Release() {
    if (ref_count_.fetch_sub(1) <= 1)
      delete this;
  }

  FlutterMinecraftInstance* Instance() const { return instance_; }

  void Instance(FlutterMinecraftInstance* instance) {
    std::scoped_lock lock(mutex_);
    instance_ = instance;
  }

  std::mutex& GetMutex() { return mutex_; }

  FlutterDesktopMessenger(const FlutterDesktopMessenger& value) = delete;
  FlutterDesktopMessenger& operator=(const FlutterDesktopMessenger& value) =
      delete;

 private:
  FlutterMinecraftInstance* instance_;
  std::atomic<int32_t> ref_count_ = 0;
  std::mutex mutex_;
};

static void EngineOnFlutterPlatformMessage(
    const FlutterPlatformMessage* message,
    void* instance) {
  if (message->struct_size != sizeof(FlutterPlatformMessage)) {
    std::cerr << "Invalid message size received. Expected: "
              << sizeof(FlutterPlatformMessage) << " but received "
              << message->struct_size << std::endl;
    return;
  }
  Cast(instance)->message_dispatcher->HandleMessage(
      *(const FlutterDesktopMessage*)message, nullptr, nullptr);
}

FlutterDesktopMessengerRef FlutterDesktopMessengerAddRef(
    FlutterDesktopMessengerRef messenger) {
  messenger->AddRef();
  return messenger;
}

void FlutterDesktopMessengerRelease(FlutterDesktopMessengerRef messenger) {
  messenger->Release();
}

bool FlutterDesktopMessengerIsAvailable(FlutterDesktopMessengerRef messenger) {
  return messenger->Instance() != nullptr;
}

FlutterDesktopMessengerRef FlutterDesktopMessengerLock(
    FlutterDesktopMessengerRef messenger) {
  messenger->GetMutex().lock();
  return messenger;
}

void FlutterDesktopMessengerUnlock(FlutterDesktopMessengerRef messenger) {
  messenger->GetMutex().unlock();
}

bool FlutterDesktopMessengerSendWithReply(FlutterDesktopMessengerRef messenger,
                                          const char* channel,
                                          const uint8_t* message,
                                          const size_t message_size,
                                          const FlutterDesktopBinaryReply reply,
                                          void* user_data) {
  FlutterPlatformMessageResponseHandle* response_handle = nullptr;
  if (reply != nullptr && user_data != nullptr) {
    FlutterEngineResult result = FlutterPlatformMessageCreateResponseHandle(
        messenger->Instance()->engine, reply, user_data, &response_handle);
    if (result != kSuccess) {
      std::cout << "Failed to create response handle\n";
      return false;
    }
  }

  FlutterPlatformMessage platform_message = {
      sizeof(FlutterPlatformMessage),
      channel,
      message,
      message_size,
      response_handle,
  };

  FlutterEngineResult message_result = FlutterEngineSendPlatformMessage(
      messenger->Instance()->engine, &platform_message);

  if (response_handle != nullptr) {
    FlutterPlatformMessageReleaseResponseHandle(messenger->Instance()->engine,
                                                response_handle);
  }

  return message_result == kSuccess;
}

bool FlutterDesktopMessengerSend(FlutterDesktopMessengerRef messenger,
                                 const char* channel,
                                 const uint8_t* message,
                                 const size_t message_size) {
  return FlutterDesktopMessengerSendWithReply(messenger, channel, message,
                                              message_size, nullptr, nullptr);
}

void FlutterDesktopMessengerSendResponse(
    FlutterDesktopMessengerRef messenger,
    const FlutterDesktopMessageResponseHandle* handle,
    const uint8_t* data,
    size_t data_length) {
  FlutterEngineSendPlatformMessageResponse(messenger->Instance()->engine,
                                           handle, data, data_length);
}

void FlutterDesktopMessengerSetCallback(FlutterDesktopMessengerRef messenger,
                                        const char* channel,
                                        FlutterDesktopMessageCallback callback,
                                        void* user_data) {
  messenger->Instance()->message_dispatcher->SetMessageCallback(
      channel, callback, user_data);
}

FlutterDesktopMessengerRef FlutterDesktopPluginRegistrarGetMessenger(
    FlutterDesktopPluginRegistrarRef registrar) {
  return registrar->instance->messenger.get();
}

FlutterDesktopTextureRegistrarRef FlutterDesktopRegistrarGetTextureRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  return nullptr;
}

int64_t FlutterDesktopTextureRegistrarRegisterExternalTexture(
    FlutterDesktopTextureRegistrarRef texture_registrar,
    const FlutterDesktopTextureInfo* texture_info) {
  return -1;
}

void FlutterDesktopTextureRegistrarUnregisterExternalTexture(
    FlutterDesktopTextureRegistrarRef texture_registrar,
    int64_t texture_id,
    void (*callback)(void* user_data),
    void* user_data) {}

bool FlutterDesktopTextureRegistrarMarkExternalTextureFrameAvailable(
    FlutterDesktopTextureRegistrarRef texture_registrar,
    int64_t texture_id) {
  return false;
}

#include "jni.h"
#include "jnipp.h"

using namespace jni;

template <typename F>
constexpr JNINativeMethod JNIMethod(const char* name, const char* sig, F ptr) {
  return {const_cast<char*>(name), const_cast<char*>(sig), (void*)ptr};
}

static void JNI_SendPointerEvent(JNIEnv*,
                                 jclass,
                                 const FlutterMinecraftInstance* instance,
                                 const FlutterPointerPhase phase,
                                 const double x,
                                 const double y,
                                 const FlutterPointerSignalKind signal_kind,
                                 const double scroll_delta_x,
                                 const double scroll_delta_y,
                                 const FlutterViewId view) {
  FlutterPointerEvent event{};
  event.struct_size = sizeof(event);
  event.phase = phase;
  event.x = x;
  event.y = y;
  event.signal_kind = signal_kind;
  event.scroll_delta_x = scroll_delta_x;
  event.scroll_delta_y = scroll_delta_y;
  event.timestamp =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::high_resolution_clock::now().time_since_epoch())
          .count();
  event.view_id = view;
  FlutterEngineSendPointerEvent(instance->engine, &event, 1);
}

static void JNI_SendKeyEvent(JNIEnv*,
                             jclass,
                             const FlutterMinecraftInstance* instance,
                             void* window,
                             const int key,
                             const int scancode,
                             const int action,
                             const int mods) {
  for (const auto& handler : instance->keyboard_hook_handlers) {
    handler->KeyboardHook(window, key, scancode, action, mods);
  }
}

static void JNI_SendCharEvent(JNIEnv*,
                              jclass,
                              const FlutterMinecraftInstance* instance,
                              void* window,
                              unsigned int code_point) {
  for (const auto& handler : instance->keyboard_hook_handlers) {
    handler->CharHook(window, code_point);
  }
}

static void JNI_SendMetricsEvent(JNIEnv*,
                                 jclass,
                                 FlutterMinecraftInstance* instance,
                                 const int width,
                                 const int height,
                                 const FlutterViewId view) {
  instance->resize = true;
  FlutterWindowMetricsEvent event{};
  event.struct_size = sizeof(event);
  event.width = width;
  event.height = height;
  event.pixel_ratio = 1;
  event.view_id = view;
  FlutterEngineSendWindowMetricsEvent(instance->engine, &event);
}

static void SetupConfig(FlutterVulkanRendererConfig& cfg,
                        const VulkanManager& vulkan) {
  cfg.version = VK_HEADER_VERSION_COMPLETE;
  cfg.instance = vulkan.instance;
  cfg.physical_device = vulkan.physical_device;
  cfg.device = vulkan.device;
  cfg.queue = vulkan.queue;
  cfg.queue_family_index = vulkan.queue_family_index;
  cfg.enabled_device_extension_count = vulkan.device_extensions_count;
  cfg.enabled_device_extensions = vulkan.device_extensions;
  cfg.get_instance_proc_address_callback =
      [](void* instance, const FlutterVulkanInstanceHandle handle,
         const char* name) {
        return Cast(instance)->vulkan->GetProcAddress((VkInstance)handle, name);
      };
  cfg.get_next_image_callback = [](void* instance,
                                   const FlutterFrameInfo* info) {
    if (Cast(instance)->resize) {
      Cast(instance)->resize = false;
      Cast(instance)->vulkan->Resize((int)info->size.width,
                                     (int)info->size.height);
    }
    return FlutterVulkanImage{
        .struct_size = sizeof(FlutterVulkanImage),
        .image = (FlutterVulkanImageHandle)Cast(instance)->vulkan->GetImage(),
        .format = VK_FORMAT_R8G8B8A8_UNORM,
    };
  };
  cfg.present_image_callback = [](void*, const FlutterVulkanImage*) {
    return true;
  };
}

static jlong JNI_CreateInstance(JNIEnv*, jclass, jstring path) {
  auto assets = Object(path).call<std::string>("toString");
  std::vector argv = {"placeholder"};
  JarAssetBundle* asset_bundle = nullptr;
  if (JarAssetBundle::IsJarPath(assets))
    asset_bundle = new JarAssetBundle(assets);
  auto instance = new FlutterMinecraftInstance;
  instance->vulkan = std::make_unique<VulkanManager>();
#ifdef _WIN32
  instance->aot_data = LoadAotData(assets + "/win.so", asset_bundle);
#elif defined(__APPLE__)
  instance->aot_data = LoadAotData(assets + "/mac.so", asset_bundle);
#elif defined(__linux__)
  instance->aot_data = LoadAotData(assets + "/lin.so", asset_bundle);
#endif
  delete asset_bundle;
  instance->event_loop = std::make_unique<HeadlessEventLoop>(
      std::this_thread::get_id(), [instance](const FlutterTask* task) {
        if (FlutterEngineRunTask(instance->engine, task) != kSuccess) {
          std::cerr << "Could not post an engine task." << std::endl;
        }
      });
  FlutterTaskRunnerDescription platform_task_runner{};
  platform_task_runner.user_data = instance;
  ConfigurePlatformTaskRunner(platform_task_runner);
  FlutterCustomTaskRunners task_runners{};
  task_runners.struct_size = sizeof(FlutterCustomTaskRunners);
  task_runners.platform_task_runner = &platform_task_runner;
  task_runners.render_task_runner = &platform_task_runner;
  FlutterRendererConfig render{};
  render.type = kVulkan;
  render.vulkan.struct_size = sizeof(render.vulkan);
  SetupConfig(render.vulkan, *instance->vulkan);
  FlutterProjectArgs args{};
  args.struct_size = sizeof(FlutterProjectArgs);
  args.assets_path = assets.c_str();
  args.icu_data_path = "jar://icudtl.dat";
  args.command_line_argc = static_cast<int>(argv.size());
  args.command_line_argv = argv.data();
  args.platform_message_callback = EngineOnFlutterPlatformMessage;
  args.custom_task_runners = &task_runners;
  args.aot_data = instance->aot_data.get();
  auto result = FlutterEngineRun(FLUTTER_ENGINE_VERSION, &render, &args,
                                 instance, &instance->engine);
  if (result != kSuccess || instance->engine == nullptr) {
    std::cerr << "Failed to start Flutter engine: error " << result
              << std::endl;
    delete instance;
    return 0;
  }
  instance->messenger = FlutterDesktopMessengerReferenceOwner(
      FlutterDesktopMessengerAddRef(new FlutterDesktopMessenger()),
      &FlutterDesktopMessengerRelease);
  instance->messenger->Instance(instance);
  instance->message_dispatcher =
      std::make_unique<IncomingMessageDispatcher>(instance->messenger.get());
  instance->plugin_registrar =
      std::make_unique<FlutterDesktopPluginRegistrar>();
  instance->plugin_registrar->instance = instance;
  instance->internal_plugin_registrar =
      std::make_unique<PluginRegistrar>(instance->plugin_registrar.get());
  instance->binary_messenger = instance->internal_plugin_registrar->messenger();
  instance->keyboard_hook_handlers.emplace_back(
      std::make_unique<KeyEventHandler>(instance->binary_messenger));
  instance->keyboard_hook_handlers.emplace_back(
      std::make_unique<TextInputPlugin>(instance->binary_messenger));
  return (jlong)instance;
}

static void JNI_DestroyInstance(JNIEnv*, jclass, void* instance) {
  FlutterEngineShutdown(Cast(instance)->engine);
  delete Cast(instance);
}

static void JNI_InitFunctions(JNIEnv*,
                              jclass,
                              const jlong f1,
                              const jlong f2,
                              const jlong f3,
                              const jlong f4) {
  KeyEventHandler::GetKeyName = (decltype(KeyEventHandler::GetKeyName))f1;
  PlatformHandler::GetClipboardString =
      (decltype(PlatformHandler::GetClipboardString))f2;
  PlatformHandler::SetClipboardString =
      (decltype(PlatformHandler::SetClipboardString))f3;
  VulkanManager::Init((void*)f4);
}

static void JNI_PollEvents(JNIEnv*,
                           jclass,
                           const FlutterMinecraftInstance* instance) {
  instance->event_loop->WaitForEvents(std::chrono::milliseconds(1));
}

static uint32_t JNI_GetTexture(JNIEnv*,
                               jclass,
                               const FlutterMinecraftInstance* instance) {
  return instance->vulkan->GetTexture();
}

static void JNI_SendPlatformMessage(JNIEnv* env,
                                    jclass,
                                    const FlutterMinecraftInstance* instance,
                                    jstring channel,
                                    jobject data,
                                    jobject reply) {
  auto ch = Object(channel).call<std::string>("toString");
  auto msg = (uint8_t*)env->GetDirectBufferAddress(data);
  auto size = env->GetDirectBufferCapacity(data);
  instance->binary_messenger->Send(
      ch, msg, size,
      reply == nullptr
          ? nullptr
          : (BinaryReply)[reply = Object(reply)](const uint8_t* data,
                                                 size_t size) {
              JNIEnv* env;
              javaVM()->GetEnv((void**)&env, JNI_VERSION_21);
              auto buff = env->NewDirectByteBuffer((void*)data, (jlong)size);
              reply.call<void>("reply(Ljava/nio/ByteBuffer;)V", buff);
            });
}

static void JNI_SendMessageResponse(JNIEnv* env,
                                    jclass,
                                    const BinaryReply& reply,
                                    const jobject data) {
  auto msg = (uint8_t*)env->GetDirectBufferAddress(data);
  auto size = env->GetDirectBufferCapacity(data);
  reply(msg, size);
}

static void JNI_SetMessageHandler(JNIEnv*,
                                  jclass,
                                  const FlutterMinecraftInstance* instance,
                                  jstring channel,
                                  jobject handler) {
  auto ch = Object(channel).call<std::string>("toString");
  if (!handler) {
    instance->binary_messenger->SetMessageHandler(ch, nullptr);
    return;
  }
  auto binary_handler = [handler = Object(handler)](const uint8_t* message,
                                                    size_t message_size,
                                                    BinaryReply reply) {
    JNIEnv* env;
    javaVM()->GetEnv((void**)&env, JNI_VERSION_21);
    Class cls("com/primogemstudio/advancedfmk/flutter/IncomingBinaryReply");
    auto ibr = cls.newInstance(cls.getConstructor("(J)V"), (jlong)&reply);
    auto buff = env->NewDirectByteBuffer((void*)message, (jlong)message_size);
    handler.call<void>(
        "onMessage(Ljava/nio/ByteBuffer;Lcom/primogemstudio/advancedfmk/"
        "flutter/BinaryReply;)V",
        buff, ibr);
  };
  instance->binary_messenger->SetMessageHandler(ch, binary_handler);
}

static int RegisterMethods(JNIEnv* env) {
  const auto native =
      env->FindClass("com/primogemstudio/advancedfmk/flutter/FlutterNative");
  JNINativeMethod methods[12];
  methods[0] =
      JNIMethod("createInstance", "(Ljava/lang/String;)J", JNI_CreateInstance);
  methods[1] = JNIMethod("destroyInstance", "(J)V", JNI_DestroyInstance);
  methods[2] = JNIMethod("init", "(JJJJ)V", JNI_InitFunctions);
  methods[3] =
      JNIMethod("sendPointerEvent", "(JIDDIDDJ)V", JNI_SendPointerEvent);
  methods[4] = JNIMethod("sendKeyEvent", "(JJIIII)V", JNI_SendKeyEvent);
  methods[5] = JNIMethod("sendCharEvent", "(JJI)V", JNI_SendCharEvent);
  methods[6] = JNIMethod("sendMetricsEvent", "(JIIJ)V", JNI_SendMetricsEvent);
  methods[7] = JNIMethod("pollEvents", "(J)V", JNI_PollEvents);
  methods[8] = JNIMethod("getTexture", "(J)I", JNI_GetTexture);
  methods[9] = JNIMethod("sendPlatformMessage",
                         "(JLjava/lang/String;Ljava/nio/ByteBuffer;Lcom/"
                         "primogemstudio/advancedfmk/flutter/BinaryReply;)V",
                         JNI_SendPlatformMessage);
  methods[10] = JNIMethod("sendMessageResponse", "(JLjava/nio/ByteBuffer;)V",
                          JNI_SendMessageResponse);
  methods[11] = JNIMethod("setMessageHandler",
                          "(JLjava/lang/String;Lcom/primogemstudio/advancedfmk/"
                          "flutter/BinaryMessageHandler;)V",
                          JNI_SetMessageHandler);
  return env->RegisterNatives(native, methods, std::size(methods));
}

namespace jni {
void InitGlobalClassRefs(JNIEnv* env);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
  init(vm);
  JNIEnv* env;
  if (vm->GetEnv((void**)&env, JNI_VERSION_21) != JNI_OK)
    return JNI_EVERSION;
  if (RegisterMethods(env) != JNI_OK)
    return JNI_ERR;
  JarAssetBundle::delegate.IsJarPath = JarAssetBundle::IsJarPath;
  JarAssetBundle::delegate.Create = [](const std::string& path) {
    return new JarAssetBundle(path);
  };
  InitGlobalClassRefs(env);
  return JNI_VERSION_21;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* reserved) {}
