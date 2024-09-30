#pragma once

#include "flutter/shell/platform/common/client_wrapper/include/flutter/binary_messenger.h"
#include "flutter/shell/platform/common/client_wrapper/include/flutter/method_channel.h"
#include "rapidjson/document.h"

namespace flutter {
class PlatformHandler {
 public:
  explicit PlatformHandler(BinaryMessenger* messenger, void* window);

  static void (*SetClipboardString)(void* window, const char* str);
  static const char* (*GetClipboardString)(void* window);

 private:
  void HandleMethodCall(
      const MethodCall<rapidjson::Document>& method_call,
      std::unique_ptr<MethodResult<rapidjson::Document>> result);

  std::unique_ptr<MethodChannel<rapidjson::Document>> channel_;

  void* window_;
};
}  // namespace flutter
