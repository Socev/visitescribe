// Reuse the in-memory filesystem fixture. No real SD/card access.
#define main retention_regressions
#include "retention_host.cpp"
#undef main
struct VsDirectOpusChunkInfo { unsigned sequence=1; };
struct VsLocalSession {
  String prefix, eventsPath, uuid, mode;
  std::vector<String> wavs;
  std::vector<VsDirectOpusChunkInfo> opus;
  bool directOpus=false;
};
uint32_t millis() { return 0; }
String vsBaseName(const char* p) { String s(p); return s.substring(s.lastIndexOf('/')+1); }
bool vsLocalSyncStateTerminal(const String& s) { return s=="ingested" || s=="quarantined_no_audio"; }
bool vsEventsShowComplete(const String& p) { auto i=SD.data.find(p); return i!=SD.data.end() && (i->second=="stopped" || i->second=="recovered"); }
std::map<String,int> opusReads;
std::vector<VsDirectOpusChunkInfo> vsDirectOpusChunksForPrefix(const String& p) {
  ++opusReads[p];
  // Model the validator's output: malformed/missing metadata is empty.
  return SD.data[String("/visitescribe/")+p+"_opus.csv"]=="valid" ? std::vector<VsDirectOpusChunkInfo>(1) : std::vector<VsDirectOpusChunkInfo>();
}
String vsDirectOpusModeFromEvents(const String&) { return "single_patient"; }
String vsModeFromWavs(const std::vector<String>&) { return "legacy"; }
#include "../src/sync_inventory.h"
int main() {
  retention_regressions();
  SD=FakeSD(); opusReads.clear();
  for(int i=1;i<=60;++i) {
    char p[64]; snprintf(p,sizeof(p),"/visitescribe/s%05d",i); String b=p;
    SD.data[b+"_events.csv"]="stopped";
    SD.data[b+"_opus.csv"]="valid";
    if(i>4) SD.data[b+"_sync.txt"]="uuid="+uuid+"\nstate=ingested\n";
  }
  SD.data["/visitescribe/s00001_events.csv"]="recovered";
  SD.data["/visitescribe/s00002_opus.csv"]="invalid";
  SD.data["/visitescribe/s00002_visit_p002.wav"]="wav";
  SD.data["/visitescribe/s00002_visit_p001.wav"]="wav";
  SD.data["/visitescribe/s00003_events.csv"]="recording_error";
  SD.data["/visitescribe/s00004_opus.csv"]="invalid";
  SD.data["/visitescribe/sBAD01_events.csv"]="stopped";
  std::vector<VsLocalSession> prepared;
  auto ids=vsPendingPrefixes(&prepared);
  assert(SD.directoryOpens==1); assert(ids.size()==2 && prepared.size()==2);
  assert(ids[0]=="s00001" && ids[1]=="s00002");
  assert(prepared[0].directOpus && prepared[0].opus.size()==1);
  assert(!prepared[1].directOpus && prepared[1].wavs.size()==2);
  assert(prepared[1].wavs[0].endsWith("p001.wav"));
  assert(opusReads["s00001"]==1 && opusReads["s00002"]==1);
  assert(opusReads["s00003"]==0 && opusReads["s00005"]==0);
  // A new inventory observes deletion/confirmation: no stale persistent cache.
  SD.data["/visitescribe/s00001_sync.txt"]="uuid="+uuid+"\nstate=ingested\n";
  SD.data.erase("/visitescribe/s00002_visit_p001.wav");
  SD.data.erase("/visitescribe/s00002_visit_p002.wav");
  ids=vsPendingPrefixes(&prepared); assert(ids.empty() && prepared.empty()); assert(SD.directoryOpens==2);
  // Maintenance can delete files from a shared snapshot. Fresh metadata checks
  // must not resurrect that recording, and must not rescan the directory.
  seed(); SD.data[base+"_events.csv"]="stopped";
  SD.data[base+"_opus.csv"]="valid";
  std::vector<String> directory;
  assert(vsReadSessionDirectory(directory));
  vsRetentionCleanup(&directory);
  auto remaining=SD.data;
  ids=vsPendingPrefixes(&prepared,&directory);
  assert(ids.empty() && SD.directoryOpens==1 && SD.data==remaining);
  puts("PASS: shared snapshot across retention/inventory; deleted files not resurrected; one physical directory pass.");
  puts("PASS: one directory pass for 60 sessions, Opus-first selection, WAV fallback/order, recovery, invalid/incomplete/ingested filtering, fresh inventory after mutation.");
}
