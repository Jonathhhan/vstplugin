#include "Interface.h"
#include "Utility.h"

#if USE_STDFS
# include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
# ifndef _WIN32
#  define widen(x) x
# endif
#else
# include <dirent.h>
# include <unistd.h>
# include <strings.h>
# include <sys/wait.h>
# include <sys/stat.h>
# include <sys/types.h>
#endif

#include <unordered_set>
#include <cstring>

namespace vst {

static std::vector<const char *> platformExtensions = {
#if USE_VST2
 #if defined(_WIN32) || USE_WINE // Windows or Wine
    ".dll",
 #endif // Windows or Wine
 #ifndef _WIN32 // Unix
  #ifdef __APPLE_
    ".vst",
  #else
    ".so",
  #endif
 #endif // Unix
#endif // VST2
#if USE_VST3
    ".vst3",
#endif // VST3
};

const std::vector<const char *>& getPluginExtensions() {
    return platformExtensions;
}

const char * getBundleBinaryPath(){
    auto path =
#if defined(_WIN32)
#ifdef _WIN64
        "Contents/x86_64-win";
#else // WIN32
        "Contents/x86-win";
#endif
#elif defined(__APPLE__)
        "Contents/MacOS";
#else // Linux
#if defined(__i386__)
        "Contents/i386-linux";
#elif defined(__x86_64__)
        "Contents/x86_64-linux";
#else
        ""; // figure out what to do with all the ARM versions...
#endif
#endif
    return path;
}

#ifdef _WIN32
# if USE_BRIDGE
   #define PROGRAMFILES(x) "%ProgramW6432%\\" x, "%ProgramFiles(x86)%\\" x
# else
#  ifdef _WIN64 // 64 bit
#   define PROGRAMFILES(x) "%ProgramFiles%\\" x
#  else // 32 bit
#   define PROGRAMFILES(x) "%ProgramFiles(x86)%\\" x
#  endif
# endif // USE_BRIDGE
#endif // WIN32

static std::vector<const char *> defaultSearchPaths = {
/*---- VST2 ----*/
#if USE_VST2
    // macOS
  #ifdef __APPLE__
    "~/Library/Audio/Plug-Ins/VST", "/Library/Audio/Plug-Ins/VST",
  #endif
    // Windows
  #ifdef _WIN32
    PROGRAMFILES("VSTPlugins"), PROGRAMFILES("Steinberg\\VSTPlugins"),
    PROGRAMFILES("Common Files\\VST2"), PROGRAMFILES("Common Files\\Steinberg\\VST2"),
  #endif
    // Linux
  #ifdef __linux__
    "~/.vst", "/usr/local/lib/vst", "/usr/lib/vst",
  #endif
#endif
/*---- VST3 ----*/
#if USE_VST3
    // macOS
  #ifdef __APPLE__
    "~/Library/Audio/Plug-Ins/VST3", "/Library/Audio/Plug-Ins/VST3"
  #endif
    // Windows
  #ifdef _WIN32
    PROGRAMFILES("Common Files\\VST3")
  #endif
    // Linux
  #ifdef __linux__
    "~/.vst3", "/usr/local/lib/vst3", "/usr/lib/vst3"
  #endif
#endif // VST3
};

#if USE_WINE
# define PROGRAMFILES(x) "/drive_c/Program Files/" x, "/drive_c/Program Files (x86)" x

static std::vector<const char *> defaultWineSearchPaths = {
#if USE_VST2
    PROGRAMFILES("VSTPlugins"), PROGRAMFILES("Steinberg/VSTPlugins"),
    PROGRAMFILES("Common Files/VST2"), PROGRAMFILES("Common Files/Steinberg/VST2"),
#endif
#if USE_VST3
    PROGRAMFILES("Common Files/VST3")
#endif
};
#endif

#ifdef PROGRAMFILES
#undef PROGRAMFILES
#endif

// get "real" default search paths
const std::vector<std::string>& getDefaultSearchPaths() {
    static std::vector<std::string> result = [](){
        std::vector<std::string> list;
        for (auto& path : defaultSearchPaths) {
            list.push_back(expandPath(path));
        }
    #if USE_WINE
        std::string winePrefix = expandPath(getWineFolder());
        for (auto& path : defaultWineSearchPaths) {
            list.push_back(winePrefix + path);
        }
    #endif
        return list;
    }();
    return result;
}

#if USE_WINE
const char * getWineCommand(){
    // users can override the 'wine' command with the
    // 'VSTPLUGIN_WINE' environment variable
    const char *cmd = getenv("VSTPLUGIN_WINE");
    return cmd ? cmd : "wine";
}

const char * getWineFolder(){
    // the default Wine folder is '~/.wine', but it can be
    // override with environment variables.
    // first try our custom variable:
    const char *var = getenv("VSTPLUGIN_WINEPREFIX");
    if (!var){
        // then try the standard variable:
        var = getenv("WINEPREFIX");
    }
    if (!var){
        // finally use the default Wine folder
        var = "~/.wine";
    }
    return var;
}
#endif

#if !USE_STDFS
// helper function
static bool isDirectory(const std::string& dir, dirent *entry){
    // we don't count "." and ".."
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0){
        return false;
    }
#ifdef _DIRENT_HAVE_D_TYPE
    // some filesystems don't support d_type, also we want to follow symlinks
    if (entry->d_type != DT_UNKNOWN && entry->d_type != DT_LNK){
        return (entry->d_type == DT_DIR);
    }
    else
#endif
    {
        struct stat stbuf;
        std::string path = dir + "/" + entry->d_name;
        if (stat(path.c_str(), &stbuf) == 0){
            return S_ISDIR(stbuf.st_mode);
        }
    }
    return false;
}
#endif

// recursively search for a VST plugin in a directory. returns empty string on failure.
std::string find(const std::string &dir, const std::string &path){
    std::string relpath = path;
#ifdef _WIN32
    const char *ext = ".dll";
#elif defined(__APPLE__)
    const char *ext = ".vst";
#else // Linux/BSD/etc.
    const char *ext = ".so";
#endif
    if (relpath.find(".vst3") == std::string::npos && relpath.find(ext) == std::string::npos){
        relpath += ext;
    }
#if USE_STDFS
    try {
        auto wdir = widen(dir);
        auto fpath = fs::path(widen(relpath));
        auto file = fs::path(wdir) / fpath;
        if (fs::is_regular_file(file)){
            return file.u8string(); // success
        }
        // continue recursively
        for (auto& entry : fs::recursive_directory_iterator(wdir)) {
            if (fs::is_directory(entry)){
                file = entry.path() / fpath;
                if (fs::exists(file)){
                    return file.u8string(); // success
                }
            }
        }
    } catch (const fs::filesystem_error& e) {};
    return std::string{};
#else // USE_STDFS
    std::string result;
    // force no trailing slash
    auto root = (dir.back() == '/') ? dir.substr(0, dir.size() - 1) : dir;

    std::string file = root + "/" + relpath;
    if (pathExists(file)){
        return file; // success
    }
    // continue recursively
    std::function<void(const std::string&)> searchDir = [&](const std::string& dirname) {
        DIR *directory = opendir(dirname.c_str());
        struct dirent *entry;
        if (directory){
            while (result.empty() && (entry = readdir(directory))){
                if (isDirectory(dirname, entry)){
                    std::string d = dirname + "/" + entry->d_name;
                    std::string absPath = d + "/" + relpath;
                    if (pathExists(absPath)){
                        result = absPath;
                        break;
                    } else {
                        // dive into the direcotry
                        searchDir(d);
                    }
                }
            }
            closedir(directory);
        }
    };
    searchDir(root);
    return result;
#endif // USE_STDFS
}

// recursively search a directory for VST plugins. for every plugin, 'fn' is called with the full absolute path.
void search(const std::string &dir, std::function<void(const std::string&)> fn, bool filter) {
    // extensions
    std::unordered_set<std::string> extensions;
    for (auto& ext : platformExtensions) {
        extensions.insert(ext);
    }
    // search recursively
#if USE_STDFS
  #ifdef _WIN32
    std::function<void(const std::wstring&)>
  #else
    std::function<void(const std::string&)>
  #endif
    searchDir = [&](const auto& dirname){
        try {
            // LOG_DEBUG("searching in " << shorten(dirname));
            for (auto& entry : fs::directory_iterator(dirname)) {
                // check the extension
                auto path = entry.path();
                auto ext = path.extension().u8string();
                if (extensions.count(ext)) {
                    // found a VST2 plugin (file or bundle)
                    fn(path.u8string());
                } else if (fs::is_directory(path)){
                    // otherwise search it if it's a directory
                    searchDir(path);
                } else if (!filter && fs::is_regular_file(path)){
                    fn(path.u8string());
                }
            }
        } catch (const fs::filesystem_error& e) {
            LOG_DEBUG(e.what());
        };
    };
    searchDir(widen(dir));
#else // USE_STDFS
    // force no trailing slash
    auto root = (dir.back() == '/') ? dir.substr(0, dir.size() - 1) : dir;
    std::function<void(const std::string&)> searchDir = [&](const std::string& dirname) {
        // search alphabetically (ignoring case)
        struct dirent **dirlist;
        auto sortnocase = [](const struct dirent** a, const struct dirent **b) -> int {
            return strcasecmp((*a)->d_name, (*b)->d_name);
        };
        int n = scandir(dirname.c_str(), &dirlist, NULL, sortnocase);
        if (n >= 0) {
            for (int i = 0; i < n; ++i) {
                auto entry = dirlist[i];
                std::string name(entry->d_name);
                std::string absPath = dirname + "/" + name;
                // check the extension
                std::string ext;
                auto extPos = name.find_last_of('.');
                if (extPos != std::string::npos){
                    ext = name.substr(extPos);
                }
                if (extensions.count(ext)){
                    // found a VST2 plugin (file or bundle)
                    fn(absPath);
                } else if (isDirectory(dirname, entry)){
                    // otherwise search it if it's a directory
                    searchDir(absPath);
                } else if (!filter && isFile(absPath)){
                    fn(absPath);
                }
                free(entry);
            }
            free(dirlist);
        }
    };
    searchDir(root);
#endif // USE_STDFS
}

} // vst
