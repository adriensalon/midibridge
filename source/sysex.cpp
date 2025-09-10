#include "sysex.hpp"

#include <fstream>
namespace {
std::vector<unsigned char> read_all(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return {};
    f.seekg(0, std::ios::end);
    auto n = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<unsigned char> buf((size_t)std::max<std::streamoff>(0, n));
    if (!buf.empty())
        f.read((char*)buf.data(), buf.size());
    return buf;
}

// First F0..F7 block in a buffer
std::vector<unsigned char> first_sysex(const std::vector<unsigned char>& buf)
{
    size_t i = 0;
    while (i < buf.size() && buf[i] != 0xF0)
        ++i;
    if (i >= buf.size())
        return {};
    size_t s = i++;
    while (i < buf.size() && buf[i] != 0xF7)
        ++i;
    if (i < buf.size())
        return std::vector<unsigned char>(buf.begin() + s, buf.begin() + i + 1);
    return {};
}

inline size_t yamaha_count(const std::vector<unsigned char>& msg)
{
    if (msg.size() < 7)
        return 0;
    return (size_t(msg[4]) << 7) | size_t(msg[5]); // MS7 | LS7
}

inline bool is_yamaha(const std::vector<unsigned char>& msg) { return msg.size() >= 2 && msg[0] == 0xF0 && msg[1] == 0x43; }

inline bool is_dx7_bank32(const std::vector<unsigned char>& msg)
{
    return is_yamaha(msg) && msg.size() >= 7 && msg[3] == 0x09 && yamaha_count(msg) == 4096;
}
inline bool is_dx7_single_voice_msg(const std::vector<unsigned char>& msg)
{
    return is_yamaha(msg) && msg.size() >= 7 && msg[3] == 0x00 && yamaha_count(msg) == 155;
}

// ASCII 10-char from 128-unsigned char chunk at bytes 118..127
std::string name_from_chunk(const unsigned char* chunk128)
{
    std::string s((const char*)chunk128 + 118, 10);
    for (char& c : s)
        if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E)
            c = ' ';
    while (!s.empty() && s.back() == ' ')
        s.pop_back();
    if (s.empty())
        s = "Voice";
    return s;
}

// checksum = (128 - (sum & 0x7F)) & 0x7F
unsigned char yamaha_checksum(const unsigned char* data, size_t len)
{
    unsigned int sum = 0;
    for (size_t i = 0; i < len; ++i)
        sum += data[i];
    return unsigned char((128 - (sum & 0x7F)) & 0x7F);
}

// Unpack one 128-unsigned char bank chunk → 155 single-voice parameter bytes (no header/checksum yet)
std::vector<unsigned char> dx7_chunk128_to_param155(const unsigned char* c)
{
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

// Build full single-voice SysEx from 155 params (adds header+checksum+F7)
std::vector<unsigned char> build_single_voice_sysex_from_params(const std::vector<unsigned char>& params155, int midiChannel /*0..15*/ = 0)
{
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

} // anon

// PUBLIC: load all .syx recursively and fill both bank.data and per-voice patches
std::vector<sysex_bank> load_sysex_banks_recursive(const std::filesystem::path& root_path)
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
        if (ext == ".syx")
            files.push_back(it->path());
    }
    std::sort(files.begin(), files.end(),
        [](const auto& a, const auto& b) { return a.generic_string() < b.generic_string(); });

    std::vector<sysex_bank> out;
    out.reserve(files.size());

    for (const auto& f : files) {
        auto raw = read_all(f);
        auto msg = first_sysex(raw);
        if (msg.empty() || !is_yamaha(msg))
            continue;

        // nice display name = relative path stem (no .syx)
        std::filesystem::path rel;
        std::error_code ec2;
        rel = std::filesystem::relative(f, root_path, ec2);
        if (ec2 || rel.empty())
            rel = f.filename();
        rel.replace_extension();
        std::string bankName = rel.generic_string();

        sysex_bank bank;
        bank.name = std::move(bankName);

        if (is_dx7_bank32(msg)) {
            // Keep the full bank message
            bank.data = msg;

            // Explode into 32 single-voice SysEx patches
            const size_t data_off = 6; // after header/count (F0 43 .. 09 MS LS)
            const unsigned char* data = msg.data() + data_off;
            for (int i = 0; i < 32; ++i) {
                const unsigned char* chunk = data + i * 128;
                auto params = dx7_chunk128_to_param155(chunk);
                auto patchMsg = build_single_voice_sysex_from_params(params, /*channel*/ 0);

                sysex_patch p;
                p.name = name_from_chunk(chunk);
                p.data = std::move(patchMsg);
                bank.patches.push_back(std::move(p));
            }
        } else if (is_dx7_single_voice_msg(msg)) {
            bank.data = msg; // <— make bank.data the same full F0..F7
            // Name: try 10 ASCII chars near the end; else use stem
            std::string nm = rel.stem().string();
            for (size_t i = 0; i + 10 <= msg.size(); ++i) {
                bool ok = true;
                for (size_t k = 0; k < 10; ++k) {
                    unsigned char c = msg[i + k];
                    if (c < 0x20 || c > 0x7E) {
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    nm.assign((const char*)&msg[i], 10);
                    break;
                }
            }
            bank.patches.push_back(sysex_patch { nm, msg }); // patch.data == bank.data (by value)
        } else {
            // Unknown Yamaha (DX7II/TX, etc.) — keep consistent:
            bank.data = msg; // <— store full message as the bank data
            bank.patches.push_back(sysex_patch { rel.filename().string(), msg });
        }

        if (!bank.patches.empty() || !bank.data.empty())
            out.push_back(std::move(bank));
    }

    return out;
}