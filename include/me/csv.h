// csv.h — parse a stream of commands from a CSV file.
//
// Schema (one command per line; '#' and blank lines ignored):
//   action,side,type,id,price,qty,tif
//     action : A=add  C=cancel  M=modify
//     side   : B=buy  S=sell                 (ignored for C)
//     type   : L=limit M=market              (ignored for C; M re-add uses L)
//     id     : order id (uint)
//     price  : integer tick (ignored for market)
//     qty    : quantity
//     tif    : GTC (default) | IOC | FOK      (optional)
// Examples:
//   A,B,L,1,100,10,GTC
//   A,S,M,2,0,5
//   C,,,1
//   M,,,3,102,7
#ifndef ME_CSV_H
#define ME_CSV_H

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "me/types.h"

namespace me {

inline std::vector<std::string> split_commas(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream is(s);
    while (std::getline(is, cur, ',')) out.push_back(cur);
    if (!s.empty() && s.back() == ',') out.push_back("");
    return out;
}

// Returns true if `out` was populated; false for blank/comment/garbage lines.
inline bool parse_command(const std::string& raw, Command& out) {
    // strip CR (Windows line endings when read in binary-ish mode) and spaces
    std::string line;
    for (char c : raw) if (c != '\r' && c != ' ' && c != '\t') line += c;
    if (line.empty() || line[0] == '#') return false;

    auto f = split_commas(line);
    if (f.empty()) return false;

    auto to_u64 = [](const std::string& s) -> std::uint64_t {
        if (s.empty()) return 0;
        return std::strtoull(s.c_str(), nullptr, 10);
    };
    auto to_i64 = [](const std::string& s) -> std::int64_t {
        if (s.empty()) return 0;
        return std::strtoll(s.c_str(), nullptr, 10);
    };

    const char act = f[0].empty() ? '?' : f[0][0];
    out = Command{};
    if (act == 'A' || act == 'a') {
        if (f.size() < 6) return false;
        out.type     = CmdType::Add;
        out.side     = (f[1] == "S" || f[1] == "s") ? Side::Sell : Side::Buy;
        out.ord_type = (f[2] == "M" || f[2] == "m") ? OrderType::Market : OrderType::Limit;
        out.id       = to_u64(f[3]);
        out.price    = to_i64(f[4]);
        out.qty      = to_u64(f[5]);
        if (f.size() >= 7) {
            if      (f[6] == "IOC" || f[6] == "ioc") out.tif = Tif::Ioc;
            else if (f[6] == "FOK" || f[6] == "fok") out.tif = Tif::Fok;
            else                                     out.tif = Tif::Gtc;
        }
        return out.qty > 0;
    }
    if (act == 'C' || act == 'c') {
        if (f.size() < 4) return false;
        out.type = CmdType::Cancel;
        out.id   = to_u64(f[3]);
        return true;
    }
    if (act == 'M' || act == 'm') {
        if (f.size() < 6) return false;
        out.type  = CmdType::Modify;
        out.id    = to_u64(f[3]);
        out.price = to_i64(f[4]);
        out.qty   = to_u64(f[5]);
        return true;
    }
    return false;
}

inline std::vector<Command> load_csv(const std::string& path) {
    std::vector<Command> cmds;
    std::ifstream in(path);
    std::string line;
    Command c;
    while (std::getline(in, line))
        if (parse_command(line, c)) cmds.push_back(c);
    return cmds;
}

}  // namespace me

#endif  // ME_CSV_H
