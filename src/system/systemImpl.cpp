//
//  systemImpl.cpp
//  Player
//
//  Created by ゾロアーク on 11/22/20.
//

#include "system.h"

#include <stdlib.h>
#include <locale>

#include <string>

std::string systemImpl::getSystemLanguage() {
    std::string language = std::locale("").name();
    const std::string::size_type encoding = language.find('.');
    if (encoding != std::string::npos)
        language.erase(encoding);
    return language;
}

std::string systemImpl::getUserName() {
    const char *username = getenv("USER");
    return username ? std::string(username) : std::string();
}

int systemImpl::getScalingFactor() {
    return 1;
}
