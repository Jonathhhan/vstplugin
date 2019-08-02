#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <iostream>
#include <functional>
#include <memory>

// for intptr_t
#ifdef _MSC_VER
#ifdef _WIN64
typedef __int64 intptr_t;
#else
typedef __int32 intptr_t;
#endif
typedef unsigned int uint32_t;
#else
#include <stdint.h>
#endif

#ifndef VSTTHREADS
#define VSTTHREADS 1
#endif

namespace vst {

struct MidiEvent {
    MidiEvent(char status = 0, char data1 = 0, char data2 = 0, int _delta = 0){
        data[0] = status; data[1] = data1; data[2] = data2; delta = _delta;
    }
    char data[3];
    int delta;
};

struct SysexEvent {
    SysexEvent(const char *_data, size_t _size, int _delta = 0)
        : data(_data, _size), delta(_delta){}
    template <typename T>
    SysexEvent(T&& _data, int _delta = 0)
        : data(std::forward<T>(_data)), delta(_delta){}
    SysexEvent() = default;
    std::string data;
    int delta;
};

class IPluginListener {
 public:
    using ptr = std::shared_ptr<IPluginListener>;
    virtual ~IPluginListener(){}
    virtual void parameterAutomated(int index, float value) = 0;
    virtual void midiEvent(const MidiEvent& event) = 0;
    virtual void sysexEvent(const SysexEvent& event) = 0;
};

enum class ProcessPrecision {
    Single,
    Double
};

struct PluginInfo;
class IWindow;

class IPlugin {
 public:
    using ptr = std::unique_ptr<IPlugin>;
    using const_ptr = std::unique_ptr<const IPlugin>;

    virtual ~IPlugin(){}

    virtual const PluginInfo& info() const = 0;
    virtual std::string getPluginName() const = 0;
    virtual std::string getPluginVendor() const = 0;
    virtual std::string getPluginCategory() const = 0;
    virtual std::string getPluginVersion() const = 0;
    virtual std::string getSDKVersion() const = 0;
    virtual int getPluginUniqueID() const = 0;
    virtual int canDo(const char *what) const = 0;
    virtual intptr_t vendorSpecific(int index, intptr_t value, void *p, float opt) = 0;

    virtual void process(const float **inputs, float **outputs, int nsamples) = 0;
    virtual void processDouble(const double **inputs, double **outputs, int nsamples) = 0;
    virtual bool hasPrecision(ProcessPrecision precision) const = 0;
    virtual void setPrecision(ProcessPrecision precision) = 0;
    virtual void suspend() = 0;
    virtual void resume() = 0;
    virtual void setSampleRate(float sr) = 0;
    virtual void setBlockSize(int n) = 0;
    virtual int getNumInputs() const = 0;
    virtual int getNumOutputs() const = 0;
    virtual bool isSynth() const = 0;
    virtual bool hasTail() const = 0;
    virtual int getTailSize() const = 0;
    virtual bool hasBypass() const = 0;
    virtual void setBypass(bool bypass) = 0;
    virtual void setNumSpeakers(int in, int out) = 0;

    virtual void setListener(IPluginListener::ptr listener) = 0;

    virtual void setTempoBPM(double tempo) = 0;
    virtual void setTimeSignature(int numerator, int denominator) = 0;
    virtual void setTransportPlaying(bool play) = 0;
    virtual void setTransportRecording(bool record) = 0;
    virtual void setTransportAutomationWriting(bool writing) = 0;
    virtual void setTransportAutomationReading(bool reading) = 0;
    virtual void setTransportCycleActive(bool active) = 0;
    virtual void setTransportCycleStart(double beat) = 0;
    virtual void setTransportCycleEnd(double beat) = 0;
    virtual void setTransportPosition(double beat) = 0;
    virtual double getTransportPosition() const = 0;

    virtual int getNumMidiInputChannels() const = 0;
    virtual int getNumMidiOutputChannels() const = 0;
    virtual bool hasMidiInput() const = 0;
    virtual bool hasMidiOutput() const = 0;
    virtual void sendMidiEvent(const MidiEvent& event) = 0;
    virtual void sendSysexEvent(const SysexEvent& event) = 0;

    virtual void setParameter(int index, float value) = 0;
    virtual bool setParameter(int index, const std::string& str) = 0;
    virtual float getParameter(int index) const = 0;
    virtual std::string getParameterName(int index) const = 0;
    virtual std::string getParameterLabel(int index) const = 0;
    virtual std::string getParameterDisplay(int index) const = 0;
    virtual int getNumParameters() const = 0;

    virtual void setProgram(int index) = 0;
    virtual void setProgramName(const std::string& name) = 0;
    virtual int getProgram() const = 0;
    virtual std::string getProgramName() const = 0;
    virtual std::string getProgramNameIndexed(int index) const = 0;
    virtual int getNumPrograms() const = 0;

    virtual bool hasChunkData() const = 0;
    virtual void setProgramChunkData(const void *data, size_t size) = 0;
    virtual void getProgramChunkData(void **data, size_t *size) const = 0;
    virtual void setBankChunkData(const void *data, size_t size) = 0;
    virtual void getBankChunkData(void **data, size_t *size) const = 0;

    // the following methods throws an Error exception on failure!
    virtual void readProgramFile(const std::string& path) = 0;
    virtual void readProgramData(const char *data, size_t size) = 0;
    void readProgramData(const std::string& buffer) {
        readProgramData(buffer.data(), buffer.size());
    }
    virtual void writeProgramFile(const std::string& path) = 0;
    virtual void writeProgramData(std::string& buffer) = 0;
    virtual void readBankFile(const std::string& path) = 0;
    virtual void readBankData(const char *data, size_t size) = 0;
    void readBankData(const std::string& buffer) {
        readBankData(buffer.data(), buffer.size());
    }
    virtual void writeBankFile(const std::string& path) = 0;
    virtual void writeBankData(std::string& buffer) = 0;

    virtual bool hasEditor() const = 0;
    virtual void openEditor(void *window) = 0;
    virtual void closeEditor() = 0;
    virtual void getEditorRect(int &left, int &top, int &right, int &bottom) const = 0;

    virtual void setWindow(std::unique_ptr<IWindow> window) = 0;
    virtual IWindow* getWindow() const = 0;
};

class IFactory;

enum class ProbeResult {
    success,
    fail,
    crash,
    none
};

struct PluginInfo {
    using ptr = std::shared_ptr<PluginInfo>;
    using const_ptr = std::shared_ptr<const PluginInfo>;
    using Future = std::function<PluginInfo::ptr()>;

    PluginInfo() = default;
    PluginInfo(const std::shared_ptr<const IFactory>& factory);
    PluginInfo(const std::shared_ptr<const IFactory>& factory, const IPlugin& plugin);
    void setFactory(const std::shared_ptr<const IFactory>& factory){
        factory_ = factory;
    }
    // create new instances
    // throws an Error exception on failure!
    IPlugin::ptr create() const;
    // read/write plugin description
    void serialize(std::ostream& file) const;
    void deserialize(std::istream& file);
    bool valid() const {
        return probeResult == ProbeResult::success;
    }
    // info data
    ProbeResult probeResult = ProbeResult::none;
    std::string path;
    std::string name;
    std::string vendor;
    std::string category;
    std::string version;
    int id = 0;
    int numInputs = 0;
    int numOutputs = 0;
    // parameters
    struct Param {
        std::string name;
        std::string label;
    };
    std::vector<Param> parameters;
    // param name to param index
    std::unordered_map<std::string, int> paramMap;
    // default programs
    std::vector<std::string> programs;
    bool hasEditor() const {
        return flags_ & HasEditor;
    }
    bool isSynth() const {
        return flags_ & IsSynth;
    }
    bool singlePrecision() const {
        return flags_ & SinglePrecision;
    }
    bool doublePrecision() const {
        return flags_ & DoublePrecision;
    }
    bool midiInput() const {
        return flags_ & MidiInput;
    }
    bool midiOutput() const {
        return flags_ & MidiOutput;
    }
    bool sysexInput() const {
        return flags_ & SysexInput;
    }
    bool sysexOutput() const {
        return flags_ & SysexOutput;
    }
 private:
    friend class VST2Plugin;
    friend class VST2Factory;
    friend class VST3Plugin;
    friend class VST3Factory;
    std::weak_ptr<const IFactory> factory_;
    // flags
    enum Flags {
        HasEditor = 1 << 0,
        IsSynth = 1 << 1,
        SinglePrecision = 1 << 2,
        DoublePrecision = 1 << 3,
        MidiInput = 1 << 4,
        MidiOutput = 1 << 5,
        SysexInput = 1 << 6,
        SysexOutput = 1 << 7
    };
    uint32_t flags_ = 0;
    // shell plugin
    struct ShellPlugin {
        std::string name;
        int id;
    };
    std::vector<ShellPlugin> shellPlugins_;
};

class IModule {
 public:
     // throws an Error exception on failure!
    static std::unique_ptr<IModule> load(const std::string& path);
    virtual ~IModule(){}
    virtual bool init() = 0; // VST3 only
    virtual bool exit() = 0; // VST3 only
    template<typename T>
    T getFnPtr(const char *name) const {
        return (T)doGetFnPtr(name);
    }
 protected:
    virtual void *doGetFnPtr(const char *name) const = 0;
};

class IFactory : public std::enable_shared_from_this<IFactory> {
 public:
    using ptr = std::shared_ptr<IFactory>;
    using const_ptr = std::shared_ptr<const IFactory>;
    using ProbeCallback = std::function<void(const PluginInfo&, int, int)>;
    using ProbeFuture = std::function<void(ProbeCallback)>;

    // expects an absolute path to the actual plugin file with or without extension
    // throws an Error exception on failure!
    static IFactory::ptr load(const std::string& path);

    virtual ~IFactory(){}
    virtual void addPlugin(PluginInfo::ptr desc) = 0;
    virtual PluginInfo::const_ptr getPlugin(int index) const = 0;
    virtual int numPlugins() const = 0;
    // throws an Error exception on failure!
    void probe(ProbeCallback callback);
    virtual ProbeFuture probeAsync() = 0;
    virtual bool isProbed() const = 0;
    virtual bool valid() const = 0; // contains at least one valid plugin
    virtual std::string path() const = 0;
    // create a new plugin instance
    // throws an Error exception on failure!
    virtual IPlugin::ptr create(const std::string& name, bool probe = false) const = 0;
 protected:
    PluginInfo::Future probePlugin(const std::string& name, int shellPluginID = 0);
};

class Error : public std::exception {
 public:
    Error() = default;
    Error(const std::string& msg)
        : msg_(msg){}
    const char * what() const noexcept override {
        return msg_.c_str();
    }
 private:
    std::string msg_;
};

// recursively search 'dir' for VST plug-ins. for each plugin, the callback function is evaluated with the absolute path and basename.
void search(const std::string& dir, std::function<void(const std::string&, const std::string&)> fn);

// recursively search 'dir' for a VST plugin. returns empty string on failure
std::string find(const std::string& dir, const std::string& path);

const std::vector<std::string>& getDefaultSearchPaths();

const std::vector<const char *>& getPluginExtensions();

class IWindow {
 public:
    using ptr = std::unique_ptr<IWindow>;
    using const_ptr = std::unique_ptr<const IWindow>;

    virtual ~IWindow() {}

    virtual void* getHandle() = 0; // get system-specific handle to the window

    virtual void setTitle(const std::string& title) = 0;
    virtual void setGeometry(int left, int top, int right, int bottom) = 0;

    virtual void show() = 0;
    virtual void hide() = 0;
    virtual void minimize() = 0;
    virtual void restore() = 0; // un-minimize
    virtual void bringToTop() = 0;
    virtual void update() {}
};

namespace UIThread {
    IPlugin::ptr create(const PluginInfo& info);
    void destroy(IPlugin::ptr plugin);
#if !VSTTTHREADS
    void poll();
#endif
}

} // vst