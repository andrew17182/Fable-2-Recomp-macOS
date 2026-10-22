#include "fable2_session_journal.h"
#include <cassert>
#include <iostream>
using namespace fable2::session;
int main(int argc,char** argv) {
  assert(argc==2);
  auto folder=std::filesystem::absolute(argv[1]);
  std::filesystem::create_directories(folder);
  setenv("FABLE2_SESSION_DIR",folder.c_str(),1);
  setenv("FABLE2_RECORD_INPUT","1",1);
  auto origin=std::to_string(NowUs());
  setenv("FABLE2_SESSION_ORIGIN_US",origin.c_str(),1);
  {
    Journal j;
    j.Key(69,true);j.Key(69,false);
    Pad pressed{4096};j.Input(pressed);
    j.Marker("MainMenu");
    Pad released;j.Input(released);
    {std::ofstream f(folder/"replay-pad.txt");f<<"1 "<<NowUs()+150000<<" 8192 100 200 -32768 32767 0 0\n";}
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    Pad p;assert(j.Input(p));assert(p.buttons==8192&&p.lx==-32768&&p.lt==100);
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
    p={};assert(!j.Input(p));assert(p.buttons==0);
    {std::ofstream f(folder/"stop-recording");}
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    j.Key(88,true);
  }
  std::ifstream f(folder/"events.jsonl");
  std::string data((std::istreambuf_iterator<char>(f)),{});
  assert(data.find("AppLaunch")!=std::string::npos);
  assert(data.find("MainMenu")!=std::string::npos);
  assert(data.find("recording_end")!=std::string::npos);
  assert(data.find("\"key\":88")==std::string::npos);
  assert(data.find("[4096,0,0,0,0,0,0]")!=std::string::npos);
  assert(data.find("\"down\":false")!=std::string::npos);
  assert(data.find("\"schema\":3")!=std::string::npos);
  // Schema 3: controller sticks are recorded raw ("raw" mask) with what the game read beside them, and
  // replay remaps them with the replaying session's deadzone settings (stick_deadzone.h ApplySession).
  auto rec=folder/"raw"/"recording.jsonl";
  std::filesystem::create_directories(rec.parent_path());
  setenv("FABLE2_SESSION_DIR",(folder/"raw").c_str(),1);
  {
    Journal j;
    j.Marker("Go");  // anchor: inputs are timed from it (the process guest clock already runs)
    Pad game{0,0,0,13244,0,0,0},raw{0,0,0,6554,0,0,0};
    j.Input(game,raw,1);
    assert(game.lx==13244);  // recording never changes what the game reads
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Pad keyboard{0,0,0,32767,0,0,0};j.Input(keyboard);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    j.Finish("quit");
  }
  std::filesystem::rename(folder/"raw"/"events.jsonl",rec);
  {
    std::ifstream in(rec);
    std::string text((std::istreambuf_iterator<char>(in)),{});
    assert(text.find("\"pad\":[0,0,0,6554,0,0,0],\"raw\":1,\"game\":[0,0,0,13244,0,0,0]")!=std::string::npos);
    assert(text.find("\"pad\":[0,0,0,32767,0,0,0],\"raw\":0}")!=std::string::npos);
  }
  // Replay with a 6% deadzone + 5% walk deadzone (env read once by stick_deadzone.h).
  setenv("FABLE2_STICK_DEADZONE_LEFT","6",1);setenv("FABLE2_WALK_DEADZONE","5",1);
  unsetenv("FABLE2_RECORD_INPUT");
  setenv("FABLE2_SESSION_DIR",(folder/"replay").c_str(),1);
  setenv("FABLE2_REPLAY_INPUT",rec.c_str(),1);
  {
    Journal j;
    int16_t ex=6554,ey=0;
    fable2::stickdz::ApplyLeft(fable2::stickdz::Left(),fable2::stickdz::Walk(),ex,ey);
    bool remapped=false,keyboard=false;
    j.Marker("Go");
    for(int i=0;i<400&&!(remapped&&keyboard);++i) {
      Pad p;j.Input(p);
      if(p.lx==ex&&ex!=6554) {
        remapped=true;
        assert(fable2::stickdz::g_left_replay_percent.load()==20);  // F9 readout: the raw replayed stick (6554)
      }
      if(remapped&&p.lx==32767) keyboard=true;  // not a controller stick: unchanged
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(remapped&&keyboard);
    assert(fable2::stickdz::g_left_replay_percent.load()==100);  // keyboard-pad stick: as recorded
  }
  unsetenv("FABLE2_REPLAY_INPUT");
  // Player recording (no session dir): the starting saves are copied next to the file at record start, and
  // the AppLaunch meta names the start and the copy (launcher playback boots that copy).
  unsetenv("FABLE2_SESSION_DIR");
  auto state=folder/"player-state";
  std::filesystem::create_directories(state/"saves"/"B13EBABEBABEBABE"/"4D5307F1"/"00000001"/"Hero002");
  {std::ofstream f(state/"saves"/"B13EBABEBABEBABE"/"4D5307F1"/"00000001"/"Hero002"/"mainsave.bin");f<<"hero";}
  setenv("FABLE2_APP_STATE_ROOT",state.c_str(),1);
  setenv("FABLE2_RECORD_INPUT","1",1);setenv("FABLE2_BOOT_SAVE","1",1);setenv("FABLE2_RECORD_INPUT_SAVE","Hero002",1);
  auto player_rec=folder/"recordings"/"player.jsonl";
  setenv("FABLE2_RECORD_INPUT_PATH",player_rec.c_str(),1);
  {
    Journal j;
    assert(j.Path()==player_rec);
    j.Marker("Go");
    Pad pressed{4096};j.Input(pressed);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    Pad released;j.Input(released);
    j.Finish("quit");
  }
  {
    auto copy=folder/"recordings"/"player.save"/"B13EBABEBABEBABE"/"4D5307F1"/"00000001"/"Hero002"/"mainsave.bin";
    assert(std::filesystem::is_regular_file(copy));
    std::ifstream in(player_rec);
    std::string first;std::getline(in,first);
    assert(first.find("\"start\":\"save\"")!=std::string::npos);
    assert(first.find("\"save_copy\":\"player.save\"")!=std::string::npos);
    assert(first.find("\"save\":\"Hero002\"")!=std::string::npos);
  }
  // Replay of it without a session dir (launcher playback): recording stays off even with Record input on,
  // the status goes to <state>/logs/replay-status.json, and Esc ends the replay and holds the live pad
  // until it is idle.
  setenv("FABLE2_REPLAY_INPUT",player_rec.c_str(),1);
  {
    Journal j;
    assert(j.Path().empty());
    assert(j.Replaying());
    j.Marker("Go");
    bool saw=false;
    for(int i=0;i<200&&!saw;++i) {Pad live{0,0,0,0,0,0,0};j.Input(live);saw=live.buttons==4096;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    assert(saw);
    Pad live{4096};j.Input(live);assert(live.buttons==4096);  // the replay's pad, not the live one
    j.Key(0x1B,true);
    assert(!j.Replaying());
    Pad held{16};assert(j.Input(held));assert(held.buttons==0);  // Esc's own Pause mapping held back
    Pad idle;j.Input(idle);
    Pad next{4096};assert(!j.Input(next));assert(next.buttons==4096);  // live input again
    std::ifstream in(state/"logs"/"replay-status.json");
    std::string text((std::istreambuf_iterator<char>(in)),{});
    assert(text.find("\"status\":\"cancelled\"")!=std::string::npos);
    assert(text.find("player.jsonl")!=std::string::npos);
  }
  {
    // No new recording was made by the replay.
    size_t files=0;
    for(auto& e:std::filesystem::directory_iterator(folder/"recordings")) files+=e.path().extension()==".jsonl";
    assert(files==1);
  }
  unsetenv("FABLE2_REPLAY_INPUT");unsetenv("FABLE2_RECORD_INPUT");unsetenv("FABLE2_BOOT_SAVE");
  std::cout<<"Session clock, raw edges, pad snapshots, leases, stop, raw-stick replay, start saves and Esc passed\n";
}
