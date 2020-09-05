#include "PluginManager.h"
#include "Utility.h"

#include <fstream>
#include <cstdlib>
#include <sstream>
#include <algorithm>

namespace vst {

void PluginManager::addFactory(const std::string& path, IFactory::ptr factory) {
    WriteLock lock(mutex_);
    factories_[path] = std::move(factory);
}

IFactory::const_ptr PluginManager::findFactory(const std::string& path) const {
    ReadLock lock(mutex_);
    auto factory = factories_.find(path);
    if (factory != factories_.end()){
        return factory->second;
    } else {
        return nullptr;
    }
}

void PluginManager::addException(const std::string &path){
    WriteLock lock(mutex_);
    exceptions_.insert(path);
}

bool PluginManager::isException(const std::string& path) const {
    ReadLock lock(mutex_);
    return exceptions_.count(path) != 0;
}

void PluginManager::addPlugin(const std::string& key, PluginInfo::const_ptr plugin) {
    WriteLock lock(mutex_);
    int index = plugin->bridged() ? BRIDGED : NATIVE;
    plugins_[index][key] = std::move(plugin);
}

PluginInfo::const_ptr PluginManager::findPlugin(const std::string& key) const {
    ReadLock lock(mutex_);
    // first try to find native plugin
    auto it = plugins_[NATIVE].find(key);
    if (it != plugins_[NATIVE].end()){
        return it->second;
    }
    // then try to find bridged plugin
    it = plugins_[BRIDGED].find(key);
    if (it != plugins_[BRIDGED].end()){
        return it->second;
    }
    return nullptr;
}

void PluginManager::clear() {
    WriteLock lock(mutex_);
    factories_.clear();
    for (auto& plugins : plugins_){
        plugins.clear();
    }
    exceptions_.clear();
}

bool getLine(std::istream& stream, std::string& line);
int getCount(const std::string& line);

void PluginManager::read(const std::string& path, bool update){
    ReadLock lock(mutex_);
    int versionMajor = 0, versionMinor = 0, versionBugfix = 0;
    bool outdated = false;
    File file(path);
    std::string line;
    while (getLine(file, line)){
        if (line == "[version]"){
            std::getline(file, line);
            char *pos = (char *)line.c_str();
            if (*pos){
                versionMajor = std::strtol(pos, &pos, 10);
                if (*pos++ == '.'){
                    versionMinor = std::strtol(pos, &pos, 10);
                    if (*pos++ == '.'){
                        versionBugfix = std::strtol(pos, &pos, 10);
                    }
                }
            }
        } else if (line == "[plugins]"){
            std::getline(file, line);
            int numPlugins = getCount(line);
            while (numPlugins--){
                // read a single plugin description
                auto plugin = doReadPlugin(file, versionMajor,
                                           versionMinor, versionBugfix);
                if (plugin){
                    // collect keys
                    std::vector<std::string> keys;
                    std::string line;
                    while (getLine(file, line)){
                        if (line == "[keys]"){
                            std::getline(file, line);
                            int n = getCount(line);
                            while (n-- && std::getline(file, line)){
                                keys.push_back(std::move(line));
                            }
                            break;
                        } else {
                            throw Error("bad format");
                        }
                    }
                    // store plugin at keys
                    for (auto& key : keys){
                        int index = plugin->bridged() ? BRIDGED : NATIVE;
                        plugins_[index][key] = plugin;
                    }
                } else {
                    // plugin is outdated, we need to update the cache
                    outdated = true;
                }
            }
        } else if (line == "[ignore]"){
            std::getline(file, line);
            int numExceptions = getCount(line);
            while (numExceptions-- && std::getline(file, line)){
                exceptions_.insert(line);
            }
        } else {
            throw Error("bad data: " + line);
        }
    }
    if (update && outdated){
        // overwrite file
        file.close();
        try {
            doWrite(path);
        } catch (const Error& e){
            throw Error("couldn't update cache file");
        }
        LOG_VERBOSE("updated cache file");
    }
    LOG_DEBUG("cache file version: v" << versionMajor
              << "." << versionMinor << "." << versionBugfix);
}

PluginInfo::const_ptr PluginManager::readPlugin(std::istream& stream){
    WriteLock lock(mutex_);
    return doReadPlugin(stream, VERSION_MAJOR,
                        VERSION_MINOR, VERSION_PATCH);
}

PluginInfo::const_ptr PluginManager::doReadPlugin(std::istream& stream, int versionMajor,
                                                  int versionMinor, int versionPatch){
    // deserialize plugin
    auto desc = std::make_shared<PluginInfo>(nullptr);
    desc->deserialize(stream, versionMajor, versionMinor, versionPatch);

    // load the factory (if not loaded already) to verify that the plugin still exists
    IFactory::ptr factory;
    if (!factories_.count(desc->path())){
        try {
            factory = IFactory::load(desc->path());
            factories_[desc->path()] = factory;
        } catch (const Error& e){
            // this probably happens when the plugin has been (re)moved
            LOG_ERROR("couldn't load '" << desc->name <<
                      "' (" << desc->path() << "): " << e.what());
            return nullptr; // skip plugin
        }
    } else {
        factory = factories_[desc->path()];
        // check if plugin has already been added
        auto result = factory->findPlugin(desc->name);
        if (result){
            // return existing plugin descriptor
            return result;
        }
    }
    // associate plugin and factory
    desc->setFactory(factory);
    factory->addPlugin(desc);
    // scan presets
    desc->scanPresets();

    return desc;
}

void PluginManager::write(const std::string &path) const {
    WriteLock lock(mutex_);
    doWrite(path);
}

void PluginManager::doWrite(const std::string& path) const {
    File file(path, File::WRITE);
    if (!file.is_open()){
        throw Error("couldn't create file " + path);
    }
    // inverse mapping (plugin -> keys)
    std::unordered_map<PluginInfo::const_ptr, std::vector<std::string>> pluginMap;
    for (auto& plugins : plugins_){
        for (auto& it : plugins){
            pluginMap[it.second].push_back(it.first);
        }
    }
    // write version number
    file << "[version]\n";
    file << VERSION_MAJOR << "." << VERSION_MINOR << "." << VERSION_PATCH << "\n";
    // serialize plugins
    file << "[plugins]\n";
    file << "n=" << pluginMap.size() << "\n";
    for (auto& it : pluginMap){
        // serialize plugin info
        it.first->serialize(file);
        // serialize keys
        file << "[keys]\n";
        auto& keys = it.second;
        file << "n=" << keys.size() << "\n";
        // sort by length, so that the short key comes first
        std::sort(keys.begin(), keys.end(), [](auto& a, auto& b){ return a.size() < b.size(); });
        for (auto& key : keys){
            file << key << "\n";
        }
    }
    // serialize exceptions
    file << "[ignore]\n";
    file << "n=" << exceptions_.size() << "\n";
    for (auto& e : exceptions_){
        file << e << "\n";
    }
    LOG_DEBUG("wrote cache file: " << path);
}

} // vst