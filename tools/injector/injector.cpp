// OpenSlinky Injector - small Win32 UI that loads libslinky.dll into a running Minecraft (java/javaw) process.
//
// Injection is the plain LoadLibraryW + CreateRemoteThread route (the DLL starts itself from DllMain).
// By default a copy of the DLL is injected from %TEMP%, so the build output stays writable and you can
// rebuild while the game is running.

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <tlhelp32.h>

#include <cwchar>
#include <string>
#include <vector>

namespace {

enum ControlId : int {
    kIdProcessCombo = 101,
    kIdRefresh,
    kIdDllEdit,
    kIdBrowse,
    kIdCopyCheck,
    kIdInject,
    kIdStatus,
};

struct JavaProcess {
    DWORD pid = 0;
    std::wstring exe;
    std::wstring title;
};

HWND g_wnd = nullptr;
HWND g_combo = nullptr;
HWND g_dll_edit = nullptr;
HWND g_copy_check = nullptr;
HWND g_status = nullptr;
HFONT g_font = nullptr;
std::vector<JavaProcess> g_processes;

std::wstring error_text(DWORD code) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring text = buf != nullptr ? buf : L"Unknown error";
    if (buf != nullptr) {
        LocalFree(buf);
    }
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ')) {
        text.pop_back();
    }
    return text + L" (" + std::to_wstring(code) + L")";
}

void set_status(const std::wstring& text) {
    SetWindowTextW(g_status, text.c_str());
}

std::wstring exe_dir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    const size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : dir.substr(0, slash);
}

bool file_exists(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring default_dll_path() {
    const std::wstring dir = exe_dir();
    const std::wstring candidates[] = {
        dir + L"\\libslinky.dll",
        dir + L"\\build\\libslinky.dll",
        dir + L"\\..\\build\\libslinky.dll",
    };
    for (const std::wstring& c : candidates) {
        if (file_exists(c)) {
            wchar_t full[MAX_PATH]{};
            GetFullPathNameW(c.c_str(), MAX_PATH, full, nullptr);
            return full;
        }
    }
    return dir + L"\\libslinky.dll";
}

struct TitleSearch {
    DWORD pid;
    std::wstring title;
};

BOOL CALLBACK find_window_title(HWND hwnd, LPARAM lparam) {
    auto* search = reinterpret_cast<TitleSearch*>(lparam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != search->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
    }
    wchar_t title[256]{};
    if (GetWindowTextW(hwnd, title, 256) > 0) {
        search->title = title;
        return FALSE;
    }
    return TRUE;
}

bool contains_ci(const std::wstring& haystack, const wchar_t* needle) {
    std::wstring h = haystack;
    std::wstring n = needle;
    CharLowerBuffW(h.data(), static_cast<DWORD>(h.size()));
    CharLowerBuffW(n.data(), static_cast<DWORD>(n.size()));
    return h.find(n) != std::wstring::npos;
}

void refresh_processes() {
    const DWORD previous = (SendMessageW(g_combo, CB_GETCURSEL, 0, 0) >= 0 && !g_processes.empty())
        ? g_processes[static_cast<size_t>(SendMessageW(g_combo, CB_GETCURSEL, 0, 0))].pid
        : 0;

    g_processes.clear();
    SendMessageW(g_combo, CB_RESETCONTENT, 0, 0);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL ok = Process32FirstW(snap, &entry); ok; ok = Process32NextW(snap, &entry)) {
            if (_wcsicmp(entry.szExeFile, L"javaw.exe") != 0 && _wcsicmp(entry.szExeFile, L"java.exe") != 0) {
                continue;
            }
            TitleSearch search{entry.th32ProcessID, L""};
            EnumWindows(find_window_title, reinterpret_cast<LPARAM>(&search));
            g_processes.push_back({entry.th32ProcessID, entry.szExeFile, search.title});
        }
        CloseHandle(snap);
    }

    int select = -1;
    for (size_t i = 0; i < g_processes.size(); ++i) {
        const JavaProcess& p = g_processes[i];
        std::wstring label = p.exe + L"  (PID " + std::to_wstring(p.pid) + L")";
        if (!p.title.empty()) {
            label += L"  -  " + p.title;
        }
        SendMessageW(g_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (p.pid == previous) {
            select = static_cast<int>(i);
        } else if (select < 0 && (contains_ci(p.title, L"minecraft") || contains_ci(p.title, L"lunar"))) {
            select = static_cast<int>(i);
        }
    }
    if (select < 0 && !g_processes.empty()) {
        select = 0;
    }
    SendMessageW(g_combo, CB_SETCURSEL, static_cast<WPARAM>(select), 0);
    EnableWindow(GetDlgItem(g_wnd, kIdInject), g_processes.empty() ? FALSE : TRUE);
    set_status(g_processes.empty() ? L"No Minecraft (java/javaw) process found. Start the game and press Refresh."
                                   : L"Ready.");
}

// Copies the DLL to %TEMP%\openslinky\ under a unique name and removes old, unlocked copies.
bool make_shadow_copy(const std::wstring& source, std::wstring* out_copy) {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring dir = std::wstring(temp) + L"openslinky";
    CreateDirectoryW(dir.c_str(), nullptr);

    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\libslinky_*.dll").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            DeleteFileW((dir + L"\\" + fd.cFileName).c_str()); // fails silently while still loaded
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }

    *out_copy = dir + L"\\libslinky_" + std::to_wstring(GetTickCount64()) + L".dll";
    return CopyFileW(source.c_str(), out_copy->c_str(), FALSE) != FALSE;
}

bool inject(DWORD pid, const std::wstring& dll_path, std::wstring* out_error) {
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                     PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (process == nullptr) {
        *out_error = L"Could not open the process: " + error_text(GetLastError());
        return false;
    }

    BOOL target_wow64 = FALSE;
    BOOL self_wow64 = FALSE;
    IsWow64Process(process, &target_wow64);
    IsWow64Process(GetCurrentProcess(), &self_wow64);
    if (target_wow64 != self_wow64) {
        *out_error = L"Architecture mismatch: the game is 32-bit, but the injector and DLL are 64-bit.";
        CloseHandle(process);
        return false;
    }

    const SIZE_T bytes = (dll_path.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote == nullptr) {
        *out_error = L"VirtualAllocEx failed: " + error_text(GetLastError());
        CloseHandle(process);
        return false;
    }

    bool ok = false;
    if (WriteProcessMemory(process, remote, dll_path.c_str(), bytes, nullptr) == FALSE) {
        *out_error = L"WriteProcessMemory failed: " + error_text(GetLastError());
    } else {
        auto* load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW")));
        HANDLE thread = CreateRemoteThread(process, nullptr, 0, load_library, remote, 0, nullptr);
        if (thread == nullptr) {
            *out_error = L"CreateRemoteThread failed: " + error_text(GetLastError());
        } else {
            if (WaitForSingleObject(thread, 15000) != WAIT_OBJECT_0) {
                *out_error = L"Timed out waiting for LoadLibrary in the game.";
            } else {
                DWORD module_low = 0;
                GetExitCodeThread(thread, &module_low);
                ok = module_low != 0;
                if (!ok) {
                    *out_error = L"LoadLibrary failed inside the game (missing dependency or wrong file?).";
                }
            }
            CloseHandle(thread);
        }
    }

    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    return ok;
}

void on_inject() {
    const LRESULT sel = SendMessageW(g_combo, CB_GETCURSEL, 0, 0);
    if (sel < 0 || static_cast<size_t>(sel) >= g_processes.size()) {
        set_status(L"Select a Minecraft process first.");
        return;
    }
    const DWORD pid = g_processes[static_cast<size_t>(sel)].pid;

    wchar_t raw[MAX_PATH]{};
    GetWindowTextW(g_dll_edit, raw, MAX_PATH);
    wchar_t full[MAX_PATH]{};
    GetFullPathNameW(raw, MAX_PATH, full, nullptr);
    std::wstring dll = full;
    if (!file_exists(dll)) {
        set_status(L"DLL not found: " + dll);
        return;
    }

    if (SendMessageW(g_copy_check, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        std::wstring copy;
        if (!make_shadow_copy(dll, &copy)) {
            set_status(L"Could not copy the DLL to %TEMP%: " + error_text(GetLastError()));
            return;
        }
        dll = copy;
    }

    set_status(L"Injecting into PID " + std::to_wstring(pid) + L"...");
    UpdateWindow(g_status);
    std::wstring error;
    if (inject(pid, dll, &error)) {
        set_status(L"Injected into PID " + std::to_wstring(pid) + L".");
    } else {
        set_status(error);
    }
}

void on_browse() {
    wchar_t path[MAX_PATH]{};
    GetWindowTextW(g_dll_edit, path, MAX_PATH);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_wnd;
    ofn.lpstrFilter = L"DLL files (*.dll)\0*.dll\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn) != FALSE) {
        SetWindowTextW(g_dll_edit, path);
    }
}

HWND add_control(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    HWND ctl = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(ctl, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return ctl;
}

void create_controls() {
    constexpr int kMargin = 12;
    constexpr int kLabelW = 72;
    constexpr int kWidth = 520;
    const int field_x = kMargin + kLabelW;
    const int field_w = kWidth - field_x - kMargin - 90;

    add_control(L"STATIC", L"Minecraft:", SS_LEFT, kMargin, kMargin + 4, kLabelW, 20, 0);
    g_combo = add_control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                          field_x, kMargin, field_w, 300, kIdProcessCombo);
    add_control(L"BUTTON", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, field_x + field_w + 8, kMargin - 1, 82, 26, kIdRefresh);

    add_control(L"STATIC", L"DLL:", SS_LEFT, kMargin, kMargin + 42, kLabelW, 20, 0);
    g_dll_edit = add_control(L"EDIT", default_dll_path().c_str(), ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                             field_x, kMargin + 38, field_w, 24, kIdDllEdit);
    add_control(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, field_x + field_w + 8, kMargin + 37, 82, 26, kIdBrowse);

    g_copy_check = add_control(L"BUTTON", L"Inject a copy (keeps build\\libslinky.dll writable while the game runs)",
                               BS_AUTOCHECKBOX | WS_TABSTOP, field_x, kMargin + 72, kWidth - field_x - kMargin, 22, kIdCopyCheck);
    SendMessageW(g_copy_check, BM_SETCHECK, BST_CHECKED, 0);

    add_control(L"BUTTON", L"Inject", BS_DEFPUSHBUTTON | WS_TABSTOP, field_x, kMargin + 104, 140, 34, kIdInject);
    g_status = add_control(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, kMargin, kMargin + 150,
                           kWidth - 2 * kMargin, 40, kIdStatus);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_CREATE:
            g_wnd = hwnd;
            create_controls();
            refresh_processes();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case kIdRefresh: refresh_processes(); return 0;
                case kIdBrowse: on_browse(); return 0;
                case kIdInject: on_inject(); return 0;
                default: break;
            }
            break;
        case WM_CTLCOLORSTATIC: {
            // Transparent label backgrounds on the window colour.
            SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"OpenSlinkyInjector";
    RegisterClassExW(&wc);

    RECT rc{0, 0, 520, 210};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rc, style, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"OpenSlinky Injector", style, CW_USEDEFAULT, CW_USEDEFAULT,
                                rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, instance, nullptr);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DeleteObject(g_font);
    return static_cast<int>(msg.wParam);
}
