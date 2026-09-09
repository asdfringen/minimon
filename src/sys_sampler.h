#pragma once
#include <string>

// ---- CPU ----
struct CpuSample {
  double totalPct = 0.0;  // 0-100
  bool valid = false;
};

class CpuSampler {
 public:
  CpuSampler();
  ~CpuSampler();
  bool init(std::string* err);
  CpuSample sample();  // call every refresh; first 1-2 calls may be invalid
 private:
  void* query_ = nullptr;    // PDH_HQUERY
  void* counter_ = nullptr;  // PDH_HCOUNTER
  bool ready_ = false;
};

// ---- Memory ----
struct MemInfo {
  unsigned long long totalBytes = 0;
  unsigned long long availBytes = 0;
  unsigned long long usedBytes = 0;
  double usedPct = 0.0;
  unsigned long long cacheBytes = 0;  // SystemCache (best effort)
};

MemInfo queryMemory();
