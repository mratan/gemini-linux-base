// ispcap — turn on the MT6797 vendor ISP driver's own register-write trace.
//
// #66. The vendor kernel writes only 16 of pass1's 555 registers itself; every
// tuning value the camera HAL uses arrives through ISP_WriteRegToHw() as a
// (module, offset, value) triple. That function already has a log line, gated
// on IspInfo.DebugMask & ISP_DBG_WRITE_REG, and the mask is settable from
// userspace. So the vendor's entire ISP programme can be recorded without
// patching anything and without /dev/mem — which matters, because the Android
// side has CONFIG_DEVMEM=n.
//
// Run on the VENDOR stack (boot1 Android), as root, before starting the camera:
//
//     ./ispcap 0x8        # ISP_DBG_WRITE_REG
//     <start the camera>
//     dmesg
//
//     ./ispcap -d         # ISP_DUMP_REG: dump the register state as it stands
//
// Definitions from the vendor driver, gemian-3.18:
//   drivers/misc/mediatek/cameraisp/src/mt6797/inc/camera_isp.h
//     ISP_MAGIC 'k'; ISP_DEV_NAME "camera-isp"
//     ISP_DUMP_REG   _IO (ISP_MAGIC, ISP_CMD_DUMP_REG)
//     ISP_DEBUG_FLAG _IOW(ISP_MAGIC, ISP_CMD_DEBUG_FLAG, unsigned char *)
// ISP_CMD_DUMP_REG and ISP_CMD_DEBUG_FLAG are ENUM POSITIONS, counted from the
// top of the ioctl enum in that header: 9 and 12 respectively (also
// ISP_CMD_READ_REG = 5, ISP_CMD_WRITE_REG = 6, which are the other half of the
// story if a replay ever needs to be driven from userspace).
//
// They are NOT stable across MediaTek releases, and the gemian-3.18 tree is a
// CROSS-CHECK for the stock vendor kernel boot1 actually runs, not the same
// build. So both are printed and overridable by environment variable rather
// than baked in. If the ioctl returns EINVAL/ENOTTY, recount them against the
// header for the running kernel before assuming the driver is uncooperative.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define ISP_MAGIC 'k'

int main(int argc, char **argv)
{
	const char *dev = getenv("ISPDEV") ? getenv("ISPDEV") : "/dev/camera-isp";
	unsigned cmd_dbg  = getenv("CMD_DEBUG_FLAG") ? strtoul(getenv("CMD_DEBUG_FLAG"), 0, 0) : 12;
	unsigned cmd_dump = getenv("CMD_DUMP_REG")   ? strtoul(getenv("CMD_DUMP_REG"), 0, 0)   : 9;
	int fd, ret;

	if (argc < 2) {
		fprintf(stderr,
		    "usage: %s <mask>   set IspInfo.DebugMask (0x8 = ISP_DBG_WRITE_REG)\n"
		    "       %s -d       ISP_DUMP_REG\n"
		    "env: ISPDEV, CMD_DEBUG_FLAG, CMD_DUMP_REG (enum positions; verify!)\n",
		    argv[0], argv[0]);
		return 2;
	}

	fd = open(dev, O_RDWR);
	if (fd < 0) { perror(dev); return 1; }

	if (!strcmp(argv[1], "-d")) {
		unsigned long req = _IO(ISP_MAGIC, cmd_dump);
		printf("ISP_DUMP_REG: ioctl 0x%lx on %s\n", req, dev);
		ret = ioctl(fd, req, 0);
	} else {
		unsigned int mask = strtoul(argv[1], 0, 0);
		unsigned long req = _IOW(ISP_MAGIC, cmd_dbg, unsigned char *);
		printf("ISP_DEBUG_FLAG: ioctl 0x%lx on %s, mask 0x%x\n", req, dev, mask);
		ret = ioctl(fd, req, &mask);
	}

	if (ret < 0) perror("ioctl");
	else printf("ok\n");
	close(fd);
	return ret < 0;
}
