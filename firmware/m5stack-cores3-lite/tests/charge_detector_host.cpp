#include <cassert>
#include <cstdio>
#include "../src/charge_detector.h"
static unsigned t=0;
static VsChargeDetector::Event tick(VsChargeDetector& d,int mv,bool idle=true,int context=0,bool usb=false) {
  t+=1000; return d.sample(t,mv,idle,context,usb);
}
static void quiet(VsChargeDetector& d,int seconds,int mv,bool idle=true,int context=0,bool usb=false) {
  for(int i=0;i<seconds;++i) assert(tick(d,mv,idle,context,usb)==VsChargeDetector::NONE);
}
int main(int argc, char** argv) {
  VsChargeDetector d;
  quiet(d,15,4080); quiet(d,5,4075);
  quiet(d,1,4120); quiet(d,12,4075); // transient rejected
  int events=0;
  for(int i=0;i<15;++i) events+=tick(d,4115)==VsChargeDetector::VOLTAGE_RISE;
  assert(events==1); quiet(d,60,4115); // cancellation/one-shot latch
  quiet(d,30,4070); assert(!d.latched);
  events=0; for(int i=0;i<15;++i) events+=tick(d,4110)==VsChargeDetector::VOLTAGE_RISE;
  assert(events==1);
  VsChargeDetector load;
  quiet(load,12,4070); quiet(load,30,4120,true,1); // known dim/off transition
  quiet(load,20,4160,false,1); quiet(load,20,4160,true,1); // busy baseline reset
  VsChargeDetector u;
  quiet(u,12,4080,true,0,true); // boot attached must not sync
  quiet(u,1,4080); assert(tick(u,4080,true,0,true)==VsChargeDetector::USB_POWER);
  quiet(u,20,4080,true,0,true);
  u.suspend(); quiet(u,12,4080); assert(!u.latched);
  VsChargeDetector timeout;
  quiet(timeout,12,4080); quiet(timeout,1,4105); quiet(timeout,32,4099);
  assert(!timeout.candidate);
  VsChargeDetector wrap; t=0xffffd000u;
  quiet(wrap,15,4080); events=0;
  for(int i=0;i<15;++i) events+=tick(wrap,4120)==VsChargeDetector::VOLTAGE_RISE;
  assert(events==1);
  if (argc>1) {
    FILE* f=fopen(argv[1],"r"); assert(f);
    char line[200]; assert(fgets(line,sizeof(line),f));
    VsChargeDetector measured; int hits=0; unsigned detected=0;
    while(fgets(line,sizeof(line),f)) {
      unsigned ms,a,b; int vbus,mv,status;
      assert(sscanf(line,"%u,%u,%u,%d,%d,%d",&ms,&a,&b,&vbus,&mv,&status)==6);
      if(measured.sample(ms,mv,true,0,vbus>=4000)==VsChargeDetector::VOLTAGE_RISE) {
        ++hits; detected=ms;
      }
    }
    fclose(f); assert(hits==1 && detected>=37000 && detected<39000);
    printf("Measured Bottom3 trace: one detection at %u ms (constant display load assumption)\n",detected);
  }
  puts("charge detector: all tests passed");
}
