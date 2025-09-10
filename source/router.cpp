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

using PFN_CreateEx2 = LPVM_MIDI_PORT(WINAPI*)(LPCWSTR, LPVM_MIDI_DATA_CB, LPVOID, DWORD, DWORD);
using PFN_GetData = BOOL(WINAPI*)(LPVM_MIDI_PORT, PBYTE, PDWORD);
using PFN_SendData = BOOL(WINAPI*)(LPVM_MIDI_PORT, PBYTE, DWORD);
using PFN_Close = VOID(WINAPI*)(LPVM_MIDI_PORT);

static HMODULE vtmidi_module = nullptr;
static PFN_CreateEx2 vtmidi_create_ex2 = nullptr;
static PFN_GetData vtmidi_get_data = nullptr;
static PFN_SendData vtmidi_send_data = nullptr;
static PFN_Close vtmidi_close = nullptr;

static std::mutex global_hardware_mutex;
static RtMidiOut global_hardware_midiout;

static LPVM_MIDI_PORT global_virtual_midiout = nullptr;
static std::atomic<bool> global_is_virtual_running = false;
static std::thread global_virtual_thread;

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

static void unload_vtmidi_library()
{
    vtmidi_create_ex2 = nullptr;
    vtmidi_get_data = nullptr;
    vtmidi_send_data = nullptr;
    vtmidi_close = nullptr;

    if (vtmidi_module) {
        FreeLibrary(vtmidi_module);
        vtmidi_module = nullptr;
    }
}

static void load_vtmidi_library()
{
#if defined(_WIN64)
    const wchar_t* _candidate_paths[] = { L".\\teVirtualMIDI64.dll", L"teVirtualMIDI64.dll" };
#else
    const wchar_t* _candidate_paths[] = { L".\\teVirtualMIDI32.dll", L"teVirtualMIDI32.dll" };
#endif

    if (vtmidi_module) {
        return;
    }

    for (const wchar_t* _path : _candidate_paths) {
        vtmidi_module = LoadLibraryW(_path);
        if (vtmidi_module) {
            break;
        }
    }

    if (!vtmidi_module) {
        DWORD _error = GetLastError();
        throw std::runtime_error(std::string("LoadLibrary failed: ") + get_last_error_message(_error).c_str());
    }

    vtmidi_create_ex2 = reinterpret_cast<PFN_CreateEx2>(GetProcAddress(vtmidi_module, "virtualMIDICreatePortEx2"));
    vtmidi_get_data = reinterpret_cast<PFN_GetData>(GetProcAddress(vtmidi_module, "virtualMIDIGetData"));
    vtmidi_send_data = reinterpret_cast<PFN_SendData>(GetProcAddress(vtmidi_module, "virtualMIDISendData"));
    vtmidi_close = reinterpret_cast<PFN_Close>(GetProcAddress(vtmidi_module, "virtualMIDIClosePort"));

    if (!vtmidi_create_ex2 || !vtmidi_get_data || !vtmidi_close) {
        unload_vtmidi_library();
        throw std::runtime_error("GetProcAddress failed (missing exports)");
    }
}

[[nodiscard]] static bool is_status(unsigned char byte)
{
    return (byte & 0x80) != 0;
}

[[nodiscard]] static bool is_realtime(unsigned char byte)
{
    return byte >= 0xF8;
}

[[nodiscard]] static bool is_system_common(unsigned char byte)
{
    return byte >= 0xF0 && byte <= 0xF7;
}

[[nodiscard]] static int data_count_for_status(unsigned char byte)
{
    if (byte < 0xF0) { // channel voice
        unsigned char hi = byte & 0xF0;
        return (hi == 0xC0 || hi == 0xD0) ? 1 : 2; // PC/Channel Pressure=1, others=2
    }
    switch (byte) { // system common
    case 0xF0:
        return -1; // SysEx (variable until F7)
    case 0xF1:
        return 1; // MTC Quarter Frame
    case 0xF2:
        return 2; // Song Position
    case 0xF3:
        return 1; // Song Select
    case 0xF6:
        return 0; // Tune Request
    case 0xF7:
        return 0; // EOX (handled mainly inside SysEx)
    default:
        return 0; // real-time (F8..FF) or undefined F4/F5 => 0 data
    }
}

static void send_short(const unsigned char* data, const std::size_t length)
{
    try {
        global_hardware_midiout.sendMessage(data, (int)length);
    } catch (...) {
    }
}

static void send_vector(const std::vector<unsigned char>& data)
{
    if (!data.empty()) {
        try {
            global_hardware_midiout.sendMessage(&data);
        } catch (...) {
        }
    }
}

static void send_byte(const unsigned char byte)
{
    send_short(&byte, 1);
}

static void split_and_send(const std::vector<unsigned char>& data)
{
    static unsigned char _running_status = 0;
    static std::vector<unsigned char> _sysex_accumulate;

    std::size_t _index = 0;
    while (_index < data.size()) {
        const unsigned char _byte = data[_index];
        if (is_realtime(_byte)) {
            send_byte(_byte);
            ++_index;
            continue;
        }

        if (_byte == 0xF0 || !_sysex_accumulate.empty()) {
            if (_byte == 0xF0 && _sysex_accumulate.empty()) {
                _sysex_accumulate.clear();
                _sysex_accumulate.push_back(0xF0);
                ++_index;
            }
            for (; _index < data.size(); ++_index) {
                const unsigned char _char = data[_index];
                if (is_realtime(_char)) {
                    send_byte(_char);
                    continue;
                }
                _sysex_accumulate.push_back(_char);
                if (_char == 0xF7) {
                    send_vector(_sysex_accumulate);
                    _sysex_accumulate.clear();
                    ++_index;
                    break;
                }
            }
            _running_status = 0;
            continue;
        }

        if (is_status(_byte)) {
            const int _need = data_count_for_status(_byte);
            if (is_system_common(_byte) && _byte != 0xF0) {
                const std::size_t _have = data.size() - (_index + 1);
                const std::size_t _take = (_need > 0 && _have >= static_cast<std::size_t>(_need)) ? static_cast<std::size_t>(_need) : 0;
                unsigned char _message[3] = { _byte, 0, 0 };
                for (size_t k = 0; k < _take; ++k) {
                    _message[1 + k] = data[_index + 1 + k];
                }
                send_short(_message, 1 + _take);
                _index += 1 + _take;
                _running_status = 0;
                continue;
            }

            if (_need >= 0) {
                if (_index + 1 + _need <= data.size()) {
                    unsigned char _message[3] = { _byte, 0, 0 };
                    for (int k = 0; k < _need; ++k) {
                        _message[1 + k] = data[_index + 1 + k];
                    }
                    send_short(_message, 1 + static_cast<std::size_t>(_need));
                    _running_status = _byte;
                    _index += 1 + static_cast<std::size_t>(_need);
                } else {
                    _index = data.size();
                }
                continue;
            }
            ++_index;
            continue;
        }

        if (_running_status) {
            const int _need = data_count_for_status(_running_status);
            if (_need == 1) {
                const unsigned char _message[2] = { _running_status, data[_index] };
                send_short(_message, 2);
                ++_index;
            } else if (_need == 2) {
                if (_index + 1 < data.size()) {
                    unsigned char _message[3] = { _running_status, data[_index], data[_index + 1] };
                    send_short(_message, 3);
                    _index += 2;
                } else {
                    ++_index;
                }
            } else {
                ++_index;
            }
            continue;
        }

        ++_index;
    }
}

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

void send_to_hardware_output(const std::vector<unsigned char>& msg)
{
    std::lock_guard<std::mutex> _lock_guard(global_hardware_mutex);
    if (global_hardware_midiout.isPortOpen()) {
        split_and_send(msg);
    }
}

void open_virtual_input(const std::string& port, const std::function<void(const std::vector<unsigned char>&)>& callback)
{
    if (global_is_virtual_running.load()) {
        return;
    }

    load_vtmidi_library();

    const DWORD _flags = 0;
    const DWORD _max_sysex_size = 65535;
    global_virtual_midiout = vtmidi_create_ex2(to_wstring(port).c_str(), nullptr, nullptr, _max_sysex_size, _flags);
    if (!global_virtual_midiout) {
        throw std::runtime_error("CreatePortEx2 failed");
    }

    global_is_virtual_running = true;
    global_virtual_thread = std::thread([callback] {
        std::vector<unsigned char> _buffer(65536);
        while (global_is_virtual_running.load()) {

            DWORD _size;
            if (vtmidi_get_data(global_virtual_midiout, _buffer.data(), &_size)) {
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
        vtmidi_close(global_virtual_midiout);
        global_virtual_midiout = nullptr;
    }

    unload_vtmidi_library();
}

bool is_virtual_input_open()
{
    return global_is_virtual_running.load();
}
