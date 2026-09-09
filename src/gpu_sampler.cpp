#include "gpu_sampler.h"
#include <windows.h>
#include <dxgi.h>
#include <pdh.h>
#ifndef PDH_CSTATUS_VALID_DATA
#define PDH_CSTATUS_VALID_DATA 0x00000000
#define PDH_CSTATUS_NEW_DATA 0x00000001
#endif
#ifndef PERF_DETAIL_WIZARD
#define PERF_DETAIL_WIZARD 400
#endif
#ifndef PERF_DETAIL_NOVICE
#define PERF_DETAIL_NOVICE 100
#endif
#include <algorithm>
#include <cctype>
#include <cwchar>

static std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(n - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  return s;
}
static std::string toLowerStr(const std::string& s) {
  std::string o = s;
  std::transform(o.begin(), o.end(), o.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return o;
}

// ---------- Pure engine-instance helpers (no PDH; unit-testable) ----------
ParsedEngine parseEngineInstance(const std::string& lower) {
  ParsedEngine r;
  auto p = lower.find("_eng_");
  if (p != std::string::npos) r.engOrdinal = atoi(lower.c_str() + p + 5);
  if (lower.find("engtype_3d") != std::string::npos)
    r.slot = EngineSlot::D3D;
  else if (lower.find("engtype_compute") != std::string::npos)
    r.slot = EngineSlot::Compute;
  else if (lower.find("engtype_copy") != std::string::npos)
    r.slot = EngineSlot::Copy0;  // refined to Copy0/Copy1 in assignCopySlots
  return r;
}

EngineSlot copySlotForOrdinal(const std::vector<int>& sortedDistinctOrds, int engOrdinal) {
  for (size_t i = 0; i < sortedDistinctOrds.size(); ++i) {
    if (sortedDistinctOrds[i] == engOrdinal) return (i == 0) ? EngineSlot::Copy0 : EngineSlot::Copy1;
  }
  return EngineSlot::Copy0;  // unknown ordinal -> first group
}

std::string formatLuidMatch(unsigned long lowPart, long highPart) {
  // Driver instance format is luid_<High>_<Low>, e.g. luid_0x00000000_0x0000f70d.
  char buf[32];
  snprintf(buf, sizeof(buf), "luid_0x%08lx_0x%08lx", (unsigned long)highPart, lowPart);
  return buf;
}

// ---------- ADL (AMD) dynamic load for temperature ----------
namespace adl {
typedef void* ADL_CONTEXT_HANDLE;
typedef int (*ADL_MAIN_CONTROL_CREATE)(void* (*)(int), int);
typedef int (*ADL_MAIN_CONTROL_DESTROY)();
typedef int (*ADL_ADAPTER_NUMBEROFADAPTERS_GET)(int*);
typedef struct AdapterInfo {
  int iSize;
  int iAdapterIndex;
  char strUDID[256];
  int iBusNumber;
  int iDeviceNumber;
  int iFunctionNumber;
  int iVendorID;
  char strAdapterName[256];
  char strDisplayName[256];
  int iPresent;
  int iExist;
  char strDriverPath[256];
  char strDriverPathExt[256];
  char strPNPString[256];
  int iOSDisplayIndex;
} AdapterInfo, *LPAdapterInfo;
typedef int (*ADL_ADAPTER_ADAPTERINFO_GET)(LPAdapterInfo, int);
typedef struct ADLTemperature {
  int iSize;
  int iTemperature;  // millidegrees C
} ADLTemperature;
typedef int (*ADL_OVERDRIVE5_TEMPERATURE_GET)(int, int, ADLTemperature*);
typedef int (*ADL2_MAIN_CONTROL_CREATE_FN)(void* (*)(int), int, ADL_CONTEXT_HANDLE*);
typedef int (*ADL2_MAIN_CONTROL_DESTROY_FN)(ADL_CONTEXT_HANDLE);
typedef int (*ADL2_OVERDRIVEN_TEMPERATURE_GET)(ADL_CONTEXT_HANDLE, int, int*);

static void* __stdcall allocCb(int size) { return malloc(size); }

struct Loader {
  HMODULE mod = nullptr;
  ADL_MAIN_CONTROL_CREATE create = nullptr;
  ADL_ADAPTER_NUMBEROFADAPTERS_GET numAdapters = nullptr;
  ADL_ADAPTER_ADAPTERINFO_GET adapterInfo = nullptr;
  ADL_OVERDRIVE5_TEMPERATURE_GET od5temp = nullptr;
  ADL2_MAIN_CONTROL_CREATE_FN adl2create = nullptr;
  ADL2_MAIN_CONTROL_DESTROY_FN adl2destroy = nullptr;
  ADL2_OVERDRIVEN_TEMPERATURE_GET odNtemp = nullptr;
  ADL_CONTEXT_HANDLE ctx = nullptr;
  bool inited = false;
  bool adl2inited = false;

  ~Loader() { unload(); }
  void unload() {
    if (mod) {
      if (adl2inited && ctx && adl2destroy) adl2destroy(ctx);
      if (inited) {
        auto dest = (ADL_MAIN_CONTROL_DESTROY)GetProcAddress(mod, "ADL_Main_Control_Destroy");
        if (dest) dest();
      }
      FreeLibrary(mod);
      mod = nullptr;
    }
    inited = false;
    adl2inited = false;
    ctx = nullptr;
  }
  bool load() {
    const wchar_t* dlls[] = {L"atiadlxx.dll", L"atiadlxy.dll"};
    for (auto d : dlls) {
      mod = LoadLibraryW(d);
      if (mod) break;
    }
    if (!mod) return false;
    create = (ADL_MAIN_CONTROL_CREATE)GetProcAddress(mod, "ADL_Main_Control_Create");
    numAdapters = (ADL_ADAPTER_NUMBEROFADAPTERS_GET)GetProcAddress(
        mod, "ADL_Adapter_NumberOfAdapters_Get");
    adapterInfo =
        (ADL_ADAPTER_ADAPTERINFO_GET)GetProcAddress(mod, "ADL_Adapter_AdapterInfo_Get");
    od5temp = (ADL_OVERDRIVE5_TEMPERATURE_GET)GetProcAddress(
        mod, "ADL_Overdrive5_Temperature_Get");
    adl2create =
        (ADL2_MAIN_CONTROL_CREATE_FN)GetProcAddress(mod, "ADL2_Main_Control_Create");
    adl2destroy =
        (ADL2_MAIN_CONTROL_DESTROY_FN)GetProcAddress(mod, "ADL2_Main_Control_Destroy");
    odNtemp = (ADL2_OVERDRIVEN_TEMPERATURE_GET)GetProcAddress(
        mod, "ADL2_OverdriveN_Temperature_Get");
    if (create && numAdapters && adapterInfo) {
      if (create((void* (*)(int))allocCb, 1) == 0) inited = true;
    }
    if (adl2create) {
      ADL_CONTEXT_HANDLE c = nullptr;
      if (adl2create((void* (*)(int))allocCb, 1, &c) == 0 && c) {
        ctx = c;
        adl2inited = true;
      }
    }
    return inited || adl2inited;
  }
  std::optional<double> queryTempC() {
    if (!inited && !adl2inited) return std::nullopt;
    int n = 0;
    if (!numAdapters || numAdapters(&n) != 0 || n <= 0) return std::nullopt;
    std::vector<AdapterInfo> infos(n);
    for (auto& a : infos) a.iSize = sizeof(AdapterInfo);
    if (!adapterInfo ||
        adapterInfo(infos.data(), (int)(infos.size() * sizeof(AdapterInfo))) != 0)
      return std::nullopt;
    for (auto& a : infos) {
      if (!a.iPresent) continue;
      // Try OverdriveN (RDNA/RDNA4) first
      if (adl2inited && odNtemp && ctx) {
        int t = 0;
        if (odNtemp(ctx, a.iAdapterIndex, &t) == 0) {
          double c = t / 1000.0;
          // ADL-N sometimes returns decidegrees; normalize
          if (c > 150 && c < 1500) c /= 10.0;
          if (c > -50 && c < 150) return c;
        }
      }
      if (inited && od5temp) {
        ADLTemperature t{};
        t.iSize = sizeof(t);
        if (od5temp(a.iAdapterIndex, 0, &t) == 0) {
          double c = t.iTemperature / 1000.0;
          if (c > -50 && c < 150) return c;
        }
      }
    }
    return std::nullopt;
  }
};
}  // namespace adl

// ---------- ADL PMLog temperature (RDNA2+, incl. RDNA4; dynamic load) ----------
// Same DLL (atiadlxx.dll), no static dependency. Flow per AMD's PMLog sample:
// Device_Create -> Support_Get -> Start(edge/hotspot/mem sensors, 1000ms) ->
// read driver-shared ADLPMLogData via pLoggingAddress -> Stop on exit.
namespace pm {
enum : unsigned short {
  SENSOR_MAXTYPES = 0,
  TEMP_EDGE = 8,
  TEMP_MEM = 9,
  TEMP_HOTSPOT = 27,
  TEMP_GFX = 28,
  MAX_SENSORS = 256
};

struct SupportInfo {
  unsigned short usSensors[MAX_SENSORS];
  int reserved[16];
};
struct StartInput {
  unsigned short usSensors[MAX_SENSORS];
  unsigned long sampleRate;  // ms
  int reserved[15];
};
struct LogData {
  unsigned int version;
  unsigned int activeRate;
  unsigned long long lastUpdated;
  unsigned int values[MAX_SENSORS][2];  // [i][0]=sensor id, [i][1]=value (deg C)
  unsigned int reserved[256];
};
struct StartOutput {
  union {
    void* loggingAddress;
    unsigned long long ptrAddr;
  };
  int reserved[14];
};

typedef int (*FN2_CREATE)(void* (*)(int), int, void**);
typedef int (*FN2_DESTROY)(void*);
typedef int (*FN_NUM)(int*);
typedef int (*FN_INFO)(adl::AdapterInfo*, int);
typedef int (*FN_DEV_CREATE)(void*, int, void**);
typedef int (*FN_DEV_DESTROY)(void*, void*);
typedef int (*FN_SUP)(void*, int, SupportInfo*);
typedef int (*FN_START)(void*, int, StartInput*, StartOutput*, void*);
typedef int (*FN_STOP)(void*, int, void*);

struct Session {
  int adapterIndex = -1;
  void* device = nullptr;
  volatile LogData* log = nullptr;
};

struct Manager {
  HMODULE mod = nullptr;
  void* ctx = nullptr;
  FN2_DESTROY destroy2 = nullptr;
  FN_DEV_DESTROY devDestroy = nullptr;
  FN_STOP stop = nullptr;
  std::vector<Session> sessions;
  bool started = false;

  ~Manager() { shutdown(); }

  void shutdown() {
    for (auto& s : sessions) {
      if (stop && ctx) stop(ctx, s.adapterIndex, s.device);
      if (devDestroy && ctx) devDestroy(ctx, s.device);
    }
    sessions.clear();
    if (destroy2 && ctx) destroy2(ctx);
    ctx = nullptr;
    if (mod) {
      FreeLibrary(mod);
      mod = nullptr;
    }
    started = false;
  }

  // Idempotent; true when at least one adapter is logging.
  bool start() {
    if (started) return !sessions.empty();
    started = true;
    mod = LoadLibraryW(L"atiadlxx.dll");
    if (!mod) mod = LoadLibraryW(L"atiadlxy.dll");
    if (!mod) return false;
    auto create2 = (FN2_CREATE)GetProcAddress(mod, "ADL2_Main_Control_Create");
    destroy2 = (FN2_DESTROY)GetProcAddress(mod, "ADL2_Main_Control_Destroy");
    auto numAdapters = (FN_NUM)GetProcAddress(mod, "ADL_Adapter_NumberOfAdapters_Get");
    auto adapterInfo = (FN_INFO)GetProcAddress(mod, "ADL_Adapter_AdapterInfo_Get");
    auto devCreate = (FN_DEV_CREATE)GetProcAddress(mod, "ADL2_Device_PMLog_Device_Create");
    devDestroy = (FN_DEV_DESTROY)GetProcAddress(mod, "ADL2_Device_PMLog_Device_Destroy");
    auto supGet = (FN_SUP)GetProcAddress(mod, "ADL2_Adapter_PMLog_Support_Get");
    auto startFn = (FN_START)GetProcAddress(mod, "ADL2_Adapter_PMLog_Start");
    stop = (FN_STOP)GetProcAddress(mod, "ADL2_Adapter_PMLog_Stop");
    if (!create2 || !numAdapters || !adapterInfo || !devCreate || !devDestroy || !supGet ||
        !startFn || !stop) {
      shutdown();
      return false;
    }
    if (create2((void* (*)(int))adl::allocCb, 1, &ctx) != 0 || !ctx) {
      shutdown();
      return false;
    }
    int n = 0;
    if (numAdapters(&n) != 0 || n <= 0) {
      shutdown();
      return false;
    }
    std::vector<adl::AdapterInfo> infos(n);
    for (auto& a : infos) a.iSize = sizeof(adl::AdapterInfo);
    if (adapterInfo(infos.data(), (int)(infos.size() * sizeof(adl::AdapterInfo))) != 0) {
      shutdown();
      return false;
    }
    const unsigned short want[] = {TEMP_EDGE, TEMP_HOTSPOT, TEMP_MEM, TEMP_GFX};
    for (auto& a : infos) {
      if (!a.iPresent) continue;
      void* dev = nullptr;
      if (devCreate(ctx, a.iAdapterIndex, &dev) != 0 || !dev) continue;
      bool ok = false;
      SupportInfo sup{};
      if (supGet(ctx, a.iAdapterIndex, &sup) == 0) {
        StartInput in{};
        int k = 0;
        for (unsigned short w : want) {
          for (int j = 0; j < MAX_SENSORS && sup.usSensors[j] != SENSOR_MAXTYPES; ++j) {
            if (sup.usSensors[j] == w) {
              in.usSensors[k++] = w;
              break;
            }
          }
        }
        if (k > 0) {
          in.usSensors[k] = SENSOR_MAXTYPES;
          in.sampleRate = 1000;
          StartOutput out{};
          if (startFn(ctx, a.iAdapterIndex, &in, &out, dev) == 0 && out.loggingAddress) {
            Session s;
            s.adapterIndex = a.iAdapterIndex;
            s.device = dev;
            s.log = (volatile LogData*)out.loggingAddress;
            sessions.push_back(s);
            ok = true;
          }
        }
      }
      if (!ok && dev) devDestroy(ctx, dev);
    }
    if (sessions.empty()) {
      shutdown();
      return false;
    }
    return true;
  }

  std::optional<double> readTempC() {
    if (!start()) return std::nullopt;
    const unsigned short order[] = {TEMP_EDGE, TEMP_HOTSPOT, TEMP_MEM, TEMP_GFX};
    for (unsigned short id : order) {
      for (auto& s : sessions) {
        volatile LogData* log = s.log;
        if (!log) continue;
        for (int i = 0; i < MAX_SENSORS; ++i) {
          unsigned int sid = log->values[i][0];
          if (sid == SENSOR_MAXTYPES) break;
          if (sid == id) {
            double c = (double)(int)log->values[i][1];
            if (c > -50.0 && c < 150.0) return c;
            break;  // present but out of range: try next session/sensor
          }
        }
      }
    }
    return std::nullopt;
  }
};
}  // namespace pm

GpuSampler::GpuSampler() {}
GpuSampler::~GpuSampler() {
  if (query_) PdhCloseQuery((PDH_HQUERY)query_);
}

bool GpuSampler::init(std::string* err) {
  PDH_STATUS st = PdhOpenQueryW(nullptr, 0, (PDH_HQUERY*)&query_);
  if (st != ERROR_SUCCESS) {
    if (err) *err = "GPU PdhOpenQuery failed";
    return false;
  }
  std::string e1, e2;
  queryAdapterName();  // first: provides the LUID filter + DXGI memory limits
  initEngineCounters(&e1);  // best effort (may be empty on some systems)
  initMemCounters(&e2);
  return true;
}

static std::vector<std::wstring> enumInstances(const wchar_t* object) {
  DWORD csz = 0, isz = 0;
  PdhEnumObjectItemsW(nullptr, nullptr, object, nullptr, &csz, nullptr, &isz,
                      PERF_DETAIL_NOVICE, 0);
  if (isz == 0) return {};
  std::vector<wchar_t> ibuf(isz);
  std::vector<wchar_t> cbuf(csz ? csz : 1);
  if (PdhEnumObjectItemsW(nullptr, nullptr, object, cbuf.data(), &csz, ibuf.data(), &isz,
                          PERF_DETAIL_NOVICE, 0) != ERROR_SUCCESS)
    return {};
  std::vector<std::wstring> out;
  const wchar_t* p = ibuf.data();
  while (*p) {
    out.emplace_back(p);
    p += wcslen(p) + 1;
  }
  return out;
}

bool GpuSampler::initEngineCounters(std::string*) {
  syncEngines();
  PdhCollectQueryData((PDH_HQUERY)query_);
  return !engines_.empty();
}

// Incremental sync: GPU Engine instances are per-process (pid_...), so
// workloads started after launch would otherwise stay invisible forever.
void GpuSampler::syncEngines() {
  if (!query_) return;
  auto insts = enumInstances(L"GPU Engine");
  // Use the LUID filter only if it actually matches something (driver
  // formatting may vary); otherwise fall back to all instances.
  bool useFilter = false;
  if (!adapterLuidMatch_.empty()) {
    for (auto& inst : insts) {
      if (toLowerStr(narrow(inst)).find(adapterLuidMatch_) != std::string::npos) {
        useFilter = true;
        break;
      }
    }
  }
  auto wanted = [&](const std::string& lower) {
    return !useFilter || lower.find(adapterLuidMatch_) != std::string::npos;
  };
  // Add new instances.
  for (auto& inst : insts) {
    std::string lower = toLowerStr(narrow(inst));
    if (!wanted(lower)) continue;
    bool known = false;
    for (auto& e : engines_) {
      if (e.instance == inst) {
        known = true;
        break;
      }
    }
    if (known) continue;
    std::wstring path = L"\\GPU Engine(" + inst + L")\\Utilization Percentage";
    PDH_HCOUNTER c = nullptr;
    if (PdhAddEnglishCounterW((PDH_HQUERY)query_, path.c_str(), 0, &c) == ERROR_SUCCESS) {
      EngineCounter e;
      e.counter = c;
      e.instance = inst;
      e.lower = lower;
      ParsedEngine pe = parseEngineInstance(lower);
      e.slot = pe.slot;
      e.engOrdinal = pe.engOrdinal;
      engines_.push_back(std::move(e));
    }
  }
  // Remove vanished (or newly filtered-out) instances.
  for (size_t i = 0; i < engines_.size();) {
    bool alive = false;
    if (wanted(engines_[i].lower)) {
      for (auto& inst : insts) {
        if (engines_[i].instance == inst) {
          alive = true;
          break;
        }
      }
    }
    if (!alive) {
      PdhRemoveCounter((PDH_HCOUNTER)engines_[i].counter);
      engines_.erase(engines_.begin() + i);
    } else {
      ++i;
    }
  }
  assignCopySlots();
}

void GpuSampler::assignCopySlots() {
  // Distinct copy-engine ordinals ascending: 1st -> Copy, rest -> Copy1.
  // (Ordinal is the physical engine number, stable across processes.)
  std::vector<int> ords;
  for (auto& e : engines_) {
    if (e.slot != EngineSlot::Copy0 && e.slot != EngineSlot::Copy1) continue;
    if (std::find(ords.begin(), ords.end(), e.engOrdinal) == ords.end())
      ords.push_back(e.engOrdinal);
  }
  std::sort(ords.begin(), ords.end());
  for (auto& e : engines_) {
    if (e.slot != EngineSlot::Copy0 && e.slot != EngineSlot::Copy1) continue;
    e.slot = copySlotForOrdinal(ords, e.engOrdinal);
  }
}

bool GpuSampler::initMemCounters(std::string*) {
  auto insts = enumInstances(L"GPU Adapter Memory");
  // Pass 0: only our adapter's instances; pass 1 (fallback): all.
  for (int pass = 0; pass < 2 && memCounters_.empty(); ++pass) {
    for (auto& inst : insts) {
      if (pass == 0 && !adapterLuidMatch_.empty() &&
          toLowerStr(narrow(inst)).find(adapterLuidMatch_) == std::string::npos)
        continue;
    std::wstring base = L"\\GPU Adapter Memory(" + inst + L")\\";
    PDH_HCOUNTER du = nullptr, su = nullptr;
    PDH_STATUS s1 = PdhAddEnglishCounterW((PDH_HQUERY)query_, (base + L"Dedicated Usage").c_str(), 0, &du);
    PDH_STATUS s3 = PdhAddEnglishCounterW((PDH_HQUERY)query_, (base + L"Shared Usage").c_str(), 0, &su);
    (void)s1;
    (void)s3;
    if (du) memCounters_.push_back({du, true});
    if (su) memCounters_.push_back({su, false});
    }
  }
  memReady_ = !memCounters_.empty();
  PdhCollectQueryData((PDH_HQUERY)query_);
  return memReady_;
}

void GpuSampler::queryAdapterName() {
  IDXGIFactory* f = nullptr;
  if (CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&f) != S_OK) return;
  // Pick adapter with largest DedicatedVideoMemory (discrete GPU first)
  IDXGIAdapter* best = nullptr;
  DXGI_ADAPTER_DESC bestDesc{};
  for (UINT i = 0;; ++i) {
    IDXGIAdapter* a = nullptr;
    if (f->EnumAdapters(i, &a) != S_OK) break;
    DXGI_ADAPTER_DESC d{};
    if (a->GetDesc(&d) == S_OK) {
      if (!best || d.DedicatedVideoMemory > bestDesc.DedicatedVideoMemory) {
        if (best) best->Release();
        best = a;
        bestDesc = d;
      } else {
        a->Release();
      }
    } else {
      a->Release();
    }
  }
  if (best) {
    last_.name = narrow(bestDesc.Description);
    adapterLuidMatch_ = formatLuidMatch((unsigned long)bestDesc.AdapterLuid.LowPart,
                                        bestDesc.AdapterLuid.HighPart);
    // Limits come from DXGI (PDH has only Usage, no Limit for GPU Adapter Memory)
    if (bestDesc.DedicatedVideoMemory > 0)
      last_.dedicatedLimit = (unsigned long long)bestDesc.DedicatedVideoMemory;
    if (bestDesc.SharedSystemMemory > 0)
      last_.sharedLimit = (unsigned long long)bestDesc.SharedSystemMemory;
    adapterNameResolved_ = true;
    best->Release();
  }
  f->Release();
}

void GpuSampler::queryTemp() {
  // ADL challenge: dynamic load, never link statically.
  static adl::Loader loader;
  static bool tried = false;
  if (!tried) {
    tried = true;
    loader.load();  // best effort
  }
  if (loader.inited || loader.adl2inited) {
    auto t = loader.queryTempC();
    if (t) {
      last_.tempC = t;
      last_.tempSource = "ADL";
      return;
    }
  }
  // PMLog path (RDNA2+, incl. RDNA4): driver-shared perf logging.
  {
    static pm::Manager pmLog;
    auto t = pmLog.readTempC();
    if (t) {
      last_.tempC = t;
      last_.tempSource = "ADL-PMLog";
      return;
    }
  }
  // No reliable inbox sensor for AMD temp -> N/A
  last_.tempC.reset();
  last_.tempSource.clear();
}

static double getDouble(void* counter) {
  PDH_FMT_COUNTERVALUE v{};
  if (PdhGetFormattedCounterValue((PDH_HCOUNTER)counter, PDH_FMT_DOUBLE, nullptr, &v) !=
      ERROR_SUCCESS)
    return 0.0;
  if (v.CStatus != ERROR_SUCCESS && v.CStatus != PDH_CSTATUS_VALID_DATA &&
      v.CStatus != PDH_CSTATUS_NEW_DATA)
    return 0.0;
  return v.doubleValue;
}
static unsigned long long getLarge(void* counter) {
  PDH_FMT_COUNTERVALUE v{};
  if (PdhGetFormattedCounterValue((PDH_HCOUNTER)counter, PDH_FMT_LARGE, nullptr, &v) !=
      ERROR_SUCCESS)
    return 0;
  if (v.CStatus != ERROR_SUCCESS && v.CStatus != PDH_CSTATUS_VALID_DATA &&
      v.CStatus != PDH_CSTATUS_NEW_DATA)
    return 0;
  long long x = v.largeValue;
  return x > 0 ? (unsigned long long)x : 0;
}

GpuInfo GpuSampler::sample() {
  syncEngines();  // pick up processes started after launch
  if (query_) PdhCollectQueryData((PDH_HQUERY)query_);

  double d3d = 0, comp = 0, copy0 = 0, copy1 = 0;
  bool any = false;
  for (auto& e : engines_) {
    double v = getDouble(e.counter);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (v > 0.0001) any = true;
    switch (e.slot) {
      case EngineSlot::D3D: d3d += v; break;
      case EngineSlot::Compute: comp += v; break;
      case EngineSlot::Copy0: copy0 += v; break;
      case EngineSlot::Copy1: copy1 += v; break;
      default: break;
    }
  }
  // Clamp sums (multiple engines each 0-100, but display per-type can exceed 100;
  // clamp to 100 for meter sanity while keeping relative info)
  auto clamp100 = [](double v) { return v > 100.0 ? 100.0 : v; };
  last_.util3D = clamp100(d3d);
  last_.utilCompute = clamp100(comp);
  last_.utilCopy = clamp100(copy0);
  last_.utilCopy1 = clamp100(copy1);
  last_.engineValid = !engines_.empty() && (any || true);

  // Memory: PDH gives Usage only; Limits come from DXGI (set in queryAdapterName).
  unsigned long long dUsed = 0, sUsed = 0;
  for (auto& m : memCounters_) {
    unsigned long long u = getLarge(m.usage);
    if (m.isDedicated)
      dUsed += u;
    else
      sUsed += u;
  }
  if (!memCounters_.empty()) {
    last_.dedicatedUsed = dUsed;
    last_.sharedUsed = sUsed;
    // dedicatedLimit/sharedLimit already set from DXGI; keep them
    last_.memValid = true;
  }

  queryTemp();  // best effort, cached loader
  return last_;
}
