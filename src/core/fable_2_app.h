// fable_2 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>

#include <rex/perf/counter.h>
#include <rex/rex_app.h>
#include <rex/system.h>
#include <rex/system/gpu_plugin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif
#if defined(__APPLE__)
#include <dlfcn.h>
#endif

#include "alloc_watch.h"
#include "dir_manifest_heal.h"
#include "fable2_config.h"
#include "fable2_flight_recorder.h"  // telemetry phase 1 (FABLE2_TRACE)
#include "fable2_benchmark.h"  // built-in benchmark (FABLE2_BENCHMARK; docs/benchmark-kit.md)
#include "fable2_fps_auto.h"
#include "fable2_guest_barriers_flag.h"
#include "fable2_turbo.h"
#include "fable2_turbo_notice.h"
#include "fable2_fps_counter.h"
#include "fable2_version.h"  // generated: FABLE2_VERSION_STRING (tools/fable2_version.py)
#include "recomp_hotkeys.h"
#include "fable2_state_path.h"
#include "fable2_session_journal.h"
#include "fable2_patches.h"
#include "fable2_readback_config.h"
#include "fable2_graphics_config.h"
#include "fable2_window_title.h"
#ifdef __APPLE__
#include "fable2_window_position.h"
#endif
// #include "fable2_deadbeef_overlay.h"
// 30fps-cap instrumentation (writes fps_probe.log next to the exe). Disabled
// now that the cap is lifted via REX_VSYNC=0 (see tools/fable2-uncapped.cmd).
// Re-enable to re-measure the frame pacing:
// #include "fps_probe.h"
#include "keyboard_gamepad.h"
#include "controller_hotkeys.h"
#include <rex/input/device_assignment.h>
#include <rex/input/flags.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/input/sdl/sdl_input_driver.h>
#ifdef FABLE2_REMOTE_CONTROL
#include "remote_control_server.h"
#include "remote_gamepad_driver.h"
// 1-second game-state classifier (src/diagnostics/fable2_state_probe.h, single-TU inline).
// Forward-declared here; the inline definitions live in the state probe header
// (included once, in main.cpp), so the app can start/stop it alongside the
// remote control server without pulling the whole probe into this header.
namespace fable2::stateprobe {
void start();
void stop();
std::int64_t t_ms();
void record_a_press(std::int64_t ms);
}
#endif  // FABLE2_REMOTE_CONTROL
#include "xex_verify.h"

#ifdef FABLE2_CONTINUATION_NATIVE_TRACE
#include "continuation/native_indirect_trace_api.h"
#endif
REXCVAR_DECLARE(bool, audio_mute);  // SDK (src/audio/sdl/sdl_audio_driver.cpp)

class Fable2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;
  bool metal_backend_ = false;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Fable2App>(new Fable2App(ctx, "fable_2",
        PPCImageConfig));
  }

  // Window title: "Fable II Recomp 0.1.0 (c0b9d4a) · Metal · 2× (Auto) · 60 FPS (Auto)" (no SDK
  // build stamp). FABLE2_INSTANCE_ROLE (tools/fable2_dev_session.py, AppLauncher.m for the playtest
  // copy) tells side-by-side instances apart (local agent docs):
  //   playtest -> "Fable II Playtest 0.1.0 (c0b9d4a) · Metal · …"
  //   agent    -> "Fable II [AGENT TEST <FABLE2_INSTANCE_ID>] 0.1.0 (c0b9d4a) · Metal · … · test agent-2"
  // The options suffix (src/core/fable2_window_title.h) is computed once, after OnPreSetup resolved
  // the resolution scale (and with it the frame target): scale, frame target, MetalFX, turbo, and
  // the launcher's FABLE2_WINDOW_TAG.
  std::string TitleOptions() const {
    static const std::string suffix = [this] {
      fable2::window_title::Options o;
      if (const char* env = std::getenv("FABLE2_RES_SCALE"); env && *env) o.scale = std::atoi(env);
      if (o.scale < 1) o.scale = 1;
      o.scale_auto = fable2::res_auto::AutoPicked() > 0;
      // Use60() first: fps-auto may turn vsync off (the 2x rule's uncapped).
      const bool use60 = fable2::fps::Use60();
      o.fps = rex::cvar::GetFlagByName("vsync") == "false" ? 0 : (use60 ? 60 : 30);
      o.fps_auto = fable2::fps::PickedByAuto() && (o.fps > 0 || fable2::fps::AutoUncapped());
      const char* source = "";
      o.metalfx = metal_backend_ &&
                  fable2::graphics_config::ResolveUpscaling(fable2::config::Get(), &source, false) ==
                      fable2::config::Values::Upscaling::kMetalFxSpatial;
      o.turbo = fable2::turbo::EnvScale() && fable2::turbo::ConfiguredScale() > 1.0;
      if (const char* tag = std::getenv("FABLE2_WINDOW_TAG")) o.tag = tag;
      return fable2::window_title::Suffix(o);
    }();
    return suffix;
  }

  std::string WindowTitle() const {
    const char* role = std::getenv("FABLE2_INSTANCE_ROLE");
    const char* id = std::getenv("FABLE2_INSTANCE_ID");
    std::string name = "Fable II Recomp ";
    if (role && std::string_view(role) == "playtest") name = "Fable II Playtest ";
    else if (role && std::string_view(role) == "agent")
      name = std::string("Fable II [AGENT TEST") + (id && *id ? std::string(" ") + id : std::string()) + "] ";
    return name + FABLE2_VERSION_STRING + (metal_backend_ ? " \xC2\xB7 Metal" : " \xC2\xB7 Vulkan") +
           TitleOptions();
  }

#ifdef __APPLE__
  fable2::WindowPosition window_position_;

  bool sdk_window_position_ = false;
  bool restore_window_size_ = true;

  bool SetupPresentation() override {
    // The base call creates the window and shows it. Apply the saved frame
    // (position + size, fable2_window.toml) before it is visible (startup "jump"):
    // - source-built SDK (REX_UI_WINDOW_INITIAL_POSITION): OnPreOpenWindow hands
    //   it to Window::SetInitialPosition / SetInitialSize, applied while the
    //   window is hidden;
    // - prebuilt SDK, or FABLE2_SDK_WINDOW_POSITION=0: Arm the SHOWN watch, which
    //   applies it while SDL_ShowWindow runs.
    // FABLE2_EARLY_WINDOW_RESTORE=0 restores after showing, as before.
    const char* early = std::getenv("FABLE2_EARLY_WINDOW_RESTORE");
    const bool early_on = !(early && std::string_view(early) == "0");
    const bool restore = !rex::cvar::HasNonDefaultValue("monitor");
    // Explicit window_width / window_height cvars win over the saved size.
    restore_window_size_ = !rex::cvar::HasNonDefaultValue("window_width") &&
                           !rex::cvar::HasNonDefaultValue("window_height");
#ifdef REX_UI_WINDOW_INITIAL_POSITION
    const char* sdk = std::getenv("FABLE2_SDK_WINDOW_POSITION");
    sdk_window_position_ = early_on && restore && !(sdk && std::string_view(sdk) == "0");
#endif
    // The SHOWN watch binds the game window by ID (first window shown), whatever
    // its title; with SDK placement it only binds (the SDK already placed it).
    if (early_on) {
      window_position_.Arm(fable2::StateDirectory() / "fable2_window.toml", restore,
                           restore_window_size_, /*restore_on_show=*/!sdk_window_position_);
    }
    if (!rex::ReXApp::SetupPresentation()) return false;
    // The SDK titled the window "<name> <build stamp>"; with the window-placement
    // SDK patch OnPreOpenWindow already set ours before the window was created.
    if (window()->GetTitle() != WindowTitle()) window()->SetTitle(WindowTitle());
    REXLOG_INFO("Window title: {}", window()->GetTitle());
    if (!window_position_.Attach(
            window()->GetTitle(),
            fable2::StateDirectory() / "fable2_window.toml", restore, restore_window_size_)) {
      REXLOG_WARN("Could not enable window position saving");
    }
    return true;
  }

#ifdef REX_UI_WINDOW_INITIAL_POSITION
  void OnPreOpenWindow(rex::ui::Window* window) override {
    // Our title before the window exists (SDL creates it with this title).
    window->SetTitle(WindowTitle());
    if (!sdk_window_position_) return;
    // The monitor cvar takes priority inside the SDK. Under fullscreen the
    // frame becomes the windowed frame that leaving fullscreen returns to.
    if (const auto saved =
            window_position_.LoadForSdk(fable2::StateDirectory() / "fable2_window.toml")) {
      window->SetInitialPosition(saved->position.x, saved->position.y);
#ifdef REX_UI_WINDOW_INITIAL_SIZE
      if (saved->size && restore_window_size_) {
        window->SetInitialSize(saved->size->x, saved->size->y);
      }
#endif
    }
  }
#endif
#endif

  // Remote control (AI input channel) - see plans/ai-remote-input-control.md
  // and src/input/remote_control_server.h. The store is shared by the remote pad
  // driver (registered in OnPreSetup) and the command server (started in
  // OnPostInitLogging); the server publishes resolved snapshots and the
  // guest-side driver consumes the latest on each poll. DEBUG builds only
  // (gated on FABLE2_REMOTE_CONTROL; a hidden input channel must not ship in
  // Release builds).
#ifdef FABLE2_REMOTE_CONTROL
  std::shared_ptr<fable2::remote::InputStateStore> remote_state_ =
      std::make_shared<fable2::remote::InputStateStore>();
  std::atomic<bool> remote_pad_enabled_{true};
  fable2::remote::ControlServer remote_server_{remote_state_.get(),
                                               &remote_pad_enabled_};
#endif  // FABLE2_REMOTE_CONTROL

  // Emulate the Xbox 360 Xenos GPU. Two plugins are staged next to the exe:
  //   rexgpu-xenos[d].dll        -> D3D12 (prebuilt SDK plugin; the default)
  //   rexgpu-xenos-vulkan[d].dll -> Vulkan (built from the SDK source via
  //                                   tools/build_sdk_vulkan.cmd)
  // Swap the renderer at launch with --gpu_plugin, no rebuild required:
  //   --gpu_plugin=xenos            D3D12 (default)
  //   --gpu_plugin=xenos-vulkan     Vulkan
  void OnPreSetup(rex::RuntimeConfig& config) override {
    std::string plugin = config.gpu_plugin.empty() ? "xenos" : config.gpu_plugin;
    config.gpu_plugin = plugin;
    metal_backend_ = plugin == "metal";
    if (plugin == "xenos-vulkan") {
      // Load the source-built plugin and force the Vulkan backend. It is
      // compiled with both D3D12 and Vulkan; the default "any" would pick
      // D3D12, so pass "vulkan" explicitly to get the VulkanGraphicsSystem.
      config.graphics = rex::system::LoadGpuPlugin("xenos-vulkan", "vulkan");
    }
    // Otherwise leave config.graphics null so ReXApp loads
    // LoadGpuPlugin("xenos") -> the prebuilt D3D12 plugin.

    // Build on top of the default input system (SDL gamepad + NOP) and add a
    // synthetic "keyboard gamepad" driver so host keys can drive the guest.
    // The mapping is the `keyboard_gamepad_map` cvar (default "E:A"). See
    // src/input/keyboard_gamepad.h.
    config.input_factory = [this](bool tool_mode) ->
        std::unique_ptr<rex::system::IInputSystem> {
      // Same drivers as rex::input::CreateDefaultInputSystem (SDL + MnK + NOP,
      // slot assignment), except that the SDL gamepad driver is wrapped so the
      // View button no longer reaches the game as Back (it takes screenshots;
      // src/input/controller_hotkeys.h).
      using rex::X_STATUS;  // for X_STATUS_SUCCESS
      auto system = std::make_unique<rex::input::InputSystem>(nullptr);
      // Test-only instance (FABLE2_PHYSICAL_INPUT=0): the SDL pad driver
      // stays for the test channel's virtual pad (physical backends are off in SDL, see
      // test_background.h); the SDK mouse/keyboard driver is left out.
      const bool physical = fable2::testbg::physical_input();
      if (!tool_mode) {
        if (REXCVAR_GET(input_backend) == "sdl") {
          auto sdl_driver = std::make_unique<rex::input::sdl::SDLInputDriver>(nullptr, 0);
          if (sdl_driver->Setup() == X_STATUS_SUCCESS) {
            system->AddDriver(std::make_unique<fable2::padkeys::BackFilterDriver>(
                std::move(sdl_driver)));
          }
        }
        if (physical) {
          auto mnk_driver = std::make_unique<rex::input::mnk::MnkInputDriver>(nullptr, 0);
          if (mnk_driver->Setup() == X_STATUS_SUCCESS) system->AddDriver(std::move(mnk_driver));
        }
      }
      system->AddDriver(
          std::make_unique<rex::input::nop::NopInputDriver>(nullptr, tool_mode ? 0 : 1));
      system->SetDeviceAssignment(std::make_unique<rex::input::SlotAssignment>());
      system->AddDriver(
          std::make_unique<fable2::KeyboardGamepadDriver>(system->window(), 0));
#ifdef FABLE2_REMOTE_CONTROL
      // Remote (AI) pad: driven over localhost TCP by an external harness
      // (src/input/remote_control_server.h). OR-merges with the pads above; not
      // gated on window focus. Debug builds only.
      system->AddDriver(std::make_unique<fable2::remote::GamepadDriver>(
          system->window(), 0, remote_state_.get(), &remote_pad_enabled_));
#endif  // FABLE2_REMOTE_CONTROL
      return system;  // C++14 unique_ptr<Derived> -> unique_ptr<Base>
    };
  }

  void OnPostSetup() override {
    // Controller hotkeys: Share = quick save, View = screenshot
    // (src/input/controller_hotkeys.h). The SDL gamepad subsystem is up once
    // the input system is attached to the window.
    fable2::padkeys::SetDefaultScreenshotDir(game_data_root() / "Screenshots");
    // Background test launches pass --audio_mute=true (SDK cvar); log it so a
    // run shows whether it took effect.
    if (REXCVAR_GET(audio_mute)) REXLOG_INFO("Audio muted (audio_mute=true)");
    // Guest sync/lwsync fences (src/core/fable2_guest_memory.h); off only with
    // FABLE2_GUEST_BARRIERS=0.
    REXSYS_INFO("[guest-barriers] {}{}", fable2::guestmem::g_barriers ? "on" : "off (FABLE2_GUEST_BARRIERS=0)",
                fable2::guestmem::g_crash_diag ? "; crash-diag on" : "");
    REXSYS_INFO("[fctiw-rounding] {} (FABLE2_FCTIW_ROUNDING)",
                fable2::guestmem::g_fctiw_rounding
                    ? "on: fctiw/fctid round per the guest rounding mode (nearest-even by default)"
                    : "off: old round-half-away-from-zero conversion");
    // CR fixes 2026-10-07 (docs/cr-fixes-20261007.md): each on by default, =0 to disable.
    REXSYS_INFO("[fast-msr] {} (FABLE2_FAST_MSR)", fable2::guestmem::g_fast_msr
                    ? "on: lwarx/stwcx.-only interrupt-off sections skip the global mutex; mfmsr reads a per-thread depth"
                    : "off: every guest mfmsr/mtmsrd takes the global mutex");
    // Turbo (guest clock speed-up), configured by [patches] turbo_scale or
    // FABLE2_TURBO_SCALE; see src/core/fable2_turbo.h.
#ifdef __APPLE__
    fable2::guestclock::LogMode();
#endif
    fable2::turbo::Init();
    // Frame-rate mode (fps_60 true/false/auto); logs "[fps]"/"[fps-auto]" once.
    fable2::fps::Use60();
    fable2::fr::SetTag(fable2::fr::kTagFpsMode, fable2::fps::Use60() ? 60 : 30);
    fable2::fr::SetTag(fable2::fr::kTagTurbo, fable2::turbo::Active() ? 1 : 0);
    fable2::turbo_notice::Install(game_data_root(), [this] { return immediate_drawer(); });
    fable2::character_editor::SetGameDataRoot(game_data_root());  // opt-in mod (docs/character-editor.md)
    {
      // Skip intro (src/core/fable2_skip_intro.h): the effective decision, and whether the stand-in movies the
      // logo opens are redirected to exist. Missing stand-ins mean the full movies play (fresh clone, 2026-10-15;
      // tools/fable2_skip_intro.py makes them). Fast title (FABLE2_TITLE_FAST) follows skip intro.
      const char* skip = std::getenv("FABLE2_SKIP_INTRO_MOVIES");
      const bool on = skip && skip[0] == '1';
      int present = 0;
      std::error_code ec;
      for (const char* name : {"ms.bik", "lh.bik", "mw.bik"})
        if (std::filesystem::is_regular_file(game_data_root() / "out/skipintro" / name, ec)) ++present;
      if (!on)
        REXSYS_INFO("[skip-intro] off: the logo movies play (FABLE2_SKIP_INTRO_MOVIES={})", skip ? skip : "unset");
      else if (present == 3)
        REXSYS_INFO("[skip-intro] on: logo movies redirected to {}/out/skipintro (3/3 stand-ins)",
                    game_data_root().string());
      else
        REXSYS_WARN("[skip-intro] on, but only {}/3 stand-in movies in {}/out/skipintro: the missing ones play in "
                    "full (python3 tools/fable2_skip_intro.py ensure)", present, game_data_root().string());
    }
#ifdef __APPLE__
    // FPS counter (src/core/fable2_fps_counter.h): F9 toggles; start state from
    // [graphics] fps_counter / FABLE2_FPS_COUNTER.
    fable2::hotkey_hooks::g_toggle_fps = &fable2::fps_counter::Toggle;
    if (fable2::fps_counter::ConfiguredOn(fable2::config::Get()) && window()) {
      window()->app_context().CallInUIThreadDeferred([] { fable2::fps_counter::SetVisible(true); });
    }
#endif
    if (window()) {
      auto* context = &window()->app_context();
      context->CallInUIThreadDeferred([context] { fable2::padkeys::Install(context); });
    }
    // Built-in benchmark (FABLE2_BENCHMARK=<result.json>): headings8 after boot-save, then quit the way
    // the window close button does (an SDL quit event on the UI thread).
    if (window() && fable2::bench::Enabled()) {
      auto* context = &window()->app_context();
      fable2::bench::Start([context] {
        context->CallInUIThreadDeferred([] { SDL_Event event{}; event.type = SDL_EVENT_QUIT; SDL_PushEvent(&event); });
      });
    }

    // Hero/dog black-texture fix (see plans/hero-dog-texture-readback.md).
    // Approach + guest base 0x12704000 credit just-harry's Unofficial Xenia
    // femtofork for Fable II. readback_resolve_force_addresses is defined in the
    // GPU plugin (loaded in OnPreSetup, i.e. after OnPostInitLogging), so it is
    // seeded here, now that the plugin is loaded and its cvar is registered.
    // Force a CPU readback of the render-to-texture resolve that regenerates the
    // hero/dog face+skin texture; the address is Fable-II-specific, so it lives in
    // the app and the SDK only knows "read back resolves to these addresses".
    {
      const fable2::config::Values& cfg = fable2::config::Get();
      if (!fable2::SeedHeroDogReadback(metal_backend_, cfg.hero_dog_texture_readback)) {
        REXSYS_WARN("[fable2-config] graphics plugin does not support the "
                    "hero/dog readback setting; the fix may be inactive");
      }
      // [graphics] upscaling -> metal_presenter_use_metalfx (plugin cvar).
      fable2::graphics_config::SeedMetalFx(metal_backend_, cfg);
      // [graphics] effects_native_scale -> draw_resolution_scaled_texture_offsets.
      fable2::graphics_config::SeedEffectsNative(metal_backend_, cfg);
    }

    // Feed the F3 debug overlay with guest FPS (below). The GPU plugin
    // records per-guest-swap frame timing into the shared perf registry
    // (rex::perf, state lives in rexruntime.dll and is shared with the
    // plugin); read the last snapshot and expose it as FrameStats so the
    // overlay's "Guest: X FPS (Y ms)" line works on every runtime build.

#ifndef __APPLE__
    // The watcher below requires Windows memory queries or Linux /proc and
    // assumes the old guest base. On macOS it cannot sample the arena and
    // otherwise wakes every 5 ms forever without gathering useful data.
    // Background monitor for the table-zeroing / init-ordering bug. Runs on a
    // SEPARATE OS thread with hardcoded stable addresses (no recompiled probe
    // dependency), so it does not perturb the recompiled hot path that the
    // race depends on. It samples the allocator table continuously from t=0
    // and distinguishes:
    //   (A) table valid then zeroed  -> "HEAD WENT NULL" after "FIRST VALID"
    //   (B) never valid (consumer first) -> only "null" ever, no "FIRST VALID"
    std::thread([]() {
      using namespace fable2::allocwatch;
      const uintptr_t base = kGuestBase;  // 0x100000000, stable
      auto t0 = std::chrono::steady_clock::now();
      auto ms = [&] { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(); };
      // The guest arena is commit-on-fault; a page may be uncommitted when we
      // sample early. Use VirtualQuery to only read committed pages, so an
      // early sample is skipped instead of faulting the monitor thread.
      // (Linux/Android: the analogue is a /proc/self/maps scan; PROT_NONE
      // arena pages show up in maps but are not safe to read, so require
      // r+w permissions, refreshed periodically.)
      auto committed = [](const volatile void* p) {
#ifdef _WIN32
        MEMORY_BASIC_INFORMATION mbi{};
        if (::VirtualQuery(const_cast<void*>(const_cast<const void*>(p)), &mbi, sizeof(mbi)) == 0) return false;
        return mbi.State == MEM_COMMIT &&
               (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY)) != 0;
#else
        static std::vector<std::pair<uintptr_t, uintptr_t>> rw_ranges;
        static std::chrono::steady_clock::time_point last_refresh{};
        auto now = std::chrono::steady_clock::now();
        if (now - last_refresh > std::chrono::milliseconds(500)) {
          last_refresh = now;
          rw_ranges.clear();
          std::ifstream maps("/proc/self/maps");
          std::string line;
          while (std::getline(maps, line)) {
            uintptr_t lo = 0, hi = 0;
            char dash = 0;
            std::istringstream iss(line);
            if (!(iss >> std::hex >> lo >> dash >> hi)) continue;
            std::string perms;
            iss >> perms;
            if (perms.find('r') != std::string::npos &&
                perms.find('w') != std::string::npos) {
              rw_ranges.emplace_back(lo, hi);
            }
          }
        }
        const auto a = reinterpret_cast<uintptr_t>(p);
        for (const auto& r : rw_ranges) {
          if (a >= r.first && a < r.second) return true;
        }
        return false;
#endif
      };
      auto safe_sample = [&](uint32_t& tbl, uint32_t& flag, uint32_t& head) -> bool {
        tbl = flag = head = 0;
        if (!committed(GuestPtr(base, kTablePtrGuest))) return false;
        tbl = *GuestPtr(base, kTablePtrGuest);
        flag = *GuestByte(base, kFlagGuestAddr);
        if (tbl != 0) {
          if (!committed(GuestPtr(base, tbl + 12))) return false;
          head = *GuestPtr(base, tbl + 12);
        }
        return true;
      };
      bool first_valid = false;
      bool ever_valid = false;
      int null_samples = 0;
      while (true) {
        uint32_t tbl, flag, head;
        if (!safe_sample(tbl, flag, head)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
          continue;  // page not committed yet
        }
        if (tbl != 0 && head != 0 && !first_valid) {
          first_valid = true; ever_valid = true;
          REXSYS_ERROR("[alloc-watch] FIRST VALID head: t={}ms tbl@83496BD4=0x{:08X} head=0x{:08X} flag=0x{:02X}",
                       ms(), tbl, head, flag);
        }
        if (tbl != 0 && head != 0) ever_valid = true;
        // Log the transition valid->null (the zeroing) with full context.
        static uint32_t s_prev_head = 0;
        static bool s_have_prev = false;
        if (tbl != 0) {
          if (s_have_prev && s_prev_head != 0 && head == 0) {
            uint32_t ev = null_samples++;
            if (ev < 6) {
              REXSYS_ERROR("[alloc-watch] HEAD WENT NULL: t={}ms tbl=0x{:08X} prevHead=0x{:08X} flag=0x{:02X} ever_valid={} ; dump:",
                           ms(), tbl, s_prev_head, flag, ever_valid);
              for (int o = 0; o < 20; o += 4)
                REXSYS_ERROR("[alloc-watch]   table+{:2d}=0x{:08X}", o, *GuestPtr(base, tbl + o));
              for (uint32_t a = 0x83496BC0; a < 0x83496BF0; a += 4)
                REXSYS_ERROR("[alloc-watch]   0x{:08X}=0x{:08X}", a, *GuestPtr(base, a));
            }
          }
          s_prev_head = head; s_have_prev = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }).detach();
#endif

    SetGuestFrameStats([]() {
      rex::ui::FrameStats stats;
      auto ft_us =
          rex::perf::GetSnapshotCounter(rex::perf::CounterId::kFrameTimeUs);
      if (ft_us > 0) {
        stats.frame_time_ms = double(ft_us) / 1000.0;
        stats.fps = 1000000.0 / double(ft_us);
        stats.frame_count = 1;  // non-zero = "has data" (gates overlay text)
      }
      return stats;
    });
  }

  // Load the recomp config from the state directory (Application Support for
  // the Mac app, beside the executable for command builds),
  // creating it with defaults on first launch. Runs after the SDK's logging
  // init so load/create/parse problems land in logs/ (see src/core/fable2_config.h).
  // Plain settings are read via fable2::config::Get(); settings that back a
  // cvar are seeded into it below so the console/overlay keep working.
  void OnPostInitLogging() override {
    REXSYS_INFO("Fable II Recomp {}", FABLE2_VERSION_STRING);
    // Flight recorder (src/diagnostics/fable2_flight_recorder.h): parse FABLE2_TRACE (and the old
    // trace switches as aliases) before any guest thread starts, create <log folder>/trace/, add
    // the log sink (digest + first-AV / GPU-timeout / persistent-miss triggers).
    fable2::fr::Init();
    fable2::fr::InstallLogSink();
    // Start before the window/guest movies: raw inputs and launch anchor are retained.
    fable2::session::Get();
    const auto state_dir = fable2::StateDirectory();
    fable2::config::Load(state_dir / "fable2_config.toml");

    // Seed cvars from the config. Only when the cvar is still at its
    // compiled default (source kDefault), so higher-priority sources --
    // fable_2.toml (kConfig), REX_* env vars (kEnvironment), and the command
    // line (kCommandLine) -- keep winning over the recomp config. The F3
    // console can still change the value live afterwards (the driver
    // hot-reloads on text change, see src/input/keyboard_gamepad.h).
    const auto seed_cvar = [](std::string_view cvar, std::string_view value) {
      if (rex::cvar::GetFlagSource(cvar) != rex::cvar::Source::kDefault) return;
      if (!rex::cvar::SetFlagByName(cvar, value)) {
        REXSYS_WARN(
            "[fable2-config] cvar {} rejected config value '{}'; keeping "
            "built-in default",
            cvar, value);
        return;
      }
      REXSYS_INFO("[fable2-config] seeded cvar {} from fable2_config.toml",
                  cvar);
    };
#ifdef __APPLE__
    // clock_no_scaling=true was a launcher workaround for the SDK's guest
    // clock drift, now fixed by src/core/fable2_guest_clock.h. It also blocks
    // turbo (set_guest_time_scalar returns early). Old fable_2.toml files and
    // REX_CLOCK_NO_SCALING still carry it, so only an explicit command-line
    // --clock_no_scaling=true is honoured. Runs before the guest starts.
    if (REXCVAR_GET(clock_no_scaling) &&
        rex::cvar::GetFlagSource("clock_no_scaling") != rex::cvar::Source::kCommandLine) {
      rex::cvar::SetFlagByName("clock_no_scaling", "false");
      REXSYS_INFO(
          "[clock] ignoring obsolete clock_no_scaling=true from fable_2.toml/environment "
          "(guest clock drift is fixed; pass --clock_no_scaling=true to force host time)");
    }
#endif
    const fable2::config::Values& cfg = fable2::config::Get();
    seed_cvar("keyboard_gamepad_map", cfg.keyboard_gamepad_map);
    seed_cvar("mouse_look", cfg.mouse_look ? "true" : "false");
    seed_cvar("mouse_look_scale", std::to_string(cfg.mouse_look_scale));
    // [graphics] resolution_scale -> FABLE2_RES_SCALE (read by the Metal
    // plugin when its command processor starts, after this).
    fable2::graphics_config::ApplyResolutionScale(cfg);
    // [graphics] shader_cache_target -> FABLE2_MSC_MIN_TARGET (read by the
    // Metal plugin when its pipeline cache initializes, after this).
    fable2::graphics_config::ApplyShaderCacheTarget(cfg);
    // [graphics] fullscreen / FABLE2_FULLSCREEN -> the SDK fullscreen cvar,
    // read when the window is created (SetupPresentation, after this).
    fable2::graphics_config::SeedFullscreen(cfg);
    // NOTE: the hero/dog readback fix (readback_resolve_force_addresses) is a
    // GPU-PLUGIN cvar, so it is seeded in OnPostSetup() (after the plugin is
    // loaded) rather than here - see plans/hero-dog-texture-readback.md.

#ifdef FABLE2_REMOTE_CONTROL
    // Start the remote control server (AI input channel; debug-only, no
    // config knobs - always on with ControlServer::Config defaults: 127.0.0.1:
    // 8791, no auth). Debug builds only (FABLE2_REMOTE_CONTROL); Release
    // builds never open the port.
    if (!remote_server_.Start({})) {
      REXSYS_WARN(
          "[fable2-config] remote control server could not start; remote "
          "input disabled (see logs/)");
    }

    // Observe remote A-presses (the state machine's input transition). The
    // press is brief, so latch its timestamp here (on the server thread) and
    // compare against it in the 1-second classifier sample.
    remote_state_->SetOnPublish([](const fable2::remote::InputSnapshot& s) {
      if ((s.buttons & 0x1000u) != 0)  // X_INPUT_GAMEPAD_A
        fable2::stateprobe::record_a_press(fable2::stateprobe::t_ms());
    });
    // Start the 1-second game-state classifier (feeds the remote `state`
    // command + the [state] in-game log). The state is always tracked in
    // debug builds.
    fable2::stateprobe::start();
#endif  // FABLE2_REMOTE_CONTROL
  }

#ifdef FABLE2_REMOTE_CONTROL
  // Stop the state classifier thread, then the remote control server threads,
  // before anything else tears down.
  void OnShutdown() override {
    fable2::stateprobe::stop();
    remote_server_.Stop();
  }
#endif  // FABLE2_REMOTE_CONTROL
#ifdef FABLE2_CONTINUATION_NATIVE_TRACE
  void OnPreLaunchModule() override {
    // SDK has registered the mappings; no title guest thread has started.
    // Failure rolls back wrappers and is reported by the native trace gate.
    fable2::continuation::native_trace::Install(*runtime());
  }
#endif
  // Apply the game patches (see src/core/fable2_patches.h) once the SDK has
  // decrypted default.xex into the guest arena, before the module launches.
  // The patch table is data-driven: fable2_patches.toml in the state directory
  // (created with built-in defaults on first launch; a broken file falls
  // back to the built-ins, so a hand edit can never wedge the launch).
  void OnPostLoadXexImage() override {
    const auto state_dir = fable2::StateDirectory();
    fable2::patches::Load(state_dir / "fable2_patches.toml");
    fable2::patches::ApplyAll(runtime()->memory(), PPCImageConfig);
  }

  // Startup integrity check: this build was recompiled against a specific
  // default.xex, so verify its SHA-256 before the runtime loads it (see
  // src/core/xex_verify.h). A previously verified file is skipped via the
  // cache/default.xex.sha256 marker; a mismatch aborts with a dialog that
  // shows how to check the hash yourself.
  void OnLoadXexImage(std::string& xex_image) override {
    // Self-heal the guest VFS index before the title runs: the game indexes
    // all of data/ from data/dir.manifest, and a missing entry means the
    // guest sees null (crashes at startup). Content migrated with an
    // mtime-respecting copy can leave a stale/truncated manifest in place;
    // this appends any file that is on disk but unlisted. Idempotent.
    fable2::manifestheal::EnsureComplete(game_data_root());
    // [bank-prep] The headless shader prep reads the disc's shader bank from here (Metal plugin,
    // docs/shader-precompile-20261015.md).
    if (const char* root = std::getenv("FABLE2_GAME_DATA_ROOT"); !root || !*root) {  // getenv-ok: once
      setenv("FABLE2_GAME_DATA_ROOT", game_data_root().string().c_str(), 1);
    }

    // Resolve the host path the same way the SDK does (game:\ / d:\ ->
    // game_data_root). The SDK's path may use either slash direction
    // (e.g. "game:\\default.xex" or "game:/default.xex"), so strip the
    // device prefix and any following separator generically.
    std::string_view tail = xex_image;
    if (tail.starts_with("game:")) tail.remove_prefix(5);
    else if (tail.starts_with("d:")) tail.remove_prefix(2);
    if (!tail.empty() && (tail.front() == '\\' || tail.front() == '/'))
      tail.remove_prefix(1);
    std::string host_tail{tail};
    std::replace(host_tail.begin(), host_tail.end(), '\\', '/');
    const std::filesystem::path xex = game_data_root() / host_tail;
    std::filesystem::path cache = cache_root();
    if (cache.empty()) cache = game_data_root() / "cache";
    const std::filesystem::path marker = cache / "default.xex.sha256";

    REXSYS_INFO("[xex-verify] {}", xex.string());
    REXSYS_INFO("[xex-verify] expected SHA-256: {}", fable2::xexverify::kExpectedSha256);
    const auto r = fable2::xexverify::Check(xex, marker);
    switch (r.result) {
      case fable2::xexverify::Result::VerifiedCached:
        REXSYS_INFO("[xex-verify] actual SHA-256: {} (verified on a previous start; "
                    "size/mtime unchanged, hash pass skipped)",
                    r.actual_hash);
        break;
      case fable2::xexverify::Result::VerifiedFresh:
        REXSYS_INFO("[xex-verify] actual SHA-256: {} (MATCH; recorded in {} so "
                    "future starts skip the check)",
                    r.actual_hash, marker.string());
        break;
      case fable2::xexverify::Result::Mismatch: {
        // FABLE2_XEX_MISMATCH_OK=1: the player chose "Continue anyway" for a
        // different disc image (portable build, docs/portable-build.md).
        if (const char* ok = std::getenv("FABLE2_XEX_MISMATCH_OK"); ok && ok[0] == '1') {
          REXSYS_WARN("[xex-verify] actual SHA-256: {} differs from expected {}; "
                      "continuing (FABLE2_XEX_MISMATCH_OK=1)",
                      r.actual_hash, fable2::xexverify::kExpectedSha256);
          break;
        }
        REXSYS_ERROR("[xex-verify] actual SHA-256: {}", r.actual_hash);
        REXSYS_ERROR("[xex-verify] expected SHA-256: {} (MISMATCH)",
                     fable2::xexverify::kExpectedSha256);
        const std::string msg = std::format(
            "default.xex hash mismatch\n\n"
            "This build was recompiled against a specific default.xex, but\n"
            "the file found here has a different SHA-256 hash, so the game\n"
            "may not run correctly.\n\n"
            "File:     {}\n"
            "Actual:   {}\n"
            "Expected: {}\n\n"
            "You can check the hash yourself in a Windows terminal:\n"
            "  certutil -hashfile \"{}\" SHA256\n"
            "(PowerShell: Get-FileHash \"{}\" -Algorithm SHA256)\n\n"
            "If you have the correct default.xex, replace the one above and\n"
            "start the game again.",
            xex.string(), r.actual_hash, fable2::xexverify::kExpectedSha256,
            xex.string(), xex.string());
        REXSYS_ERROR("[xex-verify] {}", msg);
        rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
        std::exit(1);
      }
      case fable2::xexverify::Result::ReadFailed:
      default:
        REXSYS_WARN("[xex-verify] could not hash {} (read error); "
                   "skipping the integrity check", xex.string());
        break;
    }
  }

  // Self-contained layout: everything lives next to the executable.
  //   <exe>/               <- content root (default.xex, data/, nxeart/,
  //                           $SystemUpdate/; staged next to the exe by the
  //                           build, see CMakeLists.txt)
  //   <exe>/saves/         <- user data: save files, settings, profiles
  //   <exe>/cache/         <- runtime caches (shader cache, etc.)
  //   <exe>/fable_2.toml   <- cvar config (SDK default)
  //   <exe>/logs/          <- game logs (SDK default)
  // The content root is the exe's own directory ONLY (no ancestor walk), so
  // the whole build folder is portable as-is: copy it anywhere and it runs.
  // Override the content location with --game_data_root=<path>.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    const std::filesystem::path exe_dir =
        rex::filesystem::GetExecutableFolder();

    if (paths.game_data_root.empty()) {
      if (std::filesystem::is_regular_file(exe_dir / "default.xex")) {
        paths.game_data_root = exe_dir;
      } else {
        // The SDK's fallback for an empty game_data_root is the cryptic
        // "--game_data_root was not provided" dialog; in this
        // self-contained layout the real problem is the missing content,
        // so say that and stop before the SDK's dialog can show.
        // (Logging is not initialized yet at this hook, dialog only.)
        const std::string msg = std::format(
            "default.xex was not found next to fable_2.exe ({}).\n\n"
            "Stage the game content there (the build does this "
            "automatically) or pass --game_data_root=<path>.",
            exe_dir.string());
        rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
        std::exit(1);
      }
    }

    // The game content itself must actually be present too (covers both the
    // exe-dir default and a --game_data_root override): without data/ the
    // guest fails deep inside content loading with cryptic errors, so fail
    // fast with the real problem.
    if (!paths.game_data_root.empty() &&
        !std::filesystem::is_directory(paths.game_data_root / "data")) {
      const std::string msg = std::format(
          "The data folder was not found in {}.",
          paths.game_data_root.string());
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
      std::exit(1);
    }

    // Command builds keep state beside the exe; the macOS app launcher
    // supplies a persistent external state directory. Both use saves/ below it.
    // The SDK's default user dir is a per-user platform location, which
    // caused save corruption. NOTE: set unconditionally because the SDK
    // defaults are non-empty, so empty-checks would never trigger (this
    // also overrides the cvar/CLI defaults).
    std::error_code ec;
    const auto state_dir = fable2::StateDirectory();
    paths.user_data_root = state_dir / "saves";
    std::filesystem::create_directories(paths.user_data_root, ec);
    paths.cache_root = state_dir / "cache";
    std::filesystem::create_directories(paths.cache_root, ec);

    if (state_dir != exe_dir) {
      paths.config_path = state_dir / "fable_2.toml";
      paths.metadata_root = state_dir / "metadata";
      std::filesystem::create_directories(paths.metadata_root, ec);
    }

    // Mount the $SystemUpdate folder (inside the content root) as update:\
    // so VdSetGraphicsInterruptCallback-era update partition lookups resolve
    // instead of failing with "device not found".
    if (paths.update_data_root.empty() && !paths.game_data_root.empty()) {
      auto update_dir = paths.game_data_root / "$SystemUpdate";
      if (std::filesystem::is_directory(update_dir)) {
        paths.update_data_root = update_dir;
      }
    }
  }
  // Flight recorder: one "exit" dump (rings, stacks, digest) when the window is closed; the SDK
  // hard-exits afterwards. Focus/minimise feed the "focus" tag (frame-gap/hang triggers are
  // suppressed while a normal session is in the background; background test mode keeps them).
  bool OnWindowCloseRequested() override {
#if defined(__APPLE__)
    // Save the Metal binary archives first (pipelines added since the last periodic save, e.g. the
    // specialized ones: FABLE2_NS2_ARCHIVE, docs/native-shaders.md). The plugin exports it.
    using ArchiveFlushFn = void (*)();
    static const auto archive_flush =
        reinterpret_cast<ArchiveFlushFn>(dlsym(RTLD_DEFAULT, "fable2_pipeline_archive_flush"));
    if (archive_flush) archive_flush();
#endif
    fable2::fr::OnCloseRequested("window close requested");
    return true;
  }
  void OnWindowFocusChanged(bool focused) override {
    if (fable2::fr::GetTag(fable2::fr::kTagFocus) >= 0) fable2::fr::SetTag(fable2::fr::kTagFocus, focused ? 1 : 0);
  }
  void OnWindowMinimized() override { fable2::fr::SetTag(fable2::fr::kTagFocus, -1); }
  void OnWindowRestored() override { fable2::fr::SetTag(fable2::fr::kTagFocus, 1); }
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    fable2::ApplyRecompHotkeyDefaults();
    // fable2::deadbeef_overlay::register_overlay(drawer);
  }
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
