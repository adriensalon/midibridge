#include "sysex.hpp"

#include <fstream>

namespace {

static std::vector<unsigned char> read_all(const std::filesystem::path& p)
{
    std::ifstream _fstream(p, std::ios::binary);
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

static std::size_t yamaha_count(const std::vector<unsigned char>& msg)
{
    if (msg.size() < 7)
        return 0;
    return (size_t(msg[4]) << 7) | size_t(msg[5]); // MS7 | LS7
}

static bool is_yamaha(const std::vector<unsigned char>& msg) { return msg.size() >= 2 && msg[0] == 0xF0 && msg[1] == 0x43; }

static bool is_dx7_bank32(const std::vector<unsigned char>& msg)
{
    return is_yamaha(msg) && msg.size() >= 7 && msg[3] == 0x09 && yamaha_count(msg) == 4096;
}

static bool is_dx7_single_voice_msg(const std::vector<unsigned char>& msg)
{
    return is_yamaha(msg) && msg.size() >= 7 && msg[3] == 0x00 && yamaha_count(msg) == 155;
}

static std::string clean_ascii_10(const char* p10)
{
    std::string s(p10, 10);
    for (char& c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u > 0x7E)
            c = ' ';
    }
    // rtrim spaces
    size_t pos = s.find_last_not_of(' ');
    if (pos == std::string::npos)
        return "Voice";
    s.resize(pos + 1);
    return s;
}

static std::string name_from_chunk(const unsigned char* chunk128)
{
    return clean_ascii_10(reinterpret_cast<const char*>(chunk128) + 118);
}

static std::string name_from_single_voice_msg(const std::vector<unsigned char>& msg)
{
    // DX7 single-voice: F0 43 0n 00 01 1B [155 params] chk F7
    // name is last 10 bytes of the 155-byte param block (offset 6+145)
    if (msg.size() >= 6 + 155 + 1 + 1) {
        return clean_ascii_10(reinterpret_cast<const char*>(msg.data()) + 6 + 145);
    }
    return "Voice";
}

static unsigned char yamaha_checksum(const unsigned char* data, size_t len)
{
    // checksum = (128 - (sum & 0x7F)) & 0x7F
    unsigned int sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum += data[i];
    }
    return unsigned char((128 - (sum & 0x7F)) & 0x7F);
}

static std::vector<unsigned char> dx7_chunk128_to_param155(const unsigned char* c)
{
    // Unpack one 128-unsigned char bank chunk → 155 single-voice parameter bytes (no header/checksum yet)
    std::vector<unsigned char> P;
    P.reserve(155);

    auto unpack_op = [&](int base) {
        // bytes base..base+16 (17 bytes) as per packed map (operator order 6..1)
        unsigned char b0 = c[base + 0], b1 = c[base + 1], b2 = c[base + 2], b3 = c[base + 3];
        unsigned char b4 = c[base + 4], b5 = c[base + 5], b6 = c[base + 6], b7 = c[base + 7];
        unsigned char b8 = c[base + 8], b9 = c[base + 9], b10 = c[base + 10], b11 = c[base + 11];
        unsigned char b12 = c[base + 12], b13 = c[base + 13], b14 = c[base + 14], b15 = c[base + 15], b16 = c[base + 16];

        // EG rates/levels
        P.push_back(b0);
        P.push_back(b1);
        P.push_back(b2);
        P.push_back(b3);
        P.push_back(b4);
        P.push_back(b5);
        P.push_back(b6);
        P.push_back(b7);

        // Keyboard scaling
        P.push_back(b8); // break point (0-99)
        P.push_back(b9); // left depth
        P.push_back(b10); // right depth

        // curves (byte11: 0 0 0 | RC(2) | LC(2))
        unsigned char LC = (b11)&0x03;
        unsigned char RC = (b11 >> 2) & 0x03;
        P.push_back(LC); // left curve
        P.push_back(RC); // right curve

        // byte12: | DET(4) | RS(3) |
        unsigned char RS = (b12)&0x07;
        unsigned char DET = (b12 >> 3) & 0x0F;

        // byte13: 0 0 | KVS(3) | AMS(2) |
        unsigned char AMS = (b13)&0x03;
        unsigned char KVS = (b13 >> 2) & 0x07;

        P.push_back(RS); // rate scaling
        P.push_back(AMS); // amp mod sens
        P.push_back(KVS); // key vel sens

        P.push_back(b14); // output level

        // byte15: 0 | FC(5) | M(1)
        unsigned char M = b15 & 0x01;
        unsigned char FC = (b15 >> 1) & 0x1F;

        P.push_back(M); // osc mode
        P.push_back(FC); // coarse
        P.push_back(b16); // fine
        P.push_back(DET); // detune (0..14)
    };

    // Operators in order OP6..OP1, 17 bytes each starting at 0,17,34,51,68,85
    unpack_op(0);
    unpack_op(17);
    unpack_op(34);
    unpack_op(51);
    unpack_op(68);
    unpack_op(85);

    // Pitch EG (bytes 102..109)
    P.push_back(c[102]);
    P.push_back(c[103]);
    P.push_back(c[104]);
    P.push_back(c[105]);
    P.push_back(c[106]);
    P.push_back(c[107]);
    P.push_back(c[108]);
    P.push_back(c[109]);

    // Alg (byte110: 0 0 | ALG(5))
    P.push_back(c[110] & 0x1F);

    // FB + Key Sync (byte111: 0 0 0 | OKS(1) | FB(3))
    unsigned char b111 = c[111];
    unsigned char FB = b111 & 0x07;
    unsigned char OKS = (b111 >> 3) & 0x01;
    P.push_back(FB);
    P.push_back(OKS);

    // LFO speed/delay/pitch-mod depth/amp-mod depth (112..115)
    P.push_back(c[112]);
    P.push_back(c[113]);
    P.push_back(c[114]);
    P.push_back(c[115]);

    // byte116: | LPMS(3) | LFW(3) | LKS(1) |
    unsigned char b116 = c[116];
    unsigned char LKS = b116 & 0x01; // LFO sync
    unsigned char LFW = (b116 >> 1) & 0x07; // LFO wave
    unsigned char LPMS = (b116 >> 4) & 0x07; // pitch mod sens
    P.push_back(LKS);
    P.push_back(LFW);
    P.push_back(LPMS);

    // Transpose (117)
    P.push_back(c[117]);

    // Name chars (118..127)
    for (int i = 118; i <= 127; ++i)
        P.push_back(c[i]);

    // Size check
    if (P.size() != 155) { /* defensive, but it should be 155 */
    }
    return P;
}

static std::vector<unsigned char> build_single_voice_sysex_from_params(const std::vector<unsigned char>& params155, int midiChannel /*0..15*/ = 0)
{
    // Build full single-voice SysEx from 155 params (adds header+checksum+F7)
    std::vector<unsigned char> msg;
    msg.reserve(1 + 1 + 1 + 1 + 2 + 155 + 1 + 1);
    msg.push_back(0xF0);
    msg.push_back(0x43); // Yamaha
    msg.push_back(unsigned char(0x00 | (midiChannel & 0x0F))); // sub-status 0x0, channel nibble
    msg.push_back(0x00); // format 0 = single voice
    // 155 = 1*128 + 27
    msg.push_back(0x01); // unsigned char count MS (7-bit)
    msg.push_back(0x1B); // unsigned char count LS (7-bit)
    msg.insert(msg.end(), params155.begin(), params155.end());
    msg.push_back(yamaha_checksum(params155.data(), params155.size()));
    msg.push_back(0xF7);
    return msg;
}

static std::vector<std::vector<unsigned char>> split_sysex_all(const std::vector<unsigned char>& buf)
{
    std::vector<std::vector<unsigned char>> out;
    size_t i = 0;
    while (i < buf.size()) {
        // find F0
        while (i < buf.size() && buf[i] != 0xF0)
            ++i;
        if (i >= buf.size())
            break;
        size_t s = i++;
        // find F7
        while (i < buf.size() && buf[i] != 0xF7)
            ++i;
        if (i < buf.size()) {
            out.emplace_back(buf.begin() + s, buf.begin() + i + 1);
            ++i; // continue after F7
        } else {
            break; // unterminated at EOF -> stop
        }
    }
    return out;
}

}

std::vector<std::filesystem::path> load_sysex_banks_recursive(const std::filesystem::path& root_path)
{
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root_path, ec), end; it != end; it.increment(ec)) {
        if (ec)
            continue;
        if (!it->is_regular_file(ec))
            continue;
        auto ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (ext == ".syx") {
            const std::filesystem::path _path = it->path();
            files.push_back(it->path());
        }
    }
    // std::sort(files.begin(), files.end(),
    //     [](const auto& a, const auto& b) { return a.generic_string() < b.generic_string(); });

    return files;
}

std::vector<sysex_patch> load_sysex_patches(const std::filesystem::path& bank)
{
    std::vector<sysex_patch> out;

    // Read file and split into ALL sysex messages
    auto raw = read_all(bank);
    auto msgs = split_sysex_all(raw);
    if (msgs.empty())
        return out;

    int single_voice_index = 0;
    int other_index = 0;

    for (const auto& msg : msgs) {
        if (!is_yamaha(msg)) {
            // Unknown vendor: still expose as a patch with a generic name
            sysex_patch p;
            p.name = bank.filename().string() + " (Msg " + std::to_string(++other_index) + ")";
            p.data = msg;
            out.push_back(std::move(p));
            continue;
        }

        if (is_dx7_bank32(msg)) {
            // Explode 32-voice bank into 32 single-voice messages
            const size_t data_off = 6;
            const unsigned char* data = msg.data() + data_off;
            for (int i = 0; i < 32; ++i) {
                const unsigned char* chunk = data + i * 128;
                auto params = dx7_chunk128_to_param155(chunk);
                auto patchMsg = build_single_voice_sysex_from_params(params, /*channel*/ 0);

                sysex_patch p;
                p.name = name_from_chunk(chunk);
                p.data = std::move(patchMsg);
                out.push_back(std::move(p));
            }
            continue;
        }

        if (is_dx7_single_voice_msg(msg)) {
            sysex_patch p;
            p.name = name_from_single_voice_msg(msg);
            if (p.name == "Voice") {
                // If name not present, label with filename + index to avoid duplicates
                p.name = bank.stem().string() + " (Voice " + std::to_string(++single_voice_index) + ")";
            }
            p.data = msg; // already a complete single-voice F0..F7
            out.push_back(std::move(p));
            continue;
        }

        // Other Yamaha formats (DX7II/TX etc.) — expose raw message
        sysex_patch p;
        p.name = bank.filename().string() + " (Yamaha Msg " + std::to_string(++other_index) + ")";
        p.data = msg;
        out.push_back(std::move(p));
    }

    return out;
}
