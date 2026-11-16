#include "util.h"

#include <shlobj.h>

#include <filesystem>
#include <stdexcept>

namespace drwin {

std::string to_utf8(std::wstring_view w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring to_wide(std::string_view s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::wstring default_library_folder() {
  PWSTR videos = nullptr;
  std::wstring base;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videos))) {
    base = videos;
  }
  CoTaskMemFree(videos);
  if (base.empty()) base = L".";
  return (std::filesystem::path(base) / L"Demo Recorder").wstring();
}

std::wstring make_project_folder(const std::wstring& library) {
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t name[96];
  swprintf_s(name, L"Recording %04u-%02u-%02u at %02u.%02u.%02u.demorecorder", t.wYear, t.wMonth,
             t.wDay, t.wHour, t.wMinute, t.wSecond);
  std::filesystem::path folder = std::filesystem::path(library) / name;
  std::filesystem::create_directories(folder);
  return folder.wstring();
}

std::string windows_version_string() {
  // GetVersionEx lies to unmanifested apps; RtlGetVersion does not.
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  auto fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
  OSVERSIONINFOW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  if (!fn || fn(&info) != 0) return "unknown";
  return std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) + "." +
         std::to_string(info.dwBuildNumber);
}

}  // namespace drwin
