#include "router.hpp"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <RtMidi.h>
#include <teVirtualMIDI.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace {

[[nodiscard]] static std::string to_string(const std::wstring& utf16)
{
    if (utf16.empty()) {
        return {};
    }

    const int _size = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), nullptr, 0, nullptr, nullptr);
    std::string _utf8(_size, 0);
    WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), _utf8.data(), _size, nullptr, nullptr);
    return _utf8;
}

[[nodiscard]] static std::wstring to_wstring(const std::string& utf8)
{
    if (utf8.empty()) {
        return {};
    }

    const int _size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring _utf16(_size, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), _utf16.data(), _size);
    return _utf16;
}

struct virtualmidi_api {
    HMODULE dll_module = nullptr;

    using PFN_CreateEx2 = LPVM_MIDI_PORT(WINAPI*)(LPCWSTR, LPVM_MIDI_DATA_CB, LPVOID, DWORD, DWORD);
    using PFN_GetData = BOOL(WINAPI*)(LPVM_MIDI_PORT, PBYTE, PDWORD);
    using PFN_SendData = BOOL(WINAPI*)(LPVM_MIDI_PORT, PBYTE, DWORD);
    using PFN_Close = VOID(WINAPI*)(LPVM_MIDI_PORT);

    PFN_CreateEx2 CreateEx2 = nullptr;
    PFN_GetData GetData = nullptr;
    PFN_SendData SendData = nullptr;
    PFN_Close Close = nullptr;

    [[nodiscard]] static std::string get_last_error_message(DWORD error)
    {
        LPWSTR _buffer = nullptr;
        const DWORD _flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
        const DWORD _language_id = MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT);
        const DWORD _result = FormatMessageW(_flags, nullptr, error, _language_id, (LPWSTR)&_buffer, 0, nullptr);
        const std::wstring _message = _result ? std::wstring(_buffer, _result) : L"(unknown)";

        if (_buffer) {
            LocalFree(_buffer);
        }

        return to_string(_message);
    }

    void load()
    {
#if defined(_WIN64)
        const wchar_t* _candidate_paths[] = { L".\\teVirtualMIDI64.dll", L"teVirtualMIDI64.dll" };
#else
        const wchar_t* _candidate_paths[] = { L".\\teVirtualMIDI32.dll", L"teVirtualMIDI32.dll" };
#endif

        if (dll_module) {
            return;
        }

        for (const wchar_t* _path : _candidate_paths) {
            dll_module = LoadLibraryW(_path);
            if (dll_module) {
                break;
            }
        }

        if (!dll_module) {
            DWORD _error = GetLastError();
            throw std::runtime_error(std::string("LoadLibrary failed: ") + get_last_error_message(_error).c_str());
        }

        CreateEx2 = reinterpret_cast<PFN_CreateEx2>(GetProcAddress(dll_module, "virtualMIDICreatePortEx2"));
        GetData = reinterpret_cast<PFN_GetData>(GetProcAddress(dll_module, "virtualMIDIGetData"));
        SendData = reinterpret_cast<PFN_SendData>(GetProcAddress(dll_module, "virtualMIDISendData"));
        Close = reinterpret_cast<PFN_Close>(GetProcAddress(dll_module, "virtualMIDIClosePort"));

        if (!CreateEx2 || !GetData || !Close) {
            unload();
            throw std::runtime_error("GetProcAddress failed (missing exports)");
        }
    }

    void unload()
    {
        CreateEx2 = nullptr;
        GetData = nullptr;
        SendData = nullptr;
        Close = nullptr;

        if (dll_module) {
            FreeLibrary(dll_module);
            dll_module = nullptr;
        }
    }
};

static std::mutex global_hardware_mutex;
static RtMidiOut global_hardware_midiout;

static LPVM_MIDI_PORT global_virtual_midiout = nullptr;
static virtualmidi_api global_virtual_api;
static std::atomic<bool> global_is_virtual_running = false;
static std::thread global_virtual_thread;

}

std::vector<std::string> get_hardware_ports()
{
    std::vector<std::string> _hardware_ports;
    _hardware_ports.resize(global_hardware_midiout.getPortCount());
    for (unsigned int _index = 0; _index < _hardware_ports.size(); ++_index) {
        _hardware_ports[_index] = global_hardware_midiout.getPortName(_index);
    }
    return _hardware_ports;
}

void open_hardware_output(const std::size_t& index)
{
    std::lock_guard<std::mutex> _lock_guard(global_hardware_mutex);    
    if (global_hardware_midiout.isPortOpen()) {
        global_hardware_midiout.closePort();
    }
    global_hardware_midiout.openPort(index);
}

void close_hardware_output()
{
    std::lock_guard<std::mutex> _lock_guard(global_hardware_mutex);
    if (global_hardware_midiout.isPortOpen()) {
        global_hardware_midiout.closePort();
    }
}

bool is_hardware_output_open()
{
    std::lock_guard<std::mutex> _lock_guard(global_hardware_mutex);
    return global_hardware_midiout.isPortOpen();
}

static unsigned char g_running = 0;                 // channel-voice running status
static std::vector<unsigned char> g_syx_accum;      // accumulate F0...F7 across chunks

static inline bool isStatus(unsigned char b)   { return (b & 0x80) != 0; }
static inline bool isRealtime(unsigned char b) { return b >= 0xF8; }
static inline bool isSystemCommon(unsigned char b){ return b >= 0xF0 && b <= 0xF7; }

static inline int dataCountForStatus(unsigned char s){
    if (s < 0xF0) { // channel voice
        unsigned char hi = s & 0xF0;
        return (hi == 0xC0 || hi == 0xD0) ? 1 : 2; // PC/Channel Pressure=1, others=2
    }
    switch (s) {    // system common
        case 0xF0: return -1; // SysEx (variable until F7)
        case 0xF1: return 1;  // MTC Quarter Frame
        case 0xF2: return 2;  // Song Position
        case 0xF3: return 1;  // Song Select
        case 0xF6: return 0;  // Tune Request
        case 0xF7: return 0;  // EOX (handled mainly inside SysEx)
        default:    return 0;  // real-time (F8..FF) or undefined F4/F5 => 0 data
    }
}

// Send ALL MIDI: short messages split properly, SysEx assembled, real-time passthrough
static void forward_unfiltered_split_and_send(const unsigned char* s, size_t n)
{
    size_t i = 0;

    auto sendShort = [&](const unsigned char* p, size_t len){
        try { global_hardware_midiout.sendMessage(p, (int)len); } catch (...) {}
    };
    auto sendVec = [&](const std::vector<unsigned char>& v){
        if (!v.empty()) { try { global_hardware_midiout.sendMessage(&v); } catch (...) {} }
    };
    auto sendByte = [&](unsigned char b){ sendShort(&b, 1); };

    while (i < n) {
        unsigned char b = s[i];

        // Real-time messages can appear anywhere; each is 1 byte
        if (isRealtime(b)) { sendByte(b); ++i; continue; }

        // Start or continue SysEx
        if (b == 0xF0 || !g_syx_accum.empty()) {
            if (b == 0xF0 && g_syx_accum.empty()) { g_syx_accum.clear(); g_syx_accum.push_back(0xF0); ++i; }
            for (; i < n; ++i) {
                unsigned char c = s[i];
                if (isRealtime(c)) { sendByte(c); continue; } // real-time allowed inside SysEx
                g_syx_accum.push_back(c);
                if (c == 0xF7) { sendVec(g_syx_accum); g_syx_accum.clear(); ++i; break; }
            }
            g_running = 0; // SysEx cancels running status
            continue;
        }

        // Status byte starts a new message
        if (isStatus(b)) {
            int need = dataCountForStatus(b);

            // System Common (non-SysEx)
            if (isSystemCommon(b) && b != 0xF0) {
                size_t have = n - (i + 1);
                size_t take = (need > 0 && have >= (size_t)need) ? (size_t)need : 0;
                unsigned char msg[3] = { b, 0, 0 };
                for (size_t k = 0; k < take; ++k) msg[1 + k] = s[i + 1 + k];
                sendShort(msg, 1 + take);
                i += 1 + take;
                g_running = 0; // System Common cancels running status
                continue;
            }

            // Channel-voice short message
            if (need >= 0) {
                if (i + 1 + need <= n) {
                    unsigned char msg[3] = { b, 0, 0 };
                    for (int k = 0; k < need; ++k) msg[1 + k] = s[i + 1 + k];
                    sendShort(msg, 1 + (size_t)need);
                    g_running = b; // set running
                    i += 1 + (size_t)need;
                } else {
                    // incomplete at end of chunk; drop remainder to avoid malformed send
                    i = n;
                }
                continue;
            }

            // need == -1 would be SysEx (handled above)
            ++i;
            continue;
        }

        // Data byte with running status (channel voice only)
        if (g_running) {
            int need = dataCountForStatus(g_running);
            if (need == 1) {
                unsigned char msg[2] = { g_running, s[i] };
                sendShort(msg, 2);
                ++i;
            } else if (need == 2) {
                if (i + 1 < n) {
                    unsigned char msg[3] = { g_running, s[i], s[i + 1] };
                    sendShort(msg, 3);
                    i += 2;
                } else {
                    // incomplete pair at end; drop the last data byte
                    ++i;
                }
            } else {
                ++i;
            }
            continue;
        }

        // Stray data byte without a running status: skip to resync
        ++i;
    }
}

void send_to_hardware_output(const std::vector<unsigned char>& msg)
{
    std::lock_guard<std::mutex> _lock_guard(global_hardware_mutex);
    if (global_hardware_midiout.isPortOpen()) {
        forward_unfiltered_split_and_send(msg.data(), msg.size());
    }
}


void open_virtual_input(const std::string& port, const std::function<void(const std::vector<unsigned char>&)>& callback)
{
    if (global_is_virtual_running.load()) {
        return;
    }

    global_virtual_api.load();

    const DWORD _flags = 0;
    const DWORD _max_sysex_size = 65535;
    global_virtual_midiout = global_virtual_api.CreateEx2(to_wstring(port).c_str(), nullptr, nullptr, _max_sysex_size, _flags);
    if (!global_virtual_midiout) {
        throw std::runtime_error("CreatePortEx2 failed");
    }

    global_is_virtual_running = true;
    global_virtual_thread = std::thread([callback] {
        std::vector<unsigned char> _buffer(65536);
        while (global_is_virtual_running.load()) {

            DWORD _size;
            if (global_virtual_api.GetData(global_virtual_midiout, _buffer.data(), &_size)) {
                _buffer.resize(_size);
                callback(_buffer);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    });
}

void close_virtual_input()
{
    if (!global_is_virtual_running.load()) {
        return;
    }

    global_is_virtual_running = false;
    if (global_virtual_thread.joinable()) {
        global_virtual_thread.join();
    }

    if (global_virtual_midiout) {
        global_virtual_api.Close(global_virtual_midiout);
        global_virtual_midiout = nullptr;
    }

    global_virtual_api.unload();
}

bool is_virtual_input_open()
{
    return global_is_virtual_running.load();
}
