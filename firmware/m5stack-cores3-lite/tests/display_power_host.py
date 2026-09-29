from pathlib import Path
import sys
r=Path(__file__).resolve().parents[1]
s=(r/'src/main_v02.cpp').read_text()
body=s[s.index('void serviceDisplayPower() {'):s.index('\nvoid setup() {',s.index('void serviceDisplayPower() {'))]
prefix='''#include <cassert>
#include <cstdint>
#include <cstdio>
enum class AppState { HOME, MODE_CONFIRM, RECORDING, PAUSED, SAVING, FINISHED, MENU, STATUS, DETAILS, SYNC, ERROR, CHARGE_SYNC };
enum class SyncPhase { NOT_STARTED, CONNECTING_1, CONNECTING_2, CONNECTED, FAILED, NO_CREDENTIALS };
enum class DisplayPower { ACTIVE, DIMMED, OFF };
static AppState state=AppState::HOME;
static SyncPhase syncPhase=SyncPhase::NOT_STARTED;
static DisplayPower displayPower=DisplayPower::ACTIVE;
static uint32_t now=0,lastUserActivityMs=0;
static constexpr int BRIGHTNESS_DIM=20;
static bool busy=false;
static bool isBusy(){return busy;}
static bool (*vsSyncTouchLockedHook)()=isBusy;
struct Display { void sleep(){} void setBrightness(int){} };
struct Device { Display Display; } M5;
static uint32_t millis(){return now;}
static void noteActivity(){lastUserActivityMs=now;displayPower=DisplayPower::ACTIVE;}
'''
tail='''
int main() {
  for(int i=0;i<=int(AppState::CHARGE_SYNC);++i) {
    state=AppState(i); noteActivity(); serviceDisplayPower();
    const unsigned dim=(state==AppState::RECORDING||state==AppState::PAUSED)?10000:15000;
    now+=dim-1; serviceDisplayPower(); assert(displayPower==DisplayPower::ACTIVE);
    ++now; serviceDisplayPower(); assert(displayPower==DisplayPower::DIMMED);
    now+=30000-dim; serviceDisplayPower(); assert(displayPower==DisplayPower::OFF);
    noteActivity(); serviceDisplayPower(); assert(displayPower==DisplayPower::ACTIVE);
  }
  state=AppState::SYNC; busy=true;
  for(int i=0;i<100;++i){now+=1000;serviceDisplayPower();assert(displayPower==DisplayPower::ACTIVE);}
  busy=false; noteActivity(); now+=15000;serviceDisplayPower();assert(displayPower==DisplayPower::DIMMED);
  now+=15000;serviceDisplayPower();assert(displayPower==DisplayPower::OFF);
  syncPhase=SyncPhase::FAILED;serviceDisplayPower();assert(displayPower==DisplayPower::ACTIVE);
  now+=30000;serviceDisplayPower();assert(displayPower==DisplayPower::OFF);
  // Periodic repaints/battery changes do not change state or reset inactivity.
  for(int i=0;i<50;++i){now+=1000;serviceDisplayPower();assert(displayPower==DisplayPower::OFF);}
  now=0xfffff000u;noteActivity();now+=30000;serviceDisplayPower();assert(displayPower==DisplayPower::OFF);
  puts("display power: all 12 states, busy transfer, error result, wake and wrap passed");
}
'''
# Generate a host fixture from the actual firmware function, avoiding a copy.
Path(sys.argv[1]).write_text(prefix+body+tail)
