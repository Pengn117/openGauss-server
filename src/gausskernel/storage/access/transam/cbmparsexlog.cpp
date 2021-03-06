/*
 * Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 *
 * openGauss is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 * -------------------------------------------------------------------------
 *
 * cbmparsexlog.cpp
 *	  Functions for parsing Write-Ahead-Log and write cbm files
 *
 * IDENTIFICATION
 *        src/gausskernel/storage/access/transam/cbmparsexlog.cpp
 *
 * -------------------------------------------------------------------------
 */
#include <sys/time.h>

#include "postgres.h"
#include "knl/knl_variable.h"

#include "access/cbmparsexlog.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "access/visibilitymap.h"
#include "catalog/pg_control.h"
#include "catalog/storage_xlog.h"
#include "commands/dbcommands.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port.h"
#include "postmaster/cbmwriter.h"
#include "storage/copydir.h"
#include "storage/fd.h"
#include "storage/freespace.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "utils/memutils.h"
#include "utils/relmapper.h"

/* we can put the following globals into XlogCbmSys */
static XLogRecPtr tmpTargetLSN = InvalidXLogRecPtr;
static XLogRecPtr latestCompTargetLSN = InvalidXLogRecPtr;
static bool tmpLSNIsRecEnd = true;

/* File name stem for bitmap files. */
static const char* const cbmFileNameStem = "pg_xlog_";
static const int cbmFileNameStemLen = strlen(cbmFileNameStem);

static const char* const mergedCbmFileNameStem = "pg_merged_xlog_";

/* File name template for bitmap files.  The 1st format tag is a directory
name, the 2nd tag is the stem, the 3rd tag is a file sequence number, the 4th
tag is the start LSN for the file, the 5th tag is the end LSN of the file. */
static const char* const bmpFileNameTemplate = "%s%s%lu_%08X%08X_%08X%08X.cbm";

static const char* const mergedBmpFileNameTemplate = "%s%s%08X%08X_%08X%08X_%ld-%d.cbm";

static void CBMFileHomeInitialize(void);
static void ResetXlogCbmSys(void);
static bool IsCBMFile(const char* fileName, uint64* seqNum, XLogRecPtr* startLSN, XLogRecPtr* endLSN);
static void ValidateCBMFile(const char* filename, XLogRecPtr* trackedLSN, uint64* lastfileSize, bool truncErrPage);
static bool ReadCBMPage(BitmapFile* cbmFile, char* page, bool* checksumOk);
static pg_crc32c CBMPageCalcCRC(const char* page);
static XLogRecPtr InitCBMTrackStartLSN(
    bool startupXlog, bool fromScratch, XLogRecPtr lastTrackedLSN, XLogRecPtr startupCPRedo);
static void InitCBMStartFileAndTrack(bool fromScratch, XLogRecPtr trackStartLSN, const char* lastfileName,
    XLogRecPtr lastfileStartLSN, XLogRecPtr lastfileTrackedLSN, uint64 lastfileSize, uint64 lastfileSeqNum);
static void SetCBMFileName(char* cbmFileNameBuf, uint64 seqNum, XLogRecPtr startLSN, XLogRecPtr endLSN);
static void SetNewCBMFileName(XLogRecPtr startLSN);
static void StartNextCBMFile(XLogRecPtr startLSN);
static void StartExistCBMFile(uint64 lastfileSize);
static HTAB* CBMPageHashInitialize(MemoryContext memoryContext);
static bool ParseXlogIntoCBMPages(TimeLineID timeLine, bool isRecEnd);
static int CBMXLogPageRead(XLogReaderState* xlogreader, XLogRecPtr targetPagePtr, int reqLen, XLogRecPtr targetRecPtr,
    char* readBuf, TimeLineID* pageTLI);
static void TrackChangeBlock(XLogReaderState* record);
static void TrackRelPageModification(XLogReaderState* record);
static void TrackCuBlockModification(XLogReaderState* record);
static void TrackRelStorageDrop(XLogReaderState* record);
static void TrackRelStorageCreate(XLogReaderState* record);
static void TrackRelStorageTruncate(XLogReaderState* record);
static void TrackVMPageModification(XLogReaderState* record);
static void TrackDbStorageChange(XLogReaderState* record);
static void TrackTblspcStorageChange(XLogReaderState* record);
static void TrackRelmapChange(XLogReaderState* record);
static void RegisterBlockChange(const RelFileNode& rNode, ForkNumber forkNum, BlockNumber blkNo);
static void RegisterBlockChangeExtended(
    const RelFileNode& rNode, ForkNumber forkNum, BlockNumber blkNo, uint8 pageType, BlockNumber truncBlkNo);
static void CBMPageEtySetBitmap(CbmHashEntry* cbmPageEntry, BlockNumber blkNo, uint8 pageType, BlockNumber truncBlkNo);
static void CBMPageSetBitmap(char* page, BlockNumber blkNo, uint8 pageType, BlockNumber truncBlkNo);
static void CreateNewCBMPageAndInsert(
    CbmHashEntry* cbmPageEntry, BlockNumber blkNo, uint8 pageType, BlockNumber truncBlkNo);
static void CreateDummyCBMEtyPageAndInsert(void);
static void FlushCBMPagesToDisk(XlogBitmap* xlogCbmSys, bool isCBMWriter);
static int CBMPageSeqCmp(const void* a, const void* b);
static void FlushOneCBMPage(const char* page, XlogBitmap* xlogCbmSys);
static void RotateCBMFile(void);
static void RemoveAllCBMFiles(int elevel);
static void PrintCBMHashTab(HTAB* cbmPageHash);
static void CBMGetMergedHash(
    XLogRecPtr startLSN, XLogRecPtr endLSN, HTAB* cbmPageHash, XLogRecPtr* mergeStartLSN, XLogRecPtr* mergeEndLSN);
static CbmFileName** GetAndValidateCBMFileArray(XLogRecPtr startLSN, XLogRecPtr endLSN, int* fileNum);
static CbmFileName** GetCBMFileArray(XLogRecPtr startLSN, XLogRecPtr endLSN, int* fileNum, bool missingOk);
static CbmFileName** SortCBMFilesList(Dllist* cbmFileList, int cbmFileNum);
static int CBMFileNameSeqCmp(const void* a, const void* b);
static void PrintCBMFileArray(CbmFileName** cbmFileNameArray, int cbmFileNum, XLogRecPtr startLSN, XLogRecPtr endLSN);
static void ValidateCBMFileArray(
    CbmFileName** cbmFileNameArray, int cbmFileNum, XLogRecPtr startLSN, XLogRecPtr endLSN);
static void MergeCBMFileArrayIntoHash(CbmFileName** cbmFileNameArray, int cbmFileNum, XLogRecPtr startLSN,
    XLogRecPtr endLSN, HTAB* cbmPageHash, XLogRecPtr* mergeStartLSN, XLogRecPtr* mergeEndLSN);
static void CBMPageIterBegin(cbmPageIterator* pageIteratorPtr, CbmFileName* cbmFile);
static bool CBMPageIterNext(cbmPageIterator* pageIteratorPtr, CbmFileName* cbmFile);
static void CBMPageIterEnd(cbmPageIterator* pageIteratorPtr, CbmFileName* cbmFile);
static void MergeCBMPageIntoHash(const char* page, HTAB* cbmPageHash);
static void ValidateCBMPageHeader(cbmpageheader* cbmPageHeader);
static void CBMHashRemove(const CBMPageTag& cbmPageTag, HTAB* cbmPageHash, bool isCBMWriter);
static void CBMHashRemoveDb(const CBMPageTag& cbmPageTag, HTAB* cbmPageHash, bool isCBMWriter);
static void CBMHashRemoveTblspc(const CBMPageTag& cbmPageTag, HTAB* cbmPageHash, bool isCBMWriter);
static void CBMPageEtyRemove(const CBMPageTag& cbmPageTag, HTAB* cbmPageHash, bool isCBMWriter, bool removeEntry);
static void CBMPageEtyRemoveRestFork(const CBMPageTag& cbmPageTag, HTAB* cbmPageHash, bool isCBMWriter);
static void CBMPageEtyTruncate(
    const CBMPageTag& cbmPageTag, HTAB* cbmPageHash, BlockNumber truncBlkNo, bool isCBMWriter);
static void CBMPageEtyMergePage(CbmHashEntry* cbmPageEntry, const char* page);
static void CBMPageMergeBitmap(char* cbmHashPage, const char* newPage);
static void CopyCBMPageAndInsert(CbmHashEntry* cbmPageEntry, const char* page);
static uint64 GetCBMHashTotalPageNum(HTAB* cbmPageHash);
static FILE* MergedXlogCBMSysInitFile(XlogBitmap* mergedXlogCbmSys);
static void FreeCBMFileArray(CbmFileName** cbmFileNameArray, int cbmFileNum);
static CBMArrayEntry* ConvertCBMHashIntoArray(HTAB* cbmPageHash, long* arrayLength, bool destroyHashEntry);
static void MergeOneCBMPageIntoArrayEntry(char* page, CBMArrayEntry* cbmArrayEntry);
static bool CBMBitmapNext(CBMBitmapIterator* iter, BlockNumber* blkno);
static void InitCBMArrayEntryBlockArray(CBMArrayEntry* cbmArrayEntry);
static void CBMArrayEntryRigsterBlock(CBMArrayEntry* cbmArrayEntry, BlockNumber blkNo);
static void UnlinkCBMFile(const char* cbmFileName);
static XLogRecPtr GetTmpTargetLSN(bool* isRecEnd);
static void SetTmpTargetLSN(XLogRecPtr targetLSN, bool isRecEnd);
static XLogRecPtr GetLatestCompTargetLSN(void);
static void SetLatestCompTargetLSN(XLogRecPtr targetLSN);

extern void* palloc_extended(Size size, int flags);

extern void InitXlogCbmSys(void)
{
    CBMFileHomeInitialize();

    t_thrd.cbm_cxt.XlogCbmSys->out.fd = -1;
    t_thrd.cbm_cxt.XlogCbmSys->xlogRead.fd = -1;
    ResetXlogCbmSys();

    t_thrd.cbm_cxt.XlogCbmSys->xlogParseFailed = false;
    t_thrd.cbm_cxt.XlogCbmSys->firstCPCreated = false;
}

static void CBMFileHomeInitialize(void)
{
    Assert(t_thrd.proc_cxt.DataDir);
    int rc = 0;
    int pathlen = strlen(t_thrd.proc_cxt.DataDir) + 8;
    if (pathlen >= MAXPGPATH)
        ereport(FATAL, (errmsg("Length of CBM file home path exceeds MAXPGPATH!")));

    pathlen = strlen(t_thrd.proc_cxt.DataDir);
    rc = strncpy_s(t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome, MAXPGPATH, t_thrd.proc_cxt.DataDir, pathlen);
    securec_check(rc, "\0", "\0");

    if (t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome[pathlen - 1] != '/') {
        t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome[pathlen] = '/';
        t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome[pathlen + 1] = '\0';
    }

    rc = strncat_s(t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome, MAXPGPATH, CBMDIR, 7);
    securec_check(rc, "\0", "\0");

    pathlen = strlen(t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome) + MAX_CBM_FILENAME_LENGTH;
    if (pathlen >= MAXPGPATH)
        ereport(FATAL, (errmsg("Length of absolute CBM file path would exceed MAXPGPATH!")));
}

static void ResetXlogCbmSys(void)
{
    int rc = 0;

    if (t_thrd.cbm_cxt.XlogCbmSys->out.fd >= 0) {
        if (pg_fsync(t_thrd.cbm_cxt.XlogCbmSys->out.fd) != 0)
            ereport(WARNING,
                (errcode_for_file_access(),
                    errmsg("fsync pending CBM file \"%s\" failed during reset", t_thrd.cbm_cxt.XlogCbmSys->out.name)));

        if (close(t_thrd.cbm_cxt.XlogCbmSys->out.fd) != 0)
            ereport(WARNING,
                (errcode_for_file_access(),
                    errmsg("close pending CBM file \"%s\" failed during reset", t_thrd.cbm_cxt.XlogCbmSys->out.name)));
    }
    rc = memset_s(t_thrd.cbm_cxt.XlogCbmSys->out.name, MAXPGPATH, 0, MAXPGPATH);
    securec_check(rc, "\0", "\0");
    t_thrd.cbm_cxt.XlogCbmSys->out.fd = -1;
    t_thrd.cbm_cxt.XlogCbmSys->out.size = 0;
    t_thrd.cbm_cxt.XlogCbmSys->out.offset = (off_t)0;

    t_thrd.cbm_cxt.XlogCbmSys->outSeqNum = 0;
    t_thrd.cbm_cxt.XlogCbmSys->startLSN = 0;
    t_thrd.cbm_cxt.XlogCbmSys->endLSN = 0;

    t_thrd.cbm_cxt.XlogCbmSys->cbmPageHash = NULL;
    DLInitList(&t_thrd.cbm_cxt.XlogCbmSys->pageFreeList);
    t_thrd.cbm_cxt.XlogCbmSys->totalPageNum = 0;

    if (t_thrd.cbm_cxt.XlogCbmSys->xlogRead.fd >= 0) {
        if (close(t_thrd.cbm_cxt.XlogCbmSys->xlogRead.fd) != 0)
            ereport(WARNING,
                (errcode_for_file_access(),
                    errmsg("could not close file \"%s\" during reset", t_thrd.cbm_cxt.XlogCbmSys->xlogRead.filePath)));
    }
    t_thrd.cbm_cxt.XlogCbmSys->xlogRead.fd = -1;
    t_thrd.cbm_cxt.XlogCbmSys->xlogRead.logSegNo = 0;
    rc = memset_s(t_thrd.cbm_cxt.XlogCbmSys->xlogRead.filePath, MAXPGPATH, 0, MAXPGPATH);
    securec_check(rc, "\0", "\0");
}

extern void CBMTrackInit(bool startupXlog, XLogRecPtr startupCPRedo)
{
    bool fromScratch = false;
    DIR* cbmdir = NULL;
    struct dirent* cbmde = NULL;
    struct stat st;
    char lastfileName[MAXPGPATH] = {0};
    uint64 lastfileSize = 0;
    uint64 lastfileSeqNum = 0;
    XLogRecPtr lastfileStartLSN = InvalidXLogRecPtr;
    XLogRecPtr lastfileEndLSN = InvalidXLogRecPtr;
    XLogRecPtr lastfileTrackedLSN = InvalidXLogRecPtr;
    XLogRecPtr trackStartLSN = InvalidXLogRecPtr;
    int rc;

    cbmdir = AllocateDir(t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome);
    if (cbmdir == NULL) {
        ereport(WARNING,
            (errcode_for_file_access(),
                errmsg("could not open CBM file directory \"%s\": %m", t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome)));
        ereport(LOG,
            (errmsg("This maybe the first time CBM tracking is enabled after "
                    "db installation or CBM track reset.")));
        /* Remove any broken directory with same name. */
        if (stat(t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome, &st) == 0 && S_ISDIR(st.st_mode))
            (void)rmtree(t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome, true, true);
        /* Create and fsync CBM file directory. */
        if (mkdir(t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome, S_IRWXU) < 0)
            ereport(ERROR,
                (errcode_for_file_access(),
                    errmsg("could not create directory \"%s\": %m", t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome)));
        fsync_fname(t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome, true);
        fromScratch = true;
    } else {
        uint64 fileSeqNum = 0;
        XLogRecPtr fileStartLSN = InvalidXLogRecPtr;
        XLogRecPtr fileEndLSN = InvalidXLogRecPtr;

        while ((cbmde = ReadDir(cbmdir, t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome)) != NULL) {
            /* Ignore files that are not CBM files. */
            if (!IsCBMFile(cbmde->d_name, &fileSeqNum, &fileStartLSN, &fileEndLSN))
                continue;

            Assert(!XLogRecPtrIsInvalid(fileStartLSN));
            Assert(XLByteLT(fileStartLSN, fileEndLSN) || XLogRecPtrIsInvalid(fileEndLSN));
            /* Find the last CBM file. */
            if (fileSeqNum > lastfileSeqNum) {
                lastfileSeqNum = fileSeqNum;
                lastfileStartLSN = fileStartLSN;
                lastfileEndLSN = fileEndLSN;
                rc = strncpy_s(lastfileName, MAXPGPATH, cbmde->d_name, strlen(cbmde->d_name));
                securec_check(rc, "\0", "\0");
            }
        }
        (void)FreeDir(cbmdir);

        if (lastfileSeqNum == 0)
            fromScratch = true;
        else
            ereport(LOG,
                (errmsg("last CBM file name \"%s\", seqnum %lu, start LSN %08X/%08X, "
                        "end LSN %08X/%08X",
                    lastfileName,
                    lastfileSeqNum,
                    (uint32)(lastfileStartLSN >> 32),
                    (uint32)lastfileStartLSN,
                    (uint32)(lastfileEndLSN >> 32),
                    (uint32)lastfileEndLSN)));
    }

    if (!fromScratch) {
        ValidateCBMFile(lastfileName, &lastfileTrackedLSN, &lastfileSize, true);

        ereport(LOG,
            (errmsg("last CBM file name \"%s\", lastfileSize %lu, tracked LSN %08X/%08X",
                lastfileName,
                lastfileSize,
                (uint32)(lastfileTrackedLSN >> 32),
                (uint32)lastfileTrackedLSN)));

        if (XLogRecPtrIsInvalid(lastfileTrackedLSN))
            lastfileTrackedLSN = lastfileStartLSN;
        else
            Assert(XLByteLT(lastfileStartLSN, lastfileTrackedLSN));
    }

    trackStartLSN = InitCBMTrackStartLSN(startupXlog, fromScratch, lastfileTrackedLSN, startupCPRedo);

    InitCBMStartFileAndTrack(
        fromScratch, trackStartLSN, lastfileName, lastfileStartLSN, lastfileTrackedLSN, lastfileSize, lastfileSeqNum);
}

static bool IsCBMFile(const char* fileName, uint64* seqNum, XLogRecPtr* startLSN, XLogRecPtr* endLSN)
{
    char stem[MAXPGPATH];

    Assert(strlen(fileName) < MAXPGPATH);

    int rc = 0;
    uint32 startLSNHi = 0;
    uint32 startLSNLo = 0;
    uint32 endLSNHi = 0;
    uint32 endLSNLo = 0;

    rc = sscanf_s(fileName,
        "%[a-z_]%lu_%8X%8X_%8X%8X.cbm",
        stem,
        MAXPGPATH - 1,
        seqNum,
        &startLSNHi,
        &startLSNLo,
        &endLSNHi,
        &endLSNLo);
    *startLSN = (((uint64)startLSNHi) << 32) | startLSNLo;
    *endLSN = (((uint64)endLSNHi) << 32) | endLSNLo;

    ereport(DEBUG1,
        (errmsg("file name \"%s\", rc %d, stem \"%s\", seqnum %lu, "
                "startLSN: %08X/%08X, endLSN: %08X/%08X",
            fileName,
            rc,
            stem,
            *seqNum,
            (uint32)(*startLSN >> 32),
            (uint32)(*startLSN),
            (uint32)(*endLSN >> 32),
            (uint32)(*endLSN))));

    return ((rc == 6) && (!strncmp(stem, cbmFileNameStem, cbmFileNameStemLen)));
}

static void ValidateCBMFile(const char* filename, XLogRecPtr* trackedLSN, uint64* lastfileSize, bool truncErrPage)
{
    struct stat st;
    char* page = NULL;
    char filePath[MAXPGPATH];
    bool isLastPage = false;
    bool checksumOk = false;
    off_t readOffset = 0;
    BitmapFile cbmFile;
    int rc;

    rc = snprintf_s(filePath, MAXPGPATH, MAXPGPATH - 1, "%s%s", t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome, filename);
    securec_check_ss(rc, "\0", "\0");

    if (stat(filePath, &st) != 0) {
        *trackedLSN = InvalidXLogRecPtr;
        *lastfileSize = 0;
        ereport(LOG, (errmsg("could not stat CBM file \"%s\" ", filePath)));
        return;
    }

    if (st.st_size % (off_t)CBMPAGESIZE)
        ereport(WARNING,
            (errmsg("size(%ld) of CBM file \"%s\" is not a multiple of CBMPAGESIZE, "
                    "which may imply file corruption.",
                st.st_size,
                filePath)));

    cbmFile.fd = BasicOpenFile(filePath, O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
    if (cbmFile.fd < 0) {
        *trackedLSN = InvalidXLogRecPtr;
        *lastfileSize = 0;
        ereport(LOG,
            (errcode_for_file_access(), errmsg("could not open CBM file \"%s\" while invalidation: %m", filePath)));
        return;
    }

    cbmFile.size = readOffset = st.st_size - st.st_size % (off_t)CBMPAGESIZE;
    rc = strncpy_s(cbmFile.name, MAXPGPATH, filePath, strlen(filePath));
    securec_check(rc, "\0", "\0");

    page = (char*)palloc_extended(CBMPAGESIZE, MCXT_ALLOC_NO_OOM);
    if (page == NULL) {
        if (close(cbmFile.fd))
            ereport(WARNING, (errcode_for_file_access(), errmsg("could not close CBM file \"%s\": %m", filePath)));

        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                errmsg("memory is temporarily unavailable while allocate "
                       "page read buffer during validate CBM file")));
    }

    while (readOffset > (off_t)0 && (!isLastPage || !checksumOk)) {
        readOffset -= (off_t)CBMPAGESIZE;
        cbmFile.offset = readOffset;

        if (!ReadCBMPage(&cbmFile, page, &checksumOk)) {
            checksumOk = false;
            break;
        }

        if (checksumOk)
            isLastPage = ((cbmpageheader*)page)->isLastBlock;
        else
            ereport(WARNING,
                (errmsg("Corruption detected in CBM file \"%s\", page offset %ld",
                    filePath,
                    cbmFile.offset - (off_t)CBMPAGESIZE)));
    }

    *trackedLSN = (checksumOk && isLastPage) ? ((cbmpageheader*)page)->pageEndLsn : InvalidXLogRecPtr;

    if (XLogRecPtrIsInvalid(*trackedLSN))
        *lastfileSize = (off_t)0;
    else {
        if (cbmFile.offset < st.st_size && truncErrPage) {
            if (ftruncate(cbmFile.fd, cbmFile.offset))
                ereport(ERROR,
                    (errcode_for_file_access(),
                        errmsg("Failed to truncate CBM file \"%s\" to length %ld", filePath, cbmFile.offset)));
        }

        *lastfileSize = cbmFile.offset;
    }

    if (close(cbmFile.fd))
        ereport(WARNING, (errcode_for_file_access(), errmsg("could not close CBM file \"%s\": %m", filePath)));

    pfree(page);
}

static bool ReadCBMPage(BitmapFile* cbmFile, char* page, bool* checksumOk)
{
    pg_crc32c checksum;
    pg_crc32c actualChecksum;
    ssize_t readLen;

    Assert(cbmFile->size >= CBMPAGESIZE);
    Assert(cbmFile->offset <= (off_t)(cbmFile->size - CBMPAGESIZE));
    Assert(cbmFile->offset % (off_t)CBMPAGESIZE == (off_t)0);

    PGSTAT_INIT_TIME_RECORD();
    PGSTAT_START_TIME_RECORD();
    readLen = pread(cbmFile->fd, page, (size_t)CBMPAGESIZE, cbmFile->offset);
    PGSTAT_END_TIME_RECORD(DATA_IO_TIME);

    if (readLen != (ssize_t)CBMPAGESIZE) {
        ereport(WARNING, (errmsg("failed reading CBM file \"%s\", page offset %ld", cbmFile->name, cbmFile->offset)));
        return false;
    }

    cbmFile->offset += (off_t)CBMPAGESIZE;
    Assert(cbmFile->offset <= (off_t)cbmFile->size);

    checksum = ((cbmpageheader*)page)->pageCrc;
    actualChecksum = CBMPageCalcCRC(page);
    *checksumOk = (checksum == actualChecksum);

    return true;
}

static pg_crc32c CBMPageCalcCRC(const char* page)
{
    pg_crc32c crc;
    INIT_CRC32C(crc);
    COMP_CRC32C(crc, page + offsetof(CbmPageHeader, isLastBlock), CBMPAGESIZE - offsetof(CbmPageHeader, isLastBlock));
    FIN_CRC32C(crc);
    return crc;
}

static XLogRecPtr InitCBMTrackStartLSN(
    bool startupXlog, bool fromScratch, XLogRecPtr lastTrackedLSN, XLogRecPtr startupCPRedo)
{
    XLogRecPtr trackStartLSN = InvalidXLogRecPtr;

    if (startupXlog) {
        Assert(!t_thrd.cbm_cxt.XlogCbmSys->xlogParseFailed);
        if (fromScratch && u_sess->attr.attr_storage.enable_cbm_tracking) {
            Assert(XLogRecPtrIsInvalid(lastTrackedLSN));
            Assert(!XLogRecPtrIsInvalid(startupCPRedo));
            trackStartLSN = startupCPRedo;
        } else
            trackStartLSN = lastTrackedLSN;

        SetCBMTrackedLSN(trackStartLSN);
    } else {
        (void)LWLockAcquire(ControlFileLock, LW_SHARED);

        if (fromScratch) {
            Assert(XLogRecPtrIsInvalid(lastTrackedLSN));
            Assert(XLogRecPtrIsInvalid(startupCPRedo));
            trackStartLSN = t_thrd.shemem_ptr_cxt.ControlFile->checkPointCopy.redo;
        } else {
            trackStartLSN = (t_thrd.cbm_cxt.XlogCbmSys->xlogParseFailed &&
                                XLByteLT(lastTrackedLSN, t_thrd.shemem_ptr_cxt.ControlFile->checkPointCopy.redo))
                                ? t_thrd.shemem_ptr_cxt.ControlFile->checkPointCopy.redo
                                : lastTrackedLSN;
            if (XLByteLT(lastTrackedLSN, trackStartLSN))
                ereport(WARNING,
                    (errmsg("Last tracked LSN %08X/%08X is smaller than CBM track start "
                            "LSN %08X/%08X. This may be caused by CBM file or xlog file corruption",
                        (uint32)(lastTrackedLSN >> 32),
                        (uint32)lastTrackedLSN,
                        (uint32)(trackStartLSN >> 32),
                        (uint32)trackStartLSN)));
        }

        SetCBMTrackedLSN(trackStartLSN);
        LWLockRelease(ControlFileLock);
    }

    Assert(!XLogRecPtrIsInvalid(trackStartLSN));
    return trackStartLSN;
}

static void InitCBMStartFileAndTrack(bool fromScratch, XLogRecPtr trackStartLSN, const char* lastfileName,
    XLogRecPtr lastfileStartLSN, XLogRecPtr lastfileTrackedLSN, uint64 lastfileSize, uint64 lastfileSeqNum)
{
    int rc;
    volatile bool switchFile = false;
    char lastfilePath[MAXPGPATH];
    char filePath[MAXPGPATH];

    if (lastfileSize >= MAXCBMFILESIZE || (lastfileSize != 0 && XLByteLT(lastfileTrackedLSN, trackStartLSN)))
        switchFile = true;

    t_thrd.cbm_cxt.XlogCbmSys->outSeqNum = fromScratch ? 1 : (switchFile ? (lastfileSeqNum + 1) : lastfileSeqNum);
    t_thrd.cbm_cxt.XlogCbmSys->startLSN = t_thrd.cbm_cxt.XlogCbmSys->endLSN = trackStartLSN;

    if (!fromScratch) {
        rc = snprintf_s(
            lastfilePath, MAXPGPATH, MAXPGPATH - 1, "%s%s", t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome, lastfileName);
        securec_check_ss(rc, "\0", "\0");

        if (switchFile) {
            Assert(XLByteLT(lastfileStartLSN, lastfileTrackedLSN));
            SetCBMFileName(filePath, lastfileSeqNum, lastfileStartLSN, lastfileTrackedLSN);

            if (durable_rename(lastfilePath, filePath, ERROR))
                ereport(ERROR,
                    (errcode_for_file_access(),
                        errmsg("could not rename file \"%s\" to \"%s\": %m", lastfilePath, filePath)));
        } else if (lastfileSize == 0) {
            switchFile = true;
            rc = unlink(lastfilePath);
            if (rc < 0 && errno != ENOENT)
                ereport(ERROR, (errcode_for_file_access(), errmsg("could not remove file \"%s\": %m", lastfilePath)));
        } else {
            SetNewCBMFileName(lastfileStartLSN);

            if (durable_rename(lastfilePath, t_thrd.cbm_cxt.XlogCbmSys->out.name, ERROR))
                ereport(ERROR,
                    (errcode_for_file_access(),
                        errmsg("could not rename file \"%s\" to \"%s\": %m",
                            lastfilePath,
                            t_thrd.cbm_cxt.XlogCbmSys->out.name)));
        }
    }

    if (fromScratch || switchFile)
        StartNextCBMFile(trackStartLSN);
    else
        StartExistCBMFile(lastfileSize);
}

static void SetCBMFileName(char* cbmFileNameBuf, uint64 seqNum, XLogRecPtr startLSN, XLogRecPtr endLSN)
{
    int rc;

    rc = snprintf_s(cbmFileNameBuf,
        MAXPGPATH,
        MAXPGPATH - 1,
        bmpFileNameTemplate,
        t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome,
        cbmFileNameStem,
        seqNum,
        (uint32)(startLSN >> 32),
        (uint32)startLSN,
        (uint32)(endLSN >> 32),
        (uint32)endLSN);
    securec_check_ss(rc, "\0", "\0");
}

static void SetNewCBMFileName(XLogRecPtr startLSN)
{
    SetCBMFileName(
        t_thrd.cbm_cxt.XlogCbmSys->out.name, t_thrd.cbm_cxt.XlogCbmSys->outSeqNum, startLSN, InvalidXLogRecPtr);
}

static void StartNextCBMFile(XLogRecPtr startLSN)
{
    SetNewCBMFileName(startLSN);

    int fd =
        BasicOpenFile(t_thrd.cbm_cxt.XlogCbmSys->out.name, O_RDWR | O_CREAT | O_EXCL | PG_BINARY, S_IRUSR | S_IWUSR);
    if (fd < 0)
        ereport(ERROR,
            (errcode_for_file_access(),
                errmsg("could not create new CBM file \"%s\": %m", t_thrd.cbm_cxt.XlogCbmSys->out.name)));

    t_thrd.cbm_cxt.XlogCbmSys->out.fd = fd;
    t_thrd.cbm_cxt.XlogCbmSys->out.size = 0;
    t_thrd.cbm_cxt.XlogCbmSys->out.offset = (off_t)0;
}

static void StartExistCBMFile(uint64 lastfileSize)
{
    int fd = BasicOpenFile(t_thrd.cbm_cxt.XlogCbmSys->out.name, O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
    if (fd < 0)
        ereport(ERROR,
            (errcode_for_file_access(),
                errmsg("could not open CBM file \"%s\": %m", t_thrd.cbm_cxt.XlogCbmSys->out.name)));

    t_thrd.cbm_cxt.XlogCbmSys->out.fd = fd;
    t_thrd.cbm_cxt.XlogCbmSys->out.size = lastfileSize;
    t_thrd.cbm_cxt.XlogCbmSys->out.offset = (off_t)lastfileSize;
}

static HTAB* CBMPageHashInitialize(MemoryContext memoryContext)
{
    HASHCTL ctl;
    HTAB* hTab = NULL;

    /*
     * create hashtable that indexes the CBM pages
     */
    errno_t rc = memset_s(&ctl, sizeof(ctl), 0, sizeof(ctl));
    securec_check(rc, "\0", "\0");
    ctl.hcxt = memoryContext;
    ctl.keysize = sizeof(CBMPageTag);
    ctl.entrysize = sizeof(CbmHashEntry);
    ctl.hash = tag_hash;
    hTab = hash_create("CBM page hash by relfilenode and forknum",
        INITCBMPAGEHASHSIZE,
        &ctl,
        HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);

    return hTab;
}

extern void CBMFollowXlog(void)
{
    (void)LWLockAcquire(CBMParseXlogLock, LW_EXCLUSIVE);
    if (t_thrd.cbm_cxt.XlogCbmSys->needReset) {
        /* Flush any leaked data in the top-level context */
        MemoryContextResetAndDeleteChildren(t_thrd.cbm_cxt.cbmwriter_context);
        MemoryContextResetAndDeleteChildren(t_thrd.cbm_cxt.cbmwriter_page_context);

        ResetXlogCbmSys();
        CBMTrackInit(false, InvalidXLogRecPtr);
        t_thrd.cbm_cxt.XlogCbmSys->xlogParseFailed = false;
    } else {
        t_thrd.cbm_cxt.XlogCbmSys->needReset = true;

        struct stat statbuf;
        if (lstat(t_thrd.cbm_cxt.XlogCbmSys->out.name, &statbuf) != 0)
            ereport(ERROR,
                (errcode_for_file_access(),
                    errmsg("failed to stat current cbm file %s :%m", t_thrd.cbm_cxt.XlogCbmSys->out.name)));
    }

    if (t_thrd.cbm_cxt.XlogCbmSys->cbmPageHash == NULL)
        t_thrd.cbm_cxt.XlogCbmSys->cbmPageHash = CBMPageHashInitialize(t_thrd.cbm_cxt.cbmwriter_page_context);

    TimeLineID timeLine;
    XLogRecPtr checkPointRedo;
    XLogRecPtr tmpEndLSN;
    XLogRecPtr tmpForceEnd;
    bool isRecEnd = false;
    (void)LWLockAcquire(ControlFileLock, LW_SHARED);
    checkPointRedo = t_thrd.shemem_ptr_cxt.ControlFile->checkPointCopy.redo;
    timeLine = t_thrd.shemem_ptr_cxt.ControlFile->checkPointCopy.ThisTimeLineID;
    LWLockRelease(ControlFileLock);

    tmpEndLSN = checkPointRedo;
    tmpForceEnd = GetTmpTargetLSN(&isRecEnd);
    if (!XLogRecPtrIsInvalid(tmpForceEnd)) {
        if (XLByteLT(t_thrd.cbm_cxt.XlogCbmSys->startLSN, tmpForceEnd))
            tmpEndLSN = tmpForceEnd;
        else {
            tmpForceEnd = InvalidXLogRecPtr;
            isRecEnd = true;
            SetTmpTargetLSN(InvalidXLogRecPtr, true);
        }
    } else
        isRecEnd = true;

    if (XLByteLT(tmpEndLSN, t_thrd.cbm_cxt.XlogCbmSys->startLSN)) {
        if (XLByteEQ(t_thrd.cbm_cxt.XlogCbmSys->startLSN, GetLatestCompTargetLSN())) {
            Assert(XLByteEQ(tmpEndLSN, checkPointRedo));
            ereport(LOG,
                (errmsg("The xlog LSN to be parsed %08X/%08X is smaller than "
                        "already tracked xlog LSN %08X/%08X, due to previous force "
                        "CBM track. Skip CBM track this time",
                    (uint32)(tmpEndLSN >> 32),
                    (uint32)tmpEndLSN,
                    (uint32)(t_thrd.cbm_cxt.XlogCbmSys->startLSN >> 32),
                    (uint32)t_thrd.cbm_cxt.XlogCbmSys->startLSN)));
            t_thrd.cbm_cxt.XlogCbmSys->needReset = false;
            LWLockRelease(CBMParseXlogLock);
            return;
        } else if (!t_thrd.cbm_cxt.XlogCbmSys->firstCPCreated) {
            ereport(LOG,
                (errmsg("The xlog LSN to be parsed %08X/%08X is smaller than "
                        "already tracked xlog LSN %08X/%08X. This may be caused by "
                        "crush recovery or switchover/failover, before the first checkpoint "
                        "following recovery has been created. Usually you can ignore this "
                        "message. But if you have manually modified xlog, to be safe, "
                        "please check if xlog records are consistent and uncorrupted",
                    (uint32)(tmpEndLSN >> 32),
                    (uint32)tmpEndLSN,
                    (uint32)(t_thrd.cbm_cxt.XlogCbmSys->startLSN >> 32),
                    (uint32)t_thrd.cbm_cxt.XlogCbmSys->startLSN)));
            t_thrd.cbm_cxt.XlogCbmSys->needReset = false;
            LWLockRelease(CBMParseXlogLock);
            return;
        } else {
            RemoveAllCBMFiles(PANIC);
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("The xlog LSN to be parsed %08X/%08X is smaller than "
                           "already tracked xlog LSN %08X/%08X. This may be caused by "
                           "xlog truncation (pg_rewind), xlog corruption or PITR (at present "
                           "CBM does not support multiple timelines). Under these scenarios, "
                           "inconsistent CBM files may be created. "
                           "To be safe, we zap all existing CBM files and restart CBM tracking",
                        (uint32)(tmpEndLSN >> 32),
                        (uint32)tmpEndLSN,
                        (uint32)(t_thrd.cbm_cxt.XlogCbmSys->startLSN >> 32),
                        (uint32)t_thrd.cbm_cxt.XlogCbmSys->startLSN)));
        }
    } else if (XLByteEQ(tmpEndLSN, t_thrd.cbm_cxt.XlogCbmSys->startLSN)) {
        ereport(LOG,
            (errmsg("The xlog LSN to be parsed %08X/%08X is equal to "
                    "already tracked xlog LSN %08X/%08X. Skip CBM track this time",
                (uint32)(tmpEndLSN >> 32),
                (uint32)tmpEndLSN,
                (uint32)(t_thrd.cbm_cxt.XlogCbmSys->startLSN >> 32),
                (uint32)t_thrd.cbm_cxt.XlogCbmSys->startLSN)));
        t_thrd.cbm_cxt.XlogCbmSys->needReset = false;
        LWLockRelease(CBMParseXlogLock);
        return;
    } else
        ereport(LOG,
            (errmsg("The xlog LSN to be parsed %08X/%08X is larger than "
                    "already tracked xlog LSN %08X/%08X. Do CBM track one time",
                (uint32)(tmpEndLSN >> 32),
                (uint32)tmpEndLSN,
                (uint32)(t_thrd.cbm_cxt.XlogCbmSys->startLSN >> 32),
                (uint32)t_thrd.cbm_cxt.XlogCbmSys->startLSN)));

    t_thrd.cbm_cxt.XlogCbmSys->endLSN = tmpEndLSN;

    if (ParseXlogIntoCBMPages(timeLine, isRecEnd)) {
        ereport(LOG,
            (errmsg("Found no any valid xlog record From the already tracked xlog "
                    "LSN %08X/%08X. Skip CBM track this time",
                (uint32)(t_thrd.cbm_cxt.XlogCbmSys->startLSN >> 32),
                (uint32)t_thrd.cbm_cxt.XlogCbmSys->startLSN)));

        t_thrd.cbm_cxt.XlogCbmSys->needReset = false;
        LWLockRelease(CBMParseXlogLock);
        return;
    }

    if (t_thrd.cbm_cxt.XlogCbmSys->totalPageNum == 0)
        CreateDummyCBMEtyPageAndInsert();

    PrintCBMHashTab(t_thrd.cbm_cxt.XlogCbmSys->cbmPageHash);

    FlushCBMPagesToDisk(t_thrd.cbm_cxt.XlogCbmSys, true);

    t_thrd.cbm_cxt.XlogCbmSys->startLSN = t_thrd.cbm_cxt.XlogCbmSys->endLSN;
    SetCBMTrackedLSN(t_thrd.cbm_cxt.XlogCbmSys->startLSN);

    if (!XLogRecPtrIsInvalid(tmpForceEnd)) {
        Assert(XLByteLT(GetLatestCompTargetLSN(), tmpForceEnd));
        if (isRecEnd) {
            Assert(XLByteEQ(tmpForceEnd, t_thrd.cbm_cxt.XlogCbmSys->startLSN));
        }
        /*
         * we use XlogCbmSys->startLSN to mark LatestCompTargetLSN,
         * because we COULD NOT GUARANTEE that GetTmpTargetLSN() return
         * a available xlog record end if isRecEnd is false.
         */
        SetLatestCompTargetLSN(t_thrd.cbm_cxt.XlogCbmSys->startLSN);
        SetTmpTargetLSN(InvalidXLogRecPtr, true);
    }

    if (DLListLength(&t_thrd.cbm_cxt.XlogCbmSys->pageFreeList) > MAXCBMFREEPAGENUM) {
        MemoryContextResetAndDeleteChildren(t_thrd.cbm_cxt.cbmwriter_page_context);
        DLInitList(&t_thrd.cbm_cxt.XlogCbmSys->pageFreeList);
        t_thrd.cbm_cxt.XlogCbmSys->cbmPageHash = NULL;
    }

    t_thrd.cbm_cxt.XlogCbmSys->needReset = false;
    LWLockRelease(CBMParseXlogLock);
}

static bool ParseXlogIntoCBMPages(TimeLineID timeLine, bool isRecEnd)
{
    XLogRecord* record = NULL;
    XLogReaderState* xlogreader = NULL;
    char* errormsg = NULL;
    XLogPageReadPrivateCBM readprivate;
    XLogRecPtr startPoint = t_thrd.cbm_cxt.XlogCbmSys->startLSN;
    bool parseSkip = false;

    readprivate.datadir = t_thrd.proc_cxt.DataDir;
    readprivate.tli = timeLine;
    xlogreader = XLogReaderAllocate(&CBMXLogPageRead, &readprivate);
    if (xlogreader == NULL)
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                errmsg("memory is temporarily unavailable while allocate xlog reader")));

    do {
        record = XLogReadRecord(xlogreader, startPoint, &errormsg);
        if (record == NULL) {
            XLogRecPtr errptr;

            if (XLByteEQ(startPoint, InvalidXLogRecPtr))
                errptr = xlogreader->EndRecPtr;
            else
                errptr = startPoint;

            if (!isRecEnd) {
                if (errormsg != NULL)
                    ereport(WARNING,
                        (errmsg("could not read WAL record at %08X/%08X: %s",
                            (uint32)(errptr >> 32),
                            (uint32)errptr,
                            errormsg)));
                else
                    ereport(WARNING,
                        (errmsg("could not read WAL record at %08X/%08X", (uint32)(errptr >> 32), (uint32)errptr)));

                if (XLByteEQ(startPoint, InvalidXLogRecPtr)) {
                    ereport(LOG,
                        (errmsg("reach CBM parse end. The next xlog record starts at %08X/%08X",
                            (uint32)(xlogreader->EndRecPtr >> 32),
                            (uint32)xlogreader->EndRecPtr)));

                    t_thrd.cbm_cxt.XlogCbmSys->endLSN = xlogreader->EndRecPtr;
                    parseSkip = false;
                } else
                    parseSkip = true;

                break;
            } else {
                t_thrd.cbm_cxt.XlogCbmSys->xlogParseFailed = true;

                if (errormsg != NULL)
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_EXCEPTION),
                            errmsg("could not read WAL record at %08X/%08X: %s",
                                (uint32)(errptr >> 32),
                                (uint32)errptr,
                                errormsg)));
                else
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_EXCEPTION),
                            errmsg("could not read WAL record at %08X/%08X", (uint32)(errptr >> 32), (uint32)errptr)));
            }
        }

        TrackChangeBlock(xlogreader);

        advanceXlogPtrToNextPageIfNeeded(&(xlogreader->EndRecPtr));

        if (XLByteLE(t_thrd.cbm_cxt.XlogCbmSys->endLSN, xlogreader->EndRecPtr)) {
            ereport(LOG,
                (errmsg("reach CBM parse end. The next xlog record starts at %08X/%08X",
                    (uint32)(xlogreader->EndRecPtr >> 32),
                    (uint32)xlogreader->EndRecPtr)));

            /*
             * Force the coming startLSN be set to a valid xlog record start LSN
             * if this is a forced cbm track with a non-record-end stop position.
             */
            if (!isRecEnd)
                t_thrd.cbm_cxt.XlogCbmSys->endLSN = xlogreader->EndRecPtr;

            parseSkip = false;
            break;
        }

        startPoint = InvalidXLogRecPtr; /* continue reading at next record */
    } while (true);

    XLogReaderFree(xlogreader);
    if (t_thrd.cbm_cxt.XlogCbmSys->xlogRead.fd != -1) {
        if (close(t_thrd.cbm_cxt.XlogCbmSys->xlogRead.fd) != 0)
            ereport(WARNING,
                (errcode_for_file_access(),
                    errmsg("could not close file \"%s\" ", t_thrd.cbm_cxt.XlogCbmSys->xlogRead.filePath)));

        t_thrd.cbm_cxt.XlogCbmSys->xlogRead.fd = -1;
    }

    return parseSkip;
}

/* XLogreader callback function, to read a WAL page */
static int CBMXLogPageRead(XLogReaderState* xlogreader, XLogRecPtr targetPagePtr, int reqLen, XLogRecPtr targetRecPtr,
    char* readBuf, TimeLineID* pageTLI)
{
    XLogPageReadPrivateCBM* readprivate = (XLogPageReadPrivateCBM*)xlogreader->private_data;
    uint32 targetPageOff;
    int rc = 0;

    targetPageOff = targetPagePtr % XLogSegSize;

    /*
     * See if we need to switch to a new segment because the requested record
     * is not in the currently open one.
     */
    if (t_thrd.cbm_cxt.XlogCbmSys->xlogRead.fd >= 0 &&
        !XLByteInSeg(targetPagePtr, t_thrd.cbm_cxt.XlogCbmSys->xlogRead.logSegNo)) {
        if (close(t_thrd.cbm_cxt.XlogCbmSys->xlogRead.fd) != 0)
            ereport(WARNING,
                (errcode_for_file_access(),
                    errmsg("could not close file \"%s\" ", t_thrd.cbm_cxt.XlogCbmSys->xlogRead.filePath)));

        t_thrd.cbm_cxt.XlogCbmSys->xlogRead.fd = -1;
    }

    XLByteToSeg(targetPagePtr, t_thrd.cbm_cxt.XlogCbmSys->xlogRead.logSegNo);

    if (t_thrd.cbm_cxt.XlogCbmSys->xlogRead.fd < 0) {
        char xlogfname[MAXFNAMELEN];

        rc = snprintf_s(xlogfname,
            MAXFNAMELEN,
            MAXFNAMELEN - 1,
            "%08X%08X%08X",
            readprivate->tli,
            (uint32)((t_thrd.cbm_cxt.XlogCbmSys->xlogRead.logSegNo) / XLogSegmentsPerXLogId),
            (uint32)((t_thrd.cbm_cxt.XlogCbmSys->xlogRead.logSegNo) % XLogSegmentsPerXLogId));
        securec_check_ss(rc, "", "");

        rc = snprintf_s(t_thrd.cbm_cxt.XlogCbmSys->xlogRead.filePath,
            MAXPGPATH,
            MAXPGPATH - 1,
            "%s/" XLOGDIR "/%s",
            readprivate->datadir,
            xlogfname);
        securec_check_ss(rc, "\0", "\0");

        t_thrd.cbm_cxt.XlogCbmSys->xlogRead.fd =
            BasicOpenFile(t_thrd.cbm_cxt.XlogCbmSys->xlogRead.filePath, O_RDONLY | PG_BINARY, 0);

        if (t_thrd.cbm_cxt.XlogCbmSys->xlogRead.fd < 0) {
            ereport(WARNING,
                (errmsg(
                    "could not open file \"%s\": %s", t_thrd.cbm_cxt.XlogCbmSys->xlogRead.filePath, strerror(errno))));
            return -1;
        }
    }

    /*
     * At this point, we have the right segment open.
     */
    Assert(t_thrd.cbm_cxt.XlogCbmSys->xlogRead.fd != -1);

    PGSTAT_INIT_TIME_RECORD();
    PGSTAT_START_TIME_RECORD();
    if (pread(t_thrd.cbm_cxt.XlogCbmSys->xlogRead.fd, readBuf, XLOG_BLCKSZ, (off_t)targetPageOff) != XLOG_BLCKSZ) {
        PGSTAT_END_TIME_RECORD(DATA_IO_TIME);
        ereport(WARNING,
            (errmsg("could not read page from file \"%s\" at page offset %u: %s",
                t_thrd.cbm_cxt.XlogCbmSys->xlogRead.filePath,
                targetPageOff,
                strerror(errno))));
        return -1;
    }
    PGSTAT_END_TIME_RECORD(DATA_IO_TIME);

    *pageTLI = readprivate->tli;
    return XLOG_BLCKSZ;
}

/*
 * Extract information on which blocks the current record modifies.
 */
static void TrackChangeBlock(XLogReaderState* record)
{
    XLogRecPtr lsn = record->EndRecPtr;
    XLogRecPtr prev = XLogRecGetPrev(record);

    Assert(record != NULL);

    ereport(DEBUG1,
        (errmsg("extract WAL: cur: %X/%X; prev %X/%X; xid %lu; "
                "len/total_len %u/%u; info %u; rmid %u",
            (uint32)(lsn >> 32),
            (uint32)lsn,
            (uint32)(prev >> 32),
            (uint32)prev,
            XLogRecGetXid(record),
            XLogRecGetDataLen(record),
            XLogRecGetTotalLen(record),
            (uint32)XLogRecGetInfo(record),
            (uint32)XLogRecGetRmid(record))));

    if (XLogRecHasAnyBlockRefs(record))
        TrackRelPageModification(record);
    else if (XLogRecGetRmid(record) == RM_HEAP2_ID) {
        /* CU modification does not have block ref */
        Assert(!XLogRecHasAnyBlockRefs(record));
        TrackCuBlockModification(record);
    }

    /* following xlog record rm catagories do not have block ref */
    if (XLogRecGetRmid(record) == RM_XACT_ID) {
        Assert(!XLogRecHasAnyBlockRefs(record));
        TrackRelStorageDrop(record);
    } else if (XLogRecGetRmid(record) == RM_SMGR_ID) {
        Assert(!XLogRecHasAnyBlockRefs(record));
        uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
        if (info == XLOG_SMGR_CREATE)
            TrackRelStorageCreate(record);
        else if (info == XLOG_SMGR_TRUNCATE)
            TrackRelStorageTruncate(record);
    } else if (XLogRecGetRmid(record) == RM_DBASE_ID) {
        Assert(!XLogRecHasAnyBlockRefs(record));
        TrackDbStorageChange(record);
    } else if (XLogRecGetRmid(record) == RM_TBLSPC_ID) {
        Assert(!XLogRecHasAnyBlockRefs(record));
        TrackTblspcStorageChange(record);
    } else if (XLogRecGetRmid(record) == RM_RELMAP_ID) {
        Assert(!XLogRecHasAnyBlockRefs(record));
        TrackRelmapChange(record);
    }

    /* clean of vm bit needs to be tracked seperately */
    TrackVMPageModification(record);
}

static void TrackRelPageModification(XLogReaderState* record)
{
    int block_id = 0;
    RelFileNode rNode;
    ForkNumber forkNum;
    BlockNumber blkNo;

    /* do actual block analyze */
    for (block_id = 0; block_id <= record->max_block_id; block_id++) {

        if (!XLogRecGetBlockTag(record, block_id, &rNode, &forkNum, &blkNo))
            continue;
        /*
         * At present, we do not record page modification information for dfs relations.
         */
        if (forkNum <= InvalidForkNumber)
            continue;

        /* Only main fork, fsm fork, vm fork and init fork have page ref xlog */
        Assert(forkNum == MAIN_FORKNUM || forkNum == FSM_FORKNUM || forkNum == VISIBILITYMAP_FORKNUM ||
               forkNum == INIT_FORKNUM);

        ereport(DEBUG3,
            (errmsg("block%d: rel %u/%u/%u forknum %d blkno %u",
                block_id,
                rNode.spcNode,
                rNode.dbNode,
                rNode.relNode,
                forkNum,
                blkNo)));

        RegisterBlockChange(rNode, forkNum, blkNo);
    }
}

static void TrackCuBlockModification(XLogReaderState* record)
{
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

    if ((info & XLOG_HEAP_OPMASK) == XLOG_HEAP2_LOGICAL_NEWPAGE) {
        xl_heap_logical_newpage* xlrec = (xl_heap_logical_newpage*)XLogRecGetData(record);
        BlockNumber blkNo;
        Assert(xlrec->type == COLUMN_STORE);
        Assert(xlrec->offset % ALIGNOF_CUSIZE == 0);
        Assert(xlrec->blockSize % ALIGNOF_CUSIZE == 0);

        RelFileNode tmp_node;
        RelFileNodeCopy(tmp_node, xlrec->node, XLogRecGetBucketId(record));

        for (blkNo = (xlrec->offset / ALIGNOF_CUSIZE); blkNo < ((xlrec->offset + xlrec->blockSize) / ALIGNOF_CUSIZE);
             blkNo++)
            RegisterBlockChange(tmp_node, ColumnId2ColForkNum(xlrec->attid), blkNo);
    }
}

static void TrackRelStorageDrop(XLogReaderState* record)
{
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
    int nrels = 0;
    ColFileNodeRel* xnodes = NULL;

    if (info == XLOG_XACT_COMMIT) {
        xl_xact_commit* xlrec = (xl_xact_commit*)XLogRecGetData(record);

        nrels = xlrec->nrels;
        xnodes = xlrec->xnodes;
    } else if (info == XLOG_XACT_ABORT) {
        xl_xact_abort* xlrec = (xl_xact_abort*)XLogRecGetData(record);

        nrels = xlrec->nrels;
        xnodes = xlrec->xnodes;
    } else if (info == XLOG_XACT_COMMIT_PREPARED) {
        xl_xact_commit_prepared* xlrec = (xl_xact_commit_prepared*)XLogRecGetData(record);

        nrels = xlrec->crec.nrels;
        xnodes = xlrec->crec.xnodes;
    } else if (info == XLOG_XACT_ABORT_PREPARED) {
        xl_xact_abort_prepared* xlrec = (xl_xact_abort_prepared*)XLogRecGetData(record);

        nrels = xlrec->arec.nrels;
        xnodes = xlrec->arec.xnodes;
    }

    for (int i = 0; i < nrels; ++i) {
        ColFileNode colFileNodeData;
        ColFileNodeRel* colFileNodeRel = xnodes + i;

        ColFileNodeCopy(&colFileNodeData, colFileNodeRel);

        /*
         * At present, we do not record relation drop on hdfs storage.
         */
        if (colFileNodeData.forknum <= InvalidForkNumber)
            continue;

        Assert(colFileNodeData.forknum == MAIN_FORKNUM || IsValidColForkNum(colFileNodeData.forknum));
        RegisterBlockChangeExtended(
            colFileNodeData.filenode, colFileNodeData.forknum, InvalidBlockNumber, PAGETYPE_DROP, InvalidBlockNumber);
    }
}

static void TrackRelStorageCreate(XLogReaderState* record)
{
    xl_smgr_create* xlrec = (xl_smgr_create*)XLogRecGetData(record);

    /*
     * Originally, only main fork and init fork will have storage create xlog. However,
     * alter table set tablespace also record storage create for fsm and vm fork
     * (seems unnecessary), so we have to loosen the check.
     */
    Assert(xlrec->forkNum >= MAIN_FORKNUM);

    /* At present, we do not record dfs relation information */
    if (xlrec->forkNum <= InvalidForkNumber)
        return;

    RelFileNode rnode;
    RelFileNodeCopy(rnode, xlrec->rnode, XLogRecGetBucketId(record));

    RegisterBlockChangeExtended(rnode, xlrec->forkNum, InvalidBlockNumber, PAGETYPE_CREATE, InvalidBlockNumber);
}

static void TrackRelStorageTruncate(XLogReaderState* record)
{
    xl_smgr_truncate* xlrec = (xl_smgr_truncate*)XLogRecGetData(record);
    BlockNumber mainTruncBlkNo, fsmTruncBlkNo, vmTruncBlkNo;
    RelFileNode rnode;
    RelFileNodeCopy(rnode, xlrec->rnode, XLogRecGetBucketId(record));

    mainTruncBlkNo = xlrec->blkno;
    RegisterBlockChangeExtended(rnode, MAIN_FORKNUM, InvalidBlockNumber, PAGETYPE_TRUNCATE, mainTruncBlkNo);

    fsmTruncBlkNo = FreeSpaceMapCalTruncBlkNo(mainTruncBlkNo);
    RegisterBlockChangeExtended(rnode, FSM_FORKNUM, InvalidBlockNumber, PAGETYPE_TRUNCATE, fsmTruncBlkNo);

    vmTruncBlkNo = VisibilityMapCalTruncBlkNo(mainTruncBlkNo);
    RegisterBlockChangeExtended(rnode, VISIBILITYMAP_FORKNUM, InvalidBlockNumber, PAGETYPE_TRUNCATE, vmTruncBlkNo);
}

static void TrackVMPageModification(XLogReaderState* record)
{
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
    BlockNumber heapBlkNo1 = InvalidBlockNumber;
    BlockNumber heapBlkNo2 = InvalidBlockNumber;
    BlockNumber vmBlkNo = InvalidBlockNumber;
    RelFileNode rNode = InvalidRelFileNode;
    bool isinit = ((XLogRecGetInfo(record) & XLOG_HEAP_INIT_PAGE) != 0);
    Pointer recData = (Pointer)XLogRecGetData(record);

    if (XLogRecGetRmid(record) == RM_HEAP_ID) {
        switch (info & XLOG_HEAP_OPMASK) {
            case XLOG_HEAP_INSERT: {
                if (isinit)
                    recData += sizeof(TransactionId);
                xl_heap_insert* xlrec = (xl_heap_insert*)recData;

                if (xlrec->flags & XLH_INSERT_ALL_VISIBLE_CLEARED)
                    (void)XLogRecGetBlockTag(record, 0, &rNode, NULL, &heapBlkNo1);

                break;
            }
            case XLOG_HEAP_DELETE: {
                xl_heap_delete* xlrec = (xl_heap_delete*)recData;

                if (xlrec->flags & XLH_DELETE_ALL_VISIBLE_CLEARED)
                    (void)XLogRecGetBlockTag(record, 0, &rNode, NULL, &heapBlkNo1);

                break;
            }
            case XLOG_HEAP_UPDATE: {
                if (isinit)
                    recData += sizeof(TransactionId);
                xl_heap_update* xlrec = (xl_heap_update*)recData;

                if (xlrec->flags & XLH_UPDATE_OLD_ALL_VISIBLE_CLEARED)
                    (void)XLogRecGetBlockTag(record, 1, &rNode, NULL, &heapBlkNo1);

                if (xlrec->flags & XLH_UPDATE_OLD_ALL_VISIBLE_CLEARED)
                    (void)XLogRecGetBlockTag(record, 0, &rNode, NULL, &heapBlkNo2);

                break;
            }
            default:
                break;
        };
    } else if (XLogRecGetRmid(record) == RM_HEAP2_ID) {
        if ((info & XLOG_HEAP_OPMASK) == XLOG_HEAP2_MULTI_INSERT) {
            if (isinit)
                recData += sizeof(TransactionId);
            xl_heap_multi_insert* xlrec = (xl_heap_multi_insert*)recData;

            if (xlrec->flags & XLH_INSERT_ALL_VISIBLE_CLEARED)
                (void)XLogRecGetBlockTag(record, 0, &rNode, NULL, &heapBlkNo1);
        }
    } else
        return;

    if (BlockNumberIsValid(heapBlkNo1)) {
        Assert(!RelFileNodeEquals(rNode, InvalidRelFileNode));
        vmBlkNo = HEAPBLK_TO_MAPBLOCK(heapBlkNo1);
        RegisterBlockChange(rNode, VISIBILITYMAP_FORKNUM, vmBlkNo);
    }

    if (BlockNumberIsValid(heapBlkNo2) && (heapBlkNo1 != heapBlkNo2)) {
        Assert(!RelFileNodeEquals(rNode, InvalidRelFileNode));
        vmBlkNo = HEAPBLK_TO_MAPBLOCK(heapBlkNo2);
        RegisterBlockChange(rNode, VISIBILITYMAP_FORKNUM, vmBlkNo);
    }
}

static void TrackDbStorageChange(XLogReaderState* record)
{
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
    RelFileNode rNode = InvalidRelFileNode;

    if (info == XLOG_DBASE_CREATE) {
        xl_dbase_create_rec* xlrec = (xl_dbase_create_rec*)XLogRecGetData(record);

        rNode.spcNode = xlrec->tablespace_id;
        rNode.dbNode = xlrec->db_id;

        Assert(rNode.dbNode != InvalidOid);

        RegisterBlockChangeExtended(rNode, MAIN_FORKNUM, InvalidBlockNumber, PAGETYPE_CREATE, InvalidBlockNumber);
    } else if (info == XLOG_DBASE_DROP) {
        xl_dbase_drop_rec* xlrec = (xl_dbase_drop_rec*)XLogRecGetData(record);

        rNode.spcNode = xlrec->tablespace_id;
        rNode.dbNode = xlrec->db_id;

        Assert(rNode.dbNode != InvalidOid);

        RegisterBlockChangeExtended(rNode, MAIN_FORKNUM, InvalidBlockNumber, PAGETYPE_DROP, InvalidBlockNumber);
    }
}

static void TrackTblspcStorageChange(XLogReaderState* record)
{
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
    RelFileNode rNode = InvalidRelFileNode;

    if (info == XLOG_TBLSPC_CREATE || info == XLOG_TBLSPC_RELATIVE_CREATE) {
        xl_tblspc_create_rec* xlrec = (xl_tblspc_create_rec*)XLogRecGetData(record);

        rNode.spcNode = xlrec->ts_id;

        RegisterBlockChangeExtended(rNode, MAIN_FORKNUM, InvalidBlockNumber, PAGETYPE_CREATE, InvalidBlockNumber);
    } else if (info == XLOG_TBLSPC_DROP) {
        xl_tblspc_drop_rec* xlrec = (xl_tblspc_drop_rec*)XLogRecGetData(record);

        rNode.spcNode = xlrec->ts_id;

        RegisterBlockChangeExtended(rNode, MAIN_FORKNUM, InvalidBlockNumber, PAGETYPE_DROP, InvalidBlockNumber);
    }
}

static void TrackRelmapChange(XLogReaderState* record)
{
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
    RelFileNode rNode = InvalidRelFileNode;

    if (info == XLOG_RELMAP_UPDATE) {
        xl_relmap_update* xlrec = (xl_relmap_update*)XLogRecGetData(record);

        rNode.spcNode = xlrec->tsid;
        rNode.dbNode = xlrec->dbid;

        RegisterBlockChangeExtended(rNode, MAIN_FORKNUM, InvalidBlockNumber, PAGETYPE_TRUNCATE, 0);
    }
}

static void RegisterBlockChange(const RelFileNode& rNode, ForkNumber forkNum, BlockNumber blkNo)
{
    RegisterBlockChangeExtended(rNode, forkNum, blkNo, PAGETYPE_MODIFY, InvalidBlockNumber);
}

static void RegisterBlockChangeExtended(
    const RelFileNode& rNode, ForkNumber forkNum, BlockNumber blkNo, uint8 pageType, BlockNumber truncBlkNo)
{
    Assert((BlockNumberIsValid(blkNo) && pageType == PAGETYPE_MODIFY) ||
           (!BlockNumberIsValid(blkNo) &&
               (pageType == PAGETYPE_DROP || pageType == PAGETYPE_TRUNCATE || pageType == PAGETYPE_CREATE)));

    Assert((BlockNumberIsValid(truncBlkNo) && pageType == PAGETYPE_TRUNCATE) ||
           (!BlockNumberIsValid(truncBlkNo) && pageType != PAGETYPE_TRUNCATE));

    CBMPageTag cbmPageTag;
    CbmHashEntry* cbmPageEntry = NULL;
    bool found = true;

    INIT_CBMPAGETAG(cbmPageTag, rNode, forkNum);

    if (pageType == PAGETYPE_DROP) {
        CBMHashRemove(cbmPageTag, t_thrd.cbm_cxt.XlogCbmSys->cbmPageHash, true);
    } else if (pageType == PAGETYPE_TRUNCATE && (forkNum == MAIN_FORKNUM || forkNum == VISIBILITYMAP_FORKNUM) &&
               rNode.relNode != InvalidOid) {
        Assert(BlockNumberIsValid(truncBlkNo));

        CBMPageEtyTruncate(cbmPageTag, t_thrd.cbm_cxt.XlogCbmSys->cbmPageHash, truncBlkNo, true);
    }

    cbmPageEntry =
        (CbmHashEntry*)hash_search(t_thrd.cbm_cxt.XlogCbmSys->cbmPageHash, (void*)&cbmPageTag, HASH_ENTER, &found);
    if (cbmPageEntry == NULL)
        ereport(ERROR,
            (errcode(ERRCODE_FETCH_DATA_FAILED),
                errmsg("could not find or create CBM page entry: rel %u/%u/%u "
                       "forknum %d blkno %u page type %d truncate blkno %u",
                    rNode.spcNode,
                    rNode.dbNode,
                    rNode.relNode,
                    forkNum,
                    blkNo,
                    pageType,
                    truncBlkNo)));

    if (!found)
        INIT_CBMPAGEENTRY(cbmPageEntry);

    CBMPageEtySetBitmap(cbmPageEntry, blkNo, pageType, truncBlkNo);
}

static void CBMPageEtySetBitmap(CbmHashEntry* cbmPageEntry, BlockNumber blkNo, uint8 pageType, BlockNumber truncBlkNo)
{
    BlockNumber pageFirstBlock = BLKNO_TO_CBM_PAGEFIRSTBOCK(blkNo);
    CbmPageHeader* cbmPageHeader = NULL;
    Dlelem* elt = NULL;

    for (elt = DLGetHead(&cbmPageEntry->pageDllist); elt; elt = DLGetSucc(elt)) {
        cbmPageHeader = (CbmPageHeader*)DLE_VAL(elt);
        if (cbmPageHeader->firstBlkNo != pageFirstBlock)
            continue;

        CBMPageSetBitmap((char*)cbmPageHeader, blkNo, pageType, truncBlkNo);
        /*
         * We found a match in the cache.  Move it to the front of the list
         * for its hashbucket, in order to speed subsequent searches.  (The
         * most frequently accessed elements in any hashbucket will tend to be
         * near the front of the hashbucket's list.)
         */
        DLMoveToFront(elt);

        return;
    }

    CreateNewCBMPageAndInsert(cbmPageEntry, blkNo, pageType, truncBlkNo);
}

static void CBMPageSetBitmap(char* page, BlockNumber blkNo, uint8 pageType, BlockNumber truncBlkNo)
{
    Assert(page);

    CbmPageHeader* cbmPageHeader = (CbmPageHeader*)page;

    cbmPageHeader->pageType |= pageType;

    if (BlockNumberIsValid(truncBlkNo)) {
        Assert(pageType == PAGETYPE_TRUNCATE);
        cbmPageHeader->truncBlkNo = truncBlkNo;
    }

    if (!BlockNumberIsValid(blkNo))
        return;

    char* bitMap = page + MAXALIGN(sizeof(CbmPageHeader));
    int mapByte = BLKNO_TO_CBMBYTEOFPAGE(blkNo);
    int mapBit = BLKNO_TO_CBMBITOFBYTE(blkNo);

    SET_CBM_PAGE_BITMAP(bitMap[mapByte], mapBit, CBM_PAGE_CHANGED);
}

static void CreateNewCBMPageAndInsert(
    CbmHashEntry* cbmPageEntry, BlockNumber blkNo, uint8 pageType, BlockNumber truncBlkNo)
{
    CbmPageHeader* cbmPageHeader = NULL;
    BlockNumber pageFirstBlock = BLKNO_TO_CBM_PAGEFIRSTBOCK(blkNo);
    Dlelem* elt = NULL;

    if (DLGetHead(&t_thrd.cbm_cxt.XlogCbmSys->pageFreeList)) {
        int rc = 0;
        elt = DLRemHead(&t_thrd.cbm_cxt.XlogCbmSys->pageFreeList);
        cbmPageHeader = (CbmPageHeader*)DLE_VAL(elt);
        Assert(cbmPageHeader);

        rc = memset_s(cbmPageHeader, CBMPAGESIZE, 0, CBMPAGESIZE);
        securec_check(rc, "\0", "\0");
    } else {
        /* For now, only cbm writer thread can do cbm-parsing-xlog */
        Assert(CurrentMemoryContext == t_thrd.cbm_cxt.cbmwriter_context);
        (void)MemoryContextSwitchTo(t_thrd.cbm_cxt.cbmwriter_page_context);
        cbmPageHeader = (CbmPageHeader*)palloc_extended(CBMPAGESIZE, MCXT_ALLOC_NO_OOM | MCXT_ALLOC_ZERO);
        if (cbmPageHeader == NULL)
            ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                    errmsg("memory is temporarily unavailable while allocate new CBM page")));

        elt = DLNewElem((void*)cbmPageHeader);
        (void)MemoryContextSwitchTo(t_thrd.cbm_cxt.cbmwriter_context);
    }

    INIT_CBMPAGEHEADER(cbmPageHeader, cbmPageEntry->cbmTag, pageFirstBlock);
    CBMPageSetBitmap((char*)cbmPageHeader, blkNo, pageType, truncBlkNo);

    DLAddHead(&cbmPageEntry->pageDllist, elt);
    cbmPageEntry->pageNum++;
    t_thrd.cbm_cxt.XlogCbmSys->totalPageNum++;

    ereport(DEBUG1,
        (errmsg("create new CBM page: rel %u/%u/%u forknum %d first blkno %u "
                "page type %d truncate blkno %u",
            cbmPageHeader->rNode.spcNode,
            cbmPageHeader->rNode.dbNode,
            cbmPageHeader->rNode.relNode,
            cbmPageHeader->forkNum,
            cbmPageHeader->firstBlkNo,
            cbmPageHeader->pageType,
            cbmPageHeader->truncBlkNo)));
}

static void CreateDummyCBMEtyPageAndInsert(void)
{
    CBMPageTag dummyPageTag;
    CbmHashEntry* dummyPageEntry = NULL;
    bool found = false;

    INIT_DUMMYCBMPAGETAG(dummyPageTag);
    dummyPageEntry =
        (CbmHashEntry*)hash_search(t_thrd.cbm_cxt.XlogCbmSys->cbmPageHash, (void*)&dummyPageTag, HASH_ENTER, &found);
    if (dummyPageEntry == NULL)
        ereport(ERROR,
            (errcode(ERRCODE_FETCH_DATA_FAILED),
                errmsg("could not create dummy CBM page entry: rel %u/%u/%u "
                       "forknum %d",
                    dummyPageTag.rNode.spcNode,
                    dummyPageTag.rNode.dbNode,
                    dummyPageTag.rNode.relNode,
                    dummyPageTag.forkNum)));

    Assert(!found);
    INIT_CBMPAGEENTRY(dummyPageEntry);

    CreateNewCBMPageAndInsert(dummyPageEntry, InvalidBlockNumber, PAGETYPE_MODIFY, InvalidBlockNumber);
}

static void FlushCBMPagesToDisk(XlogBitmap* xlogCbmSys, bool isCBMWriter)
{
    HASH_SEQ_STATUS status;
    CbmHashEntry* cbmPageEntry = NULL;

    hash_seq_init(&status, xlogCbmSys->cbmPageHash);

    while ((cbmPageEntry = (CbmHashEntry*)hash_seq_search(&status)) != NULL) {
        Dlelem* elt = NULL;
        int pageNum = cbmPageEntry->pageNum;
        Assert(pageNum);
        CbmPageHeader** cbmPageHeaderArray =
            (CbmPageHeader**)palloc_extended(pageNum * sizeof(CbmPageHeader*), MCXT_ALLOC_NO_OOM | MCXT_ALLOC_ZERO);

        if (cbmPageHeaderArray == NULL)
            ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                    errmsg("memory is temporarily unavailable while allocate CBM page header array")));

        for (elt = DLGetHead(&cbmPageEntry->pageDllist); elt; elt = DLGetSucc(elt)) {
            cbmPageHeaderArray[--pageNum] = (CbmPageHeader*)DLE_VAL(elt);
            Assert(cbmPageHeaderArray[pageNum]);
        }

        Assert(!pageNum);

        qsort(cbmPageHeaderArray, cbmPageEntry->pageNum, sizeof(CbmPageHeader*), CBMPageSeqCmp);

        for (pageNum = 0; pageNum < cbmPageEntry->pageNum; pageNum++) {
            FlushOneCBMPage((char*)(cbmPageHeaderArray[pageNum]), xlogCbmSys);

            ereport(DEBUG1,
                (errmsg("flush CBM page: rel %u/%u/%u forknum %d first blkno %u "
                        "page type %d truncate blkno %u",
                    cbmPageHeaderArray[pageNum]->rNode.spcNode,
                    cbmPageHeaderArray[pageNum]->rNode.dbNode,
                    cbmPageHeaderArray[pageNum]->rNode.relNode,
                    cbmPageHeaderArray[pageNum]->forkNum,
                    cbmPageHeaderArray[pageNum]->firstBlkNo,
                    cbmPageHeaderArray[pageNum]->pageType,
                    cbmPageHeaderArray[pageNum]->truncBlkNo)));
        }

        pfree(cbmPageHeaderArray);

        for (pageNum = 0; pageNum < cbmPageEntry->pageNum; pageNum++) {
            elt = DLRemHead(&cbmPageEntry->pageDllist);
            Assert(elt);

            if (elt != NULL) {
                if (isCBMWriter)
                    DLAddTail(&xlogCbmSys->pageFreeList, elt);
                else {
                    pfree(DLE_VAL(elt));
                    DLFreeElem(elt);
                }
            }
        }

        Assert(DLGetHead(&cbmPageEntry->pageDllist) == NULL && DLGetTail(&cbmPageEntry->pageDllist) == NULL);

        if (hash_search(xlogCbmSys->cbmPageHash, (void*)&cbmPageEntry->cbmTag, HASH_REMOVE, NULL) == NULL)
            ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("CBM hash table corrupted")));
    }

    Assert(xlogCbmSys->totalPageNum == 0);

    if (pg_fsync(xlogCbmSys->out.fd) != 0)
        ereport(ERROR,
            (errcode_for_file_access(), errmsg("fsync CBM file \"%s\" failed during flushing", xlogCbmSys->out.name)));

    if (isCBMWriter && xlogCbmSys->out.offset >= MAXCBMFILESIZE)
        RotateCBMFile();
}

static int CBMPageSeqCmp(const void* a, const void* b)
{
    CbmPageHeader* pha = *((CbmPageHeader**)a);
    CbmPageHeader* phb = *((CbmPageHeader**)b);

    return CBM_BLOCKNO_CMP(pha->firstBlkNo, phb->firstBlkNo);
}

static void FlushOneCBMPage(const char* page, XlogBitmap* xlogCbmSys)
{
    CbmPageHeader* cbmPageHeader = (CbmPageHeader*)page;

    cbmPageHeader->isLastBlock = (xlogCbmSys->totalPageNum == 1 ? true : false);
    cbmPageHeader->pageStartLsn = xlogCbmSys->startLSN;
    cbmPageHeader->pageEndLsn = xlogCbmSys->endLSN;
    cbmPageHeader->pageCrc = CBMPageCalcCRC(page);

    PGSTAT_INIT_TIME_RECORD();
    PGSTAT_START_TIME_RECORD();
    ssize_t size = pwrite(xlogCbmSys->out.fd, page, (size_t)CBMPAGESIZE, xlogCbmSys->out.offset);
    PGSTAT_END_TIME_RECORD(DATA_IO_TIME);

    if (size != (ssize_t)CBMPAGESIZE)
        ereport(ERROR,
            (errcode_for_file_access(),
                errmsg(
                    "could not write CBM file \"%s\", page offset %ld", xlogCbmSys->out.name, xlogCbmSys->out.offset)));

    xlogCbmSys->out.offset += (off_t)CBMPAGESIZE;
    xlogCbmSys->totalPageNum--;
}

static void RotateCBMFile(void)
{
    char prefix[MAXPGPATH];
    char filePath[MAXPGPATH];
    uint64 seqNum;
    XLogRecPtr startLSN;
    XLogRecPtr endLSN;
    uint32 startLSNHi = 0;
    uint32 startLSNLo = 0;
    uint32 endLSNHi = 0;
    uint32 endLSNLo = 0;

    Assert(t_thrd.cbm_cxt.XlogCbmSys->out.fd >= 0);
    if (close(t_thrd.cbm_cxt.XlogCbmSys->out.fd) != 0)
        ereport(ERROR,
            (errcode_for_file_access(),
                errmsg("close CBM file \"%s\" failed during rotate", t_thrd.cbm_cxt.XlogCbmSys->out.name)));

    if (strncmp(t_thrd.cbm_cxt.XlogCbmSys->out.name,
            t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome,
            strlen(t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome)) ||
        sscanf_s(t_thrd.cbm_cxt.XlogCbmSys->out.name + strlen(t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome),
            "%[a-z_]%lu_%8X%8X_%8X%8X.cbm",
            prefix,
            MAXPGPATH - 1,
            &seqNum,
            &startLSNHi,
            &startLSNLo,
            &endLSNHi,
            &endLSNLo) != 6) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_NAME),
                errmsg("found invalid CBM file name\"%s\" before rotate", t_thrd.cbm_cxt.XlogCbmSys->out.name)));
    }

    startLSN = (((uint64)startLSNHi) << 32) | startLSNLo;
    endLSN = (((uint64)endLSNHi) << 32) | endLSNLo;

    Assert(XLByteLT(startLSN, t_thrd.cbm_cxt.XlogCbmSys->endLSN));
    Assert(XLogRecPtrIsInvalid(endLSN));
    Assert(seqNum == t_thrd.cbm_cxt.XlogCbmSys->outSeqNum);

    SetCBMFileName(filePath, seqNum, startLSN, t_thrd.cbm_cxt.XlogCbmSys->endLSN);

    if (durable_rename(t_thrd.cbm_cxt.XlogCbmSys->out.name, filePath, ERROR))
        ereport(ERROR,
            (errcode_for_file_access(),
                errmsg("could not rename file \"%s\" to \"%s\" during rotate: %m",
                    t_thrd.cbm_cxt.XlogCbmSys->out.name,
                    filePath)));

    t_thrd.cbm_cxt.XlogCbmSys->outSeqNum++;
    StartNextCBMFile(t_thrd.cbm_cxt.XlogCbmSys->endLSN);
}

static void RemoveAllCBMFiles(int elevel)
{
    if (!rmtree(t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome, false, true))
        ereport(elevel, (errmsg("Failed to remove all CBM files")));
}

static void PrintCBMHashTab(HTAB* cbmPageHash)
{
    HASH_SEQ_STATUS status;
    CbmHashEntry* cbmPageEntry = NULL;

    hash_seq_init(&status, cbmPageHash);

    while ((cbmPageEntry = (CbmHashEntry*)hash_seq_search(&status)) != NULL) {
        Dlelem* elt = NULL;
        CbmPageHeader* cbmPageHeader = NULL;
        int pageNum = cbmPageEntry->pageNum;

        for (elt = DLGetHead(&cbmPageEntry->pageDllist); elt; elt = DLGetSucc(elt)) {
            cbmPageHeader = (CbmPageHeader*)DLE_VAL(elt);

            ereport(DEBUG1,
                (errmsg("print CBM page info: rel %u/%u/%u forknum %d first blkno %u "
                        "page type %d truncate blkno %u",
                    cbmPageHeader->rNode.spcNode,
                    cbmPageHeader->rNode.dbNode,
                    cbmPageHeader->rNode.relNode,
                    cbmPageHeader->forkNum,
                    cbmPageHeader->firstBlkNo,
                    cbmPageHeader->pageType,
                    cbmPageHeader->truncBlkNo)));
            pageNum--;
        }

        Assert(!pageNum);
    }
}

void CBMGetMergedFile(XLogRecPtr startLSN, XLogRecPtr endLSN, char* mergedFileName)
{
    XlogBitmap mergedXlogCbmSys;
    FILE* mergeFile = NULL;
    int rc;

    mergedXlogCbmSys.cbmPageHash = CBMPageHashInitialize(CurrentMemoryContext);

    CBMGetMergedHash(
        startLSN, endLSN, mergedXlogCbmSys.cbmPageHash, &mergedXlogCbmSys.startLSN, &mergedXlogCbmSys.endLSN);

    mergedXlogCbmSys.totalPageNum = GetCBMHashTotalPageNum(mergedXlogCbmSys.cbmPageHash);

    PrintCBMHashTab(mergedXlogCbmSys.cbmPageHash);

    mergeFile = MergedXlogCBMSysInitFile(&mergedXlogCbmSys);

    FlushCBMPagesToDisk(&mergedXlogCbmSys, false);

    if (FreeFile(mergeFile))
        ereport(WARNING,
            (errcode_for_file_access(),
                errmsg("could not close merged CBM file \"%s\": %m", mergedXlogCbmSys.out.name)));

    hash_destroy(mergedXlogCbmSys.cbmPageHash);

    rc = strncpy_s(mergedFileName, MAXPGPATH, mergedXlogCbmSys.out.name, strlen(mergedXlogCbmSys.out.name));
    securec_check(rc, "\0", "\0");
}

static void CBMGetMergedHash(
    XLogRecPtr startLSN, XLogRecPtr endLSN, HTAB* cbmPageHash, XLogRecPtr* mergeStartLSN, XLogRecPtr* mergeEndLSN)
{
    int cbmFileNum = 0;
    CbmFileName** cbmFileNameArray;

    cbmFileNameArray = GetAndValidateCBMFileArray(startLSN, endLSN, &cbmFileNum);

    MergeCBMFileArrayIntoHash(cbmFileNameArray, cbmFileNum, startLSN, endLSN, cbmPageHash, mergeStartLSN, mergeEndLSN);

    FreeCBMFileArray(cbmFileNameArray, cbmFileNum);
}

static CbmFileName** GetAndValidateCBMFileArray(XLogRecPtr startLSN, XLogRecPtr endLSN, int* fileNum)
{
    int cbmFileNum = 0;
    CbmFileName** cbmFileNameArray;

    cbmFileNameArray = GetCBMFileArray(startLSN, endLSN, &cbmFileNum, false);

    PrintCBMFileArray(cbmFileNameArray, cbmFileNum, startLSN, endLSN);

    ValidateCBMFileArray(cbmFileNameArray, cbmFileNum, startLSN, endLSN);

    *fileNum = cbmFileNum;

    return cbmFileNameArray;
}

static CbmFileName** GetCBMFileArray(XLogRecPtr startLSN, XLogRecPtr endLSN, int* fileNum, bool missingOk)
{
    DIR* cbmdir = NULL;
    struct dirent* cbmde = NULL;
    Dllist* cbmFileList = NULL;
    int cbmFileNum = 0;
    Dlelem* elt = NULL;
    CbmFileName** cbmFileNameArray = NULL;

    cbmdir = AllocateDir(t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome);
    if (cbmdir == NULL) {
        ereport(ERROR,
            (errcode_for_file_access(),
                errmsg("could not open CBM file directory \"%s\": %m", t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome)));
    }

    cbmFileList = DLNewList();

    while ((cbmde = ReadDir(cbmdir, t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome)) != NULL) {
        uint64 fileSeqNum = 0;
        XLogRecPtr fileStartLSN = InvalidXLogRecPtr;
        XLogRecPtr fileEndLSN = InvalidXLogRecPtr;

        /* Ignore files that are not CBM files. */
        if (!IsCBMFile(cbmde->d_name, &fileSeqNum, &fileStartLSN, &fileEndLSN)) {
            continue;
        }

        Assert(!XLogRecPtrIsInvalid(fileStartLSN));
        Assert(XLByteLT(fileStartLSN, fileEndLSN) || XLogRecPtrIsInvalid(fileEndLSN));

        if ((XLByteLE(fileStartLSN, startLSN) && (XLByteLT(startLSN, fileEndLSN) || XLogRecPtrIsInvalid(fileEndLSN))) ||
            (XLByteLE(startLSN, fileStartLSN) && !XLogRecPtrIsInvalid(fileEndLSN) && (XLByteLE(fileEndLSN, endLSN))) ||
            (XLByteLT(fileStartLSN, endLSN) && (XLByteLE(endLSN, fileEndLSN) || XLogRecPtrIsInvalid(fileEndLSN)))) {
            CbmFileName* cbmFileName = (CbmFileName*)palloc0(sizeof(CbmFileName));
            INIT_CBMFILENAME(cbmFileName, cbmde->d_name, fileSeqNum, fileStartLSN, fileEndLSN);
            elt = DLNewElem((void*)cbmFileName);
            DLAddHead(cbmFileList, elt);
            cbmFileNum++;
        }
    }
    (void)FreeDir(cbmdir);

    *fileNum = cbmFileNum;

    if (cbmFileNum == 0) {
        if (missingOk) {
            DLFreeList(cbmFileList);
            return NULL;
        } else {
            ereport(ERROR,
                (errcode(ERRCODE_FILE_READ_FAILED),
                    errmsg("could not find valid CBM file between %08X/%08X and "
                           "%08X/%08X, which may be caused by previous CBM switch-off, "
                           "truncation, or corruption",
                        (uint32)(startLSN >> 32),
                        (uint32)startLSN,
                        (uint32)(endLSN >> 32),
                        (uint32)endLSN)));
        }
    }

    cbmFileNameArray = SortCBMFilesList(cbmFileList, cbmFileNum);
    DLFreeList(cbmFileList);

    return cbmFileNameArray;
}

static CbmFileName** SortCBMFilesList(Dllist* cbmFileList, int cbmFileNum)
{
    CbmFileName** cbmFileNameArray = (CbmFileName**)palloc(cbmFileNum * sizeof(CbmFileName*));

    Dlelem* elt = NULL;
    int fileNum = cbmFileNum;

    for (elt = DLGetHead(cbmFileList); elt; elt = DLGetSucc(elt)) {
        cbmFileNameArray[--fileNum] = (CbmFileName*)DLE_VAL(elt);
        Assert(cbmFileNameArray[fileNum]);
    }

    Assert(!fileNum);

    qsort(cbmFileNameArray, cbmFileNum, sizeof(CbmFileName*), CBMFileNameSeqCmp);

    return cbmFileNameArray;
}

static int CBMFileNameSeqCmp(const void* a, const void* b)
{
    CbmFileName* fna = *((CbmFileName**)a);
    CbmFileName* fnb = *((CbmFileName**)b);

    return fna->seqNum < fnb->seqNum ? -1 : (fna->seqNum > fnb->seqNum ? 1 : 0);
}

static void PrintCBMFileArray(CbmFileName** cbmFileNameArray, int cbmFileNum, XLogRecPtr startLSN, XLogRecPtr endLSN)
{
    int i;
    StringInfo log = makeStringInfo();

    for (i = 0; i < cbmFileNum; i++) {
        appendStringInfo(log, " %s", cbmFileNameArray[i]->name);
    }

    ereport(DEBUG1,
        (errmsg("CBM file list for merging between %08X/%08X and "
                "%08X/%08X is:%s",
            (uint32)(startLSN >> 32),
            (uint32)startLSN,
            (uint32)(endLSN >> 32),
            (uint32)endLSN,
            log->data)));

    pfree(log->data);
    pfree(log);
}

static void ValidateCBMFileArray(CbmFileName** cbmFileNameArray, int cbmFileNum, XLogRecPtr startLSN, XLogRecPtr endLSN)
{
    uint64 lastfileSize = 0;
    XLogRecPtr lastfileTrackedLSN = InvalidXLogRecPtr;
    int i;

    if (XLByteLT(startLSN, cbmFileNameArray[0]->startLSN)) {
        ereport(ERROR,
            (errcode(ERRCODE_FILE_READ_FAILED),
                errmsg("could not find valid CBM file that contains the merging "
                       "start point %08X/%08X",
                    (uint32)(startLSN >> 32),
                    (uint32)startLSN)));
    }

    ValidateCBMFile(cbmFileNameArray[cbmFileNum - 1]->name, &lastfileTrackedLSN, &lastfileSize, false);
    if (XLByteLT(lastfileTrackedLSN, endLSN)) {
        ereport(ERROR,
            (errcode(ERRCODE_FILE_READ_FAILED),
                errmsg("could not find valid CBM file that contains the merging "
                       "end point %08X/%08X",
                    (uint32)(endLSN >> 32),
                    (uint32)endLSN)));
    }

    if (cbmFileNum == 1) {
        return;
    }

    for (i = 1; i < cbmFileNum; i++) {
        if (!XLByteEQ(cbmFileNameArray[i - 1]->endLSN, cbmFileNameArray[i]->startLSN)) {
            ereport(ERROR,
                (errcode(ERRCODE_FETCH_DATA_FAILED),
                    errmsg("there is a gap between CBM file %s and %s, which "
                           "may be caused by previous CBM switch-off, truncation, or "
                           "corruption",
                        cbmFileNameArray[i - 1]->name,
                        cbmFileNameArray[i]->name)));
        }
    }
}

static void MergeCBMFileArrayIntoHash(CbmFileName** cbmFileNameArray, int cbmFileNum, XLogRecPtr startLSN,
    XLogRecPtr endLSN, HTAB* cbmPageHash, XLogRecPtr* mergeStartLSN, XLogRecPtr* mergeEndLSN)
{
    int i;
    FILE* file = NULL;
    char* page = NULL;
    char filePath[MAXPGPATH];
    XLogRecPtr mergeStartPos = InvalidXLogRecPtr;
    XLogRecPtr mergeEndPos = InvalidXLogRecPtr;
    int rc;

    page = (char*)palloc_extended(CBMPAGESIZE, MCXT_ALLOC_NO_OOM);
    if (page == NULL)
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                errmsg("memory is temporarily unavailable while allocate "
                       "page read buffer during merge CBM file array")));

    for (i = 0; i < cbmFileNum; i++) {
        cbmPageIterator pageIterator;

        rc = snprintf_s(filePath,
            MAXPGPATH,
            MAXPGPATH - 1,
            "%s%s",
            t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome,
            cbmFileNameArray[i]->name);
        securec_check_ss(rc, "\0", "\0");

        file = AllocateFile(filePath, PG_BINARY_R);
        if (file == NULL)
            ereport(ERROR,
                (errcode_for_file_access(), errmsg("could not open CBM file \"%s\" while merging: %m", filePath)));

        ereport(DEBUG1, (errmsg("start iterating through CBM file \"%s\"", filePath)));

        INIT_CBMPAGEITERATOR(pageIterator, file, page);

        CBMPageIterBegin(&pageIterator, cbmFileNameArray[i]);

        while (CBMPageIterNext(&pageIterator, cbmFileNameArray[i])) {
            if (XLByteLE(pageIterator.pageEndLsn, startLSN)) {
                Assert(i == 0);
                continue;
            }

            MergeCBMPageIntoHash(pageIterator.buffer, cbmPageHash);

            if (pageIterator.isLastBlock) {
                if (XLByteLE(pageIterator.pageStartLsn, startLSN)) {
                    Assert(i == 0);
                    Assert(XLogRecPtrIsInvalid(mergeStartPos));
                    mergeStartPos = pageIterator.pageStartLsn;
                }

                if (XLByteLE(endLSN, pageIterator.pageEndLsn)) {
                    Assert(i == (cbmFileNum - 1));
                    Assert(XLogRecPtrIsInvalid(mergeEndPos));
                    mergeEndPos = pageIterator.pageEndLsn;
                }
            }

            if (!XLogRecPtrIsInvalid(mergeEndPos))
                break;
        }

        if (FreeFile(file))
            ereport(WARNING, (errcode_for_file_access(), errmsg("could not close CBM file \"%s\": %m", filePath)));

        if (XLogRecPtrIsInvalid(mergeEndPos))
            CBMPageIterEnd(&pageIterator, cbmFileNameArray[i]);

        if (i == 0 && XLogRecPtrIsInvalid(mergeStartPos))
            ereport(ERROR,
                (errcode(ERRCODE_FETCH_DATA_FAILED),
                    errmsg("could not find merge start point %08X/%08X "
                           "in CBM files ",
                        (uint32)(startLSN >> 32),
                        (uint32)startLSN)));

        if (i == (cbmFileNum - 1) && XLogRecPtrIsInvalid(mergeEndPos) &&
            XLByteLT((mergeEndPos = pageIterator.pageEndLsn), endLSN))
            ereport(ERROR,
                (errcode(ERRCODE_FETCH_DATA_FAILED),
                    errmsg("could not find merge end point %08X/%08X "
                           "in CBM files, the last cbm page end at %08X/%08X",
                        (uint32)(endLSN >> 32),
                        (uint32)endLSN,
                        (uint32)(pageIterator.pageEndLsn >> 32),
                        (uint32)pageIterator.pageEndLsn)));
    }

    if (mergeStartLSN != NULL)
        *mergeStartLSN = mergeStartPos;

    if (mergeEndLSN != NULL)
        *mergeEndLSN = mergeEndPos;

    pfree(page);
}

static void CBMPageIterBegin(cbmPageIterator* pageIteratorPtr, CbmFileName* cbmFile)
{
    ssize_t readLen;

    Assert(pageIteratorPtr->readOffset == (off_t)0);

    PGSTAT_INIT_TIME_RECORD();
    PGSTAT_START_TIME_RECORD();
    readLen = pread(fileno(pageIteratorPtr->file),
        pageIteratorPtr->buffer,
        (size_t)sizeof(cbmpageheader),
        pageIteratorPtr->readOffset);
    PGSTAT_END_TIME_RECORD(DATA_IO_TIME);

    if (readLen != (ssize_t)sizeof(cbmpageheader))
        ereport(ERROR,
            (errcode(ERRCODE_FETCH_DATA_FAILED),
                errmsg("could not read the first page head of CBM file \"%s\" ", cbmFile->name)));

    pageIteratorPtr->pageStartLsn = ((cbmpageheader*)(pageIteratorPtr->buffer))->pageStartLsn;

    if (!XLByteEQ(pageIteratorPtr->pageStartLsn, cbmFile->startLSN))
        ereport(ERROR,
            (errcode(ERRCODE_FETCH_DATA_FAILED),
                errmsg("the first page start LSN %08X/%08X of CBM file \"%s\" "
                       "does not equal the file start LSN %08X/%08X",
                    (uint32)(pageIteratorPtr->pageStartLsn >> 32),
                    (uint32)pageIteratorPtr->pageStartLsn,
                    cbmFile->name,
                    (uint32)(cbmFile->startLSN >> 32),
                    (uint32)cbmFile->startLSN)));

    pageIteratorPtr->pageEndLsn = ((cbmpageheader*)(pageIteratorPtr->buffer))->pageEndLsn;
    pageIteratorPtr->isLastBlock = false;
}

static bool CBMPageIterNext(cbmPageIterator* pageIteratorPtr, CbmFileName* cbmFile)
{
    pg_crc32c checksum;
    pg_crc32c actualChecksum;
    ssize_t readLen;

    Assert(pageIteratorPtr->readOffset % (off_t)CBMPAGESIZE == (off_t)0);

    PGSTAT_INIT_TIME_RECORD();
    PGSTAT_START_TIME_RECORD();
    readLen =
        pread(fileno(pageIteratorPtr->file), pageIteratorPtr->buffer, (size_t)CBMPAGESIZE, pageIteratorPtr->readOffset);
    PGSTAT_END_TIME_RECORD(DATA_IO_TIME);

    if (readLen != (ssize_t)CBMPAGESIZE) {
        if (readLen == (ssize_t)0)
            ereport(DEBUG1,
                (errmsg("reach end at page offset %ld of CBM file \"%s\", stop reading",
                    pageIteratorPtr->readOffset,
                    cbmFile->name)));
        else
            ereport(ERROR,
                (errcode(ERRCODE_FILE_READ_FAILED),
                    errmsg("partial page read occurs at page offset %ld of CBM file \"%s\", "
                           "stop reading",
                        pageIteratorPtr->readOffset,
                        cbmFile->name)));
        return false;
    }

    checksum = ((cbmpageheader*)(pageIteratorPtr->buffer))->pageCrc;
    actualChecksum = CBMPageCalcCRC(pageIteratorPtr->buffer);

    pageIteratorPtr->checksumOk = (checksum == actualChecksum);
    if (!pageIteratorPtr->checksumOk)
        ereport(ERROR,
            (errcode(ERRCODE_FILE_READ_FAILED),
                errmsg("Corruption detected in CBM file \"%s\", page offset %ld",
                    cbmFile->name,
                    pageIteratorPtr->readOffset)));

    pageIteratorPtr->prevStartLsn = pageIteratorPtr->pageStartLsn;
    pageIteratorPtr->prevEndLsn = pageIteratorPtr->pageEndLsn;
    pageIteratorPtr->isPrevLastBlock = pageIteratorPtr->isLastBlock;

    pageIteratorPtr->pageStartLsn = ((cbmpageheader*)(pageIteratorPtr->buffer))->pageStartLsn;
    pageIteratorPtr->pageEndLsn = ((cbmpageheader*)(pageIteratorPtr->buffer))->pageEndLsn;
    pageIteratorPtr->isLastBlock = ((cbmpageheader*)(pageIteratorPtr->buffer))->isLastBlock;

    if (pageIteratorPtr->isPrevLastBlock) {
        if (!XLByteEQ(pageIteratorPtr->pageStartLsn, pageIteratorPtr->prevEndLsn))
            ereport(ERROR,
                (errcode(ERRCODE_FETCH_DATA_FAILED),
                    errmsg("LSN track gap detected in CBM file \"%s\", page offset %ld: "
                           "previous page batch end LSN is %08X/%08X, current page batch start "
                           "LSN is %08X/%08X",
                        cbmFile->name,
                        pageIteratorPtr->readOffset,
                        (uint32)(pageIteratorPtr->prevEndLsn >> 32),
                        (uint32)pageIteratorPtr->prevEndLsn,
                        (uint32)(pageIteratorPtr->pageStartLsn >> 32),
                        (uint32)pageIteratorPtr->pageStartLsn)));
    } else {
        if (!XLByteEQ(pageIteratorPtr->pageStartLsn, pageIteratorPtr->prevStartLsn) ||
            !XLByteEQ(pageIteratorPtr->pageEndLsn, pageIteratorPtr->prevEndLsn))
            ereport(ERROR,
                (errcode(ERRCODE_FETCH_DATA_FAILED),
                    errmsg("Inconsistent start/end LSN in one page batch for CBM file "
                           "\"%s\" at page offset %ld: previous page start-end LSN %08X/%08X-"
                           "%08X/%08X, curent page start-end LSN %08X/%08X-%08X/%08X",
                        cbmFile->name,
                        pageIteratorPtr->readOffset,
                        (uint32)(pageIteratorPtr->prevStartLsn >> 32),
                        (uint32)pageIteratorPtr->prevStartLsn,
                        (uint32)(pageIteratorPtr->prevEndLsn >> 32),
                        (uint32)pageIteratorPtr->prevEndLsn,
                        (uint32)(pageIteratorPtr->pageStartLsn >> 32),
                        (uint32)pageIteratorPtr->pageStartLsn,
                        (uint32)(pageIteratorPtr->pageEndLsn >> 32),
                        (uint32)pageIteratorPtr->pageEndLsn)));
    }

    pageIteratorPtr->readOffset += (off_t)CBMPAGESIZE;

    return true;
}

static void CBMPageIterEnd(cbmPageIterator* pageIteratorPtr, CbmFileName* cbmFile)
{
    if (!XLogRecPtrIsInvalid(cbmFile->endLSN) && !XLByteEQ(pageIteratorPtr->pageEndLsn, cbmFile->endLSN))
        ereport(ERROR,
            (errcode(ERRCODE_FETCH_DATA_FAILED),
                errmsg("the last read page end LSN %08X/%08X of CBM file \"%s\" "
                       "does not equal the file end LSN %08X/%08X",
                    (uint32)(pageIteratorPtr->pageEndLsn >> 32),
                    (uint32)pageIteratorPtr->pageEndLsn,
                    cbmFile->name,
                    (uint32)(cbmFile->endLSN >> 32),
                    (uint32)cbmFile->endLSN)));
}

static void MergeCBMPageIntoHash(const char* page, HTAB* cbmPageHash)
{
    CbmPageHeader* cbmPageHeader = (cbmpageheader*)page;
    CBMPageTag cbmPageTag;
    CbmHashEntry* cbmPageEntry = NULL;
    bool found = true;

    if (CBM_PAGE_IS_DUMMY(cbmPageHeader)) {
        Assert(cbmPageHeader->firstBlkNo == InvalidBlockNumber);
        Assert(cbmPageHeader->isLastBlock);
        ereport(DEBUG1,
            (errmsg("reach a dummy page for LSN range %08X/%08X to "
                    "%08X/%08X, so skip",
                (uint32)(cbmPageHeader->pageStartLsn >> 32),
                (uint32)cbmPageHeader->pageStartLsn,
                (uint32)(cbmPageHeader->pageEndLsn >> 32),
                (uint32)cbmPageHeader->pageEndLsn)));
        return;
    }

    ValidateCBMPageHeader(cbmPageHeader);

    INIT_CBMPAGETAG(cbmPageTag, cbmPageHeader->rNode, cbmPageHeader->forkNum);

    if (cbmPageHeader->pageType & PAGETYPE_DROP) {
        Assert(!BlockNumberIsValid(cbmPageHeader->firstBlkNo));
        CBMHashRemove(cbmPageTag, cbmPageHash, false);
    }

    if (cbmPageHeader->pageType & PAGETYPE_TRUNCATE) {
        Assert(!BlockNumberIsValid(cbmPageHeader->firstBlkNo));
        Assert(BlockNumberIsValid(cbmPageHeader->truncBlkNo));
        Assert(cbmPageHeader->forkNum == MAIN_FORKNUM || cbmPageHeader->forkNum == FSM_FORKNUM ||
               cbmPageHeader->forkNum == VISIBILITYMAP_FORKNUM);

        if ((cbmPageHeader->forkNum == MAIN_FORKNUM || cbmPageHeader->forkNum == VISIBILITYMAP_FORKNUM) &&
            cbmPageHeader->rNode.relNode != InvalidOid)
            CBMPageEtyTruncate(cbmPageTag, cbmPageHash, cbmPageHeader->truncBlkNo, false);
    }

    cbmPageEntry = (CbmHashEntry*)hash_search(cbmPageHash, (void*)&cbmPageTag, HASH_ENTER, &found);
    if (cbmPageEntry == NULL)
        ereport(ERROR,
            (errcode(ERRCODE_FILE_READ_FAILED),
                errmsg("could not find or create CBM page entry: rel %u/%u/%u "
                       "forknum %d during merge into hash",
                    cbmPageTag.rNode.spcNode,
                    cbmPageTag.rNode.dbNode,
                    cbmPageTag.rNode.relNode,
                    cbmPageTag.forkNum)));

    if (!found)
        INIT_CBMPAGEENTRY(cbmPageEntry);

    CBMPageEtyMergePage(cbmPageEntry, page);
}

static void ValidateCBMPageHeader(cbmpageheader* cbmPageHeader)
{
    uint8 pageType = cbmPageHeader->pageType;
    XLogRecPtr pageStartLsn = cbmPageHeader->pageStartLsn;
    XLogRecPtr pageEndLsn = cbmPageHeader->pageEndLsn;
    RelFileNode rNode = cbmPageHeader->rNode;
    BlockNumber firstBlkNo = cbmPageHeader->firstBlkNo;
    BlockNumber truncBlkNo = cbmPageHeader->truncBlkNo;

    if (XLByteLE(pageEndLsn, pageStartLsn) || RelFileNodeEquals(rNode, InvalidRelFileNode) ||
        (BlockNumberIsValid(firstBlkNo) && pageType != PAGETYPE_MODIFY) ||
        (!BlockNumberIsValid(firstBlkNo) &&
            !((pageType & PAGETYPE_DROP) || (pageType & PAGETYPE_TRUNCATE) || (pageType & PAGETYPE_CREATE))) ||
        (BlockNumberIsValid(truncBlkNo) && !(pageType & PAGETYPE_TRUNCATE)) ||
        (!BlockNumberIsValid(truncBlkNo) && (pageType & PAGETYPE_TRUNCATE)))
        ereport(ERROR,
            (errcode(ERRCODE_FETCH_DATA_FAILED),
                errmsg("invalid CBM page header: rel %u/%u/%u forknum %d "
                       "first blkno %u page type %d truncate blkno %u",
                    cbmPageHeader->rNode.spcNode,
                    cbmPageHeader->rNode.dbNode,
                    cbmPageHeader->rNode.relNode,
                    cbmPageHeader->forkNum,
                    cbmPageHeader->firstBlkNo,
                    cbmPageHeader->pageType,
                    cbmPageHeader->truncBlkNo)));
}

static void CBMHashRemove(const CBMPageTag& cbmPageTag, HTAB* cbmPageHash, bool isCBMWriter)
{
    Assert(cbmPageTag.rNode.spcNode != InvalidOid);

    if (cbmPageTag.rNode.relNode != InvalidOid) {
        CBMPageEtyRemove(cbmPageTag, cbmPageHash, isCBMWriter, false);

        if (cbmPageTag.forkNum == MAIN_FORKNUM) {
            CBMPageEtyRemoveRestFork(cbmPageTag, cbmPageHash, isCBMWriter);
        }
    } else if (cbmPageTag.rNode.dbNode != InvalidOid) {
        CBMHashRemoveDb(cbmPageTag, cbmPageHash, isCBMWriter);
    } else {
        CBMHashRemoveTblspc(cbmPageTag, cbmPageHash, isCBMWriter);
    }
}

static void CBMHashRemoveDb(const CBMPageTag& cbmPageTag, HTAB* cbmPageHash, bool isCBMWriter)
{
    HASH_SEQ_STATUS status;
    CbmHashEntry* cbmPageEntry = NULL;

    Assert(cbmPageTag.rNode.spcNode != InvalidOid && cbmPageTag.rNode.dbNode != InvalidOid &&
           cbmPageTag.rNode.relNode == InvalidOid);

    hash_seq_init(&status, cbmPageHash);

    while ((cbmPageEntry = (CbmHashEntry*)hash_seq_search(&status)) != NULL) {
        if (cbmPageEntry->cbmTag.rNode.spcNode == cbmPageTag.rNode.spcNode &&
            cbmPageEntry->cbmTag.rNode.dbNode == cbmPageTag.rNode.dbNode)
            CBMPageEtyRemove(cbmPageEntry->cbmTag, cbmPageHash, isCBMWriter, true);
    }
}

static void CBMHashRemoveTblspc(const CBMPageTag& cbmPageTag, HTAB* cbmPageHash, bool isCBMWriter)
{
    HASH_SEQ_STATUS status;
    CbmHashEntry* cbmPageEntry = NULL;

    Assert(cbmPageTag.rNode.spcNode != InvalidOid && cbmPageTag.rNode.dbNode == InvalidOid &&
           cbmPageTag.rNode.relNode == InvalidOid);

    hash_seq_init(&status, cbmPageHash);

    while ((cbmPageEntry = (CbmHashEntry*)hash_seq_search(&status)) != NULL) {
        if (cbmPageEntry->cbmTag.rNode.spcNode == cbmPageTag.rNode.spcNode)
            CBMPageEtyRemove(cbmPageEntry->cbmTag, cbmPageHash, isCBMWriter, true);
    }
}

static void CBMPageEtyRemove(const CBMPageTag& cbmPageTag, HTAB* cbmPageHash, bool isCBMWriter, bool removeEntry)
{
    CbmHashEntry* cbmPageEntry = NULL;
    Dlelem* elt = NULL;
    bool found = true;

    cbmPageEntry = (CbmHashEntry*)hash_search(cbmPageHash, (void*)&cbmPageTag, HASH_FIND, &found);

    if (!found) {
        return;
    }

    Assert(cbmPageEntry != NULL);

    StringInfo log = makeStringInfo();
    appendStringInfo(log,
        "remove all cbm pages of rel %u/%u/%u forknum %d",
        cbmPageTag.rNode.spcNode,
        cbmPageTag.rNode.dbNode,
        cbmPageTag.rNode.relNode,
        cbmPageTag.forkNum);

    while ((elt = DLRemHead(&cbmPageEntry->pageDllist)) != NULL) {
        appendStringInfo(log, " page first blocknum %u", ((CbmPageHeader*)DLE_VAL(elt))->firstBlkNo);

        cbmPageEntry->pageNum--;

        if (isCBMWriter) {
            t_thrd.cbm_cxt.XlogCbmSys->totalPageNum--;
            DLAddTail(&t_thrd.cbm_cxt.XlogCbmSys->pageFreeList, elt);
        } else {
            pfree(DLE_VAL(elt));
            DLFreeElem(elt);
        }
    }

    Assert(cbmPageEntry->pageNum == 0);
    Assert(DLGetHead(&cbmPageEntry->pageDllist) == NULL && DLGetTail(&cbmPageEntry->pageDllist) == NULL);

    if (removeEntry && hash_search(cbmPageHash, (void*)&cbmPageTag, HASH_REMOVE, NULL) == NULL) {
        ereport(ERROR, (errcode(ERRCODE_FILE_READ_FAILED), errmsg("CBM hash table corrupted")));
    }

    ereport(DEBUG1, (errmsg("Remove CBM hash entry: %s", log->data)));

    pfree(log->data);
    pfree(log);
}

static void CBMPageEtyRemoveRestFork(const CBMPageTag& cbmPageTag, HTAB* cbmPageHash, bool isCBMWriter)
{
    int tmpForkNum;

    CBMPageTag tmpTag = cbmPageTag;
    for (tmpForkNum = FSM_FORKNUM; tmpForkNum <= MAX_FORKNUM; tmpForkNum++) {
        tmpTag.forkNum = tmpForkNum;
        CBMPageEtyRemove(tmpTag, cbmPageHash, isCBMWriter, true);
    }
}

static void CBMPageEtyTruncate(
    const CBMPageTag& cbmPageTag, HTAB* cbmPageHash, BlockNumber truncBlkNo, bool isCBMWriter)
{
    CbmHashEntry* cbmPageEntry = NULL;
    CbmPageHeader* cbmPageHeader = NULL;
    Dlelem* elt = NULL;
    Dlelem* nelt = NULL;
    bool found = true;

    cbmPageEntry = (CbmHashEntry*)hash_search(cbmPageHash, (void*)&cbmPageTag, HASH_FIND, &found);
    if (!found) {
        return;
    }

    Assert(cbmPageEntry != NULL);

    BlockNumber resPageFirstBlkNo;
    StringInfo log = makeStringInfo();

    appendStringInfo(log,
        "truncate cbm pages of rel %u/%u/%u forknum %d "
        "to %u blocks",
        cbmPageTag.rNode.spcNode,
        cbmPageTag.rNode.dbNode,
        cbmPageTag.rNode.relNode,
        cbmPageTag.forkNum,
        truncBlkNo);

    resPageFirstBlkNo = (truncBlkNo == 0 ? InvalidBlockNumber : BLKNO_TO_CBM_PAGEFIRSTBOCK(truncBlkNo - 1));

    for (elt = DLGetHead(&cbmPageEntry->pageDllist); elt; elt = nelt) {
        nelt = DLGetSucc(elt);
        cbmPageHeader = (CbmPageHeader*)DLE_VAL(elt);
        if (!BlockNumberIsValid(cbmPageHeader->firstBlkNo)) {
            continue;
        }

        if (CBM_BLOCKNO_CMP(cbmPageHeader->firstBlkNo, resPageFirstBlkNo) < 0) {
            continue;
        } else if (CBM_BLOCKNO_CMP(cbmPageHeader->firstBlkNo, resPageFirstBlkNo) > 0) {
            appendStringInfo(log, " truncate whole page with first blocknum %u", cbmPageHeader->firstBlkNo);

            DLRemove(elt);
            cbmPageEntry->pageNum--;

            if (isCBMWriter) {
                t_thrd.cbm_cxt.XlogCbmSys->totalPageNum--;
                DLAddTail(&t_thrd.cbm_cxt.XlogCbmSys->pageFreeList, elt);
            } else {
                pfree(DLE_VAL(elt));
                DLFreeElem(elt);
            }
        } else {
            bool needReserve = false;
            BlockNumber blkNo;
            char* bitMap = (char*)cbmPageHeader + MAXALIGN(sizeof(CbmPageHeader));
            int mapByte, mapBit;

            for (blkNo = cbmPageHeader->firstBlkNo; blkNo <= (truncBlkNo - 1); blkNo++) {
                mapByte = BLKNO_TO_CBMBYTEOFPAGE(blkNo);
                mapBit = BLKNO_TO_CBMBITOFBYTE(blkNo);
                if (bitMap[mapByte] & (1U << (uint32)mapBit)) {
                    needReserve = true;
                    break;
                }
            }

            if (needReserve) {
                for (blkNo = truncBlkNo; blkNo < (cbmPageHeader->firstBlkNo + CBM_BLOCKS_PER_PAGE); blkNo++) {
                    mapByte = BLKNO_TO_CBMBYTEOFPAGE(blkNo);
                    mapBit = BLKNO_TO_CBMBITOFBYTE(blkNo);

                    CLEAR_CBM_PAGE_BITMAP(bitMap[mapByte], mapBit);
                }
            } else {
                DLRemove(elt);
                cbmPageEntry->pageNum--;

                if (isCBMWriter) {
                    t_thrd.cbm_cxt.XlogCbmSys->totalPageNum--;
                    DLAddTail(&t_thrd.cbm_cxt.XlogCbmSys->pageFreeList, elt);
                } else {
                    pfree(DLE_VAL(elt));
                    DLFreeElem(elt);
                }
            }

            appendStringInfo(log,
                " truncate %s page with first blocknum %u",
                needReserve ? "partial" : "whole",
                cbmPageHeader->firstBlkNo);
        }
    }

    ereport(DEBUG1, (errmsg("Truncate CBM hash entry: %s", log->data)));

    pfree(log->data);
    pfree(log);
}

static void CBMPageEtyMergePage(CbmHashEntry* cbmPageEntry, const char* page)
{
    BlockNumber pageFirstBlock = ((CbmPageHeader*)page)->firstBlkNo;
    Dlelem* elt = NULL;
    CbmPageHeader* cbmPageHeader = NULL;

    for (elt = DLGetHead(&cbmPageEntry->pageDllist); elt; elt = DLGetSucc(elt)) {
        cbmPageHeader = (CbmPageHeader*)DLE_VAL(elt);
        if (cbmPageHeader->firstBlkNo != pageFirstBlock)
            continue;

        CBMPageMergeBitmap((char*)cbmPageHeader, page);
        /*
         * We found a match in the cache.  Move it to the front of the list
         * for its hashbucket, in order to speed subsequent searches.  (The
         * most frequently accessed elements in any hashbucket will tend to be
         * near the front of the hashbucket's list.)
         */
        DLMoveToFront(elt);

        return;
    }

    CopyCBMPageAndInsert(cbmPageEntry, page);
}

static void CBMPageMergeBitmap(char* cbmHashPage, const char* newPage)
{
    int i;

    CbmPageHeader* cbmPageHeader = (CbmPageHeader*)cbmHashPage;
    CbmPageHeader* newPageHeader = (CbmPageHeader*)newPage;

    cbmPageHeader->pageType |= newPageHeader->pageType;

    if (BlockNumberIsValid(newPageHeader->truncBlkNo)) {
        Assert(newPageHeader->pageType & PAGETYPE_TRUNCATE);
        cbmPageHeader->truncBlkNo = newPageHeader->truncBlkNo;
    }

    if (!BlockNumberIsValid(newPageHeader->firstBlkNo))
        return;

    for (i = MAXALIGN(sizeof(CbmPageHeader)); i < CBMPAGESIZE; i++)
        cbmHashPage[i] = (char)((unsigned char)cbmHashPage[i] | (unsigned char)newPage[i]);
}

static void CopyCBMPageAndInsert(CbmHashEntry* cbmPageEntry, const char* page)
{
    Dlelem* elt = NULL;
    int rc;
    CbmPageHeader* cbmPageHeader = (CbmPageHeader*)palloc_extended(CBMPAGESIZE, MCXT_ALLOC_NO_OOM);

    if (cbmPageHeader == NULL)
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                errmsg("memory is temporarily unavailable while allocate new CBM page")));

    rc = memcpy_s(cbmPageHeader, CBMPAGESIZE, page, CBMPAGESIZE);
    securec_check(rc, "\0", "\0");

    elt = DLNewElem((void*)cbmPageHeader);

    DLAddHead(&cbmPageEntry->pageDllist, elt);
    cbmPageEntry->pageNum++;
}

static uint64 GetCBMHashTotalPageNum(HTAB* cbmPageHash)
{
    HASH_SEQ_STATUS status;
    CbmHashEntry* cbmPageEntry = NULL;
    uint64 totalPageNum = 0;

    hash_seq_init(&status, cbmPageHash);

    while ((cbmPageEntry = (CbmHashEntry*)hash_seq_search(&status)) != NULL)
        totalPageNum += cbmPageEntry->pageNum;

    return totalPageNum;
}

static FILE* MergedXlogCBMSysInitFile(XlogBitmap* mergedXlogCbmSys)
{
    FILE* file = NULL;
    int rc;
    struct timeval curTime = {0, 0};

    (void)gettimeofday(&curTime, NULL);

    rc = snprintf_s(mergedXlogCbmSys->out.name,
        MAXPGPATH,
        MAXPGPATH - 1,
        mergedBmpFileNameTemplate,
        t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome,
        mergedCbmFileNameStem,
        (uint32)(mergedXlogCbmSys->startLSN >> 32),
        (uint32)mergedXlogCbmSys->startLSN,
        (uint32)(mergedXlogCbmSys->endLSN >> 32),
        (uint32)mergedXlogCbmSys->endLSN,
        curTime.tv_sec,
        curTime.tv_usec);
    securec_check_ss(rc, "\0", "\0");

    file = AllocateFile(mergedXlogCbmSys->out.name, PG_BINARY_W);
    if (file == NULL || (mergedXlogCbmSys->out.fd = fileno(file)) < 0)
        ereport(ERROR,
            (errcode_for_file_access(),
                errmsg("could not create merge dest CBM file \"%s\": %m", mergedXlogCbmSys->out.name)));

    mergedXlogCbmSys->out.offset = (off_t)0;

    return file;
}

static void FreeCBMFileArray(CbmFileName** cbmFileNameArray, int cbmFileNum)
{
    int i;

    for (i = 0; i < cbmFileNum; i++) {
        pfree(cbmFileNameArray[i]->name);
        pfree(cbmFileNameArray[i]);
    }

    pfree(cbmFileNameArray);
}

CBMArray* CBMGetMergedArray(XLogRecPtr startLSN, XLogRecPtr endLSN)
{
    HTAB* cbmPageHash = NULL;
    CBMArray* cbmArray = (CBMArray*)palloc_extended(sizeof(CBMArray), MCXT_ALLOC_NO_OOM | MCXT_ALLOC_ZERO);

    if (cbmArray == NULL) {
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                errmsg("memory is temporarily unavailable while allocate CBM array")));
    }

    cbmPageHash = CBMPageHashInitialize(CurrentMemoryContext);

    CBMGetMergedHash(startLSN, endLSN, cbmPageHash, &cbmArray->startLSN, &cbmArray->endLSN);

    PrintCBMHashTab(cbmPageHash);

    if (GetCBMHashTotalPageNum(cbmPageHash) != 0)
        cbmArray->arrayEntry = ConvertCBMHashIntoArray(cbmPageHash, &cbmArray->arrayLength, true);

    hash_destroy(cbmPageHash);

    return cbmArray;
}

static CBMArrayEntry* ConvertCBMHashIntoArray(HTAB* cbmPageHash, long* arrayLength, bool destroyHashEntry)
{
    HASH_SEQ_STATUS status;
    CbmHashEntry* cbmPageEntry = NULL;
    long totalHashEntryNum = hash_get_num_entries(cbmPageHash);
    long hashEntryIndex = 0;

    CBMArrayEntry* cbmArrayEntry =
        (CBMArrayEntry*)palloc_extended(totalHashEntryNum * sizeof(CBMArrayEntry), MCXT_ALLOC_NO_OOM | MCXT_ALLOC_ZERO);

    if (cbmArrayEntry == NULL)
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                errmsg("memory is temporarily unavailable while allocate CBM array entry")));

    hash_seq_init(&status, cbmPageHash);

    while ((cbmPageEntry = (CbmHashEntry*)hash_seq_search(&status)) != NULL) {
        Dlelem* elt = NULL;
        int pageNum = cbmPageEntry->pageNum;
        Assert(pageNum);
        CbmPageHeader** cbmPageHeaderArray =
            (CbmPageHeader**)palloc_extended(pageNum * sizeof(CbmPageHeader*), MCXT_ALLOC_NO_OOM | MCXT_ALLOC_ZERO);

        if (cbmPageHeaderArray == NULL)
            ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                    errmsg("memory is temporarily unavailable while allocate CBM page header array")));

        for (elt = DLGetHead(&cbmPageEntry->pageDllist); elt; elt = DLGetSucc(elt)) {
            cbmPageHeaderArray[--pageNum] = (CbmPageHeader*)DLE_VAL(elt);
            Assert(cbmPageHeaderArray[pageNum]);
        }

        Assert(!pageNum);

        qsort(cbmPageHeaderArray, cbmPageEntry->pageNum, sizeof(CbmPageHeader*), CBMPageSeqCmp);

        INIT_CBMARRAYENTRY(&(cbmArrayEntry[hashEntryIndex]), cbmPageEntry->cbmTag);

        for (pageNum = 0; pageNum < cbmPageEntry->pageNum; pageNum++) {
            MergeOneCBMPageIntoArrayEntry((char*)(cbmPageHeaderArray[pageNum]), &(cbmArrayEntry[hashEntryIndex]));

            ereport(DEBUG1,
                (errmsg("convert CBM page into array: rel %u/%u/%u "
                        "forknum %d first blkno %u page type %d truncate blkno %u",
                    cbmPageHeaderArray[pageNum]->rNode.spcNode,
                    cbmPageHeaderArray[pageNum]->rNode.dbNode,
                    cbmPageHeaderArray[pageNum]->rNode.relNode,
                    cbmPageHeaderArray[pageNum]->forkNum,
                    cbmPageHeaderArray[pageNum]->firstBlkNo,
                    cbmPageHeaderArray[pageNum]->pageType,
                    cbmPageHeaderArray[pageNum]->truncBlkNo)));
        }

        pfree(cbmPageHeaderArray);

        hashEntryIndex++;

        if (destroyHashEntry) {
            for (pageNum = 0; pageNum < cbmPageEntry->pageNum; pageNum++) {
                elt = DLRemHead(&cbmPageEntry->pageDllist);
                Assert(elt);

                if (elt != NULL) {
                    pfree(DLE_VAL(elt));
                    DLFreeElem(elt);
                }
            }

            Assert(DLGetHead(&cbmPageEntry->pageDllist) == NULL && DLGetTail(&cbmPageEntry->pageDllist) == NULL);

            if (hash_search(cbmPageHash, (void*)&cbmPageEntry->cbmTag, HASH_REMOVE, NULL) == NULL)
                ereport(ERROR, (errcode(ERRCODE_FETCH_DATA_FAILED), errmsg("CBM hash table corrupted")));
        }
    }

    if ((*arrayLength = hashEntryIndex) != totalHashEntryNum)
        ereport(ERROR,
            (errcode(ERRCODE_FETCH_DATA_FAILED),
                errmsg("CBM hash table corrupted: incorrect total hash entry number")));

    return cbmArrayEntry;
}

static void MergeOneCBMPageIntoArrayEntry(char* page, CBMArrayEntry* cbmArrayEntry)
{
    CbmPageHeader* cbmPageHeader = (CbmPageHeader*)page;
    CBMBitmapIterator bitmapIter;
    BlockNumber blkNo;

    if (!BlockNumberIsValid(cbmPageHeader->firstBlkNo)) {
        cbmArrayEntry->changeType = cbmPageHeader->pageType;

        if (cbmArrayEntry->changeType & PAGETYPE_TRUNCATE) {
            Assert(BlockNumberIsValid(cbmPageHeader->truncBlkNo));
            cbmArrayEntry->truncBlockNum = cbmPageHeader->truncBlkNo;
        }

        return;
    }

    INIT_CBMBITMAPITERATOR(bitmapIter,
        (char*)(page + MAXALIGN(sizeof(CbmPageHeader))),
        cbmPageHeader->firstBlkNo,
        cbmPageHeader->firstBlkNo + CBM_BLOCKS_PER_PAGE - 1);

    while (CBMBitmapNext(&bitmapIter, &blkNo))
        CBMArrayEntryRigsterBlock(cbmArrayEntry, blkNo);
}

static bool CBMBitmapNext(CBMBitmapIterator* iter, BlockNumber* blkno)
{
    char* bitmap = iter->bitmap;
    BlockNumber blkNo;
    int mapByte;
    int mapBit;

    for (;;) {
        blkNo = iter->nextBlkNo++;

        if (blkNo > iter->endBlkNo)
            break;

        mapByte = BLKNO_TO_CBMBYTEOFPAGE(blkNo);
        mapBit = BLKNO_TO_CBMBITOFBYTE(blkNo);
        if (bitmap[mapByte] & (1U << (uint32)mapBit)) {
            *blkno = blkNo;
            return true;
        }
    }

    /* no more set bits in this bitmap. */
    return false;
}

static void InitCBMArrayEntryBlockArray(CBMArrayEntry* cbmArrayEntry)
{
    cbmArrayEntry->changedBlock =
        (BlockNumber*)palloc_extended(INITBLOCKARRAYSIZE * sizeof(BlockNumber), MCXT_ALLOC_NO_OOM | MCXT_ALLOC_ZERO);

    if (cbmArrayEntry->changedBlock == NULL)
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                errmsg("memory is temporarily unavailable while allocate CBM block array")));

    cbmArrayEntry->maxSize = INITBLOCKARRAYSIZE;
}

static void CBMArrayEntryRigsterBlock(CBMArrayEntry* cbmArrayEntry, BlockNumber blkNo)
{
    if (cbmArrayEntry->totalBlockNum >= cbmArrayEntry->maxSize) {
        cbmArrayEntry->maxSize *= 2;
        cbmArrayEntry->changedBlock =
            (BlockNumber*)repalloc(cbmArrayEntry->changedBlock, cbmArrayEntry->maxSize * sizeof(BlockNumber));
    }

    cbmArrayEntry->changedBlock[cbmArrayEntry->totalBlockNum++] = blkNo;
}

void FreeCBMArray(CBMArray* cbmArray)
{
    long i;

    if (cbmArray == NULL)
        return;

    if (cbmArray->arrayLength > 0) {
        for (i = 0; i < cbmArray->arrayLength; i++)
            pfree(cbmArray->arrayEntry[i].changedBlock);

        pfree(cbmArray->arrayEntry);
    }

    pfree(cbmArray);
}

void CBMRecycleFile(XLogRecPtr targetLSN, XLogRecPtr* endLSN)
{
    int cbmFileNum = 0;
    CbmFileName** cbmFileNameArray;
    uint64 fileSize = 0;
    XLogRecPtr fileTrackedLSN = InvalidXLogRecPtr;
    XLogRecPtr maxFileTrackedLSN = InvalidXLogRecPtr;
    XLogRecPtr minFileStartLSN = InvalidXLogRecPtr;
    int i;

    cbmFileNameArray = GetCBMFileArray(InvalidXLogRecPtr, targetLSN, &cbmFileNum, true);

    if (cbmFileNum == 0) {
        *endLSN = targetLSN;
        return;
    }

    for (i = 0; i < cbmFileNum; i++) {
        fileTrackedLSN = cbmFileNameArray[i]->endLSN;

        if (XLogRecPtrIsInvalid(fileTrackedLSN))
            ValidateCBMFile(cbmFileNameArray[i]->name, &fileTrackedLSN, &fileSize, false);

        if (XLogRecPtrIsInvalid(fileTrackedLSN))
            fileTrackedLSN = cbmFileNameArray[i]->startLSN;

        Assert(!XLogRecPtrIsInvalid(fileTrackedLSN));

        if (XLByteLE(fileTrackedLSN, targetLSN)) {
            UnlinkCBMFile(cbmFileNameArray[i]->name);

            if (XLByteLT(maxFileTrackedLSN, fileTrackedLSN))
                maxFileTrackedLSN = fileTrackedLSN;
        } else {
            if (XLogRecPtrIsInvalid(minFileStartLSN))
                minFileStartLSN = cbmFileNameArray[i]->startLSN;
            else if (XLByteLT(cbmFileNameArray[i]->startLSN, minFileStartLSN))
                minFileStartLSN = cbmFileNameArray[i]->startLSN;
        }
    }

    *endLSN = XLogRecPtrIsInvalid(maxFileTrackedLSN) ? minFileStartLSN : maxFileTrackedLSN;

    FreeCBMFileArray(cbmFileNameArray, cbmFileNum);
}

static void UnlinkCBMFile(const char* cbmFileName)
{
    char filePath[MAXPGPATH];
    int rc;

    rc = snprintf_s(filePath, MAXPGPATH, MAXPGPATH - 1, "%s%s", t_thrd.cbm_cxt.XlogCbmSys->cbmFileHome, cbmFileName);
    securec_check_ss(rc, "\0", "\0");

    rc = unlink(filePath);
    if (rc < 0 && errno != ENOENT)
        ereport(ERROR, (errcode_for_file_access(), errmsg("could not remove file \"%s\": %m", filePath)));
}

/*
 * If needed, trigger one-cycle xlog parse at once, at least up to targetLSN.
 * On success, the actual parse stop position is returned.
 * On failure, invalid xlog position is returned.
 *
 * If one want to parse to an exact xlog position, caller should already hold
 * CBMParseXlogLock exclusive lock , then write xlog and pass in the xlog position.
 * On return, the CBMParseXlogLock lock would be released anyway.
 */
XLogRecPtr ForceTrackCBMOnce(XLogRecPtr targetLSN, int timeOut, bool wait, bool lockHeld, bool isRecEnd)
{
    XLogRecPtr endLSN;

    if (!u_sess->attr.attr_storage.enable_cbm_tracking)
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                errmsg("could not force tracking cbm because cbm tracking function is not enabled!")));

    if (wait && timeOut < 0)
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("Negative timeout for force track cbm!")));

    advanceXlogPtrToNextPageIfNeeded(&targetLSN);
    endLSN = GetCBMTrackedLSN();

    if (XLByteLT(endLSN, targetLSN)) {
        if (!lockHeld)
            (void)LWLockAcquire(CBMParseXlogLock, LW_EXCLUSIVE);

        endLSN = GetCBMTrackedLSN();
        if (XLByteLE(targetLSN, endLSN))
            LWLockRelease(CBMParseXlogLock);
        else {
            XLogRecPtr tmpTargeLSN;
            bool isTmpRecEnd = false;
            tmpTargeLSN = GetTmpTargetLSN(&isTmpRecEnd);
            if (XLogRecPtrIsInvalid(tmpTargeLSN)) {
                SetTmpTargetLSN(targetLSN, isRecEnd);
            } else if (lockHeld) {
                /* when lock is held, we pass in just-finished xlog record end position */
                Assert(XLByteLT(tmpTargeLSN, targetLSN));
                Assert(isRecEnd);
                SetTmpTargetLSN(targetLSN, isRecEnd);
            }

            if (g_instance.proc_base->cbmwriterLatch)
                SetLatch(g_instance.proc_base->cbmwriterLatch);

            LWLockRelease(CBMParseXlogLock);

            for (;;) {
                if (XLByteLE(targetLSN, endLSN))
                    break;
                else if (!wait || timeOut <= 0) {
                    endLSN = InvalidXLogRecPtr;
                    break;
                }

                CHECK_FOR_INTERRUPTS();
                pg_usleep(1000L);
                timeOut--;
                endLSN = GetCBMTrackedLSN();
            }
        }
    } else {
        if (lockHeld)
            LWLockRelease(CBMParseXlogLock);
    }

    return endLSN;
}

static XLogRecPtr GetTmpTargetLSN(bool* isRecEnd)
{
    *isRecEnd = tmpLSNIsRecEnd;
    return tmpTargetLSN;
}

static void SetTmpTargetLSN(XLogRecPtr targetLSN, bool isRecEnd)
{
    tmpTargetLSN = targetLSN;
    tmpLSNIsRecEnd = isRecEnd;
}

static XLogRecPtr GetLatestCompTargetLSN(void)
{
    return latestCompTargetLSN;
}

static void SetLatestCompTargetLSN(XLogRecPtr latestCompletedLSN)
{
    latestCompTargetLSN = latestCompletedLSN;
}

void advanceXlogPtrToNextPageIfNeeded(XLogRecPtr* recPtr)
{
    if (*recPtr % XLogSegSize == 0) {
        XLByteAdvance((*recPtr), SizeOfXLogLongPHD);
    } else if (*recPtr % XLOG_BLCKSZ == 0) {
        XLByteAdvance((*recPtr), SizeOfXLogShortPHD);
    }
}
