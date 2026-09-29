from pathlib import Path
import sys
r=Path(__file__).resolve().parents[1]
s=(r/'tests/retention_host.cpp').read_text(); s=s[:s.index('#include "../src/sync_retention_policy.h"')]
s+='''
#define pdMS_TO_TICKS(x) (x)
static uint16_t vsDoSeenSessionId=1;
static uint32_t vsDoChunkSequence=2;
static bool vsDirectOpusLockSd(int){return true;}
static void vsDirectOpusUnlockSd(){}
'''
backend=(r/'src/direct_opus_backend.h').read_text(); s+=backend[backend.index('static bool vsDirectOpusDiscardSession('):backend.index('static void vsDoPersistFailure(')]
s+='''
int main(){
 const String base="/visitescribe/s00001";
 SD.data[base+"_events.csv"]="events";
 SD.data[base+"_active.txt"]="active";
 SD.data[base+"_opus.csv"]="meta";
 SD.data[base+"_chunk_000001.opus"]="audio";
 SD.data[base+"_chunk_000002.opus.tmp"]="partial";
 for(int i=2;i<200;++i){char p[100];snprintf(p,sizeof(p),"/visitescribe/s%05u_chunk_000001.opus",i);SD.data[p]="old audio";}
 auto before=SD.data;
 assert(!vsDirectOpusDiscardSession(2));assert(SD.data==before);
 vsDoWorkerDone=false;assert(!vsDirectOpusDiscardSession(1));assert(SD.data==before);vsDoWorkerDone=true;
 SD.data[base+"_sync.txt"]="protected";assert(!vsDirectOpusDiscardSession(1));SD.data.erase(base+"_sync.txt");
 SD.failRemove=base+"_chunk_000001.opus";assert(!vsDirectOpusDiscardSession(1));assert(SD.exists(base+"_active.txt"));
 SD.failRemove="";assert(vsDirectOpusDiscardSession(1));assert(!SD.exists(base+"_active.txt"));
 assert(SD.data.size()==198);assert(SD.directoryOpens==0);
 puts("false-start: current-session scope, busy/protected guard, failure/retry and zero directory scans passed");
}
'''
Path(sys.argv[1]).write_text(s)
