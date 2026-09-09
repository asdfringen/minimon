#include "renderer.h"
#include <cstdio>
#include <sstream>

#ifndef MINIMON_VERSION
#define MINIMON_VERSION "?"
#endif

// UTF-8 block chars (console must be UTF-8)
static const char* FULL = "\xE2\x96\x88";   // █
static const char* LIGHT = "\xE2\x96\x91";  // ░
static const char* WHITE = "\x1b[37m";
static const char* GREEN = "\x1b[32m";
static const char* RED = "\x1b[31m";
// Back to white (not default) so all text stays white.
static const char* TO_WHITE = "\x1b[0m\x1b[37m";

// Byte truncation that never splits a UTF-8 sequence (avoids mojibake).
static std::string truncText(const std::string& s, size_t maxLen) {
  if (s.size() <= maxLen) return s;
  size_t end = maxLen;
  while (end > 0 && ((unsigned char)s[end] & 0xC0) == 0x80) --end;
  if (end == 0) return "";
  return s.substr(0, end);
}

// Bar is green (<=70%) / red (>70%); trailing % number is always white.
std::string meter(double pct, int width) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  if (width < 4) width = 4;
  if (width > 20) width = 20;
  int fill = (int)(pct / 100.0 * width + 0.5);
  if (fill > width) fill = width;
  const char* color = (pct <= 70.0) ? GREEN : RED;
  std::string s;
  s += color;
  s += "[";
  for (int i = 0; i < fill; ++i) s += FULL;
  for (int i = fill; i < width; ++i) s += LIGHT;
  s += "]";
  s += TO_WHITE;
  char buf[32];
  snprintf(buf, sizeof(buf), " %5.1f%%", pct);
  s += buf;
  return s;
}

std::string bytesToGB(unsigned long long b) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%.2f GB", b / 1024.0 / 1024.0 / 1024.0);
  return buf;
}

static std::string bytesToGBcompact(unsigned long long b) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%.2fGB", b / 1024.0 / 1024.0 / 1024.0);
  return buf;
}

// Meter width so that label + bar(2 brackets) + " 100.0%"(7) fits in width.
static int meterW(size_t labelLen, int width) {
  int w = width - (int)labelLen - 9;
  if (w < 4) w = 4;
  if (w > 20) w = 20;
  return w;
}

std::string renderFrame(const FrameData& f, bool withPosition, int consoleWidth, bool compact) {
  if (consoleWidth < 20) consoleWidth = 20;
  if (consoleWidth > 200) consoleWidth = 200;
  const size_t maxText = (size_t)(consoleWidth - 1);
  auto T = [&](const std::string& s) { return truncText(s, maxText); };

  std::ostringstream o;
  if (withPosition) o << WHITE << "\x1b[H";
  o << WHITE;

  // Compact (stacked) layout: short lines, meters on their own row.
  // Selected manually with the m key; never auto-switched by width.
  // Fixed 24 lines; a full clear happens on toggle, so updates stay stable.
  if (compact) {
    o << T("minimon v" MINIMON_VERSION " " + std::to_string((int)f.refreshSec) +
            "s [q:quit +/- m:wide]") +
           "\n";

  o << T("[CPU]") << "\n";
  o << T("  Total") << "\n";
  if (f.cpu.valid)
    o << T("  ") << meter(f.cpu.totalPct, meterW(2, consoleWidth)) << WHITE << "\n";
  else
    o << T("  sampling...") << "\n";

  o << T("[Memory]") << "\n";
    {
      char buf[128];
      snprintf(buf, sizeof(buf), "  Used %s", bytesToGBcompact(f.mem.usedBytes).c_str());
      o << T(buf) << "\n";
      snprintf(buf, sizeof(buf), "  /%s(%.1f%%)", bytesToGBcompact(f.mem.totalBytes).c_str(),
               f.mem.usedPct);
      o << T(buf) << "\n";
    }

    o << T("[GPU]") << "\n";
    o << T(std::string("  ") + f.gpu.name) << "\n";
    o << T("  3D") << "\n";
    o << T("  ") << meter(f.gpu.engineValid ? f.gpu.util3D : 0.0, meterW(2, consoleWidth))
      << WHITE << "\n";
    o << T("  Compute") << "\n";
    o << T("  ")
      << meter(f.gpu.engineValid ? f.gpu.utilCompute : 0.0, meterW(2, consoleWidth))
      << WHITE << "\n";
    o << T("  Copy") << "\n";
    o << T("  ") << meter(f.gpu.engineValid ? f.gpu.utilCopy : 0.0, meterW(2, consoleWidth))
      << WHITE << "\n";
    o << T("  Copy1") << "\n";
    o << T("  ")
      << meter(f.gpu.engineValid ? f.gpu.utilCopy1 : 0.0, meterW(2, consoleWidth))
      << WHITE << "\n";
    {
      double dPct = f.gpu.dedicatedLimit
                        ? 100.0 * (double)f.gpu.dedicatedUsed / (double)f.gpu.dedicatedLimit
                        : 0.0;
      double sPct = f.gpu.sharedLimit
                        ? 100.0 * (double)f.gpu.sharedUsed / (double)f.gpu.sharedLimit
                        : 0.0;
      char buf[128];
      o << T("  Dedicated") << "\n";
      if (f.gpu.memValid)
        snprintf(buf, sizeof(buf), "  %s/%s", bytesToGBcompact(f.gpu.dedicatedUsed).c_str(),
                 bytesToGBcompact(f.gpu.dedicatedLimit).c_str());
      else
        snprintf(buf, sizeof(buf), "  N/A");
      o << T(buf) << "\n";
      o << T("  ") << meter(f.gpu.memValid ? dPct : 0.0, meterW(2, consoleWidth)) << WHITE
        << "\n";
      o << T("  Shared") << "\n";
      if (f.gpu.memValid)
        snprintf(buf, sizeof(buf), "  %s/%s", bytesToGBcompact(f.gpu.sharedUsed).c_str(),
                 bytesToGBcompact(f.gpu.sharedLimit).c_str());
      else
        snprintf(buf, sizeof(buf), "  N/A");
      o << T(buf) << "\n";
      o << T("  ") << meter(f.gpu.memValid ? sPct : 0.0, meterW(2, consoleWidth)) << WHITE
        << "\n";
    }
    if (f.gpu.tempC) {
      char buf[64];
      snprintf(buf, sizeof(buf), "  Temp %.1fC(%s)", *f.gpu.tempC, f.gpu.tempSource.c_str());
      o << T(buf) << "\n";
    } else {
      o << T("  Temp N/A") << "\n";
    }



  if (withPosition)
    o << "\x1b[J";
    else
      o << "\x1b[0m";
    return o.str();
  }

  // Wide layout: fixed 16 lines (manual mode).
  o << T("minimon v" MINIMON_VERSION "  Refresh:" + std::to_string((int)f.refreshSec) +
          "s  [q:quit +/-:interval m:compact]") +
         "\n";

  // CPU (total only) — fixed 2 lines
  o << T("[CPU Utilisation]") << "\n";
  if (f.cpu.valid)
    o << T("  Total ") << meter(f.cpu.totalPct, meterW(8, consoleWidth)) << WHITE << "\n";
  else
    o << T("  Total (sampling...)") << "\n";

  // Memory — fixed 2 lines (Used only, no Free/Cache, no meter)
  o << T("[Memory]") << "\n";
  {
    char buf[256];
    snprintf(buf, sizeof(buf), "  Used %s / %s (%.1f%%)",
             bytesToGB(f.mem.usedBytes).c_str(), bytesToGB(f.mem.totalBytes).c_str(),
             f.mem.usedPct);
    o << T(buf) << "\n";
  }

  // GPU — fixed 10 lines (name/4 meters/2x text+meter/temp, no summary % line)
  o << T("[GPU]") << "\n";
  o << T(std::string("  ") + f.gpu.name) << "\n";
  o << T("  3D       ") << meter(f.gpu.engineValid ? f.gpu.util3D : 0.0, meterW(11, consoleWidth))
    << WHITE << "\n";
  o << T("  Compute  ")
    << meter(f.gpu.engineValid ? f.gpu.utilCompute : 0.0, meterW(11, consoleWidth)) << WHITE
    << "\n";
  o << T("  Copy     ") << meter(f.gpu.engineValid ? f.gpu.utilCopy : 0.0, meterW(11, consoleWidth))
    << WHITE << "\n";
  o << T("  Copy1    ")
    << meter(f.gpu.engineValid ? f.gpu.utilCopy1 : 0.0, meterW(11, consoleWidth)) << WHITE
    << "\n";
  {
    double dPct = f.gpu.dedicatedLimit
                      ? 100.0 * (double)f.gpu.dedicatedUsed / (double)f.gpu.dedicatedLimit
                      : 0.0;
    double sPct = f.gpu.sharedLimit
                      ? 100.0 * (double)f.gpu.sharedUsed / (double)f.gpu.sharedLimit
                      : 0.0;
    char buf[256];
    if (f.gpu.memValid) {
      snprintf(buf, sizeof(buf), "  Dedicated %s / %s",
               bytesToGB(f.gpu.dedicatedUsed).c_str(),
               bytesToGB(f.gpu.dedicatedLimit).c_str());
    } else {
      snprintf(buf, sizeof(buf), "  Dedicated N/A");
    }
    o << T(buf) << "\n";
    o << T("  ") << meter(f.gpu.memValid ? dPct : 0.0, meterW(2, consoleWidth)) << WHITE
      << "\n";
    if (f.gpu.memValid) {
      snprintf(buf, sizeof(buf), "  Shared    %s / %s", bytesToGB(f.gpu.sharedUsed).c_str(),
               bytesToGB(f.gpu.sharedLimit).c_str());
    } else {
      snprintf(buf, sizeof(buf), "  Shared    N/A");
    }
    o << T(buf) << "\n";
    o << T("  ") << meter(f.gpu.memValid ? sPct : 0.0, meterW(2, consoleWidth)) << WHITE
      << "\n";
  }
  if (f.gpu.tempC) {
    char buf[64];
    snprintf(buf, sizeof(buf), "  Temp %.1f C (%s)", *f.gpu.tempC, f.gpu.tempSource.c_str());
    o << T(buf) << "\n";
  } else {
    o << T("  Temp N/A (ADL unavailable)") << "\n";
  }


  // Erase leftover text below when content shrinks (no full clear -> no flicker).
  if (withPosition)
    o << "\x1b[J";
  else
    o << "\x1b[0m";
  return o.str();
}
