/* One-shot SylixOS SD formatting. This build intentionally permits only sdcard-1. */
#define __SYLIXOS_KERNEL
#include <SylixOS.h>
#include <fs/oemDisk/oemDisk.h>
#include <fs/oemDisk/oemFdisk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#define TARGET "/dev/blk/sdcard-1"
#define MAX_MOUNTS 128
#define PATH_LEN 1024
static char volumes[MAX_MOUNTS][PATH_LEN];

static int fail(const char *stage)
{
    fprintf(stderr, "FAILED: %s (%s).\n",
            stage, strerror(errno));
    sync();
    return 1;
}

/* Exact source matching prevents sdcard-10 or unrelated devices being selected. */
static int source_matches(const char *source)
{
    const char *p;
    size_t n = strlen(TARGET);
    if (strcmp(source, TARGET) == 0) return 1;
    if (strncmp(source, TARGET, n) != 0 || source[n] != ':') return 0;
    p = source + n + 1;
    if (!*p) return 0;
    while (*p) if (!isdigit((unsigned char)*p++)) return 0;
    return 1;
}

static int list_mounts(void)
{
    FILE *fp = popen("showmount", "r");
    char line[4096], volume[PATH_LEN], source[PATH_LEN];
    int count = 0, bad = 0;
    if (!fp) return -1;
    while (fgets(line, sizeof(line), fp)) {
        if (!strchr(line, '\n') && !feof(fp)) { bad = 1; break; }
        if (sscanf(line, "%1023s %1023s", volume, source) != 2) continue;
        if (!source_matches(source)) continue;
        if (volume[0] != '/' || count == MAX_MOUNTS) { bad = 1; break; }
        strcpy(volumes[count++], volume);
    }
    if (ferror(fp)) bad = 1;
    if (pclose(fp) != 0) bad = 1;
    if (bad) { errno = EIO; return -1; }
    return count;
}

static int clear_gpt_header(int fd, ULONG sector)
{
    unsigned char buf[512];
    off_t offset = (off_t)sector * 512;
    if (pread(fd, buf, sizeof(buf), offset) != sizeof(buf)) return -1;
    if (memcmp(buf, "EFI PART", 8) != 0) return 0;
    memset(buf, 0, sizeof(buf));
    return pwrite(fd, buf, sizeof(buf), offset) == sizeof(buf) ? 0 : -1;
}

int main(int argc, char **argv)
{
    int execute = 0, i, fd, count;
    struct stat st;
    LW_BLK_INFO info;
    ULONG sectors = 0, sector_size = 0;
    PLW_OEMDISK_CB oem = NULL;
    LW_OEMFDISK_PART part;
    char mounted[PATH_LEN];

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            puts("Usage: disk_format [--yes]\n"
                 "Format " TARGET " as FAT32. --yes skips confirmation.");
            return 0;
        } else if (!strcmp(argv[i], "--yes")) execute = 1;
        else { fprintf(stderr, "Invalid arguments; use --help\n"); return 2; }
    }
    fd = open(TARGET, O_RDONLY);
    if (fd < 0) return fail("open target");
    memset(&info, 0, sizeof(info));
    if (fstat(fd, &st) || !S_ISBLK(st.st_mode) ||
        ioctl(fd, LW_BLKD_CTRL_INFO, &info) ||
        ioctl(fd, LW_BLKD_GET_SECNUM, &sectors) ||
        ioctl(fd, LW_BLKD_GET_SECSIZE, &sector_size) ||
        ioctl(fd, LW_BLKD_CTRL_OEMDISK, &oem) || !oem) {
        close(fd); return fail("inspect block device/OEM mount metadata");
    }
    info.BLKI_cSerial[sizeof(info.BLKI_cSerial)-1] = 0;
    info.BLKI_cProduct[sizeof(info.BLKI_cProduct)-1] = 0;
    info.BLKI_cMedia[sizeof(info.BLKI_cMedia)-1] = 0;
    printf("SD card: %s (%s, %.1f GiB)\n", TARGET, info.BLKI_cProduct,
           (double)sectors * sector_size / (1024.0 * 1024.0 * 1024.0));
    if (info.BLKI_uiType != LW_BLKD_CTRL_INFO_TYPE_SDMMC ||
        (strncmp(info.BLKI_cMedia, "SDSC(", 5) && strncmp(info.BLKI_cMedia, "SDHC(", 5) &&
         strncmp(info.BLKI_cMedia, "SDXC(", 5)) || sector_size != 512 ||
        sectors <= 4096 || (unsigned long long)sectors > 0xffffffffULL) {
        close(fd); fprintf(stderr, "Refused: unsupported media/geometry (eMMC forbidden).\n"); return 2;
    }
    if (!execute) {
        char answer[16];
        printf("Erase all data and format? [y/N] ");
        fflush(stdout);
        if (!fgets(answer, sizeof(answer), stdin) ||
            (strcmp(answer, "y\n") && strcmp(answer, "Y\n") &&
             strcmp(answer, "yes\n") && strcmp(answer, "YES\n"))) {
            close(fd);
            puts("Cancelled.");
            return 0;
        }
    }
    if (geteuid()) { close(fd); errno = EACCES; return fail("root required"); }
    count = list_mounts();
    if (count < 0) { close(fd); return fail("enumerate mounted partitions"); }
    sync();
    for (i = count - 1; i >= 0; i--) {
        {
            int volume_fd = open(volumes[i], O_RDONLY);
            int result;
            if (volume_fd < 0) { close(fd); return fail("open volume to disable forced unmount"); }
            result = ioctl(volume_fd, FIOSETFORCEDEL, LW_FALSE);
            close(volume_fd);
            if (result) { close(fd); return fail("disable forced unmount"); }
        }
        if (API_Unmount(volumes[i])) { close(fd); return fail("unmount (possibly busy)"); }
    }
    count = list_mounts();
    if (count != 0) { close(fd); errno = EBUSY; return fail("target still mounted or mount enumeration failed"); }
    /* The raw driver forbids writable opens while any partition is mounted. */
    close(fd);
    fd = open(TARGET, O_RDWR);
    if (fd < 0) return fail("open unmounted target for writing");
    {
        LW_BLK_INFO current;
        ULONG current_sectors = 0, current_size = 0;
        PLW_OEMDISK_CB current_oem = NULL;
        memset(&current, 0, sizeof(current));
        if (fstat(fd, &st) || !S_ISBLK(st.st_mode) ||
            ioctl(fd, LW_BLKD_CTRL_INFO, &current) ||
            ioctl(fd, LW_BLKD_GET_SECNUM, &current_sectors) ||
            ioctl(fd, LW_BLKD_GET_SECSIZE, &current_size) ||
            ioctl(fd, LW_BLKD_CTRL_OEMDISK, &current_oem)) {
            close(fd); return fail("recheck writable target identity");
        }
        current.BLKI_cSerial[sizeof(current.BLKI_cSerial)-1] = 0;
        current.BLKI_cProduct[sizeof(current.BLKI_cProduct)-1] = 0;
        current.BLKI_cMedia[sizeof(current.BLKI_cMedia)-1] = 0;
        if (current.BLKI_uiType != info.BLKI_uiType ||
            strcmp(current.BLKI_cSerial, info.BLKI_cSerial) ||
            strcmp(current.BLKI_cProduct, info.BLKI_cProduct) ||
            strcmp(current.BLKI_cMedia, info.BLKI_cMedia) ||
            current_sectors != sectors || current_size != sector_size || current_oem != oem) {
            close(fd); errno = ENXIO; return fail("target identity changed after unmount");
        }
        count = list_mounts();
        if (count != 0) {
            close(fd); errno = EBUSY; return fail("target remounted before writing");
        }
    }
    if (clear_gpt_header(fd, 1) || clear_gpt_header(fd, sectors-1) || fsync(fd)) {
        close(fd); return fail("clear old GPT headers");
    }
    close(fd);
    memset(&part, 0, sizeof(part));
    part.FDP_bIsActive = LW_TRUE;
    part.FDP_ucPartType = 0x0b;
    part.FDP_ucSzPct = 0; /* All remaining space. Native MBR API reserves 2048 sectors. */
    if (API_OemFdisk(TARGET, &part, 1, 4096) != 1) return fail("create MBR partition");
    sync();
    if (API_OemDiskRemount(oem)) return fail("remount new partition");
    if (API_OemDiskGetNPart(oem) != 1 ||
        API_OemDiskGetPath(oem, 0, mounted, sizeof(mounted))) return fail("locate new partition 0");
    if (mkfs(mounted)) return fail("mkfs");
    sync();
    printf("Done: FAT32, mounted at %s\n", mounted);
    return 0;
}
