#include "util_kenshi_telemetry.h"
#pragma once

// V717: bounded, process-local flight recorder. No GPU resource ownership or
// fence queries. A disabled recorder does not take a lock or read a clock.
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <cstdio>
#include <cstring>
#include <windows.h>
#include "log/log.h"

namespace dxvk::kenshi_fault {
  enum Kind : uint64_t { Frame=1, Submit, Complete, Batch, CameraCut, SceneReset,
    OmmReset, Memory, Checkpoint, QueueCheckpoint, DeviceLost, Control };
  // Append pass IDs: preserved V717 reports must retain their original meaning.
  enum Pass : uint64_t { Blas=1, Tlas, PathTrace, Demodulate, Denoise, Composite, Upscale,
    Gbuffer=8, Rtxdi, NeeCache, IntegrateDirect, RtxdiGradient, IntegrateIndirect, IntegrateNee };
  struct Record { uint64_t serial, tick, kind, frame, a, b, c, d; };
  struct Session {
    uint64_t run, pid, startTick, uptimeMs, movingMs, foregroundMs, frames, outcome;
    uint64_t dropped, version=717, reserved[6] {};
  };
  static_assert(sizeof(Record)==64 && sizeof(Session)==128);
  constexpr size_t Capacity=8192, FrameCapacity=2048;
  inline std::atomic<bool> active {true}, batchOptimized {true};
  inline std::atomic<uint64_t> submitted {0}, completed {0}, currentFrame {0};
  inline thread_local uint32_t batchVerifyRemaining=0, batchVerified=0, batchMismatches=0;
  inline thread_local bool batchValid=true;
  struct State {
    std::mutex mutex;
    std::array<Record,Capacity> records {};
    std::array<Record,FrameCapacity> frames {};
    std::array<Record,512> transitions {};
    uint64_t serial=0, frameSerial=0, transitionSerial=0, eventSerial=0;
    std::atomic<uint64_t> dropped {0};
    bool frozen=false, initialized=false;
    std::wstring directory;
    HANDLE ledger=INVALID_HANDLE_VALUE;
    Session session {};
    uint32_t sessionSlot=0;
    uint64_t lastTick=0, lastCamera=0, ledgerTick=0;
    std::array<HANDLE,4> controls {};
    uint64_t batchLease=0, recorderLease=0;
    uint32_t polls=0;
  };
  static_assert(sizeof(State)<1024*1024, "Recorder resident storage exceeded budget");
  inline State& state() { static State* s=new State; return *s; }
  inline void add(Kind kind,uint64_t frame,uint64_t a=0,uint64_t b=0,uint64_t c=0,uint64_t d=0) {
    if (!kenshi_telemetry::enabled()) return;
    if (!active.load(std::memory_order_relaxed) && kind!=QueueCheckpoint && kind!=DeviceLost) return;
    auto& s=state(); std::unique_lock<std::mutex> lock(s.mutex,std::defer_lock);
    if (kind==QueueCheckpoint || kind==DeviceLost) lock.lock();
    else if (!lock.try_lock()) { ++s.dropped; return; }
    if (s.frozen) return;
    Record r {++s.serial,GetTickCount64(),uint64_t(kind),frame,a,b,c,d};
    if (kind==Frame) s.frames[s.frameSerial++ % FrameCapacity]=r;
    else if (kind==CameraCut || kind==SceneReset || kind==OmmReset || kind==Memory || kind==QueueCheckpoint || kind==DeviceLost)
      s.transitions[s.transitionSerial++ % s.transitions.size()]=r;
    else s.records[s.eventSerial++%Capacity]=r;
  }
  inline void writeSession(State& s, uint64_t outcome) {
    if (s.ledger==INVALID_HANDLE_VALUE) return;
    s.session.outcome=outcome; s.session.dropped=s.dropped.load();
    LARGE_INTEGER offset; offset.QuadPart=32+128ull*s.sessionSlot;
    DWORD written=0;
    if (SetFilePointerEx(s.ledger,offset,nullptr,FILE_BEGIN))
      WriteFile(s.ledger,&s.session,sizeof(s.session),&written,nullptr);
  }
  inline void initialize() {
    if (!kenshi_telemetry::enabled()) return;
    auto& s=state(); if (s.initialized) return;
    s.initialized=true;
    wchar_t path[32768] {}; GetModuleFileNameW(nullptr,path,32768);
    s.directory=path; s.directory.resize(s.directory.find_last_of(L"\\/")+1);
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    s.session.run=(uint64_t(ft.dwHighDateTime)<<32)|ft.dwLowDateTime;
    s.session.pid=GetCurrentProcessId(); s.session.startTick=GetTickCount64();
    s.lastTick=s.ledgerTick=s.session.startTick;
    if (kenshi_telemetry::fileOutputEnabled())
      s.ledger=CreateFileW((s.directory+L"kenshi-sessions-v717.bin").c_str(),GENERIC_READ|GENERIC_WRITE,
      FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (s.ledger!=INVALID_HANDLE_VALUE) {
      uint64_t header[4] {}; DWORD count=0;
      LARGE_INTEGER size {}; GetFileSizeEx(s.ledger,&size);
      constexpr uint64_t magic=0x3731375648534e4bull;
      ReadFile(s.ledger,header,sizeof(header),&count,nullptr);
      if (size.QuadPart==0 || (count==32 && header[0]==magic && size.QuadPart<=32800)) {
        if (size.QuadPart==0) header[0]=magic;
        s.sessionSlot=uint32_t(header[1]++%256);
        LARGE_INTEGER start {}; SetFilePointerEx(s.ledger,start,nullptr,FILE_BEGIN);
        WriteFile(s.ledger,header,sizeof(header),&count,nullptr); writeSession(s,1);
      } else { CloseHandle(s.ledger); s.ledger=INVALID_HANDLE_VALUE; }
    }
    const wchar_t* names[]={L"KenshiBlasBatchBaseline-",L"KenshiBlasBatchOptimized-",
      L"KenshiFaultRecordOff-",L"KenshiFaultRecordOn-"};
    for (size_t i=0;i<4;++i) s.controls[i]=CreateEventW(nullptr,FALSE,FALSE,
      (std::wstring(L"Local\\")+names[i]+std::to_wstring(s.session.pid)).c_str());
    KENSHI_DIAGNOSTIC_INFO("[FaultRecord V717] ready ramBytes="+std::to_string(sizeof(State))+
      " reportMaxBytes=1048576 reportSlots=8 ledgerMaxBytes=32800 active=1 batchOptimized=1 oracle=off");
  }
  // Called only on the main EndFrame lane. Diagnostic control leases recover
  // normal defaults if an operator script is interrupted.
  inline void frame(uint64_t frameId,uint64_t camera,bool cameraValid,uint64_t settings) {
    if (!kenshi_telemetry::enabled()) { batchOptimized=true; return; }
    initialize(); auto& s=state(); currentFrame.store(frameId,std::memory_order_relaxed);
    const uint64_t now=GetTickCount64(), elapsed=now-s.lastTick;
    if ((++s.polls&63u)==1u) {
      bool signals[4] {};
      for (size_t i=0;i<4;++i) signals[i]=s.controls[i] && WaitForSingleObject(s.controls[i],0)==WAIT_OBJECT_0;
      if (signals[0]&&!signals[1]) { batchOptimized=false;s.batchLease=now+120000;
        KENSHI_DIAGNOSTIC_INFO("[BlasBatch V717] optimized=0 leaseSeconds=120"); }
      else if (signals[1]||(!batchOptimized&&now>=s.batchLease)) { batchOptimized=true;
        KENSHI_DIAGNOSTIC_INFO("[BlasBatch V717] optimized=1"); }
      if (signals[2]&&!signals[3]) { active=false;s.recorderLease=now+120000;
        KENSHI_DIAGNOSTIC_INFO("[FaultRecord V717] active=0 leaseSeconds=120"); }
      else if (signals[3]||(!active&&now>=s.recorderLease)) { active=true;
        KENSHI_DIAGNOSTIC_INFO("[FaultRecord V717] active=1"); }
    }
    // Exposure accounting is coarse and independent of optional event recording.
    DWORD foreground=0; GetWindowThreadProcessId(GetForegroundWindow(),&foreground);
    {
      std::lock_guard<std::mutex> lock(s.mutex);
      if (!s.frozen) {
        s.session.uptimeMs=now-s.session.startTick; ++s.session.frames;
        if (cameraValid && s.lastCamera && camera!=s.lastCamera) s.session.movingMs+=elapsed;
        if (foreground==s.session.pid) s.session.foregroundMs+=elapsed;
      }
    }
    s.lastTick=now; s.lastCamera=cameraValid?camera:0;
    add(Frame,frameId,camera,settings,elapsed,(cameraValid?1ull:0ull)|(foreground==s.session.pid?2ull:0ull));
    if (now-s.ledgerTick>=60000) { s.ledgerTick=now;
      std::lock_guard<std::mutex> lock(s.mutex); if (!s.frozen) writeSession(s,1); }
  }
  template<class Context> inline void checkpoint(Context* ctx, Pass pass, uint64_t frameId) {
    if (!kenshi_telemetry::enabled() || !active.load(std::memory_order_relaxed)) return;
    // Opaque value, never dereferenced by us. Token carries frame/pass even if
    // corresponding CPU history has wrapped. No pointer into a mutable ring.
    const uintptr_t token=(uintptr_t(frameId)<<8)|uintptr_t(pass);
    add(Checkpoint,frameId,pass,token);
    if (ctx->getDevice()->extensions().nvDeviceDiagnosticCheckpoints)
      ctx->getCommandList()->vkCmdSetCheckpointNV(reinterpret_cast<const void*>(token));
  }
  template<class Device> inline void memory(Device* device,uint64_t frameId) {
    if (!kenshi_telemetry::enabled() || !active.load(std::memory_order_relaxed) || (frameId&63u)) return;
    static thread_local uint64_t last=0;
    const auto now=GetTickCount64(); if(now-last<2000) return; last=now;
    const auto info=device->adapter()->getMemoryHeapInfo();
    uint64_t used=0,budget=0;
    for(uint32_t i=0;i<info.heapCount;++i) if(info.heaps[i].heapFlags&1u) {
      used+=info.heaps[i].memoryAllocated;budget+=info.heaps[i].memoryBudget;
    }
    add(Memory,frameId,used,budget);
  }
  inline void fault(const char* reason) {
    auto& s=state(); std::lock_guard<std::mutex> lock(s.mutex);
    if (s.frozen) return; s.frozen=true;
    if (!s.initialized) { Logger::err("[FaultRecord V717] fault before recorder initialization"); return; }
    s.session.uptimeMs=GetTickCount64()-s.session.startTick; writeSession(s,3);
    if (!kenshi_telemetry::fileOutputEnabled()) return;
    const auto path=s.directory+L"kenshi-fault-v717-"+std::to_wstring(s.sessionSlot%8)+L".txt";
    FILE* file=nullptr; _wfopen_s(&file,path.c_str(),L"wb");
    if (!file) { Logger::err("[FaultRecord V717] could not open bounded fault report"); return; }
    size_t bytes=0,omitted=0;
    const auto line=[&](const char* text) { const size_t n=strlen(text);
      if (bytes+n<=1048576-256) { fwrite(text,1,n,file); bytes+=n; } else ++omitted; };
    char text[512];
    snprintf(text,sizeof(text),"Kenshi fault V717 run=%llu pid=%llu reason=%s uptimeMs=%llu active=%u batchOptimized=%u dropped=%llu serial=%llu frames=%llu\n",
      s.session.run,s.session.pid,reason,s.session.uptimeMs,unsigned(active.load()),unsigned(batchOptimized.load()),
      s.dropped.load(),s.serial,s.frameSerial); line(text);
    line("Build identity: paired DLL SHA256 from kenshi-build-id.txt follows (deployment metadata).\n");
    FILE* identity=nullptr; _wfopen_s(&identity,(s.directory+L"kenshi-build-id.txt").c_str(),L"rb");
    if (identity) { size_t n=fread(text,1,sizeof(text)-1,identity);text[n]=0;line(text);fclose(identity); }
    line("Columns: serial tick kind frame a b c d. Records unordered; sort by serial. History overwritten on wrap.\n");
    const auto dump=[&](const auto& records) { for (const auto& r:records) if(r.serial) {
      snprintf(text,sizeof(text),"%llu %llu %llu %llu %llu %llu %llu %llu\n",r.serial,r.tick,r.kind,r.frame,r.a,r.b,r.c,r.d);line(text); } };
    dump(s.transitions);dump(s.frames);dump(s.records);
    snprintf(text,sizeof(text),"END bytes=%zu max=1048576 omittedLines=%zu eventTotal=%llu frameTotal=%llu transitionTotal=%llu dropped=%llu\n",
      bytes,omitted,s.eventSerial,s.frameSerial,s.transitionSerial,s.dropped.load());
    fwrite(text,1,strlen(text),file); fclose(file);
    Logger::err("[FaultRecord V717] saved bounded report slot="+std::to_string(s.sessionSlot%8));
  }
  inline void close() {
    auto& s=state(); if (!s.initialized) return;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.frozen) { s.session.uptimeMs=GetTickCount64()-s.session.startTick;writeSession(s,2); }
    if (s.ledger!=INVALID_HANDLE_VALUE) { CloseHandle(s.ledger);s.ledger=INVALID_HANDLE_VALUE; }
    for(auto& handle:s.controls) if(handle) { CloseHandle(handle);handle=nullptr; }
  }
}
