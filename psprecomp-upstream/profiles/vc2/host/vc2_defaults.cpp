// Started on its own (not by the launcher), the game behaves as the launcher
// starts it. Runs before main (a static initialiser), so everything that reads
// the environment later sees these values:
//  - the native pipeline (VC2_NATIVE_GU=all, VC2_NATIVE_DRAW=1,
//    VC2_NATIVE_LISTS=1) and native code only (PSPRECOMP_NO_INTERPRETER=1);
//    VC2_NATIVE=0 keeps the old GE path for a test;
//  - in a portable package (the launcher, Gallian Chronicles.exe, beside this
//    executable) the data folder beside it (VC2_DATA_DIR): the one the
//    launcher last chose (launcher.ini [Paths] DataDir), else portablekit-data,
//    -us, -jp; with none set up yet, the launcher is opened instead (setting
//    up is done there);
//  - opened by a double click (its console is its own), no console window:
//    the console output goes to play-log.txt in that data folder.
// A value already in the environment (the launcher's, a test script's) wins.

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <atomic>
#include <cctype>
#include <iostream>
#include <mutex>
#include <streambuf>

namespace {

bool has(const wchar_t *name) {
    const wchar_t *value = _wgetenv(name);
    return value != nullptr && *value != L'\0';
}

void set_default(const wchar_t *name, const std::wstring &value) {
    if (!has(name)) _wputenv_s(name, value.c_str());
}

std::filesystem::path executable_folder() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

bool is_data_folder(const std::filesystem::path &folder) {
    std::error_code error;
    return !folder.empty() && std::filesystem::is_regular_file(folder / L"EBOOT.ELF", error);
}

// launcher.ini (UTF-16 or ANSI, as WritePrivateProfileStringW wrote it): [Paths] DataDir.
std::filesystem::path launcher_data_dir(const std::filesystem::path &folder) {
    const std::filesystem::path ini = folder / L"launcher.ini";
    std::error_code error;
    if (!std::filesystem::is_regular_file(ini, error)) return {};
    wchar_t value[MAX_PATH * 2]{};
    GetPrivateProfileStringW(L"Paths", L"DataDir", L"", value, MAX_PATH * 2, ini.c_str());
    return value[0] != L'\0' ? std::filesystem::path(value) : std::filesystem::path{};
}

// The log (2026-11-16, owner: "that shouldn't be a main function of the
// game"): quiet unless asked for. Each line written to std::cout / std::cerr
// is kept only when it is start-up information (the first lines of a run) or
// reports a problem (an error, a failure, a warning, something missing).
// The diagnostics (frame statistics every few seconds, input and draw traces,
// save reads) are written only with VC2_DETAILED_LOG=1 (the launcher's
// "Detailed log (for bug reports)") or while any VC2_*TRACE*, *CAPTURE*,
// *CENSUS* or *DUMP* switch is set (development scripts).
bool detailed_log() {
    if (const wchar_t *value = _wgetenv(L"VC2_DETAILED_LOG"); value != nullptr && *value != L'\0')
        return std::wstring(value) != L"0";
    for (wchar_t **entry = _wenviron; entry != nullptr && *entry != nullptr; ++entry) {
        std::wstring name(*entry);
        name = name.substr(0, name.find(L'='));
        if (name.rfind(L"VC2_", 0) != 0) continue;
        for (const wchar_t *word : {L"TRACE", L"CAPTURE", L"CENSUS", L"DUMP"})
            if (name.find(word) != std::wstring::npos) return true;
    }
    return false;
}

class QuietLog final : public std::streambuf {
public:
    QuietLog(std::streambuf *out, int slot) : out_(out), slot_(slot) {}

protected:
    int overflow(int c) override {
        if (c == traits_type::eof()) return 0;
        line().push_back(static_cast<char>(c));
        if (c == '\n') emit();
        return c;
    }
    std::streamsize xsputn(const char *s, std::streamsize n) override {
        for (std::streamsize i = 0; i < n; ++i) overflow(static_cast<unsigned char>(s[i]));
        return n;
    }
    int sync() override { return 0; }

private:
    std::string &line() {
        thread_local std::string lines[2];  // one partial line per stream and thread
        return lines[slot_];
    }
    static std::mutex &lock() {
        static std::mutex m;
        return m;
    }
    static bool wanted(const std::string &text) {
        static std::atomic<int> kept{0};
        constexpr int kStartLines = 60;  // the run's start: which game, edition, data folder, settings
        std::string lower(text);
        for (char &ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        for (const char *word : {"error", "fail", "warn", "cannot", "can't", "couldn't", "could not", "missing",
                                 "not found", "not as expected", "crash", "exception", "fatal", "abort",
                                 "unsupported", "invalid", "refused", "denied", "stopped", "exited"})
            if (lower.find(word) != std::string::npos) return true;
        // Never the periodic statistics or input traces, even at the start.
        for (const char *noise : {" by flip ", "frames a second", "] read ", " in context ", "-trace]",
                                  "[vc2-backgrounds]", "[vc3-backgrounds]"})
            if (lower.find(noise) != std::string::npos) return false;
        return kept.fetch_add(1) < kStartLines;
    }
    void emit() {
        std::string text;
        text.swap(line());
        if (!wanted(text)) return;
        std::lock_guard<std::mutex> guard(lock());
        out_->sputn(text.data(), static_cast<std::streamsize>(text.size()));
        out_->pubsync();
    }

    std::streambuf *out_;
    int slot_;
};

void quiet_log() {
    if (detailed_log()) return;
    // Never freed: the streams are used until the process ends.
    std::cout.rdbuf(new QuietLog(std::cout.rdbuf(), 0));
    std::cerr.rdbuf(new QuietLog(std::cerr.rdbuf(), 1));
}

struct Defaults {
    Defaults() {
        if (!(has(L"VC2_NATIVE") && std::wstring(_wgetenv(L"VC2_NATIVE")) == L"0")) {
            set_default(L"VC2_NATIVE_GU", L"all");
            set_default(L"VC2_NATIVE_DRAW", L"1");
            set_default(L"VC2_NATIVE_LISTS", L"1");
        }
        set_default(L"PSPRECOMP_NO_INTERPRETER", L"1");

        const std::filesystem::path here = executable_folder();
        std::error_code error;
        std::filesystem::path data;
        if (has(L"VC2_DATA_DIR")) {
            data = _wgetenv(L"VC2_DATA_DIR");
        } else if (!here.empty() && std::filesystem::is_regular_file(here / L"Gallian Chronicles.exe", error)) {
            if (const auto chosen = launcher_data_dir(here); is_data_folder(chosen)) data = chosen;
            for (const wchar_t *name : {L"portablekit-data", L"portablekit-data-us", L"portablekit-data-jp"})
                if (data.empty() && is_data_folder(here / name)) data = here / name;
            if (data.empty()) {
                // Nothing set up yet: setting up is the launcher's (Game files >
                // Set up from disc image), so it is opened instead.
                std::wstring command = L"\"" + (here / L"Gallian Chronicles.exe").wstring() + L"\"";
                STARTUPINFOW startup{};
                startup.cb = sizeof(startup);
                PROCESS_INFORMATION process{};
                if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr,
                                   here.c_str(), &startup, &process)) {
                    CloseHandle(process.hThread);
                    CloseHandle(process.hProcess);
                    ExitProcess(0);
                }
                data = here / L"portablekit-data";
            }
            _wputenv_s(L"VC2_DATA_DIR", data.c_str());
        }

        // A console of its own: opened from Explorer. Its window goes; the
        // output is kept in the data folder (as the launcher keeps it).
        DWORD processes[2]{};
        if (GetConsoleWindow() != nullptr && GetConsoleProcessList(processes, 2) == 1) {
            FreeConsole();
            if (!data.empty()) {
                std::filesystem::create_directories(data, error);
                const std::filesystem::path log = data / L"play-log.txt";
                if (_wfreopen(log.c_str(), L"w", stdout) != nullptr) std::setvbuf(stdout, nullptr, _IOLBF, 4096);
                _wfreopen(log.c_str(), L"a", stderr);
            }
        }
        quiet_log();
    }
};

const Defaults defaults;

}  // namespace
#endif
