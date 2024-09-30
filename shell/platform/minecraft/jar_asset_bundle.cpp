#include <assets/jar_asset_bundle.h>
#include <regex>
#include "jni.h"
#include "jnipp.h"

static std::regex reg("jar:/(/.*)");

using namespace jni;

namespace jni {
static jmethodID getResourceAsStreamID, readAllBytesID, closeID;
static jclass class_;

void InitGlobalClassRefs(JNIEnv* env) {
  class_ = (jclass)env->NewWeakGlobalRef(
      env->FindClass("com/primogemstudio/advancedfmk/flutter/FlutterNative"));
  getResourceAsStreamID =
      env->GetMethodID(env->FindClass("java/lang/Class"), "getResourceAsStream",
                       "(Ljava/lang/String;)Ljava/io/InputStream;");
  readAllBytesID = env->GetMethodID(env->FindClass("java/io/InputStream"),
                                    "readAllBytes", "()[B");
  closeID =
      env->GetMethodID(env->FindClass("java/io/InputStream"), "close", "()V");
}
}  // namespace jni

namespace flutter {

JarAssetBundle::JarAssetBundle(const std::string& path) {
  std::smatch match;
  if (std::regex_match(path, match, reg)) {
    root = match[1].str();
  }
}

JarAssetBundle::~JarAssetBundle() = default;

bool JarAssetBundle::IsJarPath(const std::string& path) {
  return std::regex_match(path, reg);
}

bool JarAssetBundle::IsValid() const {
  return true;
}

bool JarAssetBundle::IsValidAfterAssetManagerChange() const {
  return true;
}

AssetResolver::AssetResolverType JarAssetBundle::GetType() const {
  return kJarEmbeddedAssetBundle;
}

std::unique_ptr<fml::Mapping> JarAssetBundle::GetAsMapping(
    const std::string& asset_name) const {
  std::smatch match;
  auto path = asset_name;
  if (std::regex_match(asset_name, match, reg)) {
    path = match[1].str();
  } else if (asset_name[0] != '/') {
    path = root + '/' + asset_name;
  }
  auto env = attach();
  auto in = env->CallObjectMethod(class_, getResourceAsStreamID,
                                  env->NewStringUTF(path.c_str()));
  auto arr = env->CallObjectMethod(in, readAllBytesID);
  env->CallVoidMethod(in, closeID);
  auto size = env->GetArrayLength((jbyteArray)arr);
  auto ptr = env->GetByteArrayElements((jbyteArray)arr, nullptr);
  std::vector<uint8_t> buff(size);
  memcpy(buff.data(), ptr, size);
  env->ReleaseByteArrayElements((jbyteArray)arr, ptr, 0);
  detach();
  return std::make_unique<fml::DataMapping>(std::move(buff));
}

const JarAssetBundle* JarAssetBundle::as_jar_asset_bundle() const {
  return this;
}

std::vector<std::unique_ptr<fml::Mapping>> JarAssetBundle::GetAsMappings(
    const std::string& asset_pattern,
    const std::optional<std::string>& subdir) const {
  std::vector<std::unique_ptr<fml::Mapping>> mappings;
  return mappings;
}

bool JarAssetBundle::operator==(const AssetResolver& other) const {
  auto other_bundle = other.as_jar_asset_bundle();
  if (!other_bundle) {
    return false;
  }
  return true;
}
}  // namespace flutter
