/*
 * The touch screen of the POCO M3 (citrus, SM6115): a FocalTech FT8719 or FT8720M on the SPI
 * bus of the SoC's serial engine 2, as a source of touches for /dev/redwood-touch
 * (sys/redwood_input.h). Only with boot-arg citrus_touch=1: every address below belongs to
 * this phone.
 *
 * There is no clock, pin or SPI framework in this kernel, so the driver does all of it itself,
 * in the order the vendor's Linux driver (drivers/input/touchscreen/FT8719) does:
 *   clocks   the serial engine's five clocks (m-ahb, s-ahb, core, core-2x, se2), by vote in
 *            the global clock controller (0x1400000); the engine's rate source is the crystal,
 *            undivided (19.2 MHz) -- the driver's divider then picks the bus rate
 *   pins     gpio6, gpio7 and gpio71 to "qup2", gpio80 (chip select 0) to "qup2",
 *            gpio93 as the interrupt input, gpio103 as the reset output
 *            (west tile 0x500000 for 6/7/71/80/93, south tile 0x900000 for 103;
 *            the SoC hangs if a pin is read at the other tile's 0x1000*n)
 *   SPI      the serial engine ("GENI") in FIFO mode, polled; the FTS packages that want
 *            their first byte handled alone (30 us before the rest, chip select held low)
 *            are split into two engine commands ("fragmentation")
 *   chip     reset; read the chip ID (registers 0xA3 and 0x9F, which say 0x87 0x19 for an
 *            FT8719); then read the touch report and post it
 *
 * The FTS SPI framing is the vendor's focaltech_spi.c, not the FT3680 one of redwood_goodix.c:
 *   package type byte first: 0x05 status, 0xC0 command, 0x3F data
 *   0x05:              { 0x05, 0x00 } -> status in the second byte (bit 7 busy)
 *   0xC0:              { 0xC0, ctrl | cmdlen, cmd... } -> ctrl 0xA0 read (with CRC), 0x00 write
 *   read command:      cmd = { register, length >> 8, length }
 *   0x3F (read):       { 0x3F, 0x00 ... } -> length data bytes, then CRC-16/Kermit, little-endian
 *   0x3F (write):      { 0x3F, data... }   (no CRC)
 * The touch report itself is read with a data package alone (no command), as the vendor does:
 * 62 bytes: [0] chip type 0x9X, [1] finger count, then ten 6-byte points
 *   [0] bits 7:6 event (0 down, 1 up, 2 contact), bits 3:0 x bits 11:8
 *   [1] x bits 7:0
 *   [2] bits 7:4 finger id, bits 3:0 y bits 11:8
 *   [3] y bits 7:0
 *   [4] pressure
 *   [5] area
 * Coordinates are the panel's pixels (1080 x 2340, the device tree's display-coords); they
 * are scaled to the 0..65535 that /dev/redwood-touch carries.
 *
 * No interrupt is used: a thread looks at the interrupt line (boot-arg touch_poll_us=N:
 * every N microseconds; default 2000).
 *
 * Novatek NT36672x (BOM alternate on the same SPI2/CS0/reset/IRQ): SPI mode 0, then a host
 * download of the panel firmware into SRAM (vendor BOOT_UPDATE_FIRMWARE) before reports work.
 *
 * Boot args: citrus_touch=1, touch_spi_hz=N (default 400000; Novatek bumps to 8 MHz),
 * citrus_touch_simple=1 (one engine command per package instead of the split),
 * touch_flip=1..3, touch_poll_us=N.
 *
 * Register layouts: drivers/clk/qcom/gcc-sm6115.c (vote 0x7900c bits 6,7,8,9,12; SE2 CBCR
 * 0x1f3a4; SE2 RCG 0x1f3a8/0x1f3ac), drivers/pinctrl/qcom/pinctrl-sm6115.c (tiles west
 * 0x500000, south 0x900000, east 0xd00000; ctl = tile + 0x1000 * gpio), drivers/spi/spi-geni-qcom.c,
 * the phone's own device tree (spi@4a88000, qupv3_se2_spi pins, focaltech@0), and the vendor's
 * FT8719 driver (greenforce kernel_xiaomi_citrus_sm6115, drivers/input/touchscreen/FT8719).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

#include "citrus_novatek_fw.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

/* Every line goes to the loader's log directly as well; see redwood_buttons.c. */
extern void loader_log_putc(char c);
extern void redwood_tl(const char *format, ...) __printflike(1, 2);

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

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

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

/*
 * Pins: SM6115 TLMM is three 4 MB tiles (arch/arm64/boot/dts/qcom/sm6115.dtsi).
 * Each GPIO's cfg/io live at tile + 0x1000 * gpio, and a read of a gpio that belongs
 * to another tile hangs the NoC (watchdog reset, no log). The vendor DT's
 * pinctrl@400000 / 0xc00000 is the whole window; 0x400000 itself is not a tile.
 */
#define TLMM_WEST_PHYS          0x00500000UL
#define TLMM_SOUTH_PHYS         0x00900000UL
#define TLMM_EAST_PHYS          0x00d00000UL
#define TLMM_TILE_SIZE          0x00400000UL
/* Full tiles are 4 MB each; mapping two of them plus GCC exhausts the IO map
 * window (kmem_alloc 1 MB then panics). Only the GPIO pages we touch. */
#define TLMM_WEST_MAP_PINS      94              /* gpio 0..93 inclusive */
#define TLMM_WEST_MAP_SIZE      (0x1000UL * TLMM_WEST_MAP_PINS)
#define TLMM_SOUTH_PIN_OFF      (0x1000UL * PIN_RESET)
#define TLMM_SOUTH_MAP_SIZE     0x2000UL        /* cfg + io for gpio 103 */

#define PULL_NONE               0
#define PULL_DOWN               1
#define PULL_UP                 3
#define PIN_FUNCTION_QUP2       1                                /* the device tree's "qup2" */

#define PIN_SPI_0               6
#define PIN_SPI_1               7
#define PIN_SPI_2               71
#define PIN_CS                  80       /* chip select 0 of the FocalTech */
#define PIN_INT                 93       /* low = a report waits */
#define PIN_RESET               103      /* the vendor's reset-gpio */

static vm_offset_t tlmm_west, tlmm_south, gcc, se;

static vm_offset_t
gpio_regs(unsigned pin)
{
	switch (pin) {
	case PIN_SPI_0:
	case PIN_SPI_1:
	case PIN_SPI_2:
	case PIN_CS:
	case PIN_INT:
		return tlmm_west + 0x1000UL * pin;
	case PIN_RESET:
		/* tlmm_south is already mapped at this pin's cfg register */
		return tlmm_south;
	default:
		return 0;
	}
}

#define GPIO_CFG(n)             (gpio_regs(n))                  /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (gpio_regs(n) + 4)              /* bit 0 in, bit 1 out */

static void
gpio_configure(unsigned pin, unsigned function, unsigned pull, unsigned ma, bool output)
{
	wr(GPIO_CFG(pin), pull | function << 2 | ma << 6 | (output ? 1u << 9 : 0));
}

static void
gpio_set(unsigned pin, bool high)
{
	uint32_t value = rd(GPIO_IO(pin));

	if (high) {
		value |= 2;
	} else {
		value &= ~2u;
	}
	wr(GPIO_IO(pin), value);
}

static unsigned
gpio_get(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
gpio_output(unsigned pin, bool high, unsigned pull, unsigned ma)
{
	gpio_configure(pin, 0, pull, ma, true);
	gpio_set(pin, high);
}

/* ---- clocks (GCC at 0x1400000) ------------------------------------------------------------------ */

#define GCC_PHYS                0x01400000UL
#define GCC_SIZE                0x00080000UL    /* vote at 0x7900c, SE2 RCG at 0x1f3ac */
#define GCC_VOTE                (gcc + 0x7900c)         /* the application processor's votes */
#define VOTE_M_AHB              (1u << 6)
#define VOTE_S_AHB              (1u << 7)
#define VOTE_CORE               (1u << 8)
#define VOTE_CORE_2X            (1u << 9)
#define VOTE_SE2                (1u << 12)
#define GCC_M_AHB_CBCR          (gcc + 0x1f004)
#define GCC_S_AHB_CBCR          (gcc + 0x1f008)
#define GCC_CORE_CBCR           (gcc + 0x1f00c)
#define GCC_CORE_2X_CBCR        (gcc + 0x1f014)
#define GCC_SE2_CBCR            (gcc + 0x1f3a4)         /* bit 31: the clock is off */
#define GCC_SE2_CMD             (gcc + 0x1f3a8)         /* bit 0: take the new configuration */
#define GCC_SE2_CFG             (gcc + 0x1f3ac)         /* [4:0] 2*divider-1, [10:8] source, [13:12] mode */
#define CLOCK_OFF               (1u << 31)
#define XO_HZ                   19200000

static uint32_t se_source_hz = XO_HZ;

static bool
clocks_on(void)
{
	printf("citrus-touch: clock step: set the divider\n");
	/* the crystal, undivided: the engine's rate source (gcc_parent_map_1 source 0) */
	wr(GCC_SE2_CFG, (rd(GCC_SE2_CFG) & ~0x371fu) | 1);
	wr(GCC_SE2_CMD, rd(GCC_SE2_CMD) | 1);
	for (int i = 0; i < 500 && (rd(GCC_SE2_CMD) & 1); i++) {
		sleep_us(10);
	}
	if (rd(GCC_SE2_CMD) & 1) {
		printf("citrus-touch: the serial engine's clock does not take its configuration (%#x, cfg %#x)\n",
		    rd(GCC_SE2_CMD), rd(GCC_SE2_CFG));
		return false;
	}
	printf("citrus-touch: clock step: divider set (cfg %#x)\n", rd(GCC_SE2_CFG));
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_M_AHB | VOTE_S_AHB | VOTE_CORE | VOTE_CORE_2X | VOTE_SE2);
	printf("citrus-touch: clock step: voted %#x\n", rd(GCC_VOTE));
	for (int i = 0; i < 1000; i++) {
		if (!((rd(GCC_M_AHB_CBCR) | rd(GCC_S_AHB_CBCR) | rd(GCC_CORE_CBCR) | rd(GCC_CORE_2X_CBCR) |
		    rd(GCC_SE2_CBCR)) & CLOCK_OFF)) {
			printf("citrus-touch: clock step: running\n");
			return true;
		}
		sleep_us(10);
	}
	printf("citrus-touch: clocks stay off: vote %#x, m %#x s %#x core %#x core2x %#x se2 %#x\n", rd(GCC_VOTE),
	    rd(GCC_M_AHB_CBCR), rd(GCC_S_AHB_CBCR), rd(GCC_CORE_CBCR), rd(GCC_CORE_2X_CBCR), rd(GCC_SE2_CBCR));
	return false;
}

/* ---- SPI through the serial engine (GENI), serial engine 2 at 0x4a88000 ------------------------- */

#define SE_PHYS                 0x04a88000UL
#define SE_SIZE                 0x4000UL
#define GENI_FORCE_DEFAULT      (se + 0x020)
#define GENI_OUTPUT_CTRL        (se + 0x024)
#define GENI_CGC_CTRL           (se + 0x028)
#define GENI_STATUS             (se + 0x040)    /* bit 0: a command is running */
#define GENI_SER_M_CLK_CFG      (se + 0x048)
#define GENI_IF_DISABLE_RO      (se + 0x064)
#define GENI_FW_REVISION_RO     (se + 0x068)    /* [15:8] protocol: 1 = SPI */
#define GENI_CLK_SEL            (se + 0x07c)
#define SPI_CPHA                (se + 0x224)
#define SPI_LOOPBACK            (se + 0x22c)
#define SPI_CPOL                (se + 0x230)
#define SPI_DEMUX_OUTPUT_INV    (se + 0x24c)
#define SPI_DEMUX_SEL           (se + 0x250)
#define GENI_BYTE_GRAN          (se + 0x254)
#define GENI_DMA_MODE_EN        (se + 0x258)
#define SPI_TRANS_CFG           (se + 0x25c)
#define GENI_TX_PACKING_CFG0    (se + 0x260)
#define GENI_TX_PACKING_CFG1    (se + 0x264)
#define SPI_WORD_LEN            (se + 0x268)
#define SPI_TX_TRANS_LEN        (se + 0x26c)
#define SPI_RX_TRANS_LEN        (se + 0x270)
#define SPI_DELAY_COUNTERS      (se + 0x278)
#define GENI_RX_PACKING_CFG0    (se + 0x284)
#define GENI_RX_PACKING_CFG1    (se + 0x288)
#define GENI_M_CMD0             (se + 0x600)
#define GENI_M_CMD_CTRL         (se + 0x604)
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_EN           (se + 0x614)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_S_IRQ_CLEAR        (se + 0x648)
#define GENI_TX_FIFO            (se + 0x700)
#define GENI_RX_FIFO            (se + 0x780)
#define GENI_TX_FIFO_STATUS     (se + 0x800)
#define GENI_RX_FIFO_STATUS     (se + 0x804)
#define GENI_TX_WATERMARK       (se + 0x80c)
#define GENI_RX_WATERMARK       (se + 0x810)
#define GENI_RX_RFR_WATERMARK   (se + 0x814)
#define SE_DMA_TX_IRQ_CLR       (se + 0xc44)
#define SE_DMA_RX_IRQ_CLR       (se + 0xd44)
#define SE_GSI_EVENT_EN         (se + 0xe18)
#define SE_IRQ_EN               (se + 0xe1c)
#define SE_HW_PARAM_0           (se + 0xe24)
#define SE_DMA_GENERAL_CFG      (se + 0xe30)

#define M_CMD_DONE              (1u << 0)
#define M_CMD_CANCEL            (1u << 4)
#define M_CMD_ABORT             (1u << 5)
#define M_ERRORS                ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29))
#define SPI_FULL_DUPLEX         (3u << 27)
#define M_PARAM_FRAGMENT        (1u << 2)       /* keep chip select low after this command */

static uint32_t fifo_depth;

static const char *
geni_spi_init(uint32_t hz)
{
	uint32_t divider;

	if ((rd(GENI_FW_REVISION_RO) >> 8 & 0xff) != 1) {
		return "the serial engine has no SPI firmware";
	}
	if (rd(GENI_IF_DISABLE_RO) & 1) {
		return "the serial engine's FIFO interface is disabled";
	}
	fifo_depth = rd(SE_HW_PARAM_0) >> 16 & 0x3f;
	if (fifo_depth < 4) {
		return "the serial engine reports no FIFO";
	}
	wr(SE_GSI_EVENT_EN, 0);
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(GENI_CGC_CTRL, rd(GENI_CGC_CTRL) | 0x7f);
	wr(SE_DMA_GENERAL_CFG, rd(SE_DMA_GENERAL_CFG) | 0xf);
	wr(GENI_OUTPUT_CTRL, 0x7f);
	wr(GENI_FORCE_DEFAULT, 1);
	wr(GENI_DMA_MODE_EN, rd(GENI_DMA_MODE_EN) & ~1u);
	wr(GENI_RX_WATERMARK, fifo_depth - 3);
	wr(GENI_RX_RFR_WATERMARK, fifo_depth - 2);
	wr(GENI_TX_WATERMARK, 0);
	wr(SE_IRQ_EN, rd(SE_IRQ_EN) | 0xf);
	wr(GENI_M_IRQ_EN, rd(GENI_M_IRQ_EN) | 0x7fc0007f);

	/* default mode 1 for FocalTech; Novatek probe switches to mode 0 */
	wr(SPI_LOOPBACK, 0);
	wr(SPI_DEMUX_SEL, 0);
	wr(SPI_CPHA, 1);
	wr(SPI_CPOL, 0);
	wr(SPI_DEMUX_OUTPUT_INV, 0);
	wr(SPI_TRANS_CFG, rd(SPI_TRANS_CFG) & ~2u);
	wr(SPI_DELAY_COUNTERS, 0);
	wr(GENI_TX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_TX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_RX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_RX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_BYTE_GRAN, 0);
	wr(SPI_WORD_LEN, 8 - 4);

	divider = (se_source_hz + hz - 1) / hz;
	wr(GENI_CLK_SEL, 0);
	wr(GENI_SER_M_CLK_CFG, (divider ? divider : 1) << 4 | 1);
	return NULL;
}

static void
geni_drain(uint8_t *rx, uint32_t length, uint32_t *received)
{
	uint32_t status = rd(GENI_RX_FIFO_STATUS), words = status & 0x01ffffff, bytes = words * 4;

	if (status & 0x80000000u) {
		uint32_t valid = status >> 28 & 7;

		if (valid && valid < 4) {
			bytes -= 4 - valid;
		}
	}
	for (uint32_t i = 0; i < words; i++) {
		uint32_t word = rd(GENI_RX_FIFO);

		for (int j = 0; j < 4 && bytes; j++, bytes--) {
			if (*received < length) {
				if (rx) {
					rx[*received] = (uint8_t)(word >> (8 * j));
				}
				(*received)++;
			}
		}
	}
}

static void
geni_recover(void)
{
	wr(GENI_M_CMD_CTRL, 1u << 2);
	for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL); i++) {
		sleep_us(100);
	}
	if (!(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL)) {
		wr(GENI_M_CMD_CTRL, 1u << 1);
		for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_ABORT); i++) {
			sleep_us(100);
		}
	}
	for (int i = 0; i < 64 && (rd(GENI_RX_FIFO_STATUS) & 0x01ffffff); i++) {
		(void)rd(GENI_RX_FIFO);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
}

/* One command: `length` bytes out (zeros without tx) and as many in; `more` keeps chip select low. */
static bool
geni_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length, bool more)
{
	static unsigned complaints;
	uint32_t sent = 0, received = 0, status = 0;
	uint64_t deadline;

	for (int i = 0; rd(GENI_STATUS) & 1; i++) {
		if (i > 2000) {
			geni_recover();
			break;
		}
		sleep_us(100);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(SPI_TX_TRANS_LEN, length);
	wr(SPI_RX_TRANS_LEN, length);
	wr(GENI_M_CMD0, SPI_FULL_DUPLEX | (more ? M_PARAM_FRAGMENT : 0));
	/* large host-download chunks need more than the old fixed 200 ms */
	clock_interval_to_deadline(200 + length / 40 + 1, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		while (sent < length && (rd(GENI_TX_FIFO_STATUS) & 0x0fffffff) + 1 < fifo_depth) {
			uint32_t word = 0;

			for (int j = 0; j < 4 && sent < length; j++, sent++) {
				word |= (uint32_t)(tx ? tx[sent] : 0) << (8 * j);
			}
			wr(GENI_TX_FIFO, word);
		}
		status = rd(GENI_M_IRQ_STATUS);
		geni_drain(rx, length, &received);
		if (status & M_ERRORS) {
			break;
		}
		if (status & M_CMD_DONE) {
			geni_drain(rx, length, &received);
			wr(GENI_M_IRQ_CLEAR, status);
			if (received == length) {
				return true;
			}
			break;
		}
	}
	if (complaints++ < 8) {
		printf("citrus-touch: SPI transfer of %u bytes failed: status %#x, sent %u, received %u, more %u\n", length,
		    status, sent, received, more);
	}
	geni_recover();
	return false;
}

/*
 * The vendor's driver splits every package after its first byte, with a 30 us pause and chip
 * select held low (two transfers in one SPI message). Boot-arg citrus_touch_simple=1 sends
 * each package as one command instead.
 */
static bool spi_split = true;
/* how far to go: 0 = only look at the clock controller, 1 = clocks on, 2 = +read the engine,
 * 3 = +set the engine up, 4 = +reset the chip, 5 = everything (the default) */
static uint32_t steps = 5;

/*
 * Drive CS on gpio80 as a GPIO. The DT muxes it to qup2 (HW CS0), but with that
 * mux the status package always comes back 0xff — the chip never sees a select.
 * Holding CS ourselves matches how redwood_goodix talks to its SPI panel.
 */
static bool gpio_cs = true;

static void
cs_low(void)
{
	if (gpio_cs) {
		gpio_set(PIN_CS, false);
	}
}

static void
cs_high(void)
{
	if (gpio_cs) {
		gpio_set(PIN_CS, true);
		sleep_us(150);          /* vendor CS_HIGH_DELAY */
	}
}

static bool
spi_package(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	bool ok;

	cs_low();
	if (!spi_split || length < 2) {
		ok = geni_transfer(tx, rx, length, false);
	} else if (!geni_transfer(tx, rx, 1, true)) {
		ok = false;
	} else {
		sleep_us(30);
		ok = geni_transfer(tx + 1, rx ? rx + 1 : NULL, length - 1, false);
	}
	cs_high();
	return ok;
}

/* ---- the FocalTech chip -------------------------------------------------------------------------- */

#define FTS_PACKAGE_STATUS      0x05
#define FTS_PACKAGE_COMMAND     0xC0
#define FTS_PACKAGE_DATA        0x3F
#define FTS_CTRL_READ           0xA0            /* read, data CRC enabled */
#define FTS_CTRL_WRITE          0x00

#define FTS_REG_TOUCH           0x01
#define FTS_REG_CHIP_ID         0xA3
#define FTS_REG_CHIP_ID2        0x9F
#define FTS_REG_FW_VERSION      0xA6
#define FTS_REG_POWER_MODE      0xA5
#define FTS_POWER_MODE_ACTIVE   0x00

#define FTS_MAX_TOUCHES         10
#define FTS_POINT_LENGTH        6
#define FTS_REPORT_LENGTH       (2 + FTS_POINT_LENGTH * FTS_MAX_TOUCHES)  /* 62 */
#define FTS_MAX_READ            128

#define FTS_EVENT_DOWN          0
#define FTS_EVENT_UP            1
#define FTS_EVENT_CONTACT       2

enum { REPORT_UNKNOWN, REPORT_COMMAND, REPORT_BARE };

static uint8_t ft_tx[FTS_MAX_READ + 4], ft_rx[FTS_MAX_READ + 4];
static enum { CHIP_UNKNOWN, CHIP_FOCALTECH, CHIP_NOVATEK, CHIP_ABSENT } chip;
static uint32_t touch_max_x = 1080, touch_max_y = 2340;   /* panel pixels; Novatek FW info may replace */

static void
geni_spi_mode(unsigned mode)
{
	/* SPI mode: bit0 = CPHA, bit1 = CPOL */
	wr(SPI_CPHA, mode & 1);
	wr(SPI_CPOL, (mode >> 1) & 1);
}

static void
geni_spi_hz(uint32_t hz)
{
	uint32_t divider = (se_source_hz + hz - 1) / hz;

	wr(GENI_CLK_SEL, 0);
	wr(GENI_SER_M_CLK_CFG, (divider ? divider : 1) << 4 | 1);
}
static int report_kind = REPORT_UNKNOWN;
static bool fts_with_length = true;     /* the command carries the length (a running firmware) */

static uint16_t
crc16_kermit(const uint8_t *data, uint32_t length)
{
	uint16_t crc = 0xffff;

	for (uint32_t i = 0; i < length; i++) {
		crc ^= data[i];
		for (int bit = 0; bit < 8; bit++) {
			crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
		}
	}
	return crc;
}

static bool
fts_status(uint8_t *status)
{
	ft_tx[0] = FTS_PACKAGE_STATUS;
	ft_tx[1] = 0;
	if (!spi_package(ft_tx, ft_rx, 2)) {
		return false;
	}
	*status = ft_rx[1];
	return true;
}

/*
 * Is the chip's SPI interface ready for the next package?
 * Vendor focaltech_spi.c: mask 0x81; idle is 0x01 with firmware running, 0x00 in boot ROM.
 */
static bool
fts_wait_idle(void)
{
	uint8_t status = 0;
	uint8_t want = fts_with_length ? 0x01 : 0x00;

	for (int i = 0; i < 100; i++) {
		sleep_us(150);
		if (fts_status(&status) && (status & 0x81) == want) {
			return true;
		}
	}
	return false;
}

/* The command package: { 0xC0, ctrl | count, command... }. */
static bool
fts_command(uint8_t ctrl, const uint8_t *command, uint32_t length)
{
	if (length == 0 || length > 15) {
		return false;
	}
	ft_tx[0] = FTS_PACKAGE_COMMAND;
	ft_tx[1] = ctrl | (length & 0x0f);
	memcpy(ft_tx + 2, command, length);
	return spi_package(ft_tx, ft_rx, 2 + length);
}

/* Read `length` bytes of the chip's registers (the command carries the length, as with a running firmware). */
static bool
fts_read(uint8_t address, uint8_t *data, uint32_t length)
{
	uint8_t command[3] = { address, (uint8_t)(length >> 8), (uint8_t)length };
	uint32_t command_length = fts_with_length ? 3 : 1;
	uint32_t transfer = 1 + length + 2;
	bool crc_ok;

	if (length == 0 || length > FTS_MAX_READ) {
		return false;
	}
	if (!fts_wait_idle()) {
		return false;
	}
	if (!fts_command(FTS_CTRL_READ, command, command_length)) {
		return false;
	}
	if (!fts_wait_idle()) {
		return false;
	}
	memset(ft_tx, 0, transfer);
	ft_tx[0] = FTS_PACKAGE_DATA;
	if (!spi_package(ft_tx, ft_rx, transfer)) {
		return false;
	}
	memcpy(data, ft_rx + 1, length);
	crc_ok = crc16_kermit(ft_rx + 1, length) ==
	    (uint16_t)(ft_rx[1 + length] | ft_rx[2 + length] << 8);
	sleep_us(150);
	return crc_ok;
}

/* The vendor reads the touch report with a data package alone: no command before it. */
static bool
fts_read_data(uint8_t *data, uint32_t length)
{
	uint32_t transfer = 1 + length + 2;
	bool crc_ok;

	if (length == 0 || length > FTS_MAX_READ) {
		return false;
	}
	memset(ft_tx, 0, transfer);
	ft_tx[0] = FTS_PACKAGE_DATA;
	if (!spi_package(ft_tx, ft_rx, transfer)) {
		return false;
	}
	memcpy(data, ft_rx + 1, length);
	crc_ok = crc16_kermit(ft_rx + 1, length) ==
	    (uint16_t)(ft_rx[1 + length] | ft_rx[2 + length] << 8);
	return crc_ok;
}

static bool
fts_write(uint8_t address, const uint8_t *data, uint32_t length)
{
	uint8_t command[3] = { address, (uint8_t)(length >> 8), (uint8_t)length };
	uint32_t transfer = 1 + length;

	if (length == 0 || length + 1 > sizeof(ft_tx)) {
		return false;
	}
	if (!fts_wait_idle()) {
		return false;
	}
	if (!fts_command(FTS_CTRL_WRITE, command, 3)) {
		return false;
	}
	if (!fts_wait_idle()) {
		return false;
	}
	ft_tx[0] = FTS_PACKAGE_DATA;
	memcpy(ft_tx + 1, data, length);
	if (!spi_package(ft_tx, ft_rx, transfer)) {
		return false;
	}
	sleep_us(150);
	return true;
}

static void
touch_reset(void)
{
	gpio_output(PIN_RESET, false, PULL_NONE, 8);
	sleep_ms(20);           /* vendor fts_reset_proc: 20 ms low */
	gpio_set(PIN_RESET, true);
	sleep_ms(200);          /* then 200 ms after the release */
}

/* Is an FT8719 (or FT8720M) there and running its firmware? */
static bool
focaltech_probe(bool verbose)
{
	uint8_t high = 0, low = 0, version = 0;
	bool found = false;

	/*
	 * Four ways to ask, in case the engine does not do the split or the chip answers only one
	 * command style: split or single packages, with or without the length in the command.
	 */
	for (unsigned attempt = 0; attempt < 4; attempt++) {
		bool split = (attempt & 1) == 0;
		uint8_t status = 0;

		spi_split = split;
		fts_with_length = attempt < 2;
		high = low = 0;
		if (verbose && fts_status(&status)) {
			printf("citrus-touch: status %#x before ID (%s packages, command %s the length)\n",
			    status, split ? "split" : "single", fts_with_length ? "with" : "without");
		}
		if (!fts_read(FTS_REG_CHIP_ID, &high, 1) || !fts_read(FTS_REG_CHIP_ID2, &low, 1)) {
			if (verbose) {
				printf("citrus-touch: no answer at the chip's ID registers (%s packages, command %s the length)\n",
				    split ? "split" : "single", fts_with_length ? "with" : "without");
			}
			continue;
		}
		printf("citrus-touch: chip ID %02x %02x (an FT8719 is 87 19) with %s packages, command %s the length\n",
		    high, low, split ? "split" : "single", fts_with_length ? "with" : "without");
		if (high == 0x87 && low == 0x19) {
			found = true;
			break;
		}
	}
	if (!found) {
		return false;
	}
	if (fts_read(FTS_REG_FW_VERSION, &version, 1)) {
		printf("citrus-touch: firmware version %#x\n", version);
	}
	/* wake it, in case the last session left it asleep (the vendor's resume does the same) */
	{
		uint8_t active = FTS_POWER_MODE_ACTIVE;

		if (!fts_write(FTS_REG_POWER_MODE, &active, 1)) {
			printf("citrus-touch: could not take the chip out of sleep; carrying on\n");
		}
	}
	return true;
}

/* ---- Novatek NT36672x (same pins; SPI mode 0; DT novatek@1 but chip_select forced to 0) ---------- */

#define NVT_SWRST_N8_ADDR       0x3F0FE         /* DT novatek,swrst-n8-addr */
#define NVT_SPI_RD_FAST_ADDR    0x3F310         /* DT novatek,spi-rd-fast-addr */
#define NVT_ENG_RST_ADDR        0x7FFF80
#define NVT_CHIP_VER_TRIM       0x3F004
#define NVT_CHIP_VER_TRIM_OLD   0x1F64E
#define NVT_EVENT_BUF_ADDR      0x21C00         /* NT36672A_memory_map; trim 0A xx xx 72 66 03 */
#define NVT_POINT_DATA_LEN      65
#define NVT_MAX_TOUCHES         10

static uint32_t nvt_event_buf = NVT_EVENT_BUF_ADDR;

static bool
nvt_xfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	bool ok;

	cs_low();
	ok = geni_transfer(tx, rx, length, false);
	cs_high();
	return ok;
}

static bool
nvt_write(uint8_t *buf, uint32_t length)
{
	if (length == 0) {
		return false;
	}
	buf[0] |= 0x80;                 /* SPI_WRITE_MASK */
	return nvt_xfer(buf, NULL, length);
}

static bool
nvt_read(uint8_t *buf, uint32_t length)
{
	uint8_t tx[128], rx[128];
	uint32_t transfer;

	if (length < 2 || length + 1 > sizeof(tx)) {
		return false;
	}
	transfer = length + 1;          /* one dummy byte on the wire */
	memset(tx, 0, transfer);
	memset(rx, 0, transfer);
	tx[0] = buf[0] & 0x7f;          /* SPI_READ_MASK */
	if (!nvt_xfer(tx, rx, transfer)) {
		return false;
	}
	/* vendor copies from rbuf+2 into buf+1 */
	memcpy(buf + 1, rx + 2, length - 1);
	return true;
}

static bool
nvt_set_page(uint32_t addr)
{
	uint8_t buf[3] = { 0xff, (uint8_t)(addr >> 15), (uint8_t)(addr >> 7) };

	return nvt_write(buf, 3);
}

static bool
nvt_write_addr(uint32_t addr, uint8_t data)
{
	uint8_t buf[3];

	if (!nvt_set_page(addr)) {
		return false;
	}
	buf[0] = (uint8_t)(addr & 0x7f);
	buf[1] = data;
	return nvt_write(buf, 2);
}

static void
nvt_bootloader_reset(void)
{
	nvt_write_addr(NVT_SWRST_N8_ADDR, 0x69);
	sleep_ms(5);
	nvt_write_addr(NVT_SPI_RD_FAST_ADDR, 0x00);
}

#define NVT_BOOT_RDY_ADDR       0x3F10D
#define NVT_BLD_CRC_EN_ADDR     0x3F30E
#define NVT_ILM_DES_ADDR        0x3F128
#define NVT_ILM_LENGTH_ADDR     0x3F118
#define NVT_DLM_DES_ADDR        0x3F12C
#define NVT_DLM_LENGTH_ADDR     0x3F130
#define NVT_G_ILM_CHECKSUM_ADDR 0x3F100
#define NVT_G_DLM_CHECKSUM_ADDR 0x3F104
#define NVT_EVENT_MAP_HOST_CMD  0x50
#define NVT_EVENT_MAP_RESET     0x60
#define NVT_EVENT_MAP_FWINFO    0x78
#define NVT_RESET_STATE_INIT    0xA0
#define NVT_RESET_STATE_MAX     0xAF
#define NVT_FW_CHUNK            4096
#define NVT_BIN_MAP_MAX         32

struct nvt_bin_map {
	uint32_t bin_addr, sram_addr, size, crc;
};

static uint32_t
nvt_word(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool
nvt_check_fw_reset_state(uint8_t want, int retries)
{
	uint8_t buf[8];

	for (int i = 0; i < retries; i++) {
		if (!nvt_set_page(nvt_event_buf | NVT_EVENT_MAP_RESET)) {
			sleep_ms(10);
			continue;
		}
		memset(buf, 0, sizeof(buf));
		buf[0] = NVT_EVENT_MAP_RESET;
		if (!nvt_read(buf, 6)) {
			sleep_ms(10);
			continue;
		}
		if (buf[1] >= want && buf[1] <= NVT_RESET_STATE_MAX) {
			printf("citrus-touch: Novatek firmware reset state %#x (wanted %#x)\n", buf[1], want);
			return true;
		}
		if (i == 0 || i + 1 == retries) {
			printf("citrus-touch: Novatek reset state %#x %#x %#x %#x %#x (wanted %#x+)\n",
			    buf[1], buf[2], buf[3], buf[4], buf[5], want);
		}
		sleep_ms(10);
	}
	return false;
}

static void
nvt_bld_crc_enable(void)
{
	uint8_t buf[2];

	if (!nvt_set_page(NVT_BLD_CRC_EN_ADDR)) {
		return;
	}
	memset(buf, 0, sizeof(buf));
	buf[0] = (uint8_t)(NVT_BLD_CRC_EN_ADDR & 0x7f);
	if (!nvt_read(buf, 2)) {
		return;
	}
	buf[0] = (uint8_t)(NVT_BLD_CRC_EN_ADDR & 0x7f);
	buf[1] |= 0x80;
	nvt_write(buf, 2);
}

static void
nvt_fw_crc_enable(void)
{
	uint8_t buf[2];

	nvt_set_page(nvt_event_buf);
	buf[0] = NVT_EVENT_MAP_RESET;
	buf[1] = 0x00;
	nvt_write(buf, 2);
	buf[0] = NVT_EVENT_MAP_HOST_CMD;
	buf[1] = 0xAE;
	nvt_write(buf, 2);
}

static void
nvt_boot_ready(void)
{
	nvt_write_addr(NVT_BOOT_RDY_ADDR, 1);
	sleep_ms(5);
}

static void
nvt_set_bld_crc_bank(uint32_t des_addr, uint32_t sram_addr, uint32_t length_addr,
    uint32_t size, uint32_t g_checksum_addr, uint32_t crc)
{
	uint8_t buf[8];

	nvt_set_page(des_addr);
	buf[0] = (uint8_t)(des_addr & 0x7f);
	buf[1] = (uint8_t)sram_addr;
	buf[2] = (uint8_t)(sram_addr >> 8);
	buf[3] = (uint8_t)(sram_addr >> 16);
	nvt_write(buf, 4);
	buf[0] = (uint8_t)(length_addr & 0x7f);
	buf[1] = (uint8_t)size;
	buf[2] = (uint8_t)(size >> 8);
	nvt_write(buf, 3);              /* NT36672A hw_crc == 1: 2-byte length */
	buf[0] = (uint8_t)(g_checksum_addr & 0x7f);
	buf[1] = (uint8_t)crc;
	buf[2] = (uint8_t)(crc >> 8);
	buf[3] = (uint8_t)(crc >> 16);
	buf[4] = (uint8_t)(crc >> 24);
	nvt_write(buf, 5);
}

static bool
nvt_write_sram(const uint8_t *fw, uint32_t sram_addr, uint32_t size, uint32_t bin_addr)
{
	static uint8_t chunk[NVT_FW_CHUNK + 1];

	while (size > 0) {
		uint32_t len = size < NVT_FW_CHUNK ? size : NVT_FW_CHUNK;

		if (!nvt_set_page(sram_addr)) {
			return false;
		}
		chunk[0] = (uint8_t)(sram_addr & 0x7f);
		memcpy(chunk + 1, fw + bin_addr, len);
		if (!nvt_write(chunk, len + 1)) {
			return false;
		}
		sram_addr += len;
		bin_addr += len;
		size -= len;
	}
	return true;
}

static int
nvt_parse_bin_map(const uint8_t *fw, uint32_t fwsize, struct nvt_bin_map *map, uint32_t max)
{
	uint32_t end = nvt_word(fw);
	uint32_t info_sec = 0, ovly_sec = 0, partition, list;
	uint8_t ovly_info;

	if (end < 0x30 || end > fwsize) {
		return -1;
	}
	for (uint32_t pos = 0x30; pos < end; pos += 0x10) {
		info_sec++;
	}
	ovly_info = (uint8_t)((fw[0x28] & 0x10) >> 4);
	ovly_sec = ovly_info ? (fw[0x28] & 0x0f) : 0;
	partition = 2 + ovly_sec + info_sec;
	if (partition == 0 || partition > max) {
		return -1;
	}
	for (list = 0; list < partition; list++) {
		if (list < 2) {
			map[list].bin_addr = nvt_word(fw + list * 12);
			map[list].sram_addr = nvt_word(fw + 4 + list * 12);
			map[list].size = nvt_word(fw + 8 + list * 12);
			map[list].crc = nvt_word(fw + 0x18 + list * 4);
		} else if (list < 2 + info_sec) {
			uint32_t pos = 0x30 + 0x10 * (list - 2);

			map[list].sram_addr = nvt_word(fw + pos);
			map[list].size = nvt_word(fw + pos + 4);
			map[list].bin_addr = nvt_word(fw + pos + 8);
			map[list].crc = nvt_word(fw + pos + 12);
		} else {
			uint32_t pos = map[1].bin_addr + 0x10 * (list - 2 - info_sec);

			if (pos + 16 > fwsize) {
				return -1;
			}
			map[list].sram_addr = nvt_word(fw + pos);
			map[list].size = nvt_word(fw + pos + 4);
			map[list].bin_addr = nvt_word(fw + pos + 8);
			map[list].crc = nvt_word(fw + pos + 12);
		}
		if (map[list].size != 0 && map[list].bin_addr + map[list].size + 1 > fwsize) {
			printf("citrus-touch: Novatek FW part %u past end of the binary\n", list);
			return -1;
		}
	}
	return (int)partition;
}

static bool
nvt_write_firmware(const uint8_t *fw, uint32_t fwsize, const struct nvt_bin_map *map, int partition)
{
	for (int list = 0; list < partition; list++) {
		uint32_t size = map[list].size;

		if (size == 0) {
			continue;
		}
		size += 1;
		if (!nvt_write_sram(fw, map[list].sram_addr, size, map[list].bin_addr)) {
			printf("citrus-touch: Novatek SRAM write failed at part %d (sram %#x)\n",
			    list, map[list].sram_addr);
			return false;
		}
	}
	return true;
}

static bool
nvt_host_download(const uint8_t *fw, uint32_t fwsize, const char *name)
{
	struct nvt_bin_map map[NVT_BIN_MAP_MAX];
	int partition;
	uint32_t need = 0;

	for (uint32_t i = fwsize / 4096; i > 0; i--) {
		const uint8_t *end = fw + i * 4096 - 3;

		if ((end[0] == 'N' && end[1] == 'V' && end[2] == 'T') ||
		    (end[0] == 'M' && end[1] == 'O' && end[2] == 'D')) {
			need = i * 4096;
			break;
		}
	}
	if (need == 0 || fw[need - 4096] + fw[need - 4096 + 1] != 0xff) {
		printf("citrus-touch: Novatek %s firmware looks wrong (need %u)\n", name, need);
		return false;
	}
	partition = nvt_parse_bin_map(fw, fwsize, map, NVT_BIN_MAP_MAX);
	if (partition < 2) {
		printf("citrus-touch: Novatek %s firmware header is unreadable\n", name);
		return false;
	}
	printf("citrus-touch: downloading Novatek %s firmware (%u bytes, %d parts)\n",
	    name, need, partition);

	for (int attempt = 0; attempt < 3; attempt++) {
		nvt_bootloader_reset();
		if (!nvt_write_firmware(fw, fwsize, map, partition)) {
			continue;
		}
		nvt_set_bld_crc_bank(NVT_ILM_DES_ADDR, map[0].sram_addr, NVT_ILM_LENGTH_ADDR,
		    map[0].size, NVT_G_ILM_CHECKSUM_ADDR, map[0].crc);
		nvt_set_bld_crc_bank(NVT_DLM_DES_ADDR, map[1].sram_addr, NVT_DLM_LENGTH_ADDR,
		    map[1].size, NVT_G_DLM_CHECKSUM_ADDR, map[1].crc);
		nvt_bld_crc_enable();
		nvt_fw_crc_enable();
		nvt_boot_ready();
		if (nvt_check_fw_reset_state(NVT_RESET_STATE_INIT, 50)) {
			uint8_t info[18];

			/* baseline ReK; fingers stay empty until this finishes */
			(void)nvt_check_fw_reset_state(0xA3, 50);
			nvt_set_page(nvt_event_buf | NVT_EVENT_MAP_FWINFO);
			memset(info, 0, sizeof(info));
			info[0] = NVT_EVENT_MAP_FWINFO;
			if (nvt_read(info, 17) && (uint8_t)(info[1] + info[2]) == 0xff) {
				uint32_t abs_x = ((uint32_t)info[5] << 8) | info[6];
				uint32_t abs_y = ((uint32_t)info[7] << 8) | info[8];

				printf("citrus-touch: Novatek FW ver %#x channels %u x %u abs %u x %u\n",
				    info[1], info[3], info[4], abs_x, abs_y);
				if (abs_x >= 320 && abs_x <= 4096) {
					touch_max_x = abs_x;
				}
				if (abs_y >= 320 && abs_y <= 4096) {
					touch_max_y = abs_y;
				}
			} else {
				printf("citrus-touch: Novatek FW info %#x %#x %#x %#x %#x\n",
				    info[1], info[2], info[3], info[4], info[5]);
			}
			/* keep 8 MHz for reports — 1 MHz made each 66-byte read ~1.5 ms */
			geni_spi_hz(8000000);
			geni_spi_mode(0);
			nvt_set_page(nvt_event_buf);
			return true;
		}
		printf("citrus-touch: Novatek %s download attempt %d did not reach reset %#x\n",
		    name, attempt + 1, NVT_RESET_STATE_INIT);
	}
	return false;
}

static bool
novatek_probe(bool verbose)
{
	uint8_t id[7];
	uint32_t addrs[] = { NVT_CHIP_VER_TRIM, NVT_CHIP_VER_TRIM_OLD };
	bool any = false;

	printf("citrus-touch: trying Novatek (SPI mode 0, CS0)\n");
	geni_spi_mode(0);
	/* eng reset while reset is low, then release — vendor nvt_ts_probe */
	gpio_output(PIN_RESET, false, PULL_NONE, 8);
	nvt_write_addr(NVT_ENG_RST_ADDR, 0x5A);
	sleep_ms(1);
	gpio_set(PIN_RESET, true);
	sleep_ms(10);

	for (unsigned a = 0; a < sizeof(addrs) / sizeof(addrs[0]); a++) {
		for (int retry = 0; retry < 5; retry++) {
			nvt_bootloader_reset();
			if (!nvt_set_page(addrs[a])) {
				continue;
			}
			memset(id, 0, sizeof(id));
			id[0] = (uint8_t)(addrs[a] & 0x7f);
			if (!nvt_read(id, 7)) {
				continue;
			}
			if (verbose) {
				printf("citrus-touch: Novatek trim %#x: %02x %02x %02x %02x %02x %02x\n",
				    addrs[a], id[1], id[2], id[3], id[4], id[5], id[6]);
			}
			/* trim table: mask {1,0,0,1,1,1} for 0A/0B… 72 66 03 → NT36672A */
			if (id[1] == 0x0a || id[1] == 0x0b || id[1] == 0x0c || id[1] == 0x0e) {
				if (id[4] == 0x72 && (id[5] == 0x66 || id[5] == 0x65) && id[6] == 0x03) {
					nvt_event_buf = NVT_EVENT_BUF_ADDR;
					printf("citrus-touch: Novatek NT36672x at %#x (event buf %#x)\n",
					    addrs[a], nvt_event_buf);
					/* vendor BOOT_UPDATE: SRAM host-download, not NOR flash boot.
					 * Keep SPI mode 0 (geni_spi_init would force mode 1 and kill writes).
					 * Bump clock only via the divider — never re-init the engine. */
					geni_spi_hz(8000000);
					geni_spi_mode(0);
					printf("citrus-touch: Novatek SPI mode %u for host-download\n",
					    (unsigned)(rd(SPI_CPHA) | (rd(SPI_CPOL) << 1)));
					if (nvt_host_download(citrus_nvt_fw_tianma, CITRUS_NVT_FW_TIANMA_LENGTH,
					    "tianma") ||
					    nvt_host_download(citrus_nvt_fw_truly, CITRUS_NVT_FW_TRULY_LENGTH,
					    "truly")) {
						return true;
					}
					printf("citrus-touch: Novatek host-download failed\n");
					return false;
				}
			}
			if (!((id[1] == 0xff && id[2] == 0xff && id[3] == 0xff) ||
			    (id[1] == 0x00 && id[2] == 0x00 && id[3] == 0x00))) {
				any = true;
				printf("citrus-touch: Novatek-like ID at %#x (not in the NT36672 list)\n", addrs[a]);
			}
			sleep_ms(10);
		}
	}
	if (verbose && !any) {
		printf("citrus-touch: no Novatek chip ID either\n");
	}
	geni_spi_mode(1);               /* leave the bus as FocalTech found it */
	return false;
}

/* ---- touch reports ------------------------------------------------------------------------------- */

static struct {
	bool down, seen;
	uint32_t x, y;
} fingers[FTS_MAX_TOUCHES];

static uint32_t touch_flip;
static unsigned shown;

struct report_stats {
	unsigned polls, reports, bad, points;
	uint32_t last_x, last_y;
} stats;

static void
finger_update(uint32_t id, uint32_t event, uint32_t x, uint32_t y)
{
	if (id >= FTS_MAX_TOUCHES || touch_max_x == 0 || touch_max_y == 0) {
		return;
	}
	if (event == FTS_EVENT_UP) {
		fingers[id].seen = true;
		if (fingers[id].down) {
			fingers[id].down = false;
			redwood_touch_post(REDWOOD_TOUCH_UP, id, fingers[id].x, fingers[id].y);
		}
		return;
	}
	switch (touch_flip) {
	case 1: x = touch_max_x - 1 - x; break;
	case 2: y = touch_max_y - 1 - y; break;
	case 3: x = touch_max_x - 1 - x; y = touch_max_y - 1 - y; break;
	}
	x = (uint32_t)(((uint64_t)x * 65535) / (touch_max_x - 1));
	y = (uint32_t)(((uint64_t)y * 65535) / (touch_max_y - 1));
	fingers[id].seen = true;
	stats.points++;
	stats.last_x = x;
	stats.last_y = y;
	if (!fingers[id].down) {
		fingers[id].down = true;
		redwood_touch_post(REDWOOD_TOUCH_DOWN, id, x, y);
	} else if (fingers[id].x != x || fingers[id].y != y) {
		redwood_touch_post(REDWOOD_TOUCH_MOVE, id, x, y);
	}
	fingers[id].x = x;
	fingers[id].y = y;
	if (shown++ < 12) {
		printf("citrus-touch: finger %u %s at %u, %u\n", id,
		    event == FTS_EVENT_DOWN ? "down" : "move", x, y);
	}
}

static void
parse_report(const uint8_t *report)
{
	uint32_t count = report[1] & 0x0f;

	if (count > FTS_MAX_TOUCHES) {
		count = FTS_MAX_TOUCHES;
	}
	for (uint32_t i = 0; i < FTS_MAX_TOUCHES; i++) {
		fingers[i].seen = false;
	}
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *point = report + 2 + FTS_POINT_LENGTH * i;
		uint32_t event = point[0] >> 6;
		uint32_t id = point[2] >> 4;
		uint32_t x = ((uint32_t)(point[0] & 0x0f) << 8) | point[1];
		uint32_t y = ((uint32_t)(point[2] & 0x0f) << 8) | point[3];

		if (event == FTS_EVENT_DOWN || event == FTS_EVENT_CONTACT || event == FTS_EVENT_UP) {
			finger_update(id, event, x, y);
		}
	}
	for (uint32_t i = 0; i < FTS_MAX_TOUCHES; i++) {
		if (!fingers[i].seen && fingers[i].down) {
			fingers[i].down = false;
			redwood_touch_post(REDWOOD_TOUCH_UP, i, fingers[i].x, fingers[i].y);
		}
	}
}

static bool nvt_page_set;

static bool
nvt_read_points(uint8_t *point_data)
{
	uint8_t buf[NVT_POINT_DATA_LEN + 2];

	/* vendor re-selects the event page around each report read */
	if (!nvt_set_page(nvt_event_buf)) {
		return false;
	}
	nvt_page_set = true;
	memset(buf, 0, sizeof(buf));
	buf[0] = 0;
	if (!nvt_read(buf, NVT_POINT_DATA_LEN + 1)) {
		nvt_page_set = false;
		return false;
	}
	memcpy(point_data, buf, NVT_POINT_DATA_LEN + 1);
	return true;
}

static void
nvt_parse_points(const uint8_t *point_data)
{
	bool seen[NVT_MAX_TOUCHES] = { false };

	for (uint32_t i = 0; i < NVT_MAX_TOUCHES; i++) {
		uint32_t position = 1 + 6 * i;
		uint32_t input_id = point_data[position] >> 3;
		uint32_t state = point_data[position] & 0x07;
		uint32_t x, y;

		if (input_id == 0 || input_id > NVT_MAX_TOUCHES) {
			continue;
		}
		input_id -= 1;
		if (state != 0x01 && state != 0x02) {
			continue;
		}
		x = ((uint32_t)point_data[position + 1] << 4) | (point_data[position + 3] >> 4);
		y = ((uint32_t)point_data[position + 2] << 4) | (point_data[position + 3] & 0x0f);
		if (x >= touch_max_x || y >= touch_max_y) {
			continue;
		}
		seen[input_id] = true;
		finger_update(input_id, fingers[input_id].down ? FTS_EVENT_CONTACT : FTS_EVENT_DOWN, x, y);
	}
	for (uint32_t i = 0; i < NVT_MAX_TOUCHES; i++) {
		if (!seen[i] && fingers[i].down) {
			finger_update(i, FTS_EVENT_UP, fingers[i].x, fingers[i].y);
		}
	}
}

static bool
touch_report(uint8_t *report)
{
	if (chip == CHIP_NOVATEK) {
		return nvt_read_points(report);
	}
	if (report_kind != REPORT_BARE) {
		if (fts_read(FTS_REG_TOUCH, report, FTS_REPORT_LENGTH) && (report[0] & 0xf0) == 0x90) {
			if (report_kind == REPORT_UNKNOWN) {
				printf("citrus-touch: the report answers a read of register 0x01\n");
			}
			report_kind = REPORT_COMMAND;
			return true;
		}
		if (report_kind == REPORT_COMMAND) {
			return false;
		}
	}
	if (fts_read_data(report, FTS_REPORT_LENGTH) && (report[0] & 0xf0) == 0x90) {
		if (report_kind == REPORT_UNKNOWN) {
			printf("citrus-touch: the report is read with a data package alone, as the vendor driver does\n");
		}
		report_kind = REPORT_BARE;
		return true;
	}
	return false;
}

/*
 * The GCC registers can be read at any time, but the serial engine's registers at 0x4a88000
 * hang the bus (and the watchdog then resets the SoC without a word) while its clocks are off.
 * So the state is printed in two parts, and the engine's part only after clocks_on().
 */
static void
show_gcc(const char *when)
{
	printf("citrus-touch: %s: vote %#x; m %#x s %#x core %#x core2x %#x se2 %#x; clock cfg %#x cmd %#x\n", when,
	    rd(GCC_VOTE), rd(GCC_M_AHB_CBCR), rd(GCC_S_AHB_CBCR), rd(GCC_CORE_CBCR), rd(GCC_CORE_2X_CBCR),
	    rd(GCC_SE2_CBCR), rd(GCC_SE2_CFG), rd(GCC_SE2_CMD));
}

static uint32_t pause_ms;

static void
pause_before(const char *what)
{
	if (pause_ms) {
		printf("citrus-touch: %u ms, then %s\n", pause_ms, what);
		sleep_ms(pause_ms);
	}
}

static void
show_engine_regs(const char *when)
{
	printf("citrus-touch: %s:\n", when);
	pause_before("read the engine's firmware revision");
	printf("citrus-touch: engine firmware revision %#x\n", rd(GENI_FW_REVISION_RO));
	pause_before("read the engine's interface flag");
	printf("citrus-touch: engine interface flag %#x\n", rd(GENI_IF_DISABLE_RO));
	pause_before("read the engine's parameters");
	printf("citrus-touch: engine parameters %#x\n", rd(SE_HW_PARAM_0));
}

/*
 * TLMM reads can hang the NoC while the serial engine's pins are still in their
 * power-on state; only look at them after use_engine_pins() has programmed them.
 */
static void
show_gpio_state(const char *when)
{
	printf("citrus-touch: %s (gpio):\n", when);
	pause_before("read gpio 6 (west)");
	printf("citrus-touch: gpio 6 cfg %#x\n", rd(GPIO_CFG(PIN_SPI_0)));
	pause_before("read gpio 7 (west)");
	printf("citrus-touch: gpio 7 cfg %#x\n", rd(GPIO_CFG(PIN_SPI_1)));
	pause_before("read gpio 71/80 (west)");
	printf("citrus-touch: gpio 71 cfg %#x, cs %#x\n", rd(GPIO_CFG(PIN_SPI_2)), rd(GPIO_CFG(PIN_CS)));
	pause_before("read gpio 93/103");
	printf("citrus-touch: interrupt cfg %#x io %u; reset cfg %#x io %u\n", rd(GPIO_CFG(PIN_INT)),
	    rd(GPIO_IO(PIN_INT)), rd(GPIO_CFG(PIN_RESET)), rd(GPIO_IO(PIN_RESET)));
}

static void
use_engine_pins(void)
{
	/* DT qupv3_se2_spi_active: gpio6/7/71 pull-down; CS is gpio (see gpio_cs) */
	gpio_configure(PIN_SPI_0, PIN_FUNCTION_QUP2, PULL_DOWN, 6, false);
	gpio_configure(PIN_SPI_1, PIN_FUNCTION_QUP2, PULL_DOWN, 6, false);
	gpio_configure(PIN_SPI_2, PIN_FUNCTION_QUP2, PULL_DOWN, 6, false);
	if (gpio_cs) {
		gpio_output(PIN_CS, true, PULL_UP, 6);          /* idle high; we toggle it */
	} else {
		gpio_configure(PIN_CS, PIN_FUNCTION_QUP2, PULL_UP, 6, false);
	}
	gpio_configure(PIN_INT, 0, PULL_UP, 8, false);         /* an input; the chip pulls it low for a report */
}

static bool
bring_up(void)
{
	const char *problem;
	uint32_t spi_hz = 400000;

	show_gcc("as found");
	if (steps < 1) {
		printf("citrus-touch: stopping after the clock controller's state (steps %u)\n", steps);
		return false;
	}
	printf("citrus-touch: step 1: turn on the serial engine's clocks\n");
	problem = clocks_on() ? NULL : "its clocks cannot be turned on";
	if (problem == NULL) {
		if (steps < 2) {
			printf("citrus-touch: stopping after the clocks (steps %u)\n", steps);
			return false;
		}
		sleep_ms(2);
		printf("citrus-touch: step 2: read the serial engine\n");
		show_engine_regs("clocks on");
		if (steps < 3) {
			printf("citrus-touch: stopping after the engine's registers (steps %u)\n", steps);
			return false;
		}
		printf("citrus-touch: serial engine 2: firmware %#x, interface %#x, parameters %#x, crystal %u Hz\n",
		    rd(GENI_FW_REVISION_RO), rd(GENI_IF_DISABLE_RO), rd(SE_HW_PARAM_0), se_source_hz);
		PE_parse_boot_argn("touch_spi_hz", &spi_hz, sizeof(spi_hz));
		if (spi_hz < 100000 || spi_hz > 20000000) {
			spi_hz = 400000;
		}
	{
		uint32_t hw_cs = 0;

		if (PE_parse_boot_argn("citrus_touch_hw_cs", &hw_cs, sizeof(hw_cs)) && hw_cs) {
			gpio_cs = false;
		}
	}
	printf("citrus-touch: the bus at %u Hz, %s packages, %s chip select\n", spi_hz,
	    spi_split ? "split" : "single", gpio_cs ? "GPIO" : "QUP");
	printf("citrus-touch: step 3: set the engine up\n");
	problem = geni_spi_init(spi_hz);
	}
	if (problem != NULL) {
		printf("citrus-touch: %s\n", problem);
		return false;
	}
	if (steps < 4) {
		printf("citrus-touch: stopping after the engine's setup (steps %u)\n", steps);
		return false;
	}
	use_engine_pins();
	show_gpio_state("pins set");
	touch_reset();
	printf("citrus-touch: reset released; interrupt line %s\n", gpio_get(PIN_INT) ? "high" : "LOW");
	if (steps < 5) {
		printf("citrus-touch: stopping after the chip's reset (steps %u)\n", steps);
		return false;
	}
	if (focaltech_probe(true)) {
		chip = CHIP_FOCALTECH;
		return true;
	}
	/*
	 * Same SPI2 / CS0 / reset / IRQ serve either FocalTech or Novatek
	 * (panel BOM). Novatek's driver forces chip_select=0 and SPI_MODE_0.
	 */
	if (novatek_probe(true)) {
		chip = CHIP_NOVATEK;
		return true;
	}
	show_engine_regs("no answer");
	show_gpio_state("no answer");
	return false;
}

static uint64_t
micros_now(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

static void
touch_thread(__unused void *parameter, __unused wait_result_t result)
{
	/* Novatek INT often stays low; sample often enough that a tap is not a 250 ms lottery */
	uint32_t poll_us = 2000, latency = 0, reads = 0;
	uint64_t reading = 0;

	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	PE_parse_boot_argn("touch_lat", &latency, sizeof(latency));
	PE_parse_boot_argn("touch_flip", &touch_flip, sizeof(touch_flip));
	PE_parse_boot_argn("citrus_touch_pause_ms", &pause_ms, sizeof(pause_ms));
	{
		uint32_t value = 0;

		if (PE_parse_boot_argn("citrus_touch_max_x", &value, sizeof(value)) && value > 1) {
			touch_max_x = value;
		}
		value = 0;
		if (PE_parse_boot_argn("citrus_touch_max_y", &value, sizeof(value)) && value > 1) {
			touch_max_y = value;
		}
	}
	if (poll_us < 250 || poll_us > 100000) {
		poll_us = 2000;
	}

	tlmm_west = ml_io_map(TLMM_WEST_PHYS, TLMM_WEST_MAP_SIZE);
	tlmm_south = ml_io_map(TLMM_SOUTH_PHYS + TLMM_SOUTH_PIN_OFF, TLMM_SOUTH_MAP_SIZE);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	se = ml_io_map(SE_PHYS, SE_SIZE);
	if (tlmm_west == 0 || tlmm_south == 0 || gcc == 0 || se == 0) {
		printf("citrus-touch: ml_io_map failed (west %#lx south %#lx gcc %#lx se %#lx)\n",
		    tlmm_west, tlmm_south, gcc, se);
		thread_terminate(current_thread());
		return;
	}
	printf("citrus-touch: mapped west %#lx south %#lx, clocks %#lx, engine %#lx (steps %u)\n",
	    tlmm_west, tlmm_south, gcc, se, steps);
	printf("citrus-touch: gpio6/7/71/80/93 west, gpio103 south; cfg 6=%#lx 71=%#lx 103=%#lx\n",
	    GPIO_CFG(PIN_SPI_0), GPIO_CFG(PIN_SPI_2), GPIO_CFG(PIN_RESET));

	if (!bring_up()) {
		printf("citrus-touch: no FocalTech touch screen\n");
		thread_terminate(current_thread());
		return;
	}
	printf("citrus-touch: %s ready; the interrupt line is looked at every %u us; panel %u x %u\n",
	    chip == CHIP_NOVATEK ? "Novatek" : "FT8719", poll_us, touch_max_x, touch_max_y);
	{
		unsigned fail_streak = 0;

		/*
		 * Both chips hold INT low while a report is waiting. Reading clears it (Novatek's
		 * Linux driver uses EDGE_RISING because the line rises again after the read).
		 * On this panel INT often stays low forever, so a "peek every 32 ticks" throttle
		 * made glass touch sample at ~4 Hz. Drain every poll; only back off when SPI
		 * keeps failing (that path used to wedge the serial engine / watchdog).
		 */
		for (unsigned tick = 1;; tick++) {
			uint64_t looked = micros_now(), read_at;
			uint8_t report[NVT_POINT_DATA_LEN + 2];
			unsigned int_now = gpio_get(PIN_INT);
			bool do_read = false;

			stats.polls++;
			if (!int_now) {
				if (fail_streak < 8 || (tick % 32) == 0) {
					do_read = true;
				}
			} else {
				fail_streak = 0;
			}
			if (do_read) {
				if (touch_report(report)) {
					fail_streak = 0;
					stats.reports++;
					if (chip == CHIP_NOVATEK) {
						if (stats.reports <= 8 || (stats.reports % 200) == 0) {
							printf("citrus-touch: Novatek report %u:"
							    " %02x %02x %02x %02x %02x %02x %02x %02x"
							    " %02x %02x %02x %02x %02x %02x %02x %02x\n",
							    stats.reports,
							    report[0], report[1], report[2], report[3],
							    report[4], report[5], report[6], report[7],
							    report[8], report[9], report[10], report[11],
							    report[12], report[13], report[14], report[15]);
						}
						nvt_parse_points(report);
					} else {
						parse_report(report);
					}
				} else {
					fail_streak++;
					stats.bad++;
				}
				read_at = micros_now();
				reads++;
				reading += read_at - looked;
				if (latency) {
					redwood_tl("tl: seen %llu read %llu\n", looked, read_at);
				}
			}
			if (tick % (60 * (1000000 / poll_us)) == 0) {
				printf("citrus-touch: so far %u reports in %u looks (%u bad), %u finger positions, the last at %u, %u; "
				    "interrupt line %s; a report takes %llu us to read; the line looked at every %u us\n",
				    stats.reports, stats.polls, stats.bad, stats.points, stats.last_x, stats.last_y,
				    int_now ? "high" : "low", reads ? reading / reads : 0, poll_us);
			}
			sleep_us(poll_us);
		}
	}
}

void
citrus_touch_init(void)
{
	uint32_t enabled = 0, simple = 0, value = 5;
	thread_t thread;

	if (!PE_parse_boot_argn("citrus_touch", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	if (PE_parse_boot_argn("citrus_touch_steps", &value, sizeof(value))) {
		steps = value;
	}
	if (PE_parse_boot_argn("citrus_touch_simple", &simple, sizeof(simple)) && simple) {
		spi_split = false;
	}
	if (kernel_thread_start(touch_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
