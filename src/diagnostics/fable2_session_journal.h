#pragma once
// Opt-in app-owned recording. Inert outside test sessions.
#include <chrono>
#include <time.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <atomic>
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
struct Pad {
  int buttons=0,lt=0,rt=0,lx=0,ly=0,rx=0,ry=0;
  bool operator==(const Pad&) const=default;
  std::string Json() const {
    std::ostringstream s;
    s<<'['<<buttons<<','<<lt<<','<<rt<<','<<lx<<','<<ly<<','<<rx<<','<<ry<<']';
    return s.str();
  }
};
class Journal {
 public:
  Journal() {
    const char* dir=std::getenv("FABLE2_SESSION_DIR");
    if(!dir||!*dir) return;
    folder_=dir;
    if(!folder_.is_absolute()) {folder_.clear();return;}
    const char* origin=std::getenv("FABLE2_SESSION_ORIGIN_US");
    origin_=origin?std::strtoll(origin,nullptr,10):NowUs();
    recording_=std::getenv("FABLE2_RECORD_INPUT")!=nullptr;
    std::filesystem::create_directories(folder_);
    output_.open(folder_/"events.jsonl",std::ios::out|std::ios::trunc);
    if(!output_) {folder_.clear();return;}
    output_<<"{\"schema\":1,\"seq\":0,\"type\":\"state\",\"t_us\":0,\"state\":\"AppLaunch\",\"anchor\":0,\"dt_us\":0}\n";
    output_.flush();
    worker_=std::thread([this]{Worker();});
  }
  ~Journal() {stop_=true;if(worker_.joinable()) worker_.join();}
  bool Enabled() const {return !folder_.empty();}
  void Key(unsigned key,bool down,bool repeat=false) {
    if(!Enabled()) return;
    std::lock_guard lock(mu_);
    if(recording_) Write("key","\"key\":"+std::to_string(key)+",\"down\":"+
        (down?"true":"false")+",\"repeat\":"+(repeat?"true":"false"));
  }
  bool Input(Pad& pad) {
    if(!Enabled()) return false;
    std::lock_guard lock(mu_);
    if(replaying_) pad=replay_pad_;
    if(recording_&&(!have_pad_||pad!=previous_)) {
      Write("pad","\"pad\":"+pad.Json());previous_=pad;have_pad_=true;
    }
    return replaying_;
  }
  void Marker(const std::string& name) {
    if(!Enabled()) return;
    std::lock_guard lock(mu_);Transition(name);
  }
  void Signature(const std::string& name) {
    if(!Enabled()) return;
    std::lock_guard lock(mu_);
    signature_=name;last_ui_=NowUs();
  }
 private:
  void Write(const char* type,const std::string& fields) {
    auto t=NowUs()-origin_;
    output_<<"{\"schema\":1,\"seq\":"<<++seq_<<",\"type\":"<<Quote(type)
      <<",\"t_us\":"<<t<<",\"anchor\":"<<anchor_<<",\"dt_us\":"
      <<t-anchor_time_<<','<<fields<<"}\n";output_.flush();
  }
  void Transition(const std::string& state) {
    if(state==state_) return;
    state_=state;
    if(state!="Unknown") {anchor_=seq_+1;anchor_time_=NowUs()-origin_;}
    Write("state","\"state\":"+Quote(state));
  }
  void Worker() {
    while(!stop_) {
      {
        std::lock_guard lock(mu_);
        if(recording_&&std::filesystem::exists(folder_/"stop-recording")) {
          Write("recording_end","\"reason\":\"requested\"");recording_=false;
        }
        std::ifstream replay(folder_/"replay-pad.txt");
        int active;Pad pad;int64_t lease;
        if(replay>>active>>lease>>pad.buttons>>pad.lt>>pad.rt>>pad.lx>>pad.ly>>pad.rx>>pad.ry) {
          replaying_=active&&NowUs()<lease;replay_pad_=pad;
        } else replaying_=false;
        auto now=NowUs();
        std::string candidate=now-last_ui_.load()<300000 ? signature_ : "Unknown";
        if(!candidate.empty()) {
          if(candidate!=pending_) {pending_=candidate;pending_since_=now;}
          else if(now-pending_since_>=150000) Transition(candidate);
        } else pending_.clear();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  std::filesystem::path folder_;
  std::ofstream output_;
  std::mutex mu_;
  std::thread worker_;
  std::atomic<bool> stop_{false};
  std::atomic<int64_t> last_ui_{0};
  bool recording_=false,replaying_=false,have_pad_=false;
  int64_t origin_=0,anchor_time_=0,pending_since_=0;
  uint64_t seq_=0,anchor_=0;
  Pad previous_,replay_pad_;
  std::string state_="AppLaunch",pending_;
  std::string signature_;
};
inline Journal& Get() {static Journal journal;return journal;}
} // namespace fable2::session
