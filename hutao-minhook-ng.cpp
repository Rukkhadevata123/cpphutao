// MIT License, Copyright (c) 2025 Rukkhadevata123. See LICENSE.
// C++23 / Windows x64. MinHook's separate license is in MINHOOK-LICENSE.txt.
// Build on Linux: ./build.sh
// Build on Windows: ./build.ps1
//
// config.ini, offset.ini and hutao_minhook_ng.log are relative to this DLL.
// Offsets use "<major>.<minor> Offsets" sections and RVA keys.
// Remove stale offset.ini after updating game signatures.
// HOME reloads settings, not the startup feature inventory. DisableHotReload=1
// limits that inventory to features initially enabled. Touch input and HDR are
// initialized once.
// Malformed reloads are logged and leave the previous configuration intact.
// Game-call memory faults are SEH-protected and logged before applying failure defaults.
// The DLL pins itself before installing hooks: unloading live game callbacks is
// unsafe. Restart the process to replace the DLL; no loader-lock hook teardown.
#include <windows.h>
#include <share.h>
#include <stddef.h> // offsetof
#include <stdio.h>  // _wfsopen

import std;

// MinHook v1.3.4's C ABI. Keeping these declarations here avoids a third source
// file or a dependency on the temporary checkout at application compile time.
enum MH_STATUS
{
    MH_UNKNOWN = -1,
    MH_OK = 0,
    MH_ERROR_ALREADY_INITIALIZED,
    MH_ERROR_NOT_INITIALIZED,
    MH_ERROR_ALREADY_CREATED,
    MH_ERROR_NOT_CREATED,
    MH_ERROR_ENABLED,
    MH_ERROR_DISABLED,
    MH_ERROR_NOT_EXECUTABLE,
    MH_ERROR_UNSUPPORTED_FUNCTION,
    MH_ERROR_MEMORY_ALLOC,
    MH_ERROR_MEMORY_PROTECT,
    MH_ERROR_MODULE_NOT_FOUND,
    MH_ERROR_FUNCTION_NOT_FOUND
};
extern "C"
{
    MH_STATUS WINAPI MH_Initialize();
    MH_STATUS WINAPI MH_Uninitialize();
    MH_STATUS WINAPI MH_CreateHook(void *, void *, void **);
    MH_STATUS WINAPI MH_EnableHook(void *);
    MH_STATUS WINAPI MH_DisableHook(void *);
    const char *WINAPI MH_StatusToString(MH_STATUS);
}

namespace hutao
{
    namespace fs = std::filesystem;

    struct Config
    {
        bool enable_fps_override = false;
        int selected_fps = 60;
        bool enable_fov_override = false;
        float fov_value = 60.0f;

        bool enable_hdr = false;
        bool use_touch_screen = false;
        bool disable_hot_reload = false;
        bool debug_mode = false;

        bool remove_fog = false;
        bool remove_blur = false;
        bool remove_dive_mosaic = false;
        bool hide_quest_banner = false;
        bool hide_uid = false;
        bool disable_event_camera_move = false;
        bool disable_show_damage_text = false;

        bool redirect_crafting = false;
        bool remove_team_animation = false;

        int toggle_key = VK_HOME;
        int craft_key = 0;
    };

    SRWLOCK config_lock = SRWLOCK_INIT;
    std::mutex log_mutex;
    Config current_config;
    fs::path config_path;
    fs::path offset_path;
    fs::path log_path;
    std::string offset_section = "Offsets";

    Config get_config()
    {
        AcquireSRWLockShared(&config_lock);
        const Config config = current_config;
        ReleaseSRWLockShared(&config_lock);
        return config;
    }

    void publish_config(const Config &config)
    {
        AcquireSRWLockExclusive(&config_lock);
        current_config = config;
        ReleaseSRWLockExclusive(&config_lock);
    }

    void log_line(std::string text)
    {
        if (text.empty() || text.back() != '\n')
        {
            text += '\n';
        }
        OutputDebugStringA(text.c_str());
        if (log_path.empty())
        {
            return;
        }

        text.pop_back();
        const std::lock_guard lock(log_mutex);
        using File = std::unique_ptr<std::FILE, decltype(&std::fclose)>;
        const File file(_wfsopen(log_path.c_str(), L"ab", _SH_DENYWR), &std::fclose);
        if (!file)
        {
            OutputDebugStringA("hutao: could not open the log file.\n");
            return;
        }

        try
        {
            std::println(file.get(), "{}", text);
            if (std::fflush(file.get()) != 0)
            {
                OutputDebugStringA("hutao: could not write the log file.\n");
            }
        }
        catch (const std::system_error &)
        {
            OutputDebugStringA("hutao: could not write the log file.\n");
        }
    }

    template <class... Args> void log(std::format_string<Args...> format, Args &&...args)
    {
        log_line(std::format(format, std::forward<Args>(args)...));
    }

    template <class... Args> void debug(std::format_string<Args...> format, Args &&...args)
    {
        if (get_config().debug_mode)
            log(format, std::forward<Args>(args)...);
    }

    fs::path module_path(HMODULE module)
    {
        std::wstring buffer(512, L'\0');
        for (;;)
        {
            const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (!length)
                throw std::runtime_error(std::format("GetModuleFileNameW: {}", GetLastError()));
            if (length < buffer.size())
            {
                buffer.resize(length);
                return buffer;
            }
            if (buffer.size() >= 32768)
                throw std::runtime_error("DLL path exceeds the Windows path limit");
            buffer.resize(buffer.size() * 2);
        }
    }

    std::string_view trim(std::string_view text)
    {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos)
            return {};
        return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    }

    std::string narrow_ascii(std::wstring_view value)
    {
        std::string result;
        result.reserve(value.size());
        for (const wchar_t ch : value)
        {
            if (ch > 127)
                throw std::runtime_error("Expected an ASCII numeric setting");
            result += static_cast<char>(ch);
        }
        return result;
    }

    std::wstring ini_string(const fs::path &path, const wchar_t *section, const wchar_t *key, const wchar_t *fallback)
    {
        std::wstring result(32768, L'\0');
        const DWORD length = GetPrivateProfileStringW(
            section, key, fallback, result.data(), static_cast<DWORD>(result.size()), path.c_str());
        if (length >= result.size() - 1)
            throw std::runtime_error("INI value is too long");
        result.resize(length);
        return result;
    }

    int ini_int(const fs::path &path, const wchar_t *section, const wchar_t *key, int fallback = 0)
    {
        const auto default_value = std::to_wstring(fallback);
        const auto value = narrow_ascii(ini_string(path, section, key, default_value.c_str()));
        auto token = trim(value);
        bool negative = false;
        if (!token.empty() && (token.front() == '+' || token.front() == '-'))
        {
            negative = token.front() == '-';
            token.remove_prefix(1);
        }
        int base = 10;
        if (token.starts_with("0x") || token.starts_with("0X"))
        {
            base = 16;
            token.remove_prefix(2);
        }
        if (token.empty())
            throw std::runtime_error("Empty integer setting: " + narrow_ascii(key));

        std::uint64_t magnitude = 0;
        const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), magnitude, base);
        const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<int>::max()) + (negative ? 1 : 0);
        if (error != std::errc{} || end != token.data() + token.size() || magnitude > maximum)
            throw std::runtime_error("Invalid integer setting: " + narrow_ascii(key));

        return static_cast<int>(negative ? -static_cast<std::int64_t>(magnitude)
                                         : static_cast<std::int64_t>(magnitude));
    }

    float read_target_fov(const fs::path &path)
    {
        const auto setting = narrow_ascii(ini_string(path, L"Settings", L"TargetFOV", L"60.0"));
        auto token = trim(setting);
        if (token.starts_with('+'))
        {
            token.remove_prefix(1);
        }
        if (token.empty())
        {
            throw std::runtime_error("TargetFOV must not be empty");
        }

        float value{};
        const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
        if (error != std::errc{} || end != token.data() + token.size() || !std::isfinite(value) || value <= 0)
        {
            throw std::runtime_error("TargetFOV must be a positive finite number");
        }
        return value;
    }

    Config read_config(const fs::path &path)
    {
        if (!fs::is_regular_file(path))
            throw std::runtime_error("config.ini is missing beside the DLL");

        Config result;
        result.enable_fps_override = ini_int(path, L"Settings", L"EnableFPSUnlock") != 0;
        result.selected_fps = ini_int(path, L"Settings", L"TargetFPS", 60);
        result.enable_fov_override = ini_int(path, L"Settings", L"EnableFOVModify") != 0;
        result.enable_hdr = ini_int(path, L"Settings", L"EnableHDR") != 0;
        result.use_touch_screen = ini_int(path, L"Settings", L"UseTouchScreen") != 0;
        result.disable_hot_reload = ini_int(path, L"Settings", L"DisableHotReload") != 0;
        result.debug_mode = ini_int(path, L"Settings", L"DebugMode") != 0;

        result.fov_value = read_target_fov(path);
        if (result.selected_fps <= 0)
            throw std::runtime_error("TargetFPS must be positive");

        result.remove_fog = ini_int(path, L"Visuals", L"RemoveFog") != 0;
        result.remove_blur = ini_int(path, L"Visuals", L"RemoveBlur") != 0;
        result.remove_dive_mosaic = ini_int(path, L"Visuals", L"RemoveDiveMosaic") != 0;
        result.hide_quest_banner = ini_int(path, L"Visuals", L"HideQuestBanner") != 0;
        result.hide_uid = ini_int(path, L"Visuals", L"HideUID") != 0;
        result.disable_event_camera_move = ini_int(path, L"Visuals", L"DisableEventCameraMove") != 0;
        result.disable_show_damage_text = ini_int(path, L"Visuals", L"DisableShowDamageText") != 0;

        result.redirect_crafting = ini_int(path, L"Features", L"RedirectCrafting") != 0;
        result.remove_team_animation = ini_int(path, L"Features", L"RemoveTeamAnimation") != 0;

        result.toggle_key = ini_int(path, L"Hotkeys", L"ToggleKey", VK_HOME);
        result.craft_key = ini_int(path, L"Hotkeys", L"CraftKey");
        if (!std::in_range<std::uint8_t>(result.toggle_key) || !std::in_range<std::uint8_t>(result.craft_key))
        {
            throw std::runtime_error("Hotkeys must be virtual-key codes in 0..255");
        }

        return result;
    }

    std::string read_text(const fs::path &path, bool optional = false)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            if (optional && !fs::exists(path))
                return {};
            throw std::runtime_error("Could not read " + path.filename().string());
        }
        std::string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if (file.bad())
            throw std::runtime_error("I/O error reading " + path.filename().string());
        return contents;
    }

    std::string game_version(std::string_view contents)
    {
        while (!contents.empty())
        {
            const auto newline = contents.find('\n');
            const auto line = trim(contents.substr(0, newline));
            if (newline == std::string_view::npos)
                contents = {};
            else
                contents.remove_prefix(newline + 1);
            if (line.empty() || line.front() == ';' || line.front() == '#')
                continue;
            const auto equal = line.find('=');
            if (equal == std::string_view::npos)
                continue;
            std::string key(trim(line.substr(0, equal)));
            std::ranges::transform(key, key.begin(), [](unsigned char ch) {
                return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : static_cast<char>(ch);
            });
            if (key != "game_version")
                continue;
            auto value = trim(line.substr(equal + 1));
            while (!value.empty() && value.front() == '"')
                value.remove_prefix(1);
            while (!value.empty() && value.back() == '"')
                value.remove_suffix(1);
            const auto first = value.find('.');
            if (first == std::string_view::npos || first == 0)
                return std::string(value);
            const auto second = value.find('.', first + 1);
            if (second == first + 1 || first + 1 == value.size())
                return std::string(value);
            return std::string(value.substr(0, second));
        }
        return {};
    }

    void refresh_offset_section()
    {
        const auto game = ini_string(config_path, L"Settings", L"GamePath", L"");
        std::string version;
        if (!game.empty())
            version = game_version(read_text(fs::path(game).parent_path() / L"config.ini", true));
        offset_section = version.empty() ? "Offsets" : version + " Offsets";
    }

    std::optional<std::string_view> section_name(std::string_view line)
    {
        line = trim(line);
        if (line.size() >= 2 && line.front() == '[' && line.back() == ']')
            return line.substr(1, line.size() - 2);
        return std::nullopt;
    }

    std::optional<std::string> offset_value(std::string_view contents, std::string_view section, std::string_view key)
    {
        bool selected = false;
        while (!contents.empty())
        {
            const auto newline = contents.find('\n');
            const auto line = trim(contents.substr(0, newline));
            if (newline == std::string_view::npos)
                contents = {};
            else
                contents.remove_prefix(newline + 1);
            if (auto name = section_name(line))
            {
                selected = *name == section;
                continue;
            }
            if (!selected || line.empty() || line.front() == ';' || line.front() == '#')
                continue;
            const auto equal = line.find('=');
            if (equal != std::string_view::npos && trim(line.substr(0, equal)) == key)
                return std::string(trim(line.substr(equal + 1)));
        }
        return std::nullopt;
    }

    std::string upsert_offset(std::string_view contents,
                              std::string_view section,
                              std::string_view key,
                              std::string_view value)
    {
        const auto newline = contents.find('\n');
        const bool uses_crlf = newline == std::string_view::npos || (newline > 0 && contents[newline - 1] == '\r');
        const std::string line_ending = uses_crlf ? "\r\n" : "\n";
        const bool has_trailing_newline = contents.empty() || contents.back() == '\n' || contents.back() == '\r';

        std::vector<std::string> lines;
        std::istringstream input{std::string(contents)};
        for (std::string line; std::getline(input, line);)
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines.push_back(std::move(line));
        }

        std::optional<std::size_t> start;
        std::size_t finish = lines.size();
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            const auto name = section_name(lines[i]);
            if (!name)
                continue;
            if (!start)
            {
                if (*name == section)
                    start = i;
            }
            else
            {
                finish = i;
                break;
            }
        }

        const auto assignment = std::format("{}={}", key, value);
        bool replaced = false;
        if (start)
        {
            for (auto i = *start + 1; i < finish; ++i)
            {
                const auto line = trim(lines[i]);
                if (line.empty() || line.front() == ';' || line.front() == '#')
                    continue;
                const auto equal = line.find('=');
                if (equal != std::string_view::npos && trim(line.substr(0, equal)) == key)
                {
                    lines[i] = assignment;
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
            {
                while (finish > *start + 1 && trim(lines[finish - 1]).empty())
                    --finish;
                lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(finish), assignment);
            }
        }
        else
        {
            std::vector<std::string> prefix{std::format("[{}]", section), assignment};
            if (!lines.empty())
                prefix.emplace_back();
            lines.insert(lines.begin(), prefix.begin(), prefix.end());
        }

        std::string result;
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            if (i)
                result += line_ending;
            result += lines[i];
        }
        if (has_trailing_newline)
            result += line_ending;
        return result;
    }

    bool reload_config()
    {
        try
        {
            const auto replacement = read_config(config_path);
            refresh_offset_section();
            publish_config(replacement);
            return true;
        }
        catch (const std::exception &error)
        {
            log("config: {} (keeping previous settings)", error.what());
            return false;
        }
    }

    int seh_filter(DWORD code)
    {
        return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER
                                                                                     : EXCEPTION_CONTINUE_SEARCH;
    }

    bool seh_execute(void (*action)(void *), void *context, DWORD *error)
    {
        __try
        {
            action(context);
            return true;
        }
        __except (seh_filter(GetExceptionCode()))
        {
            *error = GetExceptionCode();
            return false;
        }
    }

    template <class F> bool guarded(std::string_view operation, F &&action)
    {
        DWORD error = 0;
        const auto execute = [](void *context) {
            (*static_cast<std::remove_reference_t<F> *>(context))();
        };
        if (seh_execute(execute, &action, &error))
            return true;
        log("{}: memory exception 0x{:08x}", operation, error);
        return false;
    }

    struct ImageRange
    {
        std::uintptr_t base = 0;
        std::size_t size = 0;
        bool contains(std::uintptr_t address, std::size_t length = 1) const
        {
            return length <= size && address >= base && address - base <= size - length;
        }
    };

    ImageRange main_image()
    {
        ImageRange result;
        guarded("PE headers", [&] {
            const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            if (!base)
                return;

            const auto dos_header = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
            if (dos_header->e_magic != IMAGE_DOS_SIGNATURE || dos_header->e_lfanew < 0)
                return;

            const auto nt_headers = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos_header->e_lfanew);
            if (nt_headers->Signature != IMAGE_NT_SIGNATURE ||
                nt_headers->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
                return;

            result = {base, nt_headers->OptionalHeader.SizeOfImage};
        });
        if (!result.size)
            throw std::runtime_error("Could not determine the main executable's PE image range");
        return result;
    }

    bool readable(DWORD protection)
    {
        if (protection & (PAGE_GUARD | PAGE_NOACCESS))
            return false;
        const DWORD base = protection & 0xff;
        return base == PAGE_READONLY || base == PAGE_READWRITE || base == PAGE_WRITECOPY || base == PAGE_EXECUTE_READ ||
               base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
    }

    bool executable_address(std::uintptr_t address, const ImageRange &image)
    {
        if (!image.contains(address))
            return false;
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(reinterpret_cast<void *>(address), &memory, sizeof(memory)) || memory.State != MEM_COMMIT ||
            (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;
        const DWORD protection = memory.Protect & 0xff;
        return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE ||
               protection == PAGE_EXECUTE_WRITECOPY;
    }

    struct Pattern
    {
        std::vector<int> bytes;
        std::size_t anchor = 0;
        bool has_anchor = false;
    };

    constexpr std::size_t scan_match_limit = 10;
    constexpr std::size_t rel32_instruction_size = 5;

    Pattern parse_pattern(std::string_view text)
    {
        Pattern result;
        std::istringstream stream{std::string(text)};
        for (std::string token; stream >> token;)
        {
            if (token == "?" || token == "??")
                result.bytes.push_back(-1);
            else
            {
                unsigned value = 0;
                const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value, 16);
                if (token.size() != 2 || error != std::errc{} || end != token.data() + token.size() || value > 255)
                    throw std::runtime_error("Invalid signature token: " + token);
                if (!result.has_anchor)
                {
                    result.anchor = result.bytes.size();
                    result.has_anchor = true;
                }
                result.bytes.push_back(static_cast<int>(value));
            }
        }
        if (result.bytes.empty())
            throw std::runtime_error("Empty signature");
        return result;
    }

    std::vector<std::uintptr_t> scan_region(const std::uint8_t *base,
                                            std::size_t size,
                                            const Pattern &pattern,
                                            std::size_t limit)
    {
        std::array<std::uintptr_t, scan_match_limit> matches{};
        std::size_t count = 0;
        limit = std::min(limit, matches.size());
        if (!limit || size < pattern.bytes.size())
            return {};

        const bool success = guarded("signature scan", [&] {
            const auto last_candidate = size - pattern.bytes.size();
            for (std::size_t candidate = 0; candidate <= last_candidate;)
            {
                if (pattern.has_anchor)
                {
                    const void *found = std::memchr(base + candidate + pattern.anchor,
                                                    pattern.bytes[pattern.anchor],
                                                    last_candidate - candidate + 1);
                    if (!found)
                        break;

                    candidate = static_cast<const std::uint8_t *>(found) - base - pattern.anchor;
                }

                bool matches_pattern = true;
                for (std::size_t i = 0; i < pattern.bytes.size(); ++i)
                {
                    if (pattern.bytes[i] >= 0 && base[candidate + i] != pattern.bytes[i])
                    {
                        matches_pattern = false;
                        break;
                    }
                }

                if (matches_pattern)
                {
                    matches[count++] = reinterpret_cast<std::uintptr_t>(base + candidate);
                    if (count == limit)
                        break;
                }
                ++candidate;
            }
        });
        if (!success)
            return {};
        return {matches.begin(), matches.begin() + static_cast<std::ptrdiff_t>(count)};
    }

    std::uintptr_t relative_address(std::uintptr_t instruction)
    {
        std::int32_t displacement = 0;
        if (!guarded("relative address", [&] {
                std::memcpy(&displacement, reinterpret_cast<const void *>(instruction + 1), sizeof(displacement));
            }))
            return 0;
        return instruction + rel32_instruction_size + static_cast<std::intptr_t>(displacement);
    }

    struct Region
    {
        std::uintptr_t base;
        std::size_t size;
    };

    std::vector<Region> image_regions(const ImageRange &image)
    {
        std::vector<Region> result;
        const auto end = image.base + image.size;
        auto address = image.base;
        while (address < end)
        {
            MEMORY_BASIC_INFORMATION memory{};
            if (!VirtualQuery(reinterpret_cast<void *>(address), &memory, sizeof(memory)))
                throw std::runtime_error(std::format("VirtualQuery: {}", GetLastError()));
            const auto next = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
            if (next <= address)
                throw std::runtime_error("Invalid memory region size");
            if (memory.State == MEM_COMMIT && readable(memory.Protect))
                result.push_back({address, std::min(next, end) - address});
            address = next;
        }
        return result;
    }

    enum class Function : std::size_t
    {
        GetFrameCount,
        SetFrameCount,
        ChangeFov,
        CameraBrainFlush,
        SwitchInputDevice,
        SetupQuestBanner,
        FindGameObject,
        SetActive,
        EventCameraMove,
        ShowDamageText,
        DisplayFog,
        PlayerPerspective,
        PlayerDiveMosaic,
        DisplayEffect,
        FindString,
        CraftEntryPartner,
        CraftEntry,
        CheckCanOpenMap,
        CheckCanEnter,
        OpenTeamPage,
        OpenTeam,
        Count
    };
    constexpr auto function_count = static_cast<std::size_t>(Function::Count);
    constexpr std::size_t index(Function function)
    {
        return static_cast<std::size_t>(function);
    }

    struct Signature
    {
        const char *name{};
        const char *pattern{};
        unsigned resolve_count = 0;
    };
    constexpr std::array<Signature, function_count> signatures{{
        {
            .name = "GetFrameCount",
            .pattern = "E8 ? ? ? ? 85 C0 7E 0E E8 ? ? ? ? 0F 57 C0 F3 0F 2A C0 EB 08",
            .resolve_count = 1,
        },
        {
            .name = "SetFrameCount",
            .pattern = "E8 ? ? ? ? E8 ? ? ? ? 83 F8 1F 0F 9C 05 ? ? ? ? 48 8B 05",
            .resolve_count = 1,
        },
        {
            .name = "ChangeFov",
            .pattern = "40 53 48 83 EC 60 0F 29 74 24 ? 48 8B D9 0F 28 F1 E8 ? ? ? ? "
                       "48 85 C0 0F 84 ? ? ? ? E8 ? ? ? ? 48 8B C8",
        },
        {
            .name = "CameraBrainFlush",
            .pattern = "56 57 48 83 EC 48 48 89 CE E8 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? "
                       "8B 4E 6C 89 4C 24 28 48 8B 4E 64 48 89 4C 24 20 48 8D 54 24 20 48 89 C1 E8",
        },
        {
            .name = "SwitchInputDevice",
            .pattern = "56 57 48 83 EC ? 48 89 CE 80 3D ? ? ? ? 00 48 8B 05 ? ? ? ? "
                       "0F 85 ? ? ? ? 48 8B ? ? ? ? ? 48 85 C9 0F 84 ? ? ? ? 48 8B 15 ? ? ? ? "
                       "E8 ? ? ? ? 48 89 C7 48 8B 05 ? ? ? ? 48 8B ? ? ? ? ? 48 85 C9 0F 84 ? ? ? ? "
                       "31 D2 41 B0 01 E8 ? ? ? ? 48 8B 05 ? ? ? ? 48 8B ? ? ? ? ? 48 85 C9 0F 84 ? ? ? ? "
                       "31 D2 45 31 C0 E8 ? ? ? ? 48 85 FF 74 ? 48 8B 05 ? ? ? ? 48 8B ? ? ? ? ? "
                       "48 85 C9 0F 84 ? ? ? ? E8 ? ? ? ? 80 3D ? ? ? ? 00 0F 85 ? ? ? ? "
                       "48 89 F9 89 C2 E8 ? ? ? ? 48 8B 05 ? ? ? ? 48 8B ? ? ? ? ? "
                       "48 85 C9 0F 84 ? ? ? ? E8 ? ? ? ? 48 8B ? A0 02 00 00",
        },
        {
            .name = "SetupQuestBanner",
            .pattern = "41 57 41 56 56 57 55 53 48 81 EC ? ? ? ? 0F 29 ? 24 ? ? ? ? "
                       "0F 29 ? 24 ? ? ? ? 48 89 CE 0F 57 C0 0F 29 ? 24",
        },
        {
            .name = "FindGameObject",
            .pattern = "E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? "
                       "E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? 48 83 EC ? C7 44 24 ? 00 00 00 00 48 8D 54 24",
            .resolve_count = 1,
        },
        {
            .name = "SetActive",
            .pattern = "E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? "
                       "E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? "
                       "E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? "
                       "E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? "
                       "E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? "
                       "E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? "
                       "E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? "
                       "E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? "
                       "E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? "
                       "E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? 45 31 C9",
            .resolve_count = 1,
        },
        {
            .name = "EventCameraMove",
            .pattern = "41 57 41 56 56 57 55 53 48 83 EC ? 48 89 D7 49 89 CE 80 3D ? ? ? ? 00 "
                       "0F 85 ? ? ? ? 80 3D ? ? ? ? 00 0F 85 ? ? ? ? 49 8B 96",
        },
        {
            .name = "ShowDamageText",
            .pattern = "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC ? ? ? ? "
                       "44 0F 29 ? 24 ? ? ? ? 44 0F 29 ? 24 ? ? ? ? 44 0F 29 ? 24 ? ? ? ? "
                       "44 0F 29 ? 24 ? ? ? ? 44 0F 29 ? 24 ? ? ? ? 44 0F 29 ? 24 ? ? ? ? "
                       "44 0F 29 ? 24 ? ? ? ? 44 0F 29 ? 24 ? ? ? ? "
                       "0F 29 ? 24 ? ? ? ? 0F 29 ? 24 ? ? ? ? 45 89 ? 89",
        },
        {
            .name = "DisplayFog",
            .pattern = "E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? "
                       "E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? "
                       "E9 ? ? ? ? 66 66 2E 0F 1F 84 00 ? ? ? ? C3 66 66 66 66 66 66 2E 0F 1F 84 00 ? ? ? ? "
                       "48 8B 41 ? C3 66 66 2E 0F 1F 84 00 ? ? ? ? 48 8B 41 ? "
                       "C3 66 66 2E 0F 1F 84 00 ? ? ? ? 8B 41",
        },
        {
            .name = "PlayerPerspective",
            .pattern = "41 56 56 57 55 53 48 83 EC 20 41 89 D0 48 89 CE 80 3D ? ? ? ? 00 "
                       "0F 85 ? ? ? ? 48 8B BE ? ? ? ? 48 85 FF 0F 84",
        },
        {
            .name = "PlayerDiveMosaic",
            .pattern = "41 57 41 56 56 57 53 48 81 EC ? ? ? ? 48 89 CE 80 3D ? ? ? ? ? "
                       "0F 85 ? ? ? ? 48 8B 86 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 8B 80 ? ? ? ? 48 83 F8",
        },
        {
            .name = "DisplayEffect",
            .pattern = "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC ? ? ? ? "
                       "0F 29 ? 24 ? ? ? ? 4C 89 CF 48 89 D6 49 89 CC 48 8B AC 24",
        },
        {
            .name = "FindString",
            .pattern = "56 48 83 ec 20 48 89 ce e8 ? ? ? ? 48 89 f1 89 c2 48 83 c4 20 5e e9 ? ? ? ? cc cc cc cc",
        },
        {
            .name = "CraftEntryPartner",
            .pattern = "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC ? ? ? ? 4D 89 ? 4C 89 C6 49 89 D4 49 89 CE",
        },
        {
            .name = "CraftEntry",
            .pattern = "41 56 56 57 53 48 83 EC ? 49 89 CE 80 3D ? ? ? ? 00 0F 84 ? ? ? ? "
                       "80 3D ? ? ? ? 00 48 8B 0D ? ? ? ? 0F 85 ? ? ? ? 48 8B 81 ? ? ? ? "
                       "48 85 C0 0F 84 ? ? ? ? 80 3D ? ? ? ? 00",
        },
        {
            .name = "CheckCanOpenMap",
            .pattern = "E8 ? ? ? ? 84 C0 0F 85 ? ? ? ? 48 8B 45 ? 48 85 C0 74 ? "
                       "41 8B 17 4C 8B 40 ? 48 8B 48 ? FF 50 ? 84 C0 0F 84",
            .resolve_count = 1,
        },
        {
            .name = "CheckCanEnter",
            .pattern = "56 48 81 EC ? ? ? ? 80 3D ? ? ? ? 00 0F 84 ? ? ? ? 80 3D ? ? ? ? 00 "
                       "48 8B ? ? ? ? ? 0F 85 ? ? ? ? 48 8B ? ? ? ? ? 48 85 C0 0F 84",
        },
        {
            .name = "OpenTeamPage",
            .pattern = "56 57 53 48 83 ec 20 89 cb 80 3d ? ? ? ? 00 74 7a 80 3d ? ? ? ? 00 48 8b 05",
        },
        {
            .name = "OpenTeam",
            .pattern = "48 83 EC ? 80 3D ? ? ? ? 00 75 ? 48 8B 0D ? ? ? ? 80 B9 C7 00 00 00 00 "
                       "0F 84 ? ? ? ? B9 0C 00 00 00",
        },
    }};

    std::string cache_key(std::string_view name)
    {
        std::string result;
        for (const char ch : name)
        {
            if (ch >= 'A' && ch <= 'Z')
            {
                if (!result.empty())
                    result += '_';
                result += static_cast<char>(ch + ('a' - 'A'));
            }
            else
                result += ch;
        }
        return result + "_addr";
    }

    class Scanner
    {
        ImageRange image_ = main_image();
        std::vector<Region> regions_ = image_regions(image_);
        std::string cache_ = read_text(offset_path, true);
        bool debug_ = get_config().debug_mode;

        void *find_cached_address(std::string_view key) const
        {
            const auto cached = offset_value(cache_, offset_section, key);
            if (!cached)
                return nullptr;

            auto remaining = std::string_view(*cached);
            while (!remaining.empty())
            {
                const auto comma = remaining.find(',');
                auto token = trim(remaining.substr(0, comma));
                if (token.starts_with("0x") || token.starts_with("0X"))
                    token.remove_prefix(2);

                std::uintptr_t rva = 0;
                const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), rva, 16);
                const bool valid_rva = error == std::errc{} && end == token.data() + token.size() && rva < image_.size;
                if (valid_rva && executable_address(image_.base + rva, image_))
                    return reinterpret_cast<void *>(image_.base + rva);

                log("cache: invalid {} entry; rescanning", key);
                if (comma == std::string_view::npos)
                    break;
                remaining.remove_prefix(comma + 1);
            }
            return nullptr;
        }

        void store_offsets(std::string_view key, const std::vector<std::uintptr_t> &addresses)
        {
            std::string value;
            for (const auto address : addresses)
            {
                if (!value.empty())
                    value += ',';
                value += std::format("0x{:x}", address - image_.base);
            }

            cache_ = upsert_offset(cache_, offset_section, key, value);
            std::ofstream file(offset_path, std::ios::binary | std::ios::trunc);
            if (!file || !(file << cache_) || !file.flush())
                log("cache: could not write offset.ini");
        }

      public:
        void *resolve(Function function)
        {
            const auto &signature = signatures[index(function)];
            const auto key = cache_key(signature.name);
            if (!debug_)
                if (void *cached_address = find_cached_address(key))
                    return cached_address;

            const auto pattern = parse_pattern(signature.pattern);
            std::vector<std::uintptr_t> matches;
            for (const auto &region : regions_)
            {
                const auto found = scan_region(reinterpret_cast<const std::uint8_t *>(region.base),
                                               region.size,
                                               pattern,
                                               scan_match_limit - matches.size());
                matches.insert(matches.end(), found.begin(), found.end());
                if (matches.size() == scan_match_limit)
                    break;
            }

            std::vector<std::uintptr_t> resolved;
            for (auto address : matches)
            {
                for (unsigned i = 0; i < signature.resolve_count && address; ++i)
                {
                    if (!image_.contains(address, rel32_instruction_size))
                    {
                        address = 0;
                        break;
                    }
                    address = relative_address(address);
                }
                if (!executable_address(address, image_) ||
                    std::ranges::contains(resolved, address))
                    continue;
                resolved.push_back(address);
            }
            if (resolved.empty())
                return nullptr;

            if (!debug_)
                store_offsets(key, resolved);

            return reinterpret_cast<void *>(resolved.front());
        }
    };

    struct Il2CppString
    {
        void *klass;
        void *monitor;
        std::int32_t length;
        wchar_t chars[32];
    };
    static_assert(sizeof(void *) == 8);
    static_assert(offsetof(Il2CppString, length) == 16 && offsetof(Il2CppString, chars) == 20);
    using GetFrameCountFn = int(WINAPI *)();
    using SetFrameCountFn = int(WINAPI *)(int);
    using ChangeFovFn = int(WINAPI *)(void *, float);
    using CameraBrainFlushFn = std::int64_t(WINAPI *)(void *);
    using SwitchInputFn = void(WINAPI *)(void *);
    using ObjectVoidFn = void(WINAPI *)(void *);
    using FindObjectFn = void *(WINAPI *)(Il2CppString *);
    using SetActiveFn = void(WINAPI *)(void *, bool);
    using EventCameraFn = bool(WINAPI *)(void *, void *);
    // The current game reads five more stack arguments after reaction. Preserve
    // their values without assigning unverified game-side meanings to them.
    using DamageTextFn = void(WINAPI *)(void *,
                                       int,
                                       int,
                                       int,
                                       float,
                                       Il2CppString *,
                                       void *,
                                       void *,
                                       int,
                                       std::uint8_t,
                                       float,
                                       std::uint8_t,
                                       std::uint8_t,
                                       float);
    using DisplayFogFn = void(WINAPI *)(bool);
    using PerspectiveFn = void(WINAPI *)(void *, bool);
    using FindStringFn = Il2CppString *(WINAPI *)(const char *);
    using CraftPartnerFn = bool(WINAPI *)(Il2CppString *, void *, void *, void *, void *);
    using CheckMapFn = bool(WINAPI *)(void *);
    using CheckEnterFn = bool(WINAPI *)();
    using OpenTeamFn = void(WINAPI *)();
    using OpenTeamPageFn = void(WINAPI *)(bool);

    std::array<std::atomic<void *>, function_count> originals{};
    std::array<std::atomic<void *>, function_count> helpers{};

    template <class T> T original(Function function)
    {
        return reinterpret_cast<T>(originals[index(function)].load(std::memory_order_acquire));
    }
    template <class T> T helper(Function function)
    {
        return reinterpret_cast<T>(helpers[index(function)].load(std::memory_order_acquire));
    }

    template <class F, class... Args> auto game_call(Function id, F function, Args... args)
    {
        using Result = std::invoke_result_t<F, Args...>;
        const auto name = signatures[index(id)].name;
        if (!function)
            log("{}: game function unavailable", name);

        if constexpr (std::is_void_v<Result>)
        {
            return function && guarded(name, [&] {
                       function(args...);
                   });
        }
        else
        {
            std::optional<Result> result;
            if (function && !guarded(name, [&] {
                    result = function(args...);
                }))
                result.reset();
            return result;
        }
    }

    enum class Feature : std::size_t
    {
        Pump,
        DamageText,
        EventCamera,
        Team,
        Craft,
        Uid,
        QuestBanner,
        Perspective,
        DiveMosaic,
        TouchScreen,
        FrameRate,
        Fog,
        Fov,
        Count
    };
    constexpr auto feature_count = static_cast<std::size_t>(Feature::Count);
    std::array<bool, feature_count> active_features{};
    bool feature_active(Feature feature)
    {
        return active_features[static_cast<std::size_t>(feature)];
    }

    std::array<bool, feature_count> select_features(const Config &config)
    {
        if (!config.disable_hot_reload)
        {
            std::array<bool, feature_count> result{};
            result.fill(true);
            return result;
        }
        return {
            true,
            config.disable_show_damage_text,
            config.disable_event_camera_move,
            config.remove_team_animation,
            config.redirect_crafting,
            config.hide_uid,
            config.hide_quest_banner,
            config.remove_blur,
            config.remove_dive_mosaic,
            config.use_touch_screen,
            config.enable_fps_override,
            config.remove_fog,
            config.enable_fov_override,
        };
    }

    bool set_path_active(const char *path, bool visible)
    {
        const auto find_string = helper<FindStringFn>(Function::FindString);
        const auto find_object = helper<FindObjectFn>(Function::FindGameObject);
        const auto set_active = helper<SetActiveFn>(Function::SetActive);
        if (!find_string || !find_object || !set_active)
            return false;

        const auto name = game_call(Function::FindString, find_string, path).value_or(nullptr);
        if (!name)
            return false;

        const auto object = game_call(Function::FindGameObject, find_object, name).value_or(nullptr);
        if (!object)
            return false;

        return game_call(Function::SetActive, set_active, object, visible);
    }

    bool open_synthesis_page()
    {
        const auto find_string = helper<FindStringFn>(Function::FindString);
        const auto open = helper<CraftPartnerFn>(Function::CraftEntryPartner);
        if (!find_string || !open)
            return false;

        const auto page = game_call(Function::FindString, find_string, "SynthesisPage").value_or(nullptr);
        if (!page)
            return false;

        // The helper's bool is not a success flag; completion without a fault is.
        return game_call(Function::CraftEntryPartner, open, page, nullptr, nullptr, nullptr, nullptr).has_value();
    }

    int WINAPI hook_frame_count()
    {
        const auto get_frame_count = original<GetFrameCountFn>(Function::GetFrameCount);
        const int frame_count = game_call(Function::GetFrameCount, get_frame_count).value_or(60);
        if (frame_count >= 60)
            return 60;
        if (frame_count >= 45)
            return 45;
        if (frame_count >= 30)
            return 30;
        return frame_count;
    }

    int WINAPI hook_change_fov(void *self, float value)
    {
        const auto config = get_config();
        if (value > 30.0f && config.enable_fov_override)
            value = config.fov_value;

        const auto change_fov = original<ChangeFovFn>(Function::ChangeFov);
        return game_call(Function::ChangeFov, change_fov, self, value).value_or(0);
    }

    void WINAPI hook_quest_banner(void *self)
    {
        if (get_config().hide_quest_banner &&
            set_path_active("Canvas/Pages/InLevelMapPage/GrpMap/GrpPointTips/Layout/QuestBanner", false))
            return;

        game_call(Function::SetupQuestBanner, original<ObjectVoidFn>(Function::SetupQuestBanner), self);
    }

    bool WINAPI hook_event_camera(void *self, void *event)
    {
        if (get_config().disable_event_camera_move)
            return true;

        const auto move_camera = original<EventCameraFn>(Function::EventCameraMove);
        return game_call(Function::EventCameraMove, move_camera, self, event).value_or(true);
    }

    void WINAPI hook_damage_text(void *self,
                                 int type,
                                 int damage_type,
                                 int show_type,
                                 float damage,
                                 Il2CppString *text,
                                 void *position,
                                 void *attackee,
                                 int reaction,
                                 std::uint8_t extra_flag_1,
                                 float extra_value_1,
                                 std::uint8_t extra_flag_2,
                                 std::uint8_t extra_flag_3,
                                 float extra_value_2)
    {
        if (!get_config().disable_show_damage_text)
            game_call(Function::ShowDamageText,
                      original<DamageTextFn>(Function::ShowDamageText),
                      self,
                      type,
                      damage_type,
                      show_type,
                      damage,
                      text,
                      position,
                      attackee,
                      reaction,
                      extra_flag_1,
                      extra_value_1,
                      extra_flag_2,
                      extra_flag_3,
                      extra_value_2);
    }

    void WINAPI hook_perspective(void *self, bool display)
    {
        if (!get_config().remove_blur)
            game_call(Function::PlayerPerspective, original<PerspectiveFn>(Function::PlayerPerspective), self, display);
    }

    bool WINAPI hook_check_map(void *self)
    {
        if (get_config().redirect_crafting)
            return false;

        const auto check_map = original<CheckMapFn>(Function::CheckCanOpenMap);
        return game_call(Function::CheckCanOpenMap, check_map, self).value_or(false);
    }

    void WINAPI hook_craft(void *self)
    {
        if (get_config().redirect_crafting && open_synthesis_page())
            return;

        game_call(Function::CraftEntry, original<ObjectVoidFn>(Function::CraftEntry), self);
    }

    void WINAPI hook_team()
    {
        const auto can_enter = helper<CheckEnterFn>(Function::CheckCanEnter);
        const auto open_team_page = helper<OpenTeamPageFn>(Function::OpenTeamPage);
        if (get_config().remove_team_animation && can_enter && open_team_page)
        {
            const bool can_open_team = game_call(Function::CheckCanEnter, can_enter).value_or(false);
            if (can_open_team && game_call(Function::OpenTeamPage, open_team_page, false))
                return;
        }

        game_call(Function::OpenTeam, original<OpenTeamFn>(Function::OpenTeam));
    }

    struct MosaicPatch
    {
        std::uint8_t *address = nullptr;
        std::array<std::uint8_t, rel32_instruction_size> original{};
        bool patched = false;

        bool set(bool enabled)
        {
            if (!address || patched == enabled)
                return address != nullptr;
            constexpr std::array<std::uint8_t, rel32_instruction_size> replacement{
                0xB8,
                0,
                0,
                0,
                0,
            };

            DWORD previous = 0;
            if (!VirtualProtect(address, original.size(), PAGE_EXECUTE_READWRITE, &previous))
            {
                log("dive_mosaic: VirtualProtect failed: {}", GetLastError());
                return false;
            }

            const auto &bytes = enabled ? replacement : original;
            const bool copied = guarded("dive_mosaic write", [&] {
                std::memcpy(address, bytes.data(), bytes.size());
            });

            DWORD discarded = 0;
            const bool restored = VirtualProtect(address, original.size(), previous, &discarded) != FALSE;
            if (!restored)
                log("dive_mosaic: protection restore failed: {}", GetLastError());

            const bool flushed = FlushInstructionCache(GetCurrentProcess(), address, original.size()) != FALSE;
            if (!flushed)
                log("dive_mosaic: FlushInstructionCache failed: {}", GetLastError());

            if (copied)
                patched = enabled;
            return copied && restored && flushed;
        }
    };
    MosaicPatch mosaic_patch;
    bool mosaic_location_attempted = false;
    bool touch_initialized = false;

    std::uint8_t *find_mosaic_call(std::uint8_t *entry, void *effect, std::size_t size)
    {
        std::uint8_t *result = nullptr;
        if (size < rel32_instruction_size)
            return nullptr;

        guarded("dive_mosaic locate", [&] {
            for (std::size_t i = 0; i <= size - rel32_instruction_size; ++i)
            {
                if (entry[i] == 0xE8 && relative_address(reinterpret_cast<std::uintptr_t>(entry + i)) ==
                                            reinterpret_cast<std::uintptr_t>(effect))
                    result = entry + i;
            }
        });
        return result;
    }

    void update_mosaic(bool remove)
    {
        constexpr std::size_t mosaic_scan_window = 0x800;
        if (!mosaic_location_attempted)
        {
            mosaic_location_attempted = true;
            const auto entry = helper<std::uint8_t *>(Function::PlayerDiveMosaic);
            const auto effect = helper<void *>(Function::DisplayEffect);
            if (entry && effect)
            {
                if (const auto call = find_mosaic_call(entry, effect, mosaic_scan_window))
                {
                    if (guarded("dive_mosaic original", [&] {
                            std::memcpy(mosaic_patch.original.data(), call, mosaic_patch.original.size());
                        }))
                        mosaic_patch.address = call;
                }
                else
                    log("dive_mosaic: no matching effect call found");
            }
        }
        if (mosaic_patch.address)
            mosaic_patch.set(remove);
    }

    constexpr unsigned command_craft = 1;
    constexpr unsigned command_uid = 2;
    std::atomic<unsigned> pending_commands{0};
    std::atomic<bool> game_update_ready{false};

    void request(unsigned command)
    {
        pending_commands.fetch_or(command, std::memory_order_relaxed);
        debug("command: request {}", command);
    }

    void dispatch_game_thread()
    {
        if (!game_update_ready.exchange(true, std::memory_order_release))
            debug("pump: first tick");

        const auto config = get_config();
        const unsigned commands = pending_commands.exchange(0, std::memory_order_relaxed);
        if (commands & command_craft)
        {
            const bool success = feature_active(Feature::Craft) && open_synthesis_page();
            debug("command: craft {}", success ? "handled" : "unavailable");
        }
        if (commands & command_uid)
        {
            const bool success = feature_active(Feature::Uid) &&
                                 set_path_active("/BetaWatermarkCanvas(Clone)/Panel/TxtUID", !config.hide_uid);
            debug("command: UID {}", success ? "handled" : "unavailable");
        }

        if (feature_active(Feature::DiveMosaic))
            update_mosaic(config.remove_dive_mosaic);
        if (feature_active(Feature::TouchScreen) && !touch_initialized)
        {
            touch_initialized = true;
            if (const auto function = helper<SwitchInputFn>(Function::SwitchInputDevice);
                config.use_touch_screen && function)
                game_call(Function::SwitchInputDevice, function, nullptr);
        }
        if (feature_active(Feature::FrameRate) && config.enable_fps_override)
        {
            if (const auto function = helper<SetFrameCountFn>(Function::SetFrameCount))
                game_call(Function::SetFrameCount, function, config.selected_fps);
        }
        if (feature_active(Feature::Fog))
        {
            if (const auto function = helper<DisplayFogFn>(Function::DisplayFog))
                game_call(Function::DisplayFog, function, !config.remove_fog);
        }
    }

    std::int64_t WINAPI hook_camera_flush(void *self)
    {
        dispatch_game_thread();

        const auto flush_camera = original<CameraBrainFlushFn>(Function::CameraBrainFlush);
        return game_call(Function::CameraBrainFlush, flush_camera, self).value_or(0);
    }

    struct HookSpec
    {
        Feature feature;
        Function function;
        void *detour;
    };
    const std::array<HookSpec, 10> hook_specs{{
        {
            .feature = Feature::Pump,
            .function = Function::CameraBrainFlush,
            .detour = reinterpret_cast<void *>(hook_camera_flush),
        },
        {
            .feature = Feature::DamageText,
            .function = Function::ShowDamageText,
            .detour = reinterpret_cast<void *>(hook_damage_text),
        },
        {
            .feature = Feature::EventCamera,
            .function = Function::EventCameraMove,
            .detour = reinterpret_cast<void *>(hook_event_camera),
        },
        {
            .feature = Feature::Team,
            .function = Function::OpenTeam,
            .detour = reinterpret_cast<void *>(hook_team),
        },
        {
            .feature = Feature::Craft,
            .function = Function::CraftEntry,
            .detour = reinterpret_cast<void *>(hook_craft),
        },
        {
            .feature = Feature::Craft,
            .function = Function::CheckCanOpenMap,
            .detour = reinterpret_cast<void *>(hook_check_map),
        },
        {
            .feature = Feature::QuestBanner,
            .function = Function::SetupQuestBanner,
            .detour = reinterpret_cast<void *>(hook_quest_banner),
        },
        {
            .feature = Feature::Perspective,
            .function = Function::PlayerPerspective,
            .detour = reinterpret_cast<void *>(hook_perspective),
        },
        {
            .feature = Feature::FrameRate,
            .function = Function::GetFrameCount,
            .detour = reinterpret_cast<void *>(hook_frame_count),
        },
        {
            .feature = Feature::Fov,
            .function = Function::ChangeFov,
            .detour = reinterpret_cast<void *>(hook_change_fov),
        },
    }};

    struct HelperSpec
    {
        Feature feature;
        Function function;
    };
    constexpr std::array<HelperSpec, 15> helper_specs{{
        {
            .feature = Feature::Team,
            .function = Function::CheckCanEnter,
        },
        {
            .feature = Feature::Team,
            .function = Function::OpenTeamPage,
        },
        {
            .feature = Feature::Craft,
            .function = Function::FindString,
        },
        {
            .feature = Feature::Craft,
            .function = Function::CraftEntryPartner,
        },
        {
            .feature = Feature::Uid,
            .function = Function::FindString,
        },
        {
            .feature = Feature::Uid,
            .function = Function::FindGameObject,
        },
        {
            .feature = Feature::Uid,
            .function = Function::SetActive,
        },
        {
            .feature = Feature::QuestBanner,
            .function = Function::FindString,
        },
        {
            .feature = Feature::QuestBanner,
            .function = Function::FindGameObject,
        },
        {
            .feature = Feature::QuestBanner,
            .function = Function::SetActive,
        },
        {
            .feature = Feature::DiveMosaic,
            .function = Function::PlayerDiveMosaic,
        },
        {
            .feature = Feature::DiveMosaic,
            .function = Function::DisplayEffect,
        },
        {
            .feature = Feature::TouchScreen,
            .function = Function::SwitchInputDevice,
        },
        {
            .feature = Feature::FrameRate,
            .function = Function::SetFrameCount,
        },
        {
            .feature = Feature::Fog,
            .function = Function::DisplayFog,
        },
    }};

    bool install_hooks()
    {
        const auto config = get_config();
        active_features = select_features(config);

        const auto initialize_status = MH_Initialize();
        if (initialize_status != MH_OK)
        {
            log("install: {}", MH_StatusToString(initialize_status));
            return false;
        }

        std::array<bool, function_count> required{};
        for (const auto &hook : hook_specs)
            if (feature_active(hook.feature))
                required[index(hook.function)] = true;
        for (const auto &spec : helper_specs)
            if (feature_active(spec.feature))
                required[index(spec.function)] = true;

        Scanner scanner;
        std::array<void *, function_count> addresses{};
        for (std::size_t i = 0; i < function_count; ++i)
        {
            if (!required[i])
                continue;

            addresses[i] = scanner.resolve(static_cast<Function>(i));
            if (addresses[i])
                debug("resolve: {} addr={}", signatures[i].name, addresses[i]);
            else
                log("resolve: {} missing", signatures[i].name);
        }

        for (const auto &spec : helper_specs)
            if (feature_active(spec.feature))
                helpers[index(spec.function)].store(addresses[index(spec.function)], std::memory_order_release);

        bool pump_created = false;
        for (const auto &hook : hook_specs)
        {
            if (!feature_active(hook.feature) || !addresses[index(hook.function)])
                continue;

            void *trampoline = nullptr;
            const auto create_status = MH_CreateHook(addresses[index(hook.function)], hook.detour, &trampoline);
            if (create_status != MH_OK)
            {
                log("hook: {} {}", signatures[index(hook.function)].name, MH_StatusToString(create_status));
                continue;
            }

            originals[index(hook.function)].store(trampoline, std::memory_order_release);
            if (hook.feature == Feature::Pump)
                pump_created = true;
            debug("hook: {} target={} trampoline={}",
                  signatures[index(hook.function)].name,
                  addresses[index(hook.function)],
                  trampoline);
        }

        // A missing pump otherwise leaves the runtime waiting forever, with no features running.
        if (!pump_created)
        {
            log("install: game-thread pump unavailable; hooks not enabled");
            const auto cleanup_status = MH_Uninitialize();
            if (cleanup_status != MH_OK)
                log("uninitialize: {}", MH_StatusToString(cleanup_status));
            return false;
        }

        const auto enable_status = MH_EnableHook(nullptr);
        if (enable_status != MH_OK)
        {
            log("enable: {}", MH_StatusToString(enable_status));
            const auto cleanup_status = MH_Uninitialize();
            if (cleanup_status != MH_OK)
                log("uninitialize: {}", MH_StatusToString(cleanup_status));
            return false;
        }

        debug("install: enable all ok");
        return true;
    }

    void enable_hdr()
    {
        HKEY key = nullptr;
        LSTATUS result = RegCreateKeyExW(HKEY_CURRENT_USER,
                                         L"Software\\miHoYo\\\u539f\u795e",
                                         0,
                                         nullptr,
                                         REG_OPTION_NON_VOLATILE,
                                         KEY_SET_VALUE,
                                         nullptr,
                                         &key,
                                         nullptr);
        if (result != ERROR_SUCCESS)
        {
            log("hdr: RegCreateKeyExW failed: {}", result);
            return;
        }

        const DWORD value = 1;
        result = RegSetValueExW(
            key, L"WINDOWS_HDR_ON_h3132281285", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value));
        const auto closed = RegCloseKey(key);
        if (result != ERROR_SUCCESS)
            log("hdr: RegSetValueExW failed: {}", result);
        else
            debug("hdr: registry value enabled");

        if (closed != ERROR_SUCCESS)
            log("hdr: RegCloseKey failed: {}", closed);
    }

    bool game_is_foreground()
    {
        const HWND window = GetForegroundWindow();
        DWORD process_id = 0;
        return window && GetWindowThreadProcessId(window, &process_id) && process_id == GetCurrentProcessId();
    }

    DWORD WINAPI run(void *parameter)
    {
        try
        {
            const auto directory = module_path(static_cast<HMODULE>(parameter)).parent_path();
            config_path = directory / L"config.ini";
            offset_path = directory / L"offset.ini";
            log_path = directory / L"hutao_minhook_ng.log";

            if (!reload_config())
                return 1;
            if (get_config().debug_mode)
            {
                const HANDLE file = CreateFileW(log_path.c_str(),
                                                GENERIC_WRITE,
                                                FILE_SHARE_READ,
                                                nullptr,
                                                CREATE_ALWAYS,
                                                FILE_ATTRIBUTE_NORMAL,
                                                nullptr);
                if (file != INVALID_HANDLE_VALUE)
                    CloseHandle(file);
                else
                    OutputDebugStringA("hutao: could not reset the debug log.\n");
            }

            debug("logger: start");
            HMODULE pinned = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                    reinterpret_cast<LPCWSTR>(&run),
                                    &pinned))
            {
                log("runtime: could not pin the DLL: {}", GetLastError());
                return 1;
            }

            if (get_config().enable_hdr)
                enable_hdr();
            if (!install_hooks())
                return 1;

            while (!game_update_ready.load(std::memory_order_acquire))
                Sleep(1000);

            debug("runtime: game update ready");
            request(command_uid);
            for (;;)
            {
                const auto config = get_config();
                if (config.toggle_key && (GetAsyncKeyState(config.toggle_key) & 0x8000))
                {
                    if (reload_config())
                    {
                        debug("config: reload");
                        request(command_uid);
                    }
                    Sleep(500);
                }

                if (config.craft_key && game_is_foreground() && (GetAsyncKeyState(config.craft_key) & 0x8000))
                {
                    request(command_craft);
                    Sleep(500);
                }

                Sleep(100);
            }
        }
        catch (const std::exception &error)
        {
            log("runtime: {}", error.what());
            return 1;
        }
    }

} // namespace hutao

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, void *)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        // Keep thread notifications enabled for the statically linked CRT.
        const HANDLE thread = CreateThread(nullptr, 0, hutao::run, module, 0, nullptr);
        if (!thread)
        {
            OutputDebugStringA("hutao: could not create the runtime thread.\n");
            return FALSE;
        }
        CloseHandle(thread);
    }
    return TRUE;
}
