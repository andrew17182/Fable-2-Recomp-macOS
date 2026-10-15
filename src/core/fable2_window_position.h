#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <SDL3/SDL.h>

namespace fable2 {

// Last windowed frame read from fable2_window.toml: the client-area position
// (screen coordinates) and, when the file has it, the client size in window
// coordinates (points on macOS). Files from before size memory have only x/y.
struct SavedWindowFrame {
  SDL_Point position{};
  std::optional<SDL_Point> size;  // x = width, y = height
};

// Owns a watch on the game's SDL window. Arm, Attach and destruction run on
// the UI thread. Only windowed (not fullscreen, maximised or minimised) moves
// and resizes are saved; each save writes the whole frame (x, y, width, height).
//
// The SDK opens the window and shows it at SDL's default (centred) position
// before the app regains control. Arm registers the watch before that, so the
// saved frame is applied inside SDL_ShowWindow on SDL_EVENT_WINDOW_SHOWN, in
// the same main-thread turn that orders the window in. Without Arm, Attach
// restores after the window is already on screen (the old behaviour).
//
// A source-built SDK with Window::SetInitialPosition / SetInitialSize
// (REX_UI_WINDOW_INITIAL_POSITION / REX_UI_WINDOW_INITIAL_SIZE,
// patches/sdk/window-initial-position.patch) places and sizes the window while
// it is still hidden. Then the app calls LoadForSdk before the window opens and
// passes the result to the SDK; Arm is not used and Attach does not restore
// again. The SHOWN watch stays the fallback for the prebuilt SDK.
//
// Leaving fullscreen re-applies the saved windowed frame.
class WindowPosition {
 public:
  // Smallest restored client size (window coordinates); the SDK patch uses the same.
  static constexpr int kMinWidth = 640;
  static constexpr int kMinHeight = 360;
  // Largest size accepted from the file; anything above is ignored as corrupt.
  static constexpr int kMaxSize = 16384;

  WindowPosition() = default;
  ~WindowPosition();
  WindowPosition(const WindowPosition&) = delete;
  WindowPosition& operator=(const WindowPosition&) = delete;

  // Call before the window is created. The first window shown while armed is
  // the game window (the app has one SDL window); it is bound by its window ID,
  // not by title, so the title text can change freely. `restore` is false when
  // the monitor cvar wins; `restore_size` is false when the window_width /
  // window_height cvars win. `restore_on_show` false only binds the window
  // (the SDK placed it; LoadForSdk), so Attach needs no title search.
  bool Arm(const std::filesystem::path& path, bool restore = true, bool restore_size = true,
           bool restore_on_show = true);
  // Binds the window found by Arm or, without Arm, the window whose current
  // title equals `title` exactly.
  bool Attach(std::string_view title, const std::filesystem::path& path,
              bool restore = true, bool restore_size = true);
  // SDK placement: read the saved frame for Window::SetInitialPosition /
  // SetInitialSize. The SDK applies it (with the same on-display check and
  // size clamping) before showing the window, so the later Attach only
  // observes and saves moves and resizes.
  std::optional<SavedWindowFrame> LoadForSdk(const std::filesystem::path& path);

 private:
  static bool SDLCALL Watch(void* context, SDL_Event* event);
  void Load();
  void Restore();
  bool Windowed() const;
  void Save(SDL_Point position, SDL_Point size);

  SDL_Window* window_ = nullptr;
  SDL_WindowID window_id_ = 0;
  std::filesystem::path path_;
  std::optional<SavedWindowFrame> saved_;
  bool armed_ = false;
  bool restore_on_show_ = true;
  bool early_restored_ = false;
  bool restore_ = true;
  bool restore_size_ = true;
  bool early_bound_ = false;
  bool placed_by_sdk_ = false;
  bool attached_ = false;
  bool watching_ = false;
  bool restoring_ = false;
  bool warned_ = false;
};

}  // namespace fable2
