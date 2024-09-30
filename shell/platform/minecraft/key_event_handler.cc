#include "flutter/shell/platform/minecraft/key_event_handler.h"

#include <iostream>

#include "flutter/shell/platform/common/json_message_codec.h"
#include "flutter/shell/platform/minecraft/glfw_defines.h"

static constexpr char kChannelName[] = "flutter/keyevent";

static constexpr char kKeyCodeKey[] = "keyCode";
static constexpr char kKeyMapKey[] = "keymap";
static constexpr char kScanCodeKey[] = "scanCode";
static constexpr char kModifiersKey[] = "modifiers";
static constexpr char kTypeKey[] = "type";
static constexpr char kToolkitKey[] = "toolkit";
static constexpr char kUnicodeScalarValues[] = "unicodeScalarValues";

static constexpr char kLinuxKeyMap[] = "linux";
static constexpr char kGLFWKey[] = "glfw";

static constexpr char kKeyUp[] = "keyup";
static constexpr char kKeyDown[] = "keydown";

static constexpr int kTwoByteMask = 0xC0;
static constexpr int kThreeByteMask = 0xE0;
static constexpr int kFourByteMask = 0xF0;

namespace flutter {
const char* (*KeyEventHandler::GetKeyName)(int, int) = nullptr;
namespace {
struct UTF8CodePointInfo {
  int first_byte_mask;
  size_t length;
};

UTF8CodePointInfo GetUTF8CodePointInfo(int first_byte) {
  UTF8CodePointInfo byte_info;
  if ((first_byte & kFourByteMask) == kFourByteMask) {
    byte_info.first_byte_mask = 0x07;
    byte_info.length = 4;
  } else if ((first_byte & kThreeByteMask) == kThreeByteMask) {
    byte_info.first_byte_mask = 0x0F;
    byte_info.length = 3;
  } else if ((first_byte & kTwoByteMask) == kTwoByteMask) {
    byte_info.first_byte_mask = 0x1F;
    byte_info.length = 2;
  } else {
    byte_info.first_byte_mask = 0xFF;
    byte_info.length = 1;
  }
  return byte_info;
}

bool GetUTF32CodePointFromGLFWKey(int key,
                                  int scan_code,
                                  uint32_t* code_point) {
  const char* utf8 = KeyEventHandler::GetKeyName(key, scan_code);
  if (utf8 == nullptr) {
    return false;
  }
  const auto byte_info = GetUTF8CodePointInfo(utf8[0]);
  int shift = byte_info.length - 1;

  uint32_t result = 0;

  size_t current_byte_index = 0;
  while (current_byte_index < byte_info.length) {
    constexpr int complement_mask = 0x3F;
    const int current_byte = utf8[current_byte_index];
    const int mask =
        current_byte_index == 0 ? byte_info.first_byte_mask : complement_mask;
    current_byte_index++;
    const int bits_to_shift = 6 * shift--;
    result += (current_byte & mask) << bits_to_shift;
  }
  *code_point = result;
  return true;
}
}  // namespace

KeyEventHandler::KeyEventHandler(BinaryMessenger* messenger)
    : channel_(std::make_unique<BasicMessageChannel<rapidjson::Document>>(
          messenger,
          kChannelName,
          &JsonMessageCodec::GetInstance())) {}

KeyEventHandler::~KeyEventHandler() = default;

void KeyEventHandler::CharHook(void* window, unsigned int code_point) {}

void KeyEventHandler::KeyboardHook(void* window,
                                   int key,
                                   int scancode,
                                   int action,
                                   int mods) {
  rapidjson::Document event(rapidjson::kObjectType);
  auto& allocator = event.GetAllocator();
  event.AddMember(kKeyCodeKey, key, allocator);
  event.AddMember(kKeyMapKey, kLinuxKeyMap, allocator);
  event.AddMember(kScanCodeKey, scancode, allocator);
  event.AddMember(kModifiersKey, mods, allocator);
  event.AddMember(kToolkitKey, kGLFWKey, allocator);

  uint32_t unicodeInt;
  if (GetUTF32CodePointFromGLFWKey(key, scancode, &unicodeInt)) {
    event.AddMember(kUnicodeScalarValues, unicodeInt, allocator);
  }

  switch (action) {
    case GLFW_PRESS:
    case GLFW_REPEAT:
      event.AddMember(kTypeKey, kKeyDown, allocator);
      break;
    case GLFW_RELEASE:
      event.AddMember(kTypeKey, kKeyUp, allocator);
      break;
    default:
      std::cerr << "Unknown key event action: " << action << std::endl;
      return;
  }
  channel_->Send(event);
}
}  // namespace flutter
