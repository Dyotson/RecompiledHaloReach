// REACH_SYNCTRACE=FILE records the guest's kernel waits and signals with host timestamps,
// for frame pacing analysis (docs/framerate.md). Touch FILE.trigger to start a recording:
// it runs REACH_SYNCTRACE_SECS seconds (default 3), then FILE is written and the trigger
// removed (touch it again for another recording, which replaces FILE).
//
// FILE has one line per call: start and duration in microseconds, host thread id, the
// kernel function, its first argument (a handle, object pointer or count), an extra value
// (wait timeout in ms, -1 for none; release count; delay in us), the return value, and the
// guest call stack (return addresses: the caller of the import, then three frames up the
// guest stack's back chain). "# thread TID NAME" lines name the threads. Off unless the
// variable is set; then each wrapped call costs an atomic load until a recording starts.

#include "sync_trace.h"

#include "../platform/guest_memory.h"
#include "../platform/sdk_import.h"

#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/xthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#ifndef _WIN32
#include <unistd.h>
#endif

using reach::GuestFunc;
using reach::GuestPtr;

namespace {

struct Record {
  uint64_t start_ns;
  uint64_t dur_ns;
  uint32_t tid;
  uint32_t fn;
  uint32_t arg;
  int32_t aux;
  uint32_t ret;
  uint32_t lr[4];
};

constexpr size_t kMaxRecords = size_t(1) << 20;

enum State : int { kOff, kArmed, kRecording, kWriting };

const char* kNames[] = {"NtWaitForSingleObjectEx", "KeWaitForSingleObject",
                        "NtWaitForMultipleObjectsEx", "KeWaitForMultipleObjects",
                        "NtSignalAndWaitForSingleObjectEx", "KeDelayExecutionThread",
                        "NtSetEvent", "NtPulseEvent", "NtReleaseSemaphore",
                        "KeSetEvent", "KePulseEvent", "KeReleaseSemaphore"};
enum Fn : uint32_t {
  kNtWait, kKeWait, kNtWaitMulti, kKeWaitMulti, kNtSignalAndWait, kDelay,
  kNtSetEvent, kNtPulseEvent, kNtReleaseSemaphore, kKeSetEvent, kKePulseEvent,
  kKeReleaseSemaphore, kMark,
};

struct Trace {
  std::atomic<int> state{kOff};
  std::string path, trigger;
  double seconds = 3;
  std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
  std::atomic<uint64_t> next_poll_ns{0};
  uint64_t end_ns = 0;
  Record* records = nullptr;
  std::atomic<size_t> count{0};
  std::mutex mutex;  // threads, marks
  std::unordered_map<uint32_t, std::string> threads;
  std::unordered_map<std::string, uint32_t> marks;
};

uint64_t NowNs(const Trace& t) {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - t.epoch)
                      .count());
}

Trace& Get() {
  static Trace* trace = [] {
    auto* t = new Trace;
    if (const char* p = std::getenv("REACH_SYNCTRACE"); p && *p) {
      t->path = p;
      t->trigger = t->path + ".trigger";
      if (const char* s = std::getenv("REACH_SYNCTRACE_SECS")) t->seconds = std::atof(s);
      t->records = new Record[kMaxRecords];
      t->state = kArmed;
    }
    return t;
  }();
  return *trace;
}

uint32_t HostTid() {
#ifdef _WIN32
  return 0;
#else
  return uint32_t(gettid());
#endif
}

uint32_t Load32(uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, GuestPtr(base, address), 4);
  return __builtin_bswap32(v);
}

void Write(Trace& t) {
  FILE* f = std::fopen(t.path.c_str(), "w");
  if (f) {
    {
      std::lock_guard lock(t.mutex);
      for (const auto& [tid, name] : t.threads) {
        std::string n = name;
#ifndef _WIN32
        if (n.empty()) {  // host thread name (comm)
          char path[64], buf[64] = {};
          std::snprintf(path, sizeof(path), "/proc/self/task/%u/comm", tid);
          if (FILE* c = std::fopen(path, "r")) {
            if (std::fgets(buf, sizeof(buf), c)) n = buf;
            std::fclose(c);
            if (!n.empty() && n.back() == '\n') n.pop_back();
          }
        }
#endif
        std::fprintf(f, "# thread %u %s\n", tid, n.c_str());
      }
      for (const auto& [name, id] : t.marks) std::fprintf(f, "# mark %u %s\n", id, name.c_str());
    }
    const size_t n = std::min(t.count.load(), kMaxRecords);
    for (size_t i = 0; i < n; ++i) {
      const Record& r = t.records[i];
      const char* fn = r.fn < kMark ? kNames[r.fn] : "Mark";
      std::fprintf(f, "%.1f %.1f %u %s %08X %d %08X %08X %08X %08X %08X\n", r.start_ns / 1e3,
                   r.dur_ns / 1e3, r.tid, fn, r.arg, r.aux, r.ret, r.lr[0], r.lr[1], r.lr[2],
                   r.lr[3]);
    }
    std::fclose(f);
  }
  std::remove(t.trigger.c_str());
  REXLOG_INFO("SYNCTRACE: wrote {} records to {}", std::min(t.count.load(), kMaxRecords), t.path);
}

// True while recording; starts and ends recordings.
bool Active(Trace& t) {
  const int state = t.state.load(std::memory_order_relaxed);
  if (state == kOff || state == kWriting) return false;
  const uint64_t now = NowNs(t);
  if (state == kArmed) {
    uint64_t next = t.next_poll_ns.load(std::memory_order_relaxed);
    if (now < next || !t.next_poll_ns.compare_exchange_strong(next, now + 200'000'000)) return false;
    FILE* f = std::fopen(t.trigger.c_str(), "r");
    if (!f) return false;
    std::fclose(f);
    t.count = 0;
    t.end_ns = now + uint64_t(t.seconds * 1e9);
    int expected = kArmed;
    t.state.compare_exchange_strong(expected, kRecording);
    REXLOG_INFO("SYNCTRACE: recording {} s", t.seconds);
    return true;
  }
  if (now > t.end_ns) {
    int expected = kRecording;
    if (t.state.compare_exchange_strong(expected, kWriting)) {
      Write(t);
      t.next_poll_ns = now + 1'000'000'000;
      t.state = kArmed;
    }
    return false;
  }
  return true;
}

void NameThread(Trace& t, uint32_t tid) {
  thread_local bool named = false;
  if (named) return;
  named = true;
  auto* thread = rex::system::XThread::GetCurrentThread();
  std::lock_guard lock(t.mutex);
  t.threads[tid] = thread ? thread->name() : std::string("host");
}

void Add(Trace& t, PPCContext& ctx, uint8_t* base, uint32_t fn, uint32_t arg, int32_t aux,
         uint64_t start, uint64_t end) {
  const size_t i = t.count.fetch_add(1, std::memory_order_relaxed);
  if (i >= kMaxRecords) return;
  Record& r = t.records[i];
  r.start_ns = start;
  r.dur_ns = end - start;
  r.tid = HostTid();
  NameThread(t, r.tid);
  r.fn = fn;
  r.arg = arg;
  r.aux = aux;
  r.ret = ctx.r3.u32;
  r.lr[0] = uint32_t(ctx.lr);
  // Return addresses up the guest stack: a frame's back chain is at its stack pointer and
  // the caller's return address 8 bytes below the back chain. Reads stay below the stack's
  // top (PCR+0x70, r13 is the PCR): the page above it is unmapped.
  const uint32_t top = ctx.r13.u32 ? Load32(base, ctx.r13.u32 + 0x70) : 0;
  uint32_t sp = ctx.r1.u32;
  for (int k = 1; k < 4; ++k) {
    r.lr[k] = 0;
    if (!sp || sp + 4 > top) {
      sp = 0;
      continue;
    }
    const uint32_t back = Load32(base, sp);
    if (back <= sp || back > top || back - sp > 0x40000) {
      sp = 0;
      continue;
    }
    r.lr[k] = Load32(base, back - 8);
    sp = back;
  }
}

int32_t TimeoutMs(uint8_t* base, uint32_t ptr) {
  if (!ptr) return -1;
  uint32_t hi = Load32(base, ptr), lo = Load32(base, ptr + 4);
  const int64_t ticks = int64_t((uint64_t(hi) << 32) | lo);
  return ticks < 0 ? int32_t(-ticks / 10000) : 0;
}

// Calls the SDK's implementation and records it.
void Traced(PPCContext& ctx, uint8_t* base, GuestFunc sdk, uint32_t fn, uint32_t arg, int32_t aux) {
  Trace& t = Get();
  if (!Active(t)) {
    sdk(ctx, base);
    return;
  }
  const uint32_t lr = uint32_t(ctx.lr), r1 = ctx.r1.u32;
  const uint64_t start = NowNs(t);
  sdk(ctx, base);
  const uint64_t end = NowNs(t);
  // The import may have changed lr; the stack walk wants the caller's view.
  const uint64_t saved_lr = ctx.lr;
  const uint32_t saved_r1 = ctx.r1.u32;
  ctx.lr = lr;
  ctx.r1.u32 = r1;
  Add(t, ctx, base, fn, arg, aux, start, end);
  ctx.lr = saved_lr;
  ctx.r1.u32 = saved_r1;
}

}  // namespace

namespace reach::synctrace {

void Mark(PPCContext& ctx, uint8_t* base, const char* what) {
  Trace& t = Get();
  if (!Active(t)) return;
  uint32_t id;
  {
    std::lock_guard lock(t.mutex);
    id = t.marks.emplace(what, uint32_t(t.marks.size())).first->second;
  }
  const uint64_t now = NowNs(t);
  Add(t, ctx, base, kMark, id, 0, now, now);
}

}  // namespace reach::synctrace

#define SYNC_IMPORT(name, fn, arg, aux)                         \
  extern "C" REX_FUNC(__imp__##name) {                          \
    static GuestFunc sdk = reach::SdkImport("__imp__" #name);   \
    Traced(ctx, base, sdk, fn, arg, aux);                       \
  }

SYNC_IMPORT(NtWaitForSingleObjectEx, kNtWait, ctx.r3.u32, TimeoutMs(base, ctx.r6.u32))
SYNC_IMPORT(KeWaitForSingleObject, kKeWait, ctx.r3.u32, TimeoutMs(base, ctx.r7.u32))
SYNC_IMPORT(NtWaitForMultipleObjectsEx, kNtWaitMulti,
            ctx.r3.u32 ? Load32(base, ctx.r4.u32) : 0, int32_t(ctx.r3.u32))
SYNC_IMPORT(KeWaitForMultipleObjects, kKeWaitMulti,
            ctx.r3.u32 ? Load32(base, ctx.r4.u32) : 0, int32_t(ctx.r3.u32))
SYNC_IMPORT(NtSignalAndWaitForSingleObjectEx, kNtSignalAndWait, ctx.r4.u32,
            int32_t(ctx.r3.u32))
SYNC_IMPORT(KeDelayExecutionThread, kDelay, ctx.r4.u32, TimeoutMs(base, ctx.r5.u32))
SYNC_IMPORT(NtSetEvent, kNtSetEvent, ctx.r3.u32, 0)
SYNC_IMPORT(NtPulseEvent, kNtPulseEvent, ctx.r3.u32, 0)
SYNC_IMPORT(NtReleaseSemaphore, kNtReleaseSemaphore, ctx.r3.u32, int32_t(ctx.r4.u32))
SYNC_IMPORT(KeSetEvent, kKeSetEvent, ctx.r3.u32, 0)
SYNC_IMPORT(KePulseEvent, kKePulseEvent, ctx.r3.u32, 0)
SYNC_IMPORT(KeReleaseSemaphore, kKeReleaseSemaphore, ctx.r3.u32, int32_t(ctx.r5.u32))
