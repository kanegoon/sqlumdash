/*
** SPDX-License-Identifier: MIT
**
** Copyright (c) 2018 Toshiba Corporation
**
*************************************************************************
** This file contains the main implementation of row lock feature.
*/
#ifndef SQLITE_OMIT_ROWLOCK
#include "btreeInt.h"
#include "vdbeInt.h"
#include "rowlock_hash.h"
#include "rowlock_ipc.h"
#include "rowlock.h"
#include <signal.h>

static u8 rowlockIsIndex(Btree *p, int rootPage);
static u8 cachedRowidFlagGet(BtCursor *pCur);

static void rowlockIpcCleanup(void){
  BtShared *pBt;
  for(pBt=sharedCacheListGet(); pBt; pBt=pBt->pNext){
    const char *dbFullPath = sqlite3PagerFilename(pBt->pPager, 0);
    sqlite3rowlockIpcUnlockRecordProc(NULL, dbFullPath);
    sqlite3rowlockIpcUnlockTablesProc(NULL, dbFullPath);
    sqlite3rowlockIpcCachedRowidReset(NULL, dbFullPath);
  }
}

/* Launched when a signal is catched. */
#if SQLITE_OS_WIN
static void rowlockSignalHandler(int signal){
#else
static void rowlockSignalHandler(int signal, siginfo_t *info, void *ctx) {
#endif
  switch( signal ){
    case SIGINT:
    case SIGILL: 
    case SIGFPE: 
    case SIGSEGV: 
    case SIGTERM: 
#if SQLITE_OS_WIN
    case SIGBREAK: 
#endif
    case SIGABRT:
      rowlockIpcCleanup();
      break;
  }
}

/* Setting of signal handler */
static int rowlockSetSignalAction(){
  int signals[] = {SIGINT, SIGILL, SIGFPE, SIGSEGV, SIGTERM, SIGABRT
#if SQLITE_OS_WIN
    ,SIGBREAK
#endif
  };

  return rowlockOsSetSignalAction(signals, sizeof(signals)/sizeof(int), &rowlockSignalHandler);
}

/*
** Initialize rowlock feature. It must be called before sqlite3_open().
** It is called automatically when this library is loaded or sqlite_initialize for the test.
*/
int rowlockInitialize(){
  int ret;
  sqlite3_enable_shared_cache(1);
  ret = rowlockSetSignalAction();
  if( ret!=EXIT_SUCCESS ) return SQLITE_ERROR;
  return SQLITE_OK;
}

/* 
** The following functions are called when library is loaded or unloaded.
** Row lock feature requires to enable shared cache. 
*/
#if SQLITE_OS_WIN
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD fdwReason, LPVOID lpvReserved){
  int rc;
  switch (fdwReason) {
    /* DLL is loaded */
    case DLL_PROCESS_ATTACH:
      rc = rowlockInitialize();
      if( rc!=SQLITE_OK ) return FALSE;
      break;
    /* New thread is creating */
    case DLL_THREAD_ATTACH:
      break;
    /* New thread is closing */
    case DLL_THREAD_DETACH:
      break;
    /* DLL is unloaded */
    case DLL_PROCESS_DETACH:
      rowlockIpcCleanup();
      break;
  }

  return TRUE;
}
#else
__attribute__((constructor)) static void constructor(){
  rowlockInitialize();
}

__attribute__((destructor)) static void destructor(){
  rowlockIpcCleanup();
}
#endif

/* Return 1 if the btree uses a transaction btree. */
static int transBtreeIsUsed(Btree *pBtree){
  BtreeTrans *pBtTrans = &pBtree->btTrans;
  return (pBtTrans->pBtree != NULL);
}

/* Return 1 if the cursor uses a transaction cursor. */
static int transBtreeCursorIsUsed(BtCursor *pCur){
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  return (pCurTrans->state != CURSOR_NOT_USE);
}

/* Return 1 if the shared cursor is using. */
#define cursorSharedIsUsed(pCur) ((pCur->state&CURSOR_USE_SHARED)!=0)

/* Return 1 if the transaction cursor is using. */
#define cursorTransIsUsed(pCur) ((pCur->state&CURSOR_USE_TRANS)!=0)

/* 
** Initialize a root page mapping which is a map of root page 
** between shared btree and transaction btree.
** Its structure is hash. Key is a root page number of shared
** btree. Value is a instance of TransRootPage structure.
*/
static void transRootPagesInit(HashI64 *pTransRootPages){
  sqlite3HashI64Init(pTransRootPages);
}

/* Finalize a root page mapping. */
static void transRootPagesFinish(HashI64 *pTransRootPages){
  HashElemI64 *elem = sqliteHashI64First(pTransRootPages);

  /* Cleanup for every element in a hash. */
  while( elem ){
    i64 iKey = sqliteHashI64Key(elem);
    TransRootPage *pData = (TransRootPage*)sqliteHashI64Data(elem);
    sqlite3KeyInfoUnref(pData->pKeyInfo);
    sqlite3_free(pData);
    /* Remove the element from hash. */
    sqlite3HashI64Insert(pTransRootPages, iKey, NULL);
    elem = sqliteHashI64First(pTransRootPages);
  }
  sqlite3HashI64Clear(pTransRootPages);
}

/*
** Open transaction databases.
*/
static int sqlite3TransBtreeOpen(
  Btree *pBtree,        /* Pointer to Btree object created in */
  int flags,            /* Flags used to open btree */
  int vfsFlags          /* vfsFlags used to open btree */
){
  int rc;
  sqlite3 *db = pBtree->db;
  Btree *pBtreeTrans = NULL;
  static const int transFlags =
    BTREE_TRANS;
  static const int transVfsFlags =
    SQLITE_OPEN_READWRITE |
    SQLITE_OPEN_CREATE |
    SQLITE_OPEN_EXCLUSIVE |
    SQLITE_OPEN_DELETEONCLOSE |
    SQLITE_OPEN_TEMP_DB |
    SQLITE_OPEN_MEMORY;
  BtreeTrans *pBtTrans = &pBtree->btTrans;
  const char *dbFullPath;

  /*
  ** Only main database should manage data in transaction btree.
  ** Don't need to create trans Btree for Ephemeral.
  */
  if( (vfsFlags & SQLITE_OPEN_MAIN_DB)==0 ){
    pBtTrans->pBtree = NULL;
    return SQLITE_OK;
  }

  /* 
  ** Do not create transaction btree when pager is not asociated with
  ** file. This situation occurs by "ATTACH DATABASE '' AS dbname"
  ** which is executed by VACUUM.
  */
  dbFullPath = sqlite3PagerFilename(pBtree->pBt->pPager, 0);
  if( sqlite3_stricmp(dbFullPath, "")== 0 ) return SQLITE_OK;

  /* Open transaction Btree for Inserted record. */
  rc = sqlite3BtreeOpenOriginal(db->pVfs, 0, db, &pBtreeTrans,
    transFlags, transVfsFlags);
  if( rc ) return rc;

  /* Initialize BtreeTrans members. */
  memset(&pBtTrans->lockSavepoint, 0, sizeof(RowLockSavepoint));
  rc = sqlite3rowlockIpcInit(&pBtTrans->ipcHandle, sqlite3GlobalConfig.szMmapRowLock, sqlite3GlobalConfig.szMmapTableLock, pBtree, dbFullPath);
  if( rc ) goto trans_btree_open_failed;

  pBtTrans->pBtree = pBtreeTrans;
  transRootPagesInit(&pBtTrans->rootPages);

  return SQLITE_OK;

trans_btree_open_failed:
  sqlite3BtreeCloseOriginal(pBtreeTrans);
  return rc;
}

/* 
** Open shared btree and transaction btree.
*/
int sqlite3BtreeOpenAll(sqlite3_vfs *pVfs, const char *zFilename, sqlite3 *db, Btree **ppBtree, int flags, int vfsFlags){
  int rc;

  rc = sqlite3BtreeOpenOriginal(pVfs, zFilename, db, ppBtree, flags, vfsFlags);
  if( rc ) return rc;

  rc = sqlite3TransBtreeOpen(*ppBtree, flags, vfsFlags);
  if( rc ) sqlite3BtreeClose(*ppBtree);

  return rc;
}

/*
** Close transaction databases and invalidate cursors.
*/
static int sqlite3TransBtreeClose(Btree *pBtree){
  BtreeTrans *pBtTrans = &pBtree->btTrans;
  Btree *pBtreeTrans = pBtTrans->pBtree;

  if( pBtreeTrans ){
    sqlite3rowlockSavepointClose(&pBtTrans->lockSavepoint);
    sqlite3rowlockIpcFinish(&pBtTrans->ipcHandle);
    sqlite3BtreeClose(pBtreeTrans);
    transRootPagesFinish(&pBtTrans->rootPages);
  }
  memset(pBtTrans, 0, sizeof(BtreeTrans));

  return SQLITE_OK;
}

/*
** Close transation btree and shared btree.
*/
int sqlite3BtreeCloseAll(Btree *p){
  sqlite3TransBtreeClose(p);
  return sqlite3BtreeCloseOriginal(p);
}

/*
** Begin a transaction for transaction btree.
*/
int sqlite3TransBtreeBeginTrans(Btree *p, int wrflag){
  BtreeTrans *pBtTrans = &p->btTrans;
  Btree *pBtreeTrans = pBtTrans->pBtree;
  int rc;

  /* Do nothing if this is a transaction btree. */
  if( !pBtreeTrans ){
    return SQLITE_OK;
  }

  rc = sqlite3BtreeBeginTransOriginal(pBtreeTrans, wrflag, 0);
  if( rc ) return rc;

  rc = sqlite3TransBtreeSavepointCreate(p, p->db->nSavepoint);
  return rc;
}

/* Add new entry into a root page mapping. */
static TransRootPage *addTransRootPage(Btree *p, Pgno iTable, Pgno iInsTable, 
                                       Pgno iDelTable, struct KeyInfo *pKeyInfo){
  TransRootPage *pNew = NULL;
  TransRootPage *pOld = NULL;

  pOld = (TransRootPage*)sqlite3HashI64Find(&p->btTrans.rootPages, iTable);
  if( pOld ){
    /* If it already exists, it was created when delete all records. */
    pNew = pOld;
    assert( pNew->deleteAll==1 );
    assert( pNew->iIns==0 );
    assert( pNew->iDel==0 );
    assert( !pNew->pKeyInfo );
  }else{
    pNew = (TransRootPage*)sqlite3MallocZero(sizeof(TransRootPage));
    if( !pNew ) return NULL;

    /* Initialization */
    pNew->deleteAll = 0;

    pOld = (TransRootPage*)sqlite3HashI64Insert(&p->btTrans.rootPages, iTable, pNew);
    if( pOld == pNew ){
      sqlite3_free(pNew);
      return NULL;
    }
    assert( pOld==0 );
  }

  /* 
  ** Increment a reference counter of pKeyInfo. We use pKeyInfo at COMMIT.
  ** If we do not increment it, pKeyInfo will be freed when statement is closed 
  ** and we cannot use it at COMMIT.
  */
  sqlite3KeyInfoRef(pKeyInfo);

  pNew->iIns = iInsTable;
  pNew->iDel = iDelTable;
  pNew->pKeyInfo = pKeyInfo;

  return pNew;
}

/*
** TransRootPage **ppRootPage : The pointer of rootPage information in 
** transaction btree. This information is managed in the hash 
** (Btree.btTrans.rootPages). It should be freed when the hash is 
** cleared. 
*/
int sqlite3TransBtreeCreateTable(Btree *p, int iTable, struct KeyInfo *pKeyInfo, 
                                 TransRootPage **ppRootPage){
  int rc = SQLITE_OK;
  int flags;
  BtreeTrans *pBtTrans = &p->btTrans;
  Btree *pBtreeTrans = pBtTrans->pBtree;
  int iInsTable;
  int iDelTable = 0;
  int iMoved;
  TransRootPage *pNew = NULL;

  if( pKeyInfo ){
    /* Index creation */
    flags = BTREE_BLOBKEY;
  }else{
    /* Table creation */
    flags = BTREE_INTKEY;
  }

  if( pBtreeTrans->pBt->inTransaction!=TRANS_WRITE ){
    rc = sqlite3TransBtreeBeginTrans(p, 2);
    if( rc ) return rc;
  }

  rc = sqlite3BtreeCreateTableOriginal(pBtreeTrans, &iInsTable, flags);
  if( rc ) return rc;

  rc = sqlite3BtreeCreateTableOriginal(pBtreeTrans, &iDelTable, flags);
  if( rc ) goto trans_btree_create_table_failed;

  /* Associates iTable to iInsTable and iDelTable. */
  pNew = addTransRootPage(p, iTable, iInsTable, iDelTable, pKeyInfo);
  if( !pNew ){
    rc = SQLITE_NOMEM_BKPT;
    goto trans_btree_create_table_failed;
  }

  rc = sqlite3rowlockHistoryAddNewTable(&pBtTrans->lockSavepoint, iTable);
  if( rc ) goto trans_btree_create_table_failed;

  if( ppRootPage ){
    *ppRootPage = pNew;
  }

  return SQLITE_OK;

trans_btree_create_table_failed:
  sqlite3BtreeDropTable(pBtreeTrans, iInsTable, &iMoved);
  if( iDelTable>0 ) sqlite3BtreeDropTable(pBtreeTrans, iDelTable, &iMoved);
  return rc;
}

/* 
** Drop tables in  transaction btree. The talbe number of shared btree is 
** spevified by an argument. We delete 2 tables. One is a insertion table, 
** another is adeletion table.
*/
static int transBtreeDropTable(Btree *p, int iTable){
  int rc = SQLITE_OK;
  BtreeTrans *pBtTrans = &p->btTrans;
  Btree *pBtreeTrans = pBtTrans->pBtree;
  TransRootPage *pRootPage;
  int iMoved;
  TransRootPage *pData = NULL;

  if( !transBtreeIsUsed(p) ){
    return SQLITE_OK;
  }

  pRootPage = (TransRootPage*)sqlite3HashI64Find(&p->btTrans.rootPages, iTable);
  if( !pRootPage ){
    return SQLITE_OK;
  }

  if( pRootPage->iIns>0 ){
    assert( pRootPage->iDel>0 );
    rc = sqlite3BtreeDropTableOriginal(pBtreeTrans, pRootPage->iIns, &iMoved);
    if( rc ) return rc;
    rc = sqlite3BtreeDropTableOriginal(pBtreeTrans, pRootPage->iDel, &iMoved);
  }
  /* Delete the mapping and Free memory in DROP table command.*/
  pData = (TransRootPage*)sqlite3HashI64Insert(&p->btTrans.rootPages, iTable, NULL);
  sqlite3KeyInfoUnref(pData->pKeyInfo);
  sqlite3_free(pData);
  return rc;
}

/*
** Drop tables in both shared btree and transaction btree.
*/
int sqlite3BtreeDropTableAll(Btree *p, int iTable, int *piMoved){
  int rc = SQLITE_OK;
  if( transBtreeIsUsed(p) ){
    rc = transBtreeDropTable(p, iTable);
    if( rc ) return rc;
  }
  rc = sqlite3BtreeDropTableOriginal(p, iTable, piMoved);
  if( rc ) return rc;

  /* Delete cachedRowid if it is a table. */
  if( transBtreeIsUsed(p) && !rowlockIsIndex(p, iTable) ){
    sqlite3rowlockIpcCachedRowidDropTable(&p->btTrans.ipcHandle, iTable);
  }

  return SQLITE_OK;
}

/* 
** Count the record in a table. 
** In order to call sqlite3BtreeCount(), we create a cursor. 
*/
static int btreeCountEntry(Btree *p, int iTable, i64 *pnEntry){
  i64 nEntry = 0;                      /* Value to return in *pnEntry */
  int rc;                              /* Return code */
  BtCursor *pCur = NULL;

  pCur = (BtCursor*)sqlite3MallocZero(sizeof(BtCursor));
  if( !pCur ) return SQLITE_NOMEM_BKPT;

  rc = sqlite3BtreeCursorOriginal(p, iTable, 0, NULL, pCur);
  if( rc ){
    sqlite3_free(pCur);
    return rc;
  }

  rc = sqlite3BtreeCount(pCur, &nEntry);
  sqlite3BtreeCloseCursorOriginal(pCur);
  sqlite3_free(pCur);
  if( rc ) return rc;

  *pnEntry = nEntry;
  return SQLITE_OK;
}

/*
** Convert a btree pointer into the iDb index that indicates
** which database file in db->aDb[] the btree refers to.
** Refer sqlite3SchemaToIndex() to understand the logic.
*/
static int rowlockBtreeToIndex(sqlite3 *db, Btree *p){
  int i = -1000000;

  assert( sqlite3_mutex_held(db->mutex) );
  assert( p );
  for(i=0; 1; i++){
    assert( i<db->nDb );
    if( db->aDb[i].pBt==p ){
      break;
    }
  }
  assert( i>=0 && i<db->nDb );
  return i;
}

/* Judge if it is table or index. */
static u8 rowlockIsIndex(Btree *p, int rootPage){
  sqlite3 *db = p->db;
  int iDb;
  Schema *pSchema;
  HashElem *i;

  iDb = rowlockBtreeToIndex(db, p);
  assert( sqlite3SchemaMutexHeld(db, iDb, 0) );
  pSchema = db->aDb[iDb].pSchema;

  for(i=sqliteHashFirst(&pSchema->idxHash); i; i=sqliteHashNext(i)){
    Index *pIdx = (Index*)sqliteHashData(i);
    if( pIdx->tnum==rootPage ){
      return 1;
    }
  }

  return 0;
}

/*
** Check if on one have a record lock in a table.
** Return SQLITE_OK if no one have a record lock in table.
** Return SQLITE_LOCKED if someone have a lock for any one of records in table.
*/
static int rowlockLockRecordsQuery(Btree *p, int iTable){
  int rc = SQLITE_OK;
  BtCursor *pCur;
  int res;

  if( rowlockIsIndex(p,iTable) ){
    /* Do nothing if it is an index. */
    return SQLITE_OK;
  }

  pCur = (BtCursor*)sqlite3MallocZero(sizeof(BtCursor));
  if( !pCur ) return SQLITE_NOMEM_BKPT;

  rc = sqlite3BtreeCursorOriginal(p, iTable, 0, NULL, pCur);
  if( rc ) goto rowlock_lock_records_query_end_1;

  rc = sqlite3BtreeFirst(pCur, &res);
  if( rc ) goto rowlock_lock_records_query_end_2;
  if( res==0 ){
    do{
      i64 rowid = sqlite3BtreeIntegerKeyOriginal(pCur);
      rc = sqlite3rowlockIpcLockRecordQuery(&p->btTrans.ipcHandle, iTable, rowid);
      if( rc ) goto rowlock_lock_records_query_end_2;

      rc = sqlite3BtreeNext(pCur, 0);
    } while( rc==SQLITE_OK );
    if( rc != SQLITE_DONE ) goto rowlock_lock_records_query_end_2;
    assert( rc==SQLITE_DONE );
    rc = SQLITE_OK;
  }
  /* Here, no one have a lock for all records in the table. */

rowlock_lock_records_query_end_2:
  sqlite3BtreeCloseCursorOriginal(pCur);
rowlock_lock_records_query_end_1:
  sqlite3_free(pCur);

  return rc;
}

/* Get a table lock. If it is successful, we memorize locked table id into history. */
int sqlite3rowlockIpcLockTableAndAddHistory(Btree *p, int iTable, u8 eLock){
  int rc = SQLITE_OK;
  u8 prevLock = eLock;

  if( !transBtreeIsUsed(p) ) return SQLITE_OK;

  rc = sqlite3rowlockIpcLockTable(&p->btTrans.ipcHandle, iTable, eLock, MODE_LOCK_NORMAL, &prevLock);
  if( rc ) return rc;

  if( eLock>prevLock ){
    rc = sqlite3rowlockHistoryAddTableLock(&p->btTrans.lockSavepoint, iTable, prevLock);
  }

  return rc;
}

/*
** Delete all records in a table.
** We just set a flag for the performance.
*/
int sqlite3TransBtreeClearTable(Btree *p, int iTable, int *pnChange){
  if( transBtreeIsUsed(p) ){
    int rc = SQLITE_OK;
    BtreeTrans *pBtTrans = &p->btTrans;
    HashI64 *pTransRootPages = &pBtTrans->rootPages;
    TransRootPage *pRootPage = NULL;
    int nChangeIns = 0;

    /* Check if someone has a table lock. */
    rc = sqlite3rowlockIpcLockTableAndAddHistory(p, iTable, WRITEEX_LOCK);
    if( rc ) return rc;

    /* Check if someone has a record lock in the table. */
    rc = rowlockLockRecordsQuery(p, iTable);
    if( rc ) return rc;

    pRootPage = (TransRootPage*)sqlite3HashI64Find(pTransRootPages, 
                                                   iTable);
    if( pRootPage ){
      /* Delete all records in transaction btree. */
      if( pRootPage->iIns>0 ){
        rc = sqlite3BtreeClearTable(pBtTrans->pBtree, pRootPage->iIns, 
                                    (pnChange ? &nChangeIns : 0));
        if( rc ) return rc;
      }
    }else{
      pRootPage = addTransRootPage(p, iTable, 0, 0, NULL);
      if( !pRootPage ) return SQLITE_NOMEM_BKPT;
    }

    /* Set the flag */
    pRootPage->deleteAll = 1;

    /* Count deleted record number if necessary. */
    if( pnChange ){
      i64 nChange = 0;
      rc = btreeCountEntry(p, iTable, &nChange);
      if( rc ) return rc;
      *pnChange = nChange + nChangeIns; /* ToDo: sqlite3BtreeClearTable cannot return nChange by i64 type. */
    }

    return SQLITE_OK;
  }else{
    return sqlite3BtreeClearTable(p, iTable, pnChange);
  }
}

static int rowlockIsSequenceTable(BtCursor *pCur){
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  return pCurTrans->isSeqTbl;
}

/* 
** Judge whether the cursor is of sqlite_sequence table or not.
** The result is stored into pCurTrans->isSeqTbl.
*/
static void rowlockJudgeSequenceTable(Btree *p, BtCursor *pCur){
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  Schema *pSchema;
  int iDb = rowlockBtreeToIndex(p->db, p);

  assert( sqlite3SchemaMutexHeld(p->db, iDb, 0) );
  pSchema = p->db->aDb[iDb].pSchema;
  if( pSchema->pSeqTab && (i64)pSchema->pSeqTab->tnum==(i64)pCur->pgnoRoot ){
    pCurTrans->isSeqTbl = 1;
  }else{
    pCurTrans->isSeqTbl = 0;
  }
}

/*
** Create cursors for insertion table and deletion talbe in trancation btree.
*/
static int transBtreeCursor(
  Btree *p,                                   /* The btree */
  int iTable,                                 /* Root page of table to open */
  int wrFlag,                                 /* 1 to write. 0 read-only */
  struct KeyInfo *pKeyInfo,                   /* First arg to xCompare() */
  BtCursor *pCur                              /* Write new cursor here */
){
  int rc;
  BtreeTrans *pBtTrans = &p->btTrans;
  Btree *pBtreeTrans = pBtTrans->pBtree;
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  BtCursor *pCurTransIns;
  BtCursor *pCurTransDel;
  TransRootPage *pRootPage;
  int iInsTable;
  int iDelTable;

  /* Do nothing if it is transaction btree. */
  if( iTable==1 || !transBtreeIsUsed(p) ){
    pCurTrans->pCurIns = NULL;
    pCurTrans->pCurDel = NULL;
    pCurTrans->state = CURSOR_NOT_USE;
    return SQLITE_OK;
  }

  /*
  ** Search root page from transaction btree.
  ** If not found, create new table.
  */
  pRootPage = (TransRootPage*)sqlite3HashI64Find(&pBtTrans->rootPages, iTable);
  if( !pRootPage || pRootPage->iIns==0 ){
    TransRootPage *pRootPage = NULL;
    rc = sqlite3TransBtreeCreateTable(p, iTable, pKeyInfo, &pRootPage);
    if( rc ) return rc;
    iInsTable = pRootPage->iIns;
    iDelTable = pRootPage->iDel;
  }else{
    iInsTable = pRootPage->iIns;
    iDelTable = pRootPage->iDel;
  }

  pCurTransIns = (BtCursor*)sqlite3MallocZero(sizeof(BtCursor));
  if( !pCurTransIns ){
    return SQLITE_NOMEM_BKPT;
  }
  pCurTransDel = (BtCursor*)sqlite3MallocZero(sizeof(BtCursor));
  if( !pCurTransDel ){
    sqlite3_free(pCurTransIns);
    return SQLITE_NOMEM_BKPT;
  }

  rc = sqlite3BtreeCursorOriginal(pBtreeTrans, iInsTable, wrFlag, pKeyInfo, 
                                pCurTransIns);
  if( rc ) goto trans_btree_cursor_failed;

  rc = sqlite3BtreeCursorOriginal(pBtreeTrans, iDelTable, wrFlag, pKeyInfo, 
                                pCurTransDel);
  if( rc ) goto trans_btree_cursor_failed;

  pCurTrans->pCurIns = pCurTransIns;
  pCurTrans->pCurDel = pCurTransDel;
  pCurTrans->state = CURSOR_USE_SHARED;
  rowlockJudgeSequenceTable(p, pCur);

  return SQLITE_OK;

trans_btree_cursor_failed:
  sqlite3BtreeCloseCursorOriginal(pCurTransIns);
  sqlite3BtreeCloseCursorOriginal(pCurTransDel);
  sqlite3_free(pCurTransIns);
  sqlite3_free(pCurTransDel);
  return rc;
}

/*
** Create cursors for shared btree and transaction btree.
** Cursor for shared btree is read only, cursor for transaction btree depends on the flag.
** Please see sqlite3BtreeBeginTrans(). Write transaction for shared btree is not started.
** But we create a write cursor for sqlite_master because it prevents from modification of
** sqlite_master from multiple users. So we have to start a write transaction before 
** creating a write cursor for sqlite_master.
**
** This function is called from during SQL execution and commit. The 6th argument 'flag' 
** indicates it. If 'flag' is 0, it is in SQL execution and this function behaves as above.
** If 'flag' is 1, it is in commit process and we can create a write cursor for shared
** btree. A write transaction was already started by sqlite3BtreeBeginTransForCommit().
*/
int sqlite3BtreeCursorAll(Btree *p, int iTable, int wrFlag, 
                          struct KeyInfo *pKeyInfo, BtCursor *pCur, int flag){
  int rc;

  if( p->sharable && iTable==1 && wrFlag>0 && !sqlite3BtreeIsInTransOriginal(p) ){
    rc = sqlite3BtreeBeginTransOriginal(p, 1, 0);
    if( rc ) return rc;
  }

  sqlite3BtreeCursorZero(pCur);
  if( p->sharable && iTable!=1 && !flag ){
    rc = sqlite3BtreeCursorOriginal(p, iTable, 0, pKeyInfo, pCur);
  }else{
    rc = sqlite3BtreeCursorOriginal(p, iTable, wrFlag, pKeyInfo, pCur);
  }
  if( rc ) return rc;

  rc = transBtreeCursor(p, iTable, wrFlag, pKeyInfo, pCur);
  if( rc ){
    sqlite3BtreeCloseCursorOriginal(pCur);
  }

  return rc;
}

/* Close sursors for transaction btree. */
static int sqlite3TransBtreeCloseCursor(BtCursor *pCur){
  if( transBtreeCursorIsUsed(pCur) ){
    BtCursorTrans *pCurTrans = &pCur->btCurTrans;
    sqlite3BtreeCloseCursorOriginal(pCurTrans->pCurIns);
    sqlite3BtreeCloseCursorOriginal(pCurTrans->pCurDel);
    sqlite3_free(pCurTrans->pCurIns);
    sqlite3_free(pCurTrans->pCurDel);
    pCurTrans->pCurIns = NULL;
    pCurTrans->pCurDel = NULL;
    pCurTrans->state = CURSOR_NOT_USE;
    pCurTrans->deleteAll = 0;
  }
  return SQLITE_OK;
}

/* Close sursors for both shared btree and transaction btree. */
int sqlite3BtreeCloseCursorAll(BtCursor *pCur){
  Btree *pBtree = pCur->pBtree;
  if( pBtree ) sqlite3TransBtreeCloseCursor(pCur);
  return sqlite3BtreeCloseCursorOriginal(pCur);
}

/* Compare key pointed by cursors. */
static int btreeKeyCompareCursors(BtCursor *pCur1, BtCursor *pCur2, i64 *pRet){
  if( !pCur1->pKeyInfo ){
    /* Table case */
    i64 nKey1 = sqlite3BtreeIntegerKeyOriginal(pCur1);
    i64 nKey2 = sqlite3BtreeIntegerKeyOriginal(pCur2);
    *pRet = nKey1 - nKey2;
    return SQLITE_OK;
  }else{
    /* Index case */
    int rc = SQLITE_OK;             /* Status code */
    UnpackedRecord *pIdxKey = NULL; /* Unpacked index key */
    void *pKey1, *pKey2;            /* Packed key */
    u32 nKey1, nKey2;               /* Size of pKey */
    RecordCompare xRecordCompare;

    assert( pCur2->pKeyInfo );
    /* 
    ** Calculate pKey and nKey which is required for calling 
    ** the comparison function.
    */
    nKey1 = sqlite3BtreePayloadSizeOriginal(pCur1);
    assert( nKey1==(i64)(int)nKey1 );
    nKey2 = sqlite3BtreePayloadSizeOriginal(pCur2);
    assert( nKey2==(i64)(int)nKey2 );

    pKey1 = (char*)sqlite3DbMallocZero(pCur1->pKeyInfo->db, nKey1);
    if( !pKey1 ){
      return SQLITE_NOMEM_BKPT;
    }
    pKey2 = (char*)sqlite3DbMallocZero(pCur2->pKeyInfo->db, nKey2);
    if( !pKey2 ){
      rc = SQLITE_NOMEM_BKPT;
      goto btree_idxkey_compare_done;
    }

    rc = sqlite3BtreePayloadOriginal(pCur1, 0, nKey1, pKey1);
    if( rc ) goto btree_idxkey_compare_done;
    rc = sqlite3BtreePayloadOriginal(pCur2, 0, nKey2, pKey2);
    if( rc ) goto btree_idxkey_compare_done;

    pIdxKey = sqlite3VdbeAllocUnpackedRecord(pCur2->pKeyInfo);
    if( pIdxKey==0 ) {
      rc = SQLITE_NOMEM_BKPT;
      goto btree_idxkey_compare_done;
    }
    sqlite3VdbeRecordUnpack(pCur2->pKeyInfo, (int)nKey2, pKey2, pIdxKey);
    if( pIdxKey->nField==0 ){
      rc = SQLITE_CORRUPT_BKPT;
      goto btree_idxkey_compare_done;
    }

    /* Compare index keys. */
    xRecordCompare = sqlite3VdbeFindCompare(pIdxKey);
    assert( xRecordCompare );
    /* For a WITHOUT ROWID table. nField is used as the number of key columns
    ** in xRecordCompare. See the comment of btreeCursorMovetoKey(). */
    pIdxKey->nField = pIdxKey->pKeyInfo->nKeyField;
    *pRet = xRecordCompare(nKey1, pKey1, pIdxKey);

btree_idxkey_compare_done:
    sqlite3DbFree(pCur1->pKeyInfo->db, pKey1);
    sqlite3DbFree(pCur2->pKeyInfo->db, pKey2);
    sqlite3DbFree(pCur2->pKeyInfo->db, pIdxKey);
    return rc;
  }
}

/* Return 1 if thecursor is pointing valid record. */
static int btreeCursorIsPointing(BtCursor *pCur){
  return pCur && pCur->eState==CURSOR_VALID;
}

/* 
** Move a cursor on the basis of key info.
** There are 3 paterns about a record structure.
** Record data consists of record-key and record-value.
** 1. pKey is NULL. This case is a normal table.
*: 2. pKey has record-key. This case is an index.
** 3. pKey has record-key and record-value. This case is a WITHOUT ROWID table.
** For the pattern-3, this function compares record by only record-key.
*/
static int btreeCursorMovetoKey(BtCursor *pCur, const void *pKey, i64 nKey, int *pRet){
  int rc = SQLITE_OK; /* Status code */
  UnpackedRecord *pIdxKey = NULL; /* Unpacked index key */

  if( pCur->pKeyInfo ){
    /* Create unpacked record. */
    pIdxKey = sqlite3VdbeAllocUnpackedRecord(pCur->pKeyInfo);
    if( pIdxKey==0 ) return SQLITE_NOMEM_BKPT;

    sqlite3VdbeRecordUnpack(pCur->pKeyInfo, (int)nKey, pKey, pIdxKey);
    if( pIdxKey->nField==0 ){
      sqlite3DbFree(pCur->pKeyInfo->db, pIdxKey);
      return SQLITE_CORRUPT_BKPT;
    }

    /* For the pattern-3, the comparison is done by only record-key. */
    pIdxKey->nField = pIdxKey->pKeyInfo->nKeyField;
  }

  /* Search for pCur. */
  rc = sqlite3BtreeMovetoUnpacked(pCur, pIdxKey, nKey, 0, pRet);
  if( pCur->pKeyInfo ) sqlite3DbFree(pCur->pKeyInfo->db, pIdxKey);

  return rc;
}

/* This is called instead of sqlite3BtreeInsert. */
int sqlite3TransBtreeInsert(
  BtCursor *pCur,                /* Insert data into the table of this cursor */
  const BtreePayload *pX,        /* Content of the row to be inserted */
  int flags,                     /* True if this is likely an append */
  int seekResult                 /* Result of prior MovetoUnpacked() call */
){
  int rc = SQLITE_OK;
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  BtCursor *pCurIns = pCurTrans->pCurIns;
  BtCursor *pCurDel = pCurTrans->pCurDel;
  int btFlags = flags;
  int res = 1;
  int isUpdate = 0;

  if( !pCurIns ){
    return sqlite3BtreeInsert(pCur, pX, flags, seekResult);
  }

  /* Judge whether it is update case or append case.
  ** If OPFLAG_APPEND is specified or seekResult is not 0, it means the append case.
  ** Otherwise, we check it by confirming the existence of record in shared btree.
  ** If a record is found, it means the update case, else the append case.
  */
  if( !(flags&OPFLAG_APPEND) && seekResult==0 ){
    if( cursorSharedIsUsed(pCurTrans) ){
      /* If a record exists in shared btree, it should be updated. */
      rc = btreeCursorMovetoKey(pCur, pX->pKey, pX->nKey, &res);
      if( rc ) return rc;
      btFlags = BTREE_APPEND;
      if( res==0 ){
        isUpdate = 1;
      }
    }
  }

  /*
  ** We don't get a record lock for index and sqlite_sequence table. sqlite_sequence is used
  ** for auto-increment feature.
  */
  if( !rowlockIsSequenceTable(pCur) && !pCur->pKeyInfo ){
    /* Lock record */
    rc = sqlite3rowlockIpcLockRecord(&pCur->pBtree->btTrans.ipcHandle, pCur->pgnoRoot, pX->nKey);
    if( rc==SQLITE_DONE ){
      /* Do nothing if it already has a lock. We can insert new record. */
    }else if( rc ){
      if( rc==SQLITE_LOCKED && cachedRowidFlagGet(pCur)==1 ){
        /* If it is already locked, a record having same rowid is already inserted
        ** by the other process. In this case, if rowid is issued automatically,
        ** we need to retry the query execution. Therefore we return an special
        ** error code.
        */
        return SQLITE_CORRUPT_ROWID;
      }
      return rc;
    }else{
      rc = sqlite3rowlockHistoryAddRecord(&pCur->pBtree->btTrans.lockSavepoint, pCur->pgnoRoot, pX->nKey);
      if( rc ) return rc;
    }
  }

  /* If update case, we add a record into a delete table. */
  if( isUpdate ){
    rc = sqlite3BtreeInsert(pCurDel, pX, 0, 0);
    if( rc ) return rc;
  }

  rc = sqlite3BtreeInsert(pCurIns, pX, btFlags, 0);
  if( rc ) return rc;

  rc = btreeMovetoOriginal(pCurIns, pX->pKey, pX->nKey, 0, &res);
  pCurTrans->state = CURSOR_USE_SHARED | CURSOR_USE_TRANS;
  assert( sqlite3BtreeCursorIsValid(pCurIns) );

  return rc;
}

/*
** This is called instead of sqlite3BtreeDelete.
** Remember Deleted record in a transaction.
*/
int sqlite3TransBtreeDelete(BtCursor *pCur, u8 flags){
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  BtCursor *pCurIns =  pCurTrans->pCurIns;
  BtCursor *pCurDel =  pCurTrans->pCurDel;

  if( !pCurIns ){
    return sqlite3BtreeDelete(pCur, flags);
  }

  if( cursorTransIsUsed(pCurTrans) ){
    /*
    ** If the cursor is pointing a transaction tree, the row was
    ** added in a transaction. So we delete the row from
    ** transaction btree.
    */
    return sqlite3BtreeDelete(pCurIns, flags);
  }else{
    /*
    ** If the curor is pointing shared btree, we delete the row
    ** shared btree.
    */
    int rc = SQLITE_OK;        /* Status code */
    i64 nKey = 0;
    void *pKey = NULL;
    BtreePayload x = {0};   /* Payload to be inserted */

    if( pCur->pKeyInfo ){
      /* For an index btree. */
      nKey = sqlite3BtreePayloadSizeOriginal(pCur);
      assert( nKey==(i64)(int)nKey );
      pKey = (char*)sqlite3MallocZero(nKey);
      if( !pKey ){
        return SQLITE_NOMEM_BKPT;
      }
      rc = sqlite3BtreePayloadOriginal(pCur, 0, (u32)nKey, pKey);
      if( rc ) goto trans_btree_delete_end;
    }else{
      /* For a table btree. */
      nKey = sqlite3BtreeIntegerKeyOriginal(pCur);
      pKey = NULL;

      /* Lock record */
      rc = sqlite3rowlockIpcLockRecord(&pCur->pBtree->btTrans.ipcHandle, pCur->pgnoRoot, nKey);
      if( rc==SQLITE_DONE ){
        /* Do nothing */
      }else if( rc ){
        return rc;
      }else{
        rc = sqlite3rowlockHistoryAddRecord(&pCur->pBtree->btTrans.lockSavepoint, pCur->pgnoRoot, nKey);
        if( rc ) return rc;
      }
    }

    /* Operate INSERT */
    x.pKey = pKey;
    x.nKey = nKey;
    rc = sqlite3BtreeInsert(pCurDel, &x, OPFLAG_APPEND, 0);

trans_btree_delete_end:
    sqlite3_free(pKey);
    return rc;
  }
}

/* Search and move pCur to valid row. */
static int btreeSeekToExist(BtCursor *pCur, 
                     int (*xAdvance)(BtCursor*, int),
                     int flags){
  int rc = SQLITE_OK;
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  BtCursor *pCurIns =  pCurTrans->pCurIns;
  BtCursor *pCurDel =  pCurTrans->pCurDel;

  while( btreeCursorIsPointing(pCur) && 
          btreeCursorIsPointing(pCurDel) ){
    i64 cmp = 0;
    rc = btreeKeyCompareCursors(pCur, pCurDel, &cmp);
    if( rc ) return rc;
    if( (xAdvance==sqlite3BtreeNext && cmp>0) ||
        (xAdvance==sqlite3BtreePrevious && cmp<0) ){
      /* Move pCurDel to the nearest row of pCur. */
      rc = xAdvance(pCurDel, flags);
    }else if( cmp==0 ){
      /* 101: Record in shared btree is deleted. */
      rc = xAdvance(pCur, flags);
    }else{
      /* pCur is pointing a valid row. */
      return SQLITE_OK;
    }
    if( rc!=SQLITE_OK && rc!=SQLITE_DONE ) return rc;
  }
  return SQLITE_OK;
}

/*
** Move to existing row from current position
** Records are sorted by the key.
** If xAdvance is btreeNext, search the minimum record which is greater than current value.
** If xAdvance is btreePrevious, search the maximum record which is smaller than current value.
**
** (pCur,pCurIns,pCurDel)
** (111) - Record was updated
** (110) - Same key records
** (101) - Record was deleted
** (100) - Record is not updated and not deleted
** (011) - Nerver occur
** (010) - Record was inserted
** (001) - Nerver occur
** (000) - No record
*/
static int btreeSeekToExistAll(BtCursor *pCur, 
                     int (*xAdvance)(BtCursor*, int),
                     int flags, int *pRes){
  int rc = SQLITE_OK;
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  BtCursor *pCurIns =  pCurTrans->pCurIns;
  BtCursor *pCurDel =  pCurTrans->pCurDel;

  /* Move pCur to valid row. */
  rc = btreeSeekToExist(pCur, xAdvance, flags);
  if( rc ) return rc;

  if( btreeCursorIsPointing(pCur) ){
    if( btreeCursorIsPointing(pCurIns) ){
      /* Both pCur and pCurIns are valid. */
      i64 cmp = 0;
      rc = btreeKeyCompareCursors(pCur, pCurIns, &cmp);
      if( rc ) return rc;

      if( cmp == 0 ||
          (cmp<0 && xAdvance==sqlite3BtreeNext) ||
          (cmp>0 && xAdvance==sqlite3BtreePrevious) ){
        /* 100: Record in shared btree is valid. */
        /* 110: Same key records. We use a record in shared btree firstly. */
        pCurTrans->state = CURSOR_USE_SHARED;
      }else{
        /* 010: Record exists in trans btree. */
        pCurTrans->state = CURSOR_USE_TRANS;
      }
    }else{
      /* 100: Record in shared btree is valid. */
      pCurTrans->state = CURSOR_USE_SHARED;
    }
    *pRes = 0;
  }else{
    if( btreeCursorIsPointing(pCurIns) ){
      /* 010: Only pCurIns is valid. */
      pCurTrans->state = CURSOR_USE_TRANS;
      *pRes = 0;
    }else{
      /* Both pCur and pCurIns are invalid. */
      *pRes = 1;
    }
  }
  return SQLITE_OK;
}

/* Move cursors to the first position. */
int sqlite3BtreeFirstAll(BtCursor *pCur, int *pRes){
  int rc = SQLITE_OK;
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  TransRootPage *pRootPage = NULL;
  int res = 1;
  int resIns = 1;
  int resDel = 1;

  rc = sqlite3BtreeFirst(pCur, &res);
  if( rc ) return rc;

  if( !transBtreeCursorIsUsed(pCur) ){
    *pRes = res;
    return SQLITE_OK;
  }

  rc = sqlite3BtreeFirst(pCurTrans->pCurIns, &resIns);
  if( rc ) return rc;

  rc = sqlite3BtreeFirst(pCurTrans->pCurDel, &resDel);
  if( rc ) return rc;
  
  /* Check if all records in shared btree are deleted. */
  pRootPage = (TransRootPage*)sqlite3HashI64Find(&pCur->pBtree->btTrans.rootPages, pCur->pgnoRoot);
  assert( pRootPage );
  if( pRootPage->deleteAll ){
    pCur->eState = CURSOR_INVALID;
  }

  rc = btreeSeekToExistAll(pCur, sqlite3BtreeNext, 0, pRes);
  return rc;
}

/* Move cursors to the last position. */
int sqlite3BtreeLastAll(BtCursor *pCur, int *pRes){
  int rc = SQLITE_OK;
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  TransRootPage *pRootPage = NULL;
  int res = 1;
  int resIns = 1;
  int resDel = 1;

  rc = sqlite3BtreeLast(pCur, &res);
  if( rc ) return rc;

  if( !transBtreeCursorIsUsed(pCur) ){
    *pRes = res;
    return SQLITE_OK;
  }

  rc = sqlite3BtreeLast(pCurTrans->pCurIns, &resIns);
  if( rc ) return rc;

  rc = sqlite3BtreeLast(pCurTrans->pCurDel, &resDel);
  if( rc ) return rc;

  /* Check if all records in shared btree are deleted. */
  pRootPage = (TransRootPage*)sqlite3HashI64Find(&pCur->pBtree->btTrans.rootPages, pCur->pgnoRoot);
  assert( pRootPage );
  if( pRootPage->deleteAll ){
    pCur->eState = CURSOR_INVALID;
  }

  return btreeSeekToExistAll(pCur, sqlite3BtreePrevious, 0, pRes);
}

/* Move cursors to the next position. */
int sqlite3BtreeAdvanceAll(BtCursor *pCur, int flags, int(*xAdvance)(BtCursor*, int)){
  int rc;
  int res;
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  BtCursor *pCurIns = pCurTrans->pCurIns;

  assert( xAdvance==sqlite3BtreePrevious || xAdvance==sqlite3BtreeNext );

  if( !transBtreeCursorIsUsed(pCur) ){
    return xAdvance(pCur, flags);
  }

  if( cursorSharedIsUsed(pCurTrans) ){
    rc = xAdvance(pCur, flags);
    if( rc!=SQLITE_OK && rc!=SQLITE_DONE ) return rc;
  }

  if( cursorTransIsUsed(pCurTrans) ){
    rc = xAdvance(pCurIns, flags);
    if( rc!=SQLITE_OK && rc!=SQLITE_DONE ) return rc;
  }

  /* validate the cursor. */
  rc = btreeSeekToExistAll(pCur, xAdvance, flags, &res);
  if( rc ) return rc;
  if( res ){
    return SQLITE_DONE;
  }else{
    return SQLITE_OK;
  }
}

#ifndef SQLITE_OMIT_WINDOWFUNC
void sqlite3BtreeSkipNextAll(BtCursor *pCur){
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  BtCursor *pCurIns = pCurTrans->pCurIns;

  sqlite3BtreeSkipNextOriginal(pCur);
  if( transBtreeCursorIsUsed(pCur) ){
    sqlite3BtreeSkipNextOriginal(pCurIns);
    return;
  }
}
#endif /* SQLITE_OMIT_WINDOWFUNC */

/* Compare key specified by an argument with key pointed by cursor. */
static int btreeKeyCompareOfCursor(UnpackedRecord *pIdxKey, i64 intKey, BtCursor *pCur, i64 *pRes){
  if( !pIdxKey ){
    i64 nKey = sqlite3BtreeIntegerKeyOriginal(pCur);
    *pRes = intKey - nKey;
  }else{
    int rc;
    void *pKey;       /* Packed key */
    u32 nKey;         /* Size of pKey */
    RecordCompare xRecordCompare;

    assert( pCur->pKeyInfo );
    /* 
    ** Calculate pKey and nKey which is required for calling 
    ** the comparison function.
    */
    nKey = sqlite3BtreePayloadSizeOriginal(pCur);
    assert( nKey==(i64)(int)nKey );

    pKey = (char*)sqlite3DbMallocZero(pCur->pKeyInfo->db, nKey);
    if( !pKey ){
      rc = SQLITE_NOMEM_BKPT;
      goto btree_idxkey_compare_done;
    }

    rc = sqlite3BtreePayloadOriginal(pCur, 0, nKey, pKey);
    if( rc ) goto btree_idxkey_compare_done;

    xRecordCompare = sqlite3VdbeFindCompare(pIdxKey);
    *pRes = xRecordCompare(nKey, pKey, pIdxKey);

btree_idxkey_compare_done:
    sqlite3DbFree(pCur->pKeyInfo->db, pKey);
    if( rc ) return rc;
  }
  return SQLITE_OK;
}

/* 
** This function gathers cursors to one side from key. 
** In case of OP_SeekGE or OP_SeekGT,
**   - both cursors are moved to the right side from key.
**   - *pRes is set to 1 which indicates cursor is pointing at an entry that is larger than key
** In case of OP_SeekLE or OP_SeekLT,
**   - both cursors are moved to the left side from key.
**   - *pRes is set to -1 which indicates cursor is pointing at an entry that is smaller than key
** See the explanation of sqlite3BtreeMovetoUnpackedAll().
**
** res indicates where the pCur is pointing at. It is aqcuired by sqlite3MovetoUnpacked for pCur.
** resIns is that of pCurIns.
*/
static int sqlite3BtreeMovetoSameSide(BtCursor *pCur, int opcode, int res, int resIns, int *pRes){
  int rc;
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  BtCursor *pCurIns = pCurTrans->pCurIns;
  BtCursor *pCurTarget;
  i64 resCmp;
  int direction; /* 1 if opcode is OP_SeekGE or OP_SeekGT, else -1. It is also used for *pRes. */
  int(*xAdvance)(BtCursor*, int);

  assert( opcode==OP_SeekGE || opcode==OP_SeekGT || opcode==OP_SeekLE || opcode==OP_SeekLT );
  assert( btreeCursorIsPointing(pCur) );
  assert( btreeCursorIsPointing(pCurIns) );

  if( opcode==OP_SeekGE || opcode==OP_SeekGT ){
    direction = 1;
    xAdvance = sqlite3BtreeNext;
  }else{
    direction = -1;
    xAdvance = sqlite3BtreePrevious;
  }

  /* 
  ** If cursors are pointing at the different side, we move cursor.
  ** In case of OP_SeekGE or OP_SeekGT, the cursor pointing at an 
  ** entry that is smaller than key is moved until pointing at larger 
  ** entry.
  ** In case of OP_SeekLE or OP_SeekLT, the cursor pointing at an 
  ** entry that is larger than key is moved until pointing at smaller 
  ** entry.
  */
  if( res*resIns<0 ){
    /* 
    ** Decide which sursor should be moved.
    ** direction*res means (direction==1 && res<0) || (direction==-1 && res>0)
    */
    if( (direction*res<0) ){
      pCurTarget = pCur;
    }else{
      pCurTarget = pCurIns;
    }

    /* Move cursor as it points at the same side. */
    rc = xAdvance(pCurTarget, 0);
    if( rc!=SQLITE_OK && rc!=SQLITE_DONE ) return rc;

    if( pCurTarget == pCur ){
      rc = btreeSeekToExist(pCurTarget, xAdvance, 0); 
      if( rc ) return rc;
    }

    /* Check if moved cursor is invalid. If it is invalid, we use another cursor. */
    if( !btreeCursorIsPointing(pCurTarget) ){
      if( pCurTarget==pCur ){
        pCurTrans->state = CURSOR_USE_TRANS;
      }else{
        pCurTrans->state = CURSOR_USE_SHARED;
      }
      *pRes = direction;
      return SQLITE_OK;
    }
  }

  /* Decide which cursor should be used. We use a closer cursor. */
  rc = btreeKeyCompareCursors(pCur, pCurIns, &resCmp);
  if( rc ) return rc;
  if( direction*resCmp<=0 ){
    pCurTrans->state = CURSOR_USE_SHARED;
  }else{
    pCurTrans->state = CURSOR_USE_TRANS;
  }

  *pRes = direction;
  return SQLITE_OK;
}

/* 
** Chenck if an entry which pCur points at is deleted.
** *pRet indicetes the check result. It is set by sqlite3MovetoUnpacked().
** If *pRet is set to 0, it means an entry is deleted. 
*/
static int btreeIsDeleted(BtCursor *pCur, int *pRet){
  int rc = SQLITE_OK;        /* Status code */
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  BtCursor *pCurDel = pCurTrans->pCurDel;
  void *pKey = NULL;       /* Packed key */
  i64 nKey;
  UnpackedRecord *pIdxKey = NULL;   /* Unpacked index key */

  if( !btreeCursorIsPointing(pCur) ){
    *pRet = -1;
    return SQLITE_OK;
  }

  if( !pCur->pKeyInfo ){
    nKey = sqlite3BtreeIntegerKeyOriginal(pCur);
  }else{
    /* 
    ** Calculate pKey and nKey which is required for calling 
    ** the comparison function.
    */
    nKey = sqlite3BtreePayloadSizeOriginal(pCur);
    assert( nKey==(i64)(int)nKey );

    pKey = (char*)sqlite3DbMallocZero(pCur->pKeyInfo->db, nKey);
    if( !pKey ) return SQLITE_NOMEM_BKPT;

    rc = sqlite3BtreePayloadOriginal(pCur, 0, (u32)nKey, pKey);
    if( rc ){
      sqlite3DbFree(pCur->pKeyInfo->db, pKey);
      return rc;
    }
  }

  /* Search for pCurDel. */
  rc = btreeCursorMovetoKey(pCurDel, pKey, nKey, pRet);
  if( pCur->pKeyInfo ) sqlite3DbFree(pCur->pKeyInfo->db, pKey);

  return rc;
}

/*
** Search both cursors(pCur and pCurIns) by the key.
** If it is found in either of cursors, pRes is 0.
** If it is not found, pRes is not 0 (-1 or 1) and we judge which cursor should 
** be used.
** When sqlite3BtreeMovetoUnpackedAll() is called from OP_SeekXX (OP_SeekLT, 
** OP_SeekLE, OP_SeekGE, OP_SeekGT), we have to take case of the search
** direction. After this function, the cursor is moved based on pRes value.
** If cursors are pointing the different side from the key (Ex: pCur is pointing
** at an entry that is larger than key and pCur is pointingat an entry that is 
** smaller than key), it is difficult to move cursors based on pRes. Therefore 
** both cursors has to be pointing the same side. This is implaemnted in 
** sqlite3BtreeMovetoSameSide().
*/
int sqlite3BtreeMovetoUnpackedAll(BtCursor *pCur, UnpackedRecord *pIdxKey, 
                                  i64 intKey, int biasRight, int *pRes, 
                                  int opcode){
  int rc;
  int res, resIns, resDel;
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  BtCursor *pCurIns = pCurTrans->pCurIns;
  BtCursor *pCurDel = pCurTrans->pCurDel;
  TransRootPage *pRootPage = NULL;

  /* Move pCur. */
  rc = sqlite3BtreeMovetoUnpacked(pCur, pIdxKey, intKey, biasRight, &res);
  if( rc ) return rc;

  if( !transBtreeCursorIsUsed(pCur) ){
    *pRes = res;
    return SQLITE_OK;
  }

  /* Check if the record pointed by pCur is valid. */
  pRootPage = (TransRootPage*)sqlite3HashI64Find(&pCur->pBtree->btTrans.rootPages, pCur->pgnoRoot);
  assert( pRootPage );
  if( pRootPage->deleteAll ){
    pCur->eState = CURSOR_INVALID;
    res = -1;
  }else{
    /* Check if an entry which pCur points at is deleted. */
    rc = btreeIsDeleted(pCur, &resDel);
    if( rc ) return rc;

    if( resDel==0 ){
      rc = sqlite3BtreeNext(pCur, 0);
      if( rc!=SQLITE_OK && rc!=SQLITE_DONE ) return rc;
      /* Move cursor to valid location */
      rc = btreeSeekToExist(pCur, sqlite3BtreeNext, 0); 
      if( rc ) return rc;
      if( btreeCursorIsPointing(pCur) ){
        res = 1;
      }else{
        /* 
        ** Not found a valid record in the next to current position.
        ** So search an entry that is smaller than intKey/pIdxKey.
        */
        rc = sqlite3BtreeMovetoUnpacked(pCur, pIdxKey, intKey, biasRight, &res);
        if( rc ) return rc;
        if( resDel==0 ){
          rc = sqlite3BtreePrevious(pCur, 0);
          if( rc!=SQLITE_OK && rc!=SQLITE_DONE ) return rc;
        }
        rc = btreeSeekToExist(pCur, sqlite3BtreePrevious, 0); 
        if( rc ) return rc;
        res = -1;
      }
    }
  }

  /* Search for transaction btree. */
  rc = sqlite3BtreeMovetoUnpacked(pCurIns, pIdxKey, intKey, biasRight, &resIns);
  if( rc ) return rc;

  /* Judge which cursor should be used. */
  if( resIns==0 ){
    /* Found in transaction btree. */
    *pRes = 0;
    pCurTrans->state = CURSOR_USE_TRANS;
  }else if( res==0 ){
    /* Found in shared btree. */
    *pRes = 0;
    pCurTrans->state = CURSOR_USE_SHARED;
  }else{
    /* 
     * Not found in both shared btree and transaction btree.
     * Check which cursor should be used.
     */
    if( btreeCursorIsPointing(pCur) ){
      if( btreeCursorIsPointing(pCurIns) ){
        /* Both cursors are valid. Check which cursor is closer to the key. */
        if( opcode==OP_SeekGE || opcode==OP_SeekGT || opcode==OP_SeekLE || opcode==OP_SeekLT ){
          rc = sqlite3BtreeMovetoSameSide(pCur, opcode, res, resIns, pRes);
          if( rc ) return rc;
        }else{
          *pRes = res;
          pCurTrans->state = CURSOR_USE_SHARED;
        }
      }else{
        /* Only cursor of share btree is valid. */
        *pRes = res;
        pCurTrans->state = CURSOR_USE_SHARED;
     }
    }else{
      /* Cursor of share btree is invalid. So use transaction cursor. */
      *pRes = resIns;
      pCurTrans->state = CURSOR_USE_TRANS;
    }
  }

  return SQLITE_OK;
}

/* Rollback of transaction btree. */
static void sqlite3TransBtreeRollback(Btree *p, int tripCode, int writeOnly){
  if( transBtreeIsUsed(p) ){
    BtreeTrans *pBtTrans = &p->btTrans;
    sqlite3BtreeRollbackOriginal(pBtTrans->pBtree, tripCode, writeOnly);
    sqlite3rowlockSavepointClose(&pBtTrans->lockSavepoint);
    transRootPagesFinish(&pBtTrans->rootPages);
    sqlite3rowlockIpcUnlockRecordProc(&pBtTrans->ipcHandle, NULL);
    sqlite3rowlockIpcUnlockTablesProc(&pBtTrans->ipcHandle, NULL);
    sqlite3rowlockIpcCachedRowidReset(&pBtTrans->ipcHandle, NULL);
  }
}

/* Rollback of both shared btree and transaction btree. */
int sqlite3BtreeRollbackAll(Btree *p, int tripCode, int writeOnly){
  if( transBtreeIsUsed(p) ){
    sqlite3TransBtreeRollback(p, tripCode, writeOnly);
  }
  return sqlite3BtreeRollbackOriginal(p, tripCode, writeOnly);
}

/* Open write transaction and get metadata. */
int sqlite3BtreeUpdateMetaWithTransOpen(Btree *p, int idx, u32 iMeta){
  int rc = SQLITE_OK;

  /* Begin write transaction for shared btree. */
  if( !sqlite3BtreeIsInTransOriginal(p) ){
    rc = sqlite3BtreeBeginTransOriginal(p, 1, 0);
    if( rc ) return rc;
  }

  return sqlite3BtreeUpdateMetaOriginal(p, idx, iMeta);
}

/*
** Get a table lock.
** For sqlite_master:
**    Only one user can get WRITE_LOCK.
** For normal table:
**    Multiple users can get WRITE_LOCK.
**    Only one user can get EXCLUSIVE_LOCK if no one get WRITE_LOCK and EXCLUSIVE_LOCK.
*/
int sqlite3BtreeLockTableForRowLock(Btree *p, int iTab, u8 isWriteLock){
  u8 lockType = READ_LOCK;

  assert( isWriteLock==0 || isWriteLock==1 || isWriteLock==2 );
  switch( isWriteLock ){
    case 0: /* Require READ_LOCK */
    case 1: /* Require WRITE_LOCK */
      lockType = READ_LOCK + isWriteLock;
      break;
    case 2: /* Require EXCLUSIVE_LOCK */
      lockType = EXCLSV_LOCK;
      break;
  }
  return sqlite3rowlockIpcLockTableAndAddHistory(p, iTab, lockType);
}

static int rowlockPagecount(BtShared *pBt){
  MemPage *pPage1;     /* Page 1 of the database file */
  int nPage;           /* Number of pages in the database */
  int nPageFile = 0;   /* Number of pages in the database file */

  pPage1 = pBt->pPage1;
  nPage = get4byte(28+(u8*)pPage1->aData);

  sqlite3PagerPagecount(pBt->pPager, &nPageFile);
  if( nPage==0 || memcmp(24+(u8*)pPage1->aData, 92+(u8*)pPage1->aData,4)!=0 ){
    nPage = nPageFile;
  }
  if( (pBt->db->flags & SQLITE_ResetDatabase)!=0 ){
    nPage = 0;
  }

  return nPage;
}

/* Reset page cache. */
int rowlockBtreeCacheReset(Btree *p){
  int rc = SQLITE_OK;
  int iDb;
  int schemaCookie;
  u8 needReset = 0;
  
  if( p->db->init.busy ) return SQLITE_OK;
  if( p->db->nVdbeExec>1 ) return SQLITE_OK;
  if( !transBtreeIsUsed(p) ) return SQLITE_OK;

  /* Check schema version. */
  iDb = rowlockBtreeToIndex(p->db, p);
  schemaCookie = p->db->aDb[iDb].pSchema->schema_cookie;
  rc = rowlockPagerCheckSchemaVers(p->pBt->pPager, schemaCookie, &needReset);
  if( rc ) return rc;

  /* Check database file version. */
  if( !needReset ){
    rc = rowlockPagerCheckDbFileVers(p->pBt->pPager, &needReset);
    if( rc ) return rc;
  }

  if( needReset ){
    rowlockPagerCacheReset(p->pBt->pPager);
    if( p->pBt->pPage1 ){
      PgHdr *pPg = p->pBt->pPage1->pDbPage;
      rc = rowlockPagerReloadDbPage(pPg, p->pBt->pPager);
      if( rc ) return rc;
      p->pBt->nPage = rowlockPagecount(p->pBt);
    }
  }

  return SQLITE_OK;
}

/* Open write transaction and set database version. */
int sqlite3BtreeSetVersionWithTransOpen(Btree *pBtree, int iVersion){
  int rc = SQLITE_OK;

  /* Begin write transaction for shared btree. */
  if( !sqlite3BtreeIsInTransOriginal(pBtree) ){
    rc = sqlite3BtreeBeginTransOriginal(pBtree, 2, 0);
    if( rc ) return rc;
  }

  return sqlite3BtreeSetVersionOriginal(pBtree, iVersion);
}

/* Call sqlite3BtreePayloadFetch() for using cursor. */
const void *sqlite3BtreePayloadFetchAll(BtCursor *pCur, u32 *pAmt){
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  if( cursorTransIsUsed(pCurTrans) ){
    return sqlite3BtreePayloadFetchOriginal(pCurTrans->pCurIns, pAmt);
  }else{
    return sqlite3BtreePayloadFetchOriginal(pCur, pAmt);
  }
}

/* Call sqlite3BtreePayload() for using cursor. */
int sqlite3BtreePayloadAll(BtCursor *pCur, u32 offset, u32 amt, void *pBuf){
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  if( cursorTransIsUsed(pCurTrans) ){
    return sqlite3BtreePayloadOriginal(pCurTrans->pCurIns, offset, amt, pBuf);
  }else{
    return sqlite3BtreePayloadOriginal(pCur, offset, amt, pBuf);
  }
}

/* Call sqlite3BtreePayloadSize() for using cursor. */
u32 sqlite3BtreePayloadSizeAll(BtCursor *pCur){
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  if( cursorTransIsUsed(pCurTrans) ){
    return sqlite3BtreePayloadSizeOriginal(pCurTrans->pCurIns);
  }else{
    return sqlite3BtreePayloadSizeOriginal(pCur);
  }
}

/* Call sqlite3BtreeIntegerKey() for using cursor. */
i64 sqlite3BtreeIntegerKeyAll(BtCursor *pCur){
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  if( cursorTransIsUsed(pCurTrans) ){
    return sqlite3BtreeIntegerKeyOriginal(pCurTrans->pCurIns);
  }else{
    return sqlite3BtreeIntegerKeyOriginal(pCur);
  }
}

/* Call sqlite3BtreeCursorRestore() for using cursor. */
int sqlite3BtreeCursorRestoreAll(BtCursor *pCur, int *pDifferentRow){
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  if( cursorTransIsUsed(pCurTrans) ){
    return sqlite3BtreeCursorRestoreOriginal(pCurTrans->pCurIns, pDifferentRow);
  }else{
    return sqlite3BtreeCursorRestoreOriginal(pCur, pDifferentRow);
  }
}

/* Call sqlite3BtreeCursorHasMoved() for using cursor. */
int sqlite3BtreeCursorHasMovedAll(BtCursor *pCur){
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  if( cursorTransIsUsed(pCurTrans) ){
    return sqlite3BtreeCursorHasMovedOriginal(pCurTrans->pCurIns);
  }else{
    return sqlite3BtreeCursorHasMovedOriginal(pCur);
  }
}

#ifndef NDEBUG
/* Call sqlite3BtreeCursorIsValid() for using cursor. */
int sqlite3BtreeCursorIsValidAll(BtCursor *pCur){
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  if( cursorTransIsUsed(pCurTrans) ){
    return sqlite3BtreeCursorIsValidOriginal(pCurTrans->pCurIns);
  }else{
    return sqlite3BtreeCursorIsValidOriginal(pCur);
  }
}
#endif

/* Set flag which indicates that new rowid is issuued automatically. */
void sqlite3BtreeCachedRowidFlagSet(BtCursor *pCur, u8 flag){
  pCur->autoRowid = flag;
}

static u8 cachedRowidFlagGet(BtCursor *pCur){
  return pCur->autoRowid;
}

/*
** Set the cached rowid value of every cursor in the same database file
** as pCur and having the same root page number as pCur.  The value is
** set to iRowid.
**
** Only positive rowid values are considered valid for this cache.
** The cache is initialized to zero, indicating an invalid cache.
** A btree will work fine with zero or negative rowids.  We just cannot
** cache zero or negative rowids, which means tables that use zero or
** negative rowids might run a little slower.  But in practice, zero
** or negative rowids are very uncommon so this should not be a problem.
*/
void sqlite3BtreeCachedRowidSet(BtCursor *pCur, i64 iRowid){
  if( transBtreeIsUsed(pCur->pBtree) ){
    sqlite3rowlockIpcCachedRowidSet(&pCur->pBtree->btTrans.ipcHandle, pCur->pgnoRoot, iRowid);
  }else{
    pCur->cachedRowid = iRowid;
  }
}

/*
** Return the cached rowid for the given cursor.  A negative or zero
** return value indicates that the rowid cache is invalid and should be
** ignored.  If the rowid cache has never before been set, then a
** zero is returned.
*/
i64 sqlite3BtreeCachedRowidGet(BtCursor *pCur){
  if( transBtreeIsUsed(pCur->pBtree) ){
    return sqlite3rowlockIpcCachedRowidGet(&pCur->pBtree->btTrans.ipcHandle, pCur->pgnoRoot);
  }else{
    return pCur->cachedRowid;
  }
}

/*
** Set cached rowid. This function is called when cursor is opened.
** Firstly, we read cached rowid which might be set by the other users.
** If it is set, we use it. If it is not set, we calculate it by moving
** cursor to the last and getting the largest rowid.
** The cursor must belongs to shared btree.
*/
int sqlite3BtreeCachedRowidSetByOpenCursor(BtCursor *pCur){
  int rc = SQLITE_OK;
  BtCursorTrans *pCurTrans = &pCur->btCurTrans;
  int res = 0;

  /* If it is a cursor of index, do nothing. */
  if( pCur->pKeyInfo ){
    return SQLITE_OK;
  }

  /* If it is already set, do nothing. */
  if( sqlite3BtreeCachedRowidGet(pCur)!=0 ){
    return SQLITE_OK;
  }

  rc = sqlite3BtreeLastAll(pCur, &res);
  if( rc ) return rc;

  if( res ){
    /* If it is empty table, new rowid is 0. */
    sqlite3BtreeCachedRowidSet(pCur, 0);
  }else{
    u64 rowid = 0;
    assert( sqlite3BtreeCursorIsValid(pCur) );
    rowid = sqlite3BtreeIntegerKeyAll(pCur);
    sqlite3BtreeCachedRowidSet(pCur, rowid);
  }

  return SQLITE_OK;
}

/*
** Begin transaction for shared btree and transaction btree.
** If the btree is sharable(!=ephemeral btree), it should not begin write transaction.
** This is because it enables to begin a write transaction by multiple users. 
** Data insertion and deletion are operated for transaction btree. So we start only
** transaction btree's transaction.
** But it returns an error if write transaction is required for read only database.
*/
int sqlite3BtreeBeginTransAll(Btree *p, int wrflag, int *pSchemaVersion){
  int rc = SQLITE_OK;
  int wrflagNew = wrflag;

  if( !p->db->init.busy && transBtreeIsUsed(p) ){
    u8 eLock = sqlite3rowlockIpcLockTableQuery(&p->btTrans.ipcHandle, MASTER_ROOT);
    if( eLock==NOT_LOCKED ){
      rc = sqlite3BtreeLockTable(p, MASTER_ROOT, 0);
      if( rc ) return rc;
    }

    rc = rowlockBtreeCacheReset(p);

    if( eLock==NOT_LOCKED ){
      sqlite3rowlockIpcUnlockTable(&p->btTrans.ipcHandle, MASTER_ROOT);
    }

    if( rc ) return rc;
  }

  if( p->sharable ){
    if( (p->pBt->btsFlags & BTS_READ_ONLY)!=0 && wrflag ) return SQLITE_READONLY;
    /* If it is not BEGIN EXCLUSIVE, we start a read transaction of shared btree. */
    if( wrflag<=1 ) wrflagNew = 0;
  }
  rc = sqlite3BtreeBeginTransOriginal(p, wrflagNew, pSchemaVersion);
  if( rc ) return rc;

  if( transBtreeIsUsed(p) ){
    rc = sqlite3TransBtreeBeginTrans(p, wrflag);
    if( rc ) return rc;
  }
  return SQLITE_OK;
}

/* 
** Return 1 if it is in a transaction. We consider transaction 
** btree and shared btree.
*/
int sqlite3BtreeIsInTransAll(Btree *p){
  if( !p || !transBtreeIsUsed(p) ){
    return sqlite3BtreeIsInTransOriginal(p);
  }else{
    BtreeTrans *pBtTrans = &p->btTrans;
    return sqlite3BtreeIsInTransOriginal(pBtTrans->pBtree);
  }
}

/*
** Open write transaction of share btree in order to commit data into share btree.
*/
int sqlite3BtreeBeginTransForCommit(sqlite3 *db){
  int rc = SQLITE_OK;
  int i;

  /* 
  ** Begin write transaction for shared btree if only transaction btree is in 
  ** transaction and it is not read only database.
  */
  for(i=0; rc==SQLITE_OK && i<db->nDb; i++){ 
    Btree *pBt = db->aDb[i].pBt;
    if( pBt && transBtreeIsUsed(pBt) && 
        sqlite3BtreeIsInTransOriginal(pBt->btTrans.pBtree) && 
        !sqlite3BtreeIsInTransOriginal(pBt) ){
      int readOnly = (pBt->pBt->btsFlags & BTS_READ_ONLY)==1;
      rc = sqlite3BtreeBeginTransOriginal(pBt, !readOnly, 0);
    }
  }
  return rc;
}

/* Unlock tables locks acquired in the statement. */
void sqlite3BtreeUnlockStmtTableLock(sqlite3 *db){
  int i;

  for(i=0; i<db->nDb; i++){ 
    Btree *pBt = db->aDb[i].pBt;
    if( pBt && transBtreeIsUsed(pBt) ){
      sqlite3rowlockIpcUnlockTablesStmtProc(&pBt->btTrans.ipcHandle, NULL);
    }
  }
}

/* Call sqlite3BtreeBeginStmt() for transaction btree if used. */
int sqlite3BtreeBeginStmtAll(Btree *p, int iStatement){
  if( !transBtreeIsUsed(p) ){
    return sqlite3BtreeBeginStmtOriginal(p, iStatement);
  }else{
    BtreeTrans *pBtTrans = &p->btTrans;
    int rc = sqlite3BtreeBeginStmtOriginal(pBtTrans->pBtree, iStatement);
    if( rc ) return rc;
    return sqlite3TransBtreeSavepointCreate(p, p->db->nSavepoint);
  }
}

/* Call hasSharedCacheTableLock() for transaction btree if used. */
#ifdef SQLITE_DEBUG
int hasSharedCacheTableLockAll(Btree *pBtree, Pgno iRoot, int isIndex, int eLockType){
  if(  !transBtreeIsUsed(pBtree) ){
    return hasSharedCacheTableLockOriginal(pBtree, iRoot, isIndex, eLockType);
  }else{
    BtreeTrans *pBtTrans = &pBtree->btTrans;
    return hasSharedCacheTableLockOriginal(pBtTrans->pBtree, iRoot, isIndex, eLockType);
  }
}
#endif

/* Open write transaction and create table. */
int sqlite3BtreeCreateTableWithTransOpen(Btree *p, int *piTable, int flags){
  int rc = SQLITE_OK;

  if( !sqlite3BtreeIsInTransOriginal(p) ){
    rc = sqlite3BtreeBeginTransOriginal(p, 1, 0);
    if( rc ) return rc;
  }

  return sqlite3BtreeCreateTableOriginal(p, piTable, flags);
}

/* 
** Insert records into shared btree from transaction btree. 
** These records are inserted in a transaction.
*/
static int transBtreeCommitTableInsert(BtCursor *pCur){
  int rc = SQLITE_OK;
  BtCursor *pCurIns = pCur->btCurTrans.pCurIns;
  int res = 0;
  char *buff = NULL;

  rc = sqlite3BtreeFirst(pCurIns, &res);
  if( rc ) return rc;
  if( res==0 ) {
    rc = SQLITE_OK;
  }else{
    rc = SQLITE_DONE;
  }
  while( rc==SQLITE_OK ){
    char *tmp = NULL;
    u64 nKey = 0;
    void *pKey = NULL;
    int nData = 0;
    void *pData = NULL;

    u32 amt = sqlite3BtreePayloadSizeOriginal(pCurIns);
    BtreePayload x = {0};   /* Payload to be inserted or updated */
    int flags;

    tmp = (char*)sqlite3Realloc(buff, amt);
    if( !tmp ){
      rc = SQLITE_NOMEM_BKPT;
      goto commit_table_insert_failed;
    }
    buff = tmp;

    rc = sqlite3BtreePayloadOriginal(pCurIns, 0, amt, buff);
    if( rc ) goto commit_table_insert_failed;

    if( pCur->pKeyInfo ){
      /* For an index btree. */
      nKey = amt;
      pKey = buff;
      nData = 0;
      pData = NULL;
    }else{
      /* For a table btree. */
      nKey = sqlite3BtreeIntegerKey(pCurIns);
      pKey = NULL;
      nData = amt;
      pData = buff;
    }

    /* 
    ** Check if same key exists in shared btree.
    ** If found, it is UPDATE, else it is INSERT.
    */
    rc = btreeMovetoOriginal(pCur, pKey, nKey, 0, &res);
    if( rc ) goto commit_table_insert_failed;
    if( res==0 ){
      flags = OPFLAG_SAVEPOSITION;
      getCellInfoOriginal(pCur);
    }else{
      flags = OPFLAG_APPEND;
    }

    /* Operate INSERT or UPDATE. */
    x.pKey = pKey;
    x.nKey = nKey;
    x.pData = pData;
    x.nData = nData;
    rc = sqlite3BtreeInsert(pCur, &x, flags, 0);
    if( rc ) goto commit_table_insert_failed;

    rc = sqlite3BtreeNext(pCurIns, 0);
    if( rc!=SQLITE_OK && rc!=SQLITE_DONE ) return rc;
  }
  assert( rc==SQLITE_DONE );

  sqlite3_free(buff);
  return SQLITE_OK;

commit_table_insert_failed:
  sqlite3_free(buff);
  return rc;
}

/* 
** Delete records from shared btree. These records are deleted in a transaction 
** and information was stored in transaction btree.
*/
static int transBtreeCommitTableDelete(BtCursor *pCur, TransRootPage *pRootPage){
  int rc = SQLITE_OK;
  BtCursor *pCurDel = pCur->btCurTrans.pCurDel;
  int res = 0;
  char *buff = NULL;

  if( pRootPage->deleteAll ){
    return sqlite3BtreeClearTableOfCursor(pCur);
  }

  if( pRootPage->iDel==0 ){
    /* Table does not exist in transaction. So do nothing. */
    return SQLITE_OK;
  }

  rc = sqlite3BtreeFirst(pCurDel, &res);
  if( rc ) return rc;
  if( res==0 ) {
    rc = SQLITE_OK;
  }else{
    rc = SQLITE_DONE;
  }

  while( rc == SQLITE_OK ){
    char *tmp = NULL;
    u64 nKey = 0;
    void *pKey = NULL;
    int nData = 0;
    void *pData = NULL;

    if( pCur->pKeyInfo ){
      /* For an index btree. */
      u32 amt = sqlite3BtreePayloadSizeOriginal(pCurDel);
      tmp = (char*)sqlite3Realloc(buff, amt);
      if( !tmp ){
        rc = SQLITE_NOMEM_BKPT;
        goto commit_table_delete_failed;
      }
      buff = tmp;
      rc = sqlite3BtreePayloadOriginal(pCurDel, 0, amt, buff);
      if( rc ) goto commit_table_delete_failed;

      nKey = amt;
      pKey = buff;
    }else{
      /* For a table btree. */
      nKey = sqlite3BtreeIntegerKeyOriginal(pCurDel);
      pKey = NULL;
    }

    rc = btreeCursorMovetoKey(pCur, pKey, nKey, &res);
    if( rc ) goto commit_table_delete_failed;
    assert( res==0 );

    rc = sqlite3BtreeDelete(pCur, BTREE_AUXDELETE);
    if( rc ) goto commit_table_delete_failed;

    rc = sqlite3BtreeNext(pCurDel, 0);
    if( rc!=SQLITE_OK && rc!=SQLITE_DONE ) goto commit_table_delete_failed;
  }
  assert( rc==SQLITE_DONE );

  sqlite3_free(buff);
  return SQLITE_OK;

commit_table_delete_failed:
  sqlite3_free(buff);
  return rc;
}

/*
** sqlite3BtreeIncrVacuum() for rowlock. Before calling sqlite3BtreeIncrVacuum(),
** this function gets a EXCLSV_LOCK of sqlite_master and begins a write transaction.
*/
int sqlite3BtreeIncrVacuumForRowLock(Btree *p){
  int rc = SQLITE_OK;
  BtreeTrans *pBtTrans = &p->btTrans;

  assert( p!=NULL );
  assert( transBtreeIsUsed(p) );

  rc = sqlite3rowlockIpcLockTableAndAddHistory(p, MASTER_ROOT, WRITE_LOCK);
  if( rc ) return rc;

  rc = sqlite3BtreeBeginTransOriginal(p, 1, 0);
  if( rc ) return rc;

  return sqlite3BtreeIncrVacuumOriginal(p);
}


/* Get EXCLSV_LOCK of all tables for VACUUM. */
int sqlite3rowlockExclusiveLockAllTables(Btree *p){
  int rc = SQLITE_OK;
  BtreeTrans *pBtTrans = &p->btTrans;
  u8 prevLock;
  Schema *pSchema;
  unsigned int nTables = 0;
  unsigned int counter = 0;
  HashElem *i;

  assert( p!=NULL );
  assert( transBtreeIsUsed(p) );
  
  /* Get EXCLSV_LOCK of sqlite_master. */
  rc = sqlite3rowlockIpcLockTable(&pBtTrans->ipcHandle, MASTER_ROOT, WRITE_LOCK, MODE_LOCK_COMMIT, &prevLock);
  if( rc ) return rc;
  assert( prevLock==NOT_LOCKED );
  
  /* Get EXCLSV_LOCK of normal tables. */
  pSchema = (Schema*)p->pBt->pSchema;
  for(i=sqliteHashFirst(&pSchema->tblHash); i; i=sqliteHashNext(i)){
    Table *pTable = (Table*)sqliteHashData(i);
    if( pTable->tnum==MASTER_ROOT ) continue;
    rc = sqlite3rowlockIpcLockTable(&pBtTrans->ipcHandle, pTable->tnum, EXCLSV_LOCK, MODE_LOCK_COMMIT, &prevLock);
    if( rc ) goto exclusive_lock_all_tables_failed;
    assert( prevLock==NOT_LOCKED );
    nTables++;
  }

  return SQLITE_OK;

exclusive_lock_all_tables_failed:
  for(i=sqliteHashFirst(&pSchema->tblHash); i && counter<nTables; i=sqliteHashNext(i)){
    Table *pTable = (Table*)sqliteHashData(i);
    if( pTable->tnum==MASTER_ROOT ) continue;
    sqlite3rowlockIpcUnlockTable(&pBtTrans->ipcHandle, pTable->tnum);
    counter++;
  }
  sqlite3rowlockIpcUnlockTable(&pBtTrans->ipcHandle, MASTER_ROOT);
  return rc;
}

/*
** Check whether the table is mofdified by confirmig the existence of record
** of insertion table and deletion table.
*/
static int isTableModified(BtCursorTrans *pCurTrans, int *isModified){
  int rc;
  int res;

  if( pCurTrans->deleteAll ){
    *isModified = 1;
    return SQLITE_OK;
  }

  rc = sqlite3BtreeFirst(pCurTrans->pCurIns, &res);
  if( rc ) return rc;
  if( res == 0 ){
    *isModified = 1;
    return SQLITE_OK;
  }

  rc = sqlite3BtreeFirst(pCurTrans->pCurDel, &res);
  if( rc ) return rc;
  if( res == 0 ){
    *isModified = 1;
    return SQLITE_OK;
  }

  *isModified = 0;
  return SQLITE_OK;
}

/* Get EXCLSV_LOCK of modified tables for COMMIT. */
static int exclusiveLockTables(Btree *p){
  int rc = SQLITE_OK;
  BtreeTrans *pBtTrans = &p->btTrans;
  HashI64 *pTransRootPages = &pBtTrans->rootPages;
  BtCursor *pCur = NULL;
  HashElemI64 *elem = NULL;
  int *isModified = NULL;
  u8 *prevLock = NULL;
  u8 prevLockMaster = 0;
  int i = 0;
  int iTableRollback = 0;
  u64 counter = 0;

  pCur = (BtCursor*)sqlite3MallocZero(sizeof(BtCursor));
  if( !pCur ) return SQLITE_NOMEM_BKPT;
  
  /* Count the number of elements. */
  elem = sqliteHashI64First(pTransRootPages);
  while( elem ){
    counter++;
    elem = sqliteHashI64Next(elem);
  }
  if( counter>0 ){
    rc = sqlite3rowlockIpcLockTable(&pBtTrans->ipcHandle, MASTER_ROOT, WRITE_LOCK, MODE_LOCK_COMMIT, &prevLockMaster);
    if( rc ){
      sqlite3_free(pCur);
      return rc;
    }
    isModified = (int*)sqlite3MallocZero(sizeof(int) * counter);
    prevLock = (u8*)sqlite3MallocZero(sizeof(u8) * counter);
    if( !isModified || !prevLock ){
      rc = SQLITE_NOMEM_BKPT;
      goto exclusive_lock_tables_failed;
    }
  }

  elem = sqliteHashI64First(pTransRootPages);
  while( elem ){
    i64 iTable = sqliteHashI64Key(elem);
    TransRootPage *pRootPage = (TransRootPage*)sqliteHashI64Data(elem);
    struct KeyInfo *pKeyInfo = pRootPage->pKeyInfo;
    assert( iTable==(i64)(int)iTable );

    rc = sqlite3BtreeCursorAll(p, (int)iTable, 0, pKeyInfo, pCur, 1);
    if( rc ) goto exclusive_lock_tables_failed;

    /* Check if the table is modified. */
    rc = isTableModified(&pCur->btCurTrans, &isModified[i]);
    if( rc ){
      sqlite3BtreeCloseCursorAll(pCur);
      goto exclusive_lock_tables_failed;
    }

    /* Get EXCLSV_LOCK if table is modified. */
    if( isModified[i] ){
      rc = sqlite3rowlockIpcLockTable(&pBtTrans->ipcHandle, (int)iTable, EXCLSV_LOCK, MODE_LOCK_COMMIT, &prevLock[i]);
    }
    sqlite3BtreeCloseCursorAll(pCur);
    if( rc ) goto exclusive_lock_tables_failed;

    i++;
    elem = sqliteHashI64Next(elem);
  }

  sqlite3_free(isModified);
  sqlite3_free(prevLock);
  sqlite3_free(pCur);
  return SQLITE_OK;

exclusive_lock_tables_failed:
  /* Rollback the lock status. */
  if( isModified && prevLock ){
    elem = sqliteHashI64First(pTransRootPages);
    i = 0;
    while( elem ){
      i64 iTable = sqliteHashI64Key(elem);
      if( isModified[i] ) sqlite3rowlockIpcLockTable(&pBtTrans->ipcHandle, (int)iTable, MODE_LOCK_FORCE, prevLock[i], NULL);
      i++;
      elem = sqliteHashI64Next(elem);
    }
  }
  if( counter>0 ) sqlite3rowlockIpcLockTable(&pBtTrans->ipcHandle, MASTER_ROOT, MODE_LOCK_FORCE, prevLockMaster, NULL);
  sqlite3_free(isModified);
  sqlite3_free(prevLock);
  sqlite3_free(pCur);
  return rc;
}

/*
** Commit records.
** Modified record information is stored in a transaction btree. Read them and modifiy a shared btree.
*/
int sqlite3TransBtreeCommit(Btree *p){
  int rc = SQLITE_OK;
  BtreeTrans *pBtTrans = &p->btTrans;
  HashI64 *pTransRootPages = &pBtTrans->rootPages;
  BtCursor *pCur = NULL;
  HashElemI64 *elem = NULL;
  u8 eLock;
  u8 isDdl = 0;

  if( !transBtreeIsUsed(p) || (p->pBt->btsFlags & BTS_READ_ONLY)==1 ) return SQLITE_OK;

  eLock = sqlite3rowlockIpcLockTableQuery(&p->btTrans.ipcHandle, MASTER_ROOT);
  if( eLock==WRITE_LOCK ) isDdl = 1;

  /* Get EXCLSV_LOCK for modified tables in order to prevent from the table access by the other process. */
  rc = exclusiveLockTables(p);
  if( rc ) return rc;

  /*
  ** Reset the page cache if database file is changed.
  ** We check the schema version and database file version. But the scheam version on memory is always
  ** different from that of database file if DDL is executing.
  ** But in case of DDL, the modification is done for pager in shared btree. We should not reset the cache.
  ** If we reset the cache, the modification is also deleted. So we reset the cache if it is not DDL.
  ** It is no problem because DDL is operated under exclusion control.
  ** In order to know it is DDL or not, we check if sqlite_master is locked or not.
  */
  if( !isDdl ){
    rc = rowlockBtreeCacheReset(p);
    if( rc ) return rc;
  }

  pCur = (BtCursor*)sqlite3MallocZero(sizeof(BtCursor));
  if( !pCur ){
    return SQLITE_NOMEM_BKPT;
  }

  /* Begin write transaction for shared btree if only transaction btree is in transaction. */
  if( sqlite3BtreeIsInTransOriginal(pBtTrans->pBtree) && 
      !sqlite3BtreeIsInTransOriginal(p) ){
    rc = sqlite3BtreeBeginTransOriginal(p, 1, 0);
    if( rc ){
      sqlite3_free(pCur);
      return rc;
    }
  }

  /* 
  ** Traverse the transaction tree and write all the rows from
  ** this tree to the shared cache.
  */
  elem = sqliteHashI64First(pTransRootPages);
  while( elem ){
    i64 iTable = sqliteHashI64Key(elem);
    TransRootPage *pRootPage = (TransRootPage*)sqliteHashI64Data(elem);
    struct KeyInfo *pKeyInfo = pRootPage->pKeyInfo;
    assert( iTable==(i64)(int)iTable );

    /* Create cursor for the target table. Table lock is required for a write cursor creation. */
    rc = sqlite3BtreeLockTableOriginal(p, (int)iTable, 1);
    if( rc ) goto trans_btree_commit_failed;
    rc = sqlite3BtreeCursorAll(p, (int)iTable, BTREE_WRCSR, pKeyInfo, pCur, 1);
    if( rc ) goto trans_btree_commit_failed;

    /* Delete */
    rc = transBtreeCommitTableDelete(pCur, pRootPage);
    if( rc ) goto trans_btree_commit_failed;

    /* INSERT or UPDATE */
    rc = transBtreeCommitTableInsert(pCur);
    if( rc ) goto trans_btree_commit_failed;
    
    sqlite3BtreeCloseCursorAll(pCur);
    elem = sqliteHashI64Next(elem);
  }

  /* Clear Btree Cursor in normal execution routine */
  sqlite3_free(pCur);

  /* Clear transaction data. */
  sqlite3TransBtreeRollback(p, SQLITE_OK, 0);
 
  return SQLITE_OK;

trans_btree_commit_failed:
  sqlite3BtreeCloseCursorAll(pCur);
  sqlite3_free(pCur);
  return rc;
}

/* 
** Set autocommit in order to execute commit after DDL.
** If I have an exclusive lock, DDL is executing.
*/
void sqlite3SetForceCommit(Vdbe *pVdbe){
  int i;
  sqlite3 *db = pVdbe->db;

  if( db->autoCommit ) return;
  for(i=0; i<db->nDb; i++){
    Btree *p = db->aDb[i].pBt;
    u8 eLock;
    if( !p || !transBtreeIsUsed(p) ) continue;
    eLock = sqlite3rowlockIpcLockTableQuery(&p->btTrans.ipcHandle, MASTER_ROOT);
    if( eLock==WRITE_LOCK ){
      p->db->autoCommit = 1;
      pVdbe->forceCommit = 1;
      return;
    }
  }
}

#endif /* SQLITE_OMIT_ROWLOCK */
