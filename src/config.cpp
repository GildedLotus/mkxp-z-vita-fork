//
//  config.cpp
//  Player
//
//  Created by ゾロアーク on 11/21/20.
//

#include "config.h"
#include <SDL_filesystem.h>
#include <assert.h>

#include <stdint.h>
#include <vector>

#include "filesystem/filesystem.h"
#include "util/exception.h"
#include "util/debugwriter.h"
#include "util/sdl-util.h"
#include "util/util.h"

#include "util/json5pp.hpp"

#include "util/iniconfig.h"
#include "util/encoding.h"

#include "system/system.h"

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#include <fstream>
#include <stdexcept>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#include <dirent.h>
#endif

#include "vita_glue.h"
/* controllerDeadzone resolves into the analog gate both the Input runtime and
 * the settings-menu binding capture read. */
#include "input/keybindings.h"
#endif


namespace json = json5pp;

std::string prefPath(const char *org, const char *app) {
    char *path = SDL_GetPrefPath(org, app);
    if (!path)
        return std::string("");
    std::string ret(path);
    SDL_free(path);
    return ret;
}

void fillStringVec(json::value &item, std::vector<std::string> &vector) {
    if (!item.is_array()) {
        if (item.is_string()) {
            vector.push_back(item.as_string());
        }
        return;
    }
    auto &array = item.as_array();
    for (size_t i = 0; i < array.size(); i++) {
        if (!array[i].is_string())
            continue;
        
        vector.push_back(array[i].as_string());
    }
}

bool copyObject(json::value &dest, json::value &src, const char *objectName = "") {
    assert(dest.is_object());
    if (src.is_null())
        return false;
    
    if (!src.is_object())
        return false;
    
    auto &srcVec = src.as_object();
    auto &destVec = dest.as_object();
    
    for (auto it : srcVec) {
        // Specifically processs this object later.
        if (it.second.is_object() && destVec[it.first].is_object())
            continue;
        
        if ((it.second.is_array() && destVec[it.first].is_array())    ||
            (it.second.is_number() && destVec[it.first].is_number())  ||
            (it.second.is_string() && destVec[it.first].is_string())  ||
            (it.second.is_boolean() && destVec[it.first].is_boolean()) ||
            (destVec[it.first].is_null()))
        {
            destVec[it.first] = it.second;
        }
        else {
            Debug() << "Invalid variable in configuration:" << objectName << it.first;
        }
    }
    return true;
}

bool getEnvironmentBool(const char *env, bool defaultValue) {
    const char *e = SDL_getenv(env);
    if (!e)
        return defaultValue;
    
    if (!strcmp(e, "0"))
        return false;
    else if (!strcmp(e, "1"))
        return true;
    
    return defaultValue;
}

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
static json::value vitaReadConfFile(const char *path, const char **status);
#endif

json::value readConfFile(const char *path) {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    /* There is exactly one config reader on this device; see vitaReadConfFile
     * for why it cannot be the stock one. */
    const char *status = "absent";
    json::value vitaRet = vitaReadConfFile(path, &status);
    (void)status;
    return vitaRet;
#else

    json::value ret(0);
    if (!mkxp_fs::fileExists(path)) {
        return json::object({});
    }
    
    try {
        std::string cfg = mkxp_fs::contentsOfFileAsString(path);
        ret = json::parse5(Encoding::convertString(cfg));
    }
    catch (const std::exception &e) {
        Debug() << "Failed to parse" << path << ":" << e.what();
    }
    catch (const Exception &e) {
        Debug() << "Failed to parse" << path << ":" << "Unknown encoding";
    }
    
    if (!ret.is_object())
        ret = json::object({});

    return ret;
#endif
}

#define CONF_FILE "mkxp.json"

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
/* ---------------------------------------------------------------------------
 * Vita configuration layers.
 *
 * A VPK that was repacked around one game gets everything it needs from
 * app0:/mkxp.json. A launcher-started player does not: the folder arrives in
 * argv, the user's preferences live on the memory card, and the game folder
 * may carry a config written for a PC. Lowest priority to highest:
 *
 *   compiled ConfDef defaults
 *   app0:/config/default.json       the device profile shipped in the VPK
 *   ux0:/data/mkxp-z/config.json    the user's global settings
 *   <game>/mkxp.json                the game's own file, device keys removed
 *   <game>/mkxp-vita.json           the game's Vita file, all but gameFolder
 *   app0:/mkxp.json                 the VPK author's pin (the stock CWD file)
 *   --game <path>                   the launcher's selection
 *
 * and then, unchanged from stock, <customDataPath>/mkxp.json after
 * readGameINI(). The whole stack is merged before the early option reads
 * because rgssVersion, execName and dataPathOrg/App are consumed by the chdir
 * and readGameINI() that follow them.
 *
 * <game> is the --game path, else the gameFolder of app0:/mkxp.json.
 *
 * Layers stay OFF unless argv carries --game or app0:/mkxp.json sets
 * "vitaConfigLayers": true. With them off Config::read does exactly what stock
 * does, so the pinned diagnostic and test players are untouched.
 * ------------------------------------------------------------------------ */
#if defined(__vita__)
#define VITA_DEFAULT_CONF "app0:/config/default.json"
#define VITA_GLOBAL_CONF  "ux0:/data/mkxp-z/config.json"
#define VITA_GAME_CONF    "mkxp-vita.json"
#define VITA_RTP_ROOT     "ux0:/data/mkxp-z/rtp"
#else
#include "config_paths.h"
#endif
/* GAME_SCAN_PATH_MAX is the launcher's argument budget. Keep this parser
 * in step with launch_path_is_valid/launch_args_parse without linking the
 * launcher library. If one side changes, both change. */
#define VITA_GAME_PATH_MAX 256

/* vita/launcher/launch_args.c launch_path_is_valid. The string crosses a
 * process boundary and then reaches chdir/fopen, so a device-absolute
 * "<dev>:/..." shape with no "." or ".." component and no control byte is the
 * whole contract. */
static bool vitaPathIsValid(const char *path)
{
    size_t i, len, comp;

    if (!path)
        return false;
    len = strlen(path);
    if (len == 0 || len + 1 > VITA_GAME_PATH_MAX)
        return false;

    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)path[i];

        if (c < 0x20 || c == 0x7F)
            return false;
    }

    if (!((path[0] >= 'A' && path[0] <= 'Z') ||
          (path[0] >= 'a' && path[0] <= 'z')))
        return false;
    i = 1;
    while (path[i] && path[i] != ':') {
        char c = path[i];

        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9')))
            return false;
        i++;
    }
    if (path[i] != ':' || path[i + 1] != '/')
        return false;

    comp = i + 2;
    for (;;) {
        size_t end = comp;

        while (path[end] && path[end] != '/')
            end++;
        if (end - comp == 1 && path[comp] == '.')
            return false;
        if (end - comp == 2 && path[comp] == '.' && path[comp + 1] == '.')
            return false;
        if (!path[end])
            break;
        comp = end + 1;
    }
    return true;
}

/* vita/launcher/launch_args.c launch_args_parse. argv[0] is the kernel's own
 * empty string after LoadExec (and the program path on a host run), so the
 * scan starts at 1 and never inspects it; a NULL element ends the vector;
 * "--game <path>" and "--game=<path>" are both accepted; the first occurrence
 * wins whether or not its path is usable.
 *
 * Returns 1 when `out` holds a usable folder, 0 when there is no --game at
 * all, -1 when --game is present with a path this player must not use. */
static int vitaGameFolderFromArgs(int argc, char *argv[], std::string &out)
{
    out.clear();

    if (!argv || argc <= 1)
        return 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *value = 0;

        if (!a)
            break;

        if (!strcmp(a, "--game")) {
            if (i + 1 < argc)
                value = argv[i + 1];
        }
        else if (!strncmp(a, "--game=", 7)) {
            value = a + 7;
        }
        else {
            continue;
        }

        if (value && vitaPathIsValid(value)) {
            out = std::string(value);
            return 1;
        }
        return -1;
    }

    return 0;
}

/* A basename the engine will concatenate onto the game folder before every
 * open of the ini and archive: nonempty, at most 255 bytes
 * (newlib NAME_MAX), no control byte and no separator. The launcher applies
 * the same rule before offering a name; --execName is still the launcher's
 * flag, so an unusable one must be reported, never half-applied. */
static bool vitaExecNameUsable(const char *value)
{
    size_t i, n;

    if (!value || !*value)
        return false;

    n = strlen(value);
    if (n > 255)
        return false;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)value[i];

        if (c < 0x20 || c == 0x7F || c == '/' || c == '\\')
            return false;
    }
    return true;
}

/* vitaGameFolderFromArgs's scan rules for the launcher's second flag:
 * "--execName <name>" and "--execName=<name>" are both accepted, argv[0] is
 * never inspected, a NULL element ends the vector and the first occurrence
 * wins whether or not its name is usable.
 *
 * Returns 1 when `out` holds a usable name, 0 when there is no --execName at
 * all, -1 when it is present but this player must not use it. */
static int vitaExecNameFromArgs(int argc, char *argv[], std::string &out)
{
    out.clear();

    if (!argv || argc <= 1)
        return 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *value = 0;

        if (!a)
            break;

        if (!strcmp(a, "--execName")) {
            if (i + 1 < argc)
                value = argv[i + 1];
        }
        else if (!strncmp(a, "--execName=", 11)) {
            value = a + 11;
        }
        else {
            continue;
        }

        if (value && vitaExecNameUsable(value)) {
            out = std::string(value);
            return 1;
        }
        return -1;
    }

    return 0;
}

/* The launcher's own flags are not game arguments: they must not reach
 * System::launch_args, where a game script would see them. Stock already drops
 * "debug" while building launchArgs, so the vector still has argv's order and
 * "--game" is still immediately followed by its value. */
static void vitaStripLauncherArgs(std::vector<std::string> &args)
{
    std::vector<std::string> kept;

    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--game" || args[i] == "--execName") {
            i++; /* and its value, if the vector still has one */
            continue;
        }
        if (!args[i].compare(0, 7, "--game=") ||
            !args[i].compare(0, 11, "--execName="))
            continue;
        kept.push_back(args[i]);
    }

    args.swap(kept);
}

/* Keys the device owns. A game's mkxp.json was written for a desktop: its
 * window, renderer, JIT and data-path choices are wrong here by construction,
 * and gameFolder would send the player somewhere else entirely. A game that
 * really has something to say about them says it in mkxp-vita.json, except
 * gameFolder, maxTextureSize and enableHires: those stay with the device
 * (see the erases where the mkxp-vita.json layer is merged). */
static void vitaFilterGameLayer(json::value &layer)
{
    static const char *const denied[] = {
        "gameFolder", "fullscreen", "winResizable", "defScreenW",
        "defScreenH", "vsync", "syncToRefreshrate", "enableBlitting",
        "maxTextureSize", "enableHires", "textureScalingFactor",
        "framebufferScalingFactor", "atlasScalingFactor",
        "smoothScalingMipmaps", "JITEnable", "JITVerboseLevel",
        "JITMaxCache", "JITMinCalls", "YJITEnable", "dataPathOrg",
        "dataPathApp", "dumpAtlas"
    };

    if (!layer.is_object())
        return;

    json::value::object_type &obj = layer.as_object();

    for (size_t i = 0; i < sizeof(denied) / sizeof(denied[0]); i++)
        obj.erase(denied[i]);
}

/* The shared customDataPath once held saves of mkxp-z-aware games.
 * Nobody can tell which game owns such a file, so only say it is there:
 * never move, copy or delete it. Types come from the listing; an
 * untyped entry is skipped, not stat()ed, and the walk stops at 64 entries. */
static void vitaLegacySharedDataNotice(const std::string &dir)
{
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    for (int seen = 0; seen < 64; seen++) {
        struct dirent *e = readdir(d);
        if (!e)
            break;
#if defined(__vita__)
        const bool regular = SCE_S_ISREG(e->d_stat.st_mode);
#else
        const bool regular = e->d_type == DT_REG;
#endif
        const char *n = e->d_name;
        if (!regular || n[0] == '.' ||
            !strncmp(n, "keybindings.mkxp", 16) || !strncmp(n, "mkxp.json", 9))
            continue;
        char tb[640];
        snprintf(tb, sizeof(tb), "vita-config: legacy shared data in '%s' "
                 "(first: '%s'); move a game's files into its own folder",
                 dir.c_str(), n);
        vita_glue_trace(tb);
        break;
    }
    closedir(d);
}

static bool vitaDirExists(const char *path)
{
    struct stat st;

    if (!path || !*path)
        return false;
    if (stat(path, &st) != 0)
        return false;
    return S_ISDIR(st.st_mode) != 0;
}

/* Reading one layer. This cannot be the stock readConfFile body: iconv on
 * VitaSDK newlib has no CES tables, so whenever uchardet
 * guesses anything other than ASCII/UTF-8 -- which it does on any file with
 * high bytes in it -- Encoding::convertString throws and the stock reader
 * silently discards the WHOLE file. Config files on this device are UTF-8 by
 * construction, and the only thing json5pp cannot swallow is a byte-order
 * mark, so strip that and parse the bytes as they are.
 *
 * `status` is "absent" (no such file), "ok" (an object was parsed) or "error"
 * (unreadable, unparsable, or valid JSON that is not an object) for the trace
 * line. Every one of them yields an empty object, exactly as stock does. */
static json::value vitaReadConfFile(const char *path, const char **status)
{
    json::value ret(0);

    *status = "absent";
    if (!mkxp_fs::fileExists(path)) {
        return json::object({});
    }

    *status = "error";
    try {
        // Bound bytes before appending; a size probe alone misses file growth.
        static const size_t maxBytes = 64 * 1024;
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::runtime_error("Could not open config file");
        std::string cfg;
        char chunk[4096];
        for (;;) {
            input.read(chunk, sizeof(chunk));
            const size_t count = static_cast<size_t>(input.gcount());
            if (count > maxBytes - cfg.size())
                throw std::runtime_error("Config file exceeds 65536-byte limit");
            cfg.append(chunk, count);
            if (input.bad() || (input.fail() && !input.eof()))
                throw std::runtime_error("Could not read config file");
            if (input.eof())
                break;
        }

        if (cfg.size() >= 3 && (unsigned char)cfg[0] == 0xEF &&
            (unsigned char)cfg[1] == 0xBB && (unsigned char)cfg[2] == 0xBF)
            cfg.erase(0, 3);

        ret = json::parse5(cfg.data(), cfg.size(), 32);
    }
    catch (const std::exception &e) {
        Debug() << "Failed to parse" << path << ":" << e.what();
    }
    catch (const Exception &e) {
        Debug() << "Failed to parse" << path << ":" << "Unknown encoding";
    }

    if (!ret.is_object())
        ret = json::object({});
    else
        *status = "ok";

    return ret;
}

/* One layer, merged the way stock merges app0:/mkxp.json: copyObject for the
 * flat keys, plus the separate copyObject for bindingNames that copyObject
 * itself deliberately skips because both sides are objects. */
static void vitaMergeLayer(json::value &optsJ, json::value &layer)
{
    json::value::object_type &opts = optsJ.as_object();

    copyObject(optsJ, layer);
    copyObject(opts["bindingNames"], layer.as_object()["bindingNames"],
               "bindingNames .");
}

/* Read a flag out of a layer without touching it: as_object()["key"] would
 * insert a null and the next copyObject would then log it as invalid. */
static bool vitaFlagIsTrue(const json::value &conf, const char *key)
{
    if (!conf.is_object())
        return false;

    const json::value::object_type &obj = conf.as_object();
    json::value::object_type::const_iterator it = obj.find(key);

    return it != obj.end() && it->second.is_boolean() && it->second.as_boolean();
}

static bool vitaFlagIsFalse(const json::value &conf, const char *key)
{
    if (!conf.is_object())
        return false;

    const json::value::object_type &obj = conf.as_object();
    json::value::object_type::const_iterator it = obj.find(key);

    return it != obj.end() && it->second.is_boolean() && !it->second.as_boolean();
}

static std::string vitaStringOf(const json::value &conf, const char *key)
{
    if (!conf.is_object())
        return std::string();

    const json::value::object_type &obj = conf.as_object();
    json::value::object_type::const_iterator it = obj.find(key);

    if (it == obj.end() || !it->second.is_string())
        return std::string();
    return it->second.as_string();
}

/* argMode is vitaGameFolderFromArgs's return value. A --game that is present
 * but unusable still turns the layers on: the process was started by the
 * launcher either way, and the trace has to say arg=error rather than quietly
 * behaving like a pinned player. */
static bool vitaLayersEnabled(int argMode, const json::value &rootConf)
{
    return argMode != 0 || vitaFlagIsTrue(rootConf, "vitaConfigLayers");
}

static std::string vitaJoin(const std::string &dir, const char *leaf)
{
    if (dir.empty())
        return std::string();

    char last = dir[dir.size() - 1];

    if (last == '/' || last == ':')
        return dir + leaf;
    return dir + "/" + leaf;
}

/* Settings generation selection.
 * This merge in Config::read is the only in-process consumer of the
 * settings file and it runs before Ruby exists: a generation that
 * VitaSettingsFile.recover restores from the pinned backup later would
 * apply only on the next launch, while runtime queries already report the
 * restored values. One validity rule serves this selection, the CFG[] reader
 * and Ruby's recover (through vitaSettingsFileValid): a generation counts
 * only when it is a nonempty regular file that parses, within the config
 * size bound, to a JSON object. The active file is used when valid, else the
 * backup when that is, else the active path (parsed as {}). Restoring the
 * active file itself stays in settings_file.rb. */
static bool vitaSettingsGenerationValid(const std::string &path)
{
    struct stat st;
    const char *status = "absent";

    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
        return false;
    vitaReadConfFile(path.c_str(), &status);
    return !strcmp(status, "ok");
}

static std::string vitaSelectSettingsGeneration(const std::string &active)
{
    if (vitaSettingsGenerationValid(active))
        return active;

    std::string backup = active + ".bak";

    if (vitaSettingsGenerationValid(backup))
        return backup;
    return active;
}

bool vitaSettingsFileValid(const char *path)
{
    return vitaSettingsGenerationValid(path);
}

json::value readUserSettings(const char *path)
{
    const char *status = "absent";

    return vitaReadConfFile(vitaSelectSettingsGeneration(path).c_str(), &status);
}

static void vitaMergeLayers(json::value &optsJ, json::value &rootConf,
                            const char *rootStatus, int argMode,
                            const std::string &argFolder,
                            const std::string &argExec)
{
    const char *defaultStatus = "absent";
    const char *globalStatus = "absent";
    const char *gameStatus = "absent";
    const char *gameVitaStatus = "absent";
    char tb[400];
    char execSuffix[300];

    json::value deviceConf = vitaReadConfFile(VITA_DEFAULT_CONF, &defaultStatus);
    vitaMergeLayer(optsJ, deviceConf);

    json::value globalConf = vitaReadConfFile(VITA_GLOBAL_CONF, &globalStatus);
    vitaMergeLayer(optsJ, globalConf);

    std::string game = (argMode > 0) ? argFolder
                                     : vitaStringOf(rootConf, "gameFolder");

    if (!game.empty()) {
        std::string gamePath = vitaJoin(game, CONF_FILE);
        json::value gameConf = vitaReadConfFile(gamePath.c_str(), &gameStatus);

        vitaFilterGameLayer(gameConf);
        vitaMergeLayer(optsJ, gameConf);

        std::string vitaPath = vitaJoin(game, VITA_GAME_CONF);
        json::value gameVitaConf = vitaReadConfFile(vitaPath.c_str(),
                                                   &gameVitaStatus);

        /* mkxp-vita.json may say anything the device owns EXCEPT where the
         * player is: a game folder does not get to redirect the player. Both
         * layers above always re-set gameFolder -- one of them is where
         * `game` came from -- so this only ever shows up as the absence of an
         * "Invalid variable" line for a badly typed one. */
        gameVitaConf.as_object().erase("gameFolder");
        /* Both size the GL surfaces and window bases; a game has no say. */
        gameVitaConf.as_object().erase("maxTextureSize");
        gameVitaConf.as_object().erase("enableHires");
        vitaMergeLayer(optsJ, gameVitaConf);
    }

    vitaMergeLayer(optsJ, rootConf);

    if (argMode > 0) {
        json::value argConf = json::object({{"gameFolder", argFolder}});

        /* Layer 7 is the launcher's own word about the game: a
         * name it verified against the card's own listing outranks anything
         * the game folder's mkxp.json says execName should be. */
        if (!argExec.empty())
            argConf.as_object()["execName"] = argExec;

        vitaMergeLayer(optsJ, argConf);
    }

    /* The field is appended only when the launcher named the game, so the
     * line is byte-identical to the documented shape in every other case. */
    execSuffix[0] = '\0';
    if (argMode > 0 && !argExec.empty())
        snprintf(execSuffix, sizeof(execSuffix), " execName='%s'",
                 argExec.c_str());

    snprintf(tb, sizeof(tb),
             "vita-config: layers default=%s global=%s game=%s game-vita=%s "
             "root=%s arg=%s%s",
             defaultStatus, globalStatus, gameStatus, gameVitaStatus,
             rootStatus,
             argMode > 0 ? "ok" : (argMode < 0 ? "error" : "absent"),
             execSuffix);
    vita_glue_trace(tb);
}

/* The RTPs a game needs are installed once, per RGSS version, under
 * ux0:/data/mkxp-z/rtp. Only fill the list in when nobody else has: an
 * explicit RTP array, a customScript player (which has no Game.ini and no RTP
 * of its own) and "vitaAutoRTP": false all leave it alone. Supplying the list
 * is all this does; vita-rtp.h owns what SharedState then mounts. */
static void vitaAutoRtp(const json::value &optsJ, int rgssVersion,
                        const std::string &customScript,
                        std::vector<std::string> &rtps)
{
    const char *name;
    std::string path;

    if (!rtps.empty() || !customScript.empty())
        return;
    if (vitaFlagIsFalse(optsJ, "vitaAutoRTP"))
        return;

    switch (rgssVersion) {
    case 1:
        name = "XP";
        break;
    case 2:
        name = "VX";
        break;
    case 3:
        name = "VXAce";
        break;
    default:
        return;
    }

    path = std::string(VITA_RTP_ROOT "/") + name;
    if (vitaDirExists(path.c_str()))
        rtps.push_back(path);
}
#endif

Config::Config() {}

void Config::read(int argc, char *argv[]) {
    auto optsJ = json::object({
        {"rgssVersion", 0},
        {"debugMode", false},
        {"displayFPS", false},
        {"printFPS", false},
        {"winResizable", true},
        {"fullscreen", false},
        {"fixedAspectRatio", true},
        {"smoothScaling", 0},
        {"smoothScalingDown", 0},
        {"bitmapSmoothScaling", 0},
        {"bitmapSmoothScalingDown", 0},
        {"smoothScalingMipmaps", false},
        {"enableHires", false},
        {"textureScalingFactor", 1.},
        {"framebufferScalingFactor", 1.},
        {"atlasScalingFactor", 1.},
        {"vsync", false},
        {"defScreenW", 0},
        {"defScreenH", 0},
        {"windowTitle", ""},
        {"fixedFramerate", 0},
        /* Off, as in stock. Rendering is on the GPU, so an overrun is script
         * work, which skipping the draw does not shorten; RGSS itself never
         * skips. "frameSkip": true in any layer opts into the bounded skip
         * (FPSLimiter::maxConsecutiveSkips). See vita/docs/config.md. */
        {"frameSkip", false},
        {"syncToRefreshrate", false},
        {"solidFonts", json::array({})},
        {"subImageFix", false},
        {"enableBlitting", true},
        {"integerScalingActive", false},
        {"integerScalingLastMile", true},
        {"maxTextureSize", 0},
        {"gameFolder", ""},
        {"anyAltToggleFS", false},
        {"enableReset", true},
        {"enableSettings", true},
        {"allowSymlinks", true},
        {"dataPathOrg", ""},
        {"dataPathApp", ""},
        {"execName", "Game"},
        {"midiSoundFont", ""},
        {"midiChorus", false},
        {"midiReverb", false},
        {"SESourceCount", 6},
        {"BGMTrackCount", 1},
        {"customScript", ""},
        {"pathCache", true},
        {"useScriptNames", true},
        {"preloadScript", json::array({})},
        {"postloadScript", json::array({})},
        {"RTP", json::array({})},
        {"patches", json::array({})},
        {"fontSub", json::array({})},
        {"fontScale", 0.0f},
        {"fontKerning", true},
        {"fontHinting", 3}, // TTF_HINTING_NONE
        {"fontHeightReporting", 0},
        {"fontOutlineCrop", true},
        {"rubyLoadpath", json::array({})},
        {"JITEnable", false},
        {"JITVerboseLevel", 0},
        {"JITMaxCache", 100},
        {"JITMinCalls", 10000},
        {"YJITEnable", false},
        {"dumpAtlas", false},
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        /* Vita-only keys. Declared here so a layer that
         * carries them type-checks against a boolean instead of landing in
         * copyObject's null case, and so System::CONFIG reports them. */
        {"vitaConfigLayers", false},
        {"vitaAutoRTP", true},
        {"vitaTouchMouse", true},
        {"vitaglRamPoolMiB", 0},
        {"vitaglCdramPoolMiB", 0},
        {"vitaglPhycontPoolMiB", 0},
        /* Analog gate, as a fraction of SDL's int16 axis range. 0.30 instead of stock's implicit 0.5 because SDL's
         * Vita driver eases the low half of the stick's travel: at 0.5 a
         * diagonal needs a ~30 degree window, at 0.30 it gets ~47. See
         * src/input/keybindings.h. */
        {"controllerDeadzone", 0.3},
#endif
        {"bindingNames", json::object({
            {"a", "A"},
            {"b", "B"},
            {"c", "C"},
            {"x", "X"},
            {"y", "Y"},
            {"z", "Z"},
            {"l", "L"},
            {"r", "R"}
        })}
    });
    
    auto &opts = optsJ.as_object();
    
#define GUARD(exp) \
try { exp } catch (...) {}
    
    editor.debug = false;
    editor.battleTest = false;
    
    if (argc > 1) {
        if (!strcmp(argv[1], "debug") || !strcmp(argv[1], "test"))
            editor.debug = true;
        else if (!strcmp(argv[1], "btest"))
            editor.battleTest = true;
        
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "debug"))
                launchArgs.push_back(argv[i]);
        }
    }
    
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    /* The layer stack owns this read. Everything it merges has to be in place
     * before the early option reads below, because gameFolder, rgssVersion,
     * execName and dataPathOrg/App are all consumed by the chdir and the
     * readGameINI() that follow them. The else branch is stock: the same
     * baseConf and the same two copyObject calls, in the same place. */
    const char *vitaRootStatus = "absent";
    std::string vitaArgFolder;
    std::string vitaArgExec;
    int vitaArgMode = vitaGameFolderFromArgs(argc, argv, vitaArgFolder);

    vitaExecNameFromArgs(argc, argv, vitaArgExec);
    json::value baseConf = vitaReadConfFile(CONF_FILE, &vitaRootStatus);

    if (vitaLayersEnabled(vitaArgMode, baseConf)) {
        vitaMergeLayers(optsJ, baseConf, vitaRootStatus, vitaArgMode,
                        vitaArgFolder, vitaArgExec);
        vitaStripLauncherArgs(launchArgs);
    }
    else {
        copyObject(optsJ, baseConf);
        copyObject(opts["bindingNames"], baseConf.as_object()["bindingNames"], "bindingNames .");
    }
#else
    json::value baseConf = readConfFile(CONF_FILE);
    copyObject(optsJ, baseConf);
    copyObject(opts["bindingNames"], baseConf.as_object()["bindingNames"], "bindingNames .");
#endif

#define SET_OPT_CUSTOMKEY(var, key, type) GUARD(var = opts[#key].as_##type();)
#define SET_OPT(var, type) SET_OPT_CUSTOMKEY(var, var, type)
#define SET_STRINGOPT(var, key) GUARD(var = std::string(opts[#key].as_string());)
    
    SET_STRINGOPT(gameFolder, gameFolder);
    SET_STRINGOPT(dataPathOrg, dataPathOrg);
    SET_STRINGOPT(dataPathApp, dataPathApp);
    SET_STRINGOPT(execName, execName);
    SET_OPT(allowSymlinks, boolean);
    SET_OPT(pathCache, boolean);
    SET_OPT_CUSTOMKEY(jit.enabled, JITEnable, boolean);
    SET_OPT_CUSTOMKEY(jit.verboseLevel, JITVerboseLevel, integer);
    SET_OPT_CUSTOMKEY(jit.maxCache, JITMaxCache, integer);
    SET_OPT_CUSTOMKEY(jit.minCalls, JITMinCalls, integer);
    SET_OPT_CUSTOMKEY(yjit.enabled, YJITEnable, boolean);
    SET_OPT(rgssVersion, integer);
    SET_OPT(defScreenW, integer);
    SET_OPT(defScreenH, integer);
    
    // Take a break real quick and witch to set game folder and read the game's ini
    if (!gameFolder.empty() && !mkxp_fs::setCurrentDirectory(gameFolder.c_str())) {
        throw Exception(Exception::MKXPError, "Unable to switch into gameFolder %s", gameFolder.c_str());
    }
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    /* Saves go in the game's own folder. The CWD, not the
     * configured value: a relative gameFolder must resolve exactly once. */
    gameDataPath.clear();
    if (!gameFolder.empty()) {
        try {
            gameDataPath = mkxp_fs::getCurrentDirectory();
        } catch (const Exception &) {
            gameDataPath.clear();
        }
    }
#endif
    
    readGameINI();
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    if (!gameDataPath.empty()) {
        char tb[600];
        snprintf(tb, sizeof(tb), "vita-config: data directory='%s'",
                 gameDataPath.c_str());
        vita_glue_trace(tb);
        vitaLegacySharedDataNotice(customDataPath);
    }
#endif
    
    // Now check for an extra mkxp.conf in the user's save directory and merge anything else from that
    userConfPath = mkxp_fs::normalizePath(std::string(customDataPath + "/" CONF_FILE).c_str(), 0, 1);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    /* Selection before the merge, not recovery after it (see the helper
     * above): this read is the only in-process consumer of the file
     * and Ruby does not exist yet, so merging the active path unconditionally
     * would run this launch on defaults and apply the restored generation
     * only on the next one. */
    std::string settingsGen = vitaSelectSettingsGeneration(userConfPath);
    if (settingsGen != userConfPath) {
        char tb[512];
        snprintf(tb, sizeof(tb),
                 "vita-config: settings generation=backup (active missing, "
                 "empty or invalid; merging %s)", settingsGen.c_str());
        vita_glue_trace(tb);
    }
    json::value userConf = readConfFile(settingsGen.c_str());
#else
    json::value userConf = readConfFile(userConfPath.c_str());
#endif
    copyObject(optsJ, userConf);
    
    // now RESUME
    
    SET_OPT(debugMode, boolean);
    SET_OPT(displayFPS, boolean);
    SET_OPT(printFPS, boolean);
    SET_OPT(fullscreen, boolean);
    SET_OPT(fixedAspectRatio, boolean);
    SET_OPT(smoothScaling, integer);
    SET_OPT(smoothScalingDown, integer);
    SET_OPT(bitmapSmoothScaling, integer);
    SET_OPT(bitmapSmoothScalingDown, integer);
    SET_OPT(smoothScalingMipmaps, boolean);
    SET_OPT(enableHires, boolean);
    SET_OPT(textureScalingFactor, number);
    SET_OPT(framebufferScalingFactor, number);
    SET_OPT(atlasScalingFactor, number);
    SET_OPT(winResizable, boolean);
    SET_OPT(vsync, boolean);
    SET_STRINGOPT(windowTitle, windowTitle);
    SET_OPT(fixedFramerate, integer);
    SET_OPT(frameSkip, boolean);
    SET_OPT(syncToRefreshrate, boolean);
    fillStringVec(opts["solidFonts"], solidFonts);
    for (std::string & solidFont : solidFonts)
        std::transform(solidFont.begin(), solidFont.end(), solidFont.begin(),
            [](unsigned char c) { return std::tolower(c); });
    SET_OPT(subImageFix, boolean);
    SET_OPT(enableBlitting, boolean);
    SET_OPT_CUSTOMKEY(integerScaling.active, integerScalingActive, boolean);
    SET_OPT_CUSTOMKEY(integerScaling.lastMileScaling, integerScalingLastMile, boolean);
    SET_OPT(maxTextureSize, integer);
    SET_OPT(anyAltToggleFS, boolean);
    SET_OPT(enableReset, boolean);
    SET_OPT(enableSettings, boolean);
    SET_STRINGOPT(midi.soundFont, midiSoundFont);
    SET_OPT_CUSTOMKEY(midi.chorus, midiChorus, boolean);
    SET_OPT_CUSTOMKEY(midi.reverb, midiReverb, boolean);
    SET_OPT_CUSTOMKEY(SE.sourceCount, SESourceCount, integer);
    SET_OPT_CUSTOMKEY(BGM.trackCount, BGMTrackCount, integer);
    SET_STRINGOPT(customScript, customScript);
    SET_OPT(useScriptNames, boolean);
    SET_OPT(dumpAtlas, boolean);
    
    fillStringVec(opts["preloadScript"], preloadScripts);
    fillStringVec(opts["postloadScript"], postloadScripts);
    fillStringVec(opts["RTP"], rtps);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    vitaAutoRtp(optsJ, rgssVersion, customScript, rtps);
#endif
    fillStringVec(opts["patches"], patches);
    fillStringVec(opts["fontSub"], fontSubs);
    for (std::string & fontSub : fontSubs)
        std::transform(fontSub.begin(), fontSub.end(), fontSub.begin(),
            [](unsigned char c) { return std::tolower(c); });
    SET_OPT(fontScale, number);
    SET_OPT(fontKerning, boolean);
    SET_OPT(fontHinting, integer);
    SET_OPT(fontHeightReporting, integer);
    SET_OPT(fontOutlineCrop, boolean);
    fillStringVec(opts["rubyLoadpath"], rubyLoadpaths);
    
    auto &bnames = opts["bindingNames"].as_object();
    
#define BINDING_NAME(btn) kbActionNames.btn = bnames[#btn].as_string()
    BINDING_NAME(a);
    BINDING_NAME(b);
    BINDING_NAME(c);
    BINDING_NAME(x);
    BINDING_NAME(y);
    BINDING_NAME(z);
    BINDING_NAME(l);
    BINDING_NAME(r);
    
    rgssVersion = clamp(rgssVersion, 0, 3);
    SE.sourceCount = clamp(SE.sourceCount, 1, 64);
    BGM.trackCount = clamp(BGM.trackCount, 1, 16);

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    vitaglRamPoolMiB = vitaglCdramPoolMiB = vitaglPhycontPoolMiB = 0;
    SET_OPT(vitaglRamPoolMiB, integer);
    SET_OPT(vitaglCdramPoolMiB, integer);
    SET_OPT(vitaglPhycontPoolMiB, integer);

    /* Assigned before the merged value is read: Config has no member
     * initializers, and an indeterminate byte here would decide a preference
     * the player never set. */
    vitaTouchMouse = true;
    SET_OPT(vitaTouchMouse, boolean);

    /* The software Bitmap backend throws at every Bitmap construction under
     * enableHires. The game layers cannot set it, but the device defaults
     * and the root mkxp.json can, and that would fail at the first Bitmap;
     * refuse it here with one line instead. */
    if (enableHires) {
        vita_glue_trace("vita-config: enableHires is not supported on this "
                        "platform; ignored");
        enableHires = false;
    }

    /* The analog gate. Read after the merge like every
     * other preference, clamped to a band that can neither leave an axis
     * permanently active nor make it unreachable, then resolved ONCE into the
     * int16 both CtrlAxisBinding::sourceActive() and the settings-menu
     * binding capture compare against. Nothing downstream re-reads the
     * double, so no divide or float compare ever reaches a per-frame path. */
    SET_OPT(controllerDeadzone, number);
    controllerDeadzone =
        clamp(controllerDeadzone, JAXIS_DEADZONE_MIN, JAXIS_DEADZONE_MAX);
    setJAxisThreshold(controllerDeadzone);

    /* Its own line rather than a field of the effective trace below: that
     * format string is pinned field-for-field against vita/docs/config.md. Printed
     * as thousandths so the log needs no float conversion. */
    {
        char tb[128];

        snprintf(tb, sizeof(tb),
                 "vita-input: controllerDeadzone=%d/1000 axisThreshold=%d "
                 "stock=%d",
                 (int)(controllerDeadzone * 1000.0 + 0.5),
                 (int)jAxisThreshold(), (int)JAXIS_THRESHOLD_DEFAULT);
        vita_glue_trace(tb);
    }

    /* What the engine actually got, after every layer and both clamps. This
     * line is the one thing a hardware log needs to settle "which config won".
     * It is emitted whether or not the layers ran. */
    {
        std::string rtpList;
        char tb[640];

        for (size_t i = 0; i < rtps.size(); i++) {
            if (i)
                rtpList += ",";
            rtpList += rtps[i];
        }

        snprintf(tb, sizeof(tb),
                 "vita-config: effective gameFolder='%s' rgssVersion=%d "
                 "rtp=[%s] smoothScaling=%d fixedAspect=%d integer=%d/%d "
                 "frameSkip=%d fixedFramerate=%d",
                 gameFolder.c_str(), rgssVersion, rtpList.c_str(),
                 smoothScaling, (int)fixedAspectRatio,
                 (int)integerScaling.active,
                 (int)integerScaling.lastMileScaling, (int)frameSkip,
                 fixedFramerate);
        vita_glue_trace(tb);
    }
#endif

    raw = optsJ;
}

static void setupScreenSize(Config &conf) {
    if (conf.defScreenW <= 0)
        conf.defScreenW = (conf.rgssVersion == 1 ? 640 : 544);
    
    if (conf.defScreenH <= 0)
        conf.defScreenH = (conf.rgssVersion == 1 ? 480 : 416);
}

bool Config::fontIsSolid(const char *fontName) const {
    for (std::string solidfont : solidFonts)
        if (!strcmp(solidfont.c_str(), fontName)) return true;
    
    return false;
}

void Config::readGameINI() {
    if (!customScript.empty()) {
        game.title = customScript.c_str();
        
        if (rgssVersion == 0)
            rgssVersion = 1;
        
        setupScreenSize(*this);
        
        return;
    }
    
    std::string iniFileName(execName + ".ini");
    SDLRWStream iniFile(iniFileName.c_str(), "r");
    
    bool convSuccess = false;
    if (iniFile)
    {
        INIConfiguration ic;
        if (ic.load(iniFile.stream()))
        {
            GUARD(game.title = ic.getStringProperty("Game", "Title"););
            GUARD(game.scripts = ic.getStringProperty("Game", "Scripts"););
            
            strReplace(game.scripts, '\\', '/');
            
            if (game.title.empty()) {
                Debug() << iniFileName + ": Could not find Game.Title";
            }
            
            if (game.scripts.empty())
                Debug() << iniFileName + ": Could not find Game.Scripts";
        }
    }
    else
        Debug() << "Could not read" << iniFileName;
    
    try {
        game.title = Encoding::convertString(game.title);
        convSuccess = true;
    }
    catch (const Exception &e) {
        Debug() << iniFileName + ": Could not determine encoding of Game.Title";
    }
    
    if (game.title.empty() || !convSuccess)
        game.title = "mkxp-z";
    
    if (dataPathOrg.empty())
        dataPathOrg = ".";
    
    if (dataPathApp.empty())
        dataPathApp = game.title;
    
    customDataPath = mkxp_fs::normalizePath(prefPath(dataPathOrg.c_str(), dataPathApp.c_str()).c_str(), 0, 1);
    
    if (rgssVersion == 0) {
        /* Try to guess RGSS version based on Data/Scripts extension */
        rgssVersion = 1;
        
        if (!game.scripts.empty()) {
            const char *p = &game.scripts[game.scripts.size()];
            const char *head = &game.scripts[0];
            
            while (--p != head)
                if (*p == '.')
                    break;
            
            if (!strcmp(p, ".rvdata"))
                rgssVersion = 2;
            else if (!strcmp(p, ".rvdata2"))
                rgssVersion = 3;
        }
    }
    
    setupScreenSize(*this);
}
