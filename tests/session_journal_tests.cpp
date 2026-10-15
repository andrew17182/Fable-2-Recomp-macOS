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
  std::cout<<"Session clock, raw edges, pad snapshots, leases and stop passed\n";
}
