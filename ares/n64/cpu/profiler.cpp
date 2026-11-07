//The frontend (Tools > Profiler) arms the profiler with the "Profiler" option, which takes
//effect at power on, and starts and stops captures through n64/profiler.hpp. A capture can
//also be started on a game that was not armed: the profiler is then enabled mid-game, and
//functions that were already running at that point are missing from the top of the call tree.
//
//Without the frontend, environment variables enable it and record from power on:
//  ARES_N64_PROFILE_ELF=<path to game .elf>   enables profiling with symbols
//  ARES_N64_PROFILE=1                         enables profiling without symbols
//  ARES_N64_PROFILE_OUT=<path prefix>         output prefix (default: <elf>.profile or ares-n64.profile)
//  ARES_N64_PROFILE_SKIP=<frames>             discard everything before this frame (skip boot)
//  ARES_N64_PROFILE_FRAMES=<frames>           after this many frames: write output and quit ares
//With environment variables, output is written when the game is unloaded or reset:
//  <prefix>.callgrind  (KCachegrind / QCachegrind)
//  <prefix>.folded     (speedscope, flamegraph.pl, inferno)
//  <prefix>.txt        (flat per-function table)
//  <prefix>.trace.json (Perfetto / chrome://tracing)
auto CPU::Profiler::configure() -> void {
  auto get = [](const char* name) -> std::string {
    #if defined(_MSC_VER)
    char* value = nullptr; size_t length = 0;
    if(_dupenv_s(&value, &length, name) || !value) return {};
    std::string result = value; free(value); return result;
    #else
    auto value = std::getenv(name);
    return value ? std::string{value} : std::string{};
    #endif
  };
  auto envElf = get("ARES_N64_PROFILE_ELF");
  outputPath = get("ARES_N64_PROFILE_OUT");
  fromEnvironment = !envElf.empty() || get("ARES_N64_PROFILE") == "1";
  skipFrames = fromEnvironment ? std::strtoull(get("ARES_N64_PROFILE_SKIP").c_str(), nullptr, 10) : 0;
  stopFrame = fromEnvironment ? std::strtoull(get("ARES_N64_PROFILE_FRAMES").c_str(), nullptr, 10) : 0;
  enabled = fromEnvironment || optionEnabled;
  armedAtBoot = enabled;
  frames = 0;
  partial = false;
  recording = false;
  captured = false;
  pendingEnable = pendingStart = false;
  written = false;

  auto elf = !envElf.empty() ? envElf : optionSymbols;
  if(elf != elfPath || symbols.empty()) {
    symbols.clear();
    exactSymbols.clear();
    runningThreadSymbol = 0;
    entryPoint = 0;
    elfPath.clear();
    if(!elf.empty()) {
      if(loadSymbols(elf)) {
        elfPath = elf;
        printf("[profiler] loaded %d symbols from %s\n", (int)symbols.size(), elf.c_str());
      } else {
        printf("[profiler] could not read symbols from %s\n", elf.c_str());
      }
    }
  }
  if(!enabled) return;
  if(outputPath.empty()) outputPath = elfPath.empty() ? "ares-n64.profile" : elfPath + ".profile";
  if(fromEnvironment) printf("[profiler] enabled, output prefix %s\n", outputPath.c_str());
}

auto CPU::Profiler::loadSymbols(const std::string& path) -> bool {
  symbols.clear();
  exactSymbols.clear();
  runningThreadSymbol = 0;
  entryPoint = 0;
  std::ifstream file(path, std::ios::binary);
  if(!file) return false;
  std::vector<u8> elf((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if(elf.size() < 0x34 || elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F') return false;
  bool is64 = elf[4] == 2;
  bool big = elf[5] == 2;
  auto rd = [&](u64 offset, u32 size) -> u64 {
    if(offset + size > elf.size()) return 0;
    u64 value = 0;
    for(u32 n = 0; n < size; n++) {
      u32 byte = big ? n : size - 1 - n;
      value = value << 8 | elf[offset + byte];
    }
    return value;
  };
  entryPoint    = is64 ? rd(0x18, 8) : rd(0x18, 4);
  u64 shoff     = is64 ? rd(0x28, 8) : rd(0x20, 4);
  u32 shentsize = is64 ? rd(0x3a, 2) : rd(0x2e, 2);
  u32 shnum     = is64 ? rd(0x3c, 2) : rd(0x30, 2);
  struct Section { u32 type; u64 flags; u64 offset; u64 size; u32 link; u64 entsize; };
  auto section = [&](u32 index) -> Section {
    u64 base = shoff + (u64)index * shentsize;
    Section s;
    s.type = rd(base + 4, 4);
    if(is64) {
      s.flags = rd(base + 8, 8); s.offset = rd(base + 0x18, 8); s.size = rd(base + 0x20, 8);
      s.link = rd(base + 0x28, 4); s.entsize = rd(base + 0x38, 8);
    } else {
      s.flags = rd(base + 8, 4); s.offset = rd(base + 0x10, 4); s.size = rd(base + 0x14, 4);
      s.link = rd(base + 0x18, 4); s.entsize = rd(base + 0x24, 4);
    }
    return s;
  };
  for(u32 index = 0; index < shnum; index++) {
    auto symtab = section(index);
    if(symtab.type != 2) continue;  //SHT_SYMTAB
    auto strtab = section(symtab.link);
    u32 entsize = symtab.entsize ? symtab.entsize : (is64 ? 24 : 16);
    for(u64 offset = symtab.offset; offset + entsize <= symtab.offset + symtab.size; offset += entsize) {
      u32 nameOffset = rd(offset, 4);
      u64 value, size; u8 info; u16 shndx;
      if(is64) {
        info = elf[offset + 4]; shndx = rd(offset + 6, 2); value = rd(offset + 8, 8); size = rd(offset + 16, 8);
      } else {
        value = rd(offset + 4, 4); size = rd(offset + 8, 4); info = elf[offset + 12]; shndx = rd(offset + 14, 2);
      }
      u8 type = info & 15;
      if(shndx == 0 || shndx >= 0xff00) continue;  //undefined, absolute, common
      if(nameOffset >= strtab.size) continue;
      const char* raw = (const char*)&elf[strtab.offset + nameOffset];
      std::string name{raw, strnlen(raw, strtab.size - nameOffset)};
      if(name.empty() || name[0] == '.' || name[0] == '$') continue;
      if(name == "__osRunningThread" && type == 1) runningThreadSymbol = (u32)value;  //STT_OBJECT
      exactSymbols.push_back({(u32)value, name});
      auto sec = section(shndx);
      bool executable = sec.flags & 4;  //SHF_EXECINSTR
      if(!executable) continue;
      if(type != 2 && type != 0) continue;  //STT_FUNC or STT_NOTYPE (assembly labels)
      if(type == 0 && (info >> 4) == 0) continue;  //skip local NOTYPE labels (branch targets in asm)
      symbols.push_back({(u32)value, (u32)size, type == 2, name});
    }
  }
  std::sort(exactSymbols.begin(), exactSymbols.end());
  std::sort(symbols.begin(), symbols.end(), [](const Symbol& a, const Symbol& b) {
    if(a.address != b.address) return a.address < b.address;
    return a.function > b.function;  //prefer STT_FUNC for aliases
  });
  symbols.erase(std::unique(symbols.begin(), symbols.end(), [](const Symbol& a, const Symbol& b) {
    return a.address == b.address;
  }), symbols.end());
  //symbols without a size extend to the next symbol
  for(size_t n = 0; n < symbols.size(); n++) {
    if(symbols[n].size) continue;
    symbols[n].size = n + 1 < symbols.size() ? symbols[n + 1].address - symbols[n].address : 4;
  }
  #if defined(ARES_PROFILER_DEMANGLE)
  for(auto& symbol : symbols) {
    if(symbol.name.rfind("_Z", 0) != 0) continue;
    int status = 0;
    char* demangled = abi::__cxa_demangle(symbol.name.c_str(), nullptr, nullptr, &status);
    if(status == 0 && demangled) symbol.name = demangled;
    free(demangled);
  }
  #endif
  return !symbols.empty();
}

auto CPU::Profiler::reset() -> void {
  clockBase = 0;
  nodes.clear();
  stacks.clear();
  nodes.push_back({});  //node 0 is unused so that 0 can mean "none"
  current = 0;
  eventCount = 0;
  frames = 0;
  written = false;
  entered = false;
  stackOverflows = 0;
  recording = false;
  captured = false;
  captureStartFrame = captureFrames = 0;
  captureEnd = 0;
  clearTimeline();
  switchThread(0);
  last = counters();
  //environment variables record from power on (ARES_N64_PROFILE_SKIP starts later)
  if(fromEnvironment && !skipFrames) recording = true, captured = true;
}

//recording can start and stop at any time: the call stacks are tracked either way
auto CPU::Profiler::start() -> bool {
  if(written) return false;
  if(!enabled) {
    pendingEnable = pendingStart = true;
    return true;
  }
  if(recording) return true;
  charge();
  recording = true;
  captured = true;
  clearCosts();
  captureStartFrame = frames;
  captureFrames = 0;
  return true;
}

auto CPU::Profiler::stop() -> bool {
  pendingStart = pendingEnable = false;
  if(!enabled || !recording) return false;
  charge();
  closeTimeline();
  captureEnd = now();
  captureFrames = frames - captureStartFrame;
  recording = false;
  return true;
}

//Turning the hooks on needs recompiled code without them thrown away. This runs from
//CPU::main, between frames, where no recompiled block is running.
auto CPU::Profiler::applyPending() -> void {
  if(pendingEnable) {
    pendingEnable = false;
    if(!enabled && !written) {
      enabled = true;
      partial = true;
      entered = true;  //the program is already running
      reset();
      if constexpr(Accuracy::CPU::Recompiler) {
        self.recompiler.allocator.release();
        self.recompiler.reset();
      }
    }
  }
  if(pendingStart) {
    pendingStart = false;
    start();
  }
}

auto CPU::Profiler::clearCosts() -> void {
  for(auto& node : nodes) { node.self = {}; node.calls = 0; }
  last = counters();
  clockBase -= last.clocks;  //restart the clock at zero
  last.clocks = 0;
  eventCount = 0;
  stackOverflows = 0;
  clearTimeline();
}

auto CPU::Profiler::frame() -> void {
  if(written) return;
  if(!vi.active()) return;  //while the VI is off, ares reports arbitrary refreshes; only count real frames
  frames++;
  if(recording) captureFrames = frames - captureStartFrame;
  s64 t = now();
  instant(TrackFrames, label("frame"), t, frames);
  instant(TrackRDP, label("RDP submits per frame"), t, rdpFrameSubmits);
  instant(TrackRDP, label("RDP command bytes per frame"), t, rdpFrameBytes);
  rdpFrameSubmits = rdpFrameBytes = 0;

  //exact busy % of the VI frame that just ended, drawn from its start (the spans include merge gaps)
  if(recording && rspDmaBusy) dmaMeter.add(max(rspDmaStart, dmaMeteredUntil), t), dmaMeteredUntil = t;
  if(s64 length = t - frameStartTime; recording && length > 0) {
    if(rdp.timing.enabled) instant(TrackCounter, label("RDP busy %"), frameStartTime, 100.0 * rdpMeter.take(t) / length);
    instant(TrackCounter, label("SP DMA busy %"), frameStartTime, 100.0 * dmaMeter.take(t) / length);
  }
  frameStartTime = t;

  //a new framebuffer origin is a new frame shown; interlaced modes also move the origin
  //by one line every field, which is not a new frame
  u32 origin = vi.io.dramAddress;
  if(!haveOrigin) {
    haveOrigin = true;
    lastOrigin = origin;
  } else if(origin != lastOrigin) {
    u32 line = (u32)vi.io.width * (vi.io.colorDepth == 3 ? 4 : 2);
    u32 delta = origin > lastOrigin ? origin - lastOrigin : lastOrigin - origin;
    if(!(vi.io.serrate && delta == line)) {
      instant(TrackSwap, label("frame shown"), t, lastSwapFrame ? frames - lastSwapFrame : 0);
      lastSwapFrame = frames;
    }
    lastOrigin = origin;
  }
  if(skipFrames && frames == skipFrames) {
    start();
    printf("[profiler] skipped %llu frames, now recording\n", (unsigned long long)skipFrames);
  }
  if(stopFrame && frames == stopFrame) {
    dump();
    platform->event(ares::Event::Shutdown);
  }
}

auto CPU::Profiler::counters() const -> Counters {
  Counters c;
  c.clocks = clockBase + self.Thread::clock;
  c.icacheHits = self.profile.icacheHits;
  c.icacheMisses = self.profile.icacheMisses;
  c.dcacheHits = self.profile.dcacheHits;
  c.dcacheMisses = self.profile.dcacheMisses;
  c.dcacheWritebacks = self.profile.dcacheWritebacks;
  c.uncachedReads = rdram.profile.metrics[(u32)RBusDevice::VR4300_UNCACHED].reads;
  c.uncachedWrites = rdram.profile.metrics[(u32)RBusDevice::VR4300_UNCACHED].writes;
  return c;
}

auto CPU::Profiler::top() -> u32 {
  auto& stack = stacks[current];
  return stack.frames.empty() ? stack.root : stack.frames.back().node;
}

auto CPU::Profiler::charge() -> void {
  auto now = counters();
  if(recording) nodes[top()].self += now - last, eventCount++;
  last = now;
}

auto CPU::Profiler::child(u32 parent, u32 func) -> u32 {
  for(auto& [f, index] : nodes[parent].children) {
    if(f == func) return index;
  }
  u32 index = nodes.size();
  Node node;
  node.parent = parent;
  node.func = func;
  nodes.push_back(std::move(node));
  nodes[parent].children.push_back({func, index});
  return index;
}

auto CPU::Profiler::call(u32 target, u32 returnAddress) -> void {
  charge();
  auto& stack = stacks[current];
  if(stack.frames.size() >= MaxDepth) {
    //runaway stack (code that calls without returning): forget the oldest half
    //rather than the new call, so the code running now is still attributed
    stack.frames.erase(stack.frames.begin(), stack.frames.begin() + stack.frames.size() / 2);
    stackOverflows++;
  }
  u32 node = child(top(), resolve(target));
  countCall(node);
  stack.frames.push_back({node, returnAddress});
}

//The first jump to the ELF entry point is the boot code handing over to the
//program. Whatever the boot code left on the shadow stack never unwinds, so
//start again from the thread root. This happens once: e_entry is not always
//the real boot target, and must not repeatedly wipe the stack of a running game.
auto CPU::Profiler::enterProgram(u32 target) -> bool {
  if(entered || !entryPoint || target != entryPoint) return false;
  entered = true;
  charge();
  auto& stack = stacks[current];
  stack.frames.clear();
  stack.inException = false;
  stackOverflows = 0;  //overflows in boot code say nothing about the program's call tree
  call(target, 0xffff'ffff);
  return true;
}

auto CPU::Profiler::tailCall(u32 target) -> void {
  auto& stack = stacks[current];
  if(stack.frames.empty()) return;
  //the exception vector usually jumps to the real handler: keep the [exception] frame
  if(nodes[stack.frames.back().node].func == FuncException) return call(target, 0xffff'ffff);
  charge();
  u32 parent = nodes[stack.frames.back().node].parent;
  u32 node = child(parent, resolve(target));
  countCall(node);
  stack.frames.back().node = node;
}

auto CPU::Profiler::ret(u32 target) -> void {
  auto& stack = stacks[current];
  for(size_t n = stack.frames.size(); n-- > 0;) {
    if(stack.frames[n].returnAddress == target) {
      charge();
      stack.frames.resize(n);
      return;
    }
  }
  //no matching frame: a computed jump through $ra, or a return past where
  //profiling started; leave the shadow stack untouched
}

auto CPU::Profiler::switchThread(u32 thread) -> void {
  if(!stacks.empty()) {
    s64 t = now();
    span(TrackThreadBase + current, 0, threadStart, t);
    threadStart = t;
  }
  for(u32 n = 0; n < stacks.size(); n++) {
    if(stacks[n].thread == thread) { current = n; return; }
  }
  Stack stack;
  stack.thread = thread;
  //each thread gets its own root node (child() would merge them, since they share FuncThreadRoot)
  stack.root = nodes.size();
  Node root;
  root.func = FuncThreadRoot;
  nodes.push_back(std::move(root));
  stacks.push_back(std::move(stack));
  current = stacks.size() - 1;
}

auto CPU::Profiler::instruction(u64 vaddr, u32 instruction) -> void {
  if(written) return;
  u32 pc = (u32)vaddr;
  u32 opcode = instruction >> 26;
  u32 rs = instruction >> 21 & 31;
  auto& r = self.ipu.r;
  if(opcode == 0x00) {
    u32 funct = instruction & 63;
    if(funct == 0x08) {  //JR
      u32 target = r[rs].u32;
      if(enterProgram(target)) return;
      if(rs == 31) return ret(target);
      if(isFunctionStart(target) && resolve(target) != nodes[top()].func) return tailCall(target);
      return;
    }
    if(funct == 0x09) {  //JALR
      u32 rd = instruction >> 11 & 31;
      if(enterProgram(r[rs].u32)) return;
      if(rd == 0) return;
      return call(r[rs].u32, pc + 8);
    }
    return;
  }
  if(opcode == 0x03) {  //JAL
    u32 target = (u32)((vaddr + 4) & 0xf000'0000) | (instruction & 0x03ff'ffff) << 2;
    if(enterProgram(target)) return;
    return call(target, pc + 8);
  }
  if(opcode == 0x02) {  //J (tail call when it leaves the current function)
    u32 target = (u32)((vaddr + 4) & 0xf000'0000) | (instruction & 0x03ff'ffff) << 2;
    if(enterProgram(target)) return;
    if(isFunctionStart(target) && resolve(target) != nodes[top()].func) return tailCall(target);
    return;
  }
  if(opcode == 0x01) {  //REGIMM: BLTZAL, BGEZAL, BLTZALL, BGEZALL
    u32 rt = instruction >> 16 & 31;
    if(rt < 0x10 || rt > 0x13) return;
    bool negative = r[rs].s64 < 0;
    bool taken = (rt & 1) ? !negative : negative;
    if(!taken) return;
    u32 target = pc + 4 + ((s32)(s16)instruction << 2);
    return call(target, pc + 8);
  }
}

auto CPU::Profiler::exceptionEnter() -> void {
  if(written) return;
  auto& stack = stacks[current];
  if(stack.inException) return;
  charge();
  stack.inException = true;
  u32 node = child(top(), FuncException);
  countCall(node);
  stack.frames.push_back({node, 0xffff'ffff});
}

auto CPU::Profiler::exceptionReturn(u32 target) -> void {
  if(written) return;
  charge();
  auto& stack = stacks[current];
  if(stack.inException) {
    for(size_t n = stack.frames.size(); n-- > 0;) {
      if(nodes[stack.frames[n].node].func == FuncException) { stack.frames.resize(n); break; }
    }
    stack.inException = false;
  }
  if(runningThreadSymbol) {
    u32 thread = self.readDebug<Word>((s32)runningThreadSymbol);
    if(thread != stacks[current].thread) switchThread(thread);
  }
  resume(target);
}

//ERET resumes the (possibly different) thread at target. Frames left on its
//shadow stack from when it last stopped running (the yield path, or the thread
//dispatcher it jumped into) are stale: unwind to the frame that target belongs to.
auto CPU::Profiler::resume(u32 target) -> void {
  auto& stack = stacks[current];
  for(size_t n = stack.frames.size(); n-- > 0;) {
    if(stack.frames[n].returnAddress == target) { stack.frames.resize(n); return; }
    if(nodes[stack.frames[n].node].func == resolve(target)) { stack.frames.resize(n + 1); return; }
  }
  //nothing matches: leave the stack alone (code without symbols), unless it is
  //empty, which means a thread starting at its entry point
  if(stack.frames.empty() && isFunctionStart(target)) {
    u32 node = child(stack.root, resolve(target));
    countCall(node);
    stack.frames.push_back({node, 0xffff'ffff});
  }
}
auto CPU::Profiler::findSymbol(u32 address) const -> const Symbol* {
  auto it = std::upper_bound(symbols.begin(), symbols.end(), address, [](u32 a, const Symbol& s) { return a < s.address; });
  if(it == symbols.begin()) return nullptr;
  --it;
  if(address >= it->address && address - it->address < it->size) return &*it;
  return nullptr;
}

auto CPU::Profiler::resolve(u32 address) const -> u32 {
  if(auto symbol = findSymbol(address)) return symbol->address;
  return address;
}

auto CPU::Profiler::isFunctionStart(u32 address) const -> bool {
  auto symbol = findSymbol(address);
  return symbol && symbol->address == address;
}

auto CPU::Profiler::name(u32 func) const -> std::string {
  if(func == FuncException) return "[exception]";
  if(func == FuncThreadRoot) return "[root]";
  if(auto symbol = findSymbol(func); symbol && symbol->address == func) return symbol->name;
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "0x%08x", func);
  return buffer;
}

auto CPU::Profiler::nodeName(u32 node) const -> std::string {
  if(nodes[node].func == FuncThreadRoot) {
    for(auto& stack : stacks) {
      if(stack.root == node) {
        if(!runningThreadSymbol) return "[root]";
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "[thread 0x%08x]", stack.thread);
        return buffer;
      }
    }
  }
  return name(nodes[node].func);
}

//final output when the game is unloaded or reset: only the environment variable mode writes files
auto CPU::Profiler::dump() -> void {
  if(!enabled || written) return;
  if(fromEnvironment && captured && nodes.size() > 1) {
    stop();
    exportFiles(outputPath);
  }
  stop();
  written = true;
}

auto CPU::Profiler::aggregate() const -> Aggregate {
  Aggregate a;
  //inclusive costs: children always have higher indices than their parents
  a.inclusive.resize(nodes.size());
  for(size_t n = 0; n < nodes.size(); n++) a.inclusive[n] = nodes[n].self;
  for(size_t n = nodes.size(); n-- > 1;) a.inclusive[nodes[n].parent] += a.inclusive[n];

  //per-function aggregation; roots and thread pseudo-nodes keep their own names
  std::unordered_map<std::string, u32> functionIndex;
  auto function = [&](const std::string& name) -> FunctionCost& {
    auto it = functionIndex.find(name);
    if(it != functionIndex.end()) return a.functions[it->second];
    functionIndex[name] = a.functions.size();
    a.functions.push_back({name});
    return a.functions.back();
  };
  for(u32 n = 1; n < nodes.size(); n++) {
    auto& node = nodes[n];
    if(!node.calls && !a.inclusive[n].clocks && node.func != FuncThreadRoot) continue;  //only seen outside the capture
    auto fname = node.func == FuncThreadRoot ? rootName(n) : nodeName(n);
    {
      auto& f = function(fname);
      f.calls += node.calls;
      f.self += node.self;
    }
    //count inclusive time only at the outermost activation to avoid double counting recursion
    bool recursive = false;
    for(u32 p = node.parent; p; p = nodes[p].parent) {
      if(nodes[p].func == node.func && node.func != FuncThreadRoot) { recursive = true; break; }
    }
    if(!recursive) function(fname).inclusive += a.inclusive[n];
    if(node.parent) {
      u32 parent = node.parent;
      auto& e = function(nodes[parent].func == FuncThreadRoot ? rootName(parent) : nodeName(parent)).edge(fname);
      e.calls += node.calls;
      e.inclusive += a.inclusive[n];
    }
  }
  for(auto& f : a.functions) a.total += f.self;
  return a;
}

//a thread root is named after its thread and entry point
auto CPU::Profiler::rootName(u32 node) const -> std::string {
  for(u32 n = 0; n < stacks.size(); n++) {
    if(stacks[n].root == node) return threadName(n);
  }
  return nodeName(node);
}

auto CPU::Profiler::exportFiles(const std::string& prefix) -> bool {
  if(!enabled || !captured || nodes.size() <= 1) return false;
  if(recording) charge();
  auto a = aggregate();
  auto& inclusive = a.inclusive;
  auto& functions = a.functions;
  auto& total = a.total;
  bool ok = true;

  auto cycles = [](s64 clocks) { return clocks / 2; };  //187.5 MHz scheduler clocks -> 93.75 MHz CPU cycles
  auto costLine = [&](const Counters& c) {
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "%lld %lld %lld %lld %lld %lld %lld %lld",
      (long long)cycles(c.clocks), (long long)c.icacheHits, (long long)c.icacheMisses,
      (long long)c.dcacheHits, (long long)c.dcacheMisses, (long long)c.dcacheWritebacks,
      (long long)c.uncachedReads, (long long)c.uncachedWrites);
    return std::string{buffer};
  };

  //callgrind
  if(FILE* fp = fopen((prefix + ".callgrind").c_str(), "wb")) {
    fprintf(fp, "# callgrind format\nversion: 1\ncreator: ares-n64-profiler\n");
    fprintf(fp, "positions: line\n");
    fprintf(fp, "events: Cycles ICacheHit ICacheMiss DCacheHit DCacheMiss DCacheWriteback UncachedRdBytes UncachedWrBytes\n");
    fprintf(fp, "summary: %s\n\n", costLine(total).c_str());
    fprintf(fp, "ob=%s\nfl=%s\n", elfPath.empty() ? "rom" : elfPath.c_str(), "game");
    for(auto& f : functions) {
      fprintf(fp, "fn=%s\n0 %s\n", f.name.c_str(), costLine(f.self).c_str());
      for(auto& [callee, e] : f.callees) {
        fprintf(fp, "cfn=%s\ncalls=%llu 0\n0 %s\n", callee.c_str(), (unsigned long long)e.calls, costLine(e.inclusive).c_str());
      }
      fprintf(fp, "\n");
    }
    fclose(fp);
  } else ok = false;

  //folded stacks (one line per call tree node with self time)
  if(FILE* fp = fopen((prefix + ".folded").c_str(), "wb")) {
    std::vector<std::string> path;
    for(u32 n = 1; n < nodes.size(); n++) {
      s64 value = cycles(nodes[n].self.clocks);
      if(value <= 0) continue;
      path.clear();
      for(u32 p = n; p; p = nodes[p].parent) path.push_back(nodes[p].func == FuncThreadRoot ? rootName(p) : nodeName(p));
      std::string line;
      for(size_t i = path.size(); i-- > 0;) {
        line += path[i];
        if(i) line += ';';
      }
      fprintf(fp, "%s %lld\n", line.c_str(), (long long)value);
    }
    fclose(fp);
  } else ok = false;

  //flat text report sorted by self cycles
  if(FILE* fp = fopen((prefix + ".txt").c_str(), "wb")) {
    std::vector<FunctionCost*> sorted;
    for(auto& f : functions) sorted.push_back(&f);
    std::sort(sorted.begin(), sorted.end(), [](FunctionCost* a, FunctionCost* b) { return a->self.clocks > b->self.clocks; });
    fprintf(fp, "ares N64 profile: %lld CPU cycles (%.3f s at 93.75 MHz), %llu call/return events\n",
      (long long)cycles(total.clocks), cycles(total.clocks) / 93750000.0, (unsigned long long)eventCount);
    if(stackOverflows) fprintf(fp, "WARNING: shadow stack overflowed %llu times; call tree parents may be incomplete\n", (unsigned long long)stackOverflows);
    fprintf(fp, "I-cache: %lld hits, %lld misses. D-cache: %lld hits, %lld misses, %lld writebacks.\n\n",
      (long long)total.icacheHits, (long long)total.icacheMisses,
      (long long)total.dcacheHits, (long long)total.dcacheMisses, (long long)total.dcacheWritebacks);
    fprintf(fp, "%7s %14s %7s %14s %10s %10s %10s %10s %7s  %s\n",
      "self%", "self cycles", "incl%", "incl cycles", "calls", "I$ miss", "D$ miss", "D$ wb", "D$ hit%", "function");
    for(auto f : sorted) {
      double selfPct = total.clocks ? 100.0 * f->self.clocks / total.clocks : 0;
      double inclPct = total.clocks ? 100.0 * f->inclusive.clocks / total.clocks : 0;
      s64 dAccess = f->self.dcacheHits + f->self.dcacheMisses;
      double dHit = dAccess ? 100.0 * f->self.dcacheHits / dAccess : 100.0;
      fprintf(fp, "%6.2f%% %14lld %6.2f%% %14lld %10llu %10lld %10lld %10lld %6.1f%%  %s\n",
        selfPct, (long long)cycles(f->self.clocks), inclPct, (long long)cycles(f->inclusive.clocks),
        (unsigned long long)f->calls, (long long)f->self.icacheMisses, (long long)f->self.dcacheMisses,
        (long long)f->self.dcacheWritebacks, dHit, f->name.c_str());
    }
    writeTimelineSummary(fp);
    fclose(fp);
  } else ok = false;

  if(!writeTrace(prefix + ".trace.json")) ok = false;
  printf("[profiler] wrote %s.{callgrind,folded,txt,trace.json} (%d call tree nodes, %d timeline events)\n",
    prefix.c_str(), (int)nodes.size(), (int)(spans.size() + instants.size()));
  return ok;
}

//timeline

auto CPU::Profiler::threadName(u32 index) const -> std::string {
  auto& stack = stacks[index];
  std::string result = nodeName(stack.root);
  //name the thread after its entry point: the first function seen at its root
  if(!nodes[stack.root].children.empty()) result += " " + name(nodes[stack.root].children.front().first);
  return result;
}

auto CPU::Profiler::label(const std::string& name) -> u32 {
  for(u32 n = 0; n < labels.size(); n++) if(labels[n] == name) return n;
  labels.push_back(name);
  return labels.size() - 1;
}

auto CPU::Profiler::span(u32 track, u32 label, s64 start, s64 end) -> void {
  if(written || !recording || end <= start) return;
  if(spans.size() + instants.size() >= MaxTimelineEvents) {
    if(!droppedEvents++) timelineFullAt = start;
    return;
  }
  spans.push_back({track, label, start, end});
}

auto CPU::Profiler::instant(u32 track, u32 label, s64 time, double value) -> void {
  if(written || !recording) return;
  if(spans.size() + instants.size() >= MaxTimelineEvents) {
    if(!droppedEvents++) timelineFullAt = time;
    return;
  }
  instants.push_back({track, label, time, value});
}

auto CPU::Profiler::clearTimeline() -> void {
  spans.clear();
  instants.clear();
  droppedEvents = 0;
  timelineFullAt = -1;
  lastDmaSpan = -1;
  rspDmaBusyClocks = 0;
  rdpFrameSubmits = rdpFrameBytes = 0;
  lastRdpSpan = -1;
  rdpBusyClocks = 0;
  s64 t = now();
  threadStart = t;
  rspStart = t;
  rspDmaStart = t;
  rdpMeter = {};
  dmaMeter = {};
  dmaMeteredUntil = t;
  frameStartTime = t;
  haveOrigin = false;
  lastSwapFrame = 0;
  resumedOf.clear();
  yieldedLabel = prevRspLabel = lastRspLabel = ~0u;
  prevRspEnd = lastRspEnd = lastRspStart = t;
}

//close spans still open when the profile is written
auto CPU::Profiler::closeTimeline() -> void {
  s64 t = now();
  if(!stacks.empty()) span(TrackThreadBase + current, 0, threadStart, t);
  threadStart = t;
  if(rspRunning) span(TrackRSP, rspLabel, rspStart, t), rspStart = t;
  if(rspDmaBusy) {
    rspDmaBusyClocks += t - rspDmaStart;
    span(TrackRSPDMA, rspDmaLabel, rspDmaStart, t);
    rspDmaStart = t;
  }
}

auto CPU::Profiler::exactSymbol(u32 address) const -> const std::string* {
  auto it = std::lower_bound(exactSymbols.begin(), exactSymbols.end(), address,
    [](const std::pair<u32, std::string>& s, u32 a) { return s.first < a; });
  if(it != exactSymbols.end() && it->first == address) return &it->second;
  return nullptr;
}

//libultra's osSpTaskLoad copies the OSTask to the end of DMEM (0xfc0) before the RSP starts:
//  +0x00 type (1 = gfx, 2 = audio, ...), +0x04 flags (bit 0: OS_TASK_YIELDED, a resumed task),
//  +0x10 ucode (physical address)
//anything else (libdragon rspq, custom microcode) is named by a hash of IMEM
auto CPU::Profiler::rspTaskName(bool& libultra, bool& resumed) -> std::string {
  //DMEM is stored big-endian; read it directly to skip the debugger hook in RSP::Writable::read
  auto dmem = [&](u32 address) -> u32 { return bswap32(*(u32*)&rsp.dmem.data[address & rsp.dmem.maskWord]); };
  u32 type = dmem(0xfc0);
  u32 flags = dmem(0xfc4);
  u32 ucode = dmem(0xfd0);
  char buffer[96];
  libultra = resumed = false;
  if(type >= 1 && type <= 7 && ucode && (ucode & 0x1fff'ffff) < 0x0080'0000) {
    libultra = true;
    resumed = flags & 1;
    static const char* types[] = {"", "gfx", "audio", "video", "jpeg", "task5", "task6", "hvq"};
    u32 vaddr = 0x8000'0000 | (ucode & 0x1fff'ffff);
    if(auto symbol = exactSymbol(vaddr)) {
      snprintf(buffer, sizeof(buffer), "%s: %s", types[type], symbol->c_str());
    } else {
      snprintf(buffer, sizeof(buffer), "%s: ucode 0x%08x", types[type], vaddr);
    }
    return buffer;
  }
  u64 hash = XXH3_64bits(rsp.imem.data, 4096);
  snprintf(buffer, sizeof(buffer), "ucode %08x", (u32)hash);
  return buffer;
}

auto CPU::Profiler::rspPoll(s64 rspClock) -> void {
  if(written) return;
  s64 t = clockBase + rspClock;
  bool running = !rsp.status.halted;
  if(running != rspRunning) {
    if(running) {
      rspStart = t;
      bool libultra = false, resumed = false;
      u32 task = label(rspTaskName(libultra, resumed));
      if(!libultra) {
        //no OSTask: a task that runs again right after a different task cut in (within 0.5 ms each way)
        resumed = lastRspLabel != task && prevRspLabel == task
               && lastRspStart - prevRspEnd < 93750 && t - lastRspEnd < 93750;
      }
      //a libultra task that halted with SP_STATUS_YIELDED is resumed by the next load of the same task
      //(other tasks, such as audio, usually run in between)
      if(task == yieldedLabel) resumed = true, yieldedLabel = ~0u;
      rspLabel = task;
      if(resumed) {
        rspLabel = label(labels[task] + " (resumed)");
        resumedOf[rspLabel] = task;
      }
      prevRspLabel = lastRspLabel, prevRspEnd = lastRspEnd;
      lastRspLabel = task, lastRspStart = t;
      rspTaskLibultra = libultra;
    } else {
      span(TrackRSP, rspLabel, rspStart, t);
      lastRspEnd = t;
      //libultra's SP_STATUS_YIELDED (signal 1): the task saved its state and stopped early
      //(other microcode, such as libdragon's rspq, uses the signals for its own purposes)
      if(rspTaskLibultra && rsp.status.signal[1]) {
        auto it = resumedOf.find(rspLabel);
        yieldedLabel = it != resumedOf.end() ? it->second : rspLabel;
      }
    }
    rspRunning = running;
  }
  bool dma = (bool)rsp.dma.busy.any();
  if(dma != rspDmaBusy) {
    if(dma) {
      rspDmaStart = t;
      rspDmaLabel = label("SP DMA");
    } else if(recording) {
      rspDmaBusyClocks += t - rspDmaStart;
      dmaMeter.add(max(rspDmaStart, dmaMeteredUntil), t);
      //back-to-back DMAs (microcode streaming) merge into one span: 2000 clocks is about 10 us
      if(lastDmaSpan >= 0 && spans[lastDmaSpan].label == rspDmaLabel && rspDmaStart - spans[lastDmaSpan].end < MergeGap) {
        spans[lastDmaSpan].end = t;
      } else {
        size_t before = spans.size();
        span(TrackRSPDMA, rspDmaLabel, rspDmaStart, t);
        if(spans.size() > before) lastDmaSpan = before;
      }
    }
    rspDmaBusy = dma;
  }
}

//RDP command submissions are aggregated per VI frame (one counter sample per frame)
auto CPU::Profiler::rdpSubmit(s64 threadClock, u32 bytes) -> void {
  rdpFrameSubmits++;
  rdpFrameBytes += bytes;
}

auto CPU::Profiler::rdpBusy(s64 start, s64 end) -> void {
  if(written || !recording || end <= start) return;
  s64 t0 = clockBase + start, t1 = clockBase + end;
  rdpBusyClocks += t1 - t0;
  rdpMeter.add(t0, t1);
  //back-to-back submits merge into one span
  if(lastRdpSpan >= 0 && t0 - spans[lastRdpSpan].end < MergeGap) {
    spans[lastRdpSpan].end = t1;
    return;
  }
  size_t before = spans.size();
  span(TrackRDP, label("RDP"), t0, t1);
  if(spans.size() > before) lastRdpSpan = before;
}

//Chrome trace event format: open in https://ui.perfetto.dev or chrome://tracing
auto CPU::Profiler::writeTrace(const std::string& path) -> bool {
  FILE* fp = fopen(path.c_str(), "wb");
  if(!fp) return false;
  auto us = [](s64 clocks) { return (clocks < 0 ? 0 : clocks) / 187.5; };  //187.5 MHz -> microseconds
  auto escape = [](const std::string& s) {
    std::string r;
    for(char c : s) {
      if(c == '"' || c == '\\') r += '\\';
      if((u8)c < 0x20) continue;
      r += c;
    }
    return r;
  };
  fprintf(fp, "{\"displayTimeUnit\":\"ns\",\"traceEvents\":[\n");
  fprintf(fp, "{\"ph\":\"M\",\"pid\":1,\"name\":\"process_name\",\"args\":{\"name\":\"N64\"}}");
  auto track = [&](u32 tid, const std::string& name, u32 order) {
    fprintf(fp, ",\n{\"ph\":\"M\",\"pid\":1,\"tid\":%u,\"name\":\"thread_name\",\"args\":{\"name\":\"%s\"}}", tid, escape(name).c_str());
    fprintf(fp, ",\n{\"ph\":\"M\",\"pid\":1,\"tid\":%u,\"name\":\"thread_sort_index\",\"args\":{\"sort_index\":%u}}", tid, order);
  };
  track(TrackFrames, "VI frames", 0);
  track(TrackSwap, "Frames shown (VI origin changes)", 1);
  for(u32 n = 0; n < stacks.size(); n++) track(TrackThreadBase + n, "CPU " + threadName(n), 10 + n);
  track(TrackRSP, "RSP", 1000);
  track(TrackRSPDMA, "SP DMA", 1001);
  track(TrackRDP, "RDP (estimated)", 1002);
  for(auto& s : spans) {
    std::string name = s.track >= TrackThreadBase ? threadName(s.track - TrackThreadBase) : labels[s.label];
    fprintf(fp, ",\n{\"ph\":\"X\",\"pid\":1,\"tid\":%u,\"name\":\"%s\",\"ts\":%.3f,\"dur\":%.3f}",
      s.track, escape(name).c_str(), us(s.start), us(s.end) - us(s.start));
  }
  for(auto& i : instants) {
    if(i.track == TrackRDP || i.track == TrackCounter) {  //counter track
      fprintf(fp, ",\n{\"ph\":\"C\",\"pid\":1,\"name\":\"%s\",\"ts\":%.3f,\"args\":{\"value\":%.6g}}",
        escape(labels[i.label]).c_str(), us(i.time), i.value);
      continue;
    }
    fprintf(fp, ",\n{\"ph\":\"i\",\"s\":\"t\",\"pid\":1,\"tid\":%u,\"name\":\"%s\",\"ts\":%.3f,\"args\":{\"value\":%.0f}}",
      i.track, escape(labels[i.label]).c_str(), us(i.time), i.value);
  }
  fprintf(fp, "\n]}\n");
  fclose(fp);
  return true;
}

auto CPU::Profiler::summarize() const -> Summary {
  Summary s;
  //when the timeline filled up, percentages are relative to the part that was recorded
  s.fullRun = endTime();
  s.total = droppedEvents && timelineFullAt > 0 ? timelineFullAt : s.fullRun;
  //a yielded task and its resumptions count as one task; "longest" is the longest whole task (RSP time only)
  auto row = [&](u32 label) -> RspRow& {
    for(auto& r : s.rows) if(r.label == label) return r;
    s.rows.push_back({label});
    return s.rows.back();
  };
  for(auto& span : spans) {
    if(span.track != TrackRSP) continue;
    s64 d = span.end - span.start;
    s.rspBusy += d;
    auto it = resumedOf.find(span.label);
    if(it != resumedOf.end() && !s.rows.empty()) {
      auto& r = row(it->second);
      if(r.count) { r.yields++; r.busy += d; r.current += d; r.longest = max(r.longest, r.current); continue; }
    }
    auto& r = row(it != resumedOf.end() ? it->second : span.label);  //a resumption with no start recorded counts as a task
    r.count++; r.busy += d; r.current = d; r.longest = max(r.longest, d);
  }
  std::sort(s.rows.begin(), s.rows.end(), [](const RspRow& a, const RspRow& b) { return a.busy > b.busy; });
  for(auto& i : instants) if(i.track == TrackFrames) s.frameCount++;

  //frames shown (VI origin changes); the first one has no previous frame to measure from
  s64 lastShown = -1;
  for(auto& i : instants) {
    if(i.track != TrackSwap) continue;
    s.shown++;
    if(lastShown >= 0 && i.value > 0) s.frameTimes.push_back(i.time - lastShown), s.frameVIs.push_back((u32)i.value);
    lastShown = i.time;
  }
  return s;
}

auto CPU::Profiler::writeTimelineSummary(FILE* fp) -> void {
  auto summary = summarize();
  s64 fullRun = summary.fullRun;
  s64 total = summary.total;
  if(total <= 0 || fullRun <= 0) return;
  auto& rows = summary.rows;
  s64 rspBusy = summary.rspBusy;
  u64 frameCount = summary.frameCount;
  u64 shown = summary.shown;
  auto& frameTimes = summary.frameTimes;
  auto& frameVIs = summary.frameVIs;
  auto ms = [](s64 clocks) { return clocks / 187500.0; };
  if(!frameTimes.empty()) {
    auto pct = [](auto v, double p) { std::sort(v.begin(), v.end()); return v[(size_t)(p * (v.size() - 1) + 0.5)]; };
    s64 sum = 0; u64 viSum = 0;
    for(auto t : frameTimes) sum += t;
    for(auto v : frameVIs) viSum += v;
    double avg = ms(sum) / frameTimes.size();
    fprintf(fp, "\nFrames shown: %llu (VI origin changes) over %llu VI frames: %.2f fps\n",
      (unsigned long long)shown, (unsigned long long)frameCount, 1000.0 / avg);
    fprintf(fp, "  frame time ms: avg %.2f, median %.2f, p90 %.2f; VIs per frame: avg %.2f, median %u, p90 %u\n",
      avg, ms(pct(frameTimes, 0.5)), ms(pct(frameTimes, 0.9)),
      (double)viSum / frameVIs.size(), pct(frameVIs, 0.5), pct(frameVIs, 0.9));
    std::vector<u64> histogram;
    for(auto v : frameVIs) {
      if(v >= histogram.size()) histogram.resize(v + 1);
      histogram[v]++;
    }
    fprintf(fp, "  VIs per frame:");
    for(u32 v = 0; v < histogram.size(); v++) {
      if(histogram[v]) fprintf(fp, "  %u: %llu (%.1f%%)", v, (unsigned long long)histogram[v], 100.0 * histogram[v] / frameVIs.size());
    }
    fprintf(fp, "\n");
  } else {
    fprintf(fp, "\nFrames shown: %llu (VI origin changes) over %llu VI frames\n", (unsigned long long)shown, (unsigned long long)frameCount);
  }

  fprintf(fp, "\nRSP: busy %.2f%% (%.3f ms of %.3f ms), SP DMA busy %.2f%%%s, %llu frames recorded\n",
    100.0 * rspBusy / total, ms(rspBusy), ms(total), 100.0 * rspDmaBusyClocks / fullRun,
    droppedEvents ? " (full run)" : "", (unsigned long long)frameCount);
  fprintf(fp, "%7s %10s %8s %12s %12s %12s %10s  %s\n", "busy%", "tasks", "yields", "total ms", "avg ms", "longest ms", "per frame", "RSP task");
  for(auto& r : rows) {
    fprintf(fp, "%6.2f%% %10llu %8llu %12.3f %12.4f %12.4f %10.2f  %s\n",
      100.0 * r.busy / total, (unsigned long long)r.count, (unsigned long long)r.yields, ms(r.busy),
      r.count ? ms(r.busy) / r.count : 0.0, ms(r.longest),
      frameCount ? (double)r.count / frameCount : 0.0, labels[r.label].c_str());
  }
  fprintf(fp, "  (a task that yields to another and resumes counts once; avg and longest are per whole task;\n"
              "   per frame is tasks per VI frame)\n");
  if(rdp.timing.enabled) fprintf(fp, "\nRDP: busy %.2f%% (%.3f ms of %.3f ms, estimated by the RDP timing model)\n",
    100.0 * rdpBusyClocks / fullRun, ms(rdpBusyClocks), ms(fullRun));
  fprintf(fp, "note: in the trace, RDP and SP DMA spans less than %lld clocks (%.1f us) apart are drawn as one span,\n"
              "  so span lengths include those gaps; the busy %% above and the \"RDP busy %%\" / \"SP DMA busy %%\"\n"
              "  counter tracks (per VI frame) are exact\n", (long long)MergeGap, MergeGap / 187.5);
  if(droppedEvents) fprintf(fp, "warning: timeline full after %.3f ms; %llu later events dropped, figures above cover the recorded part only\n",
    ms(timelineFullAt), (unsigned long long)droppedEvents);
}

//results for the frontend

auto CPU::Profiler::report() -> Profiling::Report {
  Profiling::Report r;
  if(!enabled || !captured || nodes.size() <= 1) return r;
  if(recording) charge();
  auto a = aggregate();
  auto convert = [](const Counters& c) {
    Profiling::Counters o;
    o.cycles = c.clocks / 2;  //187.5 MHz scheduler clocks -> 93.75 MHz CPU cycles
    o.icacheHits = c.icacheHits; o.icacheMisses = c.icacheMisses;
    o.dcacheHits = c.dcacheHits; o.dcacheMisses = c.dcacheMisses; o.dcacheWritebacks = c.dcacheWritebacks;
    o.uncachedReads = c.uncachedReads; o.uncachedWrites = c.uncachedWrites;
    return o;
  };
  r.valid = true;
  r.symbols = elfPath;
  r.total = convert(a.total);
  r.events = eventCount;
  r.stackOverflows = stackOverflows;
  r.droppedTimelineEvents = droppedEvents;
  r.partialCallTree = partial;

  for(auto& f : a.functions) {
    Profiling::Function o;
    o.name = f.name;
    o.calls = f.calls;
    o.self = convert(f.self);
    o.inclusive = convert(f.inclusive);
    r.functions.push_back(std::move(o));
  }
  std::sort(r.functions.begin(), r.functions.end(), [](auto& x, auto& y) { return x.self.cycles > y.self.cycles; });

  //call tree: nodes only seen outside the capture are left out (their name stays empty)
  auto kept = [&](u32 n) { return nodes[n].calls || a.inclusive[n].clocks || nodes[n].func == FuncThreadRoot; };
  r.tree.resize(nodes.size());
  for(u32 n = 1; n < nodes.size(); n++) {
    if(!kept(n)) continue;
    auto& node = nodes[n];
    auto& o = r.tree[n];
    o.name = node.func == FuncThreadRoot ? rootName(n) : nodeName(n);
    o.parent = node.parent;
    o.depth = node.parent ? r.tree[node.parent].depth + 1 : 0;  //parents have lower indices
    o.calls = node.calls;
    o.self = convert(node.self);
    o.inclusive = convert(a.inclusive[n]);
    for(auto& [func, child] : node.children) if(kept(child)) o.children.push_back(child);
    std::sort(o.children.begin(), o.children.end(), [&](u32 x, u32 y) { return a.inclusive[x].clocks > a.inclusive[y].clocks; });
    if(!node.parent && a.inclusive[n].clocks > 0) r.roots.push_back(n);
  }
  std::sort(r.roots.begin(), r.roots.end(), [&](u32 x, u32 y) { return a.inclusive[x].clocks > a.inclusive[y].clocks; });

  auto s = summarize();
  auto ms = [](s64 clocks) { return clocks / 187500.0; };
  r.milliseconds = ms(s.fullRun);
  r.viFrames = s.frameCount;
  r.framesShown = s.shown;
  if(!s.frameTimes.empty()) {
    auto times = s.frameTimes;
    std::sort(times.begin(), times.end());
    s64 sum = 0;
    for(auto t : times) sum += t;
    r.frameMsAverage = ms(sum) / times.size();
    r.frameMsMedian = ms(times[(size_t)(0.5 * (times.size() - 1) + 0.5)]);
    r.frameMsP90 = ms(times[(size_t)(0.9 * (times.size() - 1) + 0.5)]);
    r.fps = r.frameMsAverage > 0 ? 1000.0 / r.frameMsAverage : 0;
  }
  if(s.total > 0) r.rspBusy = 100.0 * s.rspBusy / s.total;
  if(s.fullRun > 0) r.spDmaBusy = 100.0 * rspDmaBusyClocks / s.fullRun;
  r.rdpModelled = rdp.timing.enabled;
  if(s.fullRun > 0 && r.rdpModelled) r.rdpBusy = 100.0 * rdpBusyClocks / s.fullRun;
  for(auto& row : s.rows) {
    Profiling::RspTask t;
    t.name = labels[row.label];
    t.count = row.count;
    t.yields = row.yields;
    t.busyMs = ms(row.busy);
    t.longestMs = ms(row.longest);
    r.rspTasks.push_back(std::move(t));
  }
  return r;
}

namespace Profiling {

auto status() -> Status {
  Status s;
  auto& p = cpu.profiler;
  s.gameLoaded = (bool)cpu.node;
  if(!s.gameLoaded) return s;
  if(p.pendingEnable || p.pendingStart) s.state = State::Waiting;
  else if(!p.enabled || p.written) s.state = State::Off;
  else s.state = p.recording ? State::Recording : State::Idle;
  s.armedAtBoot = p.enabled && p.armedAtBoot;
  s.symbols = p.elfPath;
  s.symbolCount = p.symbols.size();
  s.frames = p.frames;
  if(p.captured) {
    s.captureFrames = p.captureFrames;
    s.captureMs = p.endTime() / 187500.0;
    s.timelineEvents = p.spans.size() + p.instants.size();
  }
  return s;
}

auto setArmed(bool armed) -> void {
  cpu.profiler.optionEnabled = armed;
}

auto start() -> bool {
  if(!cpu.node) return false;
  return cpu.profiler.start();
}

auto stop() -> bool {
  if(!cpu.node) return false;
  return cpu.profiler.stop();
}

auto report() -> Report {
  if(!cpu.node) return {};
  return cpu.profiler.report();
}

//symbols can change at any time; while the profiler runs, its call tree starts again
//(the old one refers to functions by the old symbols)
auto loadSymbols(const std::string& elf) -> bool {
  auto& p = cpu.profiler;
  auto symbols = p.symbols;
  auto exactSymbols = p.exactSymbols;
  auto runningThreadSymbol = p.runningThreadSymbol;
  auto entryPoint = p.entryPoint;
  if(!p.loadSymbols(elf)) {
    p.symbols = std::move(symbols);
    p.exactSymbols = std::move(exactSymbols);
    p.runningThreadSymbol = runningThreadSymbol;
    p.entryPoint = entryPoint;
    return false;
  }
  p.elfPath = elf;
  p.optionSymbols = elf;
  if(cpu.node && p.enabled && !p.written) {
    bool recording = p.recording;
    p.reset();
    p.partial = true;
    p.entered = true;  //the program is already running: the jump to the entry point has been seen
    if(recording) p.start();
  }
  return true;
}

auto exportFiles(const std::string& prefix) -> bool {
  if(!cpu.node) return false;
  return cpu.profiler.exportFiles(prefix);
}

}
