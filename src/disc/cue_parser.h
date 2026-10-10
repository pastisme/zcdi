#pragma once
#include <string>
#include <vector>

namespace zcdi {

struct CueFile {
    std::string name; // FILE "<name>" ...
    std::string type; // BINARY, MOTOROLA, WAVE, ...
};

struct CueTrack {
    int number = 0;
    std::string mode; // e.g. "MODE2/2352", "AUDIO"
    int file = 0; // index into CueSheet::files
    // (m*60+s)*75+f from the start of the track's own FILE: no -150 correction.
    int index01_frames = 0;
    int pregap_frames = 0; // PREGAP: not stored in the file
};

struct CueSheet {
    std::vector<CueFile> files;
    std::vector<CueTrack> tracks;
};

// Parses cue text, not a path. Fails if there is no track, or a track comes
// before any FILE.
bool parse_cue(const std::string& text, CueSheet& out);

} // namespace zcdi
