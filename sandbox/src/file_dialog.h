// Thin wrappers around the Win32 common file dialogs.
#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>

namespace tubane::sandbox {

// `filter` uses the GetOpenFileName format: L"Label\0*.ext\0All\0*.*\0".
std::optional<std::filesystem::path> OpenFileDialog(HWND owner, const wchar_t* filter, const wchar_t* title);
std::optional<std::filesystem::path> SaveFileDialog(HWND owner, const wchar_t* filter, const wchar_t* defaultExt,
                                                    const wchar_t* title, const std::filesystem::path& suggested);

}  // namespace tubane::sandbox
