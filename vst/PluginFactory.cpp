#include "PluginFactory.h"

#include "Log.h"
#include "FileUtils.h"
#include "MiscUtils.h"
#if USE_VST2
 #include "VST2Plugin.h"
#endif
#if USE_VST3
 #include "VST3Plugin.h"
#endif

// for probing
#ifdef _WIN32
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <windows.h>
#else
# include <unistd.h>
# include <stdlib.h>
# include <stdio.h>
# include <dlfcn.h>
# include <sys/wait.h>
# include <signal.h>
#endif

#include <thread>
#include <mutex>
#include <algorithm>
#include <sstream>

namespace vst {

/*////////////////////// platform ///////////////////*/

#ifdef _WIN32

static HINSTANCE hInstance = 0;

const std::wstring& getModuleDirectory(){
    static std::wstring dir = [](){
        wchar_t wpath[MAX_PATH+1];
        if (GetModuleFileNameW(hInstance, wpath, MAX_PATH) > 0){
            wchar_t *ptr = wpath;
            int pos = 0;
            while (*ptr){
                if (*ptr == '\\'){
                    pos = (ptr - wpath);
                }
                ++ptr;
            }
            wpath[pos] = 0;
            // LOG_DEBUG("dll directory: " << shorten(wpath));
            return std::wstring(wpath);
        } else {
            LOG_ERROR("getModuleDirectory: GetModuleFileNameW() failed!");
            return std::wstring();
        }
    }();
    return dir;
}

extern "C" {
    BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved){
        if (fdwReason == DLL_PROCESS_ATTACH){
            hInstance = hinstDLL;
        }
        return TRUE;
    }
}

#else // Linux, macOS

const std::string& getModuleDirectory(){
    static std::string dir = [](){
        // hack: obtain library info through a function pointer (vst::search)
        Dl_info dlinfo;
        if (!dladdr((void *)search, &dlinfo)) {
            throw Error(Error::SystemError, "getModuleDirectory: dladdr() failed!");
        }
        std::string path = dlinfo.dli_fname;
        auto end = path.find_last_of('/');
        return path.substr(0, end);
    }();
    return dir;
}

#endif // WIN32

std::string getHostApp(CpuArch arch){
    if (arch == getHostCpuArchitecture()){
    #ifdef _WIN32
        return "host.exe";
    #else
        return "host";
    #endif
    } else {
    #if USE_WINE
        if (arch == CpuArch::pe_i386){
            return "host_i386.exe.so";
        } else if (arch == CpuArch::pe_amd64){
            return "host_amd64.exe.so";
        }
    #endif
        std::string host = std::string("host_") + cpuArchToString(arch);
    #ifdef _WIN32
        host += ".exe";
    #endif
        return host;
    }
}

/*///////////////////// IFactory ////////////////////////*/

IFactory::ptr IFactory::load(const std::string& path, bool probe){
    // LOG_DEBUG("IFactory: loading " << path);
    auto ext = fileExtension(path);
    if (ext == ".vst3"){
    #if USE_VST3
        if (!pathExists(path)){
            throw Error(Error::ModuleError, "No such file");
        }
        return std::make_shared<VST3Factory>(path, probe);
    #else
        throw Error(Error::ModuleError, "VST3 plug-ins not supported");
    #endif
    } else {
    #if USE_VST2
        std::string realPath = path;
        if (ext.empty()){
            // no extension: assume VST2 plugin
        #ifdef _WIN32
            realPath += ".dll";
        #elif defined(__APPLE__)
            realPath += ".vst";
        #else // Linux/BSD/etc.
            realPath += ".so";
        #endif
        }
        if (!pathExists(realPath)){
            throw Error(Error::ModuleError, "No such file");
        }
        return std::make_shared<VST2Factory>(realPath, probe);
    #else
        throw Error(Error::ModuleError, "VST2 plug-ins not supported");
    #endif
    }
}

/*/////////////////////////// PluginFactory ////////////////////////*/

PluginFactory::PluginFactory(const std::string &path)
    : path_(path)
{
    auto archs = getCpuArchitectures(path);
    auto hostArch = getHostCpuArchitecture();

    if (std::find(archs.begin(), archs.end(), hostArch) != archs.end()){
        arch_ = hostArch;
    } else {
    #if USE_BRIDGE
        // Generally, we can bridge between any kinds of CPU architectures,
        // as long as the they are supported by the platform in question.
        //
        // We use the following naming scheme for the plugin bridge app:
        // host_<cpu_arch>[extension]
        // Examples: "host_i386", "host_amd64.exe", etc.
        //
        // We can selectively enable/disable CPU architectures simply by
        // including resp. omitting the corresponding app.
        // Note that we always ship a version of the *same* CPU architecture
        // called "host" resp. "host.exe" to support plugin sandboxing.
        //
        // Bridging between i386 and amd64 is typically employed on Windows,
        // but also possible on Linux and macOS (before 10.15).
        // On the upcoming ARM MacBooks, we can also bridge between amd64 and aarch64.
        // NOTE: We ship 64-bit Intel builds on Linux without "host_i386" and
        // ask people to compile it themselves if they need it.
        //
        // On macOS and Linux we can also use the plugin bridge to run Windows plugins
        // via Wine. The apps are called "host_pe_i386.exe" and "host_pe_amd64.exe".
        auto canBridge = [](auto arch){
            // check if host app exists
        #ifdef _WIN32
            auto path = shorten(getModuleDirectory()) + "\\" + getHostApp(arch);
        #else
            auto path = getModuleDirectory() + "/" + getHostApp(arch);
          #if USE_WINE
            if (arch == CpuArch::pe_i386 || arch == CpuArch::pe_amd64){
                // check if the 'wine' command can be found and works.
                // we only need to do this once!
                static bool haveWine = [](){
                    auto winecmd = getWineCommand();
                    // we pass valid arguments, so the exit code should be 0.
                    char cmdline[256];
                    snprintf(cmdline, sizeof(cmdline), "%s --version", winecmd);
                    int ret = system(cmdline);
                    if (ret < 0){
                        LOG_WARNING("Couldn't execute '" << winecmd << "': "
                                    << strerror(errno));
                        return false;
                    } else {
                        auto code = WEXITSTATUS(ret);
                        if (code == 0){
                            return true;
                        } else {
                            LOG_WARNING("'wine' command failed with exit code "
                                        << WEXITSTATUS(ret));
                            return false;
                        }
                    }
                }();
                if (!haveWine){
                    return false;
                }
            }
          #endif // USE_WINE
        #endif
            if (pathExists(path)){
                // LATER try to execute the bridge app?
                return true;
            } else {
                LOG_WARNING("Can't locate " << path);
                return false;
            }
        };

        // check if can bridge any of the given CPU architectures
        for (auto& arch : archs){
            if (canBridge(arch)){
                arch_ = arch;
                // LOG_DEBUG("created bridged plugin factory " << path);
                return;
            }
        }
        // fail
        if (archs.size() > 1){
            throw Error(Error::ModuleError, "Can't bridge CPU architectures");
        } else {
            throw Error(Error::ModuleError, "Can't bridge CPU architecture "
                        + std::string(cpuArchToString(archs.front())));
        }
    #else // USE_BRIDGE
        if (archs.size() > 1){
            throw Error(Error::ModuleError, "Unsupported CPU architectures");
        } else {
            throw Error(Error::ModuleError, "Unsupported CPU architecture "
                        + std::string(cpuArchToString(archs.front())));
        }
    #endif // USE_BRIDGE
    }
}

IFactory::ProbeFuture PluginFactory::probeAsync(bool nonblocking) {
    plugins_.clear();
    pluginMap_.clear();
    return [this,
            self = shared_from_this(),
            f = doProbePlugin(nonblocking)]
            (ProbeCallback callback){
        // call future
        ProbeResult result;
        if (f(result)){
            if (result.plugin->subPlugins.empty()){
                // factory only contains a single plugin
                if (result.valid()) {
                    plugins_ = { result.plugin };
                }
                if (callback){
                    callback(result);
                }
            } else {
                // factory contains several subplugins
                plugins_ = doProbePlugins(result.plugin->subPlugins, callback);
            }
            for (auto& desc : plugins_) {
                pluginMap_[desc->name] = desc;
            }
            return true;
        } else {
            return false;
        }
    };
}

void PluginFactory::addPlugin(PluginInfo::ptr desc){
    if (!pluginMap_.count(desc->name)){
        plugins_.push_back(desc);
        pluginMap_[desc->name] = desc;
    }
}

PluginInfo::const_ptr PluginFactory::getPlugin(int index) const {
    if (index >= 0 && index < (int)plugins_.size()){
        return plugins_[index];
    } else {
        return nullptr;
    }
}

PluginInfo::const_ptr PluginFactory::findPlugin(const std::string& name) const {
    auto it = pluginMap_.find(name);
    if (it != pluginMap_.end()){
        return it->second;
    } else {
        return nullptr;
    }
}

int PluginFactory::numPlugins() const {
    return plugins_.size();
}

// should host.exe inherit file handles and print to stdout/stderr?
#define PROBE_LOG 0

PluginFactory::ProbeResultFuture PluginFactory::doProbePlugin(bool nonblocking){
    return doProbePlugin(PluginInfo::SubPlugin { "", -1 }, nonblocking);
}

// probe a plugin in a seperate process and return the info in a file
PluginFactory::ProbeResultFuture PluginFactory::doProbePlugin(
        const PluginInfo::SubPlugin& sub, bool nonblocking)
{
    auto desc = std::make_shared<PluginInfo>(shared_from_this());
    desc->name = sub.name; // necessary for error reporting, will be overriden later
    // turn id into string
    char idString[12];
    if (sub.id >= 0){
        snprintf(idString, sizeof(idString), "0x%X", sub.id);
    } else {
        sprintf(idString, "_");
    }
    // create temp file path
    std::stringstream ss;
    // desc address should be unique as long as PluginInfos are retained.
    ss << getTmpDirectory() << "/vst_" << desc.get();
    std::string tmpPath = ss.str();
    // LOG_DEBUG("temp path: " << tmpPath);
    std::string hostApp = getHostApp(arch_);
#ifdef _WIN32
    // get absolute path to host app
    std::wstring hostPath = getModuleDirectory() + L"\\" + widen(hostApp);
    /// LOG_DEBUG("host path: " << shorten(hostPath));
    // arguments: host.exe probe <plugin_path> <plugin_id> <file_path>
    // on Windows we need to quote the arguments for _spawn to handle spaces in file names.
    std::stringstream cmdLineStream;
    cmdLineStream << hostApp << " probe "
            << "\"" << path() << "\" " << idString
            << " \"" << tmpPath + "\"";
    // LOG_DEBUG(cmdLineStream.str());
    auto cmdLine = widen(cmdLineStream.str());
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(hostPath.c_str(), &cmdLine[0], NULL, NULL,
                        PROBE_LOG, DETACHED_PROCESS, NULL, NULL, &si, &pi)){
        auto err = GetLastError();
        std::stringstream ss;
        ss << "couldn't open host process " << hostApp << " (" << errorMessage(err) << ")";
        throw Error(Error::SystemError, ss.str());
    }
    auto wait = [pi, nonblocking](DWORD& code){
        const DWORD timeout = (PROBE_TIMEOUT > 0) ? PROBE_TIMEOUT * 1000 : INFINITE;
        auto res = WaitForSingleObject(pi.hProcess, nonblocking ? 0 : timeout);
        if (res == WAIT_TIMEOUT){
            if (nonblocking){
                return false;
            } else {
                if (TerminateProcess(pi.hProcess, EXIT_FAILURE)){
                    LOG_DEBUG("terminated hanging subprocess");
                } else {
                    LOG_ERROR("couldn't terminate hanging subprocess: "
                              << errorMessage(GetLastError()));
                }
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                std::stringstream msg;
                msg << "subprocess timed out after " << PROBE_TIMEOUT << " seconds!";
                throw Error(Error::SystemError, msg.str());
            }
        } else if (res == WAIT_OBJECT_0){
            if (!GetExitCodeProcess(pi.hProcess, &code)){
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                throw Error(Error::SystemError, "couldn't retrieve exit code for subprocess!");
            }
        } else {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            throw Error(Error::SystemError, "WaitForSingleObject() failed: " + errorMessage(GetLastError()));
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
    };
#else // Unix
    // get absolute path to host app
    std::string hostPath = getModuleDirectory() + "/" + hostApp;
    // timeout
    auto timeout = std::to_string(PROBE_TIMEOUT);
    // fork
    pid_t pid = fork();
    if (pid == -1) {
        throw Error(Error::SystemError, "fork() failed!");
    } else if (pid == 0) {
        // child process: start new process with plugin path and temp file path as arguments.
        // we must not quote arguments to exec!
    #if !PROBE_LOG
        // disable stdout and stderr
        auto nullOut = fopen("/dev/null", "w");
        fflush(stdout);
        dup2(fileno(nullOut), STDOUT_FILENO);
        fflush(stderr);
        dup2(fileno(nullOut), STDERR_FILENO);
    #endif
        // arguments: host probe <plugin_path> <plugin_id> <file_path> [timeout]
    #if USE_WINE
        if (arch_ == CpuArch::pe_i386 || arch_ == CpuArch::pe_amd64){
            const char *winecmd = getWineCommand();
            // use PATH!
            if (execlp(winecmd, winecmd, hostPath.c_str(), "probe", path().c_str(),
                       idString, tmpPath.c_str(), timeout.c_str(), nullptr) < 0) {
                // LATER redirect child stderr to parent stdin
                LOG_ERROR("couldn't run 'wine' (" << errorMessage(errno) << ")");
            }
        } else
    #endif
        if (execl(hostPath.c_str(), hostApp.c_str(), "probe", path().c_str(),
                  idString, tmpPath.c_str(), timeout.c_str(), nullptr) < 0) {
            // write error to temp file
            int err = errno;
            File file(tmpPath, File::WRITE);
            if (file.is_open()){
                file << static_cast<int>(Error::SystemError) << "\n";
                file << "couldn't run subprocess " << hostApp
                     << ": " << errorMessage(err) << "\n";
            }
        }
        std::exit(EXIT_FAILURE);
    }
    // parent process: wait for child
    auto wait = [pid, nonblocking](int& code){
        int status = 0;
        if (waitpid(pid, &status, nonblocking ? WNOHANG : 0) == 0){
            return false;
        }
        if (WIFEXITED(status)) {
            code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)){
            auto sig = WTERMSIG(status);
            std::stringstream msg;
            msg << "subprocess was terminated with signal "
               << sig << " (" << strsignal(sig) << ")";
            throw Error(Error::SystemError, msg.str());
        } else if (WIFSTOPPED(status)){
            auto sig = WSTOPSIG(status);
            std::stringstream msg;
            msg << "subprocess was stopped with signal "
               << sig << " (" << strsignal(sig) << ")";
            throw Error(Error::SystemError, msg.str());
        } else if (WIFCONTINUED(status)){
            // FIXME what should be do here?
            throw Error(Error::SystemError, "subprocess continued");
        } else {
            std::stringstream msg;
            msg << "unknown exit status (" << status << ")";
            throw Error(Error::SystemError, msg.str());
        }
        return true;
    };
#endif
    return [desc=std::move(desc),
            tmpPath=std::move(tmpPath),
            wait=std::move(wait),
        #ifdef _WIN32
            pi,
        #else
            pid,
        #endif
            start = std::chrono::system_clock::now()]
            (ProbeResult& result) {
    #ifdef _WIN32
        DWORD code;
    #else
        int code;
    #endif
        result.plugin = desc;
        result.total = 1;
        // wait for process to finish
        // (returns false when nonblocking and still running)
        try {
            if (!wait(code)){
                if (PROBE_TIMEOUT > 0) {
                    using seconds = std::chrono::duration<double>;
                    auto now = std::chrono::system_clock::now();
                    auto elapsed = std::chrono::duration_cast<seconds>(now - start).count();
                    if (elapsed > PROBE_TIMEOUT){
                        // IMPORTANT: wait in a loop to check if the subprocess is really stuck!
                        // note that we're effectively using twice the specified probe time out value.
                        // maybe just use half the time each?
                        // LATER find a better way...
                        LOG_DEBUG(desc->path() << ": check timeout");
                        auto t1 = std::chrono::system_clock::now();
                        for (;;){
                            std::this_thread::sleep_for(std::chrono::milliseconds(PROBE_SLEEP_MS));

                            if (wait(code)){
                                goto probe_success; // ugly but legal
                            }

                            auto t2 = std::chrono::system_clock::now();
                            auto diff = std::chrono::duration_cast<seconds>(t2 - t1).count();
                            if (diff > PROBE_TIMEOUT){
                                LOG_DEBUG(desc->path() << ": really timed out!");
                                break;
                            }
                        }
                    #ifdef _WIN32
                        if (TerminateProcess(pi.hProcess, EXIT_FAILURE)){
                            LOG_DEBUG("terminated hanging subprocess");
                        } else {
                            LOG_ERROR("couldn't terminate hanging subprocess: "
                                      << errorMessage(GetLastError()));
                        }
                        CloseHandle(pi.hProcess);
                        CloseHandle(pi.hThread);
                    #else
                        if (kill(pid, SIGTERM) == 0){
                            LOG_DEBUG("terminated hanging subprocess");
                        } else {
                            LOG_ERROR("couldn't terminate hanging subprocess: "
                                      << errorMessage(errno));
                        }
                    #endif
                        std::stringstream msg;
                        msg << "subprocess timed out after " << PROBE_TIMEOUT << " seconds!";
                        throw Error(Error::SystemError, msg.str());
                    }
                }
                return false;
            }
        } catch (const Error& e){
            result.error = e;
            return true;
        }
    probe_success:
        /// LOG_DEBUG("return code: " << ret);
        TmpFile file(tmpPath); // removes the file on destruction
        if (code == EXIT_SUCCESS) {
            // get info from temp file
            if (file.is_open()) {
                try {
                    desc->deserialize(file);
                } catch (const Error& e) {
                    result.error = e;
                }
            } else {
            #if USE_WINE
                // On Wine, the child process (wine) might exit with 0
                // even though the grandchild (= host) has crashed.
                // The missing temp file is the only indicator we have...
                if (desc->arch() == CpuArch::pe_amd64 || desc->arch() == CpuArch::pe_i386){
                #if 1
                    result.error = Error(Error::SystemError,
                                         "couldn't read temp file (plugin crashed?)");
                #else
                    result.error = Error(Error::Crash);
                #endif
                } else
            #endif
                {
                    result.error = Error(Error::SystemError, "couldn't read temp file!");
                }
            }
        } else if (code == EXIT_FAILURE) {
            // get error from temp file
            if (file.is_open()) {
                int err;
                std::string msg;
                file >> err;
                if (file){
                    std::getline(file, msg); // skip newline
                    std::getline(file, msg); // read message
                } else {
                    // happens in certain cases, e.g. the plugin destructor
                    // terminates the probe process with exit code 1.
                    err = (int)Error::UnknownError;
                    msg = "(uncaught exception)";
                }
                LOG_DEBUG("code: " << err << ", msg: " << msg);
                result.error = Error((Error::ErrorCode)err, msg);
            } else {
                result.error = Error(Error::UnknownError, "(uncaught exception)");
            }
        } else {
            // ignore temp file
            result.error = Error(Error::Crash);
        }
        return true;
    };
}

std::vector<PluginInfo::ptr> PluginFactory::doProbePlugins(
        const PluginInfo::SubPluginList& pluginList, ProbeCallback callback)
{
    std::vector<PluginInfo::ptr> results;
    int numPlugins = pluginList.size();
#ifdef PLUGIN_LIMIT
    numPlugins = std::min<int>(numPlugins, PLUGIN_LIMIT);
#endif
    // LOG_DEBUG("numPlugins: " << numPlugins);
    auto plugin = pluginList.begin();
    int count = 0;
    int maxNumFutures = std::min<int>(PROBE_FUTURES, numPlugins);
    std::vector<ProbeResultFuture> futures;
    while (count < numPlugins) {
        // push futures
        while (futures.size() < maxNumFutures && plugin != pluginList.end()){
            try {
                futures.push_back(doProbePlugin(*plugin++, true));
            } catch (const Error& e){
                // return error future
                futures.push_back([=](ProbeResult& result){
                    result.error = e;
                    return true;
                });
            }
        }
        // collect results
        for (auto it = futures.begin(); it != futures.end();) {
            ProbeResult result;
            // call future (non-blocking)
            if ((*it)(result)){
                result.index = count++;
                result.total = numPlugins;
                if (result.valid()) {
                    results.push_back(result.plugin);
                }
                if (callback){
                    callback(result);
                }
                // remove future
                it = futures.erase(it);
            } else {
                it++;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(PROBE_SLEEP_MS));
    }
    return results;
}

} // vst
