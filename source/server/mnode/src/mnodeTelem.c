/*
 * Copyright (c) 2020 TAOS Data, Inc. <jhtao@taosdata.com>
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

#define _DEFAULT_SOURCE
#include "mnodeTelem.h"
#include "tbuffer.h"
#include "tglobal.h"

#define TELEMETRY_SERVER "telemetry.taosdata.com"
#define TELEMETRY_PORT 80
#define REPORT_INTERVAL 86400

/*
 * sem_timedwait is NOT implemented on MacOSX
 * thus we use pthread_mutex_t/pthread_cond_t to simulate
 */
static struct {
  bool             enable;
  pthread_mutex_t  lock;
  pthread_cond_t   cond;
  volatile int32_t exit;
  pthread_t        thread;
  char             email[TSDB_FQDN_LEN];
} tsTelem;

static void mnodeBeginObject(SBufferWriter* bw) { tbufWriteChar(bw, '{'); }

static void mnodeCloseObject(SBufferWriter* bw) {
  size_t len = tbufTell(bw);
  if (tbufGetData(bw, false)[len - 1] == ',') {
    tbufWriteCharAt(bw, len - 1, '}');
  } else {
    tbufWriteChar(bw, '}');
  }
  tbufWriteChar(bw, ',');
}

#if 0
static void beginArray(SBufferWriter* bw) {
  tbufWriteChar(bw, '[');
}

static void closeArray(SBufferWriter* bw) {
  size_t len = tbufTell(bw);
  if (tbufGetData(bw, false)[len - 1] == ',') {
    tbufWriteCharAt(bw, len - 1, ']');
  } else {
    tbufWriteChar(bw, ']');
  }
  tbufWriteChar(bw, ',');
}
#endif

static void mnodeWriteString(SBufferWriter* bw, const char* str) {
  tbufWriteChar(bw, '"');
  tbufWrite(bw, str, strlen(str));
  tbufWriteChar(bw, '"');
}

static void mnodeAddIntField(SBufferWriter* bw, const char* k, int64_t v) {
  mnodeWriteString(bw, k);
  tbufWriteChar(bw, ':');
  char buf[32];
  sprintf(buf, "%" PRId64, v);
  tbufWrite(bw, buf, strlen(buf));
  tbufWriteChar(bw, ',');
}

static void mnodeAddStringField(SBufferWriter* bw, const char* k, const char* v) {
  mnodeWriteString(bw, k);
  tbufWriteChar(bw, ':');
  mnodeWriteString(bw, v);
  tbufWriteChar(bw, ',');
}

static void mnodeAddCpuInfo(SBufferWriter* bw) {
  char*   line = NULL;
  size_t  size = 0;
  int32_t done = 0;

  FILE* fp = fopen("/proc/cpuinfo", "r");
  if (fp == NULL) {
    return;
  }

  while (done != 3 && (size = tgetline(&line, &size, fp)) != -1) {
    line[size - 1] = '\0';
    if (((done & 1) == 0) && strncmp(line, "model name", 10) == 0) {
      const char* v = strchr(line, ':') + 2;
      mnodeAddStringField(bw, "cpuModel", v);
      done |= 1;
    } else if (((done & 2) == 0) && strncmp(line, "cpu cores", 9) == 0) {
      const char* v = strchr(line, ':') + 2;
      mnodeWriteString(bw, "numOfCpu");
      tbufWriteChar(bw, ':');
      tbufWrite(bw, v, strlen(v));
      tbufWriteChar(bw, ',');
      done |= 2;
    }
  }

  free(line);
  fclose(fp);
}

static void mnodeAddOsInfo(SBufferWriter* bw) {
  char*  line = NULL;
  size_t size = 0;

  FILE* fp = fopen("/etc/os-release", "r");
  if (fp == NULL) {
    return;
  }

  while ((size = tgetline(&line, &size, fp)) != -1) {
    line[size - 1] = '\0';
    if (strncmp(line, "PRETTY_NAME", 11) == 0) {
      const char* p = strchr(line, '=') + 1;
      if (*p == '"') {
        p++;
        line[size - 2] = 0;
      }
      mnodeAddStringField(bw, "os", p);
      break;
    }
  }

  free(line);
  fclose(fp);
}

static void mnodeAddMemoryInfo(SBufferWriter* bw) {
  char*  line = NULL;
  size_t size = 0;

  FILE* fp = fopen("/proc/meminfo", "r");
  if (fp == NULL) {
    return;
  }

  while ((size = tgetline(&line, &size, fp)) != -1) {
    line[size - 1] = '\0';
    if (strncmp(line, "MemTotal", 8) == 0) {
      const char* p = strchr(line, ':') + 1;
      while (*p == ' ') p++;
      mnodeAddStringField(bw, "memory", p);
      break;
    }
  }

  free(line);
  fclose(fp);
}

static void mnodeAddVersionInfo(SBufferWriter* bw) {
  mnodeAddStringField(bw, "version", version);
  mnodeAddStringField(bw, "buildInfo", buildinfo);
  mnodeAddStringField(bw, "gitInfo", gitinfo);
  mnodeAddStringField(bw, "email", tsTelem.email);
}

static void mnodeAddRuntimeInfo(SBufferWriter* bw) {
  SMnodeStat stat = {0};
  if (mnodeGetStatistics(&stat) != 0) {
    return;
  }

  mnodeAddIntField(bw, "numOfDnode", stat.numOfDnode);
  mnodeAddIntField(bw, "numOfMnode", stat.numOfMnode);
  mnodeAddIntField(bw, "numOfVgroup", stat.numOfVgroup);
  mnodeAddIntField(bw, "numOfDatabase", stat.numOfDatabase);
  mnodeAddIntField(bw, "numOfSuperTable", stat.numOfSuperTable);
  mnodeAddIntField(bw, "numOfChildTable", stat.numOfChildTable);
  mnodeAddIntField(bw, "numOfColumn", stat.numOfColumn);
  mnodeAddIntField(bw, "numOfPoint", stat.totalPoints);
  mnodeAddIntField(bw, "totalStorage", stat.totalStorage);
  mnodeAddIntField(bw, "compStorage", stat.compStorage);
}

static void mnodeSendTelemetryReport() {
  char     buf[128] = {0};
  uint32_t ip = taosGetIpv4FromFqdn(TELEMETRY_SERVER);
  if (ip == 0xffffffff) {
    mTrace("failed to get IP address of " TELEMETRY_SERVER ", reason:%s", strerror(errno));
    return;
  }
  SOCKET fd = taosOpenTcpClientSocket(ip, TELEMETRY_PORT, 0);
  if (fd < 0) {
    mTrace("failed to create socket for telemetry, reason:%s", strerror(errno));
    return;
  }

  char clusterId[TSDB_CLUSTER_ID_LEN] = {0};
  mnodeGetClusterId(clusterId);

  SBufferWriter bw = tbufInitWriter(NULL, false);
  mnodeBeginObject(&bw);
  mnodeAddStringField(&bw, "instanceId", clusterId);
  mnodeAddIntField(&bw, "reportVersion", 1);
  mnodeAddOsInfo(&bw);
  mnodeAddCpuInfo(&bw);
  mnodeAddMemoryInfo(&bw);
  mnodeAddVersionInfo(&bw);
  mnodeAddRuntimeInfo(&bw);
  mnodeCloseObject(&bw);

  const char* header =
      "POST /report HTTP/1.1\n"
      "Host: " TELEMETRY_SERVER
      "\n"
      "Content-Type: application/json\n"
      "Content-Length: ";

  taosWriteSocket(fd, (void*)header, (int32_t)strlen(header));
  int32_t contLen = (int32_t)(tbufTell(&bw) - 1);
  sprintf(buf, "%d\n\n", contLen);
  taosWriteSocket(fd, buf, (int32_t)strlen(buf));
  taosWriteSocket(fd, tbufGetData(&bw, false), contLen);
  tbufCloseWriter(&bw);

  // read something to avoid nginx error 499
  if (taosReadSocket(fd, buf, 10) < 0) {
    mTrace("failed to receive response since %s", strerror(errno));
  }

  taosCloseSocket(fd);
}

static void* mnodeTelemThreadFp(void* param) {
  struct timespec end = {0};
  clock_gettime(CLOCK_REALTIME, &end);
  end.tv_sec += 300;  // wait 5 minutes before send first report

  setThreadName("mnode-telem");

  while (!tsTelem.exit) {
    int32_t         r = 0;
    struct timespec ts = end;
    pthread_mutex_lock(&tsTelem.lock);
    r = pthread_cond_timedwait(&tsTelem.cond, &tsTelem.lock, &ts);
    pthread_mutex_unlock(&tsTelem.lock);
    if (r == 0) break;
    if (r != ETIMEDOUT) continue;

    if (mnodeGetStatus() == MN_STATUS_READY) {
      mnodeSendTelemetryReport();
    }
    end.tv_sec += REPORT_INTERVAL;
  }

  return NULL;
}

static void mnodeGetEmail(char* filepath) {
  int32_t fd = taosOpenFileRead(filepath);
  if (fd < 0) {
    return;
  }

  if (taosReadFile(fd, (void*)tsTelem.email, TSDB_FQDN_LEN) < 0) {
    mError("failed to read %d bytes from file %s since %s", TSDB_FQDN_LEN, filepath, strerror(errno));
  }

  taosCloseFile(fd);
}

int32_t mnodeInitTelem() {
  tsTelem.enable = tsEnableTelemetryReporting;
  if (!tsTelem.enable) return 0;

  tsTelem.exit = 0;
  pthread_mutex_init(&tsTelem.lock, NULL);
  pthread_cond_init(&tsTelem.cond, NULL);
  tsTelem.email[0] = 0;

  mnodeGetEmail("/usr/local/taos/email");

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

  int32_t code = pthread_create(&tsTelem.thread, &attr, mnodeTelemThreadFp, NULL);
  pthread_attr_destroy(&attr);
  if (code != 0) {
    mTrace("failed to create telemetry thread since :%s", strerror(code));
  }

  mInfo("mnode telemetry is initialized");
  return 0;
}

void mnodeCleanupTelem() {
  if (!tsTelem.enable) return;

  if (taosCheckPthreadValid(tsTelem.thread)) {
    pthread_mutex_lock(&tsTelem.lock);
    tsTelem.exit = 1;
    pthread_cond_signal(&tsTelem.cond);
    pthread_mutex_unlock(&tsTelem.lock);

    pthread_join(tsTelem.thread, NULL);
  }

  pthread_mutex_destroy(&tsTelem.lock);
  pthread_cond_destroy(&tsTelem.cond);
}
