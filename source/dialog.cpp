#include "dialog.hpp"

#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <commdlg.h>

namespace {

struct CoInitRAII {
    HRESULT hr { E_FAIL };
    CoInitRAII()
    {
        hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    }
    ~CoInitRAII()
    {
        if (SUCCEEDED(hr))
            CoUninitialize();
    }
    explicit operator bool() const { return SUCCEEDED(hr); }
};

std::wstring utf8_to_wide(const std::string& s)
{
    if (s.empty())
        return L"";
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out;
    out.resize(len);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
    return out;
}

std::wstring join_extensions_pattern(const std::vector<std::string>& exts)
{
    // Create "*.png;*.jpg;*.jpeg" pattern
    std::wstring spec;
    bool first = true;
    for (const auto& e : exts) {
        if (!first)
            spec += L";";
        first = false;
        std::wstring we = utf8_to_wide(e);
        // strip any leading dots
        if (!we.empty() && we.front() == L'.')
            we.erase(we.begin());
        spec += L"*.";
        spec += we;
    }
    if (spec.empty())
        spec = L"*.*";
    return spec;
}

std::vector<COMDLG_FILTERSPEC> build_filterspec(
    const std::vector<dialog_file_filter>& filters,
    // storage to own the strings for lifetime of COM call
    std::vector<std::wstring>& owned_texts,
    std::vector<std::wstring>& owned_specs)
{
    std::vector<COMDLG_FILTERSPEC> out;
    if (filters.empty()) {
        owned_texts.push_back(L"All Files");
        owned_specs.push_back(L"*.*");
        out.push_back(COMDLG_FILTERSPEC { owned_texts.back().c_str(), owned_specs.back().c_str() });
        return out;
    }
    out.reserve(filters.size());
    owned_texts.reserve(filters.size());
    owned_specs.reserve(filters.size());
    for (const auto& f : filters) {
        owned_texts.push_back(utf8_to_wide(f.text.empty() ? std::string("Files") : f.text));
        owned_specs.push_back(join_extensions_pattern(f.extensions));
        out.push_back(COMDLG_FILTERSPEC { owned_texts.back().c_str(), owned_specs.back().c_str() });
    }
    return out;
}

std::wstring first_extension_or_empty(const std::vector<dialog_file_filter>& filters)
{
    for (const auto& f : filters) {
        for (const auto& e : f.extensions) {
            std::wstring w = utf8_to_wide(e);
            if (!w.empty() && w.front() == L'.')
                w.erase(w.begin());
            if (!w.empty())
                return w;
        }
    }
    return L"";
}

IShellItem* path_to_shell_item(const std::filesystem::path& p)
{
    if (p.empty())
        return nullptr;
    IShellItem* item = nullptr;
    const std::wstring ws = p.wstring();
    if (SUCCEEDED(SHCreateItemFromParsingName(ws.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        return item; // refcount 1
    }
    return nullptr;
}

std::optional<std::filesystem::path> item_to_path(IShellItem* item)
{
    if (!item)
        return std::nullopt;
    PWSTR psz = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz) {
        std::filesystem::path p = psz;
        CoTaskMemFree(psz);
        return p;
    }
    // network/virtual items may fail SIGDN_FILESYSPATH; try parsing name
    PWSTR ppsz = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &ppsz)) && ppsz) {
        std::filesystem::path p = ppsz;
        CoTaskMemFree(ppsz);
        return p;
    }
    return std::nullopt;
}

void set_dialog_initial_location(IFileDialog* dlg, const std::filesystem::path& default_path, bool is_save)
{
    if (!dlg)
        return;
    if (default_path.empty())
        return;

    std::filesystem::path folder = default_path;
    std::wstring filename;

    if (is_save) {
        if (std::filesystem::is_directory(default_path)) {
            folder = default_path;
        } else {
            if (default_path.has_parent_path())
                folder = default_path.parent_path();
            filename = default_path.filename().wstring();
        }
    } else {
        // open: if default_path is a directory -> set folder
        // if it's a file path, set folder to parent and file name to that file
        if (std::filesystem::is_directory(default_path)) {
            folder = default_path;
        } else {
            if (default_path.has_parent_path())
                folder = default_path.parent_path();
            filename = default_path.filename().wstring();
        }
    }

    if (!filename.empty())
        dlg->SetFileName(filename.c_str());

    if (!folder.empty()) {
        if (IShellItem* si = path_to_shell_item(folder)) {
            // Prefer SetFolder (stronger than SetDefaultFolder)
            if (FAILED(dlg->SetFolder(si))) {
                dlg->SetDefaultFolder(si);
            }
            si->Release();
        }
    }
}
}

std::optional<std::filesystem::path> open_file_dialog(
    const std::vector<dialog_file_filter> filters,
    const std::filesystem::path& default_path)
{
    CoInitRAII co;
    if (!co)
        return std::nullopt;

    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dlg));
    if (FAILED(hr) || !dlg)
        return std::nullopt;

    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts))) {
        opts |= FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
        dlg->SetOptions(opts);
    }

    // Filters
    std::vector<std::wstring> texts, specs;
    const auto filterSpec = build_filterspec(filters, texts, specs);
    if (!filterSpec.empty()) {
        dlg->SetFileTypes(static_cast<UINT>(filterSpec.size()), filterSpec.data());
        dlg->SetFileTypeIndex(1); // 1-based index
    }

    set_dialog_initial_location(dlg, default_path, /*is_save=*/false);

    std::optional<std::filesystem::path> result;
    hr = dlg->Show(nullptr);
    if (SUCCEEDED(hr)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            result = item_to_path(item);
            item->Release();
        }
    }

    dlg->Release();
    return result;
}

std::optional<std::filesystem::path> save_file_dialog(
    const std::vector<dialog_file_filter> filters,
    const std::filesystem::path& default_path)
{
    CoInitRAII co;
    if (!co)
        return std::nullopt;

    IFileSaveDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dlg));
    if (FAILED(hr) || !dlg)
        return std::nullopt;

    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts))) {
        opts |= FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST;
        dlg->SetOptions(opts);
    }

    // Filters
    std::vector<std::wstring> texts, specs;
    const auto filterSpec = build_filterspec(filters, texts, specs);
    if (!filterSpec.empty()) {
        dlg->SetFileTypes(static_cast<UINT>(filterSpec.size()), filterSpec.data());
        dlg->SetFileTypeIndex(1); // 1-based index
    }

    // Default extension: first extension we find
    const std::wstring defExt = first_extension_or_empty(filters);
    if (!defExt.empty())
        dlg->SetDefaultExtension(defExt.c_str());

    set_dialog_initial_location(dlg, default_path, /*is_save=*/true);

    std::optional<std::filesystem::path> result;
    hr = dlg->Show(nullptr);
    if (SUCCEEDED(hr)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            result = item_to_path(item);
            item->Release();
        }
    }

    dlg->Release();
    return result;
}

std::optional<std::filesystem::path> pick_directory_dialog(
    const std::filesystem::path& default_path)
{
    CoInitRAII co;
    if (!co)
        return std::nullopt;

    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dlg));
    if (FAILED(hr) || !dlg)
        return std::nullopt;

    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts))) {
        opts |= FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
        dlg->SetOptions(opts);
    }

    set_dialog_initial_location(dlg, default_path, /*is_save=*/false);

    std::optional<std::filesystem::path> result;
    hr = dlg->Show(nullptr);
    if (SUCCEEDED(hr)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            result = item_to_path(item);
            item->Release();
        }
    }

    dlg->Release();
    return result;
}