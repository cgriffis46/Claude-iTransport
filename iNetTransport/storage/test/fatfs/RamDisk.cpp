// FatFs's disk functions on a RAM disk, for HttpFatFs_test: drive 0, 512-byte
// sectors, as an SD card looks to FatFs. ramDiskFail makes every access
// fail (a card pulled out).
#include <cstring>
#include <vector>
#include "ff.h"
#include "diskio.h"

std::vector<unsigned char> ramDisk(8u << 20);   // 8 MB
bool ramDiskFail = false;

extern "C" {
DSTATUS disk_status(BYTE pdrv) { return pdrv == 0 && !ramDiskFail ? 0 : STA_NOINIT; }
DSTATUS disk_initialize(BYTE pdrv) { return disk_status(pdrv); }
DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || ramDiskFail) return RES_NOTRDY;
    if ((sector + count) * 512u > ramDisk.size()) return RES_PARERR;
    std::memcpy(buff, &ramDisk[sector * 512u], count * 512u);
    return RES_OK;
}
DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || ramDiskFail) return RES_NOTRDY;
    if ((sector + count) * 512u > ramDisk.size()) return RES_PARERR;
    std::memcpy(&ramDisk[sector * 512u], buff, count * 512u);
    return RES_OK;
}
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    if (pdrv != 0 || ramDiskFail) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC: return RES_OK;
    case GET_SECTOR_COUNT: *static_cast<LBA_t*>(buff) = ramDisk.size() / 512u; return RES_OK;
    case GET_SECTOR_SIZE: *static_cast<WORD*>(buff) = 512; return RES_OK;
    case GET_BLOCK_SIZE: *static_cast<DWORD*>(buff) = 1; return RES_OK;
    default: return RES_PARERR;
    }
}
}
