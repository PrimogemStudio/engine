#pragma once

#include "flutter/assets/asset_resolver.h"

namespace flutter {

class JarAssetBundle : public AssetResolver {
 public:
  JarAssetBundle(const std::string& path);

  ~JarAssetBundle() override;

  static bool IsJarPath(const std::string& path);

  static struct {
    bool (*IsJarPath)(const std::string& path);
    JarAssetBundle* (*Create)(const std::string& path);
  } delegate;

  std::unique_ptr<fml::Mapping> GetAsMapping(
      const std::string& asset_name) const override;

 private:
  std::string root;

  bool IsValid() const override;

  bool IsValidAfterAssetManagerChange() const override;

  AssetResolverType GetType() const override;

  std::vector<std::unique_ptr<fml::Mapping>> GetAsMappings(
      const std::string& asset_pattern,
      const std::optional<std::string>& subdir) const override;

  const JarAssetBundle* as_jar_asset_bundle() const override;

  bool operator==(const AssetResolver& other) const override;

  FML_DISALLOW_COPY_AND_ASSIGN(JarAssetBundle);
};
}  // namespace flutter
