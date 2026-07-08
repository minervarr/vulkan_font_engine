#include "android_platform.hh"

bool AndroidAssetReader::read(const char* path, std::vector<uint8_t>& out) {
  AAsset* asset = AAssetManager_open(mgr_, path, AASSET_MODE_BUFFER);
  if (!asset) return false;
  size_t size = static_cast<size_t>(AAsset_getLength(asset));
  out.resize(size);
  int64_t got = AAsset_read(asset, out.data(), size);
  AAsset_close(asset);
  return got == static_cast<int64_t>(size);
}
