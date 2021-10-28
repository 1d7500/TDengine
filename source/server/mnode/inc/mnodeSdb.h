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

#ifndef _TD_MNODE_SDB_H_
#define _TD_MNODE_SDB_H_

#include "mnodeInt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*SdbDeployFp)();
typedef void *(*SdbDecodeFp)(cJSON *root);
typedef int32_t (*SdbEncodeFp)(void *pHead, char *buf, int32_t maxLen);

int32_t sdbInit();
void    sdbCleanup();

int32_t sdbRead();
int32_t sdbCommit();

int32_t sdbDeploy();
void    sdbUnDeploy();

void   *sdbInsertRow(EMnSdb sdb, void *pObj);
void    sdbDeleteRow(EMnSdb sdb, void *pHead);
void   *sdbUpdateRow(EMnSdb sdb, void *pHead);
void   *sdbGetRow(EMnSdb sdb, void *pKey);
void   *sdbFetchRow(EMnSdb sdb, void *pIter);
void    sdbCancelFetch(EMnSdb sdb, void *pIter);
int32_t sdbGetCount(EMnSdb sdb);

void sdbSetFp(EMnSdb, EMnKey, SdbDeployFp, SdbEncodeFp, SdbDecodeFp, int32_t dataSize);

#ifdef __cplusplus
}
#endif

#endif /*_TD_MNODE_INT_H_*/
