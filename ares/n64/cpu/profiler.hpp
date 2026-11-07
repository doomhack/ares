//profiler.cpp
//Function-level CPU profiler: shadow call stack driven by call/return
//instructions, charging cycles and cache counters to a call tree.
//Enabled with the ARES_N64_PROFILE_ELF (or ARES_N64_PROFILE=1) environment variable.
struct Profiler {
  CPU& self;
  Profiler(CPU& self) : self(self) {}

  struct Counters {
    s64 clocks = 0;
    s64 icacheHits = 0, icacheMisses = 0;
    s64 dcacheHits = 0, dcacheMisses = 0, dcacheWritebacks = 0;
    s64 uncachedReads = 0, uncachedWrites = 0;

    auto operator+=(const Counters& o) -> Counters& {
      clocks += o.clocks;
      icacheHits += o.icacheHits; icacheMisses += o.icacheMisses;
      dcacheHits += o.dcacheHits; dcacheMisses += o.dcacheMisses; dcacheWritebacks += o.dcacheWritebacks;
      uncachedReads += o.uncachedReads; uncachedWrites += o.uncachedWrites;
      return *this;
    }
    auto operator-(const Counters& o) const -> Counters {
      Counters r = *this;
      r.clocks -= o.clocks;
      r.icacheHits -= o.icacheHits; r.icacheMisses -= o.icacheMisses;
      r.dcacheHits -= o.dcacheHits; r.dcacheMisses -= o.dcacheMisses; r.dcacheWritebacks -= o.dcacheWritebacks;
      r.uncachedReads -= o.uncachedReads; r.uncachedWrites -= o.uncachedWrites;
      return r;
    }
  };

  //pseudo function ids (never valid code addresses in KSEG0/KUSEG game code)
  static constexpr u32 FuncException  = 0xffff'fff0;
  static constexpr u32 FuncThreadRoot = 0xffff'fff1;
  static constexpr u32 MaxDepth = 1024;

  struct Node {
    u32 parent = 0;
    u32 func = 0;
    u64 calls = 0;
    Counters self;
    std::vector<std::pair<u32, u32>> children;  //func -> node index
  };

  struct Frame {
    u32 node;
    u32 returnAddress;
  };

  struct Stack {
    u32 thread = 0;
    u32 root = 0;
    std::vector<Frame> frames;
    bool inException = false;
  };

  struct Symbol {
    u32 address;
    u32 size;
    bool function;
    std::string name;
  };

  //configuration
  bool enabled = false;
  std::string elfPath;
  std::string outputPath;
  u64 skipFrames = 0;   //ARES_N64_PROFILE_SKIP: discard costs before this frame
  u64 stopFrame = 0;    //ARES_N64_PROFILE_FRAMES: write output and quit after this many frames
  u64 frames = 0;
  bool written = false;

  //state
  s64 clockBase = 0;
  Counters last;
  std::vector<Node> nodes;
  std::vector<Stack> stacks;
  u32 current = 0;  //index into stacks
  std::vector<Symbol> symbols;
  u32 runningThreadSymbol = 0;  //address of __osRunningThread, if known
  u32 entryPoint = 0;           //ELF e_entry, if known
  bool entered = false;         //the jump to entryPoint has been seen
  u64 stackOverflows = 0;       //times a call hit MaxDepth
  u64 eventCount = 0;

  auto configure() -> void;
  auto loadSymbols(const std::string& path) -> bool;
  auto reset() -> void;
  auto dump() -> void;

  //true for J, JAL, JR, JALR and the REGIMM link branches
  static auto isCallOrReturn(u32 instruction) -> bool {
    u32 opcode = instruction >> 26;
    if(opcode == 0x02 || opcode == 0x03) return true;
    if(opcode == 0x00) { u32 funct = instruction & 63; return funct == 0x08 || funct == 0x09; }
    if(opcode == 0x01) { u32 rt = instruction >> 16 & 31; return rt >= 0x10 && rt <= 0x13; }
    return false;
  }

  //hooks
  auto instruction(u64 vaddr, u32 instruction) -> void;
  auto exceptionEnter() -> void;
  auto exceptionReturn(u32 target) -> void;
  auto resume(u32 target) -> void;
  auto synchronize(s64 clocks) -> void { clockBase += clocks; }
  auto frame() -> void;
  auto clearCosts() -> void;

  //internals
  auto counters() const -> Counters;
  auto charge() -> void;
  auto top() -> u32;
  auto child(u32 parent, u32 func) -> u32;
  auto call(u32 target, u32 returnAddress) -> void;
  auto tailCall(u32 target) -> void;
  auto enterProgram(u32 target) -> bool;
  auto ret(u32 target) -> void;
  auto switchThread(u32 thread) -> void;
  auto findSymbol(u32 address) const -> const Symbol*;
  auto resolve(u32 address) const -> u32;
  auto isFunctionStart(u32 address) const -> bool;
  auto name(u32 func) const -> std::string;
  auto nodeName(u32 node) const -> std::string;
  auto threadName(u32 stack) const -> std::string;

  //timeline: busy spans and instant markers, in 187.5 MHz scheduler clocks
  enum : u32 { TrackFrames = 0, TrackRSP = 1, TrackRSPDMA = 2, TrackRDP = 3, TrackSwap = 4, TrackCounter = 5, TrackThreadBase = 100 };
  static constexpr u64 MaxTimelineEvents = 20'000'000;
  static constexpr s64 MergeGap = 2000;  //RDP / SP DMA spans closer than this (about 10.7 us) are drawn as one
  struct Span { u32 track; u32 label; s64 start; s64 end; };
  struct Instant { u32 track; u32 label; s64 time; double value; };  //TrackRDP and TrackCounter instants are counters

  //exact busy time per VI frame from busy intervals that may be reported late or reach into the future
  struct BusyMeter {
    std::vector<std::pair<s64, s64>> pending;
    auto add(s64 start, s64 end) -> void { if(end > start) pending.push_back({start, end}); }
    auto take(s64 until) -> s64 {  //busy time before until not yet counted; keeps the rest
      s64 sum = 0;
      for(auto& p : pending) {
        s64 e = p.second < until ? p.second : until;
        if(e > p.first) sum += e - p.first, p.first = e;
      }
      std::erase_if(pending, [](auto& p) { return p.first >= p.second; });
      return sum;
    }
  };
  BusyMeter rdpMeter, dmaMeter;
  s64 dmaMeteredUntil = 0;  //the part of an open SP DMA already given to dmaMeter
  s64 frameStartTime = 0;   //time of the previous VI frame, for the per-frame counters

  //frames shown: VI origin changes, sampled once per VI frame
  u32 lastOrigin = 0;
  u64 lastSwapFrame = 0;
  bool haveOrigin = false;

  //RSP yields: a yielded task resumes later as a span labelled "<task> (resumed)"
  std::unordered_map<u32, u32> resumedOf;  //"(resumed)" label -> task label
  u32 yieldedLabel = ~0u;                  //label of the last task that halted with SP_STATUS_YIELDED
  u32 prevRspLabel = ~0u;                  //label of the RSP span before the last one
  s64 prevRspEnd = 0, lastRspEnd = 0, lastRspStart = 0;
  u32 lastRspLabel = ~0u;
  bool rspTaskLibultra = false;
  std::vector<Span> spans;
  std::vector<Instant> instants;
  std::vector<std::string> labels;
  std::vector<std::pair<u32, std::string>> exactSymbols;  //every named symbol, for naming microcode blobs
  u64 droppedEvents = 0;
  s64 timelineFullAt = -1;  //time of the first dropped event
  s64 lastDmaSpan = -1;     //index of the last SP DMA span, for coalescing
  s64 rspDmaBusyClocks = 0; //true SP DMA busy time: coalesced spans also cover the gaps between DMAs
  u64 rdpFrameSubmits = 0, rdpFrameBytes = 0;
  s64 lastRdpSpan = -1;     //index of the last RDP span, for coalescing
  s64 rdpBusyClocks = 0;
  bool rspRunning = false, rspDmaBusy = false;
  s64 rspStart = 0, rspDmaStart = 0, threadStart = 0;
  u32 rspLabel = 0, rspDmaLabel = 0;

  auto now() const -> s64 { return clockBase + self.Thread::clock; }
  auto label(const std::string& name) -> u32;
  auto span(u32 track, u32 label, s64 start, s64 end) -> void;
  auto instant(u32 track, u32 label, s64 time, double value = 0) -> void;
  auto clearTimeline() -> void;
  auto closeTimeline() -> void;
  auto exactSymbol(u32 address) const -> const std::string*;
  auto rspTaskName(bool& libultra, bool& resumed) -> std::string;
  auto rspPoll(s64 rspClock) -> void;  //called from RSP::main
  auto rdpSubmit(s64 threadClock, u32 bytes) -> void;  //called on DPC_END writes
  auto rdpBusy(s64 start, s64 end) -> void;  //estimated work, from the RDP timing model
  auto writeTrace() -> void;
  auto writeTimelineSummary(FILE* fp) -> void;
} profiler{*this};
