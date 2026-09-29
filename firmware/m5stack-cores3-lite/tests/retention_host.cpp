// Host regression harness: only an in-memory SD card; never opens patient files.
// Build with a C++17 host compiler, e.g. cl /EHsc /std:c++17 retention_host.cpp.
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include <stdexcept>
#include <stdint.h>
class String : public std::string {
public:
  using std::string::string;
  String(const std::string& s) : std::string(s) {}
  String substring(size_t a, size_t b = npos) const { return substr(a, b == npos ? npos : b-a); }
  bool startsWith(const String& s) const { return rfind(s, 0) == 0; }
  bool endsWith(const String& s) const { return size() >= s.size() && compare(size()-s.size(), s.size(), s)==0; }
  int compareTo(const String& s) const { return compare(s); }
  int lastIndexOf(char c) const { auto i=rfind(c); return i==npos ? -1 : int(i); }
  void trim() { auto a=find_first_not_of(" \r\n\t"), b=find_last_not_of(" \r\n\t"); *this=a==npos ? "" : substr(a,b-a+1); }
};
constexpr int FILE_READ=0, FILE_WRITE=1;
struct File {
  String path; size_t pos=0, index=0; bool valid=false, directory=false;
  std::vector<String> entries;
  File() = default;
  explicit File(String p) : path(p), valid(true) {}
  operator bool() const { return valid; }
  bool isDirectory() const { return directory; }
  const char* name() const { return path.c_str(); }
  File openNextFile();
  bool available() const;
  String readStringUntil(char c);
  size_t print(const String& s);
  void flush() {}
  void close() { valid=false; }
};
struct PowerLoss {};
struct FakeSD {
  std::map<String,String> data;
  String failRemove; int directoryOpens=0; int crashAfter=0, removed=0; size_t writeLimit=SIZE_MAX;
  bool exists(const String& p) { return data.count(p)>0; }
  File open(const String& p, int mode=FILE_READ) {
    if (p=="/visitescribe") { ++directoryOpens; File f(p); f.directory=true; for(auto& e:data) if(e.first.startsWith("/visitescribe/")) f.entries.push_back(e.first); return f; }
    if (mode==FILE_WRITE) data[p]="";
    return exists(p) ? File(p) : File();
  }
  bool remove(const String& p) {
    if(p==failRemove) return false;
    bool ok=data.erase(p)>0;
    if(ok && ++removed==crashAfter) throw PowerLoss();
    return ok;
  }
  bool rename(const String& a,const String& b) { if(!exists(a)||exists(b)) return false; data[b]=data[a]; data.erase(a); return true; }
} SD;
File File::openNextFile() { return index<entries.size() ? File(entries[index++]) : File(); }
bool File::available() const { return valid && pos<SD.data.at(path).size(); }
String File::readStringUntil(char c) { auto& s=SD.data[path]; auto end=s.find(c,pos); if(end==String::npos) end=s.size(); String out=s.substr(pos,end-pos); pos=std::min(end+1,s.size()); return out; }
size_t File::print(const String& s) { size_t n=std::min(s.size(),SD.writeLimit); SD.data[path]+=s.substr(0,n); return n; }
struct { template<typename... T> void printf(const char*, T...) {} } Serial;
bool sdOk=true, captureRunning=false, sessionOpen=false, vsDoWorkerDone=true;
#define VISITESCRIBE_DIRECT_OPUS 1
bool vsReadSyncMeta(const String& prefix, String& uuid, String& state) {
  File f=SD.open(String("/visitescribe/")+prefix+"_sync.txt"); if(!f) return false;
  while(f.available()) { String l=f.readStringUntil('\n'); l.trim(); if(l.startsWith("uuid=")) uuid=l.substring(5); if(l.startsWith("state=")) state=l.substring(6); }
  return uuid.length()==36;
}
#include "../src/sync_retention_policy.h"
#include "../src/sync_retention.h"
const String base="/visitescribe/s00001", uuid="11111111-1111-1111-1111-111111111111";
const String delMarker="/visitescribe/_busy_del.txt";
void seed(const char* state="ingested", bool legacyStamp=false) {
  SD=FakeSD(); captureRunning=sessionOpen=false; vsDoWorkerDone=sdOk=true; vsPurgeDue=false;
  SD.data[base+"_sync.txt"]="uuid="+uuid+"\nstate="+state+"\n";
  SD.data[base+"_events.csv"]="events";
  SD.data[base+"_chunk_000001.opus"]="audio";
  SD.data[base+"_chunk_000002.opus.tmp"]="partial";
  if(legacyStamp) SD.data[base+"_synced_at.txt"]="uuid="+uuid+"\nconfirmed_at=1800000000\n";
  SD.data["/visitescribe/s00002_chunk_000001.opus"]="other session";
  SD.data["/visitescribe/s000010_private.txt"]="different prefix";
  SD.data["/unrelated/s00001_secret"]="outside root";
}
bool audio() { return SD.exists(base+"_chunk_000001.opus"); }
int main() {
  // Confirmed = deleted at once, no clock needed; other sessions untouched.
  seed(); assert(vsRetentionCleanup()==1); assert(SD.data.size()==3); assert(!SD.exists(base+"_sync.txt"));
  assert(!SD.exists(delMarker));
  // 0.9.x left a 72h stamp: it goes too.
  seed("ingested",true); assert(vsRetentionCleanup()==1); assert(SD.data.size()==3);
  // Anything not confirmed by the server is never touched.
  for(auto state:{"queued","uploading","quarantined_no_audio","failed"}) { seed(state); auto original=SD.data; assert(vsRetentionCleanup()==0); assert(SD.data==original); }
  // Busy guards.
  seed(); captureRunning=true; vsRetentionCleanup(); assert(audio());
  seed(); sessionOpen=true; vsRetentionCleanup(); assert(audio());
  seed(); vsDoWorkerDone=false; vsRetentionCleanup(); assert(audio());
  seed(); sdOk=false; vsRetentionCleanup(); assert(audio());
  // The v0.6.6 hook only asks for a pass; it never deletes during an upload.
  seed(); vsRetentionRecordConfirmation("s00001",uuid); assert(audio()); assert(vsPurgeDue);
  // A refused delete keeps the receipt and the marker, and asks again.
  seed(); SD.failRemove=base+"_chunk_000002.opus.tmp"; assert(vsRetentionCleanup()==0);
  assert(SD.exists(base+"_sync.txt")); assert(SD.exists(base+"_events.csv")); assert(SD.exists(delMarker)); assert(vsPurgeDue);
  SD.failRemove=""; assert(vsRetentionCleanup()==1); assert(!SD.exists(delMarker)); assert(SD.data.size()==3);
  // Reset right after each successful delete, receipt included: the marker
  // survives until the end, and a second pass always finishes the job.
  for(int step=1;step<=6;++step) {
    seed(); SD.crashAfter=step;
    bool crashed=false;
    try { vsRetentionCleanup(); } catch(PowerLoss&) { crashed=true; }
    if(crashed && SD.exists(base+"_sync.txt")) assert(SD.exists(delMarker));
    if(audio() || SD.exists(base+"_chunk_000002.opus.tmp")) assert(SD.exists(base+"_sync.txt"));
    SD.crashAfter=0; vsRetentionCleanup(); assert(SD.data.size()==3); assert(!SD.exists(delMarker));
  }
  // Markers themselves are never mistaken for sessions.
  seed(); SD.data["/visitescribe/_busy_rec.txt"]="action=rec\n"; vsRetentionCleanup();
  assert(SD.exists("/visitescribe/_busy_rec.txt")); assert(SD.data.size()==4);
  puts("PASS: delete on confirmed sync, legacy stamps, unconfirmed states kept, busy guards, deferred hook, delete failure, 6 reset points, markers.");
  return 0;
}
