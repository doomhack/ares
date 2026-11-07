#pragma once

//N64 CPU profiler: the interface the frontend uses to arm the profiler, start and
//stop captures at runtime, and read the results of the last capture.
//Plain std types only, so the frontend can include it without the N64 core.

#include <string>
#include <vector>
#include <cstdint>

namespace ares::Nintendo64::Profiling {

struct Counters {
  int64_t cycles = 0;  //93.75 MHz CPU cycles
  int64_t icacheHits = 0, icacheMisses = 0;
  int64_t dcacheHits = 0, dcacheMisses = 0, dcacheWritebacks = 0;
  int64_t uncachedReads = 0, uncachedWrites = 0;  //bytes
};

//one function, merged over every place it was called from
struct Function {
  std::string name;
  uint64_t calls = 0;
  Counters self, inclusive;  //inclusive counts each outermost activation once (recursion is not double counted)
};

//one call tree node: a function as called from one particular path
struct TreeNode {
  std::string name;
  uint32_t parent = 0;  //0: a top-level node (a thread root)
  uint32_t depth = 0;
  uint64_t calls = 0;
  Counters self, inclusive;
  std::vector<uint32_t> children;  //sorted by inclusive cycles, most first
};

struct RspTask {
  std::string name;
  uint64_t count = 0, yields = 0;
  double busyMs = 0, longestMs = 0;
};

struct Report {
  bool valid = false;
  std::string symbols;  //ELF the symbols came from, or empty
  Counters total;
  double milliseconds = 0;     //length of the capture in emulated time
  uint64_t viFrames = 0;       //VI frames (vertical interrupts with the VI active)
  uint64_t framesShown = 0;    //VI origin changes (new framebuffers shown)
  double fps = 0;              //frames shown per second, 0 if fewer than two
  double frameMsAverage = 0, frameMsMedian = 0, frameMsP90 = 0;
  double rspBusy = 0, spDmaBusy = 0, rdpBusy = 0;  //percent
  bool rdpModelled = false;    //RDP busy comes from the RDP timing model
  uint64_t events = 0;         //call/return events
  uint64_t stackOverflows = 0;
  uint64_t droppedTimelineEvents = 0;
  bool partialCallTree = false;  //started mid-game: callers already running at the start are missing
  std::vector<Function> functions;  //sorted by self cycles, most first
  std::vector<TreeNode> tree;       //index 0 is unused; roots are the nodes with parent 0
  std::vector<uint32_t> roots;      //sorted by inclusive cycles, most first
  std::vector<RspTask> rspTasks;    //sorted by busy time, most first
};

enum class State { Off, Waiting, Idle, Recording };
//Off:       no N64 game, or the profiler is off for this game
//Waiting:   enable or start requested; takes effect at the end of the current frame
//Idle:      tracking calls, not recording
//Recording: recording a capture

struct Status {
  State state = State::Off;
  bool gameLoaded = false;
  bool armedAtBoot = false;   //tracked from power on, so call trees are complete
  std::string symbols;        //ELF symbols are loaded from, or empty
  uint64_t symbolCount = 0;
  uint64_t frames = 0;        //VI frames since power on
  uint64_t captureFrames = 0; //VI frames in the capture being recorded or the last one
  double captureMs = 0;
  uint64_t timelineEvents = 0;
};

auto status() -> Status;
auto start() -> bool;   //starts recording; enables the profiler mid-game if it is off
auto stop() -> bool;    //stops recording; the capture becomes the report
auto report() -> Report;  //the last capture, or the one being recorded so far
auto setArmed(bool armed) -> void;  //profile from power on; takes effect at the next power on or reset
auto loadSymbols(const std::string& elf) -> bool;
auto exportFiles(const std::string& prefix) -> bool;  //<prefix>.{callgrind,folded,txt,trace.json}

}
