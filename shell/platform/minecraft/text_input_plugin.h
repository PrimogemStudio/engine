#pragma once

#include <memory>

#include "flutter/shell/platform/common/client_wrapper/include/flutter/binary_messenger.h"
#include "flutter/shell/platform/common/client_wrapper/include/flutter/method_channel.h"
#include "flutter/shell/platform/common/text_input_model.h"
#include "flutter/shell/platform/minecraft/keyboard_hook_handler.h"

#include "rapidjson/document.h"

namespace flutter {
class TextInputPlugin : public KeyboardHookHandler {
 public:
  explicit TextInputPlugin(BinaryMessenger* messenger);

  ~TextInputPlugin() override;

  void KeyboardHook(void* window,
                    int key,
                    int scancode,
                    int action,
                    int mods) override;

  void CharHook(void* window, unsigned int code_point) override;

 private:
  void SendStateUpdate(const TextInputModel& model);

  void EnterPressed(TextInputModel* model);

  void HandleMethodCall(
      const MethodCall<rapidjson::Document>& method_call,
      std::unique_ptr<MethodResult<rapidjson::Document>> result);

  std::unique_ptr<MethodChannel<rapidjson::Document>> channel_;

  int client_id_ = 0;

  std::unique_ptr<TextInputModel> active_model_;

  std::string input_type_;

  std::string input_action_;
};
}  // namespace flutter
