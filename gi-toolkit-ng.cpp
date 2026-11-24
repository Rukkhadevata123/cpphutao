// MIT License, Copyright (c) 2025 Rukkhadevata123. See LICENSE.
// C++23 / Windows x64.
// Build on Linux: ./build.sh
// Build on Windows: ./build.ps1
// Place config.ini and hutao-minhook-ng.dll beside the executable.
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <wchar.h> // _wcsicmp

import std;

namespace launcher
{
    namespace fs = std::filesystem;

    constexpr DWORD loader_thread_timeout_ms = 30000;
    constexpr DWORD injection_thread_timeout_ms = 30000;
    constexpr unsigned module_snapshot_attempts = 20;
    constexpr DWORD module_snapshot_retry_ms = 10;

    class UniqueHandle
    {
        HANDLE value_ = nullptr;

      public:
        explicit UniqueHandle(HANDLE value = nullptr) noexcept : value_(value) {}
        ~UniqueHandle()
        {
            if (value_ && value_ != INVALID_HANDLE_VALUE)
                CloseHandle(value_);
        }
        UniqueHandle(const UniqueHandle &) = delete;
        UniqueHandle &operator=(const UniqueHandle &) = delete;
        UniqueHandle(UniqueHandle &&other) noexcept : value_(std::exchange(other.value_, nullptr)) {}

        HANDLE get() const noexcept
        {
            return value_;
        }
    };

    std::wstring system_error(DWORD code)
    {
        wchar_t *buffer = nullptr;
        const DWORD size =
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           nullptr,
                           code,
                           0,
                           reinterpret_cast<wchar_t *>(&buffer),
                           0,
                           nullptr);
        std::wstring result = L"Windows error " + std::to_wstring(code);
        if (size)
        {
            result += L": ";
            result.append(buffer, size);
            LocalFree(buffer);
        }
        return result;
    }

    class LauncherError : public std::runtime_error
    {
      public:
        std::wstring message;
        explicit LauncherError(std::wstring text) : std::runtime_error("launcher error"), message(std::move(text)) {}
    };

    [[noreturn]] void fail(const std::wstring &operation, DWORD code = GetLastError())
    {
        throw LauncherError(operation + L"\n" + system_error(code));
    }

    fs::path executable_path()
    {
        std::wstring buffer(512, L'\0');
        for (;;)
        {
            const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (!size)
                fail(L"GetModuleFileNameW failed.");
            if (size < buffer.size())
            {
                buffer.resize(size);
                return buffer;
            }
            if (buffer.size() >= 32768)
                throw LauncherError(L"Executable path is too long.");
            buffer.resize(buffer.size() * 2);
        }
    }

    [[nodiscard]] std::expected<fs::path, std::wstring> read_game_path(const fs::path &config)
    {
        std::error_code error;
        const auto status = fs::status(config, error);
        if (error && error != std::errc::no_such_file_or_directory)
        {
            return std::unexpected(std::format(L"Could not access {} (error {}).", config.native(), error.value()));
        }
        if (!fs::is_regular_file(status))
        {
            return std::unexpected(L"Config not found: " + config.wstring());
        }

        std::wstring value(32768, L'\0');
        const DWORD length = GetPrivateProfileStringW(
            L"Settings", L"GamePath", L"", value.data(), static_cast<DWORD>(value.size()), config.c_str());
        if (length == value.size() - 1)
        {
            return std::unexpected(L"GamePath is too long.");
        }
        if (length == 0)
        {
            return std::unexpected(L"GamePath not found in config.ini or is empty.");
        }

        value.resize(length);
        return fs::path(std::move(value));
    }

    void require_native_x64(HANDLE process)
    {
        USHORT process_machine = 0;
        USHORT native_machine = 0;
        if (!IsWow64Process2(process, &process_machine, &native_machine))
            fail(L"Could not determine target architecture.");
        if (process_machine != IMAGE_FILE_MACHINE_UNKNOWN || native_machine != IMAGE_FILE_MACHINE_AMD64)
            throw LauncherError(L"The injector and target must both be native x64 programs.");
    }

    void initialize_loader(HANDLE process)
    {
        // A brand-new suspended process has no initialized PEB module list yet.
        // Its ntdll image is already mapped. Locate it without relying on the PEB
        // or equal ASLR bases, then let a no-op thread run loader initialization.
        const HMODULE local_ntdll = GetModuleHandleW(L"ntdll.dll");
        const FARPROC exit_thread = local_ntdll ? GetProcAddress(local_ntdll, "RtlExitUserThread") : nullptr;
        if (!exit_thread)
            fail(L"RtlExitUserThread was not found.");
        std::uintptr_t remote_ntdll_base = 0;
        std::uintptr_t address = 0;
        MEMORY_BASIC_INFORMATION memory{};
        while (VirtualQueryEx(process, reinterpret_cast<void *>(address), &memory, sizeof(memory)))
        {
            const auto next = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
            if (next <= address)
                throw LauncherError(L"Invalid target address space.");

            const bool image_start = memory.Type == MEM_IMAGE && memory.AllocationBase == memory.BaseAddress;
            if (image_start)
            {
                std::wstring path(32768, L'\0');
                const DWORD length =
                    K32GetMappedFileNameW(process, memory.AllocationBase, path.data(), static_cast<DWORD>(path.size()));
                if (!length)
                    fail(L"Could not identify a target image mapping.");
                path.resize(length);
                if (_wcsicmp(fs::path(path).filename().c_str(), L"ntdll.dll") == 0)
                {
                    remote_ntdll_base = reinterpret_cast<std::uintptr_t>(memory.AllocationBase);
                    break;
                }
            }
            address = next;
        }
        if (!remote_ntdll_base)
            throw LauncherError(L"ntdll.dll was not found in the suspended target.");

        const auto exit_thread_rva =
            reinterpret_cast<std::uintptr_t>(exit_thread) - reinterpret_cast<std::uintptr_t>(local_ntdll);
        const auto remote_exit_thread = reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_ntdll_base + exit_thread_rva);
        UniqueHandle thread(CreateRemoteThread(process, nullptr, 0, remote_exit_thread, nullptr, 0, nullptr));
        if (!thread.get())
            fail(L"Could not initialize the suspended target's loader.");
        const DWORD wait = WaitForSingleObject(thread.get(), loader_thread_timeout_ms);
        if (wait != WAIT_OBJECT_0)
            fail(L"Target loader initialization failed or timed out.",
                 wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
        DWORD exit_code = 0;
        if (!GetExitCodeThread(thread.get(), &exit_code))
            fail(L"Could not read the loader initialization result.");
        if (exit_code != 0)
            throw LauncherError(L"Target loader initialization failed: " + std::to_wstring(exit_code));
    }

    std::uintptr_t find_remote_module_base(DWORD process_id, const std::wstring &name)
    {
        for (unsigned attempt = 0; attempt < module_snapshot_attempts; ++attempt)
        {
            UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id));
            if (snapshot.get() == INVALID_HANDLE_VALUE)
            {
                if (GetLastError() == ERROR_BAD_LENGTH)
                {
                    Sleep(module_snapshot_retry_ms);
                    continue;
                }
                fail(L"Could not enumerate target modules.");
            }

            MODULEENTRY32W module{};
            module.dwSize = sizeof(module);
            if (!Module32FirstW(snapshot.get(), &module))
                fail(L"Could not read target module list.");
            do
            {
                if (_wcsicmp(module.szModule, name.c_str()) == 0)
                    return reinterpret_cast<std::uintptr_t>(module.modBaseAddr);
            } while (Module32NextW(snapshot.get(), &module));
            if (GetLastError() != ERROR_NO_MORE_FILES)
                fail(L"Could not finish target module enumeration.");
            return 0;
        }
        throw LauncherError(L"Target module list remained unstable.");
    }

    LPTHREAD_START_ROUTINE resolve_remote_load_library(DWORD process_id)
    {
        const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (!kernel)
            fail(L"kernel32.dll is not loaded.");
        const FARPROC load_library = GetProcAddress(kernel, "LoadLibraryW");
        if (!load_library)
            fail(L"LoadLibraryW was not found.");

        HMODULE owner = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(load_library),
                                &owner))
            fail(L"Could not locate the module implementing LoadLibraryW.");
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(owner, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size())
            fail(L"Could not read the loader module path.");
        path.resize(length);
        const auto remote_base = find_remote_module_base(process_id, fs::path(path).filename().wstring());
        if (!remote_base)
            throw LauncherError(L"The loader module is not present in the target process.");

        // LoadLibraryW may be forwarded to KernelBase; never assume equal ASLR bases.
        const auto rva = reinterpret_cast<std::uintptr_t>(load_library) - reinterpret_cast<std::uintptr_t>(owner);
        return reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_base + rva);
    }

    void inject_dll(HANDLE process, DWORD process_id, const fs::path &dll)
    {
        const auto load_library = resolve_remote_load_library(process_id);
        const auto dll_path = fs::absolute(dll).wstring();
        const SIZE_T path_bytes = (dll_path.size() + 1) * sizeof(wchar_t);
        void *remote_path = VirtualAllocEx(process, nullptr, path_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!remote_path)
            fail(L"VirtualAllocEx failed.");

        // On a wait failure the thread may still use the path. The caller terminates
        // the suspended child, which also reclaims this allocation.
        SIZE_T bytes_written = 0;
        const BOOL path_written =
            WriteProcessMemory(process, remote_path, dll_path.c_str(), path_bytes, &bytes_written);
        if (!path_written || bytes_written != path_bytes)
        {
            const DWORD error = path_written ? ERROR_WRITE_FAULT : GetLastError();
            VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
            fail(L"Could not write the DLL path.", error);
        }

        UniqueHandle thread(CreateRemoteThread(process, nullptr, 0, load_library, remote_path, 0, nullptr));
        if (!thread.get())
        {
            const DWORD error = GetLastError();
            VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
            fail(L"CreateRemoteThread failed.", error);
        }
        const DWORD wait = WaitForSingleObject(thread.get(), injection_thread_timeout_ms);
        if (wait != WAIT_OBJECT_0)
        {
            fail(L"Waiting for LoadLibraryW failed or timed out.",
                 wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
        }

        DWORD exit_code = 0;
        if (!GetExitCodeThread(thread.get(), &exit_code))
            fail(L"Could not read the loader result.");
        if (!VirtualFreeEx(process, remote_path, 0, MEM_RELEASE))
            fail(L"Could not release the remote DLL path.");

        // A module handle is 64-bit, but a thread exit code is only 32-bit.
        // Module enumeration, not a truncated handle, establishes successful loading.
        if (!find_remote_module_base(process_id, dll.filename().wstring()))
            throw LauncherError(L"LoadLibraryW did not load the DLL (thread result " + std::to_wstring(exit_code) +
                                L"): " + dll_path);
    }

    UniqueHandle launch_game(const fs::path &game, const fs::path &dll)
    {
        const auto executable = fs::absolute(game);
        if (!fs::is_regular_file(executable))
            throw LauncherError(L"Game executable not found: " + executable.wstring());
        if (!fs::is_regular_file(dll))
            throw LauncherError(L"DLL not found: " + dll.wstring());

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(executable.c_str(),
                            nullptr,
                            nullptr,
                            nullptr,
                            FALSE,
                            CREATE_SUSPENDED,
                            nullptr,
                            executable.parent_path().c_str(),
                            &startup,
                            &info))
            fail(L"Could not start the game.");
        UniqueHandle process(info.hProcess);
        UniqueHandle main_thread(info.hThread);

        try
        {
            require_native_x64(process.get());
            initialize_loader(process.get());
            inject_dll(process.get(), info.dwProcessId, dll);

            if (ResumeThread(main_thread.get()) == static_cast<DWORD>(-1))
                fail(L"Could not resume the game.");
        }
        catch (...)
        {
            // Rollback only; rethrow the original error, never disguise injection failure.
            if (!TerminateProcess(process.get(), 1))
                OutputDebugStringW(L"gi-toolkit-ng: failed to terminate the unsuccessful child process.\n");
            else
                WaitForSingleObject(process.get(), 5000);
            throw;
        }
        return process;
    }

} // namespace launcher

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    using namespace launcher;
    try
    {
        const auto directory = executable_path().parent_path();
        const auto game = read_game_path(directory / L"config.ini");
        if (!game)
        {
            MessageBoxW(nullptr, game.error().c_str(), L"gi-toolkit-ng", MB_OK | MB_ICONERROR);
            return 1;
        }

        launch_game(*game, directory / L"hutao-minhook-ng.dll");
        return 0;
    }
    catch (const LauncherError &error)
    {
        MessageBoxW(nullptr, error.message.c_str(), L"gi-toolkit-ng", MB_OK | MB_ICONERROR);
    }
    catch (const std::exception &error)
    {
        MessageBoxA(nullptr, error.what(), "gi-toolkit-ng", MB_OK | MB_ICONERROR);
    }
    return 1;
}
