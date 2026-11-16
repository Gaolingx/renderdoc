/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2014-2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

// This project deliberately references only kernel32.dll (ie. not even the CRT)
// so that when inserted into an application it has as small an overhead/impact
// as possible. Ideally it would be present only to be a pass-through hook and
// the first time only to allocate a little, check if this process should be hooked
// and load the renderdoc dll.
//
// The no-CRT restriction causes some awkward bits and pieces but the dll is simple
// enough that it's not a big issue.

#include "renderdocshim.h"
#include <windows.h>

struct CaptureOptions;
typedef void(__cdecl *pINTERNAL_SetCaptureOptions)(const CaptureOptions *opts);
typedef void(__cdecl *pINTERNAL_SetLogFile)(const char *logfile);
typedef void(__cdecl *pINTERNAL_SetDebugLogFile)(const char *logfile);

// Debug aid for the Endfield real-machine investigation. Processes whose exe path
// contains "endfield" append their decision trace to E:\caps\shim_trace.log so we can
// see what the shim did inside a process whose module list ACE hides from us.
static void TraceWrite(const wchar_t *txt)
{
  HANDLE file = CreateFileW(L"E:\\caps\\shim_trace.log", FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
  if(file == INVALID_HANDLE_VALUE)
    return;

  DWORD wrote = 0;
  WriteFile(file, txt, lstrlenW(txt) * sizeof(wchar_t), &wrote, NULL);

  CloseHandle(file);
}

static void TraceHexDWORD(const wchar_t *tag, DWORD val)
{
  wchar_t line[128];
  int n = 0;
  const wchar_t *t = tag;
  while(*t != L'\0' && n < 100)
    line[n++] = *t++;

  line[n++] = L'=';
  line[n++] = L'0';
  line[n++] = L'x';

  const wchar_t *hex = L"0123456789abcdef";
  for(int i = 28; i >= 0; i -= 4)
    line[n++] = hex[(val >> i) & 0xf];

  line[n++] = L'\r';
  line[n++] = L'\n';
  line[n] = L'\0';

  TraceWrite(line);
}

static void TraceLine2(const wchar_t *tag, const wchar_t *value)
{
  wchar_t *line = (wchar_t *)VirtualAlloc(NULL, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if(line == NULL)
    return;

  int n = 0;
  const wchar_t *t = tag;
  while(*t != L'\0')
    line[n++] = *t++;

  const wchar_t *v = value;
  while(*v != L'\0' && n < 4090)
    line[n++] = *v++;

  line[n++] = L'\r';
  line[n++] = L'\n';
  line[n] = L'\0';

  TraceWrite(line);
  VirtualFree(line, 0, MEM_RELEASE);
}

#if defined(RELEASE)
#define LOGPRINT(txt) \
  do                  \
  {                   \
  } while(0)
#else
// define this to something to get logging
// #define LOGPRINT(txt) OutputDebugStringW(txt)
#define LOGPRINT(txt) \
  do                  \
  {                   \
  } while(0)
#endif

static ShimData *MapShimData(HANDLE *mappingHandle)
{
  HANDLE datahandle = OpenFileMappingA(FILE_MAP_READ, FALSE, GLOBAL_HOOK_DATA_NAME);

  if(datahandle == NULL)
    return NULL;

  ShimData *data = (ShimData *)MapViewOfFile(datahandle, FILE_MAP_READ, 0, 0, sizeof(ShimData));

  if(data == NULL)
  {
    CloseHandle(datahandle);
    return NULL;
  }

  *mappingHandle = datahandle;
  return data;
}

static ShimData *ReadShimDataFile(HMODULE hModule)
{
  // no CRT: the path buffer has to be heap-allocated, a large stack array would pull __chkstk in
  wchar_t *cfgpath =
      (wchar_t *)VirtualAlloc(NULL, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if(cfgpath == NULL)
    return NULL;

  DWORD len = GetModuleFileNameW(hModule, cfgpath, 4095);
  if(len == 0 || len >= 4095)
  {
    VirtualFree(cfgpath, 0, MEM_RELEASE);
    return NULL;
  }

  // find the last slash so we can replace the dll name with the config file name
  wchar_t *lastSlash = cfgpath;
  for(wchar_t *p = cfgpath; *p != L'\0'; p++)
  {
    if(*p == L'\\')
      lastSlash = p + 1;
  }

  // no str*cpy functions, use the kernel32 ones
  if(lastSlash + lstrlenW(GLOBAL_HOOK_FILE_NAME_W) + 1 >= cfgpath + 4095)
  {
    VirtualFree(cfgpath, 0, MEM_RELEASE);
    return NULL;
  }
  lstrcpyW(lastSlash, GLOBAL_HOOK_FILE_NAME_W);

  HANDLE file = CreateFileW(cfgpath, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  VirtualFree(cfgpath, 0, MEM_RELEASE);

  if(file == INVALID_HANDLE_VALUE)
    return NULL;

  // no new[], need to use VirtualAlloc
  ShimData *data = (ShimData *)VirtualAlloc(NULL, sizeof(ShimData), MEM_RESERVE | MEM_COMMIT,
                                            PAGE_READWRITE);
  if(data == NULL)
  {
    CloseHandle(file);
    return NULL;
  }

  DWORD read = 0;
  BOOL ok = ReadFile(file, data, sizeof(ShimData), &read, NULL);
  CloseHandle(file);

  if(!ok || read != sizeof(ShimData))
  {
    VirtualFree(data, 0, MEM_RELEASE);
    return NULL;
  }

  return data;
}

void CheckHook(HMODULE hModule)
{
  HANDLE mappingHandle = NULL;

  // compute the exe path first, the trace gate below needs it
  const int exepathLen = 1024;
  wchar_t *exepath = (wchar_t *)VirtualAlloc(NULL, exepathLen * sizeof(wchar_t),
                                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

  bool trace = false;
  if(exepath)
  {
    // no memset :).
    SecureZeroMemory(exepath, exepathLen);

    GetModuleFileNameW(NULL, exepath, exepathLen - 1);
    trace = FindStringOrdinal(FIND_FROMSTART, exepath, -1, L"endfield", -1, TRUE) >= 0;
  }

  if(trace)
  {
    TraceHexDWORD(L"[shim] enter pid", GetCurrentProcessId());
    TraceLine2(L"[shim] exe ", exepath);
  }

  // the classic global hook holds its data in a named section, try that first
  ShimData *data = MapShimData(&mappingHandle);
  const wchar_t *config = L"classic";

  // a detached global hook leaves a config file next to this dll instead
  if(data == NULL)
  {
    data = ReadShimDataFile(hModule);
    config = data ? L"file" : L"none";
  }

  if(trace)
    TraceLine2(L"[shim] config source ", config);

  if(data == NULL)
  {
    LOGPRINT(L"renderdocshim: can't open global data\n");
    VirtualFree(exepath, 0, MEM_RELEASE);
    return;
  }

  if(data->pathmatchstring[0] == 0 || data->pathmatchstring[1] == 0 ||
     data->pathmatchstring[2] == 0 || data->pathmatchstring[3] == 0)
  {
    LOGPRINT(L"renderdocshim: invalid pathmatchstring: '");
    LOGPRINT(data->pathmatchstring);
    LOGPRINT(L"'\n");

    if(trace)
      TraceWrite(L"[shim] invalid pathmatchstring\r\n");

    if(mappingHandle)
    {
      UnmapViewOfFile(data);
      CloseHandle(mappingHandle);
    }
    else
    {
      VirtualFree(data, 0, MEM_RELEASE);
    }
    VirtualFree(exepath, 0, MEM_RELEASE);
    return;
  }

  if(exepath)
  {
    // no str*cmp functions
    int find = FindStringOrdinal(FIND_FROMSTART, exepath, -1, data->pathmatchstring, -1, TRUE);

    if(find >= 0)
    {
      LOGPRINT(L"renderdocshim: Hooking into '");
      LOGPRINT(exepath);
      LOGPRINT(L"', based on '");
      LOGPRINT(data->pathmatchstring);
      LOGPRINT(L"'\n");

      if(trace)
      {
        TraceWrite(L"[shim] MATCH\r\n");
        TraceLine2(L"[shim] loading ", data->rdocpath);
      }

      HMODULE mod = LoadLibraryW(data->rdocpath);

      if(mod)
      {
        if(trace)
          TraceWrite(L"[shim] LoadLibrary OK\r\n");

        pINTERNAL_SetCaptureOptions setopts =
            (pINTERNAL_SetCaptureOptions)GetProcAddress(mod, "INTERNAL_SetCaptureOptions");
        pINTERNAL_SetLogFile setlogfile =
            (pINTERNAL_SetLogFile)GetProcAddress(mod, "INTERNAL_SetLogFile");
        pINTERNAL_SetDebugLogFile setdebuglog =
            (pINTERNAL_SetDebugLogFile)GetProcAddress(mod, "INTERNAL_SetDebugLogFile");

        if(setopts)
          setopts((const CaptureOptions *)data->opts);

        if(setlogfile && data->capfile[0])
          setlogfile(data->capfile);

        if(setdebuglog && data->debuglog[0])
          setdebuglog(data->debuglog);

        if(trace)
          TraceWrite(L"[shim] renderdoc configured, capture ready\r\n");
      }
      else
      {
        if(trace)
          TraceHexDWORD(L"[shim] LoadLibrary FAILED gle", GetLastError());
      }
    }
    else
    {
      LOGPRINT(L"renderdocshim: NOT Hooking into '");
      LOGPRINT(exepath);
      LOGPRINT(L"', based on '");
      LOGPRINT(data->pathmatchstring);
      LOGPRINT(L"'\n");

      if(trace)
        TraceWrite(L"[shim] no match, skipping\r\n");
    }

    VirtualFree(exepath, 0, MEM_RELEASE);
  }
  else
  {
    LOGPRINT(L"renderdocshim: Failed to allocate exepath\n");
  }

  if(mappingHandle)
  {
    UnmapViewOfFile(data);
    CloseHandle(mappingHandle);
  }
  else
  {
    VirtualFree(data, 0, MEM_RELEASE);
  }
}

DWORD WINAPI CheckHookThread(LPVOID param)
{
  CheckHook((HMODULE)param);

  // this makes sure that we remove the reference to the shim dll and unload from
  // the target process. That minimises the impact of having the dll inserted into
  // every process
  FreeLibraryAndExitThread((HMODULE)param, 0);
  return 0;
}

BOOL APIENTRY dll_entry(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
  if(ul_reason_for_call == DLL_PROCESS_ATTACH)
  {
    DisableThreadLibraryCalls(hModule);

    // create a thread so that we can perform more complex actions (DllMain must be minimal
    // in size, even this is a bit dodgy).
    CreateThread(NULL, 0, CheckHookThread, (LPVOID)hModule, 0, NULL);
  }

  return TRUE;
}
