#include "sysex.hpp"

#include <fstream>

namespace {

[[nodiscard]] static std::vector<unsigned char> read_all(const std::filesystem::path& sysex_path)
{
    std::ifstream _fstream(sysex_path, std::ios::binary);
    if (!_fstream) {
        return {};
    }
    _fstream.seekg(0, std::ios::end);
    std::streampos _position = _fstream.tellg();
    _fstream.seekg(0, std::ios::beg);
    std::vector<unsigned char> _buffer((size_t)std::max<std::streamoff>(0, _position));
    if (!_buffer.empty()) {
        _fstream.read((char*)_buffer.data(), _buffer.size());
    }
    return _buffer;
}

[[nodiscard]] static std::size_t yamaha_count(const std::vector<unsigned char>& message)
{
    if (message.size() < 7) {
        return 0;
    }
    return (size_t(message[4]) << 7) | size_t(message[5]); // MS7 | LS7
}

[[nodiscard]] static bool is_yamaha(const std::vector<unsigned char>& message)
{
    return message.size() >= 2 && message[0] == 0xF0 && message[1] == 0x43;
}

[[nodiscard]] static bool is_dx7_bank32(const std::vector<unsigned char>& message)
{
    return is_yamaha(message) && message.size() >= 7 && message[3] == 0x09 && yamaha_count(message) == 4096;
}

[[nodiscard]] static bool is_dx7_single_voice_message(const std::vector<unsigned char>& message)
{
    return is_yamaha(message) && message.size() >= 7 && message[3] == 0x00 && yamaha_count(message) == 155;
}

[[nodiscard]] static std::string clean_ascii_10(const char* data)
{
    std::string _data(data, 10);
    for (char& _char : _data) {
        const unsigned char _unsigned = static_cast<unsigned char>(_char);
        if (_unsigned < 0x20 || _unsigned > 0x7E) {
            _char = ' ';
        }
    }
    const std::size_t _position = _data.find_last_not_of(' ');
    if (_position == std::string::npos) {
        return "Voice";
    }
    _data.resize(_position + 1);
    return _data;
}

[[nodiscard]] static std::string name_from_chunk(const unsigned char* chunk128)
{
    return clean_ascii_10(reinterpret_cast<const char*>(chunk128) + 118);
}

[[nodiscard]] static std::string name_from_single_voice_message(const std::vector<unsigned char>& message)
{
    // DX7 single-voice: F0 43 0n 00 01 1B [155 params] chk F7
    // name is last 10 bytes of the 155-byte param block (offset 6+145)
    if (message.size() >= 6 + 155 + 1 + 1) {
        return clean_ascii_10(reinterpret_cast<const char*>(message.data()) + 6 + 145);
    }
    return "Voice";
}

[[nodiscard]] static unsigned char yamaha_checksum(const unsigned char* data, std::size_t length)
{
    // checksum = (128 - (sum & 0x7F)) & 0x7F
    unsigned int _sum = 0;
    for (size_t i = 0; i < length; ++i) {
        _sum += data[i];
    }
    return unsigned char((128 - (_sum & 0x7F)) & 0x7F);
}

[[nodiscard]] static std::vector<unsigned char> dx7_chunk128_to_param155(const unsigned char* c)
{
    // Unpack one 128-unsigned char bank chunk → 155 single-voice parameter bytes (no header/checksum yet)
    std::vector<unsigned char> _parameter;
    _parameter.reserve(155);

    auto unpack_op = [&](int base) {
        // bytes base..base+16 (17 bytes) as per packed map (operator order 6..1)
        const unsigned char b0 = c[base + 0], b1 = c[base + 1], b2 = c[base + 2], b3 = c[base + 3];
        const unsigned char b4 = c[base + 4], b5 = c[base + 5], b6 = c[base + 6], b7 = c[base + 7];
        const unsigned char b8 = c[base + 8], b9 = c[base + 9], b10 = c[base + 10], b11 = c[base + 11];
        const unsigned char b12 = c[base + 12], b13 = c[base + 13], b14 = c[base + 14], b15 = c[base + 15], b16 = c[base + 16];

        // EG rates/levels
        _parameter.push_back(b0);
        _parameter.push_back(b1);
        _parameter.push_back(b2);
        _parameter.push_back(b3);
        _parameter.push_back(b4);
        _parameter.push_back(b5);
        _parameter.push_back(b6);
        _parameter.push_back(b7);

        // Keyboard scaling
        _parameter.push_back(b8); // break point (0-99)
        _parameter.push_back(b9); // left depth
        _parameter.push_back(b10); // right depth

        // curves (byte11: 0 0 0 | RC(2) | LC(2))
        const unsigned char LC = (b11)&0x03;
        const unsigned char RC = (b11 >> 2) & 0x03;
        _parameter.push_back(LC); // left curve
        _parameter.push_back(RC); // right curve

        // byte12: | DET(4) | RS(3) |
        const unsigned char RS = (b12)&0x07;
        const unsigned char DET = (b12 >> 3) & 0x0F;

        // byte13: 0 0 | KVS(3) | AMS(2) |
        unsigned char AMS = (b13)&0x03;
        unsigned char KVS = (b13 >> 2) & 0x07;

        _parameter.push_back(RS); // rate scaling
        _parameter.push_back(AMS); // amp mod sens
        _parameter.push_back(KVS); // key vel sens

        _parameter.push_back(b14); // output level

        // byte15: 0 | FC(5) | M(1)
        unsigned char M = b15 & 0x01;
        unsigned char FC = (b15 >> 1) & 0x1F;

        _parameter.push_back(M); // osc mode
        _parameter.push_back(FC); // coarse
        _parameter.push_back(b16); // fine
        _parameter.push_back(DET); // detune (0..14)
    };

    // Operators in order OP6..OP1, 17 bytes each starting at 0,17,34,51,68,85
    unpack_op(0);
    unpack_op(17);
    unpack_op(34);
    unpack_op(51);
    unpack_op(68);
    unpack_op(85);

    // Pitch EG (bytes 102..109)
    _parameter.push_back(c[102]);
    _parameter.push_back(c[103]);
    _parameter.push_back(c[104]);
    _parameter.push_back(c[105]);
    _parameter.push_back(c[106]);
    _parameter.push_back(c[107]);
    _parameter.push_back(c[108]);
    _parameter.push_back(c[109]);

    // Alg (byte110: 0 0 | ALG(5))
    _parameter.push_back(c[110] & 0x1F);

    // FB + Key Sync (byte111: 0 0 0 | OKS(1) | FB(3))
    const unsigned char b111 = c[111];
    const unsigned char FB = b111 & 0x07;
    const unsigned char OKS = (b111 >> 3) & 0x01;
    _parameter.push_back(FB);
    _parameter.push_back(OKS);

    // LFO speed/delay/pitch-mod depth/amp-mod depth (112..115)
    _parameter.push_back(c[112]);
    _parameter.push_back(c[113]);
    _parameter.push_back(c[114]);
    _parameter.push_back(c[115]);

    // byte116: | LPMS(3) | LFW(3) | LKS(1) |
    const unsigned char b116 = c[116];
    const unsigned char LKS = b116 & 0x01; // LFO sync
    const unsigned char LFW = (b116 >> 1) & 0x07; // LFO wave
    const unsigned char LPMS = (b116 >> 4) & 0x07; // pitch mod sens
    _parameter.push_back(LKS);
    _parameter.push_back(LFW);
    _parameter.push_back(LPMS);

    // Transpose (117)
    _parameter.push_back(c[117]);

    // Name chars (118..127)
    for (int _index = 118; _index <= 127; ++_index) {
        _parameter.push_back(c[_index]);
    }

    // Size check
    if (_parameter.size() != 155) { /* defensive, but it should be 155 */
    }
    return _parameter;
}

[[nodiscard]] static std::vector<unsigned char> build_single_voice_sysex_from_parameters(const std::vector<unsigned char>& params155, int midiChannel /*0..15*/ = 0)
{
    // Build full single-voice SysEx from 155 params (adds header+checksum+F7)
    std::vector<unsigned char> message;
    message.reserve(1 + 1 + 1 + 1 + 2 + 155 + 1 + 1);
    message.push_back(0xF0);
    message.push_back(0x43); // Yamaha
    message.push_back(unsigned char(0x00 | (midiChannel & 0x0F))); // sub-status 0x0, channel nibble
    message.push_back(0x00); // format 0 = single voice
    // 155 = 1*128 + 27
    message.push_back(0x01); // unsigned char count MS (7-bit)
    message.push_back(0x1B); // unsigned char count LS (7-bit)
    message.insert(message.end(), params155.begin(), params155.end());
    message.push_back(yamaha_checksum(params155.data(), params155.size()));
    message.push_back(0xF7);
    return message;
}

[[nodiscard]] static std::vector<std::vector<unsigned char>> split_sysex_all(const std::vector<unsigned char>& data)
{
    std::vector<std::vector<unsigned char>> _split_data;
    std::size_t _index = 0;
    while (_index < data.size()) {
        // find F0
        while (_index < data.size() && data[_index] != 0xF0) {
            ++_index;
        }
        if (_index >= data.size()) {
            break;
        }
        const std::size_t _size = _index++;
        // find F7
        while (_index < data.size() && data[_index] != 0xF7) {
            ++_index;
        }
        if (_index < data.size()) {
            _split_data.emplace_back(data.begin() + _size, data.begin() + _index + 1);
            ++_index; // continue after F7
        } else {
            break; // unterminated at EOF -> stop
        }
    }
    return _split_data;
}

}

std::vector<std::filesystem::path> load_sysex_banks_recursive(const std::filesystem::path& root_path)
{
    std::vector<std::filesystem::path> _sysex_banks;
    std::error_code _error;
    for (std::filesystem::recursive_directory_iterator _iterator(root_path, _error), end; _iterator != end; _iterator.increment(_error)) {
        if (_error) {
            continue;
        }
        if (!_iterator->is_regular_file(_error)) {
            continue;
        }
        std::string _extension = _iterator->path().extension().string();
        std::transform(_extension.begin(), _extension.end(), _extension.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (_extension == ".syx") {
            const std::filesystem::path _path = _iterator->path();
            _sysex_banks.push_back(_iterator->path());
        }
    }
    return _sysex_banks;
}

std::vector<sysex_patch> load_sysex_patches(const std::filesystem::path& bank)
{
    std::vector<sysex_patch> _sysex_patches;
    const std::vector<unsigned char> _raw_data = read_all(bank);
    const std::vector<std::vector<unsigned char>> _split_data = split_sysex_all(_raw_data);
    if (_split_data.empty()) {
        return _sysex_patches;
    }

    int _single_voice_index = 0;
    int _other_index = 0;
    for (const std::vector<unsigned char>& _message : _split_data) {
        if (!is_yamaha(_message)) {
            // Unknown vendor: still expose as a patch with a generic name
            sysex_patch _patch;
            _patch.name = bank.filename().string() + " (message " + std::to_string(++_other_index) + ")";
            _patch.data = _message;
            _sysex_patches.push_back(std::move(_patch));
            continue;
        }

        if (is_dx7_bank32(_message)) {
            // Explode 32-voice bank into 32 single-voice messages
            const std::size_t _data_offset = 6;
            const unsigned char* data = _message.data() + _data_offset;
            for (int _index = 0; _index < 32; ++_index) {
                const unsigned char* _chunk = data + _index * 128;
                std::vector<unsigned char> _parameters = dx7_chunk128_to_param155(_chunk);
                std::vector<unsigned char> _patch_message = build_single_voice_sysex_from_parameters(_parameters, /*channel*/ 0);
                sysex_patch _patch;
                _patch.name = name_from_chunk(_chunk);
                _patch.data = std::move(_patch_message);
                _sysex_patches.push_back(std::move(_patch));
            }
            continue;
        }

        if (is_dx7_single_voice_message(_message)) {
            sysex_patch _patch;
            _patch.name = name_from_single_voice_message(_message);
            if (_patch.name == "Voice") {
                // If name not present, label with filename + index to avoid duplicates
                _patch.name = bank.stem().string() + " (Voice " + std::to_string(++_single_voice_index) + ")";
            }
            _patch.data = _message; // already a complete single-voice F0..F7
            _sysex_patches.push_back(std::move(_patch));
            continue;
        }

        // Other Yamaha formats (DX7II/TX etc.) — expose raw message
        sysex_patch _patch;
        _patch.name = bank.filename().string() + " (Yamaha message " + std::to_string(++_other_index) + ")";
        _patch.data = _message;
        _sysex_patches.push_back(std::move(_patch));
    }

    return _sysex_patches;
}
