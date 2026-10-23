/*
 * The GPU of the POCO M3 (citrus), an Adreno 610 on SM6115, woken step by step. Every address
 * below belongs to this phone. Nothing here is a GMU with firmware: Linux's qcom,adreno-gmu-wrapper
 * at 0x0596a000, CPU-owned GDSCs and clocks, zap shader a610_zap.mdt, SQE a630_sqe.fw.
 *
 * The steps stay for looking, in the same discipline as redwood_adreno.c: a step says what it
 * is about to touch before it touches it, reads come before writes, and a step that does not
 * find what it needs ends the sequence instead of going on blind. citrus_adreno_run(N, skip)
 * performs steps 1 to N; gpu=1 uses plat keep_min_step and plat gpu_skip_steps.
 *
 *   1  reads   GCC clocks that feed the GPU, and the names RPMh knows its rails by
 *   2  reads   the GPU speed-bin fuse (dt: gpu_speed_bin@6006 in qfprom@1b40000)
 *   3  writes  gcc_gpu_cfg_ahb on; reads GPUCC as the boot loader left it
 *   4  writes  CX GDSC on (if it is already on, go on)
 *   5  writes  AHB, mem_iface, core clocks. No SC7280 debugcc (0x62000): CBCR bits only
 *   6  reads   the first words of the GMU wrapper (AO counter if it exists). No TCM, no a660_gmu.bin
 *   7  shared  adreno_step_smmu()        SMMU 0x059a0000
 *   8  shared  adreno_step_smmu_context()
 *   9  shared  adreno_step_trustzone()
 *  10  shared  adreno_step_zap_signature()   plat zap a610_zap.mdt
 *  11  writes  GX GDSC on once (no reset loops), then adreno_set_core_powered(true)
 *  12  shared  adreno_step_core_space()  plat sqe a630_sqe.fw; A610 has_lpac=false
 *  13  shared  adreno_step_zap_load()    plat zap_memory (dt: pil_gpu_mem@56115000)
 *  14  shared  adreno_step_core_start()
 *  15  shared  adreno_step_core_commands()
 *
 * Register layouts: drivers/clk/qcom/{gcc,gpucc}-sm6115.c, gdsc.c, clk-rcg2.c,
 * arch/arm64/boot/dts/qcom/sm6115.dtsi (gpu@5900000, gmu@596a000, gpucc@5990000, iommu@59a0000,
 * qfprom@1b40000, pil_gpu_mem@56115000), drivers/gpu/drm/msm/adreno/a6xx_gmu_wrapper.c.
 * gcc_bimc_gpu_axi (mainline gcc-sm6115.c, dt mem_iface) is 0x71154. Vendor kgsl also has
 * gcc_ddrss_gpu_axi at 0x4452c; both CBCRs are turned on. Zap memory is the phone's
 * gpu_region@55A15000, not mainline pil_gpu_mem@56115000.
 *
 * MMIO windows come from adreno_bind_plat(); helpers here are local because get/put in
 * redwood_adreno.c are static. Shared adreno_step_* are called for SMMU/zap/core.
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

#include "adreno_a6xx.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);

#define CITRUS_STEPS            15

static const struct adreno_plat *plat;
static bool gx_reset_once;              /* A610: at most one GX SW_RESET pulse this run */

extern void loader_log_putc(char c);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}
#define printf say

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* The hypervisor's SMMU imitation wants a general register, not wzr — same constraint as redwood. */

static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static uint32_t __attribute__((noinline))
rd8(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldrb %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}

static bool
need(struct adreno_block *b)
{
	if (b == NULL || b->phys == 0 || b->size == 0) {
		return false;
	}
	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
		if (b->base == 0) {
			printf("adreno: %u: cannot map %s at %#x (%#x bytes)\n", adreno_step,
			    b->name != NULL ? b->name : "block", b->phys, b->size);
			return false;
		}
	}
	return true;
}

static void
hold(bool single_access)
{
	if (adreno_step == adreno_newest_step && adreno_pause_ms != 0 && (!single_access || adreno_fine)) {
		sleep_ms(adreno_pause_ms);
	}
}

static void
announce(const char *what)
{
	printf("adreno: step %u: %s\n", adreno_step, what);
	hold(false);
}

static uint32_t
get(struct adreno_block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	if (!need(b)) {
		return 0;
	}
	if (adreno_quiet) {
		return rd(b->base + offset);
	}
	if (adreno_step == adreno_newest_step) {
		printf("adreno: %u: about to read %s %s at %#x\n", adreno_step, b->name, name, b->phys + offset);
		hold(true);
	}
	value = rd(b->base + offset);
	printf("adreno: %u: %s %s (%#x) = %#x\n", adreno_step, b->name, name, b->phys + offset, value);
	return value;
}

static uint32_t
get8(struct adreno_block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	if (!need(b)) {
		return 0;
	}
	if (adreno_step == adreno_newest_step) {
		printf("adreno: %u: about to read %s %s at %#x (a byte)\n", adreno_step, b->name, name, b->phys + offset);
		hold(true);
	}
	value = rd8(b->base + offset);
	printf("adreno: %u: %s %s (%#x) = %#x\n", adreno_step, b->name, name, b->phys + offset, value);
	return value;
}

static void
put(struct adreno_block *b, const char *name, uint32_t offset, uint32_t value)
{
	if (!need(b)) {
		return;
	}
	if (!adreno_quiet) {
		printf("adreno: %u: about to write %s %s at %#x: %#x\n", adreno_step, b->name, name, b->phys + offset, value);
		snprintf(adreno_last_write, sizeof(adreno_last_write), "step %u wrote %#x to %s %s at %#x",
		    adreno_step, value, b->name, name, b->phys + offset);
		hold(true);
	}
	wr(b->base + offset, value);
}

static uint32_t
set(struct adreno_block *b, const char *name, uint32_t offset, uint32_t bits)
{
	uint32_t was = get(b, name, offset);

	if ((was & bits) == bits) {
		return was;
	}
	put(b, name, offset, was | bits);
	return get(b, name, offset);
}

static bool
wait_for(struct adreno_block *b, uint32_t offset, uint32_t mask, uint32_t value, uint32_t ms)
{
	if (b->base == 0) {
		return false;
	}
	for (uint32_t i = 0; i < ms * 10; i++) {
		if ((rd(b->base + offset) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	return (rd(b->base + offset) & mask) == value;
}

/* ---- SM6115 GCC / GPUCC (gcc-sm6115.c, gpucc-sm6115.c, gcc-atoll.c for AXI) ----------------------- */

#define CBCR_ENABLE             (1u << 0)
#define CBCR_OFF                (1u << 31)
#define CBCR_RETAIN_PERIPH      (1u << 13)
#define CBCR_RETAIN_MEM         (1u << 14)

#define GCC_PLL_VOTE_GPLL0      (1u << 15)      /* gcc_gpu_gpll0_clk_src, enable_reg 0x79004 */
#define GCC_VENDOR_GPU_AXI_CBCR 0x4452c         /* vendor gcc_ddrss_gpu_axi; also enable beside plat 0x71154 */

#define GPUCC_PLL0_MODE         0x0000
#define GPUCC_PLL1_MODE         0x0100          /* 640 MHz: gpucc-sm6115.c gpu_cc_pll1_config */
#define GPUCC_PLL_L_VAL         0x04
#define GPUCC_PLL_ALPHA         0x08
#define GPUCC_PLL_ALPHA_U       0x0c
#define GPUCC_PLL_USER_CTL      0x10
#define GPUCC_PLL_CONFIG_CTL    0x18            /* clk-alpha-pll.c CLK_ALPHA_PLL_TYPE_DEFAULT */
#define PLL_OUTCTRL             (1u << 0)
#define PLL_BYPASSNL            (1u << 1)
#define PLL_RESET_N             (1u << 2)
#define PLL_LOCK_DET            (1u << 31)
#define GPUCC_CXO_AON_CBCR      0x1004          /* BRANCH_HALT_DELAY */
#define GPUCC_GX_SW_RESET       0x1008          /* GPU_GX_BCR */
#define GPUCC_GX_GDSCR          0x100c
#define GPUCC_GFX3D_RCGR        0x101c          /* +0 command, +4 cfg; first OPP 320 MHz */
#define GPUCC_GX_GFX3D_CBCR     0x1054          /* BRANCH_HALT_SKIP */
#define GPUCC_GX_CXO_CBCR       0x1060          /* CLK_IS_CRITICAL, BRANCH_HALT_DELAY */
#define GPUCC_CX_GDSCR          0x106c
#define GPUCC_AHB_CBCR          0x1078          /* BRANCH_HALT_DELAY */
#define GPUCC_CX_GMU_CBCR       0x1098          /* BRANCH_HALT; dt clock "gmu" */
#define GPUCC_CXO_CBCR          0x109c          /* BRANCH_HALT; dt clock "xo" */
#define GPUCC_CX_GFX3D_CBCR     0x10a4
#define GPUCC_GMU_RCGR          0x1120          /* 200 MHz from GPLL0/3 */
#define GPUCC_GX_CLAMP_IO       0x1508
#define GPUCC_CX_HW_CTRL        0x1540
#define GPUCC_SMMU_VOTE_CBCR    0x5000          /* BRANCH_VOTED */

#define GDSC_SW_COLLAPSE        (1u << 0)
#define GDSC_HW_CONTROL         (1u << 1)
#define GDSC_POWER_ON           (1u << 31)
#define GMEM_CLAMP_IO           (1u << 0)

#define RCG_UPDATE              (1u << 0)
#define RCG_SOURCE_MASK         0x371fu         /* [4:0] 2*div-1, [10:8] source, [13:12] mode */
#define RCG_SRC_DIV(src, div)   ((uint32_t)(src) << 8 | (2u * (div) - 1))
#define RCG_GFX3D_PLL1_AUX      3u              /* gpu_cc_parent_map_1: P_GPU_CC_PLL1_OUT_AUX */
#define RCG_GFX3D_GPLL0         5u
#define RCG_GMU_GPLL0           5u

#define GMU_WORD(n)             (4u * (n))
#define GPU_WORD(n)             (4u * (n))
#define GMU_CM3_SYSRESET        GMU_WORD(0x5000)
#define GMU_ALWAYS_ON_COUNTER_L GMU_WORD(0x5088)
#define GMU_ALWAYS_ON_COUNTER_H GMU_WORD(0x5089)
#define GMU_AO_GPU_CX_BUSY_STATUS GMU_WORD(0x930c)
#define GBIF_HALT               (4 * 0x45)
#define GBIF_HALT_ACK           (4 * 0x46)
#define RBBM_GBIF_HALT          GPU_WORD(0x0016)
#define RBBM_GBIF_HALT_ACK      GPU_WORD(0x0017)
#define CP_SQE_CNTL             GPU_WORD(0x0808)
#define SMMU_IDR1               0x024
#define SMMU_CB_SCTLR           0x00

static uint32_t
gcc_pll_vote(void)
{
	return plat != NULL && plat->gcc_pll_vote != 0 ? plat->gcc_pll_vote : 0x79004;
}

static uint32_t
gcc_cfg_ahb(void)
{
	return plat != NULL && plat->gcc_gpu_cfg_ahb_cbcr != 0 ? plat->gcc_gpu_cfg_ahb_cbcr : 0x36004;
}

static uint32_t
gcc_memnoc(void)
{
	return plat != NULL && plat->gcc_gpu_memnoc_gfx_cbcr != 0 ? plat->gcc_gpu_memnoc_gfx_cbcr : 0x3600c;
}

static uint32_t
gcc_snoc_dvm(void)
{
	return plat != NULL && plat->gcc_gpu_snoc_dvm_gfx_cbcr != 0 ? plat->gcc_gpu_snoc_dvm_gfx_cbcr : 0x36018;
}

static uint32_t
gcc_gpu_axi(void)
{
	return plat != NULL && plat->gcc_ddrss_gpu_axi_cbcr != 0 ? plat->gcc_ddrss_gpu_axi_cbcr : 0x71154;
}

static uint32_t
gpll_vote_bits(void)
{
	uint32_t div_bit = plat != NULL && plat->gcc_pll_vote_div_bit != 0 ? plat->gcc_pll_vote_div_bit : 16;

	return GCC_PLL_VOTE_GPLL0 | (1u << div_bit);
}

/* ---- step 1: clocks from outside, and RPMh's names ------------------------------------------------ */

#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static const struct cmd_db_header *cmd_db;

/*
 * rpmh_resource is static in redwood_adreno.c. Local lookup of the same cmd-db format;
 * if the magic is not there, skip rather than invent RPMh addresses.
 */
static uint32_t
rpmh_resource(const char *name, uint16_t *levels, unsigned *level_count)
{
	const struct cmd_db_header *db = cmd_db;
	char id[8] = { 0 };

	if (level_count != NULL) {
		*level_count = 0;
	}
	if (db == NULL) {
		db = cmd_db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
	}
	if (db == NULL || db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		cmd_db = NULL;
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			const uint8_t *data = db->data + slave->data_offset + entry[i].offset;
			unsigned count = entry[i].length / 2u;

			if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			if ((unsigned)slave->data_offset + entry[i].offset + entry[i].length > CMD_DB_SIZE - 0x1000) {
				count = 0;
			}
			for (unsigned l = 0; levels != NULL && level_count != NULL && l < count && l < 16; l++) {
				levels[l] = (uint16_t)(data[2 * l] | data[2 * l + 1] << 8);
				if (l > 0 && levels[l] == 0) {
					break;
				}
				*level_count = l + 1;
			}
			return entry[i].address;
		}
	}
	return 0;
}

static void
show_rpmh_resource(const char *name)
{
	uint16_t level[16];
	unsigned count = 0;
	uint32_t address = rpmh_resource(name, level, &count);
	char levels[160];
	int at = 0;

	if (address == 0) {
		printf("adreno: %u: RPMh has no \"%s\"\n", adreno_step, name);
		return;
	}
	levels[0] = 0;
	for (unsigned l = 0; l < count && at < (int)sizeof(levels) - 8; l++) {
		at += snprintf(levels + at, sizeof(levels) - (size_t)at, " %u", level[l]);
	}
	printf("adreno: %u: RPMh has \"%s\" at %#x; levels:%s\n", adreno_step, name, address, levels);
}

static bool
step_outside(void)
{
	announce("read the GPU's clocks in the global clock controller (SM6115 GCC 0x01400000; citrus_touch uses this too)");
	if (!need(&adreno_gcc)) {
		return false;
	}
	get(&adreno_gcc, "pll votes", gcc_pll_vote());
	get(&adreno_gcc, "gpu_cfg_ahb", gcc_cfg_ahb());
	get(&adreno_gcc, "gpu_memnoc_gfx", gcc_memnoc());
	get(&adreno_gcc, "gpu_snoc_dvm_gfx", gcc_snoc_dvm());
	get(&adreno_gcc, "ddrss_gpu_axi", gcc_gpu_axi());

	announce("look up the GPU's supply rails in the boot loader's list of RPMh resources (memory at 0x80860000)");
	if (rpmh_resource("gfx.lvl", NULL, NULL) == 0 && cmd_db == NULL) {
		printf("adreno: %u: cmd-db at 0x80860000 is not there (SM6115 dtsi has no cmd_db node; "
		    "not inventing RPMh addresses). gfx.lvl/cx.lvl/mx.lvl lookup skipped\n", adreno_step);
		return true;
	}
	show_rpmh_resource("gfx.lvl");
	show_rpmh_resource("cx.lvl");
	show_rpmh_resource("mx.lvl");
	return true;
}

/* ---- step 2: GPU speed-bin fuses ------------------------------------------------------------------ */

static const char *
a610_speed_bin_name(uint32_t fuse)
{
	/* Linux a6xx_gpu.c a610_get_speed_bin for bengal/SM6115. */
	switch (fuse) {
	case 0:
		return "bin 0";
	case 206:
		return "bin 1";
	case 200:
		return "bin 2 (950 MHz cap)";
	case 157:
		return "bin 3";
	case 127:
		return "bin 4";
	default:
		return "unknown bin";
	}
}

static bool
step_fuses(void)
{
	uint32_t offset, bin;

	offset = (plat != NULL ? plat->qfprom_corrected : 0) +
	    (plat != NULL && plat->qfprom_speed_bin != 0 ? plat->qfprom_speed_bin : 0x6006);
	announce("read the GPU's speed-bin fuse (sm6115.dtsi qfprom@1b40000 gpu_speed_bin@6006, bits 5..12)");
	if (!need(&adreno_qfprom)) {
		return false;
	}
	bin = get8(&adreno_qfprom, "speed bin, low", offset);
	bin |= get8(&adreno_qfprom, "speed bin, high", offset + 1) << 8;
	bin = bin >> 5 & 0xff;
	adreno_speed_bin = bin;
	printf("adreno: %u: speed bin fuse %u: %s (Adreno 610)\n", adreno_step, bin, a610_speed_bin_name(bin));
	return true;
}

/* ---- step 3: GPUCC as the boot loader left it ----------------------------------------------------- */

static bool
step_gpucc(void)
{
	announce("make sure the clock of the GPU clock controller's registers runs (gcc_gpu_cfg_ahb, BRANCH_HALT)");
	if ((set(&adreno_gcc, "gpu_cfg_ahb", gcc_cfg_ahb(), CBCR_ENABLE) & CBCR_OFF) &&
	    !wait_for(&adreno_gcc, gcc_cfg_ahb(), CBCR_OFF, 0, 10)) {
		printf("adreno: %u: it stays off (%#x): the GPU's clock controller would not answer\n", adreno_step,
		    adreno_gcc.base != 0 ? rd(adreno_gcc.base + gcc_cfg_ahb()) : 0);
		return false;
	}

	announce("read the GPU's clock controller (0x05990000) as it is: the first access to the GPU's part of the SoC");
	if (!need(&adreno_gpucc)) {
		return false;
	}
	get(&adreno_gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
	get(&adreno_gpucc, "cx_hw_ctrl", GPUCC_CX_HW_CTRL);
	get(&adreno_gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
	get(&adreno_gpucc, "gx_sw_reset", GPUCC_GX_SW_RESET);
	get(&adreno_gpucc, "gx_clamp_io", GPUCC_GX_CLAMP_IO);
	get(&adreno_gpucc, "pll0 mode", GPUCC_PLL0_MODE);
	get(&adreno_gpucc, "pll0 L", GPUCC_PLL0_MODE + GPUCC_PLL_L_VAL);
	get(&adreno_gpucc, "pll1 mode", GPUCC_PLL1_MODE);
	get(&adreno_gpucc, "pll1 L", GPUCC_PLL1_MODE + GPUCC_PLL_L_VAL);
	get(&adreno_gpucc, "gfx3d_clk_src command", GPUCC_GFX3D_RCGR);
	get(&adreno_gpucc, "gfx3d_clk_src source", GPUCC_GFX3D_RCGR + 4);
	get(&adreno_gpucc, "gmu_clk_src command", GPUCC_GMU_RCGR);
	get(&adreno_gpucc, "gmu_clk_src source", GPUCC_GMU_RCGR + 4);
	get(&adreno_gpucc, "cxo_aon", GPUCC_CXO_AON_CBCR);
	get(&adreno_gpucc, "gx_gfx3d", GPUCC_GX_GFX3D_CBCR);
	get(&adreno_gpucc, "gx_cxo", GPUCC_GX_CXO_CBCR);
	get(&adreno_gpucc, "ahb", GPUCC_AHB_CBCR);
	get(&adreno_gpucc, "cx_gmu", GPUCC_CX_GMU_CBCR);
	get(&adreno_gpucc, "cxo", GPUCC_CXO_CBCR);
	get(&adreno_gpucc, "cx_gfx3d", GPUCC_CX_GFX3D_CBCR);
	get(&adreno_gpucc, "smmu vote", GPUCC_SMMU_VOTE_CBCR);
	return true;
}

static bool
gpucc_answers(void)
{
	if (!need(&adreno_gcc) || !need(&adreno_gpucc)) {
		return false;
	}
	if (rd(adreno_gcc.base + gcc_cfg_ahb()) & CBCR_OFF) {
		printf("adreno: %u: the GPU clock controller's registers have no clock (gcc gpu_cfg_ahb %#x): step 3\n",
		    adreno_step, rd(adreno_gcc.base + gcc_cfg_ahb()));
		return false;
	}
	return true;
}

static bool
cx_is_on(void)
{
	if (adreno_gpucc.base == 0) {
		return false;
	}
	return (rd(adreno_gpucc.base + GPUCC_CX_GDSCR) | rd(adreno_gpucc.base + GPUCC_CX_HW_CTRL)) & GDSC_POWER_ON;
}

static bool
gx_is_on(void)
{
	return adreno_gpucc.base != 0 && (rd(adreno_gpucc.base + GPUCC_GX_GDSCR) & GDSC_POWER_ON) != 0;
}

/* ---- step 4: CX GDSC ------------------------------------------------------------------------------ */

static bool
step_cx_power(void)
{
	uint32_t gdscr;

	if (!gpucc_answers()) {
		return false;
	}
	announce("switch the CX half of the GPU block on (gpu_cx_gdsc 0x106c, hw-ctrl 0x1540): clear the collapse bit");
	gdscr = get(&adreno_gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
	get(&adreno_gpucc, "cx_hw_ctrl", GPUCC_CX_HW_CTRL);
	if (cx_is_on()) {
		printf("adreno: %u: CX is on already: going on with it as it is\n", adreno_step);
		adreno_set_cx_on(true);
		return true;
	}
	if (gdscr & GDSC_HW_CONTROL) {
		printf("adreno: %u: the switch is under hardware control: not ours to operate\n", adreno_step);
		return false;
	}
	put(&adreno_gpucc, "cx_gdscr", GPUCC_CX_GDSCR, gdscr & ~GDSC_SW_COLLAPSE);
	sleep_us(1);
	for (int i = 0; i < 500 && !cx_is_on(); i++) {
		sleep_us(100);
	}
	get(&adreno_gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
	get(&adreno_gpucc, "cx_hw_ctrl", GPUCC_CX_HW_CTRL);
	if (!cx_is_on()) {
		printf("adreno: %u: CX does not come on\n", adreno_step);
		return false;
	}
	printf("adreno: %u: CX is on\n", adreno_step);
	adreno_set_cx_on(true);
	if (gx_is_on()) {
		printf("adreno: %u: GX is on already (an earlier kernel may have left it); not toggling it\n",
		    adreno_step);
	}
	return true;
}

/* ---- step 5: AHB + mem_iface + core clocks -------------------------------------------------------- */

static bool
clock_on(struct adreno_block *b, const char *name, uint32_t cbcr, bool must, uint32_t extra)
{
	uint32_t bits = CBCR_ENABLE | extra;

	if (!(set(b, name, cbcr, bits) & CBCR_OFF) || wait_for(b, cbcr, CBCR_OFF, 0, 10)) {
		return true;
	}
	printf("adreno: %u: %s %s stays off (%#x)%s\n", adreno_step, b->name, name, rd(b->base + cbcr),
	    must ? "" : "; halt_check is not BRANCH_HALT, going on");
	return !must;
}

static bool
clock_source(const char *name, uint32_t rcgr, uint32_t source)
{
	uint32_t was = get(&adreno_gpucc, name, rcgr + 4);

	if ((was & RCG_SOURCE_MASK) == source) {
		return true;
	}
	put(&adreno_gpucc, name, rcgr + 4, (was & ~RCG_SOURCE_MASK) | source);
	put(&adreno_gpucc, name, rcgr, rd(adreno_gpucc.base + rcgr) | RCG_UPDATE);
	if (!wait_for(&adreno_gpucc, rcgr, RCG_UPDATE, 0, 10)) {
		printf("adreno: %u: %s does not take its new source (command %#x)\n", adreno_step, name,
		    rd(adreno_gpucc.base + rcgr));
		return false;
	}
	get(&adreno_gpucc, name, rcgr + 4);
	return true;
}

/*
 * PLL1 at 640 MHz (gpucc-sm6115.c gpu_cc_pll1_config), then gfx3d RCG /2 = 320 MHz, the first OPP.
 * clk-alpha-pll TYPE_DEFAULT; enable: BYPASSNL, RESET_N, wait LOCK, OUTCTRL. If it is already
 * locked, leave it. If it does not lock, the caller uses GPLL0 / 2 instead.
 */
static bool
pll1_on(void)
{
	uint32_t mode, user;

	mode = get(&adreno_gpucc, "pll1 mode", GPUCC_PLL1_MODE);
	if (mode & PLL_LOCK_DET) {
		printf("adreno: %u: PLL1 is already locked (%#x)\n", adreno_step, mode);
		if (!(mode & PLL_OUTCTRL)) {
			put(&adreno_gpucc, "pll1 mode", GPUCC_PLL1_MODE, mode | PLL_OUTCTRL);
		}
		return true;
	}
	announce("configure GPUCC PLL1 at 640 MHz (Linux gpu_cc_pll1_config) for the 320 MHz 3D clock");
	put(&adreno_gpucc, "pll1 L", GPUCC_PLL1_MODE + GPUCC_PLL_L_VAL, 0x21);
	put(&adreno_gpucc, "pll1 alpha", GPUCC_PLL1_MODE + GPUCC_PLL_ALPHA, 0x55555555);
	put(&adreno_gpucc, "pll1 alpha_u", GPUCC_PLL1_MODE + GPUCC_PLL_ALPHA_U, 0x55);
	user = 1u << 24 | 1u << 1 | 1u << 0 | 0x2u << 20;       /* alpha_en, aux, main, VCO 2 */
	put(&adreno_gpucc, "pll1 user_ctl", GPUCC_PLL1_MODE + GPUCC_PLL_USER_CTL, user);
	put(&adreno_gpucc, "pll1 config_ctl", GPUCC_PLL1_MODE + GPUCC_PLL_CONFIG_CTL, 0x4001055b);
	/* TYPE_DEFAULT has no TEST_CTL_U1; Linux's test_ctl_hi1_val is unused for this PLL. */
	mode = rd(adreno_gpucc.base + GPUCC_PLL1_MODE);
	put(&adreno_gpucc, "pll1 mode", GPUCC_PLL1_MODE, mode | PLL_BYPASSNL);
	sleep_us(10);
	put(&adreno_gpucc, "pll1 mode", GPUCC_PLL1_MODE, rd(adreno_gpucc.base + GPUCC_PLL1_MODE) | PLL_RESET_N);
	for (int i = 0; i < 1000; i++) {
		if (rd(adreno_gpucc.base + GPUCC_PLL1_MODE) & PLL_LOCK_DET) {
			break;
		}
		sleep_us(10);
	}
	mode = get(&adreno_gpucc, "pll1 mode", GPUCC_PLL1_MODE);
	if (!(mode & PLL_LOCK_DET)) {
		printf("adreno: %u: PLL1 does not lock (%#x); gfx3d will use GPLL0 / 2 instead of 320 MHz\n",
		    adreno_step, mode);
		return false;
	}
	put(&adreno_gpucc, "pll1 mode", GPUCC_PLL1_MODE, mode | PLL_OUTCTRL);
	return true;
}

static bool
step_clocks(void)
{
	uint32_t gfx3d_src, want_khz;
	bool pll1;

	if (!gpucc_answers()) {
		return false;
	}
	if (!cx_is_on()) {
		printf("adreno: %u: CX is off: step 4\n", adreno_step);
		return false;
	}

	announce("in the global clock controller: GPLL0 and GPLL0_DIV votes; the GPU's bus and memory clocks on");
	set(&adreno_gcc, "pll votes", gcc_pll_vote(), gpll_vote_bits());
	if (!clock_on(&adreno_gcc, "gpu_memnoc_gfx", gcc_memnoc(), false, 0) ||
	    !clock_on(&adreno_gcc, "gpu_snoc_dvm_gfx", gcc_snoc_dvm(), true, 0) ||
	    !clock_on(&adreno_gcc, "ddrss_gpu_axi", gcc_gpu_axi(), false, 0)) {
		return false;
	}

	announce("in the GPU's clock controller: CX_GMU from GPLL0 / 3 (200 MHz); not SC7280 hub clocks");
	if (!clock_source("gmu_clk_src", GPUCC_GMU_RCGR, RCG_SRC_DIV(RCG_GMU_GPLL0, 3))) {
		return false;
	}

	want_khz = plat != NULL && plat->first_opp_khz != 0 ? plat->first_opp_khz : 320000;
	pll1 = pll1_on();
	if (pll1) {
		announce("in the GPU's clock controller: gfx3d to the first OPP (320 MHz: PLL1_OUT_AUX / 2)");
		gfx3d_src = RCG_SRC_DIV(RCG_GFX3D_PLL1_AUX, 2);
		adreno_core_khz = want_khz;
	} else {
		announce("in the GPU's clock controller: gfx3d from GPLL0 / 2 (~300 MHz)");
		gfx3d_src = RCG_SRC_DIV(RCG_GFX3D_GPLL0, 2);
		adreno_core_khz = 300000;
	}
	if (!clock_source("gfx3d_clk_src", GPUCC_GFX3D_RCGR, gfx3d_src)) {
		return false;
	}

	announce("in the GPU's clock controller: CXO, AHB, GX_GFX3D, GX_CXO, CX_GMU, SMMU vote (gpucc-sm6115.c CBCRs)");
	if (!clock_on(&adreno_gpucc, "cxo_aon", GPUCC_CXO_AON_CBCR, false, 0) ||
	    !clock_on(&adreno_gpucc, "cxo", GPUCC_CXO_CBCR, true, 0) ||
	    !clock_on(&adreno_gpucc, "ahb", GPUCC_AHB_CBCR, false, 0) ||
	    !clock_on(&adreno_gpucc, "gx_cxo", GPUCC_GX_CXO_CBCR, false, 0) ||
	    !clock_on(&adreno_gpucc, "gx_gfx3d", GPUCC_GX_GFX3D_CBCR, false, CBCR_RETAIN_MEM | CBCR_RETAIN_PERIPH) ||
	    !clock_on(&adreno_gpucc, "cx_gfx3d", GPUCC_CX_GFX3D_CBCR, false, 0) ||
	    !clock_on(&adreno_gpucc, "cx_gmu", GPUCC_CX_GMU_CBCR, true, 0) ||
	    !clock_on(&adreno_gpucc, "smmu vote", GPUCC_SMMU_VOTE_CBCR, false, 0)) {
		return false;
	}
	printf("adreno: %u: clocks asked for; gfx3d %u kHz (CBCR enable bits, no debugcc 0x62000)\n",
	    adreno_step, adreno_core_khz);
	return true;
}

static bool
cx_ready(void)
{
	uint32_t cxo, gmu;

	if (!gpucc_answers()) {
		return false;
	}
	cxo = rd(adreno_gpucc.base + GPUCC_CXO_CBCR);
	gmu = rd(adreno_gpucc.base + GPUCC_CX_GMU_CBCR);
	if (!cx_is_on() || (cxo & (CBCR_ENABLE | CBCR_OFF)) != CBCR_ENABLE ||
	    (gmu & (CBCR_ENABLE | CBCR_OFF)) != CBCR_ENABLE) {
		printf("adreno: %u: needs CX on (it is %s), cxo running (%#x) and cx_gmu running (%#x): steps 4 and 5\n",
		    adreno_step, cx_is_on() ? "on" : "off", cxo, gmu);
		return false;
	}
	return true;
}

/* ---- step 6: first words from the GMU wrapper ----------------------------------------------------- */

static bool
step_wrapper_words(void)
{
	uint64_t start, nanos;
	uint32_t before, hz;

	if (!cx_ready()) {
		return false;
	}
	announce("read registers of the GMU wrapper (0x0596a000): the first words from the GPU block; no TCM write, no GMU firmware");
	if (!need(&adreno_gmu)) {
		return false;
	}
	get(&adreno_gmu, "cm3_sysreset", GMU_CM3_SYSRESET);
	get(&adreno_gmu, "ao_gpu_cx_busy_status", GMU_AO_GPU_CX_BUSY_STATUS);
	get(&adreno_gmu, "always_on_counter_h", GMU_ALWAYS_ON_COUNTER_H);
	get(&adreno_gmu, "always_on_counter_l", GMU_ALWAYS_ON_COUNTER_L);
	start = mach_absolute_time();
	before = rd(adreno_gmu.base + GMU_ALWAYS_ON_COUNTER_L);
	sleep_ms(10);
	hz = rd(adreno_gmu.base + GMU_ALWAYS_ON_COUNTER_L) - before;
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &nanos);
	hz = nanos ? (uint32_t)((uint64_t)hz * NSEC_PER_SEC / nanos) : 0;
	if (hz > 18000000 && hz < 20500000) {
		printf("adreno: %u: the wrapper's AO counter counts at %u Hz: the 19.2 MHz crystal: THE GPU BLOCK IS ALIVE\n",
		    adreno_step, hz);
	} else {
		printf("adreno: %u: the wrapper's AO counter counts at %u Hz: not the crystal (the wrapper may not have one)\n",
		    adreno_step, hz);
	}

	announce("read the GPU's bus interface in the CX half (GBIF at kgsl + 0x3c00 words)");
	if (!need(&adreno_gbif)) {
		return false;
	}
	get(&adreno_gbif, "halt", GBIF_HALT);
	get(&adreno_gbif, "halt_ack", GBIF_HALT_ACK);
	return true;
}

/* ---- steps 7–10, 12–15: shared 3D-core steps ------------------------------------------------------ */

static bool
step_smmu(void)
{
	if (!cx_ready()) {
		return false;
	}
	need(&adreno_smmu);
	announce("the GPU's SMMU (0x059a0000, 0x10000): shared adreno_step_smmu");
	return adreno_step_smmu();
}

static bool
step_smmu_context(void)
{
	if (!cx_ready()) {
		return false;
	}
	need(&adreno_smmu);
	announce("SMMU translation context 0: shared adreno_step_smmu_context");
	return adreno_step_smmu_context();
}

static bool
step_trustzone(void)
{
	announce("TrustZone: shared adreno_step_trustzone");
	return adreno_step_trustzone();
}

static bool
step_zap_signature(void)
{
	announce("zap shader header (plat a610_zap.mdt): shared adreno_step_zap_signature");
	if (plat != NULL && plat->zap_mdt != NULL) {
		printf("adreno: %u: zap mdt %s\n", adreno_step, plat->zap_mdt);
	}
	return adreno_step_zap_signature();
}

/* ---- step 11: GX GDSC, one reset, no toggle loops ------------------------------------------------- */

static bool
step_gx_power(void)
{
	uint32_t gx, clamp;

	if (!cx_ready()) {
		return false;
	}
	announce("switch the 3D core's GX GDSC on (0x100c, clamp-io 0x1508, sw reset 0x1008); one on, no toggle loops");
	gx = get(&adreno_gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
	clamp = get(&adreno_gpucc, "gx_clamp_io", GPUCC_GX_CLAMP_IO);
	get(&adreno_gpucc, "gx_sw_reset", GPUCC_GX_SW_RESET);
	if (gx & GDSC_POWER_ON) {
		printf("adreno: %u: GX is on already: not resetting it (A610 hangs if GX is reset in a loop)\n",
		    adreno_step);
		adreno_set_core_powered(true);
		return true;
	}
	if (gx & GDSC_HW_CONTROL) {
		printf("adreno: %u: GX is under hardware control: not ours to operate\n", adreno_step);
		return false;
	}
	put(&adreno_gpucc, "gx_gdscr", GPUCC_GX_GDSCR, gx & ~GDSC_SW_COLLAPSE);
	sleep_us(1);
	if (!wait_for(&adreno_gpucc, GPUCC_GX_GDSCR, GDSC_POWER_ON, GDSC_POWER_ON, 100)) {
		get(&adreno_gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
		printf("adreno: %u: GX does not come on\n", adreno_step);
		return false;
	}
	get(&adreno_gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
	if (!gx_reset_once) {
		gx_reset_once = true;
		put(&adreno_gpucc, "gx_sw_reset", GPUCC_GX_SW_RESET, 1);
		sleep_us(100);
		put(&adreno_gpucc, "gx_sw_reset", GPUCC_GX_SW_RESET, 0);
		sleep_us(1);
	}
	if (clamp & GMEM_CLAMP_IO) {
		put(&adreno_gpucc, "gx_clamp_io", GPUCC_GX_CLAMP_IO, clamp & ~GMEM_CLAMP_IO);
	}
	get(&adreno_gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
	get(&adreno_gpucc, "gx_clamp_io", GPUCC_GX_CLAMP_IO);
	if (!gx_is_on()) {
		printf("adreno: %u: GX dropped power after the reset pulse\n", adreno_step);
		return false;
	}
	printf("adreno: %u: GX is on\n", adreno_step);
	adreno_set_core_powered(true);
	return true;
}

static bool
step_core_space(void)
{
	if (!cx_ready() || !adreno_core_is_on) {
		printf("adreno: %u: needs GX on: step 11\n", adreno_step);
		return false;
	}
	if (plat != NULL && !plat->has_lpac) {
		printf("adreno: %u: plat has_lpac=false (A610): shared adreno_step_core_space forces stream1=0\n",
		    adreno_step);
	}
	if (plat != NULL && plat->sqe_name != NULL) {
		printf("adreno: %u: SQE %s\n", adreno_step, plat->sqe_name);
	}
	announce("the 3D core's memory and a630_sqe.fw: shared adreno_step_core_space");
	return adreno_step_core_space();
}

static bool
step_zap_load(void)
{
	uint64_t zap = plat != NULL ? plat->zap_memory : 0;
	uint32_t zap_size = plat != NULL ? plat->zap_memory_size : 0;

	if (zap == 0) {
		printf("adreno: %u: plat zap_memory is 0; sm6115.dtsi reserved-memory pil_gpu_mem is 0x56115000, 0x2000 bytes. "
		    "Trying the shared step anyway (another file sets the plat field)\n", adreno_step);
	} else {
		printf("adreno: %u: zap memory %#llx, %#x bytes (sm6115.dtsi pil_gpu_mem@56115000)\n",
		    adreno_step, zap, zap_size);
	}
	announce("load the zap shader: shared adreno_step_zap_load");
	return adreno_step_zap_load();
}

static bool
step_core_start(void)
{
	announce("start the 3D core: shared adreno_step_core_start");
	return adreno_step_core_start();
}

static bool
step_core_commands(void)
{
	announce("the 3D core's first commands: shared adreno_step_core_commands");
	return adreno_step_core_commands();
}

static bool (*const steps[CITRUS_STEPS])(void) = {
	step_outside, step_fuses, step_gpucc, step_cx_power, step_clocks,
	step_wrapper_words, step_smmu, step_smmu_context, step_trustzone, step_zap_signature,
	step_gx_power, step_core_space, step_zap_load, step_core_start, step_core_commands
};

/* ---- stop / quiesce ------------------------------------------------------------------------------- */

static void
gbif_halt(void)
{
	if (adreno_gpu.base != 0) {
		wr(adreno_gpu.base + RBBM_GBIF_HALT, 1);
		for (int i = 0; i < 100000 && !(rd(adreno_gpu.base + RBBM_GBIF_HALT_ACK) & 1); i++) {
		}
	}
	if (adreno_gbif.base != 0) {
		wr(adreno_gbif.base + GBIF_HALT, 1);
		for (int i = 0; i < 100000 && !(rd(adreno_gbif.base + GBIF_HALT_ACK) & 1); i++) {
		}
		wr(adreno_gbif.base + GBIF_HALT, 2);
		for (int i = 0; i < 100000 && !(rd(adreno_gbif.base + GBIF_HALT_ACK) & 2); i++) {
		}
	}
}

void
citrus_adreno_block_stop(void)
{
	uint32_t gx, cx;

	adreno_step = 0;
	adreno_newest_step = ~0u;
	if (adreno_gbif.base != 0 || adreno_gpu.base != 0) {
		printf("adreno: %u: halt GBIF (citrus stop)\n", adreno_step);
		gbif_halt();
	}
	if (adreno_gpucc.base == 0) {
		return;
	}
	gx = rd(adreno_gpucc.base + GPUCC_GX_GDSCR);
	if (gx & GDSC_POWER_ON) {
		printf("adreno: %u: collapse GX once\n", adreno_step);
		wr(adreno_gpucc.base + GPUCC_GX_CLAMP_IO, rd(adreno_gpucc.base + GPUCC_GX_CLAMP_IO) | GMEM_CLAMP_IO);
		wr(adreno_gpucc.base + GPUCC_GX_GDSCR, gx | GDSC_SW_COLLAPSE);
		wait_for(&adreno_gpucc, GPUCC_GX_GDSCR, GDSC_POWER_ON, 0, 100);
		adreno_set_core_powered(false);
	}
	cx = rd(adreno_gpucc.base + GPUCC_CX_GDSCR);
	if (!(cx & GDSC_SW_COLLAPSE) || cx_is_on()) {
		printf("adreno: %u: collapse CX\n", adreno_step);
		wr(adreno_gpucc.base + GPUCC_CX_GDSCR, cx | GDSC_SW_COLLAPSE);
		for (int i = 0; i < 1000 && cx_is_on(); i++) {
			sleep_us(100);
		}
		adreno_set_cx_on(false);
	}
}

void
citrus_adreno_quiesce(void)
{
	uint32_t idr1, page, pages, contexts;

	/*
	 * redwood_adreno_quiesce talks to the bound gpu/smmu/gmu blocks once bind has run, so it
	 * already covers citrus SMMU contexts the shared steps turned on. This function only does
	 * the two writes that stop the core using our memory on maps this file made: SQE halt and
	 * context-0 SCTLR 0. It does not call redwood_adreno.c statics.
	 */
	if (adreno_cx_off) {
		return;
	}
	if (adreno_gpu.base != 0) {
		wr(adreno_gpu.base + CP_SQE_CNTL, 3);           /* a6xx_recover: halt SQE first */
	}
	if (adreno_smmu.base == 0) {
		return;
	}
	idr1 = rd(adreno_smmu.base + SMMU_IDR1);
	page = idr1 >> 31 ? 0x10000 : 0x1000;
	pages = 2u << (idr1 >> 28 & 7);
	contexts = idr1 & 0xff;
	if (contexts == 0 || 2 * pages * page != adreno_smmu.size) {
		return;
	}
	wr(adreno_smmu.base + pages * page + SMMU_CB_SCTLR, 0);
}

/* ---- entry ---------------------------------------------------------------------------------------- */

bool
citrus_adreno_run(uint32_t last_step, uint32_t skip_steps, uint32_t *done, uint32_t *failed)
{
	uint32_t finished = 0, bad = 0;

	if (done != NULL) {
		*done = 0;
	}
	if (failed != NULL) {
		*failed = 0;
	}
	if (last_step == 0) {
		return true;
	}

	plat = adreno_current_plat();
	adreno_bind_plat(plat);
	if (plat == NULL || plat->name == NULL || strcmp(plat->name, "citrus") != 0) {
		printf("adreno: citrus_adreno_run: platform is %s, not citrus\n",
		    plat != NULL && plat->name != NULL ? plat->name : "(none)");
		if (failed != NULL) {
			*failed = 1;
		}
		return false;
	}
	adreno_skip_ahb_measure = true;         /* no SC7280 debugcc on SM6115 */
	PE_parse_boot_argn("adreno_pause", &adreno_pause_ms, sizeof(adreno_pause_ms));
	PE_parse_boot_argn("adreno_fine", &adreno_fine, sizeof(adreno_fine));

	if (last_step > CITRUS_STEPS) {
		last_step = CITRUS_STEPS;
	}
	for (adreno_newest_step = last_step; adreno_newest_step > 0 && (skip_steps & 1u << adreno_newest_step);
	    adreno_newest_step--) {
	}

	printf("adreno: citrus Adreno 610: steps up to %u, without those in mask %#x; step %u is the new one and waits %u ms %s\n",
	    last_step, skip_steps, adreno_newest_step, adreno_pause_ms,
	    adreno_fine ? "before every access" : "after each announcement");
	for (adreno_step = 1; adreno_step <= last_step; adreno_step++) {
		if (skip_steps & 1u << adreno_step) {
			continue;
		}
		if (!steps[adreno_step - 1]()) {
			printf("adreno: step %u: FAILED; the steps after it are not attempted\n", adreno_step);
			bad = adreno_step;
			if (done != NULL) {
				*done = finished;
			}
			if (failed != NULL) {
				*failed = bad;
			}
			return false;
		}
		printf("adreno: step %u: done\n", adreno_step);
		finished = adreno_step;
		if (done != NULL) {
			*done = finished;
		}
	}
	if (failed != NULL) {
		*failed = 0;
	}
	return true;
}
