#ifndef PGBACKREST_TEST_RECORD_PROCESS_H
#define PGBACKREST_TEST_RECORD_PROCESS_H

#include "common/walFilter/postgres_common.h"

#define GPDB6_XLOG_PAGE_MAGIC 0xD07E

FN_EXTERN bool getRelFileNodeGPDB6(XLogRecord *record, RelFileNode *node);

FN_EXTERN void validXLogRecordHeaderGPDB6(const XLogRecord *record);
FN_EXTERN void validXLogRecordGPDB6(const XLogRecord *record);
#endif // PGBACKREST_TEST_RECORD_PROCESS_H
