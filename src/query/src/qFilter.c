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
#include "os.h"
#include "queryLog.h"
#include "qFilter.h"
#include "tcompare.h"
#include "hash.h"
#include "tscUtil.h"

OptrStr gOptrStr[] = {
  {TSDB_RELATION_INVALID,                  "invalid"},
  {TSDB_RELATION_LESS,                     "<"},
  {TSDB_RELATION_GREATER,                  ">"},
  {TSDB_RELATION_EQUAL,                    "="},
  {TSDB_RELATION_LESS_EQUAL,               "<="},
  {TSDB_RELATION_GREATER_EQUAL,            ">="},
  {TSDB_RELATION_NOT_EQUAL,                "!="},
  {TSDB_RELATION_LIKE,                     "like"},
  {TSDB_RELATION_ISNULL,                   "is null"},
  {TSDB_RELATION_NOTNULL,                  "not null"},
  {TSDB_RELATION_IN,                       "in"},
  {TSDB_RELATION_AND,                      "and"},
  {TSDB_RELATION_OR,                       "or"},
  {TSDB_RELATION_NOT,                      "not"}
};

static FORCE_INLINE int32_t filterFieldColDescCompare(const void *desc1, const void *desc2) {
  const SSchema *sch1 = desc1;
  const SSchema *sch2 = desc2;

  return sch1->colId != sch2->colId;
}

static FORCE_INLINE int32_t filterFieldValDescCompare(const void *desc1, const void *desc2) {
  const tVariant *val1 = desc1;
  const tVariant *val2 = desc2;

  return tVariantCompare(val1, val2);
}


filter_desc_compare_func gDescCompare [FLD_TYPE_MAX] = {
  NULL,
  filterFieldColDescCompare,
  filterFieldValDescCompare
};


static FORCE_INLINE int32_t filterCompareGroupCtx(const void *pLeft, const void *pRight) {
  SFilterGroupCtx *left = *((SFilterGroupCtx**)pLeft), *right = *((SFilterGroupCtx**)pRight);
  if (left->colNum > right->colNum) return 1;
  if (left->colNum < right->colNum) return -1;
  return 0;
}


int32_t filterInitUnitsFields(SFilterInfo *info) {
  info->unitSize = FILTER_DEFAULT_UNIT_SIZE;
  info->units = calloc(info->unitSize, sizeof(SFilterUnit));
  
  info->fields[FLD_TYPE_COLUMN].num = 0;
  info->fields[FLD_TYPE_COLUMN].size = FILTER_DEFAULT_FIELD_SIZE;
  info->fields[FLD_TYPE_COLUMN].fields = calloc(info->fields[FLD_TYPE_COLUMN].size, COL_FIELD_SIZE);
  info->fields[FLD_TYPE_VALUE].num = 0;
  info->fields[FLD_TYPE_VALUE].size = FILTER_DEFAULT_FIELD_SIZE;
  info->fields[FLD_TYPE_VALUE].fields = calloc(info->fields[FLD_TYPE_VALUE].size, sizeof(SFilterField));

  return TSDB_CODE_SUCCESS;
}

static FORCE_INLINE SFilterRangeNode* filterNewRange(SFilterRangeCtx *ctx, SFilterRange* ra) {
  SFilterRangeNode *r = NULL;
  
  if (ctx->rf) {
    r = ctx->rf;
    ctx->rf = ctx->rf->next;
    r->prev = NULL;
    r->next = NULL;
  } else {
    r = calloc(1, sizeof(SFilterRangeNode)); 
  }

  FILTER_COPY_RA(&r->ra, ra);

  return r;
}

void* filterInitRangeCtx(int32_t type, int32_t options) {
  if (type > TSDB_DATA_TYPE_UBIGINT || type < TSDB_DATA_TYPE_BOOL || type == TSDB_DATA_TYPE_BINARY || type == TSDB_DATA_TYPE_NCHAR) {
    qError("not supported range type:%d", type);
    return NULL;
  }
  
  SFilterRangeCtx *ctx = calloc(1, sizeof(SFilterRangeCtx));

  ctx->type = type;
  ctx->options = options;
  ctx->pCompareFunc = getComparFunc(type, 0);

  return ctx;
}


int32_t filterResetRangeCtx(SFilterRangeCtx *ctx) {
  ctx->status = 0;

  if (ctx->rf == NULL) {
    ctx->rf = ctx->rs;
    ctx->rs = NULL;
    return TSDB_CODE_SUCCESS;
  }

  ctx->isnull = false;
  ctx->notnull = false;
  ctx->isrange = false;

  SFilterRangeNode *r = ctx->rf;
  
  while (r && r->next) {
    r = r->next;
  }

  r->next = ctx->rs;
  ctx->rs = NULL;
  return TSDB_CODE_SUCCESS;
}

int32_t filterReuseRangeCtx(SFilterRangeCtx *ctx, int32_t type, int32_t options) {
  filterResetRangeCtx(ctx);

  ctx->type = type;
  ctx->options = options;
  ctx->pCompareFunc = getComparFunc(type, 0);

  return TSDB_CODE_SUCCESS;
}


int32_t filterPostProcessRange(SFilterRangeCtx *cur, SFilterRange *ra, bool *notNull) {
  if (!FILTER_GET_FLAG(ra->sflag, RA_NULL)) {
    int32_t sr = cur->pCompareFunc(&ra->s, getDataMin(cur->type));
    if (sr == 0) {
      FILTER_SET_FLAG(ra->sflag, RA_NULL);
    }
  }

  if (!FILTER_GET_FLAG(ra->eflag, RA_NULL)) {
    int32_t er = cur->pCompareFunc(&ra->e, getDataMax(cur->type));
    if (er == 0) {
      FILTER_SET_FLAG(ra->eflag, RA_NULL);
    }
  }

  
  if (FILTER_GET_FLAG(ra->sflag, RA_NULL) && FILTER_GET_FLAG(ra->eflag, RA_NULL)) {
    *notNull = true;
  } else {
    *notNull = false;
  }

  return TSDB_CODE_SUCCESS;
}

int32_t filterAddRangeOptr(void* h, uint8_t raOptr, int32_t optr, bool *empty, bool *all) {
  SFilterRangeCtx *ctx = (SFilterRangeCtx *)h;

  if (optr == TSDB_RELATION_AND) {
    SET_AND_OPTR(ctx, raOptr);
    if (CHK_AND_OPTR(ctx)) {
      FILTER_SET_FLAG(ctx->status, MR_ST_EMPTY);
      *empty = true;
    }
  } else {
    SET_OR_OPTR(ctx, raOptr);
    if (CHK_OR_OPTR(ctx)) {
      FILTER_SET_FLAG(ctx->status, MR_ST_ALL);
      *all = true;
    }
  }

  return TSDB_CODE_SUCCESS;
}



int32_t filterAddRangeImpl(void* h, SFilterRange* ra, int32_t optr) {
  SFilterRangeCtx *ctx = (SFilterRangeCtx *)h;

  if (ctx->rs == NULL) {
    if ((FILTER_GET_FLAG(ctx->status, MR_ST_START) == 0) 
      || (FILTER_GET_FLAG(ctx->status, MR_ST_ALL) && (optr == TSDB_RELATION_AND))
      || ((!FILTER_GET_FLAG(ctx->status, MR_ST_ALL)) && (optr == TSDB_RELATION_OR))) {
      APPEND_RANGE(ctx, ctx->rs, ra);
      FILTER_SET_FLAG(ctx->status, MR_ST_START);
    }

    return TSDB_CODE_SUCCESS;
  }

  SFilterRangeNode *r = ctx->rs;
  SFilterRangeNode *rn = NULL;
  int32_t cr = 0;

  if (optr == TSDB_RELATION_AND) {
    while (r != NULL) {
      cr = ctx->pCompareFunc(&r->ra.s, &ra->e);
      if (FILTER_GREATER(cr, r->ra.sflag, ra->eflag)) {
        FREE_FROM_RANGE(ctx, r);
        break;
      }

      cr = ctx->pCompareFunc(&ra->s, &r->ra.e);
      if (FILTER_GREATER(cr, ra->sflag, r->ra.eflag)) {
        rn = r->next;
        FREE_RANGE(ctx, r);
        r = rn;
        continue;
      }

      cr = ctx->pCompareFunc(&ra->s, &r->ra.s);
      if (FILTER_GREATER(cr, ra->sflag, r->ra.sflag)) {
        SIMPLE_COPY_VALUES((char *)&r->ra.s, &ra->s);
        cr == 0 ? (r->ra.sflag |= ra->sflag) : (r->ra.sflag = ra->sflag);
      }

      cr = ctx->pCompareFunc(&r->ra.e, &ra->e);
      if (FILTER_GREATER(cr, r->ra.eflag, ra->eflag)) {
        SIMPLE_COPY_VALUES((char *)&r->ra.e, &ra->e);
        cr == 0 ? (r->ra.eflag |= ra->eflag) : (r->ra.eflag = ra->eflag);
        break;
      }

      r = r->next;
    }

    return TSDB_CODE_SUCCESS;
  }


  //TSDB_RELATION_OR
  
  bool smerged = false;
  bool emerged = false;

  while (r != NULL) {
    cr = ctx->pCompareFunc(&r->ra.s, &ra->e);
    if (FILTER_GREATER(cr, r->ra.sflag, ra->eflag)) {    
      if (emerged == false) {
        INSERT_RANGE(ctx, r, ra);
      }
      
      break;
    }

    if (smerged == false) {
      cr = ctx->pCompareFunc(&ra->s, &r->ra.e);
      if (FILTER_GREATER(cr, ra->sflag, r->ra.eflag)) {   
        if (r->next) {
          r= r->next;
          continue;
        }

        APPEND_RANGE(ctx, r, ra);
        break;
      }

      cr = ctx->pCompareFunc(&r->ra.s, &ra->s);
      if (FILTER_GREATER(cr, r->ra.sflag, ra->sflag)) {         
        SIMPLE_COPY_VALUES((char *)&r->ra.s, &ra->s);        
        cr == 0 ? (r->ra.sflag &= ra->sflag) : (r->ra.sflag = ra->sflag);
      }

      smerged = true;
    }
    
    if (emerged == false) {
      cr = ctx->pCompareFunc(&ra->e, &r->ra.e);
      if (FILTER_GREATER(cr, ra->eflag, r->ra.eflag)) {
        SIMPLE_COPY_VALUES((char *)&r->ra.e, &ra->e);
        if (cr == 0) { 
          r->ra.eflag &= ra->eflag;
          break;
        }
        
        r->ra.eflag = ra->eflag;
        emerged = true;
        r = r->next;
        continue;
      }

      break;
    }

    cr = ctx->pCompareFunc(&ra->e, &r->ra.e);
    if (FILTER_GREATER(cr, ra->eflag, r->ra.eflag)) {
      rn = r->next;
      FREE_RANGE(ctx, r);
      r = rn;

      continue;
    } else {
      SIMPLE_COPY_VALUES(&r->prev->ra.e, (char *)&r->ra.e);
      cr == 0 ? (r->prev->ra.eflag &= r->ra.eflag) : (r->prev->ra.eflag = r->ra.eflag);
      FREE_RANGE(ctx, r);
      
      break;
    }
  }

  if (ctx->rs && ctx->rs->next == NULL) {
    bool notnull;
    filterPostProcessRange(ctx, &ctx->rs->ra, &notnull);
    if (notnull) {
      bool all = false;
      FREE_FROM_RANGE(ctx, ctx->rs);
      filterAddRangeOptr(h, TSDB_RELATION_NOTNULL, optr, NULL, &all);
      if (all) {
        FILTER_SET_FLAG(ctx->status, MR_ST_ALL);
      }
    }
  }

  return TSDB_CODE_SUCCESS;  
}

int32_t filterAddRange(void* h, SFilterRange* ra, int32_t optr) {
  SFilterRangeCtx *ctx = (SFilterRangeCtx *)h;
  
  if (FILTER_GET_FLAG(ra->sflag, RA_NULL)) {
    SIMPLE_COPY_VALUES(&ra->s, getDataMin(ctx->type));
    //FILTER_CLR_FLAG(ra->sflag, RA_NULL);
  }

  if (FILTER_GET_FLAG(ra->eflag, RA_NULL)) {
    SIMPLE_COPY_VALUES(&ra->e, getDataMax(ctx->type));
    //FILTER_CLR_FLAG(ra->eflag, RA_NULL);
  }

  return filterAddRangeImpl(h, ra, optr);
}


int32_t filterAddRangeCtx(void *dst, void *src, int32_t optr) {
  SFilterRangeCtx *dctx = (SFilterRangeCtx *)dst;
  SFilterRangeCtx *sctx = (SFilterRangeCtx *)src;

  assert(optr == TSDB_RELATION_OR);

  if (sctx->rs == NULL) {
    return TSDB_CODE_SUCCESS;
  }

  SFilterRangeNode *r = sctx->rs;
  
  while (r) {
    filterAddRange(dctx, &r->ra, optr);
    r = r->next;
  }

  return TSDB_CODE_SUCCESS;
}

int32_t filterCopyRangeCtx(void *dst, void *src) {
  SFilterRangeCtx *dctx = (SFilterRangeCtx *)dst;
  SFilterRangeCtx *sctx = (SFilterRangeCtx *)src;

  dctx->status = sctx->status;
  
  dctx->isnull = sctx->isnull;
  dctx->notnull = sctx->notnull;
  dctx->isrange = sctx->isrange;

  SFilterRangeNode *r = sctx->rs;
  SFilterRangeNode *dr = dctx->rs;
  
  while (r) {
    APPEND_RANGE(dctx, dr, &r->ra);
    if (dr == NULL) {
      dr = dctx->rs;
    } else {
      dr = dr->next;
    }
    r = r->next;
  }

  return TSDB_CODE_SUCCESS;
}



int32_t filterFinishRange(void* h) {
  SFilterRangeCtx *ctx = (SFilterRangeCtx *)h;

  if (FILTER_GET_FLAG(ctx->status, MR_ST_FIN)) {
    return TSDB_CODE_SUCCESS;
  }

  if (FILTER_GET_FLAG(ctx->options, FI_OPTION_TIMESTAMP)) {
    SFilterRangeNode *r = ctx->rs;
    SFilterRangeNode *rn = NULL;
    
    while (r && r->next) {
      int64_t tmp = 1;
      operateVal(&tmp, &r->ra.e, &tmp, TSDB_BINARY_OP_ADD, ctx->type);
      if (ctx->pCompareFunc(&tmp, &r->next->ra.s) == 0) {
        rn = r->next;
        SIMPLE_COPY_VALUES((char *)&r->next->ra.s, (char *)&r->ra.s);
        FREE_RANGE(ctx, r);
        r = rn;
      
        continue;
      }
      
      r = r->next;
    }
  }

  FILTER_SET_FLAG(ctx->status, MR_ST_FIN);

  return TSDB_CODE_SUCCESS;
}

int32_t filterGetRangeNum(void* h, int32_t* num) {
  filterFinishRange(h);
  
  SFilterRangeCtx *ctx = (SFilterRangeCtx *)h;

  *num = 0;

  SFilterRangeNode *r = ctx->rs;
  
  while (r) {
    ++(*num);
    r = r->next;
  }

  return TSDB_CODE_SUCCESS;
}


int32_t filterGetRangeRes(void* h, SFilterRange *ra) {
  filterFinishRange(h);

  SFilterRangeCtx *ctx = (SFilterRangeCtx *)h;
  uint32_t num = 0;
  SFilterRangeNode* r = ctx->rs;
  
  while (r) {
    FILTER_COPY_RA(ra, &r->ra);

    ++num;
    r = r->next;
    ++ra;
  }

  if (num == 0) {
    qError("no range result");
    return TSDB_CODE_QRY_APP_ERROR;
  }
  
  return TSDB_CODE_SUCCESS;
}


int32_t filterSourceRangeFromCtx(SFilterRangeCtx *ctx, void *sctx, int32_t optr, bool *empty, bool *all) {
  SFilterRangeCtx *src = (SFilterRangeCtx *)sctx;

  if (src->isnull){
    filterAddRangeOptr(ctx, TSDB_RELATION_ISNULL, optr, empty, all);
    if (FILTER_GET_FLAG(ctx->status, MR_ST_ALL)) {
      *all = true;
    }
  }

  if (src->notnull) {
    filterAddRangeOptr(ctx, TSDB_RELATION_NOTNULL, optr, empty, all);
    if (FILTER_GET_FLAG(ctx->status, MR_ST_ALL)) {
      *all = true;
    }
  }

  if (src->isrange) {
    filterAddRangeOptr(ctx, 0, optr, empty, all);

    if (!(optr == TSDB_RELATION_OR && ctx->notnull)) {
      filterAddRangeCtx(ctx, src, optr);
    }
    
    if (FILTER_GET_FLAG(ctx->status, MR_ST_ALL)) {
      *all = true;
    }
  }

  return TSDB_CODE_SUCCESS;
}



int32_t filterFreeRangeCtx(void* h) {
  if (h == NULL) {
    return TSDB_CODE_SUCCESS;
  }
  
  SFilterRangeCtx *ctx = (SFilterRangeCtx *)h;
  SFilterRangeNode *r = ctx->rs;
  SFilterRangeNode *rn = NULL;
  
  while (r) {
    rn = r->next;
    free(r);
    r = rn;
  }

  free(ctx);

  return TSDB_CODE_SUCCESS;
}

int32_t filterInitVarCtx(int32_t type, int32_t options) {
  SFilterVarCtx *ctx = calloc(1, sizeof(SFilterVarCtx));

  ctx->type = type;
  ctx->options = options;

  return TSDB_CODE_SUCCESS;
}

int32_t filterAddVarValue(SFilterVarCtx *ctx, int32_t optr, void *key, int32_t keyLen, char *value, bool wild, bool *empty, bool *all) {
  SHashObj **hash = wild ? &ctx->wild : &ctx->value;

  if (*hash == NULL) {
    *hash = taosHashInit(FILTER_DEFAULT_GROUP_SIZE * FILTER_DEFAULT_VALUE_SIZE, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BINARY), false, false)
  }
  
  if (optr == TSDB_RELATION_AND) {
    SET_AND_OPTR(ctx, 0);

    if (ctx->status == 0) {
      taosHashPut(*hash, key, keyLen, value, sizeof(*value));
      FILTER_SET_FLAG(ctx->status, *value);
      return TSDB_CODE_SUCCESS;
    }

    char *v = taosHashGet(*hash, key, keyLen);
    if (v) {
      *v |= *value;
      
      if (*v == RA_EMPTY) {
        *empty = true;
      }

      return TSDB_CODE_SUCCESS;
    }

    if (wild) {
      taosHashPut(*hash, key, keyLen, value, sizeof(*value));
      FILTER_SET_FLAG(ctx->status, *value);
      
      return TSDB_CODE_SUCCESS;
    }
    
    if (*value == RA_INCLUDE) {
      if (FILTER_GET_FLAG(ctx->status, *value)) {
        *empty = true;
        return TSDB_CODE_SUCCESS;
      }

      taosHashClear(*hash);
      taosHashPut(*hash, key, keyLen, value, sizeof(*value));
      FILTER_SET_FLAG(ctx->status, *value);
      
      return TSDB_CODE_SUCCESS;
    }

    if (FILTER_GET_FLAG(ctx->status, RA_INCLUDE)) {
      return TSDB_CODE_SUCCESS;
    }

    taosHashPut(*hash, key, keyLen, value, sizeof(*value));

    return TSDB_CODE_SUCCESS;
  }

  // OR PROCESS

  SET_OR_OPTR(ctx, 0);
  
  if (ctx->status == 0) {
    taosHashPut(*hash, key, keyLen, value, sizeof(*value));
    FILTER_SET_FLAG(ctx->status, *value);
    return TSDB_CODE_SUCCESS;
  }
  
  char *v = taosHashGet(*hash, key, keyLen);
  if (v) {
    *v |= *value;
    
    if (*v == RA_ALL) {
      *all = true;
      FILTER_SET_FLAG(ctx->status, MR_ST_ALL);
    }
  
    return TSDB_CODE_SUCCESS;
  }
  
  return -1;
}

int32_t filterAddVarOptr(SFilterVarCtx *ctx, int32_t optr, int32_t raOptr, bool *empty, bool *all) {
  if (optr == TSDB_RELATION_AND) {
    SET_AND_OPTR(ctx, raOptr);
    if (CHK_AND_OPTR(ctx)) {
      FILTER_SET_FLAG(ctx->status, MR_ST_EMPTY);
      *empty = true;
    }
  } else {
    SET_OR_OPTR(ctx, raOptr);
    if (CHK_OR_OPTR(ctx)) {
      FILTER_SET_FLAG(ctx->status, MR_ST_ALL);
      *all = true;
    }
  }

  return TSDB_CODE_SUCCESS;
}

int32_t filterCopyVarCtx(void *dst, void *src) {
  SFilterVarCtx *dctx = (SFilterVarCtx *)dst;
  SFilterVarCtx *sctx = (SFilterVarCtx *)src;

  dctx->status = sctx->status;
  
  dctx->isnull = sctx->isnull;
  dctx->notnull = sctx->notnull;
  dctx->isrange = sctx->isrange;

  void *p = taosHashIterate(sctx->wild, NULL);
  while(p) {
    void *key = taosHashGetDataKey(sctx->wild, p);
    size_t keyLen = (size_t)taosHashGetDataKeyLen(sctx->wild, p);
    size_t dataLen = (size_t)varDataTLen(p);
    
    taosHashPut(dctx->wild, key, keyLen, p, dataLen);
    
    p = taosHashIterate(sctx->wild, p);
  }

  void *p = taosHashIterate(sctx->value, NULL);
  while(p) {
    void *key = taosHashGetDataKey(sctx->value, p);
    size_t keyLen = (size_t)taosHashGetDataKeyLen(sctx->value, p);
    size_t dataLen = (size_t)varDataTLen(p);
    
    taosHashPut(dctx->value, key, keyLen, p, dataLen);
    
    p = taosHashIterate(sctx->value, p);
  }

  return TSDB_CODE_SUCCESS;
}


int32_t filterResetVarCtx(SFilterVarCtx *ctx) {
  ctx->status = 0;

  ctx->isnull = false;
  ctx->notnull = false;
  ctx->isrange = false;

  taosHashClear(ctx->wild);
  taosHashClear(ctx->value);

  return TSDB_CODE_SUCCESS;
}


int32_t filterReuseVarCtx(SFilterVarCtx *ctx, int32_t type, int32_t options) {
  filterResetRangeCtx(ctx);

  ctx->type = type;
  ctx->options = options;

  return TSDB_CODE_SUCCESS;
}


void filterFreeVarCtx(SFilterVarCtx *ctx) {
  if (ctx == NULL) {
    return;
  }
  
  taosHashCleanup(ctx->wild);
  taosHashCleanup(ctx->value);
  
  tfree(ctx);
}


int32_t filterAddVarCtx(void *dst, void *src, int32_t optr) {
  SFilterVarCtx *dctx = (SFilterVarCtx *)dst;
  SFilterVarCtx *sctx = (SFilterVarCtx *)src;
  bool all = false;

  void *p = taosHashIterate(sctx->value, NULL);
  while(p) {
    void *key = taosHashGetDataKey(sctx->value, p);
    size_t keyLen = (size_t)taosHashGetDataKeyLen(sctx->value, p);
    
    filterAddVarValue(dctx, optr, key, keyLen, p, false, NULL, &all);
    if (all) {
      return TSDB_CODE_SUCCESS;
    }
    
    p = taosHashIterate(sctx->value, p);
  } 

  p = taosHashIterate(sctx->wild, NULL);
  while(p) {
    void *key = taosHashGetDataKey(sctx->wild, p);
    size_t keyLen = (size_t)taosHashGetDataKeyLen(sctx->wild, p);
    
    filterAddVarValue(dctx, optr, key, keyLen, p, true, NULL, NULL);
    
    p = taosHashIterate(sctx->wild, p);
  }

  return TSDB_CODE_SUCCESS;
}


int32_t filterSourceVarFromCtx(SFilterVarCtx *ctx, void *sctx, int32_t optr, bool *empty, bool *all) {
  SFilterVarCtx *src = (SFilterVarCtx *)sctx;

  if (src->isnull){
    filterAddVarOptr(ctx, TSDB_RELATION_ISNULL, optr, empty, all);
    if (FILTER_GET_FLAG(ctx->status, MR_ST_ALL)) {
      *all = true;
    }
  }

  if (src->notnull) {
    filterAddVarOptr(ctx, TSDB_RELATION_NOTNULL, optr, empty, all);
    if (FILTER_GET_FLAG(ctx->status, MR_ST_ALL)) {
      *all = true;
    }
  }

  if (src->isrange) {
    filterAddVarOptr(ctx, 0, optr, empty, all);

    if (!(optr == TSDB_RELATION_OR && ctx->notnull)) {
      filterAddVarCtx(ctx, src, optr);
    }
    
    if (FILTER_GET_FLAG(ctx->status, MR_ST_ALL)) {
      *all = true;
    }
  }

  return TSDB_CODE_SUCCESS;
}


int32_t filterDetachCnfGroup(SFilterGroup *gp1, SFilterGroup *gp2, SArray* group) {
  SFilterGroup gp = {0};

  //TODO CHECK DUP
  
  gp.unitNum = gp1->unitNum + gp2->unitNum;
  gp.unitIdxs = calloc(gp.unitNum, sizeof(*gp.unitIdxs));
  memcpy(gp.unitIdxs, gp1->unitIdxs, gp1->unitNum * sizeof(*gp.unitIdxs));
  memcpy(gp.unitIdxs + gp1->unitNum, gp2->unitIdxs, gp2->unitNum * sizeof(*gp.unitIdxs));    

  gp.unitFlags = NULL;
  
  taosArrayPush(group, &gp);

  return TSDB_CODE_SUCCESS;
}


int32_t filterDetachCnfGroups(SArray* group, SArray* left, SArray* right) {
  int32_t leftSize = (int32_t)taosArrayGetSize(left);
  int32_t rightSize = (int32_t)taosArrayGetSize(right);

  CHK_LRET(taosArrayGetSize(left) <= 0, TSDB_CODE_QRY_APP_ERROR, "empty group");
  CHK_LRET(taosArrayGetSize(right) <= 0, TSDB_CODE_QRY_APP_ERROR, "empty group");  
  
  for (int32_t l = 0; l < leftSize; ++l) {
    SFilterGroup *gp1 = taosArrayGet(left, l);
    
    for (int32_t r = 0; r < rightSize; ++r) {
      SFilterGroup *gp2 = taosArrayGet(right, r);

      filterDetachCnfGroup(gp1, gp2, group);
    }
  }


  return TSDB_CODE_SUCCESS;
}

int32_t filterGetFiledByDesc(SFilterFields* fields, int32_t type, void *v) {
  for (uint16_t i = 0; i < fields->num; ++i) {
    if (0 == gDescCompare[type](fields->fields[i].desc, v)) {
      return i;
    }
  }

  return -1;
}


int32_t filterGetFiledByData(SFilterInfo *info, int32_t type, void *v, int32_t dataLen) {
  if (type == FLD_TYPE_VALUE) {
    if (info->pctx.valHash == false) {
      qError("value hash is empty");
      return -1;
    }

    void *hv = taosHashGet(info->pctx.valHash, v, dataLen);
    if (hv) {
      return *(int32_t *)hv;
    }
  }

  return -1;
}


int32_t filterAddField(SFilterInfo *info, void *desc, void *data, int32_t type, SFilterFieldId *fid, int32_t dataLen) {
  int32_t idx = -1;
  uint16_t *num;

  num = &info->fields[type].num;

  if (*num > 0) {
    if (type == FLD_TYPE_COLUMN) {
      idx = filterGetFiledByDesc(&info->fields[type], type, desc);
    } else if (dataLen > 0 && FILTER_GET_FLAG(info->options, FI_OPTION_NEED_UNIQE)) {
      idx = filterGetFiledByData(info, type, data, dataLen);
    }
  }
  
  if (idx < 0) {
    idx = *num;
    if (idx >= info->fields[type].size) {
      info->fields[type].size += FILTER_DEFAULT_FIELD_SIZE;
      info->fields[type].fields = realloc(info->fields[type].fields, info->fields[type].size * sizeof(SFilterField));
    }
    
    info->fields[type].fields[idx].flag = type;  
    info->fields[type].fields[idx].desc = desc;
    info->fields[type].fields[idx].data = data;
    ++(*num);

    if (data && dataLen > 0 && FILTER_GET_FLAG(info->options, FI_OPTION_NEED_UNIQE)) {
      if (info->pctx.valHash == NULL) {
        info->pctx.valHash = taosHashInit(FILTER_DEFAULT_GROUP_SIZE * FILTER_DEFAULT_VALUE_SIZE, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BINARY), false, false);
      }
      
      taosHashPut(info->pctx.valHash, data, dataLen, &idx, sizeof(idx));
    }
  }

  fid->type = type;
  fid->idx = idx;
  
  return TSDB_CODE_SUCCESS;
}

static FORCE_INLINE int32_t filterAddColFieldFromField(SFilterInfo *info, SFilterField *field, SFilterFieldId *fid) {
  filterAddField(info, field->desc, field->data, FILTER_GET_TYPE(field->flag), fid, 0);

  FILTER_SET_FLAG(field->flag, FLD_DESC_NO_FREE);
  FILTER_SET_FLAG(field->flag, FLD_DATA_NO_FREE);

  return TSDB_CODE_SUCCESS;
}


int32_t filterAddFieldFromNode(SFilterInfo *info, tExprNode *node, SFilterFieldId *fid) {
  CHK_LRET(node == NULL, TSDB_CODE_QRY_APP_ERROR, "empty node");
  CHK_LRET(node->nodeType != TSQL_NODE_COL && node->nodeType != TSQL_NODE_VALUE, TSDB_CODE_QRY_APP_ERROR, "invalid nodeType:%d", node->nodeType);
  
  int32_t type;
  void *v;

  if (node->nodeType == TSQL_NODE_COL) {
    type = FLD_TYPE_COLUMN;
    v = node->pSchema;
    node->pSchema = NULL;
  } else {
    type = FLD_TYPE_VALUE;
    v = node->pVal;
    node->pVal = NULL;
  }

  filterAddField(info, v, NULL, type, fid, 0);
  
  return TSDB_CODE_SUCCESS;
}

int32_t filterAddUnit(SFilterInfo *info, uint8_t optr, SFilterFieldId *left, SFilterFieldId *right, uint16_t *uidx) {
  if (FILTER_GET_FLAG(info->options, FI_OPTION_NEED_UNIQE)) {
    if (info->pctx.unitHash == NULL) {
      info->pctx.unitHash = taosHashInit(FILTER_DEFAULT_GROUP_SIZE * FILTER_DEFAULT_UNIT_SIZE, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BIGINT), false, false);
    } else {
      int64_t v = 0;
      FILTER_PACKAGE_UNIT_HASH_KEY(&v, optr, left->idx, right ? right->idx : -1);
      void *hu = taosHashGet(info->pctx.unitHash, &v, sizeof(v));
      if (hu) {
        *uidx = *(uint16_t *)hu;
        return TSDB_CODE_SUCCESS;
      }
    }
  }

  if (info->unitNum >= info->unitSize) {
    uint16_t psize = info->unitSize;
    info->unitSize += FILTER_DEFAULT_UNIT_SIZE;
    info->units = realloc(info->units, info->unitSize * sizeof(SFilterUnit));
    memset(info->units + psize, 0, sizeof(*info->units) * FILTER_DEFAULT_UNIT_SIZE);
  }

  SFilterUnit *u = &info->units[info->unitNum];
  
  u->compare.optr = optr;
  u->left = *left;
  if (right) {
    u->right = *right;
  }

  if (u->right.type == FLD_TYPE_VALUE) {
    SFilterField *val = FILTER_UNIT_RIGHT_FIELD(info, u);  
    assert(FILTER_GET_FLAG(val->flag, FLD_TYPE_VALUE));
  } else {
    assert(optr == TSDB_RELATION_ISNULL || optr == TSDB_RELATION_NOTNULL);
  }
  
  SFilterField *col = FILTER_UNIT_LEFT_FIELD(info, u);
  assert(FILTER_GET_FLAG(col->flag, FLD_TYPE_COLUMN));
  
  info->units[info->unitNum].compare.type = FILTER_GET_COL_FIELD_TYPE(col);

  *uidx = info->unitNum;

  if (FILTER_GET_FLAG(info->options, FI_OPTION_NEED_UNIQE)) {
    int64_t v = 0;
    FILTER_PACKAGE_UNIT_HASH_KEY(&v, optr, left->idx, right ? right->idx : -1);  
    taosHashPut(info->pctx.unitHash, &v, sizeof(v), uidx, sizeof(*uidx));
  }
  
  ++info->unitNum;
  
  return TSDB_CODE_SUCCESS;
}



int32_t filterAddUnitToGroup(SFilterGroup *group, uint16_t unitIdx) {
  if (group->unitNum >= group->unitSize) {
    group->unitSize += FILTER_DEFAULT_UNIT_SIZE;
    group->unitIdxs = realloc(group->unitIdxs, group->unitSize * sizeof(*group->unitIdxs));
  }
  
  group->unitIdxs[group->unitNum++] = unitIdx;

  return TSDB_CODE_SUCCESS;
}



int32_t filterAddGroupUnitFromNode(SFilterInfo *info, tExprNode* tree, SArray *group) {
  SFilterFieldId left = {0}, right = {0};

  filterAddFieldFromNode(info, tree->_node.pLeft, &left);  

  tVariant* var = tree->_node.pRight->pVal;
  int32_t type = FILTER_GET_COL_FIELD_TYPE(FILTER_GET_FIELD(info, left));
  int32_t len = 0;
  uint16_t uidx = 0;

  if (tree->_node.optr == TSDB_RELATION_IN) {
    void *data = NULL;
    convertFilterSetFromBinary((void **)&data, var->pz, var->nLen, type);
    CHK_LRET(data == NULL, TSDB_CODE_QRY_APP_ERROR, "failed to convert in param");

    void *p = taosHashIterate((SHashObj *)data, NULL);
    while(p) {
      void *key = taosHashGetDataKey((SHashObj *)data, p);
      void *fdata = NULL;
    
      if (IS_VAR_DATA_TYPE(type)) {
        len = (int32_t)taosHashGetDataKeyLen((SHashObj *)data, p);
        fdata = malloc(len + VARSTR_HEADER_SIZE);
        varDataLen(fdata) = len;
        memcpy(varDataVal(fdata), key, len);
        len += VARSTR_HEADER_SIZE;
      } else {
        fdata = malloc(sizeof(int64_t));
        SIMPLE_COPY_VALUES(fdata, key);
        len = tDataTypes[type].bytes;
      }
      
      filterAddField(info, NULL, fdata, FLD_TYPE_VALUE, &right, len);

      filterAddUnit(info, TSDB_RELATION_EQUAL, &left, &right, &uidx);
      
      SFilterGroup fgroup = {0};
      filterAddUnitToGroup(&fgroup, uidx);
      
      taosArrayPush(group, &fgroup);
      
      p = taosHashIterate((SHashObj *)data, p);
    }
  } else {
    filterAddFieldFromNode(info, tree->_node.pRight, &right);  
    
    filterAddUnit(info, tree->_node.optr, &left, &right, &uidx);  

    SFilterGroup fgroup = {0};
    filterAddUnitToGroup(&fgroup, uidx);
    
    taosArrayPush(group, &fgroup);
  }
  
  return TSDB_CODE_SUCCESS;
}


int32_t filterAddUnitFromUnit(SFilterInfo *dst, SFilterInfo *src, SFilterUnit* u, uint16_t *uidx) {
  SFilterFieldId left, right, *pright = &right;
  int32_t type = FILTER_UNIT_DATA_TYPE(u);

  filterAddField(dst, FILTER_UNIT_COL_DESC(src, u), NULL, FLD_TYPE_COLUMN, &left, 0);
  SFilterField *t = FILTER_UNIT_LEFT_FIELD(src, u);
  FILTER_SET_FLAG(t->flag, FLD_DESC_NO_FREE);

  if (u->right.type == FLD_TYPE_VALUE) {
    void *data = FILTER_UNIT_VAL_DATA(src, u);
    if (IS_VAR_DATA_TYPE(type)) {
      if (FILTER_UNIT_OPTR(u) ==  TSDB_RELATION_IN) {
        filterAddField(dst, NULL, data, FLD_TYPE_VALUE, &right, 0);
      } else {
        filterAddField(dst, NULL, data, FLD_TYPE_VALUE, &right, varDataTLen(data));
      }
    } else {
      filterAddField(dst, NULL, data, FLD_TYPE_VALUE, &right, tDataTypes[type].bytes);
    }
    t = FILTER_UNIT_RIGHT_FIELD(src, u);
    FILTER_SET_FLAG(t->flag, FLD_DATA_NO_FREE);
  } else {
    pright = NULL;
  }
  

  return filterAddUnit(dst, FILTER_UNIT_OPTR(u), &left, pright, uidx);
}


int32_t filterAddGroupUnitFromCtx(SFilterInfo *dst, SFilterInfo *src, SFilterRangeCtx *ctx, uint16_t cidx, SFilterGroup *g, int32_t optr, SArray *res) {
  SFilterFieldId left, right;
  uint16_t uidx = 0;

  SFilterField *col = FILTER_GET_COL_FIELD(src, cidx);

  filterAddColFieldFromField(dst, col, &left);

  int32_t type = FILTER_GET_COL_FIELD_TYPE(FILTER_GET_FIELD(dst, left));

  if (optr == TSDB_RELATION_AND) {
    if (ctx->isnull) {
      assert(ctx->notnull == false && ctx->isrange == false);
      filterAddUnit(dst, TSDB_RELATION_ISNULL, &left, NULL, &uidx);
      filterAddUnitToGroup(g, uidx);
      return TSDB_CODE_SUCCESS;
    }

    if (ctx->notnull) {      
      assert(ctx->isnull == false && ctx->isrange == false);
      filterAddUnit(dst, TSDB_RELATION_NOTNULL, &left, NULL, &uidx);
      filterAddUnitToGroup(g, uidx);
      return TSDB_CODE_SUCCESS;
    }

    if (!ctx->isrange) {
      assert(ctx->isnull || ctx->notnull);
      return TSDB_CODE_SUCCESS;
    }

    assert(ctx->rs && ctx->rs->next == NULL);

    SFilterRange *ra = &ctx->rs->ra;
    
    assert(!((FILTER_GET_FLAG(ra->sflag, RA_NULL)) && (FILTER_GET_FLAG(ra->eflag, RA_NULL))));

    if ((!FILTER_GET_FLAG(ra->sflag, RA_NULL)) && (!FILTER_GET_FLAG(ra->eflag, RA_NULL))) {
      __compar_fn_t func = getComparFunc(type, 0);
      if (func(&ra->s, &ra->e) == 0) {
        void *data = malloc(sizeof(int64_t));
        SIMPLE_COPY_VALUES(data, &ra->s);
        filterAddField(dst, NULL, data, FLD_TYPE_VALUE, &right, tDataTypes[type].bytes);
        filterAddUnit(dst, TSDB_RELATION_EQUAL, &left, &right, &uidx);
        filterAddUnitToGroup(g, uidx);
        return TSDB_CODE_SUCCESS;              
      }
    }
    
    if (!FILTER_GET_FLAG(ra->sflag, RA_NULL)) {
      void *data = malloc(sizeof(int64_t));
      SIMPLE_COPY_VALUES(data, &ra->s);
      filterAddField(dst, NULL, data, FLD_TYPE_VALUE, &right, tDataTypes[type].bytes);
      filterAddUnit(dst, FILTER_GET_FLAG(ra->sflag, RA_EXCLUDE) ? TSDB_RELATION_GREATER : TSDB_RELATION_GREATER_EQUAL, &left, &right, &uidx);
      filterAddUnitToGroup(g, uidx);
    }

    if (!FILTER_GET_FLAG(ra->eflag, RA_NULL)) {
      void *data = malloc(sizeof(int64_t));
      SIMPLE_COPY_VALUES(data, &ra->e);
      filterAddField(dst, NULL, data, FLD_TYPE_VALUE, &right, tDataTypes[type].bytes);
      filterAddUnit(dst, FILTER_GET_FLAG(ra->eflag, RA_EXCLUDE) ? TSDB_RELATION_LESS : TSDB_RELATION_LESS_EQUAL, &left, &right, &uidx);
      filterAddUnitToGroup(g, uidx);
    }    

    return TSDB_CODE_SUCCESS;      
  } 

  // OR PROCESS
  
  SFilterGroup ng = {0};
  g = &ng;

  assert(ctx->isnull || ctx->notnull || ctx->isrange);
  
  if (ctx->isnull) {
    filterAddUnit(dst, TSDB_RELATION_ISNULL, &left, NULL, &uidx);
    filterAddUnitToGroup(g, uidx);    
    taosArrayPush(res, g);
  }
  
  if (ctx->notnull) {
    assert(!ctx->isrange);
    memset(g, 0, sizeof(*g));
    
    filterAddUnit(dst, TSDB_RELATION_NOTNULL, &left, NULL, &uidx);
    filterAddUnitToGroup(g, uidx);
    taosArrayPush(res, g);
  }

  if (!ctx->isrange) {
    assert(ctx->isnull || ctx->notnull);
    g->unitNum = 0;
    return TSDB_CODE_SUCCESS;
  }

  SFilterRangeNode *r = ctx->rs;
  
  while (r) {
    memset(g, 0, sizeof(*g));

    if ((!FILTER_GET_FLAG(r->ra.sflag, RA_NULL)) &&(!FILTER_GET_FLAG(r->ra.eflag, RA_NULL))) {
      __compar_fn_t func = getComparFunc(type, 0);
      if (func(&r->ra.s, &r->ra.e) == 0) {
        void *data = malloc(sizeof(int64_t));
        SIMPLE_COPY_VALUES(data, &r->ra.s);
        filterAddField(dst, NULL, data, FLD_TYPE_VALUE, &right, tDataTypes[type].bytes);
        filterAddUnit(dst, TSDB_RELATION_EQUAL, &left, &right, &uidx);
        filterAddUnitToGroup(g, uidx);
        
        taosArrayPush(res, g);

        r = r->next;
        continue;
      }
    }
    
    if (!FILTER_GET_FLAG(r->ra.sflag, RA_NULL)) {
      void *data = malloc(sizeof(int64_t));
      SIMPLE_COPY_VALUES(data, &r->ra.s);
      filterAddField(dst, NULL, data, FLD_TYPE_VALUE, &right, tDataTypes[type].bytes);
      filterAddUnit(dst, FILTER_GET_FLAG(r->ra.sflag, RA_EXCLUDE) ? TSDB_RELATION_GREATER : TSDB_RELATION_GREATER_EQUAL, &left, &right, &uidx);
      filterAddUnitToGroup(g, uidx);
    }
    
    if (!FILTER_GET_FLAG(r->ra.eflag, RA_NULL)) {
      void *data = malloc(sizeof(int64_t));
      SIMPLE_COPY_VALUES(data, &r->ra.e);    
      filterAddField(dst, NULL, data, FLD_TYPE_VALUE, &right, tDataTypes[type].bytes);
      filterAddUnit(dst, FILTER_GET_FLAG(r->ra.eflag, RA_EXCLUDE) ? TSDB_RELATION_LESS : TSDB_RELATION_LESS_EQUAL, &left, &right, &uidx);
      filterAddUnitToGroup(g, uidx);
    }

    assert (g->unitNum > 0);

    taosArrayPush(res, g);

    r = r->next;
  }

  g->unitNum = 0;

  return TSDB_CODE_SUCCESS;
}


static void filterFreeGroup(void *pItem) {
  SFilterGroup* p = (SFilterGroup*) pItem;
  if (p) {
    tfree(p->unitIdxs);
    tfree(p->unitFlags);
  }
}


int32_t filterTreeToGroup(tExprNode* tree, SFilterInfo *info, SArray* group) {
  int32_t code = TSDB_CODE_SUCCESS;
  SArray* leftGroup = NULL;
  SArray* rightGroup = NULL;
  
  if (tree->nodeType != TSQL_NODE_EXPR) {
    qError("invalid nodeType:%d", tree->nodeType); 
    return TSDB_CODE_QRY_APP_ERROR;
  }
  
  if (tree->_node.optr == TSDB_RELATION_AND) {
    leftGroup = taosArrayInit(4, sizeof(SFilterGroup));
    rightGroup = taosArrayInit(4, sizeof(SFilterGroup));
    ERR_JRET(filterTreeToGroup(tree->_node.pLeft, info, leftGroup));
    ERR_JRET(filterTreeToGroup(tree->_node.pRight, info, rightGroup));

    ERR_JRET(filterDetachCnfGroups(group, leftGroup, rightGroup));

    taosArrayDestroyEx(leftGroup, filterFreeGroup);
    taosArrayDestroyEx(rightGroup, filterFreeGroup);
    
    return TSDB_CODE_SUCCESS;
  }

  if (tree->_node.optr == TSDB_RELATION_OR) {
    ERR_RET(filterTreeToGroup(tree->_node.pLeft, info, group));
    ERR_RET(filterTreeToGroup(tree->_node.pRight, info, group));

    return TSDB_CODE_SUCCESS;
  }

  filterAddGroupUnitFromNode(info, tree, group);  


_err_return:

  taosArrayDestroyEx(leftGroup, filterFreeGroup);
  taosArrayDestroyEx(rightGroup, filterFreeGroup);
  
  return code;
}

int32_t filterInitUnitFunc(SFilterInfo *info) {
  for (uint16_t i = 0; i < info->unitNum; ++i) {
    SFilterUnit* unit = &info->units[i];
    unit->compare.pCompareFunc = getComparFunc(FILTER_UNIT_DATA_TYPE(unit), unit->compare.optr);
  }

  return TSDB_CODE_SUCCESS;
}



void filterDumpInfoToString(SFilterInfo *info, const char *msg) {
  CHK_LRETV(info == NULL, "%s - FilterInfo: empty", msg);

  qDebug("%s - FilterInfo:", msg);
  qDebug("COLUMN Field Num:%u", info->fields[FLD_TYPE_COLUMN].num);
  for (uint16_t i = 0; i < info->fields[FLD_TYPE_COLUMN].num; ++i) {
    SFilterField *field = &info->fields[FLD_TYPE_COLUMN].fields[i];
    SSchema *sch = field->desc;
    qDebug("COL%d => [%d][%s]", i, sch->colId, sch->name);
  }

  qDebug("VALUE Field Num:%u", info->fields[FLD_TYPE_VALUE].num);
  for (uint16_t i = 0; i < info->fields[FLD_TYPE_VALUE].num; ++i) {
    SFilterField *field = &info->fields[FLD_TYPE_VALUE].fields[i];
    if (field->desc) {
      tVariant *var = field->desc;
      if (var->nType == TSDB_DATA_TYPE_VALUE_ARRAY) {
        qDebug("VAL%d => [type:TS][val:[%" PRIi64"] - [%" PRId64 "]]", i, *(int64_t *)field->data, *(((int64_t *)field->data) + 1)); 
      } else {
        qDebug("VAL%d => [type:%d][val:%" PRIi64"]", i, var->nType, var->i64); //TODO
      }
    } else {
      qDebug("VAL%d => [type:NIL][val:0x%" PRIx64"]", i, *(int64_t *)field->data); //TODO
    }
  }

  qDebug("Unit  Num:%u", info->unitNum);
  for (uint16_t i = 0; i < info->unitNum; ++i) {
    SFilterUnit *unit = &info->units[i];
    int32_t type = FILTER_UNIT_DATA_TYPE(unit);
    int32_t len = 0;
    int32_t tlen = 0;
    char str[128] = {0};
    
    SFilterField *left = FILTER_UNIT_LEFT_FIELD(info, unit);
    SSchema *sch = left->desc;
    len = sprintf(str, "UNIT[%d] => [%d][%s]  %s  [", i, sch->colId, sch->name, gOptrStr[unit->compare.optr].str);

    if (unit->right.type == FLD_TYPE_VALUE && FILTER_UNIT_OPTR(unit) != TSDB_RELATION_IN) {
      SFilterField *right = FILTER_UNIT_RIGHT_FIELD(info, unit);
      char *data = right->data;
      if (IS_VAR_DATA_TYPE(type)) {
        tlen = varDataLen(data);
        data += VARSTR_HEADER_SIZE;
      }
      converToStr(str + len, type, data, tlen, &tlen);
    } else {
      strcat(str, "NULL");
    }
    strcat(str, "]");
    
    qDebug("%s", str); //TODO
  }

  qDebug("Group Num:%u", info->groupNum);
  for (uint16_t i = 0; i < info->groupNum; ++i) {
    SFilterGroup *group = &info->groups[i];
    qDebug("Group%d : unit num[%u]", i, group->unitNum);

    for (uint16_t u = 0; u < group->unitNum; ++u) {
      qDebug("unit id:%u", group->unitIdxs[u]);
    }
  }
}

void filterFreeColInfo(void *data) {
  SFilterColInfo* info = (SFilterColInfo *)data;

  if (info->info == NULL) {
    return;
  }

  if (info->type == RANGE_TYPE_COL_RANGE) {
    tfree(info->info);
  } else if (info->type == RANGE_TYPE_MR_CTX) {
    filterFreeRangeCtx(info->info);  
  } else if (info->type == RANGE_TYPE_UNIT) {
    taosArrayDestroy((SArray *)info->info);
  }

  //NO NEED TO FREE UNIT

  info->type = 0;
  info->info = NULL;
}

void filterFreeColCtx(void *data) {
  SFilterColCtx* ctx = (SFilterColCtx *)data;

  if (ctx->ctx) {
    filterFreeRangeCtx(ctx->ctx);
  }
}


void filterFreeGroupCtx(SFilterGroupCtx* gRes) {
  if (gRes == NULL) {
    return;
  }

  tfree(gRes->colIdx);

  int16_t i = 0, j = 0;

  while (i < gRes->colNum) {
    if (gRes->colInfo[j].info) {
      filterFreeColInfo(&gRes->colInfo[j]);
      ++i;
    }

    ++j;
  }

  tfree(gRes->colInfo);
  tfree(gRes);
}

void filterFreeInfo(SFilterInfo *info) {
  CHK_RETV(info == NULL);

  //TODO
}



int32_t filterInitValFieldData(SFilterInfo *info) {
  for (uint16_t i = 0; i < info->unitNum; ++i) {
    SFilterUnit* unit = &info->units[i];
    if (unit->right.type != FLD_TYPE_VALUE) {
      assert(unit->compare.optr == TSDB_RELATION_ISNULL || unit->compare.optr == TSDB_RELATION_NOTNULL);
      continue;
    }
    
    SFilterField* right = FILTER_UNIT_RIGHT_FIELD(info, unit);

    assert(FILTER_GET_FLAG(right->flag, FLD_TYPE_VALUE));

    uint32_t type = FILTER_UNIT_DATA_TYPE(unit);
    SFilterField* fi = right;
    
    tVariant* var = fi->desc;

    if (var == NULL) {
      assert(fi->data != NULL);
      continue;
    }

    if (unit->compare.optr == TSDB_RELATION_IN) {
      convertFilterSetFromBinary((void **)&fi->data, var->pz, var->nLen, type);
      CHK_LRET(fi->data == NULL, TSDB_CODE_QRY_APP_ERROR, "failed to convert in param");
    
      continue;
    }

    if (type == TSDB_DATA_TYPE_BINARY) {
      fi->data = calloc(1, (var->nLen + 1) * TSDB_NCHAR_SIZE);
    } else if (type == TSDB_DATA_TYPE_NCHAR) {
      fi->data = calloc(1, (var->nLen + 1) * TSDB_NCHAR_SIZE);
    } else {
      if (var->nType == TSDB_DATA_TYPE_VALUE_ARRAY) {  //TIME RANGE
        fi->data = calloc(var->nLen, tDataTypes[type].bytes);
        for (int32_t a = 0; a < var->nLen; ++a) {
          int64_t *v = taosArrayGet(var->arr, a);
          assignVal(fi->data + a * tDataTypes[type].bytes, (char *)v, 0, type);
        }

        continue;
      } else {
        fi->data = calloc(1, sizeof(int64_t));
      }
    }

    ERR_LRET(tVariantDump(var, (char*)fi->data, type, true), "dump type[%d] failed", type);
  }

  return TSDB_CODE_SUCCESS;
}



bool filterDoCompare(SFilterUnit *unit, void *left, void *right) {
  int32_t ret = unit->compare.pCompareFunc(left, right);

  switch (unit->compare.optr) {
    case TSDB_RELATION_EQUAL: {
      return ret == 0;
    }
    case TSDB_RELATION_NOT_EQUAL: {
      return ret != 0;
    }
    case TSDB_RELATION_GREATER_EQUAL: {
      return ret >= 0;
    }
    case TSDB_RELATION_GREATER: {
      return ret > 0;
    }
    case TSDB_RELATION_LESS_EQUAL: {
      return ret <= 0;
    }
    case TSDB_RELATION_LESS: {
      return ret < 0;
    }
    case TSDB_RELATION_LIKE: {
      return ret == 0;
    }
    case TSDB_RELATION_IN: {
      return ret == 1;
    }

    default:
      assert(false);
  }

  return true;
}


int32_t filterAddUnitRange(SFilterInfo *info, SFilterUnit* u, SFilterRangeCtx *ctx, int32_t optr) {
  int32_t type = FILTER_UNIT_DATA_TYPE(u);
  uint8_t uoptr = FILTER_UNIT_OPTR(u);
  void *val = FILTER_UNIT_VAL_DATA(info, u);
  SFilterRange ra = {0};
  int64_t tmp = 0;

  switch (uoptr) {
    case TSDB_RELATION_GREATER:
      SIMPLE_COPY_VALUES(&ra.s, val);
      FILTER_SET_FLAG(ra.sflag, RA_EXCLUDE);
      FILTER_SET_FLAG(ra.eflag, RA_NULL);
      break;
    case TSDB_RELATION_GREATER_EQUAL:
      SIMPLE_COPY_VALUES(&ra.s, val);
      FILTER_SET_FLAG(ra.eflag, RA_NULL);
      break;
    case TSDB_RELATION_LESS:
      SIMPLE_COPY_VALUES(&ra.e, val);
      FILTER_SET_FLAG(ra.eflag, RA_EXCLUDE);
      FILTER_SET_FLAG(ra.sflag, RA_NULL);
      break;
    case TSDB_RELATION_LESS_EQUAL:
      SIMPLE_COPY_VALUES(&ra.e, val);
      FILTER_SET_FLAG(ra.sflag, RA_NULL);
      break;
    case TSDB_RELATION_NOT_EQUAL:
      assert(type == TSDB_DATA_TYPE_BOOL);
      if (GET_INT8_VAL(val)) {
        SIMPLE_COPY_VALUES(&ra.s, &tmp);
        SIMPLE_COPY_VALUES(&ra.e, &tmp);        
      } else {
        *(bool *)&tmp = true;
        SIMPLE_COPY_VALUES(&ra.s, &tmp);
        SIMPLE_COPY_VALUES(&ra.e, &tmp);        
      }
      break;
    case TSDB_RELATION_EQUAL:
      SIMPLE_COPY_VALUES(&ra.s, val);
      SIMPLE_COPY_VALUES(&ra.e, val);
      break;
    default:
      assert(0);
  }
  
  filterAddRange(ctx, &ra, optr);

  return TSDB_CODE_SUCCESS;
}

int32_t filterCompareRangeCtx(SFilterRangeCtx *ctx1, SFilterRangeCtx *ctx2, bool *equal) {
  CHK_JMP(ctx1->status != ctx2->status);
  CHK_JMP(ctx1->isnull != ctx2->isnull);
  CHK_JMP(ctx1->notnull != ctx2->notnull);
  CHK_JMP(ctx1->isrange != ctx2->isrange);

  SFilterRangeNode *r1 = ctx1->rs;
  SFilterRangeNode *r2 = ctx2->rs;
  
  while (r1 && r2) {
    CHK_JMP(r1->ra.sflag != r2->ra.sflag);
    CHK_JMP(r1->ra.eflag != r2->ra.eflag);
    CHK_JMP(r1->ra.s != r2->ra.s);
    CHK_JMP(r1->ra.e != r2->ra.e);
    
    r1 = r1->next;
    r2 = r2->next;
  }

  CHK_JMP(r1 != r2);

  *equal = true;

  return TSDB_CODE_SUCCESS;

_err_return:
  *equal = false;
  return TSDB_CODE_SUCCESS;
}


int32_t filterMergeUnitsImpl(SFilterInfo *info, SFilterGroupCtx* gRes, uint16_t colIdx, bool *empty) {
  SArray* colArray = (SArray *)gRes->colInfo[colIdx].info;
  int32_t size = (int32_t)taosArrayGetSize(colArray);
  int32_t type = gRes->colInfo[colIdx].dataType;
  SFilterRangeCtx* ctx = filterInitRangeCtx(type, 0);
  
  for (uint32_t i = 0; i < size; ++i) {
    SFilterUnit* u = taosArrayGetP(colArray, i);
    uint8_t optr = FILTER_UNIT_OPTR(u);

    filterAddRangeOptr(ctx, optr, TSDB_RELATION_AND, empty, NULL);
    CHK_JMP(*empty);

    if (!FILTER_NO_MERGE_OPTR(optr)) {
      filterAddUnitRange(info, u, ctx, TSDB_RELATION_AND);
      CHK_JMP(MR_EMPTY_RES(ctx));
    }
  }

  taosArrayDestroy(colArray);

  FILTER_PUSH_CTX(gRes->colInfo[colIdx], ctx);

  return TSDB_CODE_SUCCESS;

_err_return:

  *empty = true;

  filterFreeRangeCtx(ctx);

  return TSDB_CODE_SUCCESS;
}

int32_t filterMergeUnitsVarImpl(SFilterInfo *info, SFilterGroupCtx* gRes, uint16_t colIdx, bool *empty) {
  SArray* colArray = (SArray *)gRes->colInfo[colIdx].info;
  int32_t size = (int32_t)taosArrayGetSize(colArray);
  int32_t type = gRes->colInfo[colIdx].dataType;
  SFilterVarCtx* ctx = filterInitVarCtx(type, 0);
  char value = RA_INCLUDE;
  
  for (uint32_t i = 0; i < size; ++i) {
    SFilterUnit* u = taosArrayGetP(colArray, i);
    uint8_t optr = FILTER_UNIT_OPTR(u);
    void *data = FILTER_UNIT_VAL_DATA(info, u);

    switch (optr) {
      case TSDB_RELATION_LIKE:
        value = RA_INCLUDE;
        filterAddVarValue(ctx, TSDB_RELATION_AND, data, varDataLen(data), &value, true, empty, NULL);
        break;
      case TSDB_RELATION_EQUAL:
        value = RA_INCLUDE;
        filterAddVarValue(ctx, TSDB_RELATION_AND, data, varDataLen(data), &value, false, empty, NULL);
        break;
      case TSDB_RELATION_NOT_EQUAL:
        value = RA_EXCLUDE;
        filterAddVarValue(ctx, TSDB_RELATION_AND, data, varDataLen(data), &value, false, empty, NULL);
        break;
      case TSDB_RELATION_ISNULL:
      case TSDB_RELATION_NOTNULL:
        filterAddVarOptr(ctx, TSDB_RELATION_AND, optr, empty, NULL);
        break;
      default:
        assert(0);
    }
      
    CHK_JMP(*empty);
  }

  taosArrayDestroy(colArray);

  FILTER_PUSH_VAR_HASH(gRes->colInfo[colIdx], ctx);

  return TSDB_CODE_SUCCESS;

_err_return:

  filterFreeVarCtx(ctx);

  return TSDB_CODE_SUCCESS;
}


int32_t filterMergeUnits(SFilterInfo *info, SFilterGroupCtx* gRes, uint16_t colIdx, bool *empty) {
  int32_t type = gRes->colInfo[colIdx].dataType;
  if (IS_VAR_DATA_TYPE(type)) {
    return filterMergeUnitsVarImpl(info, gRes, colIdx, empty);
  }

  return filterMergeUnitsImpl(info, gRes, colIdx, empty);
}

int32_t filterMergeGroupUnits(SFilterInfo *info, SFilterGroupCtx** gRes, int32_t* gResNum) {
  bool empty = false;
  uint16_t *colIdx = malloc(info->fields[FLD_TYPE_COLUMN].num * sizeof(uint16_t));
  uint16_t colIdxi = 0;
  uint16_t gResIdx = 0;
  
  for (uint16_t i = 0; i < info->groupNum; ++i) {
    SFilterGroup* g = info->groups + i;

    gRes[gResIdx] = calloc(1, sizeof(SFilterGroupCtx));
    gRes[gResIdx]->colInfo = calloc(info->fields[FLD_TYPE_COLUMN].num, sizeof(SFilterColInfo));
    colIdxi = 0;
    empty = false;
    
    for (uint16_t j = 0; j < g->unitNum; ++j) {
      SFilterUnit* u = FILTER_GROUP_UNIT(info, g, j);
      uint16_t cidx = FILTER_UNIT_COL_IDX(u);

      if (gRes[gResIdx]->colInfo[cidx].info == NULL) {
        gRes[gResIdx]->colInfo[cidx].info = (SArray *)taosArrayInit(4, POINTER_BYTES);
        colIdx[colIdxi++] = cidx;
        ++gRes[gResIdx]->colNum;
      } else {
        FILTER_SET_FLAG(info->status, FI_STATUS_REWRITE);
      }
      
      FILTER_PUSH_UNIT(gRes[gResIdx]->colInfo[cidx], u);
    }

    if (colIdxi > 1) {
      qsort(colIdx, colIdxi, sizeof(uint16_t), getComparFunc(TSDB_DATA_TYPE_USMALLINT, 0));
    }

    for (uint16_t l = 0; l < colIdxi; ++l) {
      filterMergeUnits(info, gRes[gResIdx], colIdx[l], &empty);

      if (empty) {
        break;
      }
    }

    if (empty) {
      filterFreeGroupCtx(gRes[gResIdx]);
      gRes[gResIdx] = NULL;
    
      continue;
    }
    
    gRes[gResIdx]->colNum = colIdxi;
    FILTER_COPY_IDX(&gRes[gResIdx]->colIdx, colIdx, colIdxi);
    ++gResIdx;
  }

  tfree(colIdx);

  *gResNum = gResIdx;
  
  if (gResIdx == 0) {
    FILTER_SET_FLAG(info->status, FI_STATUS_EMPTY);
  }

  return TSDB_CODE_SUCCESS;
}

void filterCheckColConflict(SFilterGroupCtx* gRes1, SFilterGroupCtx* gRes2, bool *conflict) {
  uint16_t idx1 = 0, idx2 = 0, m = 0, n = 0;
  bool equal = false;
  
  for (; m < gRes1->colNum; ++m) {
    idx1 = gRes1->colIdx[m];

    equal = false;
  
    for (; n < gRes2->colNum; ++n) {
      idx2 = gRes2->colIdx[n];
      if (idx1 < idx2) {
        *conflict = true;
        return;
      }

      if (idx1 > idx2) {
        continue;
      }
      
      ++n;
      equal = true;
      break;
    }

    if (!equal) {
      *conflict = true;
      return;
    }
  }

  *conflict = false;
  return;
}


int32_t filterMergeTwoGroupsImpl(SFilterInfo *info, void **ctx, int32_t optr, uint16_t cidx, SFilterGroupCtx* gRes1, SFilterGroupCtx* gRes2, bool *empty, bool *all) {
  SFilterField *fi = FILTER_GET_COL_FIELD(info, cidx);
  int32_t type = FILTER_GET_COL_FIELD_TYPE(fi);

  if ((*ctx) == NULL) {
    *ctx = filterInitRangeCtx(type, 0);
  } else {
    filterReuseRangeCtx(*ctx, type, 0);
  }

  assert(gRes2->colInfo[cidx].type == RANGE_TYPE_MR_CTX);
  assert(gRes1->colInfo[cidx].type == RANGE_TYPE_MR_CTX);

  filterCopyRangeCtx(*ctx, gRes2->colInfo[cidx].info);
  filterSourceRangeFromCtx(*ctx, gRes1->colInfo[cidx].info, optr, empty, all);

  return TSDB_CODE_SUCCESS;
}

int32_t filterMergeTwoVarGroupsImpl(SFilterInfo *info, void **ctx, int32_t optr, uint16_t cidx, SFilterGroupCtx* gRes1, SFilterGroupCtx* gRes2, bool *empty, bool *all) {
  SFilterField *fi = FILTER_GET_COL_FIELD(info, cidx);
  int32_t type = FILTER_GET_COL_FIELD_TYPE(fi);

  if ((*ctx) == NULL) {
    *ctx = filterInitVarCtx(type, 0);
  } else {
    filterReuseVarCtx(*ctx, type, 0);
  }

  assert(gRes2->colInfo[cidx].type == RANGE_TYPE_MR_CTX);
  assert(gRes1->colInfo[cidx].type == RANGE_TYPE_MR_CTX);

  filterCopyVarCtx(*ctx, gRes2->colInfo[cidx].info);
  filterSourceVarFromCtx(*ctx, gRes1->colInfo[cidx].info, optr, empty, all);

  return TSDB_CODE_SUCCESS;
}


int32_t filterCheckGroupMergeRes(SFilterRangeCtx *ctx, SFilterGroupCtx** gRes1, SFilterGroupCtx** gRes2, uint16_t idx, SArray* colCtxs,
                                         uint16_t *equal1, uint16_t *equal2, uint16_t total, bool *success) {
  SFilterColCtx colCtx = {0};
  bool equal = false;

  if ((*gRes1)->colNum == (*gRes2)->colNum) {
    if ((*gRes1)->colNum == 1) {
      ++(*equal1);
      colCtx.colIdx = idx;
      colCtx.ctx = ctx;
      taosArrayPush(colCtxs, &colCtx);
      break;
    } else {
      filterCompareRangeCtx(ctx, (*gRes1)->colInfo[idx].info, &equal);
      if (equal) {
        ++(*equal1);
      }
      
      filterCompareRangeCtx(ctx, (*gRes2)->colInfo[idx].info, &equal);
      if (equal) {
        ++(*equal2);
      }
  
      CHK_JMP((*equal1) != total && (*equal2) != total);
      colCtx.colIdx = idx;
      colCtx.ctx = ctx;
      ctx = NULL;
      taosArrayPush(colCtxs, &colCtx);
    }
  } else {
    filterCompareRangeCtx(ctx, (*gRes1)->colInfo[idx].info, &equal);
    if (equal) {
      ++(*equal1);
    }
  
    CHK_JMP((*equal1) != total);
    colCtx.colIdx = idx;
    colCtx.ctx = ctx;        
    ctx = NULL;
    taosArrayPush(colCtxs, &colCtx);
  }

  *success = true;

  return TSDB_CODE_SUCCESS;

_err_return:
  *success = false;
  return TSDB_CODE_SUCCESS;
}



int32_t filterMergeTwoGroups(SFilterInfo *info, SFilterGroupCtx** gRes1, SFilterGroupCtx** gRes2, bool *all) {
  bool conflict = false;
  
  filterCheckColConflict(*gRes1, *gRes2, &conflict);
  if (conflict) {
    return TSDB_CODE_SUCCESS;
  }

  FILTER_SET_FLAG(info->status, FI_STATUS_REWRITE);

  uint16_t idx1 = 0, idx2 = 0, m = 0, n = 0;
  bool numEqual = (*gRes1)->colNum == (*gRes2)->colNum;
  bool success = false;
  uint16_t equal1 = 0, equal2 = 0, total = 0;
  void *ctx = NULL;  
  void *vctx = NULL;
  SArray* colCtxs = taosArrayInit((*gRes2)->colNum, sizeof(SFilterColCtx));
  int32_t dataType = 0;

  for (; m < (*gRes1)->colNum; ++m) {
    idx1 = (*gRes1)->colIdx[m];
  
    for (; n < (*gRes2)->colNum; ++n) {
      idx2 = (*gRes2)->colIdx[n];

      if (idx1 > idx2) {
        continue;
      }

      assert(idx1 == idx2);
      
      ++total;

      dataType = gRes1->colInfo[idx1].dataType;

      if (IS_VAR_DATA_TYPE(dataType)) {
        filterMergeTwoVarGroupsImpl(info, &vctx, TSDB_RELATION_OR, idx1, *gRes1, *gRes2, NULL, all);

        CHK_JMP(*all);

        filterCheckGroupMergeRes(vctx, gRes1, gRes2, idx1, colCtxs, &equal1, &equal2, total, &success);

        CHK_JMP(!success);
      } else {
        filterMergeTwoGroupsImpl(info, &ctx, TSDB_RELATION_OR, idx1, *gRes1, *gRes2, NULL, all);

        CHK_JMP(*all);

        filterCheckGroupMergeRes(ctx, gRes1, gRes2, idx1, colCtxs, &equal1, &equal2, total, &success);

        CHK_JMP(!success);
      }

      ++n;
      break;
    }
  }

  assert(total > 0);

  SFilterColInfo *colInfo = NULL;
  assert (total == equal1 || total == equal2);

  filterFreeGroupCtx(*gRes2);
  *gRes2 = NULL;

  assert(colCtxs && taosArrayGetSize(colCtxs) > 0);

  int32_t ctxSize = (int32_t)taosArrayGetSize(colCtxs);
  SFilterColCtx *pctx = NULL;
  
  for (int32_t i = 0; i < ctxSize; ++i) {
    pctx = taosArrayGet(colCtxs, i);
    colInfo = &(*gRes1)->colInfo[pctx->colIdx];
    
    filterFreeColInfo(colInfo);
    FILTER_PUSH_CTX((*gRes1)->colInfo[pctx->colIdx], pctx->ctx);
  }

  taosArrayDestroy(colCtxs);
  
  return TSDB_CODE_SUCCESS;

_err_return:

  if (colCtxs && taosArrayGetSize(colCtxs) > 0) {
    taosArrayDestroyEx(colCtxs, filterFreeColCtx);
  }

  filterFreeVarCtx(vctx);
  filterFreeRangeCtx(ctx);
  
  return TSDB_CODE_SUCCESS;
}


int32_t filterMergeGroups(SFilterInfo *info, SFilterGroupCtx** gRes, int32_t *gResNum) {
  if (*gResNum <= 1) {
    return TSDB_CODE_SUCCESS;
  }

  qsort(gRes, *gResNum, POINTER_BYTES, filterCompareGroupCtx);

  int32_t pEnd = 0, cStart = 0, cEnd = 0;
  uint16_t pColNum = 0, cColNum = 0; 
  int32_t movedNum = 0;
  bool all = false;

  cColNum = gRes[0]->colNum;

  for (int32_t i = 1; i <= *gResNum; ++i) {
    if (i < (*gResNum) && gRes[i]->colNum == cColNum) {
      continue;
    }

    cEnd = i - 1;

    movedNum = 0;
    if (pColNum > 0) {
      for (int32_t m = 0; m <= pEnd; ++m) {
        for (int32_t n = cStart; n <= cEnd; ++n) {
          assert(m < n);
          filterMergeTwoGroups(info, &gRes[m], &gRes[n], &all);

          CHK_JMP(all);

          if (gRes[n] == NULL) {
            if (n < ((*gResNum) - 1)) {
              memmove(&gRes[n], &gRes[n+1], (*gResNum-n-1) * POINTER_BYTES);
            }
            
            --cEnd;
            --(*gResNum);
            ++movedNum;
            --n;
          }
        }
      }
    }

    for (int32_t m = cStart; m < cEnd; ++m) {
      for (int32_t n = m + 1; n <= cEnd; ++n) {
        assert(m < n);
        filterMergeTwoGroups(info, &gRes[m], &gRes[n], &all);

        CHK_JMP(all);
        
        if (gRes[n] == NULL) {
          if (n < ((*gResNum) - 1)) {
            memmove(&gRes[n], &gRes[n+1], (*gResNum-n-1) * POINTER_BYTES);
          }
          
          --cEnd;
          --(*gResNum);
          ++movedNum;
          --n;
        }
      }
    }

    pColNum = cColNum;
    pEnd = cEnd;

    i -= movedNum;

    if (i >= (*gResNum)) {
      break;
    }
    
    cStart = i;
    cEnd = i;
    cColNum = gRes[i]->colNum;    
  }

  return TSDB_CODE_SUCCESS;
  
_err_return:

  FILTER_SET_FLAG(info->status, FI_STATUS_ALL);

  return TSDB_CODE_SUCCESS;
}

int32_t filterConvertGroupFromArray(SFilterInfo *info, SArray* group) {
  size_t groupSize = taosArrayGetSize(group);

  info->groupNum = (uint16_t)groupSize;

  if (info->groupNum > 0) {
    info->groups = calloc(info->groupNum, sizeof(*info->groups));
  }

  for (size_t i = 0; i < groupSize; ++i) {
    SFilterGroup *pg = taosArrayGet(group, i);
    pg->unitFlags = calloc(pg->unitNum, sizeof(*pg->unitFlags));
    info->groups[i] = *pg;
  }

  return TSDB_CODE_SUCCESS;
}

int32_t filterRewrite(SFilterInfo *info, SFilterGroupCtx** gRes, int32_t gResNum) {
  if (!FILTER_GET_FLAG(info->status, FI_STATUS_REWRITE)) {
    qDebug("no need rewrite");
    return TSDB_CODE_SUCCESS;
  }
  
  SFilterInfo oinfo = *info;
  SArray* group = taosArrayInit(FILTER_DEFAULT_GROUP_SIZE, sizeof(SFilterGroup));
  SFilterGroupCtx *res = NULL;
  SFilterColInfo *colInfo = NULL;
  int32_t optr = 0;
  uint16_t uidx = 0;

  memset(info, 0, sizeof(*info));

  FILTER_SET_FLAG(info->options, FI_OPTION_NEED_UNIQE);

  filterInitUnitsFields(info);

  for (int32_t i = 0; i < gResNum; ++i) {
    res = gRes[i];

    optr = (res->colNum > 1) ? TSDB_RELATION_AND : TSDB_RELATION_OR;

    SFilterGroup ng = {0};
    
    for (uint16_t m = 0; m < res->colNum; ++m) {
      colInfo = &res->colInfo[res->colIdx[m]];
      if (FILTER_NO_MERGE_DATA_TYPE(colInfo->dataType)) {
        assert(colInfo->type == RANGE_TYPE_UNIT);
        int32_t usize = (int32_t)taosArrayGetSize((SArray *)colInfo->info);
        
        for (int32_t n = 0; n < usize; ++n) {
          SFilterUnit* u = taosArrayGetP((SArray *)colInfo->info, n);
          
          filterAddUnitFromUnit(info, &oinfo, u, &uidx);
          filterAddUnitToGroup(&ng, uidx);
        }
        
        continue;
      }
      
      assert(colInfo->type == RANGE_TYPE_MR_CTX);
      
      filterAddGroupUnitFromCtx(info, &oinfo, colInfo->info, res->colIdx[m], &ng, optr, group);
    }

    if (ng.unitNum > 0) {
      taosArrayPush(group, &ng);
    }
  }

  filterConvertGroupFromArray(info, group);

  taosArrayDestroy(group);

  return TSDB_CODE_SUCCESS;
}


int32_t filterPreprocess(SFilterInfo *info) {
  SFilterGroupCtx** gRes = calloc(info->groupNum, sizeof(SFilterGroupCtx *));
  int32_t gResNum = 0;
  
  filterMergeGroupUnits(info, gRes, &gResNum);

  filterMergeGroups(info, gRes, &gResNum);

  if (FILTER_GET_FLAG(info->status, FI_STATUS_ALL)) {
    qInfo("Final - FilterInfo: [ALL]");
    return TSDB_CODE_SUCCESS;
  }

  if (FILTER_GET_FLAG(info->status, FI_STATUS_EMPTY)) {
    qInfo("Final - FilterInfo: [EMPTY]");
    return TSDB_CODE_SUCCESS;
  }  

  //TODO GET COLUMN RANGE

  filterRewrite(info, gRes, gResNum);
  
  return TSDB_CODE_SUCCESS;
}


int32_t filterSetColFieldData(SFilterInfo *info, int16_t colId, void *data) {
  CHK_LRET(info == NULL, TSDB_CODE_QRY_APP_ERROR, "info NULL");
  CHK_LRET(info->fields[FLD_TYPE_COLUMN].num <= 0, TSDB_CODE_QRY_APP_ERROR, "no column fileds");

  for (uint16_t i = 0; i < info->fields[FLD_TYPE_COLUMN].num; ++i) {
    SFilterField* fi = &info->fields[FLD_TYPE_COLUMN].fields[i];
    SSchema* sch = fi->desc;
    if (sch->colId == colId) {
      fi->data = data;
      break;
    }
  }

  return TSDB_CODE_SUCCESS;
}


bool filterExecute(SFilterInfo *info, int32_t numOfRows, int8_t* p) {
  bool all = true;

  if (FILTER_GET_FLAG(info->status, FI_STATUS_EMPTY)) {
    return false;
  }

  for (int32_t i = 0; i < numOfRows; ++i) {
    FILTER_UNIT_CLR_F(info);

    p[i] = 0;
  
    for (uint16_t g = 0; g < info->groupNum; ++g) {
      SFilterGroup* group = &info->groups[g];
      bool qualified = true;
      
      for (uint16_t u = 0; u < group->unitNum; ++u) {
        uint16_t uidx = group->unitIdxs[u];
        uint8_t ures = 0;
      
        if (FILTER_UNIT_GET_F(info, uidx)) {
          ures = FILTER_UNIT_GET_R(info, uidx);
        } else {
          SFilterUnit *unit = &info->units[uidx];

          if (isNull(FILTER_UNIT_COL_DATA(info, unit, i), FILTER_UNIT_DATA_TYPE(unit))) {
            ures = unit->compare.optr == TSDB_RELATION_ISNULL ? true : false;
          } else {
            if (unit->compare.optr == TSDB_RELATION_NOTNULL) {
              ures = true;
            } else if (unit->compare.optr == TSDB_RELATION_ISNULL) {
              ures = false;
            } else {
              ures = filterDoCompare(unit, FILTER_UNIT_COL_DATA(info, unit, i), FILTER_UNIT_VAL_DATA(info, unit));
            }
          }
          
          FILTER_UNIT_SET_R(info, uidx, ures);
          FILTER_UNIT_SET_F(info, uidx);
        }

        if (!ures) {
          qualified = ures;
          break;
        }
      }

      if (qualified) {
        p[i] = 1;
        break;
      }
    }

    if (p[i] != 1) {
      all = false;
    }    
  }

  return all;
}


int32_t filterInitFromTree(tExprNode* tree, SFilterInfo **pinfo, uint32_t options) {
  int32_t code = TSDB_CODE_SUCCESS;
  SFilterInfo *info = NULL;
  
  CHK_LRET(tree == NULL || pinfo == NULL, TSDB_CODE_QRY_APP_ERROR, "invalid param");

  if (*pinfo == NULL) {
    *pinfo = calloc(1, sizeof(SFilterInfo));
  }

  info = *pinfo;

  info->options = options;
  
  SArray* group = taosArrayInit(FILTER_DEFAULT_GROUP_SIZE, sizeof(SFilterGroup));

  filterInitUnitsFields(info);

  code = filterTreeToGroup(tree, info, group);

  ERR_JRET(code);

  filterConvertGroupFromArray(info, group);

  ERR_JRET(filterInitValFieldData(info));

  if (!FILTER_GET_FLAG(info->options, FI_OPTION_NO_REWRITE)) {
    filterDumpInfoToString(info, "Before preprocess");

    ERR_JRET(filterPreprocess(info));
    
    CHK_JMP(FILTER_GET_FLAG(info->status, FI_STATUS_ALL));

    if (FILTER_GET_FLAG(info->status, FI_STATUS_EMPTY)) {
      taosArrayDestroy(group);
      return code;
    }
  }
  
  ERR_JRET(filterInitUnitFunc(info));

  info->unitRes = malloc(info->unitNum * sizeof(*info->unitRes));
  info->unitFlags = malloc(info->unitNum * sizeof(*info->unitFlags));

  filterDumpInfoToString(info, "Final");

  taosArrayDestroy(group);

  return code;

_err_return:
  qInfo("No filter, code:%d", code);
  
  taosArrayDestroy(group);

  filterFreeInfo(*pinfo);

  *pinfo = NULL;

  return code;
}

int32_t filterGetTimeRange(SFilterInfo *info, STimeWindow       *win) {
  SFilterRange ra = {0};
  SFilterRangeCtx *prev = filterInitRangeCtx(TSDB_DATA_TYPE_TIMESTAMP, FI_OPTION_TIMESTAMP);
  SFilterRangeCtx *tmpc = filterInitRangeCtx(TSDB_DATA_TYPE_TIMESTAMP, FI_OPTION_TIMESTAMP);
  SFilterRangeCtx *cur = NULL;
  int32_t num = 0;
  int32_t optr = 0;
  int32_t code = TSDB_CODE_QRY_INVALID_TIME_CONDITION;
  bool empty = false, all = false;

  for (int32_t i = 0; i < info->groupNum; ++i) {
    SFilterGroup *group = &info->groups[i];
    if (group->unitNum > 1) {
      cur = tmpc;
      optr = TSDB_RELATION_AND;
    } else {
      cur = prev;
      optr = TSDB_RELATION_OR;
    }

    for (int32_t u = 0; u < group->unitNum; ++u) {
      uint16_t uidx = group->unitIdxs[u];
      SFilterUnit *unit = &info->units[uidx];

      uint8_t raOptr = FILTER_UNIT_OPTR(unit);
      
      filterAddRangeOptr(cur, raOptr, TSDB_RELATION_AND, &empty, NULL);
      CHK_JMP(empty);
      
      if (FILTER_NO_MERGE_OPTR(raOptr)) {
        continue;
      }
      
      SFilterField *right = FILTER_UNIT_RIGHT_FIELD(info, unit);
      void *s = FILTER_GET_VAL_FIELD_DATA(right);
      void *e = FILTER_GET_VAL_FIELD_DATA(right) + tDataTypes[TSDB_DATA_TYPE_TIMESTAMP].bytes;

      SIMPLE_COPY_VALUES(&ra.s, s);
      SIMPLE_COPY_VALUES(&ra.e, e);
      
      filterAddRange(cur, &ra, optr);
    }

    if (cur->notnull) {
      prev->notnull = true;
      break;
    }

    if (group->unitNum > 1) {
      filterSourceRangeFromCtx(prev, cur, TSDB_RELATION_OR, &empty, &all);
      filterResetRangeCtx(cur);
      if (all) {
        break;
      }
    }
  }

  if (prev->notnull) {
    *win = TSWINDOW_INITIALIZER;
  } else {
    filterGetRangeNum(prev, &num);
    if (num != 1) {
      qError("only one time range accepted, num:%d", num);
      ERR_JRET(TSDB_CODE_QRY_INVALID_TIME_CONDITION);
    }

    SFilterRange tra;
    filterGetRangeRes(prev, &tra);
    win->skey = tra.s; 
    win->ekey = tra.e;
  }

  filterFreeRangeCtx(prev);
  filterFreeRangeCtx(tmpc);

  qDebug("qFilter time range:[%"PRId64 "]-[%"PRId64 "]", win->skey, win->ekey);
  return TSDB_CODE_SUCCESS;

_err_return:

  *win = TSWINDOW_DESC_INITIALIZER;

  filterFreeRangeCtx(prev);
  filterFreeRangeCtx(tmpc);

  qDebug("qFilter time range:[%"PRId64 "]-[%"PRId64 "]", win->skey, win->ekey);

  return code;
}


int32_t filterConverNcharColumns(SFilterInfo* info, int32_t rows, bool *gotNchar) {
  for (uint16_t i = 0; i < info->fields[FLD_TYPE_COLUMN].num; ++i) {
    SFilterField* fi = &info->fields[FLD_TYPE_COLUMN].fields[i];
    int32_t type = FILTER_GET_COL_FIELD_TYPE(fi);
    if (type == TSDB_DATA_TYPE_NCHAR) {
      SFilterField nfi = {0};
      nfi.desc = fi->desc;
      int32_t bytes = FILTER_GET_COL_FIELD_SIZE(fi);
      nfi.data = malloc(rows * bytes);
      int32_t bufSize = bytes - VARSTR_HEADER_SIZE;
      for (int32_t j = 0; j < rows; ++j) {
        char *src = FILTER_GET_COL_FIELD_DATA(fi, j);
        char *dst = FILTER_GET_COL_FIELD_DATA(&nfi, j);
        int32_t len = 0;
        taosMbsToUcs4(varDataVal(src), varDataLen(src), varDataVal(dst), bufSize, &len);
        varDataLen(dst) = len;
      }

      fi->data = nfi.data;
      
      *gotNchar = true;
    }
  }


#if 0
  for (int32_t i = 0; i < numOfFilterCols; ++i) {
    if (pFilterInfo[i].info.type == TSDB_DATA_TYPE_NCHAR) {
      pFilterInfo[i].pData2 = pFilterInfo[i].pData;
      pFilterInfo[i].pData = malloc(rows * pFilterInfo[i].info.bytes);
      int32_t bufSize = pFilterInfo[i].info.bytes - VARSTR_HEADER_SIZE;
      for (int32_t j = 0; j < rows; ++j) {
        char* dst = (char *)pFilterInfo[i].pData + j * pFilterInfo[i].info.bytes;
        char* src = (char *)pFilterInfo[i].pData2 + j * pFilterInfo[i].info.bytes;
        int32_t len = 0;
        taosMbsToUcs4(varDataVal(src), varDataLen(src), varDataVal(dst), bufSize, &len);
        varDataLen(dst) = len;
      }
      *gotNchar = true;
    }
  }
#endif

  return TSDB_CODE_SUCCESS;
}

int32_t filterFreeNcharColumns(SFilterInfo* info) {
#if 0
  for (int32_t i = 0; i < numOfFilterCols; ++i) {
    if (pFilterInfo[i].info.type == TSDB_DATA_TYPE_NCHAR) {
      if (pFilterInfo[i].pData2) {
        tfree(pFilterInfo[i].pData);
        pFilterInfo[i].pData = pFilterInfo[i].pData2;
        pFilterInfo[i].pData2 = NULL;
      }
    }
  }
#endif

  for (uint16_t i = 0; i < info->fields[FLD_TYPE_COLUMN].num; ++i) {
    SFilterField* fi = &info->fields[FLD_TYPE_COLUMN].fields[i];
    int32_t type = FILTER_GET_COL_FIELD_TYPE(fi);
    if (type == TSDB_DATA_TYPE_NCHAR) {
      tfree(fi->data);
    }
  }

  return TSDB_CODE_SUCCESS;
}





