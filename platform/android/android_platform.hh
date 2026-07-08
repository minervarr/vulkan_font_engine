#pragma once
// Android implementation of the engine's asset seam (core/asset_reader.hh):
// reads bundled assets out of the APK via AAssetManager.

#include "asset_reader.hh"

#include <android/asset_manager.h>

class AndroidAssetReader : public AssetReader {
 public:
  explicit AndroidAssetReader(AAssetManager* mgr) : mgr_(mgr) {}
  bool read(const char* path, std::vector<uint8_t>& out) override;

 private:
  AAssetManager* mgr_;
};
