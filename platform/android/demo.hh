#pragma once
#include <cstdint>
#include <vector>

struct Font;

class Demo {
 public:
  void init(uint32_t w, uint32_t h);
  void setInsets(uint32_t top, uint32_t bottom, uint32_t left, uint32_t right);
  void rebuildCurves(std::vector<float>& out, const Font* font) const;

  bool dirty = true;

 private:
  uint32_t screenW = 0;
  uint32_t screenH = 0;
  uint32_t insetTop = 0, insetBottom = 0, insetLeft = 0, insetRight = 0;
};
