#include "build.auto.h"

#include <stdbool.h>
#include "common/log.h"
#include "postgres/interface/crc32.h"
#include "recordProcessGPDB6.h"
#include "xlogInfoGPDB6.h"

#define XLR_INFO_MASK           0x0F
#define XLOG_HEAP_OPMASK        0x70

typedef uint32 CommandId;
typedef uint16 OffsetNumber;
typedef uint32 ForkNumber;

typedef struct BkpBlock
{
    RelFileNode node;           /* relation containing block */
    ForkNumber fork;            /* fork within the relation */
    BlockNumber block;          /* block number */
    uint16 hole_offset;         /* number of bytes before "hole" */
    uint16 hole_length;         /* number of bytes in "hole" */

    /* ACTUAL BLOCK DATA FOLLOWS AT END OF STRUCT */
} BkpBlock;

typedef struct xl_smgr_truncate
{
    BlockNumber blkno;
    RelFileNode rnode;
} xl_smgr_truncate;

typedef struct BlockIdData
{
    uint16 bi_hi;
    uint16 bi_lo;
} BlockIdData;

typedef struct ItemPointerData
{
    BlockIdData ip_blkid;
    OffsetNumber ip_posid;
}

#ifdef __arm__
__attribute__((packed))         /* Appropriate whack upside the head for ARM */
#endif
ItemPointerData;

typedef struct xl_heaptid
{
    RelFileNode node;
    ItemPointerData tid;        /* changed tuple id */
} xl_heaptid;

typedef struct xl_heap_new_cid
{
    /*
     * store toplevel xid so we don't have to merge cids from different
     * transactions
     */
    TransactionId top_xid;
    CommandId cmin;
    CommandId cmax;

    /*
     * don't really need the combocid since we have the actual values right in
     * this struct, but the padding makes it free and its useful for
     * debugging.
     */
    CommandId combocid;

    /*
     * Store the relfilenode/ctid pair to facilitate lookups.
     */
    xl_heaptid target;
} xl_heap_new_cid;

// Get RelFileNode from XLOG record.
// Only XLOG_FPI contains RelFileNode, so the other record types are ignored.
static bool
getXlog(const XLogRecord *record, RelFileNode **node)
{
    const uint8 info = (uint8) (record->xl_info & ~XLR_INFO_MASK);
    switch (info)
    {
        case XLOG_CHECKPOINT_SHUTDOWN:
        case XLOG_CHECKPOINT_ONLINE:
        case XLOG_NOOP:
        case XLOG_NEXTOID:
        case XLOG_NEXTRELFILENODE:
        case XLOG_RESTORE_POINT:
        case XLOG_BACKUP_END:
        case XLOG_PARAMETER_CHANGE:
        case XLOG_FPW_CHANGE:
        case XLOG_END_OF_RECOVERY:
        case XLOG_OVERWRITE_CONTRECORD:
        case XLOG_SWITCH:
//          ignore
            break;

        case XLOG_FPI:
        {
            *node = (RelFileNode *) XLogRecGetData(record);
            return true;
        }

        default:
            THROW_FMT(FormatError, "XLOG UNKNOWN: %d", info);
    }
    return false;
}

// Get RelFileNode from Storage record.
// in XLOG_SMGR_TRUNCATE, the RelFileNode is not at the beginning of the structure.
static bool
getStorage(const XLogRecord *record, RelFileNode **node)
{
    const uint8 info = (uint8) (record->xl_info & ~XLR_INFO_MASK);
    switch (info)
    {
        case XLOG_SMGR_CREATE:
        {
            *node = (RelFileNode *) XLogRecGetData(record);
            return true;
        }

        case XLOG_SMGR_TRUNCATE:
        {
            xl_smgr_truncate *xlrec = (xl_smgr_truncate *) XLogRecGetData(record);

            *node = &xlrec->rnode;
            return true;
        }

        default:
            THROW_FMT(FormatError, "Storage UNKNOWN: %d", info);
    }
}

// Get RelFileNode from Heap2 record.
// Only XLOG_HEAP2_REWRITE not contains RelFileNode, so it are ignored.
// in XLOG_HEAP2_NEW_CID, the RelFileNode is not at the beginning of the structure.
// this function does not throw errors because XLOG_HEAP_OPMASK contains only 3 non-zero bits, which gives 8 possible values, all of
// which are used.
static bool
getHeap2(const XLogRecord *record, RelFileNode **node)
{
    uint8 info = (uint8) (record->xl_info & ~XLR_INFO_MASK);
    info &= XLOG_HEAP_OPMASK;

    if (info == XLOG_HEAP2_NEW_CID)
    {
        xl_heap_new_cid *xlrec = (xl_heap_new_cid *) XLogRecGetData(record);
        *node = &xlrec->target.node;
        return true;
    }

    if (info == XLOG_HEAP2_REWRITE)
    {
        return false;
    }

    // XLOG_HEAP2_CLEAN
    // XLOG_HEAP2_FREEZE_PAGE
    // XLOG_HEAP2_CLEANUP_INFO
    // XLOG_HEAP2_VISIBLE
    // XLOG_HEAP2_MULTI_INSERT
    // XLOG_HEAP2_LOCK_UPDATED
    *node = (RelFileNode *) XLogRecGetData(record);
    return true;
}

// Get RelFileNode from Heap record.
// this function does not throw errors because XLOG_HEAP_OPMASK contains only 3 non-zero bits, which gives 8 possible values, all of
// which are used.
static bool
getHeap(const XLogRecord *record, RelFileNode **node)
{
    uint8 info = (uint8) (record->xl_info & ~XLR_INFO_MASK);
    info &= XLOG_HEAP_OPMASK;

    if (info == XLOG_HEAP_MOVE)
    {
        // XLOG_HEAP_MOVE is no longer used.
        THROW(FormatError, "There should be no XLOG_HEAP_MOVE entry for this version of Postgres.");
    }

    // XLOG_HEAP_INSERT
    // XLOG_HEAP_DELETE
    // XLOG_HEAP_UPDATE
    // XLOG_HEAP_HOT_UPDATE
    // XLOG_HEAP_NEWPAGE
    // XLOG_HEAP_LOCK
    // XLOG_HEAP_INPLACE
    *node = (RelFileNode *) XLogRecGetData(record);
    return true;
}

// Get RelFileNode from Btree record.
static bool
getBtree(const XLogRecord *record, RelFileNode **node)
{
    const uint8 info = (uint8) (record->xl_info & ~XLR_INFO_MASK);
    switch (info)
    {
        case XLOG_BTREE_INSERT_LEAF:
        case XLOG_BTREE_INSERT_UPPER:
        case XLOG_BTREE_INSERT_META:
        case XLOG_BTREE_SPLIT_L:
        case XLOG_BTREE_SPLIT_R:
        case XLOG_BTREE_SPLIT_L_ROOT:
        case XLOG_BTREE_SPLIT_R_ROOT:
        case XLOG_BTREE_VACUUM:
        case XLOG_BTREE_DELETE:
        case XLOG_BTREE_MARK_PAGE_HALFDEAD:
        case XLOG_BTREE_UNLINK_PAGE_META:
        case XLOG_BTREE_UNLINK_PAGE:
        case XLOG_BTREE_NEWROOT:
        case XLOG_BTREE_REUSE_PAGE:
        {
            *node = (RelFileNode *) XLogRecGetData(record);
            return true;
        }

        default:
            THROW_FMT(FormatError, "Btree UNKNOWN: %d", info);
    }
}

// Get RelFileNode from Gin record.
static bool
getGin(const XLogRecord *record, RelFileNode **node)
{
    const uint8 info = (uint8) (record->xl_info & ~XLR_INFO_MASK);
    switch (info)
    {
        case XLOG_GIN_CREATE_INDEX:
        case XLOG_GIN_CREATE_PTREE:
        case XLOG_GIN_INSERT:
        case XLOG_GIN_SPLIT:
        case XLOG_GIN_VACUUM_PAGE:
        case XLOG_GIN_VACUUM_DATA_LEAF_PAGE:
        case XLOG_GIN_DELETE_PAGE:
        case XLOG_GIN_UPDATE_META_PAGE:
        case XLOG_GIN_INSERT_LISTPAGE:
        case XLOG_GIN_DELETE_LISTPAGE:
        {
            *node = (RelFileNode *) XLogRecGetData(record);
            return true;
        }

        default:
            THROW_FMT(FormatError, "GIN UNKNOWN: %d", info);
    }
}

// Get RelFileNode from Gist record.
static bool
getGist(const XLogRecord *record, RelFileNode **node)
{
    const uint8 info = (uint8) (record->xl_info & ~XLR_INFO_MASK);
    switch (info)
    {
        case XLOG_GIST_PAGE_UPDATE:
        case XLOG_GIST_PAGE_SPLIT:
        case XLOG_GIST_CREATE_INDEX:
        {
            *node = (RelFileNode *) XLogRecGetData(record);
            return true;
        }

        default:
            THROW_FMT(FormatError, "GIST UNKNOWN: %d", info);
    }
}

// Get RelFileNode from Seq record.
static bool
getSeq(const XLogRecord *record, RelFileNode **node)
{
    const uint8 info = (uint8) (record->xl_info & ~XLR_INFO_MASK);
    if (info == XLOG_SEQ_LOG)
    {
        *node = (RelFileNode *) XLogRecGetData(record);
        return true;
    }
    THROW_FMT(FormatError, "Sequence UNKNOWN: %d", info);
}

// Get RelFileNode from Spgist record.
static bool
getSpgist(const XLogRecord *record, RelFileNode **node)
{
    const uint8 info = (uint8) (record->xl_info & ~XLR_INFO_MASK);

    switch (info)
    {
        case XLOG_SPGIST_CREATE_INDEX:
        case XLOG_SPGIST_ADD_LEAF:
        case XLOG_SPGIST_MOVE_LEAFS:
        case XLOG_SPGIST_ADD_NODE:
        case XLOG_SPGIST_SPLIT_TUPLE:
        case XLOG_SPGIST_PICKSPLIT:
        case XLOG_SPGIST_VACUUM_LEAF:
        case XLOG_SPGIST_VACUUM_ROOT:
        case XLOG_SPGIST_VACUUM_REDIRECT:
        {
            *node = (RelFileNode *) XLogRecGetData(record);
            return true;
        }

        default:
            THROW_FMT(FormatError, "SPGIST UNKNOWN: %d", info);
    }
}

// Get RelFileNode from Bitmap record.
static bool
getBitmap(const XLogRecord *record, RelFileNode **node)
{
    const uint8 info = (uint8) (record->xl_info & ~XLR_INFO_MASK);
    switch (info)
    {
        case XLOG_BITMAP_INSERT_LOVITEM:
        case XLOG_BITMAP_INSERT_META:
        case XLOG_BITMAP_INSERT_BITMAP_LASTWORDS:
        case XLOG_BITMAP_INSERT_WORDS:
        case XLOG_BITMAP_UPDATEWORD:
        case XLOG_BITMAP_UPDATEWORDS:
        {
            *node = (RelFileNode *) XLogRecGetData(record);
            return true;
        }

        default:
            THROW_FMT(FormatError, "Bitmap UNKNOWN: %d", info);
    }
}

// Get RelFileNode from Appendonly record.
static bool
getAppendonly(const XLogRecord *record, RelFileNode **node)
{
    const uint8 info = (uint8) (record->xl_info & ~XLR_INFO_MASK);
    switch (info)
    {
        case XLOG_APPENDONLY_INSERT:
        case XLOG_APPENDONLY_TRUNCATE:
        {
            *node = (RelFileNode *) XLogRecGetData(record);
            return true;
        }

        default:
            THROW_FMT(FormatError, "Appendonly UNKNOWN: %d", info);
    }
}

FN_EXTERN bool
getRelFileNodeGPDB6(const XLogRecord *record, RelFileNode **node)
{
    switch (record->xl_rmid)
    {
        case RM_XLOG_ID:
            return getXlog(record, node);

        case RM_SMGR_ID:
            return getStorage(record, node);

        case RM_HEAP2_ID:
            return getHeap2(record, node);

        case RM_HEAP_ID:
            return getHeap(record, node);

        case RM_BTREE_ID:
            return getBtree(record, node);

        case RM_GIN_ID:
            return getGin(record, node);

        case RM_GIST_ID:
            return getGist(record, node);

        case RM_SEQ_ID:
            return getSeq(record, node);

        case RM_SPGIST_ID:
            return getSpgist(record, node);

        case RM_BITMAP_ID:
            return getBitmap(record, node);

        case RM_APPEND_ONLY_ID:
            return getAppendonly(record, node);

        // Records of these types do not contain a RelFileNode.
        case RM_XACT_ID:
        case RM_CLOG_ID:
        case RM_DBASE_ID:
        case RM_TBLSPC_ID:
        case RM_MULTIXACT_ID:
        case RM_RELMAP_ID:
        case RM_STANDBY_ID:
        case RM_DISTRIBUTEDLOG_ID:
            // skip
            return false;

        case RM_HASH_ID:
            THROW(FormatError, "Not supported in greenplum. shouldn't be here");

        default:
            THROW(FormatError, "Unknown resource manager");
    }
}

FN_EXTERN void
validXLogRecordHeaderGPDB6(const XLogRecord *const record, const PgPageSize heapPageSize)
{
    /*
     * xl_len == 0 is bad data for everything except XLOG SWITCH, where it is
     * required.
     */
    if (record->xl_rmid == RM_XLOG_ID && record->xl_info == XLOG_SWITCH)
    {
        if (record->xl_len != 0)
        {
            THROW_FMT(FormatError, "invalid xlog switch record");
        }
    }
    else if (record->xl_len == 0)
    {
        THROW_FMT(FormatError, "record with zero length");
    }
    if (record->xl_tot_len < SizeOfXLogRecord + record->xl_len ||
        record->xl_tot_len > SizeOfXLogRecord + record->xl_len +
        XLR_MAX_BKP_BLOCKS * (sizeof(BkpBlock) + heapPageSize))
    {
        THROW_FMT(FormatError, "invalid record length");
    }
    if (record->xl_rmid > RM_APPEND_ONLY_ID)
    {
        THROW_FMT(FormatError, "invalid resource manager ID %u", record->xl_rmid);
    }
}

FN_EXTERN void
validXLogRecordGPDB6(const XLogRecord *const record, const PgPageSize heapPageSize)
{
    const uint32 len = record->xl_len;
    uint32 remaining = record->xl_tot_len;

    remaining -= (uint32) (SizeOfXLogRecord + len);
    pg_crc32 crc = crc32cInit();
    crc = crc32cComp(crc, (unsigned char *) XLogRecGetData(record), len);

    BkpBlock bkpb;
    /* Add in the backup blocks, if any */
    const char *blk = (char *) XLogRecGetData(record) + len;
    for (int i = 0; i < XLR_MAX_BKP_BLOCKS; i++)
    {
        uint32 blen;

        if (!(record->xl_info & XLR_BKP_BLOCK(i)))
            continue;

        if (remaining < sizeof(BkpBlock))
        {
            THROW_FMT(FormatError, "invalid backup block size in record");
        }
        memcpy(&bkpb, blk, sizeof(BkpBlock));

        if (bkpb.hole_offset + bkpb.hole_length > heapPageSize)
        {
            THROW_FMT(FormatError, "incorrect hole size in record");
        }
        blen = (uint32) sizeof(BkpBlock) + heapPageSize - bkpb.hole_length;

        if (remaining < blen)
        {
            THROW_FMT(FormatError, "invalid backup block size in record");
        }
        remaining -= blen;
        crc = crc32cComp(crc, (unsigned char *) blk, blen);
        blk += blen;
    }

    /* Check that xl_tot_len agrees with our calculation */
    if (remaining != 0)
    {
        THROW_FMT(FormatError, "incorrect total length in record");
    }

    /* Finally include the record header */
    crc = crc32cComp(crc, (unsigned char *) record, offsetof(XLogRecord, xl_crc));
    crc = crc32cFinish(crc);

    if (crc != record->xl_crc)
    {
        THROW_FMT(FormatError, "incorrect resource manager data checksum in record. expect: %u, but got: %u", record->xl_crc, crc);
    }
}
