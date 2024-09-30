#include "flutter/shell/platform/minecraft/platform_handler.h"

#include <iostream>

#include "flutter/shell/platform/common/json_method_codec.h"

static constexpr char kChannelName[] = "flutter/platform";

static constexpr char kGetClipboardDataMethod[] = "Clipboard.getData";
static constexpr char kSetClipboardDataMethod[] = "Clipboard.setData";

static constexpr char kTextPlainFormat[] = "text/plain";
static constexpr char kTextKey[] = "text";

static constexpr char kNoWindowError[] = "Missing window error";
static constexpr char kUnknownClipboardFormatError[] =
    "Unknown clipboard format error";

namespace flutter {
void (*PlatformHandler::SetClipboardString)(void* window, const char* str);
const char* (*PlatformHandler::GetClipboardString)(void* window);

PlatformHandler::PlatformHandler(BinaryMessenger* messenger, void* window)
    : channel_(std::make_unique<MethodChannel<rapidjson::Document>>(
          messenger,
          kChannelName,
          &JsonMethodCodec::GetInstance())),
      window_(window) {
  channel_->SetMethodCallHandler(
      [this](const MethodCall<rapidjson::Document>& call,
             std::unique_ptr<MethodResult<rapidjson::Document>> result) {
        HandleMethodCall(call, std::move(result));
      });
}

void PlatformHandler::HandleMethodCall(
    const MethodCall<rapidjson::Document>& method_call,
    std::unique_ptr<MethodResult<rapidjson::Document>> result) {
  const std::string& method = method_call.method_name();

  if (method == kGetClipboardDataMethod) {
    if (!window_) {
      result->Error(kNoWindowError,
                    "Clipboard is not available in GLFW headless mode.");
      return;
    }
    const rapidjson::Value& format = method_call.arguments()[0];

    if (strcmp(format.GetString(), kTextPlainFormat) != 0) {
      result->Error(kUnknownClipboardFormatError,
                    "GLFW clipboard API only supports text.");
      return;
    }

    const char* clipboardData = GetClipboardString(window_);
    if (clipboardData == nullptr) {
      result->Error(kUnknownClipboardFormatError,
                    "Failed to retrieve clipboard data from GLFW api.");
      return;
    }
    rapidjson::Document document;
    document.SetObject();
    rapidjson::Document::AllocatorType& allocator = document.GetAllocator();
    document.AddMember(rapidjson::Value(kTextKey, allocator),
                       rapidjson::Value(clipboardData, allocator), allocator);
    result->Success(document);
  } else if (method == kSetClipboardDataMethod) {
    if (!window_) {
      result->Error(kNoWindowError,
                    "Clipboard is not available in GLFW headless mode.");
      return;
    }
    const rapidjson::Value& document = *method_call.arguments();
    rapidjson::Value::ConstMemberIterator itr = document.FindMember(kTextKey);
    if (itr == document.MemberEnd()) {
      result->Error(kUnknownClipboardFormatError,
                    "Missing text to store on clipboard.");
      return;
    }
    SetClipboardString(window_, itr->value.GetString());
    result->Success();
  } else {
    result->NotImplemented();
  }
}
}  // namespace flutter
