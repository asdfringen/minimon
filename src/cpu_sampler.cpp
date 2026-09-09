#include "sys_sampler.h"
#include <windows.h>
#include <pdh.h>
#ifndef PDH_CSTATUS_VALID_DATA
#define PDH_CSTATUS_VALID_DATA 0x00000000
#define PDH_CSTATUS_NEW_DATA 0x00000001
#endif
#include <psapi.h>

CpuSampler::CpuSampler() {}
CpuSampler::~CpuSampler() {
  if (query_) PdhCloseQuery((PDH_HQUERY)query_);
}

bool CpuSampler::init(std::string* err) {
  PDH_STATUS st = PdhOpenQueryW(nullptr, 0, (PDH_HQUERY*)&query_);
  if (st != ERROR_SUCCESS) {
    if (err) *err = "PdhOpenQuery failed";
    return false;
  }
  // English counter name: locale independent
  st = PdhAddEnglishCounterW((PDH_HQUERY)query_,
                             L"\\Processor(_Total)\\% Processor Time", 0,
                             (PDH_HCOUNTER*)&counter_);
  if (st != ERROR_SUCCESS) {
    if (err) *err = "PdhAddEnglishCounter(Processor) failed";
    return false;
  }
  PdhCollectQueryData((PDH_HQUERY)query_);
  ready_ = true;
  return true;
}

CpuSample CpuSampler::sample() {
  CpuSample s;
  if (!ready_) return s;
  PDH_STATUS st = PdhCollectQueryData((PDH_HQUERY)query_);
  if (st != ERROR_SUCCESS) return s;
  PDH_FMT_COUNTERVALUE v{};
  st = PdhGetFormattedCounterValue((PDH_HCOUNTER)counter_, PDH_FMT_DOUBLE, nullptr, &v);
  if (st != ERROR_SUCCESS) return s;  // first sample: PDH_MORE_DATA
  if (v.CStatus != ERROR_SUCCESS && v.CStatus != PDH_CSTATUS_VALID_DATA &&
      v.CStatus != PDH_CSTATUS_NEW_DATA)
    return s;
  double pct = v.doubleValue;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  s.totalPct = pct;
  s.valid = true;
  return s;
}

MemInfo queryMemory() {
  MemInfo m;
  MEMORYSTATUSEX st{};
  st.dwLength = sizeof(st);
  if (GlobalMemoryStatusEx(&st)) {
    m.totalBytes = st.ullTotalPhys;
    m.availBytes = st.ullAvailPhys;
    m.usedBytes = m.totalBytes > m.availBytes ? m.totalBytes - m.availBytes : 0;
    if (m.totalBytes)
      m.usedPct = 100.0 * (double)m.usedBytes / (double)m.totalBytes;
  }
  PERFORMANCE_INFORMATION pi{};
  pi.cb = sizeof(pi);
  if (GetPerformanceInfo(&pi, sizeof(pi))) {
    m.cacheBytes = (unsigned long long)pi.SystemCache * (unsigned long long)pi.PageSize;
  }
  return m;
}
