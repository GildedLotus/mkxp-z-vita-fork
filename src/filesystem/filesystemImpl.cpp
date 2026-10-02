//
//  filesystemImpl.cpp
//  Player
//
//  Created by ゾロアーク on 11/21/20.
//

#include <SDL_filesystem.h>

#include "filesystemImpl.h"
#include "util/exception.h"
#include "util/debugwriter.h"

#include "ghc/filesystem.hpp"
namespace fs = ghc::filesystem;

#include <fstream>

// https://stackoverflow.com/questions/12774207/fastest-way-to-check-if-a-file-exist-using-standard-c-c11-c
bool filesystemImpl::fileExists(const char *path) {
    fs::path stdPath(path);
    return (fs::exists(stdPath) && !fs::is_directory(stdPath));
}


// https://stackoverflow.com/questions/2912520/read-file-contents-into-a-string-in-c
std::string filesystemImpl::contentsOfFileAsString(const char *path) {
    std::string ret;
    try {
        std::ifstream ifs(path);
        ret = std::string ( (std::istreambuf_iterator<char>(ifs) ),
                       (std::istreambuf_iterator<char>()    ) );
    } catch (...) {
        throw Exception(Exception::NoFileError, "Failed to read file at %s", path);
    }

    return ret;
}

// chdir and getcwd do not support unicode on Windows
bool filesystemImpl::setCurrentDirectory(const char *path) {
    fs::path stdPath(path);
    fs::current_path(stdPath);
#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) || defined(__psp2__)
    /* Vita getcwd may return a different spelling of the same directory
     * than the chdir argument (trailing slash, ux0: vs ux0:/). equivalent()
     * then reports false even though the chdir succeeded, and Config
     * aborts with "Unable to switch into gameFolder". Trust chdir. */
    return true;
#else
    bool ret;

    try {
        ret = fs::equivalent(fs::current_path(), stdPath);
    } catch (...) {
        Debug() << "Failed to check current path." << path;
        ret = false;
    }
    return ret;
#endif
}

std::string filesystemImpl::getCurrentDirectory() {
    std::string ret;
    try {
        ret = std::string(fs::current_path().string());
    } catch (...) {
        throw Exception(Exception::MKXPError, "Failed to retrieve current path");
    }
    return ret;
}


std::string filesystemImpl::normalizePath(const char *path, bool preferred, bool absolute) {
    fs::path stdPath(path);
    (void)preferred;
    
    if (!stdPath.is_absolute() && absolute)
        stdPath = fs::current_path() / stdPath;

    stdPath = stdPath.lexically_normal();
    std::string ret(stdPath);
    for (size_t i = 0; i < ret.length(); i++) {
        if (ret[i] == '\\')
            ret[i] = '/';
    }
    return ret;
}

std::string filesystemImpl::getDefaultGameRoot() {
    char *p = SDL_GetBasePath();
    std::string ret(p);
    SDL_free(p);
    return ret;
}
