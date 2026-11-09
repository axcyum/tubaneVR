#include "file_dialog.h"

#include <commdlg.h>

#include <cwchar>

namespace tubane::sandbox {

std::optional<std::filesystem::path> OpenFileDialog(HWND owner, const wchar_t* filter, const wchar_t* title) {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = title;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return std::nullopt;
    return std::filesystem::path(file);
}

std::optional<std::filesystem::path> SaveFileDialog(HWND owner, const wchar_t* filter, const wchar_t* defaultExt,
                                                    const wchar_t* title, const std::filesystem::path& suggested) {
    wchar_t file[MAX_PATH] = {};
    wcsncpy_s(file, suggested.wstring().c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = title;
    ofn.lpstrDefExt = defaultExt;
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return std::nullopt;
    return std::filesystem::path(file);
}

}  // namespace tubane::sandbox
