//Measured on hardware (tools/n64calib), in 187.5 MHz clocks: a transfer pays a
//fixed setup cost, then moves 8 bytes every 3.875 clocks from RDRAM, or every
//3.625 clocks to RDRAM. Fitted to v3.1 with the VI share removed, as clocks per
//transfer more than Ares v3.1: reads 8 B +4, 64 B +7, 256 B +1, 1 KB +2; writes
//8 B -10, 64 B -11, 256 B -7, 1 KB +10 (the RSP's polling loop rounds these).
static constexpr u32 DmaReadSetupClocks = 38;
static constexpr u32 DmaWriteSetupClocks = 21;
//Small reads take no less than this (dma_in_8)
static constexpr u32 DmaMinClocks = 57;
//Every row after the first takes no less than this: 64 rows of 16 bytes take
//+1893 clocks per transfer more than Ares v3.1 charged, 16 rows of 64 bytes +126,
//4 rows of 256 bytes +3 (dma_in_64x16, dma_in_16x64, dma_in_4x256).
//Not measured for writes, which use the same value.
static constexpr u32 DmaRowMinClocks = 38;

static auto dmaRowClocks(u32 length, bool write) -> u32 {
  u32 blocks = (length + 8) / 8;
  return write ? blocks * 29 / 8 : blocks * 31 / 8;
}

//RDRAM contention: the VI costs 1.6% once it runs and 9% (reads) or 12% (writes)
//more per 640x480x32 field (dma_in_1k 166138 ticks VI off, 171751 at 320x240x16,
//178729 at 640x480x32; dma_out_1k 157003, 161781, 171437; dma_out_4x256 at
//640x480x32 +12.1% over pass 1); a full rate RDP costs 4.1% (dma1k_fill32
//against dma1k_idle).
static auto dmaContended(u32 clocks, bool write) -> u32 {
  double slowdown = vi.busSlowdown(0.016, write ? 0.12 : 0.09) + 0.041 * rdp.timing.busRate;
  return (u32)(clocks * (1.0 + slowdown) + 0.5);
}

auto RSP::dmaQueue(u32 clocks, Thread& thread) -> void {
  dma.clock = (Thread::clock - thread.clock) - clocks;
}

auto RSP::dmaStep(u32 clocks) -> void {
  if(dma.busy.any()) {
    dma.clock += clocks;
    while(dma.busy.any() && dma.clock >= 0) {
      dmaTransferStep();
    }
  }
}

//The recompiler runs a whole block before RSP::main advances the DMA, so a
//poll of SP_STATUS inside the block saw a transfer that had already finished
//as busy, and looped once more than the interpreter (about 15 clocks per DMA).
//SP register accesses advance the DMA to the current clock first.
auto RSP::dmaSync() -> void {
  s64 elapsed = Thread::clock - dmaSyncClock;
  dmaSyncClock = Thread::clock;
  if(elapsed > 0) dmaStep(elapsed);  //not after CPU::synchronize rebased the clock
}

auto RSP::dmaTransferStart(Thread& thread) -> void {
  if(dma.busy.any()) return;
  if(dma.full.any()) {
    dma.current = dma.pending;
    dma.busy    = dma.full;
    dma.full    = {0,0};
    u32 clocks = dmaRowClocks(dma.current.length, dma.busy.write);
    if(dma.busy.write) clocks += DmaWriteSetupClocks;
    else clocks = max(DmaMinClocks, DmaReadSetupClocks + clocks);
    dmaQueue(dmaContended(clocks, dma.busy.write), thread);
  }
}

auto RSP::dmaTransferStep() -> void {
  s64 overshoot = dma.clock;  //time since this row finished: the next row or transfer started then
  if(dma.busy.read) {
    if constexpr(Accuracy::RSP::Recompiler) {
      if(dma.current.pbusRegion) {
        recompiler.invalidate(dma.current.pbusAddress, dma.current.length + 8);
      }
    }
    for(u32 i = 0; i <= dma.current.length; i += 8) {
      if(dma.current.pbusRegion) {
        u64 data = rdram.ram.read<Dual>(dma.current.dramAddress, RBusDevice::SP_DMA);
        imem.write<Dual>(dma.current.pbusAddress, data);
      } else {
        u32 dataLo = rdram.ram.read<Word>(dma.current.dramAddress + 0, RBusDevice::SP_DMA);
        u32 dataHi = rdram.ram.read<Word>(dma.current.dramAddress + 4, RBusDevice::SP_DMA);
        dmem.write<Word>(dma.current.pbusAddress + 0, dataLo);
        dmem.write<Word>(dma.current.pbusAddress + 4, dataHi);
      }
      if(system.homebrewMode) {
        rsp.debugger.dmaReadWord(dma.current.dramAddress, dma.current.pbusRegion, dma.current.pbusAddress);
      }
      dma.current.dramAddress += 8;
      dma.current.pbusAddress += 8;
    }
  }
  if(dma.busy.write) {
    for(u32 i = 0; i <= dma.current.length; i += 8) {
      if(dma.current.pbusRegion) {
        u64 data = imem.read<Dual>(dma.current.pbusAddress);
        rdram.ram.write<Dual>(dma.current.dramAddress, data, RBusDevice::SP_DMA);
      } else {
        u32 dataLo = dmem.read<Word>(dma.current.pbusAddress + 0);
        u32 dataHi = dmem.read<Word>(dma.current.pbusAddress + 4);
        rdram.ram.write<Word>(dma.current.dramAddress + 0, dataLo, RBusDevice::SP_DMA);
        rdram.ram.write<Word>(dma.current.dramAddress + 4, dataHi, RBusDevice::SP_DMA);
      }
      dma.current.dramAddress += 8;
      dma.current.pbusAddress += 8;
    }
  }

  if(dma.current.count) {
    dma.current.count -= 1;
    dma.current.dramAddress += dma.current.skip;
    dma.clock = overshoot - (s64)dmaContended(max(DmaRowMinClocks, dmaRowClocks(dma.current.length, dma.busy.write)), dma.busy.write);
  } else {
    dma.busy = {0,0};
    dma.current.length = 0xFF8;
    dmaTransferStart(*this);
    if(dma.busy.any()) dma.clock += overshoot;
  }
}
