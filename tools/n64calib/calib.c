// N64 timing calibration ROM.
//
// Runs fixed CPU, RSP and RDP workloads and reports how long each took, so the
// same ROM can be run on real hardware and in Ares and the numbers compared.
//
// Output: one CSV line per test via debugf (USB on flashcarts, ISViewer in
// emulators), and the same table on screen at the end.
//
//   CAL,<group>,<test>,<count>,<units>,<ticks>,<dp_clock>,<dp_cmdbuf>,<dp_pipe>,<dp_tmem>
//
//   count   : how many times the test body ran (iterations / primitives)
//   units   : total work units (instructions incl. delay slots, loads, pixels...) for per-unit maths
//   ticks   : CPU COP0 Count delta (46.875 MHz, 1 tick = 2 CPU cycles), best of 3 runs
//   dp_*    : RDP counters for that same run (62.5 MHz RDP clocks; 24-bit)
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

#define UL(x) ((unsigned long)(x))
#define NOINLINE __attribute__((noinline))
#define REPEATS 3
#define SCREEN_W 320
#define SCREEN_H 240

enum { KIND_CPU, KIND_RSP };

typedef struct {
    const char *group;
    const char *name;
    int kind;               // KIND_CPU runs with interrupts off and no rspq_wait
    uint32_t count;
    uint32_t units;         // total work units for the whole run
    void (*setup)(void);
    void (*run)(uint32_t count);
    void (*teardown)(void);
} test_t;

typedef struct {
    uint32_t ticks, clk, cmdbuf, pipe, tmem;
} meas_t;

static uint32_t cal_ovl;
static uint8_t big_buf[64*1024] __attribute__((aligned(16)));
static uint8_t small_buf[4*1024] __attribute__((aligned(16)));
static uint8_t rsp_dma_buf[1024] __attribute__((aligned(16)));
static surface_t fb16, fb32, zb, tex;

#define MAX_LINES 96
static char lines[MAX_LINES][96];
static int num_lines;

// ---------------------------------------------------------------- measurement

static void dp_reset_counters(void)
{
    *DP_STATUS = DP_WSTATUS_RESET_CLOCK_COUNTER | DP_WSTATUS_RESET_PIPE_COUNTER |
                 DP_WSTATUS_RESET_TMEM_COUNTER | DP_WSTATUS_RESET_CMD_COUNTER;
}

static meas_t measure_once(const test_t *t)
{
    meas_t m;
    uint32_t t0, t1;
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
    m.clk    = *DP_CLOCK & 0xFFFFFF;
    m.cmdbuf = *DP_BUSY & 0xFFFFFF;
    m.pipe   = *DP_PIPE_BUSY & 0xFFFFFF;
    m.tmem   = *DP_TMEM_BUSY & 0xFFFFFF;
    return m;
}

static void clear_zbuf(void);
static void cal_rdp_tri_zbuf_big(uint32_t n);

static void run_test(const test_t *t)
{
    if (t->setup) t->setup();
    if (t->kind != KIND_CPU) rspq_wait();

    meas_t best = { .ticks = 0xFFFFFFFF };
    for (int i = 0; i < REPEATS; i++) {
        // every repeat must start from a far z-buffer, or later runs fail the depth test
        if (t->run == cal_rdp_tri_zbuf_big) clear_zbuf();
        meas_t m = measure_once(t);
        if (m.ticks < best.ticks) best = m;
    }
    if (t->teardown) t->teardown();

    debugf("CAL,%s,%s,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n", t->group, t->name,
        UL(t->count), UL(t->units), UL(best.ticks), UL(best.clk), UL(best.cmdbuf), UL(best.pipe), UL(best.tmem));

    if (num_lines < MAX_LINES) {
        // cycles per unit, in hundredths, avoids float formatting
        uint32_t cpu_cyc = best.ticks * 2;
        uint32_t per100 = t->units ? (uint32_t)((uint64_t)cpu_cyc * 100 / t->units) : 0;
        snprintf(lines[num_lines++], sizeof(lines[0]),
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

// ---------------------------------------------------------------- RSP tests

static NOINLINE void cal_rsp_empty(uint32_t n)       { (void)n; rspq_write(cal_ovl, 0x0, 0, 0); }
static NOINLINE void cal_rsp_spin_scalar(uint32_t n) { rspq_write(cal_ovl, 0x0, 0, n); }
static NOINLINE void cal_rsp_spin_vector(uint32_t n) { rspq_write(cal_ovl, 0x1, 0, n); }
static NOINLINE void cal_rsp_dma_in_8(uint32_t n)    { rspq_write(cal_ovl, 0x2, 8,    PhysicalAddr(rsp_dma_buf), n); }
static NOINLINE void cal_rsp_dma_in_64(uint32_t n)   { rspq_write(cal_ovl, 0x2, 64,   PhysicalAddr(rsp_dma_buf), n); }
static NOINLINE void cal_rsp_dma_in_256(uint32_t n)  { rspq_write(cal_ovl, 0x2, 256,  PhysicalAddr(rsp_dma_buf), n); }
static NOINLINE void cal_rsp_dma_in_1k(uint32_t n)   { rspq_write(cal_ovl, 0x2, 1024, PhysicalAddr(rsp_dma_buf), n); }
static NOINLINE void cal_rsp_dma_out_1k(uint32_t n)  { rspq_write(cal_ovl, 0x3, 1024, PhysicalAddr(rsp_dma_buf), n); }

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

// ---------------------------------------------------------------- contention

// D-cache misses while the RDP is filling the screen: measures RDRAM contention.
// Only the CPU loop is timed; the RDP work is queued before and waited for after.
static uint32_t contention_ticks;
static NOINLINE void cal_bus_dload_miss_rdp(uint32_t n)
{
    rdpq_attach(&fb32, NULL);
    rdpq_set_mode_fill(RGBA32(0, 0, 0, 255));
    for (int i = 0; i < 40; i++) rdpq_fill_rectangle(0, 0, SCREEN_W, SCREEN_H);
    rdpq_detach();
    rspq_flush();
    data_cache_hit_writeback_invalidate(big_buf, sizeof(big_buf));
    uint32_t t0 = TICKS_READ();
    for (uint32_t i = 0; i < n; i++) cal_load_loop(big_buf, sizeof(big_buf)/16, 16);
    contention_ticks = TICKS_READ() - t0;
    rspq_wait();
}

// ---------------------------------------------------------------- test table

#define PX_FULL (SCREEN_W * SCREEN_H)
#define PX_BIGTRI (SCREEN_W * SCREEN_H / 2)

static const test_t tests[] = {
    // group, name, kind, count, units, setup, run, teardown
    { "cpu", "empty",          KIND_CPU, 1,      1,               NULL, cal_cpu_empty, NULL },
    { "cpu", "alu",            KIND_CPU, 100000, 100000 * 9,      NULL, cal_cpu_alu, NULL },
    { "cpu", "mult",           KIND_CPU, 100000, 100000,          NULL, cal_cpu_mult, NULL },
    { "cpu", "div",            KIND_CPU, 20000,  20000,           NULL, cal_cpu_div, NULL },
    { "cpu", "fmul",           KIND_CPU, 100000, 100000,          NULL, cal_cpu_fmul, NULL },
    { "cpu", "fdiv",           KIND_CPU, 20000,  20000,           NULL, cal_cpu_fdiv, NULL },
    { "cpu", "fmul_b2b",       KIND_CPU, 20000,  20000 * 8,       NULL, cal_cpu_fmul_b2b, NULL },
    { "cpu", "icache_hit",     KIND_CPU, 20,     20 * 8192,       NULL, cal_cpu_icache_hit, NULL },
    { "cpu", "icache_miss",    KIND_CPU, 20,     20 * 8192,       NULL, cal_cpu_icache_miss, NULL },
    { "cpu", "dload_hit",      KIND_CPU, 160,    160 * 256,       setup_dcache_hit, cal_cpu_dload_hit, NULL },
    { "cpu", "dstore_hit",     KIND_CPU, 160,    160 * 256,       setup_dcache_hit, cal_cpu_dstore_hit, NULL },
    { "cpu", "dload_miss",     KIND_CPU, 10,     10 * 4096,       setup_dcache_clean, cal_cpu_dload_miss, NULL },
    { "cpu", "dstore_miss_wb", KIND_CPU, 10,     10 * 4096,       setup_dcache_dirty, cal_cpu_dstore_miss, NULL },
    { "cpu", "uncached_load",  KIND_CPU, 10000,  10000,           NULL, cal_cpu_uncached_load, NULL },
    { "cpu", "uncached_store", KIND_CPU, 10000,  10000,           NULL, cal_cpu_uncached_store, NULL },

    { "rsp", "empty",          KIND_RSP, 1,      1,               NULL, cal_rsp_empty, NULL },
    { "rsp", "spin_scalar",    KIND_RSP, 100000, 100000 * 3,      NULL, cal_rsp_spin_scalar, NULL },
    { "rsp", "spin_vector",    KIND_RSP, 50000,  50000 * 11,      NULL, cal_rsp_spin_vector, NULL },
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

static void show_results(void)
{
    display_init(RESOLUTION_640x480, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_DISABLED);
    while (1) {
        surface_t *disp = display_get();
        graphics_fill_screen(disp, 0);
        graphics_draw_text(disp, 16, 8, "N64 calibration ROM v2");
        graphics_draw_text(disp, 16, 16, "grp test               ticks   cyc/u    clock     pipe     tmem");
        for (int i = 0; i < num_lines && i < 56; i++)
            graphics_draw_text(disp, 16, 32 + i * 8, lines[i]);
        display_show(disp);
    }
}

int main(void)
{
    debug_init_usblog();
    debug_init_isviewer();

    rspq_init();
    rdpq_init();
    cal_ovl = rspq_overlay_register(&rsp_calib);
    init_surfaces();

    debugf("CAL,begin,v2\n");
    debugf("CAL,group,test,count,units,ticks,dp_clock,dp_cmdbuf,dp_pipe,dp_tmem\n");

    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
        run_test(&tests[i]);

    // contention: same load loop with and without the RDP filling a 32-bit screen
    {
        uint32_t best_busy = 0xFFFFFFFF;
        for (int i = 0; i < REPEATS; i++) {
            cal_bus_dload_miss_rdp(4);
            if (contention_ticks < best_busy) best_busy = contention_ticks;
        }
        debugf("CAL,bus,dload_miss_rdp_fill,4,%lu,%lu,0,0,0,0\n", UL(4 * 4096), UL(best_busy));
        uint32_t per100 = (uint32_t)((uint64_t)best_busy * 2 * 100 / (4 * 4096));
        snprintf(lines[num_lines++], sizeof(lines[0]), "bus %-15s %8lu %5lu.%02lu",
            "dload_miss_rdp", UL(best_busy), UL(per100 / 100), UL(per100 % 100));
    }

    debugf("CAL,end\n");
    show_results();
}
