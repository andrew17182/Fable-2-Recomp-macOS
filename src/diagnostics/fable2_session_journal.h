#pragma once
// Input journal: opt-in recording and in-game replay of the guest pad (docs/reference.md "Input recording").
//
// Three ways it turns on (otherwise it is inert):
//   - Test sessions (FABLE2_SESSION_DIR, tools/fable2_test_control.py, fable2_dev_session.py --test-control):
//     state transitions always go to <dir>/events.jsonl; FABLE2_RECORD_INPUT=1 adds pad/key/speed events
//     until <dir>/stop-recording appears (then a recording_end).
//   - Player recording (FABLE2_RECORD_INPUT=1 without a session dir; launcher Diagnostics > Record input):
//     everything goes to FABLE2_RECORD_INPUT_PATH (a .jsonl file, or a folder), else
//     <state dir>/logs/recordings/, as "Fable II input YYYY-MM-DD HH-MM-SS.jsonl". Finish() (window close)
//     writes recording_end; every line is flushed, so a crash leaves a valid prefix.
//   - In-game replay (FABLE2_REPLAY_INPUT=<recording>, or <session dir>/replay-input.txt): the recorded pad
//     is injected at the guest's XamInputGetState boundary when the guest clock reaches each event. The live
//     pad never reaches the game while it runs (controller hotkeys still act: they are handled outside the
//     guest pad, src/input/controller_hotkeys.h). Esc ends it early; afterwards the live pad drives the game.
//     Without a session dir the status goes to <state dir>/logs/replay-status.json (launcher playback).
//
// Starting state (2026-10-22). A player recording copies the state folder's saves/ to "<recording stem>.save/"
// next to the file at record start (before the guest runs, so nothing is mid-write), and the AppLaunch meta
// says how the game started: "start":"save" (FABLE2_BOOT_SAVE, "boot_hero" = FABLE2_BOOT_SAVE_HERO) or
// "title", and "save_copy" (the folder name, empty when nothing was copied). Playback
// (tools/fable2_dev_session.py launch --mode replay) boots a session clone of that copy.
//
// Timing (schema 2). Every event carries host time (t_us, dt_us since its anchor) and guest time (g_us,
// gdt_us): the guest clock (rex::chrono::Clock::QueryGuestTickCount) runs at the turbo / slow-mode speed, so
// gdt_us is the same game moment at any speed. Anchors are reliable state transitions (UI fingerprints,
// named markers); replay matches them in recorded order and schedules each pad change at
// live anchor guest time + gdt_us. Speed changes (turbo, slow mode) are recorded as "speed" events, never replayed.
// Schema 1 recordings (host time only) replay with dt_us as guest time (exact at 1x only).
//
// Sticks (schema 3, 2026-10-22). A pad event's "pad" holds controller sticks as the controller reported
// them, before our deadzone / walk-deadzone remap (src/input/raw_pad_capture.h); "raw" says which sticks
// those are (bit 0 left, bit 1 right) and "game" (only when different) is what the game read. Replay
// remaps the "raw" sticks with the replaying session's settings (the same stick_deadzone.h functions
// BackFilterDriver applies to a live controller), so one recording replays under any deadzone. Keyboard
// pad and mouse-look sticks are not remapped live either and are recorded and replayed as the game read
// them. Buttons are always what the game read (hotkey-bound buttons removed). Schema 1/2 recordings hold
// the game-read (already remapped) sticks and replay them unchanged.
#include <algorithm>
#include <chrono>
#include <time.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <rex/chrono/clock.h>
#include <rex/logging/macros.h>
#include "fable2_guest_speed.h"
#include "fable2_state_path.h"
#include "stick_deadzone.h"
namespace fable2::session {
inline std::string Quote(const std::string& s) {
  std::string r="\"";
  for(unsigned char c:s) {
    if(c=='"'||c=='\\') {r+='\\';r+=c;}
    else if(c>=32) r+=c;
  }
  return r+'"';
}
inline int64_t NowUs() {
  timespec t{};
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec)*1000000 + t.tv_nsec/1000;
}
// Guest clock in microseconds since process start (the SDK guest tick count starts at 0).
inline int64_t GuestUs() {
  const uint64_t hz=rex::chrono::Clock::guest_tick_frequency();
  if(!hz) return 0;
  const unsigned __int128 ticks=rex::chrono::Clock::QueryGuestTickCount();
  return int64_t(ticks*1000000u/hz);
}
// Set by the app before the journal starts (FABLE2_VERSION_STRING); recorded in the AppLaunch metadata.
inline std::string& BuildVersion() {static std::string version;return version;}
inline bool EnvOn(const char* name) {
  const char* v=std::getenv(name);
  return v&&*v&&std::strcmp(v,"0")!=0;
}
struct Pad {
  int buttons=0,lt=0,rt=0,lx=0,ly=0,rx=0,ry=0;
  bool operator==(const Pad&) const=default;
  std::string Json() const {
    std::ostringstream s;
    s<<'['<<buttons<<','<<lt<<','<<rt<<','<<lx<<','<<ly<<','<<rx<<','<<ry<<']';
    return s.str();
  }
};
// Player recording file: FABLE2_RECORD_INPUT_PATH (file ending in .jsonl, else a folder), else
// <state dir>/logs/recordings. A new, unused name per launch.
inline std::filesystem::path RecordingPath() {
  namespace fs=std::filesystem;
  fs::path dir=fable2::StateDirectory()/"logs"/"recordings";
  if(const char* env=std::getenv("FABLE2_RECORD_INPUT_PATH");env&&*env) {
    fs::path p(env);
    if(p.extension()==".jsonl") return p;
    dir=p;
  }
  char stamp[64];
  const std::time_t now=std::time(nullptr);
  std::tm local{};
  localtime_r(&now,&local);
  std::strftime(stamp,sizeof(stamp),"Fable II input %Y-%m-%d %H-%M-%S",&local);
  fs::path path=dir/(std::string(stamp)+".jsonl");
  std::error_code ec;
  for(int i=2;fs::exists(path,ec)&&i<100;++i) path=dir/(std::string(stamp)+" "+std::to_string(i)+".jsonl");
  return path;
}
// Minimal readers for the journal's own one-line JSON events.
inline const char* JsonField(const std::string& line,const char* key) {
  const std::string k=std::string("\"")+key+"\":";
  auto at=line.find(k);
  return at==std::string::npos?nullptr:line.c_str()+at+k.size();
}
inline bool JsonInt(const std::string& line,const char* key,int64_t& out) {
  const char* p=JsonField(line,key);
  if(!p) return false;
  char* end=nullptr;
  out=std::strtoll(p,&end,10);
  return end!=p;
}
inline bool JsonString(const std::string& line,const char* key,std::string& out) {
  const char* p=JsonField(line,key);
  if(!p||*p!='"') return false;
  out.clear();
  for(++p;*p&&*p!='"';++p) {
    if(*p=='\\'&&p[1]) ++p;
    out+=*p;
  }
  return *p=='"';
}
// The live controller remap (BackFilterDriver, src/input/controller_hotkeys.h) applied to the sticks of
// `pad` named by `sticks` (bit 0 left, bit 1 right).
inline Pad RemapSticks(Pad pad,int sticks) {
  int16_t lx=int16_t(pad.lx),ly=int16_t(pad.ly),rx=int16_t(pad.rx),ry=int16_t(pad.ry);
  fable2::stickdz::ApplySession(lx,ly,rx,ry,sticks);
  pad.lx=lx;pad.ly=ly;pad.rx=rx;pad.ry=ry;
  return pad;
}
inline bool JsonPad(const std::string& line,Pad& pad) {
  const char* p=JsonField(line,"pad");
  if(!p||*p!='[') return false;
  int v[7];
  ++p;
  for(int i=0;i<7;++i) {
    char* end=nullptr;
    v[i]=int(std::strtol(p,&end,10));
    if(end==p) return false;
    p=end;
    if(*p==',') ++p;
  }
  pad=Pad{v[0],v[1],v[2],v[3],v[4],v[5],v[6]};
  return true;
}
// Copies the state folder's saves/ next to a player recording (see "Starting state" above). Returns the copy's
// folder name, empty when there was nothing to copy or the copy failed (the recording still works).
inline std::string CopyStartSaves(const std::filesystem::path& recording) {
  namespace fs=std::filesystem;
  std::error_code ec;
  const fs::path source=fable2::StateDirectory()/"saves";
  if(!fs::is_directory(source,ec)) return "";
  const fs::path dest=recording.parent_path()/(recording.stem().string()+".save");
  fs::remove_all(dest,ec);
  fs::copy(source,dest,fs::copy_options::recursive,ec);
  if(ec) {
    REXSYS_WARN("[record] cannot copy the starting saves to {}: {}", dest.string(), ec.message());
    fs::remove_all(dest,ec);
    return "";
  }
  REXSYS_INFO("[record] starting saves copied to {}", dest.string());
  return dest.filename().string();
}
class Journal {
 public:
  Journal() {
    const char* replay=std::getenv("FABLE2_REPLAY_INPUT");
    // Never record a replay (playback must not make a new recording of itself).
    const bool record=EnvOn("FABLE2_RECORD_INPUT")&&!(replay&&*replay);
    const char* dir=std::getenv("FABLE2_SESSION_DIR");
    if(dir&&*dir&&std::filesystem::path(dir).is_absolute()) folder_=dir;
    if(folder_.empty()&&!record&&!(replay&&*replay)) return;
    const char* origin=std::getenv("FABLE2_SESSION_ORIGIN_US");
    origin_=origin?std::strtoll(origin,nullptr,10):NowUs();
    std::error_code ec;
    if(!folder_.empty()) {
      std::filesystem::create_directories(folder_,ec);
      output_.open(folder_/"events.jsonl",std::ios::out|std::ios::trunc);
      recording_=record;
    } else if(record) {
      path_=RecordingPath();
      std::filesystem::create_directories(path_.parent_path(),ec);
      output_.open(path_,std::ios::out|std::ios::trunc);
      recording_=bool(output_);
      if(recording_) save_copy_=CopyStartSaves(path_);
      if(recording_) REXSYS_INFO("[record] recording input to {}", path_.string());
      else REXSYS_WARN("[record] cannot write {}; input is not recorded", path_.string());
    }
    if(replay&&*replay) {
      replay_path_=replay;
      LoadReplay(replay);
    }
    active_=true;
    live_.push_back({"AppLaunch",0});
    if(output_) {
      output_<<"{\"schema\":3,\"seq\":0,\"type\":\"state\",\"t_us\":0,\"state\":\"AppLaunch\",\"anchor\":0,"
             "\"dt_us\":0,\"g_us\":0,\"gdt_us\":0,\"meta\":"<<Meta()<<"}\n";
      output_.flush();
    }
    worker_=std::thread([this]{Worker();});
  }
  ~Journal() {stop_=true;if(worker_.joinable()) worker_.join();}
  bool Enabled() const {return active_;}
  // The player recording's file (empty in test sessions or when not recording).
  const std::filesystem::path& Path() const {return path_;}
  void Key(unsigned key,bool down,bool repeat=false) {
    if(!Enabled()) return;
    std::lock_guard lock(mu_);
    // Esc (rex::ui::VirtualKey::kEscape) ends an in-game replay; the live pad takes over once released.
    if(down&&!repeat&&key==0x1B&&game_replay_) {EndReplay("cancelled","\"error\":\"Esc\"");hold_live_=true;}
    if(recording_) Write("key","\"key\":"+std::to_string(key)+",\"down\":"+
        (down?"true":"false")+",\"repeat\":"+(repeat?"true":"false"));
  }
  // `pad`: what the game reads (replaced while replaying). `raw`: the same pad with the sticks named by
  // `raw_sticks` (bit 0 left, bit 1 right) as a controller reported them, before the deadzone remap.
  bool Input(Pad& pad,const Pad& raw,int raw_sticks) {
    if(!Enabled()) return false;
    std::lock_guard lock(mu_);
    if(!replay_.empty()) Advance(GuestUs());
    const bool injected=game_replay_||replaying_;
    Pad stored=raw;
    int sticks=raw_sticks;
    if(game_replay_) {
      if(raw!=Pad{}) ++live_held_;  // live input (pad, keyboard pad, test pad) the game does not get
      stored=game_pad_;sticks=game_sticks_;pad=RemapSticks(game_pad_,game_sticks_);
      // FABLE2_REPLAY_LOG=1: one line per change of what the game reads (tests; off by default).
      if(log_pads_&&pad!=logged_pad_) {logged_pad_=pad;REXSYS_INFO("[replay] game pad {}", pad.Json());}
    }
    else if(replaying_) {pad=stored=replay_pad_;sticks=0;}
    else if(hold_live_) {
      // After Esc ended a replay: keep the live pad from the game until it is idle (no button held), so
      // the Esc key's own Pause mapping does not reach the game.
      if(pad.buttons||pad.lt||pad.rt) {pad=Pad{};return true;}
      hold_live_=false;
    }
    // F9 overlay readout (stick_deadzone.h): the replayed left stick before the remap, -1 when not replaying.
    fable2::stickdz::g_left_replay_percent.store(
        injected?fable2::stickdz::RadialPercent(int16_t(stored.lx),int16_t(stored.ly)):-1,std::memory_order_relaxed);
    if(recording_&&(!have_pad_||stored!=previous_||sticks!=previous_sticks_)) {
      std::string fields="\"pad\":"+stored.Json()+",\"raw\":"+std::to_string(sticks);
      if(pad!=stored) fields+=",\"game\":"+pad.Json();
      Write("pad",fields);previous_=stored;previous_sticks_=sticks;have_pad_=true;
    }
    return injected;
  }
  bool Input(Pad& pad) {const Pad raw=pad;return Input(pad,raw,0);}
  // An in-game replay drives the pad (other guest users get no live input meanwhile, fable2_session_telemetry.h).
  bool Replaying() const {return replay_active_.load(std::memory_order_relaxed);}
  void Marker(const std::string& name) {
    if(!Enabled()) return;
    std::lock_guard lock(mu_);Transition(name,GuestUs());
  }
  void Signature(const std::string& name) {
    if(!Enabled()) return;
    std::lock_guard lock(mu_);
    signature_=name;last_ui_=NowUs();
  }
  // Normal quit (window close): end the recording so the file is complete.
  void Finish(const char* reason) {
    if(!Enabled()) return;
    std::lock_guard lock(mu_);
    if(recording_) {Write("recording_end","\"reason\":"+Quote(reason));recording_=false;}
  }
 private:
  struct Recorded {
    enum Kind {kState,kPad,kEnd} kind;
    int64_t seq=0,anchor=0,gdt=0;
    std::string state;
    Pad pad;
    int sticks=0;  // schema 3 "raw": sticks remapped on replay
  };
  std::string Meta() const {
    auto env=[](const char* name){const char* v=std::getenv(name);return Quote(v?v:"");};
    std::ostringstream s;
    s<<"{\"version\":"<<Quote(BuildVersion())<<",\"save\":"<<env("FABLE2_RECORD_INPUT_SAVE")
     <<",\"res_scale\":"<<env("FABLE2_RES_SCALE")<<",\"fps_60\":"<<env("FABLE2_FPS_60")
     <<",\"vsync\":"<<env("REX_VSYNC")<<",\"turbo_speed\":"<<env("FABLE2_TURBO_SPEED")<<",\"slow_speed\":"<<env("FABLE2_SLOW_SPEED")
     <<",\"boot_save\":"<<env("FABLE2_BOOT_SAVE")<<",\"instance\":"<<env("FABLE2_INSTANCE_ID")
     <<",\"start\":"<<Quote(EnvOn("FABLE2_BOOT_SAVE")?"save":"title")<<",\"boot_hero\":"<<env("FABLE2_BOOT_SAVE_HERO")
     <<",\"save_copy\":"<<Quote(save_copy_)<<'}';
    return s.str();
  }
  void Write(const char* type,const std::string& fields,int64_t guest=-1) {
    if(!output_) return;
    auto t=NowUs()-origin_;
    if(guest<0) guest=GuestUs();
    output_<<"{\"schema\":3,\"seq\":"<<++seq_<<",\"type\":"<<Quote(type)
      <<",\"t_us\":"<<t<<",\"anchor\":"<<anchor_<<",\"dt_us\":"
      <<t-anchor_time_<<",\"g_us\":"<<guest<<",\"gdt_us\":"<<guest-anchor_guest_<<','<<fields<<"}\n";output_.flush();
  }
  // `guest`: when the state began (a UI fingerprint is confirmed after it has been stable for a while).
  void Transition(const std::string& state,int64_t guest) {
    if(state==state_) return;
    state_=state;
    if(state!="Unknown") {
      anchor_=seq_+1;anchor_time_=NowUs()-origin_;anchor_guest_=guest;
      live_.push_back({state,guest});
      if(!replay_.empty()) Advance(GuestUs());
    }
    Write("state","\"state\":"+Quote(state),state!="Unknown"?guest:-1);
  }
  void LoadReplay(const std::filesystem::path& path) {
    replay_.clear();replay_i_=live_i_=0;rec_anchor_guest_.clear();matched_.clear();
    std::ifstream in(path);
    std::string line;
    int64_t schema=0;
    while(std::getline(in,line)) {
      std::string type;
      if(!JsonString(line,"type",type)) continue;  // a crash-truncated final line
      if(!schema) JsonInt(line,"schema",schema);
      Recorded e{Recorded::kState};
      if(!JsonInt(line,"seq",e.seq)||!JsonInt(line,"anchor",e.anchor)) continue;
      if(!JsonInt(line,"gdt_us",e.gdt)&&!JsonInt(line,"dt_us",e.gdt)) continue;  // schema 1: host time
      if(type=="state") {
        if(!JsonString(line,"state",e.state)||e.state=="Unknown") continue;
      } else if(type=="pad") {
        e.kind=Recorded::kPad;
        if(!JsonPad(line,e.pad)) continue;
        int64_t sticks=0;
        if(JsonInt(line,"raw",sticks)) e.sticks=int(sticks&3);
      } else if(type=="recording_end") {
        e.kind=Recorded::kEnd;
      } else continue;  // key (already in the pad), speed (the viewer picks the speed)
      replay_.push_back(e);
    }
    if(replay_.empty()||replay_[0].state!="AppLaunch") {
      replay_.clear();
      REXSYS_WARN("[replay] {} is not a recording (no AppLaunch anchor)", path.string());
      Status("failed","\"error\":\"not a recording (no AppLaunch anchor): "+path.filename().string()+"\"");
      return;
    }
    if(replay_.back().kind!=Recorded::kEnd) replay_.push_back(Recorded{Recorded::kEnd,0,replay_.back().anchor,
        replay_.back().gdt});
    const char* timeout=std::getenv("FABLE2_REPLAY_TIMEOUT_S");
    timeout_us_=int64_t((timeout&&*timeout?std::strtod(timeout,nullptr):60.0)*1e6);
    wait_since_=0;
    game_replay_=true;game_pad_=Pad{};game_sticks_=0;replay_active_=true;applied_=live_held_=0;
    log_pads_=EnvOn("FABLE2_REPLAY_LOG");logged_pad_=Pad{-1};
    REXSYS_INFO("[replay] in-game replay of {}: {} events", path.string(), replay_.size());
    if(schema<3)
      REXSYS_INFO("[replay] schema {} recording: its sticks are what the game read (after the deadzone settings "
                  "it was recorded with) and replay unchanged; record again for raw sticks", schema);
    else
      REXSYS_INFO("[replay] raw controller sticks are remapped with this session's settings: left {}, right {}, "
                  "walk: {}", fable2::stickdz::Describe(fable2::stickdz::Left()),
                  fable2::stickdz::Describe(fable2::stickdz::Right()),
                  fable2::stickdz::DescribeWalk(fable2::stickdz::Walk()));
    Status("running");
  }
  // Applies every recorded event that is due at guest time `now`.
  void Advance(int64_t now) {
    while(replay_i_<replay_.size()) {
      const auto& e=replay_[replay_i_];
      if(e.kind==Recorded::kState) {
        bool found=false;
        while(live_i_<live_.size()) {
          const auto& live=live_[live_i_++];
          if(live.first==e.state) {
            rec_anchor_guest_[e.seq]=live.second;found=true;
            matched_+=std::string(matched_.empty()?"":",")+"{\"state\":"+Quote(e.state)+",\"recorded_seq\":"+
                std::to_string(e.seq)+",\"live_guest_us\":"+std::to_string(live.second)+"}";
            break;
          }
        }
        if(!found) {
          if(!wait_since_) {wait_since_=NowUs();Status("waiting","\"expected\":"+Quote(e.state));}
          return;
        }
        if(wait_since_) Status("running");  // the awaited anchor came: the replay drives the pad again
        wait_since_=0;++replay_i_;
        continue;
      }
      auto anchor=rec_anchor_guest_.find(e.anchor);
      if(anchor==rec_anchor_guest_.end()) {EndReplay("failed","\"error\":\"input refers to a missing anchor\"");return;}
      if(now<anchor->second+e.gdt) return;
      if(e.kind==Recorded::kPad) {game_pad_=e.pad;game_sticks_=e.sticks;++replay_i_;++applied_;continue;}
      EndReplay("complete");
      return;
    }
  }
  void EndReplay(const char* status,const std::string& fields="") {
    replay_.clear();replay_i_=0;game_replay_=false;game_pad_=Pad{};game_sticks_=0;wait_since_=0;replay_active_=false;
    fable2::stickdz::g_left_replay_percent.store(-1,std::memory_order_relaxed);
    REXSYS_INFO("[replay] {}: {} pad changes applied; live input held back on {} polls", status, applied_, live_held_);
    Status(status,fields);
  }
  void Status(const char* status,const std::string& fields="") {
    std::error_code ec;
    // Test sessions: the session dir; launcher playback (no session dir): <state dir>/logs.
    const auto dir=folder_.empty()?fable2::StateDirectory()/"logs":folder_;
    std::filesystem::create_directories(dir,ec);
    const auto path=dir/"replay-status.json";
    {
      std::ofstream out(path.string()+".tmp",std::ios::trunc);
      out<<"{\"status\":"<<Quote(status)<<",\"driver\":\"game\",\"recording\":"<<Quote(replay_path_)
         <<",\"matched\":["<<matched_<<"]"<<(fields.empty()?"":",")<<fields<<"}\n";
    }
    std::filesystem::rename(path.string()+".tmp",path,ec);
  }
  void Worker() {
    double speed=1.0;
    while(!stop_) {
      {
        std::lock_guard lock(mu_);
        if(!folder_.empty()) {
          if(recording_&&std::filesystem::exists(folder_/"stop-recording")) {
            Write("recording_end","\"reason\":\"requested\"");recording_=false;
          }
          // In-game replay requested by tools/fable2_test_control.py after launch.
          std::error_code ec;
          if(replay_.empty()&&std::filesystem::exists(folder_/"replay-input.txt",ec)) {
            std::ifstream request(folder_/"replay-input.txt");
            std::string path;
            std::getline(request,path);
            std::filesystem::remove(folder_/"replay-input.txt",ec);
            if(!path.empty()) {replay_path_=path;LoadReplay(path);}
          }
          if(!replay_.empty()&&std::filesystem::exists(folder_/"cancel-replay",ec))
            EndReplay("failed","\"error\":\"Replay cancelled\"");
          // Host-clock replay (tools/fable2_replay.py --host-clock).
          std::ifstream replay(folder_/"replay-pad.txt");
          int active;Pad pad;int64_t lease;
          if(replay>>active>>lease>>pad.buttons>>pad.lt>>pad.rt>>pad.lx>>pad.ly>>pad.rx>>pad.ry) {
            replaying_=active&&NowUs()<lease;replay_pad_=pad;
          } else replaying_=false;
        }
        const double now_speed=fable2::guestspeed::Get();
        if(now_speed!=speed) {
          speed=now_speed;
          if(recording_) {
            char text[48];std::snprintf(text,sizeof(text),"\"speed\":%.2f",speed);
            Write("speed",text);
          }
        }
        // State fingerprints: stable for 150 ms of guest time, dated from when they first appeared, so
        // anchors land on the same game moment at any speed. The UI-gone timeout stays in host time.
        auto now=NowUs();
        auto guest=GuestUs();
        std::string candidate=now-last_ui_.load()<300000 ? signature_ : "Unknown";
        if(!candidate.empty()) {
          if(candidate!=pending_) {pending_=candidate;pending_since_=guest;}
          // Dated no earlier than the previous anchor: a marker (Marker()) may have come in between.
          else if(guest-pending_since_>=150000) Transition(candidate,std::max(pending_since_,anchor_guest_));
        } else pending_.clear();
        if(!replay_.empty()) {
          Advance(guest);
          if(!replay_.empty()&&wait_since_&&now-wait_since_>timeout_us_)
            EndReplay("failed","\"error\":"+Quote("No matching transition: "+replay_[replay_i_].state));
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  std::filesystem::path folder_,path_;
  std::ofstream output_;
  std::mutex mu_;
  std::thread worker_;
  std::atomic<bool> stop_{false};
  std::atomic<int64_t> last_ui_{0};
  bool active_=false,recording_=false,replaying_=false,have_pad_=false,game_replay_=false,hold_live_=false;
  std::atomic<bool> replay_active_{false};
  bool log_pads_=false;
  int64_t applied_=0,live_held_=0;
  Pad logged_pad_;
  std::string save_copy_,replay_path_;
  int64_t origin_=0,anchor_time_=0,anchor_guest_=0,pending_since_=0,wait_since_=0,timeout_us_=60000000;
  uint64_t seq_=0,anchor_=0;
  Pad previous_,replay_pad_,game_pad_;
  int previous_sticks_=0,game_sticks_=0;
  std::string state_="AppLaunch",pending_;
  std::string signature_;
  // In-game replay: recorded events, live anchors (state, guest us) and the matched pairs.
  std::vector<Recorded> replay_;
  size_t replay_i_=0,live_i_=0;
  std::vector<std::pair<std::string,int64_t>> live_;
  std::map<int64_t,int64_t> rec_anchor_guest_;
  std::string matched_;
};
inline Journal& Get() {static Journal journal;return journal;}
} // namespace fable2::session
