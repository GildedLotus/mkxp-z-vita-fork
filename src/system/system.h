//
//  system.h
//  Player
//
//  Created by ゾロアーク on 11/22/20.
//

#ifndef system_h
#define system_h

#include <string>

#define MKXPZ_PLATFORM_WINDOWS 0
#define MKXPZ_PLATFORM_MACOS 1
#define MKXPZ_PLATFORM_LINUX 2

#ifdef __WIN32__
#define MKXPZ_PLATFORM MKXPZ_PLATFORM_WINDOWS
#elif defined __APPLE__
#define MKXPZ_PLATFORM MKXPZ_PLATFORM_MACOS
#elif defined __linux__
#define MKXPZ_PLATFORM MKXPZ_PLATFORM_LINUX
#elif defined(__vita__) || defined(__psp2__)
/* PS Vita (VitaSDK arm-vita-eabi). newlib, not glibc; the non-Windows
 * systemImpl paths (locale / getenv / isWine=false) are correct here.
 * Reuse the Linux value so the rest of the tree needs no extra #ifdefs.
 */
#define MKXPZ_PLATFORM MKXPZ_PLATFORM_LINUX
#else
#error "Can't identify platform."
#endif

namespace systemImpl {
enum WineHostType {
    Windows,
    Linux,
    Mac
};
std::string getSystemLanguage();
std::string getUserName();
int getScalingFactor();

bool isWine();
bool isRosetta();
WineHostType getRealHostType();
}

#ifdef MKXPZ_BUILD_XCODE
std::string getPlistValue(const char *key);
void openSettingsWindow();
bool isMetalSupported();
#endif

namespace mkxp_sys = systemImpl;

#endif /* system_h */
