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

#ifndef _TD_AMALLOC_H_
#define _TD_AMALLOC_H_

#include "os.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AMALLOC_APIS                                  \
  void *(*malloc)(void *, size_t size);               \
  void *(*calloc)(void *, size_t nmemb, size_t size); \
  void *(*realloc)(void *, size_t size);              \
  void (*free)(void *ptr);

// Interfaces to implement
typedef struct {
  AMALLOC_APIS
} SMemAllocatorIf;

typedef struct {
  void *impl;
  AMALLOC_APIS
} SMemAllocator;

#define amalloc(allocator, size) ((allocator) ? (*((allocator)->malloc))((allocator)->impl, (size)) : malloc(size))
#define acalloc(allocator, nmemb, size) \
  ((allocator) ? (*((allocator)->calloc))((allocator)->impl, (nmemb), (size)) : calloc((nmemb), (size)))
#define arealloc(allocator, ptr, size) \
  ((allocator) ? (*((allocator)->realloc))((allocator)->impl, (ptr), (size)) : realloc((ptr), (size)))
#define afree(allocator, ptr, size) ((allocator) ? (*((allocator)->free))((allocator)->impl, (ptr), (size)) : free(ptr))

#ifdef __cplusplus
}
#endif

#endif /*_TD_AMALLOC_H_*/