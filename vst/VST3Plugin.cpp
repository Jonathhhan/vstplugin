#include "VST3Plugin.h"
#include "Utility.h"

#include <cstring>

DEF_CLASS_IID (IPluginBase)
DEF_CLASS_IID (IPlugView)
// DEF_CLASS_IID (IPlugFrame)
DEF_CLASS_IID (IPluginFactory)
DEF_CLASS_IID (IPluginFactory2)
DEF_CLASS_IID (IPluginFactory3)
DEF_CLASS_IID (Vst::IComponent)
DEF_CLASS_IID (Vst::IEditController)

#if SMTG_OS_LINUX
DEF_CLASS_IID (Linux::IEventHandler)
DEF_CLASS_IID (Linux::ITimerHandler)
DEF_CLASS_IID (Linux::IRunLoop)
#endif



namespace vst {

VST3Factory::VST3Factory(const std::string& path)
    : path_(path), module_(IModule::load(path))
{
    if (!module_){
        // shouldn't happen...
        throw Error("couldn't load module!");
    }
    auto factoryProc = module_->getFnPtr<GetFactoryProc>("GetPluginFactory");
    if (!factoryProc){
        throw Error("couldn't find 'GetPluginFactory' function");
    }
    if (!module_->init()){
        throw Error("couldn't init module");
    }
    factory_ = IPtr<IPluginFactory>(factoryProc());
    if (!factory_){
        throw Error("couldn't get VST3 plug-in factory");
    }
    /// LOG_DEBUG("VST3Factory: loaded " << path);
    // map plugin names to indices
    auto numPlugins = factory_->countClasses();
    /// LOG_DEBUG("module contains " << numPlugins << " classes");
    for (int i = 0; i < numPlugins; ++i){
        PClassInfo ci;
        if (factory_->getClassInfo(i, &ci) == kResultTrue){
            /// LOG_DEBUG("\t" << ci.name << ", " << ci.category);
            if (!strcmp(ci.category, kVstAudioEffectClass)){
                pluginList_.push_back(ci.name);
                pluginIndexMap_[ci.name] = i;
            }
        } else {
            throw Error("couldn't get class info!");
        }
    }
}

VST3Factory::~VST3Factory(){
    if (!module_->exit()){
        // don't throw!
        LOG_ERROR("couldn't exit module");
    }
    // LOG_DEBUG("freed VST3 module " << path_);
}

void VST3Factory::addPlugin(PluginInfo::ptr desc){
    if (!pluginMap_.count(desc->name)){
        plugins_.push_back(desc);
        pluginMap_[desc->name] = desc;
    }
}

PluginInfo::const_ptr VST3Factory::getPlugin(int index) const {
    if (index >= 0 && index < (int)plugins_.size()){
        return plugins_[index];
    } else {
        return nullptr;
    }
}

int VST3Factory::numPlugins() const {
    return plugins_.size();
}

IFactory::ProbeFuture VST3Factory::probeAsync() {
    if (pluginList_.empty()){
        throw Error("factory doesn't have any plugin(s)");
    }
    plugins_.clear();
    pluginMap_.clear();
    auto self(shared_from_this());
    /// LOG_DEBUG("got probePlugin future");
    if (pluginList_.size() > 1){
        return [this, self=std::move(self)](ProbeCallback callback){
            ProbeList pluginList;
            for (auto& name : pluginList_){
                pluginList.emplace_back(name, 0);
            }
            plugins_ = probePlugins(pluginList, callback, valid_);
            for (auto& desc : plugins_){
                pluginMap_[desc->name] = desc;
            }
        };
    } else {
        auto f = probePlugin(pluginList_[0]);
        return [this, self=std::move(self), f=std::move(f)](ProbeCallback callback){
            auto result = f();
            plugins_ = { result };
            valid_ = result->valid();
            if (callback){
                callback(*result, 0, 1);
            }
            pluginMap_[result->name] = result;
        };
    }
}

std::unique_ptr<IPlugin> VST3Factory::create(const std::string& name, bool probe) const {
    PluginInfo::ptr desc = nullptr; // will stay nullptr when probing!
    if (!probe){
        if (plugins_.empty()){
            throw Error("factory doesn't have any plugin(s)");
        }
        auto it = pluginMap_.find(name);
        if (it == pluginMap_.end()){
            throw Error("can't find (sub)plugin '" + name + "'");
        }
        desc = it->second;
        if (!desc->valid()){
            throw Error("plugin not probed successfully");
        }
    }
    return std::make_unique<VST3Plugin>(factory_, pluginIndexMap_[name], shared_from_this(), desc);
}

/*/////////////////////// VST3Plugin /////////////////////////////*/

template <typename T>
inline IPtr<T> createInstance (IPtr<IPluginFactory> factory, TUID iid){
    T* obj = nullptr;
    if (factory->createInstance (iid, T::iid, reinterpret_cast<void**> (&obj)) == kResultTrue){
        return owned(obj);
    } else {
        return nullptr;
    }
}

static FUnknown *gPluginContext = nullptr;

VST3Plugin::VST3Plugin(IPtr<IPluginFactory> factory, int which, IFactory::const_ptr f, PluginInfo::const_ptr desc)
    : factory_(std::move(f)), desc_(std::move(desc))
{
    // are we probing?
    auto newDesc = !desc_ ? std::make_shared<PluginInfo>(factory_, *this) : nullptr;

    TUID uid;
    PClassInfo2 ci2;
    auto factory2 = FUnknownPtr<IPluginFactory2> (factory);
    if (factory2 && factory2->getClassInfo2(which, &ci2) == kResultTrue){
        memcpy(uid, ci2.cid, sizeof(TUID));
        if (newDesc){
            newDesc->name = ci2.name;
            newDesc->category = ci2.subCategories;
            newDesc->vendor = ci2.vendor;
            newDesc->version = ci2.version;
            newDesc->sdkVersion = ci2.sdkVersion;
        }
    } else {
        Steinberg::PClassInfo ci;
        if (factory->getClassInfo(which, &ci) == kResultTrue){
            memcpy(uid, ci.cid, sizeof(TUID));
            if (newDesc){
                newDesc->name = ci.name;
                newDesc->category = "Uncategorized";
                newDesc->version = "0.0.0";
                newDesc->sdkVersion = "VST 3";
            }
        } else {
            throw Error("couldn't get class info!");
        }
    }
    // fill vendor
    if (newDesc && newDesc->vendor.empty()){
        PFactoryInfo i;
        if (factory->getFactoryInfo(&i) == kResultTrue){
            newDesc->vendor = i.vendor;
        } else {
            newDesc->vendor = "Unknown";
        }
    }
    if (newDesc){
        desc_ = newDesc;
    }
    // create component
    if (!(component_ = createInstance<Vst::IComponent>(factory, uid))){
        throw Error("couldn't create VST3 component");
    }
    LOG_DEBUG("created VST3 component");
    // initialize component
    if (component_->initialize(gPluginContext) != kResultOk){
        throw Error("couldn't initialize VST3 component");
    }
    // first try to create controller from the component part
    if (component_->queryInterface(Vst::IEditController::iid, (void**)&controller_) != kResultTrue){
        // if this fails, try to instantiate controller class
        TUID controllerCID;
        static TUID nulluid = {0};
        if (component_->getControllerClassId(controllerCID) == kResultTrue
                && memcmp(controllerCID, nulluid, sizeof (TUID)) != 0){
            if ((controller_ = createInstance<Vst::IEditController>(factory, controllerCID))
                && controller_->initialize(gPluginContext) != kResultOk){
                throw Error("couldn't initialize VST3 controller");
            }
        }
    }
    if (controller_){
        LOG_DEBUG("created VST3 controller");
    } else {
        LOG_DEBUG("no VST3 controller!");
    }
}

VST3Plugin::~VST3Plugin(){

}

std::string VST3Plugin::getPluginName() const {
    if (desc_){
        return desc_->name;
    } else {
        return "";
    }
}

std::string VST3Plugin::getPluginVendor() const {
    if (desc_){
        return desc_->vendor;
    } else {
        return "";
    }
}

std::string VST3Plugin::getPluginCategory() const {
    if (desc_){
        return desc_->category;
    } else {
        return "";
    }
}

std::string VST3Plugin::getPluginVersion() const {
    if (desc_){
        return desc_->version;
    } else {
        return "";
    }
}

std::string VST3Plugin::getSDKVersion() const {
    if (desc_){
        return desc_->sdkVersion;
    } else {
        return "";
    }
}

int VST3Plugin::getPluginUniqueID() const {
    return 0;
}

int VST3Plugin::canDo(const char *what) const {
    return 0;
}

intptr_t VST3Plugin::vendorSpecific(int index, intptr_t value, void *p, float opt){
    return 0;
}

void VST3Plugin::process(const float **inputs, float **outputs, int sampleFrames){

}

void VST3Plugin::processDouble(const double **inputs, double **outputs, int sampleFrames){

}

bool VST3Plugin::hasPrecision(ProcessPrecision precision) const {
    return false;
}

void VST3Plugin::setPrecision(ProcessPrecision precision){

}

void VST3Plugin::suspend(){

}

void VST3Plugin::resume(){

}

void VST3Plugin::setSampleRate(float sr){

}

void VST3Plugin::setBlockSize(int n){

}

int VST3Plugin::getNumInputs() const {
    return 0;
}

int VST3Plugin::getNumOutputs() const {
    return 0;
}

bool VST3Plugin::isSynth() const {
    return false;
}

bool VST3Plugin::hasTail() const {
    return false;
}

int VST3Plugin::getTailSize() const {
    return 0;
}

bool VST3Plugin::hasBypass() const {
    return false;
}

void VST3Plugin::setBypass(bool bypass){

}

void VST3Plugin::setNumSpeakers(int in, int out){

}

void VST3Plugin::setTempoBPM(double tempo){

}

void VST3Plugin::setTimeSignature(int numerator, int denominator){

}

void VST3Plugin::setTransportPlaying(bool play){

}

void VST3Plugin::setTransportRecording(bool record){

}

void VST3Plugin::setTransportAutomationWriting(bool writing){

}

void VST3Plugin::setTransportAutomationReading(bool reading){

}

void VST3Plugin::setTransportCycleActive(bool active){

}

void VST3Plugin::setTransportCycleStart(double beat){

}

void VST3Plugin::setTransportCycleEnd(double beat){

}

void VST3Plugin::setTransportPosition(double beat){

}

double VST3Plugin::getTransportPosition() const {
    return 0;
}

int VST3Plugin::getNumMidiInputChannels() const {
    return 0;
}

int VST3Plugin::getNumMidiOutputChannels() const {
    return 0;
}

bool VST3Plugin::hasMidiInput() const {
    return false;
}

bool VST3Plugin::hasMidiOutput() const {
    return false;
}

void VST3Plugin::sendMidiEvent(const MidiEvent &event){

}

void VST3Plugin::sendSysexEvent(const SysexEvent &event){

}

void VST3Plugin::setParameter(int index, float value){

}

bool VST3Plugin::setParameter(int index, const std::string &str){
    return false;
}

float VST3Plugin::getParameter(int index) const {
    return 0;
}

std::string VST3Plugin::getParameterName(int index) const {
    return std::string{};
}

std::string VST3Plugin::getParameterLabel(int index) const {
    return std::string{};
}

std::string VST3Plugin::getParameterDisplay(int index) const {
    return std::string{};
}

int VST3Plugin::getNumParameters() const {
    return 0;
}

void VST3Plugin::setProgram(int program){
}

void VST3Plugin::setProgramName(const std::string& name){
}

int VST3Plugin::getProgram() const {
    return 0;
}

std::string VST3Plugin::getProgramName() const {
    return std::string{};
}

std::string VST3Plugin::getProgramNameIndexed(int index) const {
    return std::string{};
}

int VST3Plugin::getNumPrograms() const {
    return 0;
}

bool VST3Plugin::hasChunkData() const {
    return false;
}

void VST3Plugin::setProgramChunkData(const void *data, size_t size){
}

void VST3Plugin::getProgramChunkData(void **data, size_t *size) const {
}

void VST3Plugin::setBankChunkData(const void *data, size_t size){
}

void VST3Plugin::getBankChunkData(void **data, size_t *size) const {
}

void VST3Plugin::readProgramFile(const std::string& path){

}

void VST3Plugin::readProgramData(const char *data, size_t size){

}

void VST3Plugin::writeProgramFile(const std::string& path){

}

void VST3Plugin::writeProgramData(std::string& buffer){

}

void VST3Plugin::readBankFile(const std::string& path){

}

void VST3Plugin::readBankData(const char *data, size_t size){

}

void VST3Plugin::writeBankFile(const std::string& path){

}

void VST3Plugin::writeBankData(std::string& buffer){

}

bool VST3Plugin::hasEditor() const {
    return false;
}

void VST3Plugin::openEditor(void * window){

}

void VST3Plugin::closeEditor(){

}

void VST3Plugin::getEditorRect(int &left, int &top, int &right, int &bottom) const {

}

} // vst
