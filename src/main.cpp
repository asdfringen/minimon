#include <windows.h>
#include <conio.h>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "sys_sampler.h"
#include "gpu_sampler.h"
#include "renderer.h"

static bool g_run = true;
static BOOL WINAPI ctrlHandler(DWORD) {
  g_run = false;
  return TRUE;
}

static std::string hostname() {
  char buf[256] = {};
  DWORD n = sizeof(buf);
  if (GetComputerNameA(buf, &n)) return buf;
  return "?";
}

static void consoleSize(int* w, int* h) {  *w = 80;
  *h = 30;
  HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (GetConsoleScreenBufferInfo(hOut, &info)) {
    int ww = (int)(info.srWindow.Right - info.srWindow.Left + 1);
    int hh = (int)(info.srWindow.Bottom - info.srWindow.Top + 1);
    if (ww >= 20 && ww <= 1000) *w = ww;
    if (hh >= 10 && hh <= 1000) *h = hh;
  }
}

int main(int argc, char** argv) {
  bool once = argc > 1 && std::string(argv[1]) == "--once";
  bool compactMode = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--compact") compactMode = true;
  }

  HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  bool isConsole = GetConsoleMode(hOut, &mode) != FALSE;

  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  // Enable Virtual Terminal Processing for ANSI
  if (isConsole) SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

  SetConsoleCtrlHandler(ctrlHandler, TRUE);

  // Hide the console scrollbars so resizing never flashes them.
  // Re-asserted on resize since the host may restore them then.
  if (HWND hwnd = GetConsoleWindow()) {
    ShowScrollBar(hwnd, SB_VERT, FALSE);
    ShowScrollBar(hwnd, SB_HORZ, FALSE);
  }

  CpuSampler cpu;
  GpuSampler gpu;
  std::string err;
  if (!cpu.init(&err)) fprintf(stderr, "CPU init: %s\n", err.c_str());
  if (!gpu.init(&err)) fprintf(stderr, "GPU init: %s\n", err.c_str());

  double refreshSec = 2.0;
  std::string host = hostname();

  // Warm up PDH (needs 2 samples) — before entering alt screen so
  // --once never touches the screen buffer.
  cpu.sample();
  gpu.sample();
  if (once) std::this_thread::sleep_for(std::chrono::milliseconds(1200));

  if (once) {
    FrameData f;
    f.hostname = host;
    f.refreshSec = refreshSec;
    f.cpu = cpu.sample();
    f.mem = queryMemory();
    f.gpu = gpu.sample();
    int w, h;
    consoleSize(&w, &h);
    std::string s = renderFrame(f, false, w, compactMode);
    fputs(s.c_str(), stdout);
    fflush(stdout);
    printf("\x1b[0m");
    return 0;
  }

  // Fixed-position fullscreen: alternate screen buffer + hidden cursor.
  // Scrollback is untouched. Disable autowrap so narrow windows clip
  // instead of wrapping (which would scroll and garble).
  printf("\x1b[?1049h\x1b[H\x1b[2J\x1b[?25l\x1b[?7l\x1b[37m");
  fflush(stdout);

  auto drawOnce = [&](int w) {
    FrameData f;
    f.hostname = host;
    f.refreshSec = refreshSec;
    f.cpu = cpu.sample();
    f.mem = queryMemory();
    f.gpu = gpu.sample();
    std::string s = renderFrame(f, true, w, compactMode);
    fputs(s.c_str(), stdout);
    fflush(stdout);
  };

  // First frame before entering the loop.
  int lastW = 0, lastH = 0;
  bool forceClear = false;
  consoleSize(&lastW, &lastH);
  printf("\x1b[H\x1b[2J");
  fflush(stdout);
  drawOnce(lastW);

  while (g_run) {
    int w, h;
    consoleSize(&w, &h);
    if (forceClear || w != lastW || h != lastH) {
      // Resize: keep scrollbars hidden (host may restore them on resize).
      if (HWND hwnd = GetConsoleWindow()) {
        ShowScrollBar(hwnd, SB_VERT, FALSE);
        ShowScrollBar(hwnd, SB_HORZ, FALSE);
      }
      // Full clear so shrunken layouts
      // don't keep leftover glyphs from the previous frame.
      printf("\x1b[H\x1b[2J");
      fflush(stdout);
      lastW = w;
      lastH = h;
      forceClear = false;
    }

    drawOnce(w);

    // Sleep in 100ms slices to handle keys promptly
    int slices = (int)(refreshSec * 10);
    for (int i = 0; i < slices && g_run; ++i) {
      if (_kbhit()) {
        int ch = _getch();
        if (ch == 'q' || ch == 'Q' || ch == 27) {
          g_run = false;
          break;
        } else if (ch == '+') {
          if (refreshSec < 10) refreshSec += 1.0;
        } else if (ch == '-') {
          if (refreshSec > 1) refreshSec -= 1.0;
        } else if (ch == 'm' || ch == 'M') {
          compactMode = !compactMode;  // manual wide/compact toggle
          forceClear = true;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  printf("\x1b[?7h\x1b[?25h\x1b[0m\x1b[?1049l");
  fflush(stdout);
  return 0;
}
