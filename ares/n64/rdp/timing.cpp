//RDP timing model. Rendering stays synchronous: commands are drawn as soon as they
//are submitted, while this estimates how long the real RDP would take and makes
//that time visible through DPC_STATUS, DPC_CURRENT, the DPC counters, the DP
//interrupt and RDRAM contention. ARES_N64_RDP_TIMING=0 turns it off.
//
//Constants are fitted to tools/n64calib on a real N64, in RDP clocks (62.5 MHz),
//without scanout (ROM v2 with the VI off, and v3.1 rows corrected for its
//blanked VI, see VI::updateBusLoad). Per-primitive and per-row costs come from
//v3.1's raw command lists, which the RSP does not touch, as clocks per primitive
//(hardware, the VI share removed): fill triangles of 14/44/152/1080 pixels
//71.5/130.7/262.8/1192.5, fill rectangles 8x8 119.1 and 32x32 804.0, 1-cycle
//triangles of 36/1035 pixels 151.9/1570.8 and 8x8 rectangles 191.8. Rates per
//pixel come from big primitives drawn through rdpq.
namespace {
  constexpr double SetupFill      = 33.5;   //fill and copy mode
  constexpr double SetupCycle     = 21.0;   //1-cycle and 2-cycle
  constexpr double OtherCommand   = 1.0;
  constexpr double TriangleRow    = 12.0;   //1-cycle and 2-cycle
  constexpr double RectangleRow   = 14.3;   //1-cycle and 2-cycle: 13.6 untextured, 15.1 textured
  constexpr double FillRow        = 6.4;    //rectangles and triangles
  constexpr double CopyRow        = 9.0;
  constexpr double Pixel1Cycle    = 0.975;
  constexpr double Pixel2Cycle    = 1.96;
  constexpr double PixelBlend     = 1.15;   //added when reading the framebuffer
  constexpr double PixelZUpdate   = 1.95;   //added for z compare and update
  constexpr double PixelZCompare  = 1.30;   //added for z compare only
  constexpr double PixelColor32   = 0.81;   //added in 1-cycle and 2-cycle to a 32-bit color image
  constexpr double PixelCopy      = 0.556;
  constexpr double PixelFill[4]   = {0.139, 0.278, 0.556, 1.131};  //by color image size
  constexpr double FillTriangle   = 1.44;   //fill triangles pay this times PixelFill per pixel
  constexpr double LoadPerByte    = 0.504;

  //a full speed RDP fill delays each CPU cache fill by 13.5 cycles (in scheduler clocks here)
  constexpr double ContentionClocks = 27.0;
  constexpr double ContentionFullRate = 3.54;  //bytes per RDP clock at full speed
  //1-cycle and 2-cycle color writes contend like 24% more bytes than they move
  //(dlmiss_tri1 48.06 and dlmiss_texrect 48.08 cycles per miss, against 39.05 idle)
  constexpr double ContentionColorBytes = 1.24;
  //and z-buffer reads and writes like half their bytes (dlmiss_zbuf 45.43)
  constexpr double ContentionZBytes = 1.0;
  //with the VI scanning out, CPU-RDP contention grows by this per busLoad
  //(dlmiss_fill32: 52.65 cycles per miss VI off, 54.68 at 320x240x16, 58.89 at 640x480x32)
  constexpr double ContentionVI = 0.47;
  //the VI slows memory-bound RDP work (fills 9.4% at 640x480x32, 2-cycle 0.2%):
  //busSlowdown(1.9%, 7%), scaled from 0 at 1.3 color bytes per color clock to 1 at 2.0
  constexpr double SlowdownRunning = 0.019;
  constexpr double SlowdownPerLoad = 0.07;

  constexpr u8 commandLength[64] = {
    1, 1, 1, 1, 1, 1, 1, 1, 4, 6,12,14,12,14,20,22,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  };

  constexpr double texelBytes[4] = {0.5, 1.0, 2.0, 4.0};

  auto timingDisabled() -> bool {
    #if defined(_MSC_VER)
    char* value = nullptr; size_t length = 0;
    if(_dupenv_s(&value, &length, "ARES_N64_RDP_TIMING") || !value) return false;
    bool disabled = value[0] == '0';
    free(value);
    return disabled;
    #else
    auto value = std::getenv("ARES_N64_RDP_TIMING");
    return value && value[0] == '0';
    #endif
  }

  auto signExtend14(u32 value) -> s32 {
    return (s32)(value << 18) >> 18;
  }
}

auto RDP::Timing::pixelCost(bool withZ) const -> double {
  if(cycleType == 3) return PixelFill[colorSize];
  if(cycleType == 2) return PixelCopy;
  double cost = cycleType == 1 ? Pixel2Cycle : Pixel1Cycle;
  if(colorSize == 3) cost += PixelColor32;
  if(imageRead) cost += PixelBlend;
  if(!withZ) return cost;
  if(zCompare && zUpdate) cost += PixelZUpdate;
  else if(zCompare || zUpdate) cost += PixelZCompare;
  return cost;
}

auto RDP::Timing::pixelBytes(bool withZ) const -> double {
  double size = texelBytes[colorSize];
  if(cycleType >= 2) return size;
  size *= ContentionColorBytes;
  if(imageRead) size *= 2;
  if(!withZ) return size;
  if(zCompare) size += ContentionZBytes;
  if(zUpdate) size += ContentionZBytes;
  return size;
}

auto RDP::Timing::draw(u32 rows, u32 pixels, double rowCost, double pixelScale) -> void {
  double pixelTime = pixels * pixelCost() * pixelScale;
  double cost = (cycleType >= 2 ? SetupFill : SetupCycle) + rows * rowCost + pixelTime;
  double moved = pixels * pixelBytes();
  if(viSlowdown > 0 && pixels) {
    double ramp = (pixelBytes(false) / (pixelCost(false) * pixelScale) - 1.3) / 0.7;
    cost *= 1.0 + viSlowdown * (ramp < 0 ? 0 : ramp > 1 ? 1 : ramp);
  }
  work += cost;
  bytes += moved;
}

//sum scanline spans from the edge coefficients, clipped to the scissor
auto RDP::Timing::triangle(const u64* words) -> void {
  s32 yl = signExtend14(words[0] >> 32 & 0x3fff);
  s32 ym = signExtend14(words[0] >> 16 & 0x3fff);
  s32 yh = signExtend14(words[0] >>  0 & 0x3fff);
  s64 xl = (s32)(words[1] >> 32), dxl = (s32)words[1];
  s64 xh = (s32)(words[2] >> 32), dxh = (s32)words[2];
  s64 xm = (s32)(words[3] >> 32), dxm = (s32)words[3];
  s32 top = yh >> 2, middle = ym >> 2, bottom = (yl + 3) >> 2;
  s32 first = top > scissorTop ? top : scissorTop;
  s32 last = bottom < scissorBottom ? bottom : scissorBottom;
  u32 rows = 0, pixels = 0;
  for(s32 y = first; y < last; y++) {
    s64 a = xh + dxh * (y - top);
    s64 b = y < middle ? xm + dxm * (y - top) : xl + dxl * (y - middle);
    s64 left = (a < b ? a : b) >> 16;
    s64 right = (a < b ? b : a) >> 16;
    if(left < scissorLeft) left = scissorLeft;
    if(right > scissorRight) right = scissorRight;
    if(right <= left) continue;
    rows++;
    pixels += right - left + (cycleType >= 2);  //fill and copy modes include the last column
  }
  if(cycleType == 3) return draw(rows, pixels, FillRow, FillTriangle);
  draw(rows, pixels, cycleType == 2 ? CopyRow : TriangleRow);
}

auto RDP::Timing::rectangle(u64 word) -> void {
  s32 inclusive = cycleType >= 2;  //fill and copy modes include the last row and column
  s32 x0 = (word >> 12 & 0xfff) >> 2, x1 = ((word >> 44 & 0xfff) >> 2) + inclusive;
  s32 y0 = (word >>  0 & 0xfff) >> 2, y1 = ((word >> 32 & 0xfff) >> 2) + inclusive;
  if(x0 < scissorLeft) x0 = scissorLeft;
  if(y0 < scissorTop) y0 = scissorTop;
  if(x1 > scissorRight) x1 = scissorRight;
  if(y1 > scissorBottom) y1 = scissorBottom;
  if(x1 <= x0 || y1 <= y0) { work += OtherCommand; return; }
  double rowCost = cycleType == 3 ? FillRow : cycleType == 2 ? CopyRow : RectangleRow;
  draw(y1 - y0, (x1 - x0) * (y1 - y0), rowCost);
}

auto RDP::Timing::load(double loadBytes) -> void {
  double cost = loadBytes * LoadPerByte * (1.0 + viSlowdown);
  work += cost;
  bytes += loadBytes;
  tmemBusy += cost;
}

auto RDP::Timing::decode(const u64* words, u32 code, s64 start) -> void {
  u64 op = words[0];
  switch(code) {
  case 0x08: case 0x09: case 0x0a: case 0x0b:
  case 0x0c: case 0x0d: case 0x0e: case 0x0f:
    return triangle(words);
  case 0x24: case 0x25: case 0x36:
    return rectangle(op);
  case 0x29:
    if(fullCount < FullQueue) fullQueue[fullCount++] = start + (s64)(work * 3);
    break;
  case 0x2d:
    scissorLeft   = (op >> 44 & 0xfff) >> 2;
    scissorTop    = (op >> 32 & 0xfff) >> 2;
    scissorRight  = (op >> 12 & 0xfff) >> 2;
    scissorBottom = (op >>  0 & 0xfff) >> 2;
    break;
  case 0x2f:
    cycleType = op >> 52 & 3;
    imageRead = op >> 6 & 1;
    zUpdate   = op >> 5 & 1;
    zCompare  = op >> 4 & 1;
    break;
  case 0x30: case 0x34: {
    u32 width  = ((u32)(op >> 12 & 0xfff) - (u32)(op >> 44 & 0xfff)) / 4 + 1;
    u32 height = ((u32)(op >>  0 & 0xfff) - (u32)(op >> 32 & 0xfff)) / 4 + 1;
    return load(width * height * texelBytes[textureSize]);
  }
  case 0x33:
    return load(((u32)(op >> 12 & 0xfff) - (u32)(op >> 44 & 0xfff) + 1) * texelBytes[textureSize]);
  case 0x3d: textureSize = op >> 51 & 3; break;
  case 0x3f: colorSize   = op >> 51 & 3; break;
  }
  work += OtherCommand;
}

auto RDP::timingPower() -> void {
  timing = {};
  timing.enabled = !timingDisabled();
  cpu.rdpContention = 0;
  cpu.updateBusContention();
}

auto RDP::timingBusy(s64 now) const -> bool {
  return timing.enabled && (command.crashed || now < timing.busyEnd);
}

//walk newly submitted commands; called before they are rendered
auto RDP::timingSubmit(Thread& thread) -> void {
  if(!timing.enabled) return;
  auto& memory = !command.source ? (Memory::Writable&)rdram.ram : (Memory::Writable&)rsp.dmem;
  s64 now = thread.clock;
  s64 start = now > timing.busyEnd ? now : timing.busyEnd;
  if(now < timing.busyEnd) timing.pendingUntil = timing.busyEnd;

  timing.work = 0;
  timing.bytes = 0;
  timing.viSlowdown = vi.busSlowdown(SlowdownRunning, SlowdownPerLoad);
  if(now >= timing.busyEnd) timing.burstWork = timing.burstBytes = 0;
  u32 begin = command.current, end = command.end;
  for(u32 address = begin; address < end; address += 8) {
    timing.carry[timing.carryLength++] = memory.readUnaligned<Dual>(command.source ? address & 0xff8 : address);
    u32 code = timing.carry[0] >> 56 & 63;
    if(timing.carryLength < commandLength[code]) continue;  //the rest may arrive in a later submit
    timing.decode(timing.carry, code, start);
    timing.carryLength = 0;
  }

  s64 finish = start + (s64)(timing.work * 3);
  timing.cmdBusy += timing.work;
  timing.segmentStart = begin;
  timing.segmentEnd = end;
  timing.segmentFrom = start;
  timing.segmentTo = finish;
  timing.busyEnd = finish;
  if(!timing.pipeOpen) {
    timing.pipeOpen = 1;
    timing.pipeFrom = now;
  }
  //a small submit behind a long queue must not hide the traffic of the work still running
  timing.burstWork += timing.work;
  timing.burstBytes += timing.bytes;
  double rate = timing.burstWork > 0 ? timing.burstBytes / timing.burstWork / ContentionFullRate : 0;
  timing.busRate = rate < 1.0 ? rate : 1.0;
  cpu.rdpContention = (s64)(ContentionClocks * timing.busRate * (1.0 + vi.busSlowdown(0.0, ContentionVI)));
  cpu.updateBusContention();
  if(unlikely(cpu.profiler.enabled)) cpu.profiler.rdpBusy(start, finish);
}

//all timestamps are relative to the last CPU synchronize point, like Thread::clock
auto RDP::timingAdvance(s64 clocks) -> void {
  if(!timing.enabled) return;
  timing.busyEnd -= clocks;
  timing.pendingUntil -= clocks;
  timing.segmentFrom -= clocks;
  timing.segmentTo -= clocks;
  timing.pipeFrom -= clocks;
  for(u32 n = 0; n < timing.fullCount; n++) timing.fullQueue[n] -= clocks;
}

auto RDP::timingPoll() -> void {
  if(!timing.enabled) return;
  if(timing.busyEnd <= 0 && cpu.rdpContention) {
    cpu.rdpContention = 0;
    timing.busRate = 0;
    cpu.updateBusContention();
  }
  while(timing.fullCount && timing.fullQueue[0] <= 0) {
    s64 finished = timing.fullQueue[0];
    for(u32 n = 1; n < timing.fullCount; n++) timing.fullQueue[n - 1] = timing.fullQueue[n];
    timing.fullCount--;
    if(command.crashed) continue;
    mi.raise(MI::IRQ::DP);
    command.startGclk = 0;
    if(timing.pipeOpen) {
      timing.pipeBusy += (finished - timing.pipeFrom) / 3;
      timing.pipeOpen = 0;
    }
  }
}

auto RDP::timingCurrent(s64 now) const -> u32 {
  if(now >= timing.segmentTo || timing.segmentTo <= timing.segmentFrom) return command.current;
  if(now <= timing.segmentFrom) return timing.segmentStart;
  double done = (double)(now - timing.segmentFrom) / (timing.segmentTo - timing.segmentFrom);
  return timing.segmentStart + ((u32)((timing.segmentEnd - timing.segmentStart) * done) & ~7u);
}

auto RDP::timingPipeBusy(s64 now) const -> u32 {
  return (timing.pipeBusy + (timing.pipeOpen ? (now - timing.pipeFrom) / 3 : 0)) & 0xffffff;
}
