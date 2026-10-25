/*
 * Adreno 610 firmware helpers (from MakrSas/ios-topaz, adapted for HackiOS citrus).
 * Reads zap/SQE from the boot image fw/ first (cold boot), then /usr/share/firmware/citrus/.
 */
#if defined(HACKIOS_ADRENO_610)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/kauth.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <stdbool.h>

extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

int topaz_fw_read(const char *path, void **data, size_t *size);
void topaz_fw_free(void *data, size_t size);
int topaz_root_mounted(void);

static const char *
fw_basename(const char *path)
{
	const char *slash = path;

	while (slash != NULL && *slash != '\0') {
		const char *next = strchr(slash, '/');
		if (next == NULL) {
			return slash;
		}
		slash = next + 1;
	}
	return path;
}

bool
hackios_gpu_is_adreno610(void)
{
	uint64_t size;

	if (redwood_image_file("fw", "a660_gmu.bin", &size, NULL) != NULL) {
		return false;
	}
	return redwood_image_file("fw", "a610_zap.mdt", &size, NULL) != NULL;
}

int
topaz_root_mounted(void)
{
	return rootvnode != NULLVP;
}

int
topaz_fw_read(const char *path, void **data, size_t *size)
{
	const char *name = fw_basename(path);
	uint64_t img_size = 0;
	const uint8_t *img;
	void *buf;

	*data = NULL;
	*size = 0;
	img = redwood_image_file("fw", name, &img_size, NULL);
	if (img != NULL && img_size > 0 && img_size <= (16 << 20)) {
		buf = kalloc_data((vm_size_t)img_size, Z_WAITOK);
		if (buf == NULL) {
			return ENOMEM;
		}
		memcpy(buf, img, (size_t)img_size);
		*data = buf;
		*size = (size_t)img_size;
		return 0;
	}
	if (rootvnode == NULLVP) {
		return ENOENT;
	}
	{
		vfs_context_t ctx = vfs_context_kernel();
		vnode_t vp = NULLVP;
		off_t len = 0;
		int resid = 0, err;

		err = vnode_open(path, FREAD, 0, 0, &vp, ctx);
		if (err) {
			return err;
		}
		err = vnode_size(vp, &len, ctx);
		if (!err && (len <= 0 || len > (16 << 20))) {
			err = EFBIG;
		}
		if (!err) {
			buf = kalloc_data((vm_size_t)len, Z_WAITOK | Z_ZERO);
			if (buf == NULL) {
				err = ENOMEM;
			} else {
				err = vn_rdwr(UIO_READ, vp, (caddr_t)buf, (int)len, 0, UIO_SYSSPACE, IO_NODELOCKED,
				    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
				if (!err && resid != 0) {
					err = EIO;
				}
				if (err) {
					kfree_data(buf, (vm_size_t)len);
				} else {
					*data = buf;
					*size = (size_t)len;
				}
			}
		}
		vnode_close(vp, FREAD, ctx);
		return err;
	}
}

void
topaz_fw_free(void *data, size_t size)
{
	if (data != NULL) {
		kfree_data(data, size);
	}
}

void
redwood_adreno_stop(void)
{
	extern void RedwoodGPUStop(void);

	RedwoodGPUStop();
}

#else

void
topaz_adreno_init(void)
{
}

#endif /* HACKIOS_ADRENO_610 */
