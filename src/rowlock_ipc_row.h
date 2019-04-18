/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This is the header file for Inter Process Communication used by 
** row lock feature.
*/
#ifndef SQLITE_OMIT_ROWLOCK
#ifndef SQLITE_ROWLOCK_IPC_ROW_H
#define SQLITE_ROWLOCK_IPC_ROW_H

#include "sqliteInt.h"
#include "rowlock_os.h"
#if SQLITE_OS_WIN
#include "Windows.h"
#endif

/* MMAP name's suffix */
#define MMAP_SUFFIX_ROWLOCK "-rowlock"
/* Mutex name's suffix */
#define MUTEX_SUFFIX_ROWLOCK "-rowlock-mutex"


typedef struct RowMetaData {
#if SQLITE_OS_UNIX
  MUTEX_HANDLE mutex; /* Mutex is shared on Linux */
#endif
  u64 nElement; /* The number of elements */
  u64 count;    /* The number of buckets */
} RowMetaData;

typedef struct RowElement {
  u64 hash;
  int iTable;
  i64 rowid;
  PID pid;
  u64 owner; /* The owner of this row lock. Having a lock if it is same as owner in IpcHandle. */
} RowElement;


int rowClassMapName(char *buf, size_t bufSize, const char *name);
u8 rowClassIsInitialized(void *pMap);
void rowClassInitArea(void *pMap, u64 allocSize);
u64 rowClassElemCount(void *pMap);
u8 rowClassIsValid(void *pElem);
u8 rowClassElemIsTarget(void *pElem1, void *pElem2);
void *rowClassElemGet(void *pMap, u64 idx);
u64 rowClassElemHash(void *pMap, u64 idx);
void rowClassElemClear(void *pMap, u64 idx);
void rowClassElemCopy(void *pMap, u64 iDest, u64 iSrc);
u64 rowClassIndexPrev(void *pMap, u64 idx);
u64 rowClassIndexNext(void *pMap, u64 idx);
u64 rowClassCalcHash(void *pMap, ...);
u64 rowClassTestHash(void *pMap, ...);


#endif /* SQLITE_ROWLOCK_IPC_ROW_H */
#endif /* SQLITE_OMIT_ROWLOCK */
