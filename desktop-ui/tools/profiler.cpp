//N64 CPU profiler panel: start and stop captures at runtime and browse the last one.
//The profiler itself lives in the N64 core (ares/n64/cpu/profiler.cpp); this panel only
//talks to it through n64/profiler.hpp, always under a Program::Guard.

#if !defined(ELLIPSIS)
#define ELLIPSIS "…"
#endif

#if defined(CORE_N64)

namespace Profiling = ares::Nintendo64::Profiling;

namespace {

auto fmt(const char* format, auto... args) -> string {
  char buffer[512];
  snprintf(buffer, sizeof(buffer), format, args...);
  return buffer;
}

//12,345,678
auto grouped(s64 value) -> string {
  char digits[32];
  snprintf(digits, sizeof(digits), "%lld", (long long)(value < 0 ? -value : value));
  string result;
  u32 length = strlen(digits);
  for(u32 n = 0; n < length; n++) {
    if(n && (length - n) % 3 == 0) result.append(",");
    result.append(digits[n]);
  }
  return value < 0 ? string{"-", result} : result;
}

auto percent(s64 part, s64 whole) -> string {
  return whole ? fmt("%.2f", 100.0 * part / whole) : string{"-"};
}

auto hitRate(s64 hits, s64 misses) -> string {
  return hits + misses ? fmt("%.1f", 100.0 * hits / (hits + misses)) : string{"-"};
}

}

auto ProfilerViewer::construct() -> void {
  setCollapsible();
  setVisible(false);

  profilerLabel.setText("N64 Profiler").setFont(Font().setBold());

  startButton.setText("Start").onActivate([&] { eventStart(); });
  stopButton.setText("Stop").onActivate([&] { eventStop(); });
  exportButton.setText("Export" ELLIPSIS).onActivate([&] { eventExport(); });

  symbolsLabel.setText("Symbols:");
  symbolsButton.setText("Load" ELLIPSIS).onActivate([&] { eventLoadSymbols(); });

  armOption.setText("Profile from power on").setChecked(settings.nintendo64.profiler).onToggle([&] {
    settings.nintendo64.profiler = armOption.checked();
    if(supported()) {
      Program::Guard guard;
      Profiling::setArmed(armOption.checked());
    }
  });
  armHint.setText("Complete call trees; costs some speed. Applies when a game is loaded or reset.")
    .setFont(Font().setSize(7.0)).setForegroundColor(SystemColor::Sublabel);

  summaryTab.setText("Summary");
  summaryView.setEditable(false).setWordWrap(false).setFont(Font().setFamily(Font::Mono));

  functionsTab.setText("Functions");
  filterLabel.setText("Filter:");
  filterEdit.onChange([&] { showFunctions(); });
  functionList.setHeadered().setSortable().onSort([&](TableViewColumn column) {
    functionSort = column.offset();
    showFunctions();
  });

  treeTab.setText("Call Tree");
  hotPathButton.setText("Expand hot path").onActivate([&] {
    expandHotPath();
    showTree();
  });
  collapseButton.setText("Collapse all").onActivate([&] {
    expanded.clear();
    showTree();
  });
  treeHint.setText("Double-click a function to expand or collapse it")
    .setFont(Font().setSize(7.0)).setForegroundColor(SystemColor::Sublabel);
  treeList.setHeadered().onActivate([&](TableViewCell cell) {
    if(auto item = treeList.selected()) {
      u32 row = item.offset();
      if(row >= treeRows.size()) return;
      u32 node = treeRows[row];
      if(!node || node >= report.tree.size() || report.tree[node].children.empty()) return;
      if(expanded.count(node)) expanded.erase(node); else expanded.insert(node);
      showTree();
      if(row < treeList.itemCount()) treeList.item(row).setSelected().setFocused();
    }
  });

  rspTab.setText("RSP Tasks");
  rspList.setHeadered();

  tabs.item(0).setSelected();
  reload();
}

auto ProfilerViewer::supported() const -> bool {
  return emulator && emulator->name == "Nintendo 64";
}

//called while the game loads, before emulator points to it
auto ProfilerViewer::symbolsFor(string location) const -> string {
  if(!location) return {};
  if(chosenGame == location && chosenSymbols && file::exists(chosenSymbols)) return chosenSymbols;
  return defaultSymbolsFor(location);
}

auto ProfilerViewer::defaultSymbolsFor(string location) -> string {
  if(!location) return {};
  if(location.endsWith("/")) location.trimRight("/", 1L);
  string elf = {Location::notsuffix(location), ".elf"};
  if(file::exists(elf)) return elf;
  return {};
}

auto ProfilerViewer::symbols() const -> string {
  if(!emulator || !emulator->game) return {};
  return symbolsFor(emulator->game->location);
}

auto ProfilerViewer::reload() -> void {
  report = {};
  treeRows.clear();
  expanded.clear();
  lastState = Profiling::State::Off;
  if(supported()) {
    Program::Guard guard;
    report = Profiling::report();
  }
  if(report.valid) expandHotPath();
  showSummary();
  showFunctions();
  showTree();
  showRsp();
  refreshStatus();
}

auto ProfilerViewer::unload() -> void {
  report = {};
  treeRows.clear();
  expanded.clear();
  showSummary();
  showFunctions();
  showTree();
  showRsp();
  refreshStatus();
}

auto ProfilerViewer::setVisible(bool visible) -> ProfilerViewer& {
  if(visible) refreshStatus();
  VerticalLayout::setVisible(visible);
  return *this;
}

//the status line follows the capture while the panel is open
auto ProfilerViewer::liveRefresh() -> void {
  if(!visible() || !supported()) return;
  u64 now = chrono::millisecond();
  if(now - lastRefresh < 250) return;
  lastRefresh = now;
  refreshStatus();
}

auto ProfilerViewer::refreshStatus() -> void {
  bool n64 = supported();
  Profiling::Status status;
  if(n64) {
    Program::Guard guard;
    status = Profiling::status();
  }
  //an enable or start requested while paused, or in a frame still running, completes later
  if(lastState == Profiling::State::Waiting && status.state == Profiling::State::Recording) report = {};
  lastState = status.state;

  using State = Profiling::State;
  bool recording = status.state == State::Recording || status.state == State::Waiting;
  startButton.setEnabled(n64 && status.gameLoaded && !recording);
  stopButton.setEnabled(n64 && recording);
  exportButton.setEnabled(n64 && !recording && report.valid);
  symbolsButton.setEnabled(n64 && status.gameLoaded);

  string text;
  if(!emulator) text = "No game loaded";
  else if(!n64) text = "The profiler is only available for Nintendo 64 games";
  else if(status.state == State::Waiting) text = "Starts at the end of the next emulated frame" ELLIPSIS;
  else if(status.state == State::Off) text = report.valid ? "Off" : "Off: Start enables it mid-game";
  else if(status.state == State::Recording) {
    text = fmt("Recording: %llu frames, %.1f ms emulated", (unsigned long long)status.captureFrames, status.captureMs);
  } else {
    text = status.armedAtBoot ? "Ready (profiling since power on)" : "Ready (enabled mid-game)";
    if(report.valid) text.append(fmt(", last capture %llu frames", (unsigned long long)status.captureFrames));
  }
  statusLabel.setText(text);

  if(!n64) symbolsPath.setText("-");
  else if(!status.symbols.empty()) symbolsPath.setText({status.symbols.c_str(), " (", status.symbolCount, " functions)"});
  else symbolsPath.setText("None: functions are shown as addresses (put <rom>.elf next to the ROM, or Load)");
}

auto ProfilerViewer::eventStart() -> void {
  if(!supported()) return;
  {
    Program::Guard guard;
    Profiling::start();
  }
  report = {};
  treeRows.clear();
  expanded.clear();
  showSummary();
  showFunctions();
  showTree();
  showRsp();
  refreshStatus();
}

auto ProfilerViewer::eventStop() -> void {
  if(!supported()) return;
  {
    Program::Guard guard;
    Profiling::stop();
    report = Profiling::report();
  }
  expanded.clear();
  if(report.valid) expandHotPath();
  showSummary();
  showFunctions();
  showTree();
  showRsp();
  refreshStatus();
}

auto ProfilerViewer::eventExport() -> void {
  if(!supported() || !report.valid) return;
  BrowserDialog dialog;
  dialog.setTitle("Export Profile");
  dialog.setAlignment(toolsWindow);
  string base = emulator->game ? Location::notsuffix(emulator->game->location) : string{"ares-n64"};
  dialog.setPath(Location::path(base));
  dialog.setName({Location::file(base), ".profile"});
  dialog.setFilters({"Profile (callgrind, folded, txt, trace.json)|*.callgrind:*.folded:*.txt:*.json", "All|*"});
  auto location = dialog.saveFile();
  if(!location) return;
  //the prefix: drop an extension the user typed or picked
  for(auto suffix : {".callgrind", ".folded", ".txt", ".trace.json", ".json"}) {
    if(location.endsWith(suffix)) { location.trimRight(suffix, 1L); break; }
  }
  bool ok;
  {
    Program::Guard guard;
    ok = Profiling::exportFiles((const char*)location);
  }
  if(ok) {
    statusLabel.setText({"Exported ", Location::file(location), ".{callgrind,folded,txt,trace.json}"});
  } else {
    MessageDialog().setTitle("Export Profile").setText({"Could not write ", location, ".*"}).setAlignment(toolsWindow).error();
  }
}

auto ProfilerViewer::eventLoadSymbols() -> void {
  if(!supported() || !emulator->game) return;
  BrowserDialog dialog;
  dialog.setTitle("Load Symbols");
  dialog.setAlignment(toolsWindow);
  dialog.setPath(Location::path(emulator->game->location));
  dialog.setFilters({"ELF|*.elf", "All|*"});
  auto location = dialog.openFile();
  if(!location) return;
  bool ok;
  {
    Program::Guard guard;
    ok = Profiling::loadSymbols((const char*)location);
  }
  if(!ok) {
    MessageDialog().setTitle("Load Symbols").setText({"No symbols found in ", location}).setAlignment(toolsWindow).error();
    return;
  }
  chosenGame = emulator->game->location;
  chosenSymbols = location;
  refreshStatus();
}

auto ProfilerViewer::showSummary() -> void {
  if(!report.valid) {
    summaryView.setText(
      "No capture yet.\n\n"
      "Start records CPU time per function (with cache statistics), RSP tasks, SP DMA and\n"
      "estimated RDP busy time, and frame times; Stop shows the results here.\n\n"
      "With \"Profile from power on\" the profiler follows every call from boot, so call trees\n"
      "are complete. Without it, Start enables the profiler mid-game: functions that are\n"
      "already running then (the main loop, thread entry points) are missing from the top\n"
      "of the call tree, though their callees are measured correctly.\n\n"
      "Rewind and run-ahead replay frames, which the profiler would count twice: turn them\n"
      "off while profiling.\n");
    return;
  }
  auto& t = report.total;
  string s;
  s.append(fmt("Capture: %.1f ms emulated, %llu VI frames, %s CPU cycles at 93.75 MHz\n",
    report.milliseconds, (unsigned long long)report.viFrames, (const char*)grouped(t.cycles)));
  if(!report.symbols.empty()) s.append("Symbols: ", report.symbols.c_str(), "\n");
  else s.append("Symbols: none (functions are named by address)\n");
  if(report.partialCallTree) s.append("Note: the profiler was enabled mid-game, so callers already running then are missing from the call tree\n");
  if(report.stackOverflows) s.append(fmt("Warning: the shadow call stack overflowed %llu times; some call tree parents are incomplete\n", (unsigned long long)report.stackOverflows));
  if(report.droppedTimelineEvents) s.append(fmt("Warning: the timeline filled up; %llu events dropped, RSP figures cover the recorded part only\n", (unsigned long long)report.droppedTimelineEvents));

  s.append("\nFrames\n");
  s.append(fmt("  frames shown:     %llu (VI origin changes)\n", (unsigned long long)report.framesShown));
  if(report.fps > 0) {
    s.append(fmt("  frame rate:       %.2f fps\n", report.fps));
    s.append(fmt("  frame time:       avg %.2f ms, median %.2f ms, p90 %.2f ms\n", report.frameMsAverage, report.frameMsMedian, report.frameMsP90));
  }

  s.append("\nBusy (share of the capture)\n");
  s.append(fmt("  RSP:              %6.2f %%\n", report.rspBusy));
  s.append(fmt("  SP DMA:           %6.2f %%\n", report.spDmaBusy));
  if(report.rdpModelled) s.append(fmt("  RDP:              %6.2f %%  (estimated by the RDP timing model)\n", report.rdpBusy));
  else s.append("  RDP:              n/a (the RDP timing model is off)\n");

  s.append("\nCPU caches\n");
  s.append(fmt("  I-cache:          %s hits, %s misses (%s %% hit rate)\n",
    (const char*)grouped(t.icacheHits), (const char*)grouped(t.icacheMisses), (const char*)hitRate(t.icacheHits, t.icacheMisses)));
  s.append(fmt("  D-cache:          %s hits, %s misses (%s %% hit rate), %s writebacks\n",
    (const char*)grouped(t.dcacheHits), (const char*)grouped(t.dcacheMisses), (const char*)hitRate(t.dcacheHits, t.dcacheMisses),
    (const char*)grouped(t.dcacheWritebacks)));
  s.append(fmt("  uncached RDRAM:   %s bytes read, %s bytes written\n",
    (const char*)grouped(t.uncachedReads), (const char*)grouped(t.uncachedWrites)));

  s.append(fmt("\nTop functions by self time (%llu call/return events)\n", (unsigned long long)report.events));
  u32 shown = 0;
  for(auto& f : report.functions) {
    if(shown++ == 15 || f.self.cycles <= 0) break;
    s.append(fmt("  %6s %%  %s\n", (const char*)percent(f.self.cycles, t.cycles), f.name.c_str()));
  }
  summaryView.setText(s);
}

auto ProfilerViewer::showFunctions() -> void {
  functionList.reset();
  functionList.setHeadered();
  const char* headers[] = {"Function", "Calls", "Self %", "Self cycles", "Incl %", "Incl cycles",
    "I$ miss", "D$ miss", "D$ hit %", "D$ writeback", "Uncached rd", "Uncached wr"};
  for(u32 n = 0; n < 12; n++) {
    TableViewColumn column;
    column.setText(headers[n]);
    if(n == 0) column.setExpandable();
    else column.setAlignment(1.0);
    if(n == functionSort) column.setSorting(n == 0 ? Sort::Ascending : Sort::Descending);
    functionList.append(column);
  }
  if(!report.valid) { functionsHint.setText(""); return; }

  auto filter = filterEdit.text().downcase();
  std::vector<const Profiling::Function*> rows;
  for(auto& f : report.functions) {
    if(!f.calls && !f.inclusive.cycles) continue;
    if(filter && !string{f.name.c_str()}.downcase().find(filter)) continue;
    rows.push_back(&f);
  }
  auto key = [&](const Profiling::Function* f) -> double {
    auto& c = f->self;
    switch(functionSort) {
    case 1: return f->calls;
    case 2: case 3: return c.cycles;
    case 4: case 5: return f->inclusive.cycles;
    case 6: return c.icacheMisses;
    case 7: return c.dcacheMisses;
    case 8: return c.dcacheHits + c.dcacheMisses ? -100.0 * c.dcacheHits / (c.dcacheHits + c.dcacheMisses) : 0;  //worst first
    case 9: return c.dcacheWritebacks;
    case 10: return c.uncachedReads;
    case 11: return c.uncachedWrites;
    }
    return 0;
  };
  if(functionSort == 0) {
    std::stable_sort(rows.begin(), rows.end(), [](auto x, auto y) { return x->name < y->name; });
  } else {
    std::stable_sort(rows.begin(), rows.end(), [&](auto x, auto y) { return key(x) > key(y); });
  }

  constexpr u32 limit = 1000;
  s64 total = report.total.cycles;
  u32 count = 0;
  for(auto f : rows) {
    if(count++ == limit) break;
    auto& c = f->self;
    TableViewItem item{&functionList};
    item.append(TableViewCell().setText(f->name.c_str()));
    item.append(TableViewCell().setText(grouped(f->calls)));
    item.append(TableViewCell().setText(percent(c.cycles, total)));
    item.append(TableViewCell().setText(grouped(c.cycles)));
    item.append(TableViewCell().setText(percent(f->inclusive.cycles, total)));
    item.append(TableViewCell().setText(grouped(f->inclusive.cycles)));
    item.append(TableViewCell().setText(grouped(c.icacheMisses)));
    item.append(TableViewCell().setText(grouped(c.dcacheMisses)));
    item.append(TableViewCell().setText(hitRate(c.dcacheHits, c.dcacheMisses)));
    item.append(TableViewCell().setText(grouped(c.dcacheWritebacks)));
    item.append(TableViewCell().setText(grouped(c.uncachedReads)));
    item.append(TableViewCell().setText(grouped(c.uncachedWrites)));
  }
  functionsHint.setText(rows.size() > limit ? fmt("%u of %u functions", limit, (u32)rows.size()) : fmt("%u functions", (u32)rows.size()));
  functionList.resizeColumns();
}

//open every thread root and follow the most expensive child while it is a large share of the capture
auto ProfilerViewer::expandHotPath() -> void {
  if(!report.valid) return;
  s64 total = report.total.cycles;
  if(total <= 0) return;
  for(auto root : report.roots) {
    if(report.tree[root].inclusive.cycles * 50 < total) continue;  //under 2%
    expanded.insert(root);
    u32 node = root;
    for(u32 depth = 0; depth < 64; depth++) {
      auto& children = report.tree[node].children;
      if(children.empty()) break;
      u32 next = children.front();
      if(report.tree[next].inclusive.cycles * 10 < total) break;  //under 10%
      expanded.insert(next);
      node = next;
    }
  }
}

auto ProfilerViewer::showTree() -> void {
  treeList.reset();
  treeList.setHeadered();
  const char* headers[] = {"Function", "Incl %", "Self %", "Incl cycles", "Self cycles", "Calls", "I$ miss", "D$ miss", "D$ hit %"};
  for(u32 n = 0; n < 9; n++) {
    TableViewColumn column;
    column.setText(headers[n]);
    if(n == 0) column.setExpandable();
    else column.setAlignment(1.0);
    treeList.append(column);
  }
  treeRows.clear();
  if(!report.valid) return;

  s64 total = report.total.cycles;
  constexpr u32 childLimit = 200;
  auto addRow = [&](u32 node) {
    auto& n = report.tree[node];
    string marker = n.children.empty() ? "    " : expanded.count(node) ? "[-] " : "[+] ";
    string indent;
    for(u32 d = 0; d < n.depth; d++) indent.append("    ");
    TableViewItem item{&treeList};
    item.append(TableViewCell().setText({indent, marker, n.name.c_str()}));
    item.append(TableViewCell().setText(percent(n.inclusive.cycles, total)));
    item.append(TableViewCell().setText(percent(n.self.cycles, total)));
    item.append(TableViewCell().setText(grouped(n.inclusive.cycles)));
    item.append(TableViewCell().setText(grouped(n.self.cycles)));
    item.append(TableViewCell().setText(grouped(n.calls)));
    item.append(TableViewCell().setText(grouped(n.inclusive.icacheMisses)));
    item.append(TableViewCell().setText(grouped(n.inclusive.dcacheMisses)));
    item.append(TableViewCell().setText(hitRate(n.inclusive.dcacheHits, n.inclusive.dcacheMisses)));
    treeRows.push_back(node);
  };
  std::function<void (u32)> visit = [&](u32 node) {
    addRow(node);
    if(!expanded.count(node)) return;
    auto& children = report.tree[node].children;
    u32 count = 0;
    for(auto child : children) {
      if(count++ == childLimit) {
        string indent;
        for(u32 d = 0; d <= report.tree[node].depth; d++) indent.append("    ");
        TableViewItem item{&treeList};
        item.append(TableViewCell().setText({indent, "    (", children.size() - childLimit, " more, cheaper)"}));
        treeRows.push_back(0);
        break;
      }
      visit(child);
    }
  };
  for(auto root : report.roots) visit(root);
  treeList.resizeColumns();
}

auto ProfilerViewer::showRsp() -> void {
  rspList.reset();
  rspList.setHeadered();
  const char* headers[] = {"RSP task", "Busy %", "Tasks", "Yields", "Total ms", "Avg ms", "Longest ms", "Per VI frame"};
  for(u32 n = 0; n < 8; n++) {
    TableViewColumn column;
    column.setText(headers[n]);
    if(n == 0) column.setExpandable();
    else column.setAlignment(1.0);
    rspList.append(column);
  }
  if(!report.valid) return;
  for(auto& t : report.rspTasks) {
    TableViewItem item{&rspList};
    item.append(TableViewCell().setText(t.name.c_str()));
    item.append(TableViewCell().setText(report.milliseconds > 0 ? fmt("%.2f", 100.0 * t.busyMs / report.milliseconds) : string{"-"}));
    item.append(TableViewCell().setText(grouped(t.count)));
    item.append(TableViewCell().setText(grouped(t.yields)));
    item.append(TableViewCell().setText(fmt("%.3f", t.busyMs)));
    item.append(TableViewCell().setText(t.count ? fmt("%.4f", t.busyMs / t.count) : string{"-"}));
    item.append(TableViewCell().setText(fmt("%.4f", t.longestMs)));
    item.append(TableViewCell().setText(report.viFrames ? fmt("%.2f", (double)t.count / report.viFrames) : string{"-"}));
  }
  rspList.resizeColumns();
}

#else

//without the N64 core the panel only says so
auto ProfilerViewer::construct() -> void {
  setCollapsible();
  setVisible(false);
  profilerLabel.setText("N64 Profiler").setFont(Font().setBold());
  statusLabel.setText("This build of ares does not include the Nintendo 64 core");
  startButton.setText("Start").setEnabled(false);
  stopButton.setText("Stop").setEnabled(false);
  exportButton.setText("Export" ELLIPSIS).setEnabled(false);
  symbolsLabel.setText("Symbols:");
  symbolsButton.setText("Load" ELLIPSIS).setEnabled(false);
  armOption.setText("Profile from power on").setEnabled(false);
}
auto ProfilerViewer::reload() -> void {}
auto ProfilerViewer::unload() -> void {}
auto ProfilerViewer::liveRefresh() -> void {}
auto ProfilerViewer::setVisible(bool visible) -> ProfilerViewer& { VerticalLayout::setVisible(visible); return *this; }

#endif
