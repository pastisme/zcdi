#include "disc/cue_parser.h"

#include <sstream>

namespace zcdi {

namespace {
std::string unquote(const std::string& s) {
    auto a = s.find('"');
    auto b = s.find_last_of('"');
    if (a != std::string::npos && b != std::string::npos && b > a) return s.substr(a + 1, b - a - 1);
    return s;
}
// Ends an unquoted FILE name.
bool is_file_type(const std::string& tok) {
    return tok == "BINARY" || tok == "WAVE" || tok == "MP3" || tok == "AIFF" || tok == "MOTOROLA";
}
CueFile parse_file(const std::string& rest) {
    CueFile file;
    std::string after_name;
    if (rest.find('"') != std::string::npos) {
        file.name = unquote(rest);
        after_name = rest.substr(rest.find_last_of('"') + 1);
    } else {
        std::istringstream ls(rest);
        std::string tok;
        while (ls >> tok) {
            if (is_file_type(tok)) {
                after_name = tok;
                break;
            }
            if (!file.name.empty()) file.name += " ";
            file.name += tok;
        }
    }
    std::istringstream ls(after_name);
    ls >> file.type;
    return file;
}
// "mm:ss:ff" -> frames, no -150 pregap correction.
bool parse_msf(const std::string& t, int& frames) {
    int m, s, f;
    char c1, c2;
    std::istringstream is(t);
    if ((is >> m >> c1 >> s >> c2 >> f) && c1 == ':' && c2 == ':') {
        frames = (m * 60 + s) * 75 + f;
        return true;
    }
    return false;
}
} // namespace

bool parse_cue(const std::string& text, CueSheet& out) {
    out = CueSheet{};
    std::istringstream in(text);
    std::string line;
    CueTrack cur;
    bool have_track = false;

    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string kw;
        ls >> kw;
        // Comments: ";" (the general CUE convention) or "//" (cdrdao).
        if (kw.empty() || kw[0] == ';' || (kw.size() >= 2 && kw[0] == '/' && kw[1] == '/')) continue;
        if (kw == "FILE") {
            std::string rest;
            std::getline(ls, rest);
            out.files.push_back(parse_file(rest));
        } else if (kw == "TRACK") {
            if (out.files.empty()) return false;
            if (have_track) out.tracks.push_back(cur);
            cur = CueTrack{};
            ls >> cur.number >> cur.mode;
            cur.file = int(out.files.size()) - 1;
            have_track = true;
        } else if (kw == "INDEX") {
            int idx = 0;
            std::string msf;
            ls >> idx >> msf;
            int frames = 0;
            if (idx == 1 && parse_msf(msf, frames)) cur.index01_frames = frames;
        } else if (kw == "PREGAP") {
            std::string msf;
            ls >> msf;
            int frames = 0;
            if (parse_msf(msf, frames)) cur.pregap_frames = frames;
        }
    }
    if (have_track) out.tracks.push_back(cur);
    return !out.tracks.empty();
}

} // namespace zcdi
