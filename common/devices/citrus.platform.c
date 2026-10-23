/*
 * citrus: paste into xnuloader/loader.c next to platform_redwood and add &platform_citrus to platforms[].
 * Every value is explained in docs/PORTING.md ("The loader").
 */
static const struct platform platform_citrus = {
	.name = "citrus",
	.io_base = 0x00400000, .io_size = 0x3FC00000,   /* TODO: one window over the SoC's peripherals, non-zero base */
	.gic_dist = 0xf200000, .gic_redist = 0xf300000,
	.uart = 0,                                      /* phones: no PL011 */
	.log_other_cpu = 1,
	.cores = 8, .core_shift = 0,                    /* plat_core_mpidr: 0..3 silver, 0x100..0x103 gold */
	.small_cores = 0xf,                             /* cpu@0..3 (capacity-dmips-mhz 0x400) = Kryo 260 Silver */
	.fb_base = 0x5C000000,                          /* Display Reserved; citrus.conf FB_BASE */
};
