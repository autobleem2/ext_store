//
// The page at / - what abstored serves and what it could not: the source's URL to add in the Store, then every
// game (title, serial, discs, size, whether its checksums are ready) and every problem the last scan found.
//
#pragma once

#include "lan_library.h"

#include <map>
#include <string>

struct IndexPageFacts {
    std::string name;    // the source's display name
    std::string baseUrl; // "http://192.168.1.20:8124"
    std::string gamesDir;
    std::string version;
    bool hashing = false; // checksums still being worked out
};

std::string indexPage(const LanSnapshot &snapshot, const std::map<std::string, std::string> &checksums,
                      const IndexPageFacts &facts);
std::string htmlEscape(const std::string &text);
