// N64 timing calibration ROM.
//
// Runs fixed CPU, RSP and RDP workloads and reports how long each took, so the
// same ROM can be run on real hardware and in Ares and the numbers compared.
//
// Output: one CSV line per test via debugf (USB on flashcarts, ISViewer in
// emulators), and the same table on screen at the end, in pages (A or Start:
// next page, B: previous page).
//
//   CAL,<group>,<test>,<count>,<units>,<ticks>,<dp_clock>,<dp_cmdbuf>,<dp_pipe>,<dp_tmem>,<pass>
//
// v3 runs the suite in up to three passes: VI off (the v2 conditions), then with
// the VI scanning out a static 320x240 16-bit framebuffer, then 640x480 32-bit
// (640x480 16-bit without an Expansion Pak). Rows that only use the CPU core or
// the RSP core (no memory traffic) run in the first pass only.
//
//   count   : how many times the test body ran (iterations / primitives)
//   units   : total work units (instructions incl. delay slots, loads, pixels...) for per-unit maths
//   ticks   : CPU COP0 Count delta (46.875 MHz, 1 tick = 2 CPU cycles), best of 3 runs
//   dp_*    : RDP counters for that same run (62.5 MHz RDP clocks; 24-bit)
//
// Group-specific columns:
//   raw     : RDP command lists written by the CPU and started with DP_START/DP_END
//             (no RSP). dp_tmem holds the pixels one primitive wrote, read back.
//   bus     : a CPU loop timed while the RDP works through queued commands;
//             dp_pipe is 1 if the RDP was still busy when the loop ended (valid).
//   bus dma*: an RSP DMA loop timed on the RSP with DP_CLOCK; ticks = clock * 3/4.
//
// Every test body is a separate noinline function named cal_<group>_<test>, so
// the Ares profiler's per-function cycles can be checked against the ticks here.

#include <libdragon.h>
#include <stdio.h>
#include <string.h>

DEFINE_RSP_UCODE(rsp_calib);

// cpu_loops.S
void cal_alu_loop(uint32_t iters);
void cal_mult_loop(uint32_t iters);
void cal_div_loop(uint32_t iters);
void cal_fmul_loop(uint32_t iters);
void cal_fmul_b2b_loop(uint32_t iters);
void cal_fdiv_loop(uint32_t iters);
void cal_load_loop(volatile void *p, uint32_t count, uint32_t stride);
void cal_store_loop(volatile void *p, uint32_t count, uint32_t stride);
void cal_code_4k(void);
void cal_code_32k(void);
void cal_fadd_loop(uint32_t iters);
void cal_fsqrt_loop(uint32_t iters);
void cal_dmul_loop(uint32_t iters);
void cal_ddiv_loop(uint32_t iters);
void cal_fmul_pair_loop(uint32_t iters);
void cal_fcmp_loop(uint32_t iters);

#define UL(x) ((unsigned long)(x))
#define NOINLINE __attribute__((noinline))
#define REPEATS 3
#define SCREEN_W 320
#define SCREEN_H 240

enum {
    KIND_CPU,   // runs with interrupts off and no rspq_wait
    KIND_RSP,   // queued through rspq/rdpq, timed until rspq_wait returns
    KIND_RAW,   // RDP command list started by the CPU, timed until the SYNC_FULL interrupt
    KIND_BUS,   // setup queues RDP work for each repeat, run is a CPU loop timed while it runs
    KIND_RSPT,  // setup queues RDP work (or none), then an RSP DMA loop that times itself
};

enum {
    F_ONCE   = 1,   // first pass only: no memory traffic, so the VI cannot change it
    F_LEGACY = 2,   // KIND_BUS: v2 method, the CPU loop starts without waiting for the RDP
};

typedef struct {
    const char *group;
    const char *name;
    int kind;
    uint32_t count;
    uint32_t units;         // total work units for the whole run
    void (*setup)(void);
    void (*run)(uint32_t count);
    void (*teardown)(void);
    int flags;
    int arg;                // raw lists: primitive size; 2D DMA: packed size
} test_t;

typedef struct {
    uint32_t ticks, clk, cmdbuf, pipe, tmem;
} meas_t;

static uint32_t cal_ovl;
static uint8_t big_buf[64*1024] __attribute__((aligned(16)));
static uint8_t small_buf[4*1024] __attribute__((aligned(16)));
static uint8_t rsp_dma_buf[1024] __attribute__((aligned(16)));
static uint64_t rsp_result[2] __attribute__((aligned(16)));
static surface_t fb16, fb32, zb, tex;
static const test_t *current_test;

#define MAX_PASSES 3
#define MAX_LINES 96
static char lines[MAX_PASSES][MAX_LINES][96];
static int num_lines[MAX_PASSES];
static char pass_names[MAX_PASSES][48];
static const char *pass_tags[MAX_PASSES] = { "vi_off", "vi_320x240x16", "vi_640x480" };
static int pass;

// raw RDP command list, written through the uncached segment
#define RAW_WORDS (2048 * 4 + 32)
static uint64_t raw_list[RAW_WORDS] __attribute__((aligned(16)));
static uint32_t raw_len;
static uint32_t raw_pixels;

// ---------------------------------------------------------------- measurement

static void dp_reset_counters(void)
{
    *DP_STATUS = DP_WSTATUS_RESET_CLOCK_COUNTER | DP_WSTATUS_RESET_PIPE_COUNTER |
                 DP_WSTATUS_RESET_TMEM_COUNTER | DP_WSTATUS_RESET_CMD_COUNTER;
}

#define SPIN_LIMIT 50000000u

static void read_counters(meas_t *m)
{
    m->clk    = *DP_CLOCK & 0xFFFFFF;
    m->cmdbuf = *DP_BUSY & 0xFFFFFF;
    m->pipe   = *DP_PIPE_BUSY & 0xFFFFFF;
    m->tmem   = *DP_TMEM_BUSY & 0xFFFFFF;
}

// starts the raw list with DP_START/DP_END and waits for its SYNC_FULL interrupt.
// rdpq appends to its buffer by writing DP_END alone, so afterwards the RDP is
// parked back at the end of rdpq's last submission (DP_START = DP_END = there),
// or its next append would run everything in memory between the two.
static meas_t measure_raw(void)
{
    meas_t m = {0};
    uint32_t spin = 0;
    disable_interrupts();
    while ((*DP_STATUS & (DP_STATUS_START_VALID | DP_STATUS_END_VALID | DP_STATUS_BUSY | DP_STATUS_PIPE_BUSY | DP_STATUS_DMA_BUSY)) && ++spin < SPIN_LIMIT) {}
    uint32_t rdpq_end = *DP_END;
    *MI_MODE = MI_WMODE_CLR_DPINT;
    *DP_STATUS = DP_WSTATUS_RESET_XBUS_DMEM_DMA | DP_WSTATUS_RESET_FREEZE | DP_WSTATUS_RESET_FLUSH;
    dp_reset_counters();
    uint32_t t0 = TICKS_READ();
    *DP_START = PhysicalAddr(raw_list);
    *DP_END   = PhysicalAddr(raw_list + raw_len);
    spin = 0;
    while (!(*MI_INTERRUPT & MI_INTERRUPT_DP) && ++spin < SPIN_LIMIT) {}
    uint32_t t1 = TICKS_READ();
    read_counters(&m);
    *MI_MODE = MI_WMODE_CLR_DPINT;
    uint32_t park = 0;
    while ((*DP_STATUS & (DP_STATUS_START_VALID | DP_STATUS_END_VALID)) && ++park < SPIN_LIMIT) {}
    *DP_START = rdpq_end;
    *DP_END   = rdpq_end;
    enable_interrupts();
    m.ticks = spin >= SPIN_LIMIT ? 0xFFFFFFFF : t1 - t0;   // a timeout shows as 4294967295
    m.tmem = raw_pixels;
    return m;
}

static meas_t measure_once(const test_t *t)
{
    meas_t m;
    uint32_t t0, t1;
    if (t->kind == KIND_RAW) return measure_raw();
    if (t->kind == KIND_BUS) {
        t->setup();                         // queues the RDP work and flushes it
        data_cache_hit_writeback_invalidate(big_buf, sizeof(big_buf));
        if (!(t->flags & F_LEGACY)) {
            uint32_t spin = 0;
            while (!(*DP_STATUS & DP_STATUS_PIPE_BUSY) && ++spin < SPIN_LIMIT) {}
        }
        dp_reset_counters();
        t0 = TICKS_READ();
        t->run(t->count);
        t1 = TICKS_READ();
        uint32_t busy = (*DP_STATUS & DP_STATUS_PIPE_BUSY) ? 1 : 0;
        read_counters(&m);
        rspq_wait();
        m.ticks = t1 - t0;
        m.pipe = busy;
        return m;
    }
    if (t->kind == KIND_RSPT) {
        if (t->setup) t->setup();
        volatile uint32_t *res = UncachedAddr(rsp_result);
        res[0] = 0;
        rspq_write(cal_ovl, 0x4, 1024, PhysicalAddr(rsp_dma_buf), t->count, PhysicalAddr(rsp_result));
        rspq_wait();
        uint32_t clk = res[0] & 0xFFFFFF;
        memset(&m, 0, sizeof(m));
        m.clk = clk;
        m.ticks = clk * 3 / 4;
        return m;
    }
    if (t->kind == KIND_CPU) {
        disable_interrupts();
        dp_reset_counters();
        t0 = TICKS_READ();
        t->run(t->count);
        t1 = TICKS_READ();
        enable_interrupts();
    } else {
        rspq_wait();
        dp_reset_counters();
        t0 = TICKS_READ();
        t->run(t->count);
        rspq_wait();
        t1 = TICKS_READ();
    }
    m.ticks  = t1 - t0;
    read_counters(&m);
    return m;
}

static void clear_zbuf(void);
static void cal_rdp_tri_zbuf_big(uint32_t n);

static void run_test(const test_t *t)
{
    if (pass > 0 && (t->flags & F_ONCE)) return;
    current_test = t;
    if (t->setup && t->kind != KIND_BUS && t->kind != KIND_RSPT) t->setup();
    if (t->kind != KIND_CPU) rspq_wait();

    meas_t best = { .ticks = 0xFFFFFFFF };
    for (int i = 0; i < REPEATS; i++) {
        // every repeat must start from a far z-buffer, or later runs fail the depth test
        if (t->run == cal_rdp_tri_zbuf_big) clear_zbuf();
        meas_t m = measure_once(t);
        if (m.ticks < best.ticks) best = m;
    }
    if (t->teardown) t->teardown();

    debugf("CAL,%s,%s,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%s\n", t->group, t->name,
        UL(t->count), UL(t->units), UL(best.ticks), UL(best.clk), UL(best.cmdbuf), UL(best.pipe), UL(best.tmem),
        pass_tags[pass]);

    if (num_lines[pass] < MAX_LINES) {
        // cycles per unit, in hundredths, avoids float formatting
        uint32_t cpu_cyc = best.ticks * 2;
        uint32_t per100 = t->units ? (uint32_t)((uint64_t)cpu_cyc * 100 / t->units) : 0;
        snprintf(lines[pass][num_lines[pass]++], sizeof(lines[0][0]),
            "%-3s %-15s %8lu %5lu.%02lu %8lu %8lu %8lu",
            t->group, t->name, UL(best.ticks), UL(per100 / 100), UL(per100 % 100),
            UL(best.clk), UL(best.pipe), UL(best.tmem));
    }
}

// ---------------------------------------------------------------- CPU tests

static NOINLINE void cal_cpu_empty(uint32_t n)    { (void)n; }
static NOINLINE void cal_cpu_alu(uint32_t n)      { cal_alu_loop(n); }
static NOINLINE void cal_cpu_mult(uint32_t n)     { cal_mult_loop(n); }
static NOINLINE void cal_cpu_div(uint32_t n)      { cal_div_loop(n); }
static NOINLINE void cal_cpu_fmul(uint32_t n)     { cal_fmul_loop(n); }
static NOINLINE void cal_cpu_fdiv(uint32_t n)     { cal_fdiv_loop(n); }
static NOINLINE void cal_cpu_fmul_b2b(uint32_t n) { cal_fmul_b2b_loop(n); }

// I-cache: same instruction count per unit, 4 KB block stays cached, 32 KB block misses every line
static NOINLINE void cal_cpu_icache_hit(uint32_t n)  { for (uint32_t i = 0; i < n; i++) for (int j = 0; j < 8; j++) cal_code_4k(); }
static NOINLINE void cal_cpu_icache_miss(uint32_t n) { for (uint32_t i = 0; i < n; i++) cal_code_32k(); }

// D-cache: 4 KB fits the 8 KB D-cache, 64 KB misses every 16-byte line
static void setup_dcache_hit(void)   { cal_load_loop(small_buf, sizeof(small_buf)/16, 16); }
static void setup_dcache_clean(void) { data_cache_hit_writeback_invalidate(big_buf, sizeof(big_buf)); }
static void setup_dcache_dirty(void) { cal_store_loop(big_buf, sizeof(big_buf)/16, 16); }
static NOINLINE void cal_cpu_dload_hit(uint32_t n)   { for (uint32_t i = 0; i < n; i++) cal_load_loop(small_buf, sizeof(small_buf)/16, 16); }
static NOINLINE void cal_cpu_dload_miss(uint32_t n)  { for (uint32_t i = 0; i < n; i++) cal_load_loop(big_buf, sizeof(big_buf)/16, 16); }
static NOINLINE void cal_cpu_dstore_hit(uint32_t n)  { for (uint32_t i = 0; i < n; i++) cal_store_loop(small_buf, sizeof(small_buf)/16, 16); }
static NOINLINE void cal_cpu_dstore_miss(uint32_t n) { for (uint32_t i = 0; i < n; i++) cal_store_loop(big_buf, sizeof(big_buf)/16, 16); }
static NOINLINE void cal_cpu_uncached_load(uint32_t n)  { cal_load_loop(UncachedAddr(big_buf), n, 4); }
static NOINLINE void cal_cpu_uncached_store(uint32_t n) { cal_store_loop(UncachedAddr(big_buf), n, 4); }
static NOINLINE void cal_cpu_fadd(uint32_t n)      { cal_fadd_loop(n); }
static NOINLINE void cal_cpu_fsqrt(uint32_t n)     { cal_fsqrt_loop(n); }
static NOINLINE void cal_cpu_dmul(uint32_t n)      { cal_dmul_loop(n); }
static NOINLINE void cal_cpu_ddiv(uint32_t n)      { cal_ddiv_loop(n); }
static NOINLINE void cal_cpu_fmul_pair(uint32_t n) { cal_fmul_pair_loop(n); }
static NOINLINE void cal_cpu_fcmp(uint32_t n)      { cal_fcmp_loop(n); }

// ---------------------------------------------------------------- RSP tests

static NOINLINE void cal_rsp_empty(uint32_t n)       { (void)n; rspq_write(cal_ovl, 0x0, 0, 0); }
static NOINLINE void cal_rsp_spin_scalar(uint32_t n) { rspq_write(cal_ovl, 0x0, 0, n); }
static NOINLINE void cal_rsp_spin_vector(uint32_t n) { rspq_write(cal_ovl, 0x1, 0, n); }
static NOINLINE void cal_rsp_dma_in_8(uint32_t n)    { rspq_write(cal_ovl, 0x2, 8,    PhysicalAddr(rsp_dma_buf), n); }
static NOINLINE void cal_rsp_dma_in_64(uint32_t n)   { rspq_write(cal_ovl, 0x2, 64,   PhysicalAddr(rsp_dma_buf), n); }
static NOINLINE void cal_rsp_dma_in_256(uint32_t n)  { rspq_write(cal_ovl, 0x2, 256,  PhysicalAddr(rsp_dma_buf), n); }
static NOINLINE void cal_rsp_dma_in_1k(uint32_t n)   { rspq_write(cal_ovl, 0x2, 1024, PhysicalAddr(rsp_dma_buf), n); }
static NOINLINE void cal_rsp_dma_out_1k(uint32_t n)  { rspq_write(cal_ovl, 0x3, 1024, PhysicalAddr(rsp_dma_buf), n); }
static NOINLINE void cal_rsp_dma_out_8(uint32_t n)   { rspq_write(cal_ovl, 0x3, 8,    PhysicalAddr(rsp_dma_buf), n); }
static NOINLINE void cal_rsp_dma_out_64(uint32_t n)  { rspq_write(cal_ovl, 0x3, 64,   PhysicalAddr(rsp_dma_buf), n); }
static NOINLINE void cal_rsp_dma_out_256(uint32_t n) { rspq_write(cal_ovl, 0x3, 256,  PhysicalAddr(rsp_dma_buf), n); }
// multi-row DMAs between DMEM and big_buf (rows of width bytes, RDRAM pitch 2 * width)
#define DMA2D(out, rows, width) ((out) << 23 | ((rows) - 1) << 12 | ((width) - 1))
static NOINLINE void cal_rsp_dma2d(uint32_t n)
{
    uint32_t width = (current_test->arg & 0xFFF) + 1;
    rspq_write(cal_ovl, 0x5, current_test->arg, PhysicalAddr(big_buf), n, width * 2);
}

// ---------------------------------------------------------------- RDP tests

static const rdpq_texparms_t tex_repeat = { .s.repeats = REPEAT_INFINITE, .t.repeats = REPEAT_INFINITE };

static void td_detach(void) { rdpq_detach_wait(); }

static void su_fill16(void) { rdpq_attach(&fb16, NULL); rdpq_set_mode_fill(RGBA32(32, 64, 128, 255)); }
static void su_fill32(void) { rdpq_attach(&fb32, NULL); rdpq_set_mode_fill(RGBA32(32, 64, 128, 255)); }
static void su_copy(void)   { rdpq_attach(&fb16, NULL); rdpq_set_mode_copy(false); rdpq_tex_upload(TILE0, &tex, NULL); }
static void su_tex1(void)   { rdpq_attach(&fb16, NULL); rdpq_set_mode_standard(); rdpq_mode_combiner(RDPQ_COMBINER_TEX);
                              rdpq_mode_filter(FILTER_POINT); rdpq_tex_upload(TILE0, &tex, &tex_repeat); }
static void su_tex1_bilinear(void) { su_tex1(); rdpq_mode_filter(FILTER_BILINEAR); }
static void su_flat(void)   { rdpq_attach(&fb16, NULL); rdpq_set_mode_standard(); rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
                              rdpq_set_prim_color(RGBA32(200, 100, 50, 255)); }
static void su_blend(void)  { su_flat(); rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY); rdpq_set_prim_color(RGBA32(200, 100, 50, 128)); }
static void su_shade(void)  { rdpq_attach(&fb16, NULL); rdpq_set_mode_standard(); rdpq_mode_combiner(RDPQ_COMBINER_SHADE); }
static void su_zbuf(void)   { rdpq_attach(&fb16, &zb); rdpq_set_mode_standard(); rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
                              rdpq_mode_zbuf(true, true); }
static void su_tex2(void)   { rdpq_attach(&fb16, NULL); rdpq_set_mode_standard();
                              rdpq_mode_combiner(RDPQ_COMBINER2((TEX0,0,SHADE,0), (TEX0,0,SHADE,0), (COMBINED,0,PRIM,0), (COMBINED,0,PRIM,0)));
                              rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
                              rdpq_mode_filter(FILTER_BILINEAR); rdpq_tex_upload(TILE0, &tex, &tex_repeat); }
static void su_upload(void) { rdpq_attach(&fb16, NULL); rdpq_set_mode_standard(); rdpq_mode_combiner(RDPQ_COMBINER_TEX); }
static void su_flat32(void) { rdpq_attach(&fb32, NULL); rdpq_set_mode_standard(); rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
                              rdpq_set_prim_color(RGBA32(200, 100, 50, 255)); }
static void clear_zbuf(void);
// z compare without update: a far z-buffer and near triangles, so every pixel passes and nothing is written
static void su_zcmp(void)   { clear_zbuf(); rdpq_attach(&fb16, &zb); rdpq_set_mode_standard(); rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
                              rdpq_mode_zbuf(true, false); }

static NOINLINE void cal_rdp_empty(uint32_t n) { (void)n; }
static NOINLINE void cal_rdp_fill_full(uint32_t n) { for (uint32_t i = 0; i < n; i++) rdpq_fill_rectangle(0, 0, SCREEN_W, SCREEN_H); }
static NOINLINE void cal_rdp_fill_32(uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) { int x = (i * 32) % (SCREEN_W - 32), y = ((i * 32) / (SCREEN_W - 32) * 32) % (SCREEN_H - 32);
        rdpq_fill_rectangle(x, y, x + 32, y + 32); }
}
static NOINLINE void cal_rdp_fill_8(uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) { int x = (i * 8) % (SCREEN_W - 8), y = ((i * 8) / (SCREEN_W - 8) * 8) % (SCREEN_H - 8);
        rdpq_fill_rectangle(x, y, x + 8, y + 8); }
}
static NOINLINE void cal_rdp_texrect_32(uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) { int x = (i * 32) % (SCREEN_W - 32), y = ((i * 32) / (SCREEN_W - 32) * 32) % (SCREEN_H - 32);
        rdpq_texture_rectangle(TILE0, x, y, x + 32, y + 32, 0, 0); }
}
static NOINLINE void cal_rdp_tex_upload(uint32_t n) { for (uint32_t i = 0; i < n; i++) rdpq_tex_upload(TILE0, &tex, NULL); }

static void tri_big(const rdpq_trifmt_t *fmt, int stride, float z)
{
    // half of the screen: 38400 pixels
    float v1[9] = { 0, 0 }, v2[9] = { SCREEN_W, 0 }, v3[9] = { 0, SCREEN_H };
    int s = 2;
    if (fmt->z_offset >= 0) { v1[s] = v2[s] = v3[s] = z; s++; }
    if (fmt->shade_offset >= 0) { for (int k = 0; k < 4; k++) { v1[s+k] = 1.0f; v2[s+k] = k == 1 ? 1.0f : 0.5f; v3[s+k] = k == 2 ? 1.0f : 0.25f; } s += 4; }
    if (fmt->tex_offset >= 0) { v1[s] = 0; v1[s+1] = 0; v2[s] = SCREEN_W; v2[s+1] = 0; v3[s] = 0; v3[s+1] = SCREEN_H; v1[s+2] = v2[s+2] = v3[s+2] = 1.0f; }
    (void)stride;
    rdpq_triangle(fmt, v1, v2, v3);
}

static void tri_small(const rdpq_trifmt_t *fmt, uint32_t i)
{
    // 8x8 right triangle: 32 pixels
    float x = (i * 8) % (SCREEN_W - 8), y = ((i * 8) / (SCREEN_W - 8) * 8) % (SCREEN_H - 8);
    float v1[9] = { x, y }, v2[9] = { x + 8, y }, v3[9] = { x, y + 8 };
    int s = 2;
    if (fmt->z_offset >= 0) { v1[s] = v2[s] = v3[s] = 0.5f; s++; }
    if (fmt->shade_offset >= 0) { for (int k = 0; k < 4; k++) { v1[s+k] = 1.0f; v2[s+k] = 0.5f; v3[s+k] = 0.25f; } s += 4; }
    if (fmt->tex_offset >= 0) { v1[s] = 0; v1[s+1] = 0; v2[s] = 8; v2[s+1] = 0; v3[s] = 0; v3[s+1] = 8; v1[s+2] = v2[s+2] = v3[s+2] = 1.0f; }
    rdpq_triangle(fmt, v1, v2, v3);
}

static NOINLINE void cal_rdp_tri_flat_big(uint32_t n)   { for (uint32_t i = 0; i < n; i++) tri_big(&TRIFMT_FILL, 0, 0); }
static NOINLINE void cal_rdp_tri_flat_small(uint32_t n) { for (uint32_t i = 0; i < n; i++) tri_small(&TRIFMT_FILL, i); }
static NOINLINE void cal_rdp_tri_shade_big(uint32_t n)  { for (uint32_t i = 0; i < n; i++) tri_big(&TRIFMT_SHADE, 0, 0); }
static NOINLINE void cal_rdp_tri_shade_small(uint32_t n){ for (uint32_t i = 0; i < n; i++) tri_small(&TRIFMT_SHADE, i); }
static NOINLINE void cal_rdp_tri_tex_big(uint32_t n)    { for (uint32_t i = 0; i < n; i++) tri_big(&TRIFMT_TEX, 0, 0); }
static NOINLINE void cal_rdp_tri_tex_small(uint32_t n)  { for (uint32_t i = 0; i < n; i++) tri_small(&TRIFMT_TEX, i); }
static NOINLINE void cal_rdp_tri_tex2_big(uint32_t n)   { for (uint32_t i = 0; i < n; i++) tri_big(&TRIFMT_SHADE_TEX, 0, 0); }
static NOINLINE void cal_rdp_tri_blend_big(uint32_t n)  { for (uint32_t i = 0; i < n; i++) tri_big(&TRIFMT_FILL, 0, 0); }
// z decreases each time so every pixel passes the depth test and writes z
static NOINLINE void cal_rdp_tri_zbuf_big(uint32_t n)   { for (uint32_t i = 0; i < n; i++) tri_big(&TRIFMT_ZBUF_SHADE, 0, 0.9f - i * 0.02f); }
static NOINLINE void cal_rdp_tri_zcmp_big(uint32_t n)   { for (uint32_t i = 0; i < n; i++) tri_big(&TRIFMT_ZBUF_SHADE, 0, 0.5f); }
static NOINLINE void cal_rdp_tri_flat32_big(uint32_t n) { for (uint32_t i = 0; i < n; i++) tri_big(&TRIFMT_FILL, 0, 0); }

// ---------------------------------------------------------------- raw RDP command lists
//
// Built by the CPU and fed to the RDP with DP_START/DP_END, without the RSP, so
// the RDP's own cost per primitive shows even for tiny primitives. Every list
// starts with the same state commands and ends with SYNC_FULL; "empty" is that
// alone, so (list - empty) / count is the cost of one primitive.

#define RDP_CMD(id)          ((uint64_t)(id) << 56)
#define RAW_COLOR            0xF801F801u     // RGBA16 red, twice for fill mode

// two 32-bit stores per command word: plain word writes to uncached RDRAM, high word first
static volatile uint32_t *raw_wp;
static int raw_one_cycle;
static void raw_put(uint64_t w) { raw_wp[0] = w >> 32; raw_wp[1] = (uint32_t)w; raw_wp += 2; }

static void raw_begin(int one_cycle)
{
    raw_wp = UncachedAddr(raw_list);
    raw_one_cycle = one_cycle;
    raw_put(RDP_CMD(0x27));                                               // SYNC_PIPE
    raw_put(RDP_CMD(0x3F) | (uint64_t)0 << 53 | (uint64_t)2 << 51 |         // SET_COLOR_IMAGE RGBA16
            (uint64_t)(SCREEN_W - 1) << 32 | PhysicalAddr(fb16.buffer));
    raw_put(RDP_CMD(0x2D) | (uint64_t)(SCREEN_W * 4) << 12 | (SCREEN_H * 4)); // SET_SCISSOR 0,0 - W,H
    if (one_cycle) {
        raw_put(RDP_CMD(0x2F) | SOM_CYCLE_1);                             // SET_OTHER_MODES: 1 cycle, no blend, no z
        raw_put(RDP_CMD(0x3C) | (RDPQ_COMBINER_FLAT & 0x00FFFFFFFFFFFFFFull)); // SET_COMBINE: prim color
        raw_put(RDP_CMD(0x3A) | 0xFF0000FFu);                             // SET_PRIM_COLOR
    } else {
        raw_put(RDP_CMD(0x2F) | SOM_CYCLE_FILL);                          // SET_OTHER_MODES: fill
        raw_put(RDP_CMD(0x37) | RAW_COLOR);                               // SET_FILL_COLOR
    }
}

static void raw_end(void)
{
    raw_put(RDP_CMD(0x29));                                               // SYNC_FULL
    raw_len = (raw_wp - (volatile uint32_t *)UncachedAddr(raw_list)) / 2;
}

// non-shaded triangle (0x08): right angle at (x, y), legs of w pixels right and down
static void raw_tri(int x, int y, int w)
{
    int32_t yh = y * 4, yl = (y + w) * 4;
    int32_t xh = x << 16, xl = (x + w) << 16;
    int32_t dxldy = -(1 << 16);                                           // the hypotenuse moves left 1 pixel per line
    raw_put(RDP_CMD(0x08) | (uint64_t)1 << 55 |                           // left major: the vertical edge is on the left
            (uint64_t)(yl & 0x3FFF) << 32 | (uint64_t)(yh & 0x3FFF) << 16 | (yh & 0x3FFF));
    raw_put((uint64_t)(uint32_t)xl << 32 | (uint32_t)dxldy);              // XL, DxLDy
    raw_put((uint64_t)(uint32_t)xh << 32 | 0);                            // XH, DxHDy (vertical)
    raw_put((uint64_t)(uint32_t)xl << 32 | 0);                            // XM, DxMDy (flat top)
}

// fill rectangle (0x36), w x w pixels: the bottom-right corner is inclusive in
// fill mode and exclusive in 1-cycle mode
static void raw_rect(int x, int y, int w)
{
    int e = raw_one_cycle ? w : w - 1;
    raw_put(RDP_CMD(0x36) | (uint64_t)((x + e) * 4) << 44 | (uint64_t)((y + e) * 4) << 32 |
            (uint64_t)(x * 4) << 12 | (y * 4));
}

typedef void (*raw_prim_t)(int x, int y, int w);

static void raw_build(raw_prim_t prim, int one_cycle, uint32_t count, int w)
{
    raw_begin(one_cycle);
    for (uint32_t i = 0; i < count; i++) {
        int per_row = SCREEN_W / w, rows = SCREEN_H / w;
        int x = (i % per_row) * w, y = ((i / per_row) % rows) * w;
        prim(x, y, w);
    }
    raw_end();
}

static uint32_t count_pixels(void)
{
    uint32_t n = 0;
    volatile uint16_t *px = UncachedAddr(fb16.buffer);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) n += px[i] != 0;
    return n;
}

// draws one primitive into a cleared framebuffer and counts what it wrote, then builds the timed list
static void raw_setup(raw_prim_t prim, int one_cycle)
{
    const test_t *t = current_test;
    memset(UncachedAddr(fb16.buffer), 0, SCREEN_W * SCREEN_H * 2);
    raw_pixels = 0;
    if (prim) {
        raw_build(prim, one_cycle, 1, t->arg);
        measure_raw();
        raw_pixels = count_pixels();
        raw_build(prim, one_cycle, t->count, t->arg);
    } else {
        raw_begin(one_cycle);
        raw_end();
    }
}

static void su_raw_empty(void) { raw_setup(NULL, 0); }
static void su_raw_ftri(void)  { raw_setup(raw_tri, 0); }
static void su_raw_frect(void) { raw_setup(raw_rect, 0); }
static void su_raw_1tri(void)  { raw_setup(raw_tri, 1); }
static void su_raw_1rect(void) { raw_setup(raw_rect, 1); }
static NOINLINE void cal_raw_run(uint32_t n) { (void)n; }   // the list is started by measure_raw

// ---------------------------------------------------------------- contention
//
// A CPU (or RSP) loop timed while the RDP works through queued commands, to
// measure RDRAM contention. The setup queues enough RDP work to outlast the loop.

static void bus_done(void) { rdpq_detach(); rspq_flush(); }
static void bl_fill32_legacy(void) { rdpq_attach(&fb32, NULL); rdpq_set_mode_fill(RGBA32(0, 0, 0, 255));
                                     for (int i = 0; i < 40; i++) { rdpq_fill_rectangle(0, 0, SCREEN_W, SCREEN_H); } bus_done(); }
static void bl_fill16(void) { su_fill16(); for (int i = 0; i < 30; i++) { rdpq_fill_rectangle(0, 0, SCREEN_W, SCREEN_H); } bus_done(); }
static void bl_fill32(void) { su_fill32(); for (int i = 0; i < 12; i++) { rdpq_fill_rectangle(0, 0, SCREEN_W, SCREEN_H); } bus_done(); }
static void bl_tri1(void)   { su_flat();   for (int i = 0; i < 20; i++) { tri_big(&TRIFMT_FILL, 0, 0); } bus_done(); }
static void bl_zbuf(void)   { clear_zbuf(); su_zbuf(); for (int i = 0; i < 6; i++) { tri_big(&TRIFMT_ZBUF_SHADE, 0, 0.9f - i * 0.02f); } bus_done(); }
static void bl_texrect(void){ su_tex1();   for (int i = 0; i < 10; i++) { rdpq_texture_rectangle(TILE0, 0, 0, SCREEN_W, SCREEN_H, 0, 0); } bus_done(); }
static void bl_fill32_rsp(void) { su_fill32(); for (int i = 0; i < 12; i++) { rdpq_fill_rectangle(0, 0, SCREEN_W, SCREEN_H); } rdpq_detach(); }

static NOINLINE void cal_bus_dload_miss(uint32_t n) { for (uint32_t i = 0; i < n; i++) cal_load_loop(big_buf, sizeof(big_buf)/16, 16); }
static NOINLINE void cal_bus_uncached(uint32_t n)   { cal_load_loop(UncachedAddr(big_buf), n, 4); }
static NOINLINE void cal_bus_rsp_dma(uint32_t n)    { (void)n; }   // the RSP loop is queued by measure_once

// ---------------------------------------------------------------- test table

#define PX_FULL (SCREEN_W * SCREEN_H)
#define PX_BIGTRI (SCREEN_W * SCREEN_H / 2)

static const test_t tests[] = {
    // group, name, kind, count, units, setup, run, teardown
    { "cpu", "empty",          KIND_CPU, 1,      1,               NULL, cal_cpu_empty, NULL, F_ONCE },
    { "cpu", "alu",            KIND_CPU, 100000, 100000 * 9,      NULL, cal_cpu_alu, NULL, F_ONCE },
    { "cpu", "mult",           KIND_CPU, 100000, 100000,          NULL, cal_cpu_mult, NULL, F_ONCE },
    { "cpu", "div",            KIND_CPU, 20000,  20000,           NULL, cal_cpu_div, NULL, F_ONCE },
    { "cpu", "fmul",           KIND_CPU, 100000, 100000,          NULL, cal_cpu_fmul, NULL, F_ONCE },
    { "cpu", "fdiv",           KIND_CPU, 20000,  20000,           NULL, cal_cpu_fdiv, NULL, F_ONCE },
    { "cpu", "fmul_b2b",       KIND_CPU, 20000,  20000 * 8,       NULL, cal_cpu_fmul_b2b, NULL, F_ONCE },
    { "cpu", "icache_hit",     KIND_CPU, 20,     20 * 8192,       NULL, cal_cpu_icache_hit, NULL, F_ONCE },
    { "cpu", "icache_miss",    KIND_CPU, 20,     20 * 8192,       NULL, cal_cpu_icache_miss, NULL },
    { "cpu", "dload_hit",      KIND_CPU, 160,    160 * 256,       setup_dcache_hit, cal_cpu_dload_hit, NULL, F_ONCE },
    { "cpu", "dstore_hit",     KIND_CPU, 160,    160 * 256,       setup_dcache_hit, cal_cpu_dstore_hit, NULL, F_ONCE },
    { "cpu", "dload_miss",     KIND_CPU, 10,     10 * 4096,       setup_dcache_clean, cal_cpu_dload_miss, NULL },
    { "cpu", "dstore_miss_wb", KIND_CPU, 10,     10 * 4096,       setup_dcache_dirty, cal_cpu_dstore_miss, NULL },
    { "cpu", "uncached_load",  KIND_CPU, 10000,  10000,           NULL, cal_cpu_uncached_load, NULL },
    { "cpu", "uncached_store", KIND_CPU, 10000,  10000,           NULL, cal_cpu_uncached_store, NULL },

    { "rsp", "empty",          KIND_RSP, 1,      1,               NULL, cal_rsp_empty, NULL, F_ONCE },
    { "rsp", "spin_scalar",    KIND_RSP, 100000, 100000 * 3,      NULL, cal_rsp_spin_scalar, NULL, F_ONCE },
    { "rsp", "spin_vector",    KIND_RSP, 50000,  50000 * 11,      NULL, cal_rsp_spin_vector, NULL, F_ONCE },
    { "rsp", "dma_in_8",       KIND_RSP, 1000,   1000 * 8,        NULL, cal_rsp_dma_in_8, NULL },
    { "rsp", "dma_in_64",      KIND_RSP, 1000,   1000 * 64,       NULL, cal_rsp_dma_in_64, NULL },
    { "rsp", "dma_in_256",     KIND_RSP, 1000,   1000 * 256,      NULL, cal_rsp_dma_in_256, NULL },
    { "rsp", "dma_in_1k",      KIND_RSP, 1000,   1000 * 1024,     NULL, cal_rsp_dma_in_1k, NULL },
    { "rsp", "dma_out_1k",     KIND_RSP, 1000,   1000 * 1024,     NULL, cal_rsp_dma_out_1k, NULL },

    { "rdp", "empty",          KIND_RSP, 1,      1,               su_fill16, cal_rdp_empty, td_detach },
    { "rdp", "fill16_full",    KIND_RSP, 20,     20 * PX_FULL,    su_fill16, cal_rdp_fill_full, td_detach },
    { "rdp", "fill32_full",    KIND_RSP, 20,     20 * PX_FULL,    su_fill32, cal_rdp_fill_full, td_detach },
    { "rdp", "fill16_32x32",   KIND_RSP, 500,    500 * 1024,      su_fill16, cal_rdp_fill_32, td_detach },
    { "rdp", "fill16_8x8",     KIND_RSP, 2000,   2000 * 64,       su_fill16, cal_rdp_fill_8, td_detach },
    { "rdp", "copy_32x32",     KIND_RSP, 500,    500 * 1024,      su_copy, cal_rdp_texrect_32, td_detach },
    { "rdp", "tex1_32x32",     KIND_RSP, 500,    500 * 1024,      su_tex1, cal_rdp_texrect_32, td_detach },
    { "rdp", "tex1bi_32x32",   KIND_RSP, 500,    500 * 1024,      su_tex1_bilinear, cal_rdp_texrect_32, td_detach },
    { "rdp", "tex_upload_2k",  KIND_RSP, 200,    200 * 2048,      su_upload, cal_rdp_tex_upload, td_detach },
    { "rdp", "tri_flat_big",   KIND_RSP, 40,     40 * PX_BIGTRI,  su_flat, cal_rdp_tri_flat_big, td_detach },
    { "rdp", "tri_flat_small", KIND_RSP, 2000,   2000 * 32,       su_flat, cal_rdp_tri_flat_small, td_detach },
    { "rdp", "tri_shade_big",  KIND_RSP, 40,     40 * PX_BIGTRI,  su_shade, cal_rdp_tri_shade_big, td_detach },
    { "rdp", "tri_shade_small",KIND_RSP, 2000,   2000 * 32,       su_shade, cal_rdp_tri_shade_small, td_detach },
    { "rdp", "tri_tex_big",    KIND_RSP, 40,     40 * PX_BIGTRI,  su_tex1, cal_rdp_tri_tex_big, td_detach },
    { "rdp", "tri_tex_small",  KIND_RSP, 2000,   2000 * 32,       su_tex1, cal_rdp_tri_tex_small, td_detach },
    { "rdp", "tri_tex2c_big",  KIND_RSP, 40,     40 * PX_BIGTRI,  su_tex2, cal_rdp_tri_tex2_big, td_detach },
    { "rdp", "tri_blend_big",  KIND_RSP, 40,     40 * PX_BIGTRI,  su_blend, cal_rdp_tri_blend_big, td_detach },
    { "rdp", "tri_zbuf_big",   KIND_RSP, 40,     40 * PX_BIGTRI,  su_zbuf, cal_rdp_tri_zbuf_big, td_detach },
    // v2's contention row, same method
    { "bus", "dload_miss_rdp_fill", KIND_BUS, 4, 4 * 4096,        bl_fill32_legacy, cal_bus_dload_miss, NULL, F_LEGACY },

    // ---- v3 rows
    { "cpu", "fadd",           KIND_CPU, 100000, 100000,          NULL, cal_cpu_fadd, NULL, F_ONCE },
    { "cpu", "fsqrt",          KIND_CPU, 20000,  20000,           NULL, cal_cpu_fsqrt, NULL, F_ONCE },
    { "cpu", "dmul",           KIND_CPU, 100000, 100000,          NULL, cal_cpu_dmul, NULL, F_ONCE },
    { "cpu", "ddiv",           KIND_CPU, 20000,  20000,           NULL, cal_cpu_ddiv, NULL, F_ONCE },
    { "cpu", "fmul_pair",      KIND_CPU, 20000,  20000 * 8,       NULL, cal_cpu_fmul_pair, NULL, F_ONCE },
    { "cpu", "fcmp",           KIND_CPU, 20000,  20000 * 8,       NULL, cal_cpu_fcmp, NULL, F_ONCE },

    { "rsp", "dma_out_8",      KIND_RSP, 1000,   1000 * 8,        NULL, cal_rsp_dma_out_8, NULL },
    { "rsp", "dma_out_64",     KIND_RSP, 1000,   1000 * 64,       NULL, cal_rsp_dma_out_64, NULL },
    { "rsp", "dma_out_256",    KIND_RSP, 1000,   1000 * 256,      NULL, cal_rsp_dma_out_256, NULL },
    { "rsp", "dma_in_4x256",   KIND_RSP, 1000,   1000 * 1024,     NULL, cal_rsp_dma2d, NULL, 0, DMA2D(0, 4, 256) },
    { "rsp", "dma_in_16x64",   KIND_RSP, 1000,   1000 * 1024,     NULL, cal_rsp_dma2d, NULL, 0, DMA2D(0, 16, 64) },
    { "rsp", "dma_in_64x16",   KIND_RSP, 1000,   1000 * 1024,     NULL, cal_rsp_dma2d, NULL, 0, DMA2D(0, 64, 16) },
    { "rsp", "dma_out_4x256",  KIND_RSP, 1000,   1000 * 1024,     NULL, cal_rsp_dma2d, NULL, 0, DMA2D(1, 4, 256) },

    { "rdp", "tri_zcmp_big",   KIND_RSP, 40,     40 * PX_BIGTRI,  su_zcmp, cal_rdp_tri_zcmp_big, td_detach },
    { "rdp", "tri_flat32_big", KIND_RSP, 40,     40 * PX_BIGTRI,  su_flat32, cal_rdp_tri_flat32_big, td_detach },

    // raw lists: units are primitives, so cyc/u is CPU cycles per primitive; see clock
    { "raw", "empty",          KIND_RAW, 1,      1,               su_raw_empty, cal_raw_run, NULL },
    { "raw", "ftri_8",         KIND_RAW, 2000,   2000,            su_raw_ftri,  cal_raw_run, NULL, 0, 4 },
    { "raw", "ftri_32",        KIND_RAW, 2000,   2000,            su_raw_ftri,  cal_raw_run, NULL, 0, 8 },
    { "raw", "ftri_128",       KIND_RAW, 2000,   2000,            su_raw_ftri,  cal_raw_run, NULL, 0, 16 },
    { "raw", "ftri_1k",        KIND_RAW, 1000,   1000,            su_raw_ftri,  cal_raw_run, NULL, 0, 45 },
    { "raw", "frect_8x8",      KIND_RAW, 2000,   2000,            su_raw_frect, cal_raw_run, NULL, 0, 8 },
    { "raw", "frect_32x32",    KIND_RAW, 1000,   1000,            su_raw_frect, cal_raw_run, NULL, 0, 32 },
    { "raw", "1tri_32",        KIND_RAW, 2000,   2000,            su_raw_1tri,  cal_raw_run, NULL, 0, 8 },
    { "raw", "1tri_1k",        KIND_RAW, 1000,   1000,            su_raw_1tri,  cal_raw_run, NULL, 0, 45 },
    { "raw", "1rect_8x8",      KIND_RAW, 2000,   2000,            su_raw_1rect, cal_raw_run, NULL, 0, 8 },

    // contention: compare with cpu dload_miss (39.05 cyc/u idle) and uncached_load
    { "bus", "dlmiss_fill16",  KIND_BUS, 4,      4 * 4096,        bl_fill16,  cal_bus_dload_miss, NULL },
    { "bus", "dlmiss_fill32",  KIND_BUS, 4,      4 * 4096,        bl_fill32,  cal_bus_dload_miss, NULL },
    { "bus", "dlmiss_tri1",    KIND_BUS, 4,      4 * 4096,        bl_tri1,    cal_bus_dload_miss, NULL },
    { "bus", "dlmiss_zbuf",    KIND_BUS, 4,      4 * 4096,        bl_zbuf,    cal_bus_dload_miss, NULL },
    { "bus", "dlmiss_texrect", KIND_BUS, 4,      4 * 4096,        bl_texrect, cal_bus_dload_miss, NULL },
    { "bus", "unc_ld_fill32",  KIND_BUS, 10000,  10000,           bl_fill32,  cal_bus_uncached, NULL },
    { "bus", "dma1k_idle",     KIND_RSPT, 1000,  1000 * 1024,     NULL,          cal_bus_rsp_dma, NULL },
    { "bus", "dma1k_fill32",   KIND_RSPT, 1000,  1000 * 1024,     bl_fill32_rsp, cal_bus_rsp_dma, NULL },
};

// ---------------------------------------------------------------- main

// surface_alloc returns uncached memory, so CPU writes need no cache flush
static void init_surfaces(void)
{
    fb16 = surface_alloc(FMT_RGBA16, SCREEN_W, SCREEN_H);
    fb32 = surface_alloc(FMT_RGBA32, SCREEN_W, SCREEN_H);
    zb   = surface_alloc(FMT_RGBA16, SCREEN_W, SCREEN_H);
    tex  = surface_alloc(FMT_RGBA16, 32, 32);

    uint16_t *t = tex.buffer;
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
            t[y * 32 + x] = ((x ^ y) & 8) ? 0xF801 : 0x07FF;
}

static void clear_zbuf(void)
{
    memset(zb.buffer, 0xFF, SCREEN_W * SCREEN_H * 2);
}

// rows per page at 640x480 with the 8x8 font, below the two header lines
#define PAGE_ROWS 56

typedef struct { int pass, first, count; } page_t;

static void show_results(void)
{
    static page_t pages[16];
    int num_pages = 0;
    for (int p = 0; p < MAX_PASSES; p++)
        for (int first = 0; first < num_lines[p] && num_pages < 16; first += PAGE_ROWS)
            pages[num_pages++] = (page_t){ p, first, num_lines[p] - first < PAGE_ROWS ? num_lines[p] - first : PAGE_ROWS };

    display_init(RESOLUTION_640x480, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_DISABLED);
    joypad_init();
    int page = 0;
    char header[96];
    while (1) {
        joypad_poll();
        joypad_buttons_t b = joypad_get_buttons_pressed(JOYPAD_PORT_1);
        if ((b.a || b.start) && page + 1 < num_pages) page++;
        if (b.b && page > 0) page--;

        const page_t *pg = &pages[page];
        surface_t *disp = display_get();
        graphics_fill_screen(disp, 0);
        snprintf(header, sizeof(header), "N64 calib ROM v3  page %d/%d  pass %d: %s",
            page + 1, num_pages, pg->pass + 1, pass_names[pg->pass]);
        graphics_draw_text(disp, 16, 2, header);
        graphics_draw_text(disp, 16, 14, "grp test               ticks   cyc/u    clock     pipe     tmem  A:next B:back");
        for (int i = 0; i < pg->count; i++)
            graphics_draw_text(disp, 16, 26 + i * 8, lines[pg->pass][pg->first + i]);
        display_show(disp);
    }
}

// a static framebuffer for the VI to scan out during a pass
static void vi_start(resolution_t res, bitdepth_t depth)
{
    display_init(res, depth, 2, GAMMA_NONE, FILTERS_DISABLED);
    surface_t *disp = display_get();
    graphics_fill_screen(disp, graphics_make_color(0, 0, 96, 255));
    graphics_draw_text(disp, 16, 16, "calibrating with the VI on...");
    display_show(disp);
    wait_ms(100);   // let the VI pick up the new framebuffer
}

static void run_pass(int p)
{
    pass = p;
    debugf("CAL,pass,%s,%s\n", pass_tags[p], pass_names[p]);
    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
        run_test(&tests[i]);
}

int main(void)
{
    debug_init_usblog();
    debug_init_isviewer();

    rspq_init();
    rdpq_init();
    cal_ovl = rspq_overlay_register(&rsp_calib);
    init_surfaces();

    debugf("CAL,begin,v3\n");
    debugf("CAL,group,test,count,units,ticks,dp_clock,dp_cmdbuf,dp_pipe,dp_tmem,pass\n");

    strcpy(pass_names[0], "VI off");
    run_pass(0);

    strcpy(pass_names[1], "VI 320x240 16-bit");
    vi_start(RESOLUTION_320x240, DEPTH_16_BPP);
    run_pass(1);
    display_close();

    // 640x480 32-bit needs 2.4 MB of framebuffers: only with the Expansion Pak
    bool big = get_memory_size() > 4 * 1024 * 1024;
    strcpy(pass_names[2], big ? "VI 640x480 32-bit" : "VI 640x480 16-bit (no Exp Pak)");
    vi_start(RESOLUTION_640x480, big ? DEPTH_32_BPP : DEPTH_16_BPP);
    run_pass(2);
    display_close();

    debugf("CAL,end\n");
    show_results();
}
