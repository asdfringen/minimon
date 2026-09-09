#pragma once
#include <optional>
#include <string>
#include <vector>

struct GpuInfo {
  std::string name = "Unknown GPU";
  double util3D = 0.0;
  double utilCompute = 0.0;
  double utilCopy = 0.0;
  double utilCopy1 = 0.0;
  bool engineValid = false;
  unsigned long long dedicatedUsed = 0;
  unsigned long long dedicatedLimit = 0;
  unsigned long long sharedUsed = 0;
  unsigned long long sharedLimit = 0;
  bool memValid = false;
  std::optional<double> tempC;  // empty = N/A
  std::string tempSource;       // "ADL" or ""
};

enum class EngineSlot { Ignore, D3D, Compute, Copy0, Copy1 };

// Pure helpers (unit-testable, no PDH needed).
struct ParsedEngine {
  EngineSlot slot = EngineSlot::Ignore;  // Copy0 = copy; Copy1 assigned later
  int engOrdinal = -1;                   // N from "..._eng_<N>_..." or -1
};
// Classify a lowercased PDH instance name, e.g.
// "pid_5508_luid_0x00000000_0x0000f70d_phys_0_eng_2_engtype_compute 0".
ParsedEngine parseEngineInstance(const std::string& lower);
// Copy slot from engine ordinal: 1st ordinal -> Copy0, rest -> Copy1.
EngineSlot copySlotForOrdinal(const std::vector<int>& sortedDistinctOrds, int engOrdinal);
// "luid_0x........_0x........" match string from a DXGI adapter LUID.
std::string formatLuidMatch(unsigned long lowPart, long highPart);

class GpuSampler {
 public:
  GpuSampler();
  ~GpuSampler();
  bool init(std::string* err);
  GpuInfo sample();

 private:
  bool initEngineCounters(std::string* err);
  bool initMemCounters(std::string* err);
  void queryAdapterName();
  void queryTemp();
  void syncEngines();      // incremental re-enumeration of GPU Engine counters
  void assignCopySlots();  // Copy0 = 1st copy ordinal, Copy1 = rest

  void* query_ = nullptr;
  struct EngineCounter {
    void* counter = nullptr;
    std::wstring instance;
    std::string lower;  // lowercase instance for engtype parse
    EngineSlot slot = EngineSlot::Ignore;
    int engOrdinal = -1;
  };
  std::vector<EngineCounter> engines_;

  struct MemCounters {
    void* usage = nullptr;
    bool isDedicated = true;
  };
  std::vector<MemCounters> memCounters_;
  bool memReady_ = false;

  // Lowercase "luid_0x........_0x........" of the DXGI best adapter.
  // Empty = filtering unavailable (fall back to all instances).
  std::string adapterLuidMatch_;

  GpuInfo last_;
  bool adapterNameResolved_ = false;
};
