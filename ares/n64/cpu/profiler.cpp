//Configuration comes from environment variables so the profiler needs no UI:
//  ARES_N64_PROFILE_ELF=<path to game .elf>   enables profiling with symbols
//  ARES_N64_PROFILE=1                         enables profiling without symbols
//  ARES_N64_PROFILE_OUT=<path prefix>         output prefix (default: <elf>.profile or ares-n64.profile)
//  ARES_N64_PROFILE_SKIP=<frames>             discard everything before this frame (skip boot)
//  ARES_N64_PROFILE_FRAMES=<frames>           after this many frames: write output and quit ares
//Output is written when the game is unloaded or reset:
//  <prefix>.callgrind  (KCachegrind / QCachegrind)
//  <prefix>.folded     (speedscope, flamegraph.pl, inferno)
//  <prefix>.txt        (flat per-function table)
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
  elfPath = get("ARES_N64_PROFILE_ELF");
  outputPath = get("ARES_N64_PROFILE_OUT");
  enabled = !elfPath.empty() || get("ARES_N64_PROFILE") == "1";
  skipFrames = std::strtoull(get("ARES_N64_PROFILE_SKIP").c_str(), nullptr, 10);
  stopFrame = std::strtoull(get("ARES_N64_PROFILE_FRAMES").c_str(), nullptr, 10);
  if(!enabled) return;
  if(outputPath.empty()) outputPath = elfPath.empty() ? "ares-n64.profile" : elfPath + ".profile";
  symbols.clear();
  exactSymbols.clear();
  runningThreadSymbol = 0;
  if(!elfPath.empty()) {
    if(loadSymbols(elfPath)) {
      printf("[profiler] loaded %d symbols from %s\n", (int)symbols.size(), elfPath.c_str());
    } else {
      printf("[profiler] could not read symbols from %s\n", elfPath.c_str());
    }
  }
  printf("[profiler] enabled, output prefix %s\n", outputPath.c_str());
}

auto CPU::Profiler::loadSymbols(const std::string& path) -> bool {
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
  clearTimeline();
  switchThread(0);
  last = counters();
}

auto CPU::Profiler::clearCosts() -> void {
  for(auto& node : nodes) { node.self = {}; node.calls = 0; }
  last = counters();
  clockBase -= last.clocks;  //restart the clock at zero
  last.clocks = 0;
  eventCount = 0;
  clearTimeline();
}

auto CPU::Profiler::frame() -> void {
  if(written) return;
  if(!vi.active()) return;  //while the VI is off, ares reports arbitrary refreshes; only count real frames
  frames++;
  instant(TrackFrames, label("frame"), now(), frames);
  if(skipFrames && frames == skipFrames) {
    clearCosts();
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
  nodes[top()].self += now - last;
  last = now;
  eventCount++;
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
  if(stack.frames.size() >= MaxDepth) return;
  u32 node = child(top(), resolve(target));
  nodes[node].calls++;
  stack.frames.push_back({node, returnAddress});
}

auto CPU::Profiler::tailCall(u32 target) -> void {
  auto& stack = stacks[current];
  if(stack.frames.empty()) return;
  //the exception vector usually jumps to the real handler: keep the [exception] frame
  if(nodes[stack.frames.back().node].func == FuncException) return call(target, 0xffff'ffff);
  charge();
  u32 parent = nodes[stack.frames.back().node].parent;
  u32 node = child(parent, resolve(target));
  nodes[node].calls++;
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
      if(rs == 31) return ret(target);
      if(isFunctionStart(target) && resolve(target) != nodes[top()].func) return tailCall(target);
      return;
    }
    if(funct == 0x09) {  //JALR
      u32 rd = instruction >> 11 & 31;
      if(rd == 0) return;
      return call(r[rs].u32, pc + 8);
    }
    return;
  }
  if(opcode == 0x03) {  //JAL
    u32 target = (u32)((vaddr + 4) & 0xf000'0000) | (instruction & 0x03ff'ffff) << 2;
    return call(target, pc + 8);
  }
  if(opcode == 0x02) {  //J (tail call when it leaves the current function)
    u32 target = (u32)((vaddr + 4) & 0xf000'0000) | (instruction & 0x03ff'ffff) << 2;
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
  nodes[node].calls++;
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
    nodes[node].calls++;
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

auto CPU::Profiler::dump() -> void {
  if(!enabled || written || nodes.size() <= 1) return;
  charge();
  closeTimeline();
  written = true;

  //inclusive costs: children always have higher indices than their parents
  std::vector<Counters> inclusive(nodes.size());
  for(size_t n = 0; n < nodes.size(); n++) inclusive[n] = nodes[n].self;
  for(size_t n = nodes.size(); n-- > 1;) inclusive[nodes[n].parent] += inclusive[n];

  auto cycles = [](s64 clocks) { return clocks / 2; };  //187.5 MHz scheduler clocks -> 93.75 MHz CPU cycles
  auto costLine = [&](const Counters& c) {
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "%lld %lld %lld %lld %lld %lld %lld %lld",
      (long long)cycles(c.clocks), (long long)c.icacheHits, (long long)c.icacheMisses,
      (long long)c.dcacheHits, (long long)c.dcacheMisses, (long long)c.dcacheWritebacks,
      (long long)c.uncachedReads, (long long)c.uncachedWrites);
    return std::string{buffer};
  };

  //per-function aggregation; roots and thread pseudo-nodes keep their own names
  struct Edge { u64 calls = 0; Counters inclusive; };
  struct Function {
    std::string name;
    u64 calls = 0;
    Counters self, inclusive;
    std::vector<std::pair<std::string, Edge>> callees;
    auto edge(const std::string& callee) -> Edge& {
      for(auto& [n, e] : callees) if(n == callee) return e;
      callees.push_back({callee, {}});
      return callees.back().second;
    }
  };
  std::vector<Function> functions;
  std::unordered_map<std::string, u32> functionIndex;
  auto function = [&](const std::string& name) -> Function& {
    auto it = functionIndex.find(name);
    if(it != functionIndex.end()) return functions[it->second];
    functionIndex[name] = functions.size();
    functions.push_back({name});
    return functions.back();
  };
  for(u32 n = 1; n < nodes.size(); n++) {
    auto fname = nodeName(n);
    auto& f = function(fname);
    f.calls += nodes[n].calls;
    f.self += nodes[n].self;
    //count inclusive time only at the outermost activation to avoid double counting recursion
    bool recursive = false;
    for(u32 p = nodes[n].parent; p; p = nodes[p].parent) {
      if(nodes[p].func == nodes[n].func && nodes[n].func != FuncThreadRoot) { recursive = true; break; }
    }
    if(!recursive) function(fname).inclusive += inclusive[n];
    if(nodes[n].parent) {
      auto& e = function(nodeName(nodes[n].parent)).edge(fname);
      e.calls += nodes[n].calls;
      e.inclusive += inclusive[n];
    }
  }

  Counters total;
  for(auto& f : functions) total += f.self;

  //callgrind
  if(FILE* fp = fopen((outputPath + ".callgrind").c_str(), "wb")) {
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
  }

  //folded stacks (one line per call tree node with self time)
  if(FILE* fp = fopen((outputPath + ".folded").c_str(), "wb")) {
    std::vector<std::string> path;
    for(u32 n = 1; n < nodes.size(); n++) {
      s64 value = cycles(nodes[n].self.clocks);
      if(value <= 0) continue;
      path.clear();
      for(u32 p = n; p; p = nodes[p].parent) path.push_back(nodeName(p));
      std::string line;
      for(size_t i = path.size(); i-- > 0;) {
        line += path[i];
        if(i) line += ';';
      }
      fprintf(fp, "%s %lld\n", line.c_str(), (long long)value);
    }
    fclose(fp);
  }

  //flat text report sorted by self cycles
  if(FILE* fp = fopen((outputPath + ".txt").c_str(), "wb")) {
    std::vector<Function*> sorted;
    for(auto& f : functions) sorted.push_back(&f);
    std::sort(sorted.begin(), sorted.end(), [](Function* a, Function* b) { return a->self.clocks > b->self.clocks; });
    fprintf(fp, "ares N64 profile: %lld CPU cycles (%.3f s at 93.75 MHz), %llu call/return events\n",
      (long long)cycles(total.clocks), cycles(total.clocks) / 93750000.0, (unsigned long long)eventCount);
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
  }

  writeTrace();
  printf("[profiler] wrote %s.{callgrind,folded,txt,trace.json} (%d call tree nodes, %d timeline events)\n",
    outputPath.c_str(), (int)nodes.size(), (int)(spans.size() + instants.size()));
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
  if(written || end <= start) return;
  if(spans.size() + instants.size() >= MaxTimelineEvents) { droppedEvents++; return; }
  spans.push_back({track, label, start, end});
}

auto CPU::Profiler::instant(u32 track, u32 label, s64 time, u32 value) -> void {
  if(written) return;
  if(spans.size() + instants.size() >= MaxTimelineEvents) { droppedEvents++; return; }
  instants.push_back({track, label, time, value});
}

auto CPU::Profiler::clearTimeline() -> void {
  spans.clear();
  instants.clear();
  droppedEvents = 0;
  s64 t = now();
  threadStart = t;
  rspStart = t;
  rspDmaStart = t;
}

//close spans still open when the profile is written
auto CPU::Profiler::closeTimeline() -> void {
  s64 t = now();
  if(!stacks.empty()) span(TrackThreadBase + current, 0, threadStart, t);
  threadStart = t;
  if(rspRunning) span(TrackRSP, rspLabel, rspStart, t), rspStart = t;
  if(rspDmaBusy) span(TrackRSPDMA, rspDmaLabel, rspDmaStart, t), rspDmaStart = t;
}

auto CPU::Profiler::exactSymbol(u32 address) const -> const std::string* {
  auto it = std::lower_bound(exactSymbols.begin(), exactSymbols.end(), address,
    [](const std::pair<u32, std::string>& s, u32 a) { return s.first < a; });
  if(it != exactSymbols.end() && it->first == address) return &it->second;
  return nullptr;
}

//libultra's osSpTaskLoad copies the OSTask to the end of DMEM (0xfc0) before the RSP starts:
//  +0x00 type (1 = gfx, 2 = audio, ...), +0x10 ucode (physical address)
//anything else (libdragon rspq, custom microcode) is named by a hash of IMEM
auto CPU::Profiler::rspTaskName() -> std::string {
  //DMEM is stored big-endian; read it directly to skip the debugger hook in RSP::Writable::read
  auto dmem = [&](u32 address) -> u32 { return bswap32(*(u32*)&rsp.dmem.data[address & rsp.dmem.maskWord]); };
  u32 type = dmem(0xfc0);
  u32 ucode = dmem(0xfd0);
  char buffer[96];
  if(type >= 1 && type <= 7 && ucode && (ucode & 0x1fff'ffff) < 0x0080'0000) {
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
      rspLabel = label(rspTaskName());
    } else {
      span(TrackRSP, rspLabel, rspStart, t);
    }
    rspRunning = running;
  }
  bool dma = (bool)rsp.dma.busy.any();
  if(dma != rspDmaBusy) {
    if(dma) {
      rspDmaStart = t;
      rspDmaLabel = label(rsp.dma.busy.read ? "RDRAM -> SP" : "SP -> RDRAM");
    } else {
      span(TrackRSPDMA, rspDmaLabel, rspDmaStart, t);
    }
    rspDmaBusy = dma;
  }
}

auto CPU::Profiler::rdpSubmit(s64 threadClock, u32 bytes) -> void {
  instant(TrackRDP, label("RDP submit"), clockBase + threadClock, bytes);
}

//Chrome trace event format: open in https://ui.perfetto.dev or chrome://tracing
auto CPU::Profiler::writeTrace() -> void {
  FILE* fp = fopen((outputPath + ".trace.json").c_str(), "wb");
  if(!fp) return;
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
  for(u32 n = 0; n < stacks.size(); n++) track(TrackThreadBase + n, "CPU " + threadName(n), 10 + n);
  track(TrackRSP, "RSP", 1000);
  track(TrackRSPDMA, "SP DMA", 1001);
  track(TrackRDP, "RDP submits", 1002);
  for(auto& s : spans) {
    std::string name = s.track >= TrackThreadBase ? threadName(s.track - TrackThreadBase) : labels[s.label];
    fprintf(fp, ",\n{\"ph\":\"X\",\"pid\":1,\"tid\":%u,\"name\":\"%s\",\"ts\":%.3f,\"dur\":%.3f}",
      s.track, escape(name).c_str(), us(s.start), us(s.end) - us(s.start));
  }
  for(auto& i : instants) {
    fprintf(fp, ",\n{\"ph\":\"i\",\"s\":\"t\",\"pid\":1,\"tid\":%u,\"name\":\"%s\",\"ts\":%.3f,\"args\":{\"value\":%u}}",
      i.track, escape(labels[i.label]).c_str(), us(i.time), i.value);
  }
  fprintf(fp, "\n]}\n");
  fclose(fp);
}

auto CPU::Profiler::writeTimelineSummary(FILE* fp) -> void {
  s64 total = now();
  if(total <= 0) return;
  struct Row { std::string name; u64 count = 0; s64 busy = 0, longest = 0; };
  std::vector<Row> rows;
  auto row = [&](const std::string& name) -> Row& {
    for(auto& r : rows) if(r.name == name) return r;
    rows.push_back({name});
    return rows.back();
  };
  s64 rspBusy = 0, dmaBusy = 0;
  for(auto& s : spans) {
    s64 d = s.end - s.start;
    if(s.track == TrackRSP) {
      rspBusy += d;
      auto& r = row(labels[s.label]);
      r.count++; r.busy += d; r.longest = max(r.longest, d);
    }
    if(s.track == TrackRSPDMA) dmaBusy += d;
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.busy > b.busy; });
  u64 frameCount = 0;
  for(auto& i : instants) if(i.track == TrackFrames) frameCount++;
  auto ms = [](s64 clocks) { return clocks / 187500.0; };
  fprintf(fp, "\nRSP: busy %.2f%% (%.3f ms of %.3f ms), SP DMA busy %.2f%%, %llu frames recorded\n",
    100.0 * rspBusy / total, ms(rspBusy), ms(total), 100.0 * dmaBusy / total, (unsigned long long)frameCount);
  fprintf(fp, "%7s %10s %12s %12s %12s %10s  %s\n", "busy%", "tasks", "total ms", "avg ms", "longest ms", "per frame", "RSP task");
  for(auto& r : rows) {
    fprintf(fp, "%6.2f%% %10llu %12.3f %12.4f %12.4f %10.2f  %s\n",
      100.0 * r.busy / total, (unsigned long long)r.count, ms(r.busy), ms(r.busy) / r.count, ms(r.longest),
      frameCount ? (double)r.count / frameCount : 0.0, r.name.c_str());
  }
  if(droppedEvents) fprintf(fp, "warning: timeline full, %llu events dropped\n", (unsigned long long)droppedEvents);
}
