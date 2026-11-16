#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace drwin {

std::string to_utf8(std::wstring_view w);
std::wstring to_wide(std::string_view s);

// %USERPROFILE%\Videos\Demo Recorder
std::wstring default_library_folder();

// Creates "<library>\Recording 2026-11-16 at 09.30.00.demorecorder".
std::wstring make_project_folder(const std::wstring& library);

std::string windows_version_string();

}  // namespace drwin
