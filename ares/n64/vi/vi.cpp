#include <n64/n64.hpp>

namespace ares::Nintendo64 {

VI vi;
#include "io.cpp"
#include "debugger.cpp"
#include "serialization.cpp"

auto VI::step(u32 clocks) -> void {
  auto scaled = (u64)clocks * system.frequency() + clockFraction;
  Thread::clock += scaled / system.videoFrequency();
  clockFraction = scaled % system.videoFrequency();
}

auto VI::load(Node::Object parent) -> void {
  node = parent->append<Node::Object>("VI");

  u32 width = 640;
  u32 height = 576;

  #if defined(VULKAN)
  if (vulkan.enable) {
    width *= vulkan.outputUpscale;
    height *= vulkan.outputUpscale;
  }
  #endif
  screen = node->append<Node::Video::Screen>("Screen", width, height);
  screen->setRefresh(std::bind_front(&VI::refresh, this));
  screen->refreshRateHint(Region::PAL() ? 50 : 60); // TODO: More accurate refresh rate hint
  screen->colors((1 << 24) + (1 << 15), [&](n32 color) -> n64 {
    if(color < (1 << 24)) {
      u64 a = 65535;
      u64 r = image::normalize(color >> 16 & 255, 8, 16);
      u64 g = image::normalize(color >>  8 & 255, 8, 16);
      u64 b = image::normalize(color >>  0 & 255, 8, 16);
      return a << 48 | r << 32 | g << 16 | b << 0;
    } else {
      u64 a = 65535;
      u64 r = image::normalize(color >> 10 & 31, 5, 16);
      u64 g = image::normalize(color >>  5 & 31, 5, 16);
      u64 b = image::normalize(color >>  0 & 31, 5, 16);
      return a << 48 | r << 32 | g << 16 | b << 0;
    }
  });
  
  int videoHeight = Region::PAL() ? 576 : 480;

  #if defined(VULKAN)
  if(vulkan.enable) {
    screen->setSize(vulkan.outputUpscale * 640, vulkan.outputUpscale * videoHeight);
    if(!vulkan.supersampleScanout) {
      screen->setScale(1.0 / vulkan.outputUpscale, 1.0 / vulkan.outputUpscale);
    }
  } else {
    screen->setSize(640, videoHeight);
  }
  #else
  screen->setSize(640, videoHeight);
  #endif

  // Pedantic N64 NTSC aspect ratio is 120:119, but let's keep 120:120 to avoid slight scaling.
  // Pedantic N64 PAL aspect ratio is 5900000:4965653, but let's use 12:10 to achieve the
  // same aspect ratio as NTSC.
  Region::PAL() ? screen->setAspect(12, 10) : screen->setAspect(120, 120);

  debugger.load(node);
}

auto VI::unload() -> void {
  debugger = {};
  node->remove(screen);
  screen.reset();
  node.reset();
}

auto VI::main() -> void {
  while(Thread::clock < 0) {
    if(active()) {
      ++io.vcounter;
      int halfline = io.vcounter << 1 | io.field;
      if(halfline >= io.halfLinesPerField+1) {
        io.vcounter = 0;
        io.field += !io.halfLinesPerField.bit(0);
        if(++io.leapCounter == 5) io.leapCounter = 0;
        updateBusLoad();
      }

      if(io.vcounter == io.vstart >> 1) {
        #if defined(VULKAN)
        if (vulkan.enable) {
          gpuOutputValid = vulkan.scanoutAsync(io.field);
          vulkan.frame();
        }
        #endif
        refreshed = true;
        screen->frame();
        ri.checkRefresh();
      }

      if(io.halfLinesPerField.bit(0)) { // progressive
        if(io.vcounter == io.coincidence >> 1) {
          mi.raise(MI::IRQ::VI);
        }
      } else { // interlaced
        if(io.coincidence.bit(0)) {
          if(io.vcounter == io.coincidence >> 1)
            mi.raise(MI::IRQ::VI);
        }
        if(!io.coincidence.bit(0)) {
          int halfline = io.vcounter << 1 | io.field;
          if(!io.field && halfline == io.coincidence)
            mi.raise(MI::IRQ::VI);
          if(io.field && halfline+1 == io.coincidence)
            mi.raise(MI::IRQ::VI);
          if(!io.field && halfline == io.halfLinesPerField && io.coincidence == 0)
            mi.raise(MI::IRQ::VI);
        }
      }

      u32 lineDuration = io.quarterLineDuration+1;
      if(io.vcounter == 1)
        lineDuration = io.hsyncLeap[io.leapPattern.bit(io.leapCounter)];      
      step(lineDuration);
    } else {
      // Arbitrarily call screen->frame() every once in a while to keep the UI responsive.
      // We do that every 200 simulated lines of 0x800 quarter-clocks. This is just arbitrary,
      // the real VI is not clocking at all when inactive.
      io.vcounter = 0;
      if(busActive) updateBusLoad();
      if(++inactiveCounter >= 200) {
        inactiveCounter = 0;
        refreshed = true;
      }
      step(0x800);
    }
  }
}

//RDRAM contention from scanout, fitted to tools/n64calib v3.1 on hardware: the
//suite with the VI off (ROM v2), blanked (v3.1 pass 1), scanning out 320x240
//16-bit (pass 2) and 640x480 32-bit interlaced (pass 3, 4x the bytes of pass 2).
//Slowdowns are affine in the bytes fetched: memory-bound rows lose about 1.8%
//once the VI runs (even blanked: fill16_full 664709 -> 677155 ticks), and about
//7% more per 640x480x32 field (689109 at 320x240x16, 727422 at 640x480x32).
//Here the CPU's share, in scheduler clocks per access (2 per CPU cycle):
//  D-cache miss 39.05 -> 39.71 / 40.12 / 40.74 cycles (dload_miss)
//  dirty miss 70.50 -> 71.62 / 72.34 / 76.29 cycles (dstore_miss_wb)
//  uncached load 36.02 -> 36.60 / 37.11 / 38.59, store 16.51 -> 16.85 / 17.12 / 18.72
//  I-cache miss 51.2 -> 52.1 / 52.2 / 53.7 cycles per line (icache_miss)
//The RDP, SP DMA and the CPU-RDP contention scale with busSlowdown too.
auto VI::updateBusLoad() -> void {
  busActive = active();
  u32 bytes = 0;
  if(busActive) {
    u32 pixels = io.hend > io.hstart ? (u32)(io.hend - io.hstart) * io.xscale >> 10 : 0;
    u32 lines = io.vend > io.vstart ? (u32)(io.vend - io.vstart) >> 1 : 0;
    bytes = pixels * lines * (io.colorDepth == 3 ? 4 : 2);
  }
  busLoad = bytes / 614400.0;
  cpu.viFill      = (s64)(busSlowdown(1.4, 2.0) + 0.5);
  cpu.viWriteBack = (s64)(busSlowdown(0.0, 8.8) + 0.5);
  cpu.viUncached  = (s64)(busSlowdown(1.2, 4.0) + 0.5);
  cpu.updateBusContention();
}

auto VI::refresh() -> void {
  #if defined(VULKAN)
  if(vulkan.enable && gpuOutputValid) {
    const u8* rgba = nullptr;
    u32 width = 0, height = 0;
    vulkan.mapScanoutRead(rgba, width, height);
    if(rgba) {
      screen->setViewport(0, 0, width, height);
      for(u32 y : range(height)) {
        auto source = rgba + width * y * sizeof(u32);
        auto target = screen->pixels(1).data() + y * vulkan.outputUpscale * 640;
        for(u32 x : range(width)) {
          target[x] = source[x * 4 + 0] << 16 | source[x * 4 + 1] << 8 | source[x * 4 + 2] << 0;
        }
      }
    } else {
      screen->setViewport(0, 0, 1, 1);
      screen->pixels(1).data()[0] = 0;
    }
    vulkan.unmapScanoutRead();
    vulkan.endScanout();

    if(Model::Aleck64()) aleck64.vdp.render(screen); //aleck64 supports overlay graphics
    return;
  }
  #endif

  if(io.serrate == 0) screen->setProgressive(0);
  if(io.serrate == 1) screen->setInterlace(!io.field);

  u32 hscan_start = Region::NTSC() ? 108 : 128;
  u32 vscan_start = Region::NTSC() ?  34 :  44;
  u32 hscan_len   = Region::NTSC() ? 640 : 640;
  u32 vscan_len   = Region::NTSC() ? 480 : 576;
  u32 hscan_stop  = hscan_start + hscan_len;
  u32 vscan_stop  = vscan_start + vscan_len;
  screen->setViewport(0, 0, hscan_len, vscan_len);

  i32 dy0 = vi.io.vstart;
  i32 dy1 = vi.io.vend;   if (dy1 < dy0) dy1 = vscan_stop;
  i32 dx0 = vi.io.hstart;
  i32 dx1 = vi.io.hend;

  dy0 = max(vscan_start, dy0);
  dy1 = min(vscan_stop,  dy1);
  dx0 = max(hscan_start, dx0);
  dx1 = min(hscan_stop,  dx1);

  // Undocumented VI guard-band "hardware bug" (match parallel-RDP)
  if(dx0 >= hscan_start) dx0 += 8;
  if(dx1 <  hscan_stop)  dx1 -= 7;

  u32 pitch = vi.io.width;
  if(vi.io.colorDepth == 2) {
    //15bpp
    u32 y0 = vi.io.ysubpixel + vi.io.yscale * (dy0 - vi.io.vstart);
    for(i32 dy = dy0; dy < dy1; dy++) {
      if(!io.serrate || (dy & 1) == !io.field) {
        u32 address = vi.io.dramAddress + (y0 >> 11) * pitch * 2;
        auto line = screen->pixels(1).data() + (dy - vscan_start) * hscan_len;
        u32 x0 = vi.io.xsubpixel + vi.io.xscale * (dx0 - vi.io.hstart);
        for(i32 dx = dx0; dx < dx1; dx++) {
          u16 data = rdram.ram.read<Half>(address + (x0 >> 10) * 2, RBusDevice::VI_DMA);
          line[dx - hscan_start] = 1 << 24 | data >> 1;
          x0 += vi.io.xscale;
        }
      }
      y0 += vi.io.yscale;
    }
  }

  if(vi.io.colorDepth == 3) {
    //24bpp
    u32 y0 = vi.io.ysubpixel + vi.io.yscale * (dy0 - vi.io.vstart);
    for(i32 dy = dy0; dy < dy1; dy++) {
      if(!io.serrate || (dy & 1) == !io.field) {
        u32 address = vi.io.dramAddress + (y0 >> 11) * pitch * 4;
        auto line = screen->pixels(1).data() + (dy - vscan_start) * hscan_len;
        u32 x0 = vi.io.xsubpixel + vi.io.xscale * (dx0 - vi.io.hstart);
        for(i32 dx = dx0; dx < dx1; dx++) {
          u32 data = rdram.ram.read<Word>(address + (x0 >> 10) * 4, RBusDevice::VI_DMA);
          line[dx - hscan_start] = data >> 8;
          x0 += vi.io.xscale;
        }
      }
      y0 += vi.io.yscale;
    }
  }
}

auto VI::power(bool reset) -> void {
  Thread::reset();
  screen->power();
  io = {};
  refreshed = false;
  clockFraction = 0;
  updateBusLoad();

  #if defined(VULKAN)
  gpuOutputValid = false;
  #endif
}

}
