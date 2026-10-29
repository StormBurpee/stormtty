#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "theme_picker.hpp"
#include <shobjidl.h>
#include <filesystem>
#include <stdexcept>

namespace stormtty {
namespace {
template<class T> struct Com {
    T* value{}; ~Com(){if(value)value->Release();}
    T** put(){return &value;} T* operator->(){return value;}
};
struct Handle { HANDLE value{INVALID_HANDLE_VALUE}; ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);} };
void check(HRESULT result) {if(FAILED(result))throw std::runtime_error("The native theme picker could not complete.");}
void regular(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(handle)!=FILE_TYPE_DISK || !GetFileInformationByHandle(handle,&info) ||
        (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))
        throw std::runtime_error("Choose a regular local theme file, not a link or device.");
    wchar_t final_path[32768]{};
    const auto length=GetFinalPathNameByHandleW(handle,final_path,32768,FILE_NAME_NORMALIZED);
    if (!length || length>=32768 || std::wstring_view(final_path).starts_with(L"\\\\?\\UNC\\"))
        throw std::runtime_error("Choose a theme file on a local drive.");
}
}
ThemePickerResult exchange_theme_file(const std::filesystem::path& path, bool importing, ThemeFormat format, const Theme& current) {
    ThemePickerResult result;
    try {
        if(format!=ThemeFormat::stormtty && format!=ThemeFormat::ghostty)
            throw std::runtime_error("Select an explicit theme format.");
        const auto name=path.wstring();
        // No network shares, device namespace or implicit relative paths. Reject
        // mapped remote drives too, so bounded palette reads cannot become SMB I/O.
        if(name.size()<3 || name[1]!=L':' || name[2]!=L'\\' || name.find(L':',2)!=std::wstring::npos || !path.is_absolute())
            throw std::runtime_error("Choose a theme file on a local drive.");
        const auto drive=GetDriveTypeW(path.root_path().c_str());
        if(drive!=DRIVE_FIXED&&drive!=DRIVE_REMOVABLE&&drive!=DRIVE_RAMDISK)
            throw std::runtime_error("Choose a theme file on a local drive.");
        if(importing) {
            Handle file{CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
            if(file.value==INVALID_HANDLE_VALUE)throw std::runtime_error("The theme file could not be opened.");
            regular(file.value);LARGE_INTEGER size{};
            if(!GetFileSizeEx(file.value,&size)||size.QuadPart<0||size.QuadPart>static_cast<LONGLONG>(max_theme_bytes))
                throw std::runtime_error("Theme files must be no larger than 64 KiB.");
            // Allocate at most limit+1, including a sentinel read for file growth.
            std::string bytes(max_theme_bytes+1,'\0');DWORD read{};
            if(!ReadFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr))
                throw std::runtime_error("The theme file could not be read.");
            bytes.resize(read);auto parsed=import_theme(bytes,format,current);
            if(!parsed){const auto& error=parsed.errors.front();result.error="Theme line "+std::to_string(error.line)+": "+error.message;return result;}
            if(!parsed.warnings.empty()) result.notice="Cursor and selection colors weren’t imported.";
            result.imported=std::move(parsed.theme);
        } else {
            const auto bytes=export_theme(current,format);if(!bytes)throw std::runtime_error("The current palette contains invalid colors.");
            // Validate an existing destination without truncation. Write a
            // sibling temporary file and replace only after a successful flush.
            Handle existing{CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
            if(existing.value!=INVALID_HANDLE_VALUE) regular(existing.value);
            else if(GetLastError()!=ERROR_FILE_NOT_FOUND) throw std::runtime_error("The palette destination could not be opened.");
            GUID guid{};check(CoCreateGuid(&guid));wchar_t identifier[40]{};
            if(!StringFromGUID2(guid,identifier,40))throw std::runtime_error("The palette export could not be prepared.");
            const auto temporary=path.parent_path()/(std::wstring(L".stormtty-theme-")+identifier+L".tmp");
            struct Cleanup {std::filesystem::path path;bool created{};~Cleanup(){if(created)DeleteFileW(path.c_str());}} cleanup{temporary};
            {
                Handle file{CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
                if(file.value==INVALID_HANDLE_VALUE)throw std::runtime_error("The palette could not be exported.");
                cleanup.created=true;regular(file.value);DWORD written{};
                if(!WriteFile(file.value,bytes->data(),static_cast<DWORD>(bytes->size()),&written,nullptr)||written!=bytes->size()||!FlushFileBuffers(file.value))
                    throw std::runtime_error("The palette could not be written; the previous file was kept.");
            }
            if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("The palette could not be replaced; the previous file was kept.");
            result.notice=format==ThemeFormat::ghostty?"Terminal colors exported; chrome and accent omitted.":"Palette exported.";
        }
    } catch(const std::exception& error) {result.error=error.what();}
      catch(...) {result.error="The theme operation could not be completed.";}
    return result;
}
ThemePickerResult pick_theme(HWND owner, bool importing, const Theme& current) {
    ThemePickerResult result;
    if(!IsWindow(owner)) return result;
    try {
        Com<IFileDialog> dialog;
        check(CoCreateInstance(importing?CLSID_FileOpenDialog:CLSID_FileSaveDialog,nullptr,CLSCTX_INPROC_SERVER,
            __uuidof(IFileDialog),reinterpret_cast<void**>(dialog.put())));
        FILEOPENDIALOGOPTIONS options{};check(dialog->GetOptions(&options));
        check(dialog->SetOptions(options|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR|FOS_DONTADDTORECENT|FOS_NODEREFERENCELINKS|
            (importing?FOS_FILEMUSTEXIST:FOS_OVERWRITEPROMPT)));
        const COMDLG_FILTERSPEC filters[]={{L"StormTTY palette (*.stormtheme)",L"*.stormtheme"},{importing?L"Ghostty terminal colors (any filename)":L"Ghostty terminal colors (*.ghostty)",importing?L"*.*":L"*.ghostty"}};
        check(dialog->SetFileTypes(2,filters));check(dialog->SetFileTypeIndex(1));
        check(dialog->SetTitle(importing?L"Import a palette into Settings":L"Export the current palette"));
        if(!importing){check(dialog->SetFileName(L"My palette"));check(dialog->SetDefaultExtension(L"stormtheme"));}
        auto shown=dialog->Show(owner);if(shown==HRESULT_FROM_WIN32(ERROR_CANCELLED))return result;check(shown);
        if(!IsWindow(owner))return result;
        UINT filter{};check(dialog->GetFileTypeIndex(&filter));
        if(filter!=1&&filter!=2)throw std::runtime_error("Select an explicit theme format.");
        const auto format=filter==1?ThemeFormat::stormtty:ThemeFormat::ghostty;
        Com<IShellItem> item;check(dialog->GetResult(item.put()));PWSTR raw{};
        check(item->GetDisplayName(SIGDN_FILESYSPATH,&raw));
        struct StringOwner{PWSTR value;~StringOwner(){CoTaskMemFree(value);}} path_owner{raw};
        const std::filesystem::path path(raw);
        return exchange_theme_file(path,importing,format,current);
    } catch(const std::exception& error) {result.error=error.what();}
      catch(...) {result.error="The theme operation could not be completed.";}
    return result;
}
}
