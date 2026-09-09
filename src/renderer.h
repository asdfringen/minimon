#pragma once
#include <string>
#include "sys_sampler.h"
#include "gpu_sampler.h"

// Text is white only; meters are green (<=70%) / red (>70%).
std::string meter(double pct, int width = 20);
std::string bytesToGB(unsigned long long bytes);

struct FrameData {
  CpuSample cpu;
  MemInfo mem;
  GpuInfo gpu;
  std::string hostname;
  double refreshSec = 2.0;
};

std::string renderFrame(const FrameData& f, bool withPosition = true, int consoleWidth = 80,
                        bool compact = false);
