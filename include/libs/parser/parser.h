/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _TD_PARSER_H_
#define _TD_PARSER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "catalog.h"
#include "common.h"
#include "tname.h"
#include "tvariant.h"

typedef struct SColumn {
  uint64_t     tableUid;
  int32_t      columnIndex;
  SColumnInfo  info;
} SColumn;

// the structure for sql function in select clause
typedef struct SSqlExpr {
  char      token[TSDB_COL_NAME_LEN];      // original token
  SSchema   resSchema;
  SColIndex colInfo;
  uint64_t  uid;            // table uid, todo refactor use the pointer
  int32_t   interBytes;     // inter result buffer size
  int16_t   numOfParams;    // argument value of each function
  SVariant  param[3];       // parameters are not more than 3
} SSqlExpr;

typedef struct SExprInfo {
  SSqlExpr           base;
  struct tExprNode  *pExpr;
} SExprInfo;

//typedef struct SInterval {
//  int32_t  tz;            // query client timezone
//  char     intervalUnit;
//  char     slidingUnit;
//  char     offsetUnit;
//  int64_t  interval;
//  int64_t  sliding;
//  int64_t  offset;
//} SInterval;
//
//typedef struct SSessionWindow {
//  int64_t  gap;             // gap between two session window(in microseconds)
//  int32_t  primaryColId;    // primary timestamp column
//} SSessionWindow;

typedef struct SGroupbyExpr {
  int16_t  tableIndex;
  SArray*  columnInfo;  // SArray<SColIndex>, group by columns information
  int16_t  orderIndex;  // order by column index
  int16_t  orderType;   // order by type: asc/desc
} SGroupbyExpr;

typedef struct SField {
  char     name[TSDB_COL_NAME_LEN];
  uint8_t  type;
  int16_t  bytes;
} SField;

typedef struct SFieldInfo {
  int16_t     numOfOutput;   // number of column in result
  SField     *final;
  SArray     *internalField; // SArray<SInternalField>
} SFieldInfo;

typedef struct SLimit {
  int64_t   limit;
  int64_t   offset;
} SLimit;

typedef struct SOrder {
  uint32_t  order;
  int32_t   orderColId;
} SOrder;

typedef struct SCond {
  uint64_t uid;
  int32_t  len;  // length of tag query condition data
  char *   cond;
} SCond;

typedef struct SJoinNode {
  uint64_t uid;
  int16_t  tagColId;
  SArray*  tsJoin;
  SArray*  tagJoin;
} SJoinNode;

typedef struct SJoinInfo {
  bool       hasJoin;
  SJoinNode *joinTables[TSDB_MAX_JOIN_TABLE_NUM];
} SJoinInfo;

typedef struct STagCond {
  int16_t   relType;     // relation between tbname list and query condition, including : TK_AND or TK_OR
  SCond     tbnameCond;  // tbname query condition, only support tbname query condition on one table
  SJoinInfo joinInfo;    // join condition, only support two tables join currently
  SArray   *pCond;       // for different table, the query condition must be seperated
} STagCond;

typedef struct STableMetaInfo {
  STableMeta    *pTableMeta;      // table meta, cached in client side and acquired by name
  SVgroupsInfo  *vgroupList;

  /*
   * 1. keep the vgroup index during the multi-vnode super table projection query
   * 2. keep the vgroup index for multi-vnode insertion
   */
  int32_t       vgroupIndex;
  SName         name;
  char          aliasName[TSDB_TABLE_NAME_LEN];    // alias name of table specified in query sql
  SArray       *tagColList;                        // SArray<SColumn*>, involved tag columns
} STableMetaInfo;

typedef struct SQueryAttrInfo {
  bool             stableQuery;
  bool             groupbyColumn;
  bool             simpleAgg;
  bool             arithmeticOnAgg;
  bool             projectionQuery;
  bool             hasFilter;
  bool             onlyTagQuery;
  bool             orderProjectQuery;
  bool             stateWindow;
  bool             globalMerge;
  bool             multigroupResult;
} SQueryAttrInfo;

typedef struct SQueryStmtInfo {
  int16_t          command;       // the command may be different for each subclause, so keep it seperately.
  uint32_t         type;          // query/insert type
  STimeWindow      window;        // the whole query time window
  SInterval        interval;      // tumble time window
  SSessionWindow   sessionWindow; // session time window
  SGroupbyExpr     groupbyExpr;   // groupby tags info
  SArray *         colList;       // SArray<SColumn*>
  SFieldInfo       fieldsInfo;
  SArray *         exprList;      // SArray<SExprInfo*>
  SArray *         exprList1;     // final exprlist in case of arithmetic expression exists
  SLimit           limit;
  SLimit           slimit;
  STagCond         tagCond;
  SArray *         colCond;
  SOrder           order;
  int16_t          numOfTables;
  int16_t          curTableIdx;
  STableMetaInfo **pTableMetaInfo;
  struct STSBuf   *tsBuf;

  int16_t          fillType;      // final result fill type
  int64_t *        fillVal;       // default value for fill
  int32_t          numOfFillVal;  // fill value size

  char *           msg;           // pointer to the pCmd->payload to keep error message temporarily
  int64_t          clauseLimit;   // limit for current sub clause

  int64_t          prjOffset;     // offset value in the original sql expression, only applied at client side
  int64_t          vgroupLimit;    // table limit in case of super table projection query + global order + limit

  int32_t          udColumnId;    // current user-defined constant output field column id, monotonically decreases from TSDB_UD_COLUMN_INDEX
  bool             distinct;   // distinct tag or not
  bool             onlyHasTagCond;
  int32_t          bufLen;
  char*            buf;
  SArray          *pUdfInfo;

  struct SQueryStmtInfo *sibling;     // sibling
  SArray            *pUpstream;   // SArray<struct SQueryStmtInfo>
  struct SQueryStmtInfo *pDownstream;
  int32_t            havingFieldNum;
  SQueryAttrInfo     info;
} SQueryStmtInfo;

typedef struct SColumnIndex {
  int16_t tableIndex;
  int16_t columnIndex;
  int16_t type;               // normal column/tag/ user input constant column
} SColumnIndex;

struct SInsertStmtInfo;

/**
 * True will be returned if the input sql string is insert, false otherwise.
 * @param pStr    sql string
 * @param length  length of the sql string
 * @return
 */
bool qIsInsertSql(const char* pStr, size_t length);

/**
 * Parse the sql statement and then return the SQueryStmtInfo as the result of bounded AST.
 * @param pSql     sql string
 * @param length   length of the sql string
 * @param id       operator id, generated by uuid generator
 * @param msg      extended error message if exists.
 * @return         error code
 */
int32_t qParseQuerySql(const char* pStr, size_t length, struct SQueryStmtInfo** pQueryInfo, int64_t id, char* msg, int32_t msgLen);

/**
 * Parse the insert sql statement.
 * @param pStr            sql string
 * @param length          length of the sql string
 * @param pInsertParam    data in binary format to submit to vnode directly.
 * @param id              operator id, generated by uuid generator.
 * @param msg             extended error message if exists to help avoid the problem in sql statement.
 * @return
 */
int32_t qParseInsertSql(const char* pStr, size_t length, struct SInsertStmtInfo** pInsertInfo, int64_t id, char* msg, int32_t msgLen);

/**
 * Convert a normal sql statement to only query tags information to enable that the subscribe client can be aware quickly of the true vgroup ids that
 * involved in the subscribe procedure.
 * @param pSql
 * @param length
 * @param pConvertSql
 * @return
 */
int32_t qParserConvertSql(const char* pStr, size_t length, char** pConvertSql);

void assignExprInfo(SExprInfo* dst, const SExprInfo* src);
void columnListCopy(SArray* dst, const SArray* src, uint64_t uid);
void columnListDestroy(SArray* pColumnList);

void dropAllExprInfo(SArray* pExprInfo);
SExprInfo* createExprInfo(STableMetaInfo* pTableMetaInfo, int16_t functionId, SColumnIndex* pColIndex, struct tExprNode* pParamExpr, SSchema* pResSchema, int16_t interSize);
int32_t copyExprInfoList(SArray* dst, const SArray* src, uint64_t uid, bool deepcopy);

STableMetaInfo* getMetaInfo(SQueryStmtInfo* pQueryInfo, int32_t tableIndex);
int32_t getNewResColId();

#ifdef __cplusplus
}
#endif

#endif /*_TD_PARSER_H_*/