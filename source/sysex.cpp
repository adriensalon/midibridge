#include "sysex.hpp"

#include <fstream>

std::vector<unsigned char> read_all(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    f.seekg(0, std::ios::end);
    auto n = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<unsigned char> buf((size_t)n);
    if (n > 0)
        f.read((char*)buf.data(), n);
    return buf;
}

std::vector<std::vector<unsigned char>> split(const std::vector<unsigned char>& buf)
{
    std::vector<std::vector<unsigned char>> out;
    size_t i = 0;
    while (i < buf.size()) {
        while (i < buf.size() && buf[i] != 0xF0)
            ++i;
        if (i >= buf.size())
            break;
        size_t s = i++;
        while (i < buf.size() && buf[i] != 0xF7)
            ++i;
        if (i < buf.size())
            out.emplace_back(buf.begin() + s, buf.begin() + i + 1);
        if (i < buf.size())
            ++i;
    }
    return out;
}

std::string guess_dx7_name(const std::vector<unsigned char>& msg, int idx)
{
    for (size_t i = 0; i + 10 <= msg.size(); ++i) {
        bool ok = true;
        for (size_t k = 0; k < 10; ++k) {
            auto c = msg[i + k];
            if (c < 0x20 || c > 0x7E) {
                ok = false;
                break;
            }
        }
        if (ok)
            return std::string((const char*)&msg[i], 10);
    }
    return "Voice " + std::to_string(idx + 1);
}

std::vector<sysex_patch> load_sysex_file(const std::string& path)
{
    std::vector<sysex_patch> patches;
    auto raw = read_all(path);
    if (raw.empty())
        return patches;
    auto pkts = split(raw);
    patches.reserve(pkts.size());
    for (size_t i = 0; i < pkts.size(); ++i)
        patches.push_back(sysex_patch { guess_dx7_name(pkts[i], (int)i), std::move(pkts[i]) });
    return patches;
}
