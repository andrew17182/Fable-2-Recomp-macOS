#include "fable2_window_position.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <string>
#include <unistd.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <toml++/toml.hpp>

namespace fable2 {

WindowPosition::~WindowPosition() {
  if (watching_) SDL_RemoveEventWatch(Watch, this);
}

bool WindowPosition::Arm(const std::filesystem::path& path, bool restore, bool restore_size,
                         bool restore_on_show) {
  if (watching_ || !SDL_IsMainThread()) return false;
  restore_on_show_ = restore_on_show;
  path_ = path;
  restore_ = restore;
  restore_size_ = restore_size;
  watching_ = armed_ = SDL_AddEventWatch(Watch, this);
  return armed_;
}

void WindowPosition::Load() {
  std::error_code error;
  if (std::filesystem::exists(path_, error)) {
    try {
      const auto table = toml::parse_file(path_.string());
      const auto x = table["x"].value<int32_t>();
      const auto y = table["y"].value<int32_t>();
      if (x && y) {
        saved_ = SavedWindowFrame{SDL_Point{*x, *y}, std::nullopt};
        // Size is optional: files written before size memory have only x/y.
        const auto width = table["width"].value<int64_t>();
        const auto height = table["height"].value<int64_t>();
        if (width && height && *width >= 1 && *width <= kMaxSize && *height >= 1 &&
            *height <= kMaxSize) {
          saved_->size = SDL_Point{int(*width), int(*height)};
        } else if (width || height || table.contains("width") || table.contains("height")) {
          REXLOG_WARN("Ignoring invalid saved window size in {}", path_.string());
        }
      }
    } catch (const toml::parse_error&) {
      REXLOG_WARN("Ignoring invalid saved window position in {}", path_.string());
    }
  }
}

std::optional<SavedWindowFrame> WindowPosition::LoadForSdk(const std::filesystem::path& path) {
  path_ = path;
  Load();
  placed_by_sdk_ = true;
  return saved_;
}

bool WindowPosition::Attach(std::string_view title, const std::filesystem::path& path,
                            bool restore, bool restore_size) {
  if (attached_ || !SDL_IsMainThread()) return false;
  if (!early_bound_) {
    int count = 0;
    auto** windows = SDL_GetWindows(&count);
    for (int i = 0; i < count; ++i) {
      if (title == SDL_GetWindowTitle(windows[i])) {
        window_ = windows[i];
        break;
      }
    }
    SDL_free(windows);
    if (!window_) return false;
  }
  // Cocoa may still have placement events pending from opening the window.
  // Finish those before restoring and observing moves so they cannot replace
  // the saved position with the initial default location.
  SDL_SyncWindow(window_);
  if (!early_restored_) {
    window_id_ = SDL_GetWindowID(window_);
    path_ = path;
    restore_size_ = restore_size;
    Load();
    // An explicit monitor selection takes priority over the saved location.
    // With SDK placement the window already opened there (or the SDK kept its
    // default because the position was off-screen); do not move it again.
    if (!restore) saved_.reset();
    else if (!placed_by_sdk_) Restore();
  }
  // The early path already restored at SDL_EVENT_WINDOW_SHOWN; do not apply
  // the position a second time.
  if (!watching_) watching_ = SDL_AddEventWatch(Watch, this);
  attached_ = watching_;
  return attached_;
}

void WindowPosition::Restore() {
  if (!saved_ || !window_ ||
      (SDL_GetWindowFlags(window_) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED))) return;

  int top = 0;
  SDL_GetWindowBordersSize(window_, &top, nullptr, nullptr, nullptr);
  int count = 0;
  auto* displays = SDL_GetDisplays(&count);
  bool visible = false;
  SDL_Rect usable{};
  for (int i = 0; i < count && !visible; ++i) {
    SDL_Rect bounds;
    if (!SDL_GetDisplayUsableBounds(displays[i], &bounds)) continue;
    // Keep a usable portion of the title bar on a connected display. Use
    // wide arithmetic for coordinates read from disk, including negative X/Y
    // on secondary displays. If the display disappeared, keep SDL's placement.
    const int64_t x = saved_->position.x;
    const int64_t y = int64_t(saved_->position.y) - top;
    if (x >= int64_t(bounds.x) - 64 && x + 64 <= int64_t(bounds.x) + bounds.w &&
        y >= bounds.y && y + 32 <= int64_t(bounds.y) + bounds.h) {
      visible = true;
      usable = bounds;
    }
  }
  SDL_free(displays);
  restoring_ = true;
  if (saved_->size && restore_size_) {
    // Clamp to [minimum, the usable area of the target display] (leaving room
    // for the title bar), the same rule as the SDK's SetInitialSize.
    if (!visible) {
      const SDL_DisplayID display = SDL_GetDisplayForWindow(window_);
      if (!display || !SDL_GetDisplayUsableBounds(display, &usable)) usable = {0, 0, kMaxSize, kMaxSize};
    }
    const int max_width = std::max(1, usable.w);
    const int max_height = std::max(1, usable.h - top);
    const int width = std::clamp(saved_->size->x, std::min(kMinWidth, max_width), max_width);
    const int height = std::clamp(saved_->size->y, std::min(kMinHeight, max_height), max_height);
    int current_width = 0, current_height = 0;
    SDL_GetWindowSize(window_, &current_width, &current_height);
    if ((width != current_width || height != current_height) &&
        SDL_SetWindowSize(window_, width, height)) {
      REXLOG_INFO("Restored window size: {}x{}{}", width, height,
                  attached_ || !early_restored_ ? "" : " (before first show)");
    }
  }
  if (visible) {
    if (SDL_SetWindowPosition(window_, saved_->position.x, saved_->position.y)) {
      REXLOG_INFO("Restored window position: {}, {}{}", saved_->position.x, saved_->position.y,
                  attached_ || !early_restored_ ? "" : " (before first show)");
    }
  } else {
    REXLOG_INFO("Saved window position {}, {} is not on a connected display; keeping default",
                saved_->position.x, saved_->position.y);
  }
  restoring_ = false;
}

bool WindowPosition::Windowed() const {
  if (!window_ ||
      (SDL_GetWindowFlags(window_) &
       (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED))) {
    return false;
  }
  // A native (Spaces) fullscreen entered with the title-bar button resizes the
  // window to the whole display before SDL sets SDL_WINDOW_FULLSCREEN; a
  // titled window can never cover the full display, so do not save that.
  int width = 0, height = 0;
  SDL_Rect bounds;
  const SDL_DisplayID display = SDL_GetDisplayForWindow(window_);
  if (SDL_GetWindowSize(window_, &width, &height) && display &&
      SDL_GetDisplayBounds(display, &bounds) && width >= bounds.w && height >= bounds.h) {
    return false;
  }
  return true;
}

void WindowPosition::Save(SDL_Point position, SDL_Point size) {
  if (size.x < 1 || size.y < 1 || size.x > kMaxSize || size.y > kMaxSize) return;
  if (saved_ && saved_->position.x == position.x && saved_->position.y == position.y &&
      saved_->size && saved_->size->x == size.x && saved_->size->y == size.y) return;
  // Save on movement rather than shutdown: the SDK hard-exits when the game
  // window closes. A unique temporary file preserves the last good position
  // if writing fails and keeps simultaneous launches from sharing a temp file.
  std::string temporary = path_.string() + ".tmp.XXXXXX";
  const int fd = mkstemp(temporary.data());
  bool success = false;
  if (fd >= 0) {
    if (FILE* file = fdopen(fd, "w")) {
      const bool written =
          std::fprintf(file,
                       "# Last windowed frame: client-area position (screen coordinates)\n"
                       "# and size (window coordinates, points on macOS).\n"
                       "x = %d\ny = %d\nwidth = %d\nheight = %d\n",
                       position.x, position.y, size.x, size.y) > 0;
      success = std::fclose(file) == 0 && written;
    } else {
      close(fd);
    }
    std::error_code error;
    if (success) {
      std::filesystem::rename(temporary, path_, error);
      success = !error;
    }
    if (!success) std::filesystem::remove(temporary, error);
  }
  if (success) {
    saved_ = SavedWindowFrame{position, size};
  } else if (!warned_) {
    warned_ = true;
    REXLOG_WARN("Could not save window position to {}", path_.string());
  }
}

bool SDLCALL WindowPosition::Watch(void* context, SDL_Event* event) {
  // SDL event watches may also be invoked by non-UI threads. Native window
  // moves are delivered on the UI thread; never call video APIs elsewhere.
  if (!SDL_IsMainThread()) return true;
  auto& self = *static_cast<WindowPosition*>(context);
  if (event->type == SDL_EVENT_WINDOW_SHOWN && self.armed_ && !self.early_bound_) {
    // Sent from inside SDL_ShowWindow, right after Cocoa orders the window in
    // and before control returns to the run loop, so the window is moved
    // before AppKit draws or displays its first frame there.
    // The first window shown while armed is the game window (bound by ID).
    SDL_Window* window = SDL_GetWindowFromID(event->window.windowID);
    if (window) {
      self.window_ = window;
      self.window_id_ = event->window.windowID;
      self.early_bound_ = true;
      if (!self.restore_on_show_) return true;  // placed by the SDK; Attach finishes
      self.early_restored_ = true;
      self.Load();
      // Fullscreen requested before showing is still pending here (SDL applies
      // it after this event), so check the cvar as well as the flags.
      if (!self.restore_) {
        self.saved_.reset();
      } else if (const auto fullscreen = rex::cvar::GetFlagByName("fullscreen");
                 fullscreen != "true" && fullscreen != "1") {
        self.Restore();
      }
    }
    return true;
  }
  if (event->type < SDL_EVENT_WINDOW_FIRST || event->type > SDL_EVENT_WINDOW_LAST ||
      event->window.windowID != self.window_id_) return true;
  if (event->type == SDL_EVENT_WINDOW_DESTROYED) {
    self.window_ = nullptr;
  } else if (event->type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) {
    self.Restore();
  } else if ((event->type == SDL_EVENT_WINDOW_MOVED || event->type == SDL_EVENT_WINDOW_RESIZED) &&
             self.attached_ && !self.restoring_ && self.Windowed()) {
    SDL_Point position{}, size{};
    if (event->type == SDL_EVENT_WINDOW_MOVED) {
      position = {event->window.data1, event->window.data2};
      if (!SDL_GetWindowSize(self.window_, &size.x, &size.y)) return true;
    } else {
      size = {event->window.data1, event->window.data2};
      if (!SDL_GetWindowPosition(self.window_, &position.x, &position.y)) return true;
    }
    self.Save(position, size);
  }
  return true;  // A watch observes events; it never consumes them.
}

}  // namespace fable2
