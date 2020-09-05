#pragma once

#include "Interface.h"
#include "PluginCommand.h"
#include "Sync.h"
#include "Utility.h"

#include <thread>
#include <atomic>
#include <vector>

#ifdef _WIN32
# include <Windows.h>
#else
# include <sys/types.h>
# include <unistd.h>
#endif

namespace vst {

class PluginServer;
class ShmInterface;
class ShmChannel;

/*///////////////// PluginHandleListener ////////*/

class PluginHandle;

class PluginHandleListener : public IPluginListener {
 public:
    PluginHandleListener(PluginHandle &owner)
        : owner_(&owner){}

    void parameterAutomated(int index, float value) override;
    void latencyChanged(int nsamples) override;
    void pluginCrashed() override {} // never called inside the bridge
    void midiEvent(const MidiEvent& event) override;
    void sysexEvent(const SysexEvent& event) override;
 private:
    PluginHandle *owner_;
};

/*///////////////// PluginHandle ///////////////*/

class PluginHandle {
 public:
    PluginHandle(PluginServer& server, IPlugin::ptr plugin,
                 uint32_t id, ShmChannel& channel);

    void handleRequest(const ShmCommand& cmd, ShmChannel& channel);
    void handleUICommand(const ShmUICommand& cmd);
 private:
    friend class PluginHandleListener;

    void updateBuffer();

    void process(const ShmCommand& cmd, ShmChannel& channel);

    template<typename T>
    void doProcess(const ShmCommand& cmd, ShmChannel& channel);

    void dispatchCommands(ShmChannel& channel);

    void sendEvents(ShmChannel& channel);

    void sendParameterUpdate(ShmChannel& channel);
    void sendProgramUpdate(ShmChannel& channel, bool bank);

    void requestParameterUpdate(int32_t index, float value);

    static bool addReply(ShmChannel& channel, const void *cmd, size_t size = 0);

    PluginServer *server_ = nullptr;
    IPlugin::ptr plugin_;
    std::shared_ptr<PluginHandleListener> proxy_;
    uint32_t id_ = 0;

    int maxBlockSize_ = 64;
    ProcessPrecision precision_{ProcessPrecision::Single};
    int numInputs_ = 0;
    int numOutputs_ = 0;
    int numAuxInputs_ = 0;
    int numAuxOutputs_ = 0;
    std::vector<char> buffer_;
    std::vector<Command> events_;

    // parameter automation from GUI
    // -> ask RT thread to send parameter state
    struct Param {
        int32_t index;
        float value;
    };
    LockfreeFifo<Param, 16> paramAutomated_;

    // cached parameter state
    std::unique_ptr<float[]> paramState_;

    void sendParam(ShmChannel& channel, int index,
                   float value, bool automated);
};

#define AddReply(cmd, field) addReply(&(cmd), (cmd).headerSize + sizeof((cmd).field))

/*////////////////// PluginServer ////////////////*/

class PluginServer {
 public:
    PluginServer(int pid, const std::string& shmPath);
    ~PluginServer();

    void run();

    void postUIThread(const ShmUICommand& cmd);
 private:
    void pollUIThread();
    void checkParentAlive();
    void runThread(ShmChannel* channel);
    void handleCommand(ShmChannel& channel,
                       const ShmCommand &cmd);
    void quit();

    void createPlugin(uint32_t id, const char *data, size_t size,
                      ShmChannel& channel);

    void destroyPlugin(uint32_t id);

    PluginHandle *findPlugin(uint32_t id);

#ifdef _WIN32
    HANDLE parent_ = 0;
#else
    int parent_ = -1;
#endif
    std::unique_ptr<ShmInterface> shm_;
    std::vector<std::thread> threads_;
    std::atomic<bool> running_;
    UIThread::Handle pollFunction_;

    std::unordered_map<uint32_t, std::unique_ptr<PluginHandle>> plugins_;
    SharedMutex pluginMutex_;
};

} // vst