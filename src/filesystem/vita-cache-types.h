// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MKXP_VITA_CACHE_TYPES_H
#define MKXP_VITA_CACHE_TYPES_H

#include "rgssad.h"
#include <dirent.h>
#include <cerrno>
#include <cctype>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Hints only: the PhysFS callback stream remains the inventory and its order.
class VitaCacheTypes {
    struct Directory {
        DIR *value;
        explicit Directory(const char *path) : value(opendir(path)) {}
        bool close() {
            DIR *handle = value;
            value = nullptr;
            return !handle || closedir(handle) == 0;
        }
        ~Directory() { close(); }
    };
    struct Mount { std::string path, point; bool native, rgss; };
    std::vector<Mount> mounts;
    static std::string folded(std::string name) {
        for (char &c : name) c = std::tolower(static_cast<unsigned char>(c));
        return name;
    }
public:
    struct Listing {
        const Mount *mount;
        std::string relative;
        std::map<std::string, int> types;
        std::map<std::string, int> aliases;
        bool complete = false;
    };
    using Listings = std::vector<Listing>;
    static constexpr size_t typeLimit = 4096, mountLimit = 64;
    VitaCacheTypes() {
        std::unique_ptr<char *, decltype(&PHYSFS_freeList)> paths(PHYSFS_getSearchPath(), PHYSFS_freeList);
        if (!paths) return;
        for (char **path = paths.get(); *path && mounts.size() < mountLimit; ++path) {
            const char *point = PHYSFS_getMountPoint(*path);
            if (!point || strlen(*path) >= 1024 || strlen(point) >= 512) break;
            std::string prefix(point);
            if (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
            const bool rgss = RGSS_pathType(*path, "") != RGSS_UNKNOWN_ARCHIVE;
            bool native = false;
            if (!rgss) {
                Directory root(*path);
                native = root.value != nullptr;
                native = root.close() && native;
            }
            mounts.push_back({*path, prefix, native, rgss});
        }
    }
    Listings list(const char *directory, BootProfile::PathCacheIO &io) const {
        Listings listings;
        size_t count = 0;
        const std::string dir(directory);
        for (const auto &mount : mounts) {
            Listing listing{&mount, {}, {}, {}};
            if (mount.point.empty()) listing.relative = dir;
            else if (dir == mount.point) listing.relative.clear();
            else if (dir.compare(0, mount.point.size() + 1, mount.point + "/") == 0)
                listing.relative = dir.substr(mount.point.size() + 1);
            else if (dir.empty() || mount.point.compare(0, dir.size() + 1, dir + "/") == 0) {
                const size_t begin = dir.empty() ? 0 : dir.size() + 1;
                listing.types.emplace(mount.point.substr(begin, mount.point.find('/', begin) - begin),
                                      PHYSFS_FILETYPE_DIRECTORY);
                listing.mount = nullptr;
                listing.complete = true;
                listings.push_back(std::move(listing));
                if (++count == typeLimit) break;
                continue;
            } else continue;
            listings.push_back(std::move(listing));
            Listing &current = listings.back();
            // Registered RGSS archives can prove absence without touching storage.
            if (mount.rgss) continue;
            if (!mount.native) break;
            const std::string real = mount.path +
                (current.relative.empty() || mount.path.back() == '/' ? "" : "/") + current.relative;
            if (real.size() >= 1024) break;
            struct Timing {
                BootProfile::PathCacheIO &io;
                double start = BootProfile::now();
                ~Timing() { io.readdirUS += BootProfile::now() - start; }
            } timing{io};
            Directory handle(real.c_str());
            if (!handle.value) {
                if (errno == ENOENT || errno == ENOTDIR) { current.complete = true; continue; }
                break;
            }
            bool nonAscii = false;
            while (count < typeLimit) {
                if (shState && shState->rtData().rqTerm)
                    throw Exception(Exception::MKXPError, "Game close requested. Aborting path cache enumeration.");
                errno = 0;
                ++io.readdirCount;
                struct dirent *entry = readdir(handle.value);
                if (!entry) { current.complete = errno == 0; break; }
                if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
                const auto mode = entry->d_stat.st_mode;
                int type = SCE_S_ISDIR(mode) ? PHYSFS_FILETYPE_DIRECTORY :
                    SCE_S_ISREG(mode) ? PHYSFS_FILETYPE_REGULAR : -1;
                for (const unsigned char *c = reinterpret_cast<const unsigned char *>(entry->d_name); *c; ++c)
                    if (*c >= 128) { nonAscii = true; type = -1; }
                if (current.types.emplace(entry->d_name, type).second) ++count;
                auto alias = current.aliases.emplace(folded(entry->d_name), type);
                if (!alias.second && alias.first->second != type) alias.first->second = -1;
            }
            if (!handle.close() || nonAscii) current.complete = false;
            if (!current.complete) break;
        }
        return listings;
    }
    static int type(const Listings &listings, const char *name) {
        // PhysFS sanitizes these paths before consulting an archiver.
        if (!strcmp(name, ".") || !strcmp(name, "..") || strpbrk(name, "\\:/")) return -1;
        int ambiguous = -2;
        auto resolved = [&](int type) { return ambiguous == -2 || ambiguous == type ? type : -1; };
        for (const auto &listing : listings) {
            if (listing.mount && listing.mount->rgss) {
                const std::string path = listing.relative.empty() ? name : listing.relative + "/" + name;
                const int type = RGSS_pathType(listing.mount->path.c_str(), path.c_str());
                if (type == RGSS_PATH_ABSENT) continue;
                return resolved(type);
            }
            // Unicode folds can cross into ASCII (for example Kelvin sign -> K).
            if (listing.mount)
                for (const unsigned char *c = reinterpret_cast<const unsigned char *>(name); *c; ++c)
                    if (*c >= 128) return -1;
            auto found = listing.types.find(name);
            if (found != listing.types.end()) return resolved(found->second);
            // A case alias may shadow later mounts; equal types are safe either way.
            auto alias = listing.aliases.find(folded(name));
            if (alias != listing.aliases.end()) {
                if (alias->second < 0 || resolved(alias->second) < 0) return -1;
                ambiguous = alias->second;
            }
            if (!listing.complete) return -1;
        }
        return -1;
    }
};
#endif
