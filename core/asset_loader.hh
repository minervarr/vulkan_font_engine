#pragma once
#include <cstdint>
#include <functional>
#include <vector>

// Reads a whole asset/file into bytes. Android implements this over
// AAssetManager; desktop backends over a plain file read.
using AssetLoader = std::function<std::vector<uint8_t>(const char*)>;
