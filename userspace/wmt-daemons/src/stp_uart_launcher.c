/* SPDX-License-Identifier: GPL-2.0
 *
 * wmt_launcher (stp_uart_launcher lineage) — MediaTek WMT userspace agent,
 * native Debian 13 port for the Gemini PDA (MT6797, SoC/BTIF flow).
 *
 * Lineage: BPI-SINOVOIP/BPI-R2-bsp-4.14
 * linux-mt/utils/wmt/src/stp_uart_launcher.c (verbatim copy in
 * upstream/src_stp_uart_launcher.c; see PROVENANCE.md). Every deviation is
 * marked GEMINI-PORT and logged in README.md ("Adaptation log").
 *
 * Role: opens /dev/stpwmt, tells the kernel WMT core the STP mode, then
 * services kernel upcalls (read/poll on /dev/stpwmt): on "srh_patch" it
 * scans the ROM-patch directory (-p) and hands matching patch names +
 * header info to the kernel via WMT_IOCTL_SET_PATCH_NUM/SET_PATCH_INFO;
 * the kernel fetches the files itself via request_firmware(). The ioctl
 * interface authority is modules/connectivity/common_main/linux/wmt_dev.c
 * in the gemini-linux-base tree.
 */

#define STATIC_BUILD 1

#include "wmt_ioctl.h"
#include "wmt_patch.h"	/* GEMINI-PORT (A3): extracted srh_patch scan */
#include "wmt_cfg.h"	/* GEMINI-PORT (A5): WMT_SOC.cfg validation */
#include "wmt_dev_wait.h" /* GEMINI-PORT (A1): bounded device waits */
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <termios.h>
#include <time.h>
#include <sys/time.h>
#include <sys/poll.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <linux/serial.h> /* struct serial_struct  */

/* For directory operation */
#include <dirent.h>

#define ALOGI  printf
#define ALOGE  printf

/******************************************************************************
*                              C O N S T A N T S
*******************************************************************************
*/

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "wmt_launcher"

#ifndef N_MTKSTP
#define N_MTKSTP    (15 + 1)  /* MediaTek WCN Serial Transport Protocol */
#endif

#define HCIUARTSETPROTO        _IOW('U', 200, int)

#define CUST_COMBO_WMT_DEV "/dev/stpwmt"
#define CUST_COMBO_STP_DEV "/dev/ttyMT2"
/* GEMINI-PORT (A2): default patch dir is the Debian firmware location the
 * Slice 2 payload is staged into (Android/BPI used /etc/firmware or
 * /vendor/firmware). The systemd unit passes -p /lib/firmware explicitly. */
#define CUST_COMBO_PATCH_PATH "/lib/firmware"
/* GEMINI-PORT (A5): name of the WMT config in the patch dir; the kernel
 * loads the same file by bare name via request_firmware(). */
#define CUST_CFG_WMT_SOC_NAME "WMT_SOC.cfg"

#define CUST_BAUDRATE_DFT (115200)

#define CUST_MULTI_PATCH (1)

/* GEMINI-PORT (A1): upstream waited forever for /dev/stpwmt and the chip id */
#define WMT_LAUNCHER_DEFAULT_WAIT_SEC 10

typedef enum {
    STP_MIN = 0x0,
    STP_UART_FULL = 0x1,
    STP_UART_MAND = 0x2,
    STP_BTIF_FULL = 0x3,
    STP_SDIO = 0x4,
    STP_MAX = 0x5,
} STP_MODE;

#define MAX_CMD_LEN (NAME_MAX+1)

typedef enum {
    UART_DISABLE_FC = 0, /*NO flow control*/
    UART_MTK_SW_FC = 1,  /*MTK SW Flow Control, differs from Linux Flow Control*/
    UART_LINUX_FC = 2,   /*Linux SW Flow Control*/
    UART_HW_FC = 3,      /*HW Flow Control*/
} STP_UART_FC;

typedef struct {
    STP_UART_FC fc;
    int parity;
    int stop_bit;
} STP_UART_CONFIG;

typedef struct {
    STP_MODE eStpMode;
    char *pPatchPath;
    char *pPatchName;
    char *gStpDev;
    int iBaudrate;
    STP_UART_CONFIG sUartConfig;
} STP_PARAMS_CONFIG, *P_STP_PARAMS_CONFIG;

#if CUST_MULTI_PATCH
/* Layout must match WMT_PATCH_INFO in the kernel's wmt_lib.h:
 * { UINT32 dowloadSeq; UINT8 addRess[4]; UINT8 patchName[256]; } */
typedef struct {
    int dowloadSeq;
    char addRess[4];
    char patchName[256];
} STP_PATCH_INFO, *P_STP_PATCH_INFO;
#endif

typedef struct {
    const char *pCfgItem;
    char cfgItemValue[NAME_MAX + 1];
} CHIP_ANT_MODE_INFO, *P_CHIP_ANT_MODE_INFO;

typedef struct {
    int chipId;
    STP_MODE stpMode;
    CHIP_ANT_MODE_INFO antMode;
} CHIP_MODE_INFO, *P_CHIP_MODE_INFO;

CHIP_MODE_INFO gChipModeInfo[] = {
    {0x6620, STP_UART_FULL, {"mt6620.defAnt", {"mt6620_ant_m3.cfg"}}},
    {0x6628, STP_UART_FULL, {"mt6628.defAnt", {"mt6628_ant_m1.cfg"}}},
    {0x6630, STP_UART_FULL, {"mt6630.defAnt", {"mt6630_ant_m1.cfg"}}},
};

/******************************************************************************
*                             D A T A   T Y P E S
*******************************************************************************
*/
struct cmd_hdr {
    const char *pCmd;
    int (*hdr_func)(P_STP_PARAMS_CONFIG pStpParamsConfig);
};

struct speed_map {
    unsigned int baud;
    speed_t      speed;
};

/******************************************************************************
*                   F U N C T I O N   D E C L A R A T I O N S
*******************************************************************************
*/
static int set_speed(int fd, struct termios *ti, int speed);
int setup_uart_param(int hComPort, int iBaudrate, STP_UART_CONFIG *stp_uart);

int cmd_hdr_baud_115k(P_STP_PARAMS_CONFIG pStpParamsConfig);
int cmd_hdr_baud_921k(P_STP_PARAMS_CONFIG pStpParamsConfig);
int cmd_hdr_baud_2kk(P_STP_PARAMS_CONFIG pStpParamsConfig);
int cmd_hdr_baud_2_5kk(P_STP_PARAMS_CONFIG pStpParamsConfig);
int cmd_hdr_baud_3kk(P_STP_PARAMS_CONFIG pStpParamsConfig);
int cmd_hdr_baud_3_5kk(P_STP_PARAMS_CONFIG pStpParamsConfig);
int cmd_hdr_baud_4kk(P_STP_PARAMS_CONFIG pStpParamsConfig);
int cmd_hdr_stp_open(P_STP_PARAMS_CONFIG pStpParamsConfig);
int cmd_hdr_stp_close(P_STP_PARAMS_CONFIG pStpParamsConfig);
int cmd_hdr_stp_rst(P_STP_PARAMS_CONFIG pStpParamsConfig);
int cmd_hdr_sch_patch(P_STP_PARAMS_CONFIG pStpParamsConfig);
static int setHifInfo(int chipId, char *cfgFilePath);
static int wmt_cfg_item_parser(char *pItem);
static int get_wmt_cfg(int chipId);
static speed_t get_speed(int baudrate);

/******************************************************************************
*                           P R I V A T E   D A T A
*******************************************************************************
*/
static struct speed_map speeds[] = {
    {115200,    B115200},
    {921600,    B921600},
    {1000000,   B1000000},
    {1152000,   B1152000},
    {2000000,   B2000000},
    {2500000,   B2500000},
    {3000000,   B3000000},
    {3500000,   B3500000},
    {4000000,   B4000000},
};

struct cmd_hdr cmd_hdr_table[] = {
    { "baud_115200_0", cmd_hdr_baud_115k},
    { "baud_921600_0", cmd_hdr_baud_921k},
    { "baud_2000000_0", cmd_hdr_baud_2kk},
    { "baud_2500000_0", cmd_hdr_baud_2_5kk},
    { "baud_3000000_0", cmd_hdr_baud_3kk},
    { "baud_3500000_0", cmd_hdr_baud_3_5kk},
    { "baud_4000000_0", cmd_hdr_baud_4kk},
    { "open_stp", cmd_hdr_stp_open},
    { "close_stp", cmd_hdr_stp_close},
    { "rst_stp", cmd_hdr_stp_rst},
    { "srh_patch", cmd_hdr_sch_patch},
};

static volatile sig_atomic_t __io_canceled = 0;
static char gPatchName[NAME_MAX+1] = {0};
static char gPatchFolder[NAME_MAX+1] = {0};
static char gStpDev[NAME_MAX+1] = {0};
static int gStpMode = -1;
static char gWmtCfgName[NAME_MAX+1] = {0};
static int gWmtFd = -1;
static int gTtyFd = -1;
static char gCmdStr[MAX_CMD_LEN] = {0};
static char gRespStr[MAX_CMD_LEN] = {0};
static int gFmMode = 2; /* 1: i2c, 2: comm I/F */
static const char *gUartName = NULL;

/******************************************************************************
*                              F U N C T I O N S
*******************************************************************************
*/

/* Used as host uart param setup callback */
int setup_uart_param(int hComPort, int iBaudrate, STP_UART_CONFIG *stpUartConfig)
{
    struct termios ti;
    int fd;

    if (!stpUartConfig) {
        ALOGE("Invalid stpUartConfig");
        return -2;
    }

    ALOGI("setup_uart_param %d %d\n", iBaudrate, stpUartConfig->fc);

    fd = hComPort;
    if (fd < 0) {
        ALOGE("Invalid serial port");
        return -2;
    }

    tcflush(fd, TCIOFLUSH);

    if (tcgetattr(fd, &ti) < 0) {
        ALOGE("Can't get port settings");
        return -3;
    }

    cfmakeraw(&ti);

    ti.c_cflag |= CLOCAL;

    if (stpUartConfig->fc == UART_DISABLE_FC) {
        ti.c_cflag &= ~CRTSCTS;
        ti.c_iflag &= ~(0x80000000);
    } else if (stpUartConfig->fc == UART_MTK_SW_FC) {
        ti.c_cflag &= ~CRTSCTS;
        ti.c_iflag |= 0x80000000; /*MTK Software FC*/
    } else if (stpUartConfig->fc == UART_HW_FC) {
        ti.c_cflag |= CRTSCTS;      /*RTS, CTS Enable*/
        ti.c_iflag &= ~(0x80000000);
    } else if (stpUartConfig->fc == UART_LINUX_FC) {
        ti.c_iflag |= (IXON | IXOFF | IXANY); /*Linux Software FC*/
        ti.c_cflag &= ~CRTSCTS;
        ti.c_iflag &= ~(0x80000000);
    } else {
        ti.c_cflag &= ~CRTSCTS;
        ti.c_iflag &= ~(0x80000000);
    }

    if (tcsetattr(fd, TCSANOW, &ti) < 0) {
        ALOGE("Can't set port settings");
        return -4;
    }

    /* Set baudrate */
    if (set_speed(fd, &ti, iBaudrate) < 0) {
        ALOGE("Can't set initial baud rate");
        return -5;
    }

    tcflush(fd, TCIOFLUSH);

    return 0;
}

static void sig_hup(int sig)
{
    (void)sig;
    fprintf(stderr, "sig_hup...\n");
}

static void sig_term(int sig)
{
    (void)sig;
    fprintf(stderr, "sig_term...\n");
    __io_canceled = 1;
    ioctl(gWmtFd, WMT_IOCTL_SET_LAUNCHER_KILL, 1);
}

static speed_t get_speed(int baudrate)
{
    unsigned int idx;

    for (idx = 0; idx < sizeof(speeds)/sizeof(speeds[0]); idx++) {
        if (baudrate == (int)speeds[idx].baud)
            return speeds[idx].speed;
    }
    return CBAUDEX;
}

int set_speed(int fd, struct termios *ti, int speed)
{
    struct serial_struct ss;
    int baudenum = get_speed(speed);

    if (speed != CBAUDEX) {
        if ((ioctl(fd, TIOCGSERIAL, &ss)) < 0) {
            ALOGI("%s: BAUD: error to get the serial_struct info:%s\n", __func__, strerror(errno));
            return -1;
        }
        ss.flags &= ~ASYNC_SPD_CUST;
        ss.flags |= (1 << 13);    /*set UPFLOWLATENCY flag to tty, or serial_core will reset tty->low_latency to 0*/
        /*set standard baudrate setting*/
        if ((ioctl(fd, TIOCSSERIAL, &ss)) < 0) {
            ALOGI("%s: BAUD: error to set serial_struct:%s\n", __func__, strerror(errno));
            return -2;
        }
        cfsetospeed(ti, baudenum);
        cfsetispeed(ti, baudenum);
        return tcsetattr(fd, TCSANOW, ti);
    }
    ALOGI("%s: unsupported non-standard baudrate: %d -> 0x%08x\n", __func__, speed, baudenum);
    return -3;
}

int cmd_hdr_baud_115k(P_STP_PARAMS_CONFIG pStpParamsConfig)
{
    STP_UART_CONFIG *gStpUartConfig = &pStpParamsConfig->sUartConfig;
    return (gTtyFd != -1) ? setup_uart_param(gTtyFd, 115200, gStpUartConfig) : -1;
}

int cmd_hdr_baud_921k(P_STP_PARAMS_CONFIG pStpParamsConfig)
{
    STP_UART_CONFIG *gStpUartConfig = &pStpParamsConfig->sUartConfig;
    return (gTtyFd != -1) ? setup_uart_param(gTtyFd, 921600, gStpUartConfig) : -1;
}

int cmd_hdr_baud_2kk(P_STP_PARAMS_CONFIG pStpParamsConfig)
{
    STP_UART_CONFIG *gStpUartConfig = &pStpParamsConfig->sUartConfig;
    return (gTtyFd != -1) ? setup_uart_param(gTtyFd, 2000000, gStpUartConfig) : -1;
}

int cmd_hdr_baud_2_5kk(P_STP_PARAMS_CONFIG pStpParamsConfig)
{
    STP_UART_CONFIG *gStpUartConfig = &pStpParamsConfig->sUartConfig;
    return (gTtyFd != -1) ? setup_uart_param(gTtyFd, 2500000, gStpUartConfig) : -1;
}

int cmd_hdr_baud_3kk(P_STP_PARAMS_CONFIG pStpParamsConfig)
{
    STP_UART_CONFIG *gStpUartConfig = &pStpParamsConfig->sUartConfig;
    return (gTtyFd != -1) ? setup_uart_param(gTtyFd, 3000000, gStpUartConfig) : -1;
}

int cmd_hdr_baud_3_5kk(P_STP_PARAMS_CONFIG pStpParamsConfig)
{
    STP_UART_CONFIG *gStpUartConfig = &pStpParamsConfig->sUartConfig;
    return (gTtyFd != -1) ? setup_uart_param(gTtyFd, 3500000, gStpUartConfig) : -1;
}

int cmd_hdr_baud_4kk(P_STP_PARAMS_CONFIG pStpParamsConfig)
{
    STP_UART_CONFIG *gStpUartConfig = &pStpParamsConfig->sUartConfig;
    return (gTtyFd != -1) ? setup_uart_param(gTtyFd, 4000000, gStpUartConfig) : -1;
}

int cmd_hdr_stp_open(P_STP_PARAMS_CONFIG pStpParamsConfig)
{
    int ld;

    (void)pStpParamsConfig;
    if ((STP_UART_FULL == gStpMode) && (-1 == gTtyFd)) {
        gTtyFd = open(gStpDev, O_RDWR | O_NOCTTY);
        if (gTtyFd < 0) {
            fprintf(stderr, "Can't open serial port %s\n", gStpDev);
            return -2;
        }
        ALOGI("real_tty(%s) opened(%d)\n", gStpDev, gTtyFd);

        /* Set TTY to N_MTKSTP line discipline */
        ld = N_MTKSTP;
        if (ioctl(gTtyFd, TIOCSETD, &ld) < 0) {
            fprintf(stderr, "Can't set ldisc to N_MTKSTP\n");
            return -3;
        }

        if (ioctl(gTtyFd, HCIUARTSETPROTO, 0) < 0) {
            ALOGE("Can't set HCIUARTSETPROTO\n");
            return -4;
        }
        return 0;
    }
    fprintf(stderr, "stp_open fail: stp_mode(%d) real_tty_fd(%d)\n", gStpMode, gTtyFd);
    return -1;
}

int cmd_hdr_stp_close(P_STP_PARAMS_CONFIG pStpParamsConfig)
{
    int ld;

    (void)pStpParamsConfig;
    if ((STP_UART_FULL == gStpMode) && (0 <= gTtyFd)) {
        /* Restore TTY line discipline */
        ld = N_TTY;
        if (ioctl(gTtyFd, TIOCSETD, &ld) < 0) {
            ALOGE("Can't restore line discipline");
            return -2;
        }

        close(gTtyFd);
        gTtyFd = -1;
        return 0;
    } else if (gTtyFd == -1) {
        return 0;
    }
    fprintf(stderr, "stp_close fail: stp_mode(%d) real_tty_fd(%d)\n", gStpMode, gTtyFd);
    return -1;
}

int cmd_hdr_stp_rst(P_STP_PARAMS_CONFIG pStpParamsConfig)
{
    int ret = 0;

    ret = cmd_hdr_stp_close(pStpParamsConfig);
    ret = cmd_hdr_stp_open(pStpParamsConfig);
    return ret;
}

#if CUST_MULTI_PATCH
/* GEMINI-PORT (A3): the directory-scan/header-parse body of the upstream
 * cmd_hdr_sch_patch lives in wmt_patch.c (unit-tested on the host); this
 * callback performs the same ioctls the upstream inline code issued. */
static int patch_ioctl_cb(const struct wmt_patch_entry *entry,
                          unsigned int patch_num, int is_first, void *ctx)
{
    STP_PATCH_INFO stpPatchInfo;

    (void)ctx;
    if (is_first)
        ioctl(gWmtFd, WMT_IOCTL_SET_PATCH_NUM, patch_num);

    memset(&stpPatchInfo, 0, sizeof(stpPatchInfo));
    stpPatchInfo.dowloadSeq = (int)entry->download_seq;
    memcpy(stpPatchInfo.addRess, entry->address, sizeof(stpPatchInfo.addRess));
    /* both are 256-byte buffers; entry->patch_name is NUL-terminated */
    memcpy(stpPatchInfo.patchName, entry->patch_name,
           sizeof(stpPatchInfo.patchName));
    stpPatchInfo.patchName[sizeof(stpPatchInfo.patchName) - 1] = '\0';
    ioctl(gWmtFd, WMT_IOCTL_SET_PATCH_INFO, &stpPatchInfo);
    return 0;
}

int cmd_hdr_sch_patch(P_STP_PARAMS_CONFIG pStpParamsConfig)
{
    int chipId = 0;
    int fwVersion = 0;
    int iRet = 0;
    int nMatched;

    if (gWmtFd < 0) {
        ALOGE("file descriptor is not valid\n");
        return -2;
    }

    /*1. ioctl to get CHIP ID (icId; aliases handled in wmt_patch.c)*/
    chipId = ioctl(gWmtFd, WMT_IOCTL_GET_CHIP_INFO, 0);
    /*2. ioctl to get FIRMWARE VERSION*/
    fwVersion = ioctl(gWmtFd, WMT_IOCTL_GET_CHIP_INFO, 2);
    ALOGI("chipId:0x%04x fwVersion:0x%04x\n", chipId, fwVersion);

    /*3. open directory patch located and offer matching patches*/
    if (NULL == pStpParamsConfig->pPatchPath)
        pStpParamsConfig->pPatchPath = (char *)CUST_COMBO_PATCH_PATH;

    nMatched = wmt_patch_scan(pStpParamsConfig->pPatchPath, chipId, fwVersion,
                              patch_ioctl_cb, NULL);
    if (nMatched == WMT_PATCH_SCAN_EOPENDIR) {
        ALOGE("patch path cannot be opened\n");
        iRet = -1;
    } else if (nMatched == WMT_PATCH_SCAN_EUNSUP) {
        iRet = -3;
    } else if (nMatched == 0) {
        /* upstream stayed silent and returned 0 here; keep the return
         * value (kernel-visible behavior) but complain loudly */
        ALOGE("WARNING: no matching WMT ROM patch in %s\n",
              pStpParamsConfig->pPatchPath);
    }
    return iRet;
}
#endif /* CUST_MULTI_PATCH */

/*
ret 0: success
ret 1: cmd not found
ret -x: handler return value
*/
int handle_cmd(P_STP_PARAMS_CONFIG pStpParamsConfig, char *cmd, int len)
{
    int ret = 1;
    int i;
    int cmd_len;

    for (i = 0; i < (int)(sizeof(cmd_hdr_table)/sizeof(cmd_hdr_table[0])); ++i) {
        cmd_len = (int)strlen(cmd_hdr_table[i].pCmd);
        if (!strncmp(cmd_hdr_table[i].pCmd, cmd, (len < cmd_len) ? len : cmd_len))
            ret = (*cmd_hdr_table[i].hdr_func)(pStpParamsConfig);
    }

    return ret;
}

void display_usage(void)
{
    unsigned int index = 0;
    static const char *usage[] = {
        "MTK WCN combo tool set, Gemini PDA native port (slice7)",
        "Usage: wmt_launcher [-m mode] [-p patchfolderpath] [-d uartdevicenode] [-b baudrate] [-c uartflowcontrol]",
        "    -m (BT/GPS/FM common interface mode selection)",
        "        -1: UART mode   -3: BTIF mode (MT6797 SoC default)   -4: SDIO mode",
        "    -p (MTK WCN chip firmware patch location)",
        "        default: /lib/firmware",
        "    -b/-d/-c: UART-mode-only options (unused on the MT6797 BTIF path)",
        "Environment: WMT_DEV_WAIT_SEC bounds the wait for /dev/stpwmt and the chip id (default 10)",
        "e.g. wmt_launcher -p /lib/firmware",
    };

    for (index = 0; index < sizeof(usage)/sizeof(usage[0]); index++)
        ALOGI("%s\n", usage[index]);
    exit(EXIT_FAILURE);
}

int para_valid_check(P_STP_PARAMS_CONFIG pStpParamConfig)
{
    if ((NULL != pStpParamConfig->pPatchPath) || (NULL != pStpParamConfig->pPatchName)) {
        if (NULL != pStpParamConfig->pPatchPath)
            ALOGI("MCU patch folder path: %s\n", pStpParamConfig->pPatchPath);
        if (NULL != pStpParamConfig->pPatchName)
            ALOGI("MCU patch full path: %s\n", pStpParamConfig->pPatchName);
    } else {
        puts("MCU patch name or patch not found, exit.");
        return -1;
    }
    if (pStpParamConfig->eStpMode != STP_SDIO && pStpParamConfig->eStpMode != STP_UART_MAND
        && pStpParamConfig->eStpMode != STP_UART_FULL) {
        puts("Stp Mode is not set, common interface use default: SDIO Mode");
        pStpParamConfig->eStpMode = STP_SDIO;
        return 0;
    }
    if (pStpParamConfig->eStpMode == STP_SDIO) {
        ALOGI("Common Interface: SDIO mode\n");
    } else if (pStpParamConfig->eStpMode == STP_UART_MAND || pStpParamConfig->eStpMode == STP_UART_FULL) {
        ALOGI("Common Interface: UART mode\n");
        if (NULL == pStpParamConfig->gStpDev) {
            pStpParamConfig->gStpDev = (char *)CUST_COMBO_STP_DEV;
            ALOGI("no uart device input, use default: %s\n", pStpParamConfig->gStpDev);
        }
        if (pStpParamConfig->iBaudrate < 0) {
            pStpParamConfig->iBaudrate = 4000000;
            ALOGI("no baudrate input, use default: %d\n", pStpParamConfig->iBaudrate);
        }
    }
    return 0;
}

static int wmt_cfg_item_parser(char *pItem)
{
    int maxIndex = sizeof(gChipModeInfo) / sizeof(gChipModeInfo[0]);
    int index = 0;
    int length = 0;
    char *str = NULL;
    char *keyStr = NULL;
    char *valueStr = NULL;

    if (NULL == pItem) {
        ALOGI("Warning:pItem is NULL\n");
        return -1;
    }
    /*all item must be start with mt66xx*/
    str = strstr(pItem, "m");
    if (NULL == str) {
        ALOGI("Warning:no string start with 'm' found in %s\n", pItem);
        return -2;
    }

    for (index = 0; index < maxIndex; index++) {
        keyStr = (char *)gChipModeInfo[index].antMode.pCfgItem;

        if (0 == strncasecmp(str, keyStr, strlen(keyStr))) {
            str = strstr(str, "=");
            if (NULL == str) {
                ALOGI("Warning:no '=' found\n");
                return -3;
            }
            str = strstr(str, "m");
            if (NULL == str) {
                ALOGI("Warning:no 'm' found\n");
                return -4;
            }

            while (((*str) == ' ') || ((*str) == '\t') || ((*str) == '\n')) {
                if (str >= pItem + strlen(pItem))
                    break;
                str++;
            }
            valueStr = str;

            while (((*str) != ' ') && ((*str) != '\t') && ((*str) != '\0') && ((*str) != '\n') && ((*str) != '\r')) {
                if (str >= pItem + strlen(pItem)) {
                    ALOGI("break\n");
                    break;
                }
                str++;
            }
            *str = '\0';
            length = sizeof(gChipModeInfo[index].antMode.cfgItemValue);
            strncpy(gChipModeInfo[index].antMode.cfgItemValue, valueStr, length - 1);
            gChipModeInfo[index].antMode.cfgItemValue[length - 1] = '\0';
            ALOGI("Info:key:%s value:%s\n", keyStr, gChipModeInfo[index].antMode.cfgItemValue);
            break;
        }
    }

    return 0;
}

/* GEMINI-PORT (A6): with STATIC_BUILD upstream's set_coredump_flag collapses
 * to "flag stays 0"; keep only the kernel-visible ioctl. */
static void set_coredump_flag(void)
{
    int coredumpEnableFlag = 0;

    if (gWmtFd < 0) {
        ALOGI("%s:invalid wmt fd\n", __func__);
        return;
    }
    /*set coredump mode to kernel driver*/
    ioctl(gWmtFd, WMT_IOCTL_WMT_COREDUMP_CTRL, coredumpEnableFlag);
}

static int get_wmt_cfg(int chipId)
{
/* GEMINI-PORT (A2): combo-chip-only ant-mode config; retargeted from
 * /system/etc/firmware to the Debian firmware dir. Unused on MT6797. */
#define WMTCFGFILEPATH "/lib/firmware/WMT.cfg"
#define OPENMODE "r"
#define MAXLINELEN 512
    FILE *file = NULL;
    int iRet = -1;
    char *pStr = NULL;
    char line[MAXLINELEN];

    (void)chipId;
    file = fopen(WMTCFGFILEPATH, OPENMODE);
    if (NULL == file) {
        ALOGI("%s cannot be opened, errno:%d\n", WMTCFGFILEPATH, errno);
        return -2;
    }
    iRet = 0;
    do {
        pStr = fgets(line, MAXLINELEN, file);
        if (NULL == pStr)
            break;
        wmt_cfg_item_parser(line);
        memset(line, 0, MAXLINELEN);
    } while (pStr != NULL);

    fclose(file);
    return iRet;
}

static int get_chip_info_index(int chipId)
{
    int index = -1;
    int left = 0;
    int middle = 0;
    int right = sizeof(gChipModeInfo) / sizeof(gChipModeInfo[0]) - 1;

    if ((chipId < gChipModeInfo[left].chipId) || (chipId > gChipModeInfo[right].chipId))
        return index;

    middle = (left + right) / 2;

    while (left <= right) {
        if (chipId > gChipModeInfo[middle].chipId) {
            left = middle + 1;
        } else if (chipId < gChipModeInfo[middle].chipId) {
            right = middle - 1;
        } else {
            index = middle;
            break;
        }
        middle = (left + right) / 2;
    }

    if (0 > index)
        ALOGI("no supported chipid found\n");
    else
        ALOGI("index:%d, chipId:0x%x\n", index, gChipModeInfo[index].chipId);

    return index;
}

static int setHifInfo(int chipId, char *cfgFilePath)
{
    int index = -1;

    index = get_chip_info_index(chipId);
    if ((gStpMode <= STP_MIN) || (STP_SDIO < gStpMode)) {
        ALOGI("STP Mode is not set, fetching default mode...\n");
        if (0 <= index)
            gStpMode = gChipModeInfo[index].stpMode;
        else
            gStpMode = -1;
    }

    if ((0 <= index) && (NULL != cfgFilePath)) {
        memset(gWmtCfgName, 0, sizeof(gWmtCfgName));
        snprintf(gWmtCfgName, sizeof(gWmtCfgName), "%s/%s", cfgFilePath,
                 gChipModeInfo[index].antMode.cfgItemValue);
    } else {
        memset(gWmtCfgName, 0, sizeof(gWmtCfgName));
    }
    ALOGI("chipId(0x%04x), default Mode(%d), wmtCfgFile(%s)\n", chipId, gStpMode, gWmtCfgName);
    return gStpMode;
}

/* GEMINI-PORT (A5): parse the staged WMT_SOC.cfg the same way the kernel
 * will, so a bad/missing payload is visible in the launcher log at startup.
 * Validation only — the kernel loads its own copy by bare name via
 * request_firmware(); we deliberately do NOT send WMT_IOCTL_WMT_CFG_NAME
 * on the SoC path (an absolute path would break request_firmware). */
static void validate_wmt_soc_cfg(const char *patchDir)
{
    char cfgPath[NAME_MAX + 1];
    struct wmt_soc_cfg cfg;
    int ret;

    if (snprintf(cfgPath, sizeof(cfgPath), "%s/%s", patchDir,
                 CUST_CFG_WMT_SOC_NAME) >= (int)sizeof(cfgPath)) {
        ALOGE("WARNING: patch dir path too long for %s check\n",
              CUST_CFG_WMT_SOC_NAME);
        return;
    }
    wmt_soc_cfg_init(&cfg);
    ret = wmt_soc_cfg_parse_file(cfgPath, &cfg);
    if (ret != 0) {
        ALOGE("WARNING: cannot read %s (%s) - kernel WMT core will fall back to built-in defaults\n",
              cfgPath, strerror(errno));
        return;
    }
    ALOGI("%s parsed: coex_wmt_ant_mode=%d wmt_gps_lna_pin=%d wmt_gps_lna_enable=%d co_clock_flag=%d (%d known / %d unknown pairs, %d malformed lines)\n",
          cfgPath, cfg.coex_wmt_ant_mode, cfg.wmt_gps_lna_pin,
          cfg.wmt_gps_lna_enable, cfg.co_clock_flag,
          cfg.pairs_parsed, cfg.pairs_unknown, cfg.lines_malformed);
}

/*
 * -m: mode (SDIO/UART/BTIF)
 * -d: uart device node
 * -b: baudrate
 * -c: enable SW FC or not
 * -p: patch folder path
 * -n: patch file name (fullpath)
 */
int main(int argc, char *argv[])
{
    static const char *opString = "m:d:b:c:p:n:?";
    int opt, ld, err;
    int baud = 0;
    struct sigaction sa;
    struct pollfd fds[2];
    int fd_num = 0;
    int len = 0;
    int uartFcCtrl = 0;
    int chipId = -1;
    int wait_sec;
    int waited_ms;
    STP_PARAMS_CONFIG sStpParaConfig;

    /* GEMINI-PORT (A7): upstream left this uninitialized; on the SoC path a
     * missing -p made pPatchPath stack garbage. */
    memset(&sStpParaConfig, 0, sizeof(sStpParaConfig));

    setvbuf(stdout, NULL, _IOLBF, 0); /* GEMINI-PORT: line-buffer for journald */
    ALOGI("wmt_launcher starting (gemini slice7 native build)\n");

    wait_sec = wmt_dev_wait_sec(WMT_LAUNCHER_DEFAULT_WAIT_SEC);

    /* GEMINI-PORT (A1): bounded wait for /dev/stpwmt (upstream: forever) */
    gWmtFd = wmt_open_dev_bounded(CUST_COMBO_WMT_DEV, O_RDWR | O_NOCTTY, wait_sec);
    if (gWmtFd < 0)
        return EXIT_FAILURE;
    ALOGI("open device node succeed.(Node:%s, fd:%d)\n", CUST_COMBO_WMT_DEV, gWmtFd);

    /* GEMINI-PORT (A1/A6): bounded chip-id query replaces the Android
     * property dance + infinite retry (upstream query_chip_id loop). The
     * queried id is the one wmt_loader published via COMBO_IOCTL_SET_CHIP_ID. */
    waited_ms = 0;
    for (;;) {
        chipId = ioctl(gWmtFd, WMT_IOCTL_WMT_QUERY_CHIPID, NULL);
        if (chipId > 0)
            break;
        if (waited_ms >= wait_sec * 1000)
            break;
        usleep(300000);
        waited_ms += 300;
    }
    if (chipId <= 0) {
        fprintf(stderr,
            "ERROR: no CONSYS chip id from kernel after %d s (got %d). Was wmt_loader run first?\n",
            wait_sec, chipId);
        close(gWmtFd);
        return EXIT_FAILURE;
    }
    ALOGI("chipId:0x%04x\n", chipId);

    if ((0x0321 == chipId) || (0x0335 == chipId) || (0x0337 == chipId)) {
        chipId = 0x6735;
        ALOGI("for denali chipid convert\n");
    }
    if (0x0326 == chipId) {
        chipId = 0x6755;
        ALOGI("for jade chipid convert\n");
    }
    /* GEMINI-PORT: everest alias, as upstream applies in cmd_hdr_sch_patch;
     * needed here too in case the stub stores the icId form. */
    if (0x0279 == chipId) {
        chipId = 0x6797;
        ALOGI("for everest chipid convert\n");
    }

    if ((0x6735 == chipId) || (0x6752 == chipId) || (0x6582 == chipId) || (0x6592 == chipId)
        || (0x6572 == chipId) || (0x6571 == chipId) || (0x8127 == chipId)
        || (0x8163 == chipId) || (0x6580 == chipId) || (0x6755 == chipId)
        || (0x6797 == chipId) || (0x7623 == chipId)) {
        ALOGI("run SOC chip flow\n");
        gStpMode = STP_BTIF_FULL;
        memset(gPatchFolder, 0, sizeof(gPatchFolder));

        opt = getopt(argc, argv, opString);
        while (opt != -1) {
            switch (opt) {
            case 'm':
                gStpMode = atoi(optarg);
                sStpParaConfig.eStpMode = gStpMode;
                ALOGI("stpmode[%d]\n", gStpMode);
                break;
            case 'p':
                strncpy(gPatchFolder, optarg, sizeof(gPatchFolder) - 1);
                gPatchFolder[sizeof(gPatchFolder) - 1] = '\0';
                sStpParaConfig.pPatchPath = gPatchFolder;
                break;
            case '?':
            default:
                display_usage();
                break;
            }
            opt = getopt(argc, argv, opString);
        }
        /* GEMINI-PORT (A7): default the patch dir instead of leaving it unset */
        if (NULL == sStpParaConfig.pPatchPath) {
            strncpy(gPatchFolder, CUST_COMBO_PATCH_PATH, sizeof(gPatchFolder) - 1);
            sStpParaConfig.pPatchPath = gPatchFolder;
            ALOGI("no -p given, default patch dir: %s\n", gPatchFolder);
        }
        /* GEMINI-PORT (A5): startup validation of the staged WMT config */
        validate_wmt_soc_cfg(sStpParaConfig.pPatchPath);

        /* send default patch file name path to driver */
        ioctl(gWmtFd, WMT_IOCTL_SET_PATCH_NAME, gPatchName);
        /* set fm mode & stp mode*/
        ioctl(gWmtFd, WMT_IOCTL_SET_STP_MODE, ((gFmMode & 0x0F) << 4) | (gStpMode & 0x0F));
        set_coredump_flag();
    } else {
        ALOGI("run combo chip flow\n");
        sStpParaConfig.pPatchPath = NULL;
        sStpParaConfig.pPatchName = NULL;
        sStpParaConfig.gStpDev = NULL;
        sStpParaConfig.eStpMode = -1;
        sStpParaConfig.iBaudrate = -1;
        sStpParaConfig.sUartConfig.fc = UART_DISABLE_FC;
        sStpParaConfig.sUartConfig.parity = 0;
        sStpParaConfig.sUartConfig.stop_bit = 0;

        /*Default parameters starts*/
        baud = 4000000;
        gStpMode = -1;
        uartFcCtrl = UART_DISABLE_FC;
        strncpy(gStpDev, CUST_COMBO_STP_DEV, sizeof(gStpDev) - 1);
        gStpDev[sizeof(gStpDev) - 1] = '\0';
        memset(gPatchFolder, 0, sizeof(gPatchFolder));
        memset(gPatchName, 0, sizeof(gPatchName));
        /*Default parameters ends*/

        opt = getopt(argc, argv, opString);
        while (opt != -1) {
            switch (opt) {
            case 'm':
                gStpMode = atoi(optarg);
                sStpParaConfig.eStpMode = gStpMode;
                break;
            case 'd':
                strncpy(gStpDev, optarg, sizeof(gStpDev) - 1);
                gStpDev[sizeof(gStpDev) - 1] = '\0';
                sStpParaConfig.gStpDev = gStpDev;
                break;
            case 'b':
                baud = atoi(optarg);
                sStpParaConfig.iBaudrate = baud;
                break;
            case 'c':
                uartFcCtrl = atoi(optarg);
                sStpParaConfig.sUartConfig.fc = uartFcCtrl;
                ALOGI("c found\n");
                break;
            case 'p':
                strncpy(gPatchFolder, optarg, sizeof(gPatchFolder) - 1);
                gPatchFolder[sizeof(gPatchFolder) - 1] = '\0';
                sStpParaConfig.pPatchPath = gPatchFolder;
                break;
            case 'n':
                strncpy(gPatchName, optarg, sizeof(gPatchName) - 1);
                gPatchName[sizeof(gPatchName) - 1] = '\0';
                sStpParaConfig.pPatchName = gPatchName;
                break;
            case '?':
            default:
                display_usage();
                break;
            }
            opt = getopt(argc, argv, opString);
        }

        ioctl(gWmtFd, WMT_IOCTL_WMT_TELL_CHIPID, chipId);
        ALOGI("set chipId(0x%x) to HIF-SDIO module\n", chipId);

        get_wmt_cfg(chipId);

        setHifInfo(chipId, sStpParaConfig.pPatchPath);
        ALOGI("HifConfig:0x%04x, wmtCfgFile:%s\n", sStpParaConfig.eStpMode, gWmtCfgName);
        set_coredump_flag();
        if (0 != para_valid_check(&sStpParaConfig)) {
            /* GEMINI-PORT (A6): dropped the legacy positional-argument
             * fallback parser; flags are the only supported syntax. */
            display_usage();
        }

        /* send default patch file name path to driver */
        ioctl(gWmtFd, WMT_IOCTL_SET_PATCH_NAME, gPatchName);
        /* send uart name to driver*/
        if (sStpParaConfig.gStpDev) {
            gUartName = strstr(sStpParaConfig.gStpDev, "tty");
            if (!gUartName)
                ALOGI("no uart name found in %s\n", sStpParaConfig.gStpDev);
            else
                ALOGI("uart name %s\n", gUartName);
        }

        if (!gUartName) {
            gUartName = "ttyMT2";
            ALOGI("use default uart %s\n", gUartName);
        }

        ioctl(gWmtFd, WMT_IOCTL_PORT_NAME, gUartName);

        /* send hardware interface configuration to driver */
        ioctl(gWmtFd, WMT_IOCTL_SET_STP_MODE, ((baud & 0xFFFFFF) << 8) | ((gFmMode & 0x0F) << 4) | (gStpMode & 0x0F));

        /* send WMT config name configuration to driver */
        ioctl(gWmtFd, WMT_IOCTL_WMT_CFG_NAME, gWmtCfgName);
    }

    ioctl(gWmtFd, WMT_IOCTL_SET_LAUNCHER_KILL, 0);

    /* GEMINI-PORT (A4): upstream called WMT_IOCTL_GET_APO_FLAG (28) here and,
     * on nonzero, spawned a thread that powered the chip on via
     * WMT_IOCTL_LPBK_POWER_CTRL. This kernel tree does not implement ioctl 28
     * (returns -EINVAL, which upstream would misread as "power on now"), and
     * the supervised bring-up plan requires that nothing powers CONSYS as a
     * daemon side effect — function drivers request power via func_on when
     * an interface comes up. Removed (with the launcher_pwr_on_chip and
     * launcher_set_fwdbg_flag threads and the pthread dependency). */

    /*set signal handler*/
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_NOCLDSTOP;
    sa.sa_handler = SIG_IGN;
    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);

    sa.sa_handler = sig_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    sa.sa_handler = sig_hup;
    sigaction(SIGHUP, &sa, NULL);

    fds[0].fd = gWmtFd; /* stp_wmt fd */
    fds[0].events = POLLIN | POLLRDNORM; /* wait read events */
    ++fd_num;

    ALOGI("entering command-service loop (patch dir: %s)\n",
          sStpParaConfig.pPatchPath ? sStpParaConfig.pPatchPath : "(none)");

    while (!__io_canceled) {
        fds[0].revents = 0;
        err = poll(fds, fd_num, 2000);
        if (err < 0) {
            if (errno == EINTR)
                continue;
            ALOGI("poll error:%d errno:%d, %s\n", err, errno, strerror(errno));
            break;
        } else if (!err) {
            continue;
        }
        if (fds[0].revents & POLLIN) {
            memset(gCmdStr, 0, sizeof(gCmdStr));
            len = read(gWmtFd, gCmdStr, sizeof(gCmdStr)-1);
            if (len <= 0 || len >= (int)sizeof(gCmdStr)) {
                ALOGI("POLLIN(%d) but read fail:%d\n", gWmtFd, len);
                continue;
            }
            gCmdStr[len] = '\0';
            err = handle_cmd(&sStpParaConfig, gCmdStr, len);
            if (!err) {
                snprintf(gRespStr, sizeof(gRespStr), "ok");
            } else {
                if (err == 1)
                    snprintf(gRespStr, sizeof(gRespStr), "cmd not found");
                else
                    snprintf(gRespStr, sizeof(gRespStr), "resp_%d", err);
            }
            ALOGI("cmd(%s) resp(%s)\n", gCmdStr, gRespStr);
            len = write(gWmtFd, gRespStr, strlen(gRespStr));
            if (len != (int)strlen(gRespStr)) {
                fprintf(stderr, "write resp(%d) fail: len(%d), errno(%d, %s)\n",
                        gWmtFd, len, errno, (len == -1) ? strerror(errno) : "");
            }
        }
    }

    if (gWmtFd >= 0) {
        close(gWmtFd);
        gWmtFd = -1;
    }

    if (gStpMode == STP_UART_FULL && gTtyFd >= 0) {
        /* Restore TTY line discipline */
        ld = N_TTY;
        if (ioctl(gTtyFd, TIOCSETD, &ld) < 0) {
            ALOGE("Can't restore line discipline");
            exit(1);
        }

        close(gTtyFd);
        gTtyFd = -1;
    }

    return 0;
}
