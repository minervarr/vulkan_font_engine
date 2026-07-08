#include "app.hh"
#include <android_native_app_glue.h>
#include <android/input.h>
#include <android/log.h>

#define TAG  "Main"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

struct FontApp {
  App vk;
};

static void pushContentRect(struct android_app* state, FontApp* fa) {
  if (!fa->vk.initialized || !state->window) return;
  int32_t w = ANativeWindow_getWidth(state->window);
  int32_t h = ANativeWindow_getHeight(state->window);
  const ARect& r = state->contentRect;
  uint32_t top    = r.top    > 0 ? (uint32_t)r.top                            : 0;
  uint32_t left   = r.left   > 0 ? (uint32_t)r.left                           : 0;
  uint32_t bottom = (r.bottom > 0 && r.bottom < h) ? (uint32_t)(h - r.bottom) : 0;
  uint32_t right  = (r.right  > 0 && r.right  < w) ? (uint32_t)(w - r.right)  : 0;
  fa->vk.setInsets(top, bottom, left, right);
}

static void handleCommand(struct android_app* state, int32_t cmd) {
  FontApp* fa = static_cast<FontApp*>(state->userData);
  switch (cmd) {
    case APP_CMD_INIT_WINDOW:
      fa->vk.init(state->window, state->activity->assetManager);
      fa->vk.initialized = true;
      fa->vk.dirty       = true;
      pushContentRect(state, fa);
      break;
    case APP_CMD_TERM_WINDOW:
      fa->vk.cleanup();
      new (&fa->vk) App();
      fa->vk.initialized = false;
      break;
    case APP_CMD_CONTENT_RECT_CHANGED:
      pushContentRect(state, fa);
      fa->vk.dirty = true;
      break;
    case APP_CMD_WINDOW_REDRAW_NEEDED:
    case APP_CMD_CONFIG_CHANGED:
      fa->vk.dirty = true;
      break;
    default: break;
  }
}

static int32_t handleInput(struct android_app* state, AInputEvent* event) {
  if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) return 0;
  int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
  if (action != AMOTION_EVENT_ACTION_DOWN) return 0;
  FontApp* fa = static_cast<FontApp*>(state->userData);
  if (!fa->vk.initialized) return 0;
  // Tap redraws (useful for forcing a redraw if the screen goes blank).
  fa->vk.dirty = true;
  return 1;
}

void android_main(struct android_app* state) {
  FontApp fa;
  state->userData     = &fa;
  state->onAppCmd     = handleCommand;
  state->onInputEvent = handleInput;

  while (true) {
    int events;
    struct android_poll_source* source;

    // Drain pending events.  Block (timeout -1) while idle so we don't spin the
    // CPU; poll non-blocking (timeout 0) only while frames are still queued.
    int timeout = (fa.vk.initialized && fa.vk.needsDraw()) ? 0 : -1;
    while (ALooper_pollOnce(timeout, nullptr, &events, (void**)&source) >= 0) {
      if (source) source->process(state, source);
      if (state->destroyRequested) return;
      timeout = 0;
    }

    if (fa.vk.initialized) {
      // A scene change queues one frame per swapchain image, so the static
      // content is painted into all of them before we go back to idle.
      if (fa.vk.dirty) { fa.vk.requestRedraw(); fa.vk.dirty = false; }
      if (fa.vk.needsDraw()) fa.vk.drawFrame();
    }
  }
}
