#include "VSTPluginUGen.h"
#include "Utility.h"

#ifdef SUPERNOVA
#include "spin_lock.hpp"
#endif

#include <limits>
#include <set>
#include <stdio.h>

static InterfaceTable *ft;

void SCLog(const std::string& msg){
	Print(msg.c_str());
}

static std::vector<std::string> userSearchPaths;
static std::vector<std::string> pluginList;
static VSTPluginMap pluginMap;
static bool isSearching = false;

// sending reply OSC messages
// we only need int, float and string

int doAddArg(char *buf, int size, int i) {
	return snprintf(buf, size, "%d\n", i);
}

int doAddArg(char *buf, int size, float f) {
	return snprintf(buf, size, "%f\n", f);
}

int doAddArg(char *buf, int size, const char *s) {
	return snprintf(buf, size, "%s\n", s);
}

int doAddArg(char *buf, int size, const std::string& s) {
	return snprintf(buf, size, "%s\n", s.c_str());
}

// end condition
int addArgs(char *buf, int size) { return 0; }

// recursively add arguments
template<typename Arg, typename... Args>
int addArgs(char *buf, int size, Arg&& arg, Args&&... args) {
	auto n = doAddArg(buf, size, std::forward<Arg>(arg));
	if (n >= 0 && n < size) {
		n += addArgs(buf + n, size - n, std::forward<Args>(args)...);
	}
	return n;
}

template<typename... Args>
int makeReply(char *buf, int size, const char *address, Args&&... args) {
	auto n = snprintf(buf, size, "%s\n", address);
	if (n >= 0 && n < size) {
		n += addArgs(buf + n, size - n, std::forward<Args>(args)...);
	}
	if (n > 0) {
		buf[n - 1] = 0; // remove trailing \n
	}
	return n;
}

#if 0
template<typename... Args>
void sendReply(World *world, void *replyAddr, const char *s, int size) {
	// "abuse" DoAsynchronousCommand to send a reply string
	// unfortunately, we can't send OSC messages directly, so I have to assemble a string
	// (with the arguments seperated by newlines) which is send as the argument for a /done message.
	// 'cmdName' isn't really copied, so we have to make sure it outlives stage 4.
	auto data = (char *)RTAlloc(world, size + 1);
	if (data) {
		LOG_DEBUG("send reply");
		memcpy(data, s, size);
		data[size] = 0;
		auto deleter = [](World *inWorld, void *cmdData) {
			LOG_DEBUG("delete reply");
			RTFree(inWorld, cmdData);
		};
		DoAsynchronousCommand(world, replyAddr, data, data, 0, 0, 0, deleter, 0, 0);
	}
	else {
		LOG_ERROR("RTAlloc failed!");
	}
}
#endif

	// format: size, chars...
int string2floatArray(const std::string& src, float *dest, int maxSize) {
	int len = std::min<int>(src.size(), maxSize-1);
	if (len >= 0) {
		*dest++ = len;
		for (int i = 0; i < len; ++i) {
			dest[i] = src[i];
		}
		return len + 1;
	}
	else {
		return 0;
	}
}

// VSTPluginListener

VSTPluginListener::VSTPluginListener(VSTPlugin &owner)
	: owner_(&owner) {}

struct ParamAutomatedData {
	VSTPlugin *owner;
	int32 index;
	float value;
};

// NOTE: in case we don't have a GUI thread we *could* get rid of nrtThreadID
// and just assume that std::this_thread::get_id() != rtThreadID_ means
// we're on the NRT thread - but I don't know if can be 100% sure about this,
// so let's play it safe.
void VSTPluginListener::parameterAutomated(int index, float value) {
	// RT thread
	if (std::this_thread::get_id() == owner_->rtThreadID_) {
#if 0
		// linked parameters automated by control busses or Ugens
		owner_->parameterAutomated(index, value);
#endif
	}
	// NRT thread
	else if (std::this_thread::get_id() == owner_->nrtThreadID_) {
		FifoMsg msg;
		auto data = new ParamAutomatedData{ owner_, index, value };
		msg.Set(owner_->mWorld, // world
			[](FifoMsg *msg) { // perform
				auto data = (ParamAutomatedData *)msg->mData;
				data->owner->parameterAutomated(data->index, data->value);
			}, [](FifoMsg *msg) { // free
				delete (ParamAutomatedData *)msg->mData;
			}, data); // data
		SendMsgToRT(owner_->mWorld, msg);
	}
#if VSTTHREADS
	// GUI thread (neither RT nor NRT thread) - push to queue
	else {
		std::lock_guard<std::mutex> guard(owner_->mutex_);
		owner_->paramQueue_.emplace_back(index, value);
	}
#endif
}

void VSTPluginListener::midiEvent(const VSTMidiEvent& midi) {
#if VSTTHREADS
	// check if we're on the realtime thread, otherwise ignore it
	if (std::this_thread::get_id() == owner_->rtThreadID_) {
#else
	{
#endif
		owner_->midiEvent(midi);
	}
}

void VSTPluginListener::sysexEvent(const VSTSysexEvent& sysex) {
#if VSTTHREADS
	// check if we're on the realtime thread, otherwise ignore it
	if (std::this_thread::get_id() == owner_->rtThreadID_) {
#else
	{
#endif
		owner_->sysexEvent(sysex);
	}
}

// VSTPlugin

VSTPlugin::VSTPlugin(){
	rtThreadID_ = std::this_thread::get_id();
	// LOG_DEBUG("RT thread ID: " << rtThreadID_);
	listener_ = std::make_unique<VSTPluginListener>(*this);

	numInChannels_ = in0(1);
	numOutChannels_ = numOutputs();
	parameterControlOnset_ = inChannelOnset_ + numInChannels_;
	numParameterControls_ = (int)(numInputs() - parameterControlOnset_) / 2;
	// LOG_DEBUG("num in: " << numInChannels_ << ", num out: " << numOutChannels_ << ", num controls: " << numParameterControls_);
    resizeBuffer();

	set_calc_function<VSTPlugin, &VSTPlugin::next>();
}

VSTPlugin::~VSTPlugin(){
	close();
	if (buf_) RTFree(mWorld, buf_);
	if (inBufVec_) RTFree(mWorld, inBufVec_);
	if (outBufVec_) RTFree(mWorld, outBufVec_);
	if (paramStates_) RTFree(mWorld, paramStates_);
	LOG_DEBUG("destroyed VSTPlugin");
}

IVSTPlugin *VSTPlugin::plugin() {
	return plugin_;
}

bool VSTPlugin::check(){
	if (plugin_) {
		return true;
	}
	else {
		LOG_WARNING("VSTPlugin: no plugin!");
		return false;
	}
}

bool VSTPlugin::valid() {
	if (magic_ == MagicNumber) {
		return true;
	}
	else {
		LOG_WARNING("VSTPlugin (" << mParent->mNode.mID << ", " << mParentIndex << ") not ready!");
		return false;
	}
}

void VSTPlugin::resizeBuffer(){
    int blockSize = bufferSize();
    int nin = numInChannels_;
    int nout = numOutChannels_;
	bool fail = false;
    if (plugin_){
        nin = std::max<int>(nin, plugin_->getNumInputs());
        nout = std::max<int>(nout, plugin_->getNumOutputs());
    }
	// buffer
	{
		int bufSize = (nin + nout) * blockSize * sizeof(float);
		auto result = (float *)RTRealloc(mWorld, buf_, bufSize);
		if (result) {
			buf_ = result;
			memset(buf_, 0, bufSize);
		}
		else {
			fail = true;
		}
	}
    // input buffer array
	{
		auto result = (const float **)RTRealloc(mWorld, inBufVec_, nin * sizeof(float *));
		if (result) {
			inBufVec_ = result;
			for (int i = 0; i < nin; ++i) {
				inBufVec_[i] = &buf_[i * blockSize];
			}
		}
		else {
			fail = true;
		}
	}
    // output buffer array
	{
		auto result = (float **)RTRealloc(mWorld, outBufVec_, nout * sizeof(float *));
		if (result) {
			outBufVec_ = result;
			for (int i = 0; i < nout; ++i) {
				outBufVec_[i] = &buf_[(i + nin) * blockSize];
			}
		}
		else {
			fail = true;
		}
	}
	if (fail) {
		LOG_ERROR("RTRealloc failed!");
		RTFree(mWorld, buf_);
		RTFree(mWorld, inBufVec_);
		RTFree(mWorld, outBufVec_);
		buf_ = nullptr; inBufVec_ = nullptr; outBufVec_ = nullptr;
	}
}

bool cmdClose(World *world, void* cmdData) {
	((VSTPluginCmdData *)cmdData)->close();
	return true;
}

	// try to close the plugin in the NRT thread with an asynchronous command
void VSTPlugin::close() {
	if (plugin_) {
		auto cmdData = makeCmdData();
		if (!cmdData) {
			return;
		}
		// plugin, window and thread don't depend on VSTPlugin so they can be
		// safely moved to cmdData (which takes care of the actual closing)
		cmdData->plugin = plugin_;
		cmdData->window = window_;
#if VSTTHREADS
		cmdData->thread = std::move(thread_);
#endif
		doCmd(cmdData, cmdClose);
		window_ = nullptr;
		plugin_ = nullptr;
	}
}

void VSTPluginCmdData::close() {
	if (!plugin) return;
#if VSTTHREADS
	if (window) {
		// terminate the message loop (will implicitly release the plugin)
		window->quit();
		// now join the thread
		if (thread.joinable()) {
			thread.join();
			LOG_DEBUG("thread joined");
		}
		// then destroy the window
		window = nullptr;
	}
	else {
#else
	{
#endif
		// first destroy the window (if any)
		window = nullptr;
		// then release the plugin
		freeVSTPlugin(plugin);
	}
	LOG_DEBUG("VST plugin closed");
}

bool cmdOpen(World *world, void* cmdData) {
	LOG_DEBUG("cmdOpen");
	// initialize GUI backend (if needed)
	auto data = (VSTPluginCmdData *)cmdData;
	data->threadID = std::this_thread::get_id();
	if (data->value) { // VST gui?
#ifdef __APPLE__
		LOG_WARNING("Warning: VST GUI not supported (yet) on macOS!");
		data->value = false;
#else
		static bool initialized = false;
		if (!initialized) {
			VSTWindowFactory::initialize();
			initialized = true;
		}
#endif
	}
	data->tryOpen();
	auto plugin = data->plugin;
	if (plugin) {
		auto owner = data->owner;
		plugin->suspend();
		// we only access immutable members of owner
		plugin->setSampleRate(owner->sampleRate());
		plugin->setBlockSize(owner->bufferSize());
		if (plugin->hasPrecision(VSTProcessPrecision::Single)) {
			plugin->setPrecision(VSTProcessPrecision::Single);
		}
		else {
			LOG_WARNING("VSTPlugin: plugin '" << plugin->getPluginName() << "' doesn't support single precision processing - bypassing!");
		}
		int nin = std::min<int>(plugin->getNumInputs(), owner->numInChannels());
		int nout = std::min<int>(plugin->getNumOutputs(), owner->numOutChannels());
		plugin->setNumSpeakers(nin, nout);
		plugin->resume();
	}
	return true;
}

bool cmdOpenDone(World *world, void* cmdData) {
	auto data = (VSTPluginCmdData *)cmdData;
	data->owner->doneOpen(*data);
	return true;
}

	// try to open the plugin in the NRT thread with an asynchronous command
void VSTPlugin::open(const char *path, bool gui) {
	LOG_DEBUG("open");
	if (isLoading_) {
		LOG_WARNING("already loading!");
		return;
	}
	close();
	if (plugin_) {
		LOG_ERROR("couldn't close current plugin!");
		return;
	}
	
    auto cmdData = makeCmdData(path);
	if (cmdData) {
		cmdData->value = gui;
		doCmd(cmdData, cmdOpen, cmdOpenDone);
		isLoading_ = true;
    }
}

void VSTPlugin::doneOpen(VSTPluginCmdData& cmd){
	LOG_DEBUG("doneOpen");
	isLoading_ = false;
	plugin_ = cmd.plugin;
	window_ = cmd.window;
	nrtThreadID_ = cmd.threadID;
	// LOG_DEBUG("NRT thread ID: " << nrtThreadID_);
#if VSTTHREADS
	thread_ = std::move(cmd.thread);
#endif
    if (plugin_){
		LOG_DEBUG("loaded " << cmd.buf);
		plugin_->setListener(listener_.get());
        resizeBuffer();
		// allocate arrays for parameter values/states
		int nParams = plugin_->getNumParameters();
		auto result = (Param *)RTRealloc(mWorld, paramStates_, nParams * sizeof(Param));
        if (result){
			paramStates_ = result;
            for (int i = 0; i < nParams; ++i) {
				paramStates_[i].value = std::numeric_limits<float>::quiet_NaN();
                paramStates_[i].bus = -1;
            }
        } else {
			RTFree(mWorld, paramStates_);
			paramStates_ = nullptr;
            LOG_ERROR("RTRealloc failed!");
        }
		// success, window
		float data[] = { 1.f, static_cast<float>(window_ != nullptr) };
		sendMsg("/vst_open", 2, data);
	} else {
		LOG_WARNING("VSTPlugin: couldn't load " << cmd.buf);
		sendMsg("/vst_open", 0);
	}
}

#if VSTTHREADS
using VSTPluginCmdPromise = std::promise<std::pair<IVSTPlugin *, std::shared_ptr<IVSTWindow>>>;
void threadFunction(VSTPluginCmdPromise promise, const char *path);
#endif

void VSTPluginCmdData::tryOpen(){
#if VSTTHREADS
        // creates a new thread where the plugin is created and the message loop runs
    if (value){ // VST gui?
		VSTPluginCmdPromise promise;
        auto future = promise.get_future();
		LOG_DEBUG("started thread");
        thread = std::thread(threadFunction, std::move(promise), buf);
			// wait for thread to return the plugin and window
        auto result = future.get();
		LOG_DEBUG("got result from thread");
		plugin = result.first;
		window = result.second;
		if (!window) {
			thread.join(); // to avoid a crash in ~VSTPluginCmdData
		}
		return;
    }
#endif
        // create plugin in main thread
    plugin = loadVSTPlugin(buf);
#if !VSTTHREADS
        // create and setup GUI window in main thread (if needed)
    if (plugin && plugin->hasEditor() && value){
        window = std::shared_ptr<IVSTWindow>(VSTWindowFactory::create(plugin));
        if (window){
			window->setTitle(plugin->getPluginName());
            int left, top, right, bottom;
            plugin->getEditorRect(left, top, right, bottom);
			window->setGeometry(left, top, right, bottom);
            // don't open the editor on macOS (see VSTWindowCocoa.mm)
#ifndef __APPLE__
            plugin->openEditor(window->getHandle());
#endif
        }
    }
#endif
}

#if VSTTHREADS
void threadFunction(VSTPluginCmdPromise promise, const char *path){
    IVSTPlugin *plugin = loadVSTPlugin(path);
    if (!plugin){
            // signal other thread
		promise.set_value({ nullptr, nullptr });
        return;
    }
        // create GUI window (if needed)
	std::shared_ptr<IVSTWindow> window;
    if (plugin->hasEditor()){
        window = std::shared_ptr<IVSTWindow>(VSTWindowFactory::create(plugin));
    }
        // return plugin and window to other thread
	promise.set_value({ plugin, window });
        // setup GUI window (if any)
    if (window){
        window->setTitle(plugin->getPluginName());
        int left, top, right, bottom;
        plugin->getEditorRect(left, top, right, bottom);
        window->setGeometry(left, top, right, bottom);

        plugin->openEditor(window->getHandle());
            // run the event loop until it gets a quit message 
			// (the editor will we closed implicitly)
		LOG_DEBUG("start message loop");
		window->run();
		LOG_DEBUG("end message loop");
            // some plugins expect to released in the same thread where they have been created.
        freeVSTPlugin(plugin);
    }
}
#endif

bool cmdShowEditor(World *world, void *cmdData) {
	auto data = (VSTPluginCmdData *)cmdData;
	if (data->value) {
		data->window->bringToTop();
	}
	else {
		data->window->hide();
	}
	return true;
}

void VSTPlugin::showEditor(bool show) {
	if (plugin_ && window_) {
        auto cmdData = makeCmdData();
		if (cmdData) {
			cmdData->window = window_;
			cmdData->value = show;
			doCmd(cmdData, cmdShowEditor);
		}
	}
}

// some plugins crash when being reset reset
// in the NRT thread. we let the user choose
// and add a big fat warning in the help file.
bool cmdReset(World *world, void *cmdData) {
	auto data = (VSTPluginCmdData *)cmdData;
	data->owner->plugin()->suspend();
	data->owner->plugin()->resume();
	return true;
}

void VSTPlugin::reset(bool async) {
	if (check()) {
		if (async) {
			// reset in the NRT thread (unsafe)
			doCmd(makeCmdData(), cmdReset);
		}
		else {
			// reset in the RT thread (safe)
			plugin_->suspend();
			plugin_->resume();
		}
	}
}

// perform routine
void VSTPlugin::next(int inNumSamples) {
    if (!(buf_ && inBufVec_ && outBufVec_)) return;
    int nin = numInChannels_;
    int nout = numOutChannels_;
    bool bypass = in0(0);
	int offset = 0;
	// disable reset for realtime-safety reasons
#if 0
    // only reset plugin when bypass changed from true to false
    if (plugin_ && !bypass && (bypass != bypass_)) {
        reset();
    }
    bypass_ = bypass;
#endif
	// setup pointer arrays:
	for (int i = 0; i < nin; ++i) {
		inBufVec_[i] = in(i + inChannelOnset_);
	}
	for (int i = 0; i < nout; ++i) {
		outBufVec_[i] = out(i);
	}

	if (plugin_ && !bypass && plugin_->hasPrecision(VSTProcessPrecision::Single)) {
		if (paramStates_) {
			// update parameters from mapped control busses
			int nparam = plugin_->getNumParameters();
			for (int i = 0; i < nparam; ++i) {
				int bus = paramStates_[i].bus;
				if (bus >= 0) {
					float value = readControlBus(bus);
					if (value != paramStates_[i].value) {
						plugin_->setParameter(i, value);
						paramStates_[i].value = value;
					}
				}
			}
			// update parameters from UGen inputs
			for (int i = 0; i < numParameterControls_; ++i) {
				int k = 2 * i + parameterControlOnset_;
				int index = in0(k);
				float value = in0(k + 1);
				// only if index is not out of range and the param is not mapped to a bus
				if (index >= 0 && index < nparam && paramStates_[index].bus < 0
					&& paramStates_[index].value != value)
				{
					plugin_->setParameter(index, value);
					paramStates_[index].value = value;
				}
			}
		}
        // process
		plugin_->process((const float **)inBufVec_, outBufVec_, inNumSamples);
		offset = plugin_->getNumOutputs();

#if VSTTHREADS
		// send parameter automation notification posted from the GUI thread.
		// we assume this is only possible if we have a VST editor window.
		// try_lock() won't block the audio thread and we don't mind if notifications
		// will be delayed if try_lock() fails (which happens rarely in practice).
		if (window_ && mutex_.try_lock()) {
			std::vector<std::pair<int, float>> queue;
			queue.swap(paramQueue_);
			mutex_.unlock();
			for (auto& p : queue) {
				parameterAutomated(p.first, p.second);
			}
		}
#endif
	}
    else {
        // bypass (copy input to output)
        int n = std::min(nin, nout);
		for (int i = 0; i < n; ++i) {
			Copy(inNumSamples, outBufVec_[i], (float *)inBufVec_[i]);
		}
		offset = n;
    }
	// zero remaining outlets
	for (int i = offset; i < nout; ++i) {
		Fill(inNumSamples, outBufVec_[i], 0.f);
	}
}

bool cmdSetParam(World *world, void *cmdData) {
	auto data = (ParamCmdData *)cmdData;
	auto index = data->index;
	if (data->display[0]) {
		data->owner->plugin()->setParameter(index, data->display);
	}
	else {
		data->owner->plugin()->setParameter(index, data->value);
	}
	return true;
}

bool cmdSetParamDone(World *world, void *cmdData) {
	auto data = (ParamCmdData *)cmdData;
	data->owner->setParamDone(data->index);
	return true;
}

void VSTPlugin::setParam(int32 index, float value) {
	if (check()) {
		if (index >= 0 && index < plugin_->getNumParameters()) {
			auto data = (ParamCmdData *)RTAlloc(mWorld, sizeof(ParamCmdData));
			if (data) {
				data->owner = this;
				data->index = index;
				data->value = value;
				data->display[0] = 0;
				doCmd(data, cmdSetParam, cmdSetParamDone);
			}
			else {
				LOG_ERROR("RTAlloc failed!");
			}
		}
		else {
			LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
		}
	}
}

void VSTPlugin::setParam(int32 index, const char *display) {
	if (check()) {
		if (index >= 0 && index < plugin_->getNumParameters()) {
			auto len = strlen(display) + 1;
			auto data = (ParamCmdData *)RTAlloc(mWorld, sizeof(ParamCmdData) + len);
			if (data) {
				data->owner = this;
				data->index = index;
				data->value = 0;
				memcpy(data->display, display, len);
				doCmd(data, cmdSetParam, cmdSetParamDone);
			}
			else {
				LOG_ERROR("RTAlloc failed!");
			}
		}
		else {
			LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
		}
	}
}

void VSTPlugin::setParamDone(int32 index) {
	paramStates_[index].value = plugin_->getParameter(index);
	paramStates_[index].bus = -1; // invalidate bus num
	sendParameter(index);
}

void VSTPlugin::queryParams(int32 index, int32 count) {
	if (check()) {
		int32 nparam = plugin_->getNumParameters();
		if (index >= 0 && index < nparam) {
			count = std::min<int32>(count, nparam - index);
			for (int i = 0; i < count; ++i) {
				sendParameter(index + i);
			}
		}
		else {
			LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
		}
	}
}

void VSTPlugin::getParam(int32 index) {
	if (check()) {
		if (index >= 0 && index < plugin_->getNumParameters()) {
			float value = plugin_->getParameter(index);
			sendMsg("/vst_set", value);
		}
		else {
			LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
		}
	}
}

void VSTPlugin::getParams(int32 index, int32 count) {
	if (check()) {
		int32 nparam = plugin_->getNumParameters();
		if (index >= 0 && index < nparam) {
			count = std::min<int32>(count, nparam - index);
			const int bufsize = count + 1;
			float *buf = (float *)RTAlloc(mWorld, sizeof(float) * bufsize);
			if (buf) {
				buf[0] = count;
				for (int i = 0; i < count; ++i) {
					float value = plugin_->getParameter(i + index);
					buf[i + 1] = value;
				}
				sendMsg("/vst_setn", bufsize, buf);
				RTFree(mWorld, buf);
			}
			else {
				LOG_WARNING("RTAlloc failed!");
			}
		}
		else {
			LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
		}
	}
}

void VSTPlugin::mapParam(int32 index, int32 bus) {
	if (check()) {
		if (index >= 0 && index < plugin_->getNumParameters()) {
			paramStates_[index].bus = bus;
		}
		else {
			LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
		}
	}
}

void VSTPlugin::unmapParam(int32 index) {
	if (check()) {
		if (index >= 0 && index < plugin_->getNumParameters()) {
			paramStates_[index].bus = -1;
		}
		else {
			LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
		}
	}
}

// program/bank
bool cmdSetProgram(World *world, void *cmdData) {
	auto data = (VSTPluginCmdData *)cmdData;
	data->owner->plugin()->setProgram(data->value);
	return true;
}

bool cmdSetProgramDone(World *world, void *cmdData) {
	auto data = (VSTPluginCmdData *)cmdData;
	data->owner->sendMsg("/vst_program_index", data->owner->plugin()->getProgram());
	return true;
}

void VSTPlugin::setProgram(int32 index) {
	if (check()) {
		if (index >= 0 && index < plugin_->getNumPrograms()) {
			auto data = makeCmdData();
			if (data) {
				data->value = index;
				doCmd(data, cmdSetProgram, cmdSetProgramDone);
			}
			else {
				LOG_ERROR("RTAlloc failed!");
			}
		}
		else {
			LOG_WARNING("VSTPlugin: program number " << index << " out of range!");
		}
	}
}
void VSTPlugin::setProgramName(const char *name) {
	if (check()) {
		plugin_->setProgramName(name);
		sendCurrentProgramName();
	}
}

void VSTPlugin::queryPrograms(int32 index, int32 count) {
	if (check()) {
		int32 nprogram = plugin_->getNumPrograms();
		if (index >= 0 && index < nprogram) {
			count = std::min<int32>(count, nprogram - index);
#if 1
            for (int i = 0; i < count; ++i) {
                sendProgramName(index + i);
            }
#else
			auto old = plugin_->getProgram();
			bool changed;
			for (int i = 0; i < count; ++i) {
				changed = sendProgramName(index + i);
			}
			if (changed) {
				plugin_->setProgram(old);
			}
#endif
		}
		else {
			LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
		}
	}
}

bool cmdSetProgramData(World *world, void *cmdData) {
	auto data = (VSTPluginCmdData *)cmdData;
	data->value = data->owner->plugin()->readProgramData(data->buf, data->size);
	return true;
}

bool cmdSetBankData(World *world, void *cmdData) {
	auto data = (VSTPluginCmdData *)cmdData;
	data->value = data->owner->plugin()->readBankData(data->buf, data->size);
	return true;
}

bool cmdReadProgram(World *world, void *cmdData){
	auto data = (VSTPluginCmdData *)cmdData;
	data->value = data->owner->plugin()->readProgramFile(data->buf);
	return true;
}

bool cmdReadBank(World *world, void *cmdData){
	auto data = (VSTPluginCmdData *)cmdData;
	data->value = data->owner->plugin()->readBankFile(data->buf);
	return true;
}

bool cmdProgramDone(World *world, void *cmdData){
    auto data = (VSTPluginCmdData *)cmdData;
	data->owner->sendMsg("/vst_program_read", data->value);
	data->owner->sendCurrentProgramName();
    return true;
}

bool cmdBankDone(World *world, void *cmdData){
	auto data = (VSTPluginCmdData *)cmdData;
	data->owner->sendMsg("/vst_bank_read", data->value);
	data->owner->sendMsg("/vst_program_index", data->owner->plugin()->getProgram());
	return true;
}

void VSTPlugin::readProgram(const char *path){
    if (check()){
		doCmd(makeCmdData(path), cmdReadProgram, cmdProgramDone);
    }
}

void VSTPlugin::readBank(const char *path) {
	if (check()) {
		doCmd(makeCmdData(path), cmdReadBank, cmdBankDone);
	}
}

void VSTPlugin::sendData(int32 totalSize, int32 onset, const char *data, int32 n, bool bank) {
	LOG_DEBUG("got packet: " << totalSize << " (total size), " << onset << " (onset), " << n << " (size)");
	// first packet only
	if (onset == 0) {
		if (dataReceived_ != 0) {
			LOG_WARNING("last data hasn't been sent completely!");
		}
		dataReceived_ = 0;
		auto result = RTRealloc(mWorld, dataRT_, totalSize);
		if (result) {
			dataRT_ = (char *)result;
			dataSize_ = totalSize;
		}
		else {
			dataSize_ = 0;
			return;
		}
	}
	else if (onset < 0 || onset >= dataSize_) {
		LOG_ERROR("bug: bad onset!");
		return;
	}
	// append data
	auto size = dataSize_;
	if (size > 0) {
		if (n > (size - onset)) {
			LOG_ERROR("bug: data exceeding total size!");
			n = size - onset;
		}
		memcpy(dataRT_ + onset, data, n);
		if (onset != dataReceived_) {
			LOG_WARNING("onset and received data out of sync!");
		}
		dataReceived_ += n;
		LOG_DEBUG("data received: " << dataReceived_);
		// finished?
		if (dataReceived_ >= size) {
			if (bank) {
				doCmd(makeCmdData(dataRT_, size), cmdSetBankData, cmdBankDone);
			}
			else {
				doCmd(makeCmdData(dataRT_, size), cmdSetProgramData, cmdProgramDone);
			}
			dataReceived_ = 0;
		}
	}
}

bool cmdWriteProgram(World *world, void *cmdData){
	auto data = (VSTPluginCmdData *)cmdData;
	data->owner->plugin()->writeProgramFile(data->buf);
	return true;
}

bool cmdWriteBank(World *world, void *cmdData){
	auto data = (VSTPluginCmdData *)cmdData;
	data->owner->plugin()->writeBankFile(data->buf);
	return true;
}

bool cmdWriteProgramDone(World *world, void *cmdData){
    auto data = (VSTPluginCmdData *)cmdData;
	data->owner->sendMsg("/vst_program_write", 1); // LATER get real return value
    return true;
}

bool cmdWriteBankDone(World *world, void *cmdData) {
	auto data = (VSTPluginCmdData *)cmdData;
	data->owner->sendMsg("/vst_bank_write", 1); // LATER get real return value
	return true;
}

void VSTPlugin::writeProgram(const char *path) {
    if (check()) {
		doCmd(makeCmdData(path), cmdWriteProgram, cmdWriteProgramDone);
	}
}

void VSTPlugin::writeBank(const char *path) {
	if (check()) {
		doCmd(makeCmdData(path), cmdWriteBank, cmdWriteBankDone);
	}
}

bool VSTPlugin::cmdGetData(World *world, void *cmdData, bool bank) {
	auto data = (VSTPluginCmdData *)cmdData;
	auto owner = data->owner;
	auto& buffer = owner->dataNRT_;
	int count = data->value;
	if (count == 0) {
		// write whole program/bank data into buffer
		if (bank) {
			owner->plugin()->writeBankData(buffer);
		}
		else {
			owner->plugin()->writeProgramData(buffer);
		}
		owner->dataSent_ = 0;
		LOG_DEBUG("total data size: " << buffer.size());
	}
	// data left to send?
	auto onset = owner->dataSent_;
	auto remaining = buffer.size() - onset;
	if (remaining > 0) {
		// we want to send floats (but size is the number of bytes)
		int maxArgs = data->size / sizeof(float);
		// leave space for 3 extra arguments
		auto size = std::min<int>(remaining, maxArgs - 3);
		auto buf = (float *)data->buf;
		buf[0] = buffer.size(); // total
		buf[1] = onset; // onset
		buf[2] = size; // packet size
		// copy packet to cmdData's RT buffer
		auto packet = buffer.data() + onset;
		for (int i = 0; i < size; ++i) {
			// no need to cast to unsigned because SC's Int8Array is signed anyway
			buf[i + 3] = packet[i];
		}
		data->size = size + 3; // size_ becomes the number of float args
		owner->dataSent_ += size;
		LOG_DEBUG("send packet: " << buf[0] << " (total), " << buf[1] << " (onset), " << buf[2] << " (size)");
	}
	else {
		// avoid sending packet
		data->size = 0;
		// free program/bank data
		std::string empty;
		buffer.swap(empty);
		data->owner->dataSent_ = 0;
		LOG_DEBUG("done! free data");
	}
	return true;
}

bool VSTPlugin::cmdGetDataDone(World *world, void *cmdData, bool bank) {
	auto data = (VSTPluginCmdData *)cmdData;
	if (data->size > 0) {
		if (bank) {
			data->owner->sendMsg("/vst_bank_data", data->size, (const float *)data->buf);
		}
		else {
			data->owner->sendMsg("/vst_program_data", data->size, (const float *)data->buf);
		}
	}
	return true;
}

void VSTPlugin::receiveProgramData(int count) {
	if (check()) {
		auto data = makeCmdData(MAX_OSC_PACKET_SIZE);
		if (data) {
			data->value = count;
			doCmd(data, [](World *world, void *cmdData) { return cmdGetData(world, cmdData, false); }, 
				[](World *world, void *cmdData) { return cmdGetDataDone(world, cmdData, false); });
		}
	}
}

void VSTPlugin::receiveBankData(int count) {
	if (check()) {
		auto data = makeCmdData(MAX_OSC_PACKET_SIZE);
		if (data) {
			data->value = count;
			doCmd(data, [](World *world, void *cmdData) { return cmdGetData(world, cmdData, true); },
				[](World *world, void *cmdData) { return cmdGetDataDone(world, cmdData, true); });
		}
	}
}

// midi
void VSTPlugin::sendMidiMsg(int32 status, int32 data1, int32 data2) {
	if (check()) {
		plugin_->sendMidiEvent(VSTMidiEvent(status, data1, data2));
	}
}
void VSTPlugin::sendSysexMsg(const char *data, int32 n) {
	if (check()) {
		plugin_->sendSysexEvent(VSTSysexEvent(data, n));
	}
}
// transport
void VSTPlugin::setTempo(float bpm) {
	if (check()) {
		plugin_->setTempoBPM(bpm);
	}
}
void VSTPlugin::setTimeSig(int32 num, int32 denom) {
	if (check()) {
		plugin_->setTimeSignature(num, denom);
	}
}
void VSTPlugin::setTransportPlaying(bool play) {
	if (check()) {
		plugin_->setTransportPlaying(play);
	}
}
void VSTPlugin::setTransportPos(float pos) {
	if (check()) {
		plugin_->setTransportPosition(pos);
	}
}
void VSTPlugin::getTransportPos() {
	if (check()) {
		float f = plugin_->getTransportPosition();
		sendMsg("/vst_transport", f);
	}
}

// advanced

void VSTPlugin::canDo(const char *what) {
	if (check()) {
		auto result = plugin_->canDo(what);
		sendMsg("/vst_can_do", (float)result);
	}
}

void VSTPlugin::vendorSpecific(int32 index, int32 value, void *ptr, float opt) {
	if (check()) {
        auto result = plugin_->vendorSpecific(index, value, ptr, opt);
		sendMsg("/vst_vendor_method", (float)result);
	}
}

/*** helper methods ***/

float VSTPlugin::readControlBus(int32 num) {
    if (num >= 0 && num < mWorld->mNumControlBusChannels) {
#define unit this
		ACQUIRE_BUS_CONTROL(num);
		float value = mWorld->mControlBus[num];
		RELEASE_BUS_CONTROL(num);
		return value;
#undef unit
	}
	else {
		return 0.f;
	}
}

// unchecked
bool VSTPlugin::sendProgramName(int32 num) {
    const int maxSize = 64;
    float buf[maxSize];
	bool changed = false;
	auto name = plugin_->getProgramNameIndexed(num);
#if 0
	// some old plugins don't support indexed program name lookup
	if (name.empty()) {
		plugin_->setProgram(num);
		name = plugin_->getProgramName();
		changed = true;
	}
#endif
	// msg format: index, len, characters...
	buf[0] = num;
    int size = string2floatArray(name, buf + 1, maxSize - 1);
	sendMsg("/vst_program", size + 1, buf);
	return changed;
}

void VSTPlugin::sendCurrentProgramName() {
	const int maxSize = 64;
	float buf[maxSize];
	// msg format: index, len, characters...
	buf[0] = plugin_->getProgram();
	int size = string2floatArray(plugin_->getProgramName(), buf + 1, maxSize - 1);
	sendMsg("/vst_program", size + 1, buf);
}

// unchecked
void VSTPlugin::sendParameter(int32 index) {
	const int maxSize = 64;
	float buf[maxSize];
	// msg format: index, value, display length, display chars...
	buf[0] = index;
	buf[1] = plugin_->getParameter(index);
	int size = string2floatArray(plugin_->getParameterDisplay(index), buf + 2, maxSize - 2);
	sendMsg("/vst_param", size + 2, buf);
}

void VSTPlugin::parameterAutomated(int32 index, float value) {
	sendParameter(index);
	float buf[2] = { (float)index, value };
	sendMsg("/vst_auto", 2, buf);
}

void VSTPlugin::midiEvent(const VSTMidiEvent& midi) {
	float buf[3];
	// we don't want negative values here
	buf[0] = (unsigned char) midi.data[0];
	buf[1] = (unsigned char) midi.data[1];
	buf[2] = (unsigned char) midi.data[2];
	sendMsg("/vst_midi", 3, buf);
}

void VSTPlugin::sysexEvent(const VSTSysexEvent& sysex) {
	auto& data = sysex.data;
	int size = data.size();
	if ((size * sizeof(float)) > MAX_OSC_PACKET_SIZE) {
		LOG_WARNING("sysex message (" << size << " bytes) too large for UDP packet - dropped!");
		return;
	}
	float *buf = (float *)RTAlloc(mWorld, size * sizeof(float));
	if (buf) {
		for (int i = 0; i < size; ++i) {
			// no need to cast to unsigned because SC's Int8Array is signed anyway
			buf[i] = data[i];
		}
		sendMsg("/vst_sysex", size, buf);
		RTFree(mWorld, buf);
	}
	else {
		LOG_WARNING("RTAlloc failed!");
	}
}

void VSTPlugin::sendMsg(const char *cmd, float f) {
	// LOG_DEBUG("sending msg: " << cmd);
	SendNodeReply(&mParent->mNode, mParentIndex, cmd, 1, &f);
}

void VSTPlugin::sendMsg(const char *cmd, int n, const float *data) {
	// LOG_DEBUG("sending msg: " << cmd);
	SendNodeReply(&mParent->mNode, mParentIndex, cmd, n, data);
}

VSTPluginCmdData* VSTPlugin::makeCmdData(const char *data, size_t size){
    VSTPluginCmdData *cmdData = (VSTPluginCmdData *)RTAlloc(mWorld, sizeof(VSTPluginCmdData) + size);
    if (cmdData){
        new (cmdData) VSTPluginCmdData();
        cmdData->owner = this;
        if (data){
            memcpy(cmdData->buf, data, size);
        }
		cmdData->size = size; // independent from data
	}
	else {
		LOG_ERROR("RTAlloc failed!");
	}
	return cmdData;
}

VSTPluginCmdData* VSTPlugin::makeCmdData(const char *path){
    size_t len = path ? (strlen(path) + 1) : 0;
    return makeCmdData(path, len);
}

VSTPluginCmdData* VSTPlugin::makeCmdData(size_t size) {
	return makeCmdData(nullptr, size);
}

VSTPluginCmdData* VSTPlugin::makeCmdData() {
	return makeCmdData(nullptr, 0);
}

void cmdRTfree(World *world, void* cmdData) {
	RTFree(world, cmdData);
	// LOG_DEBUG("cmdRTfree!");
}
template<typename T>
bool cmdNRTfree(World *world, void* cmdData) {
	((T*)cmdData)->~T();
	// LOG_DEBUG("cmdNRTfree!");
	return true;
}
template<typename T>
void VSTPlugin::doCmd(T *cmdData, AsyncStageFn nrt, AsyncStageFn rt) {
	// so we don't have to always check the return value of makeCmdData
	if (cmdData) {
		DoAsynchronousCommand(mWorld, 0, 0, cmdData, nrt, rt, cmdNRTfree<T>, cmdRTfree, 0, 0);
	}
}

/*** unit command callbacks ***/

#define CHECK_UNIT {if (!CAST_UNIT->valid()) return; }
#define CAST_UNIT (static_cast<VSTPlugin*>(unit))

void vst_open(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	const char *path = args->gets();
	auto gui = args->geti();
	if (path) {
        CAST_UNIT->open(path, gui);
	}
	else {
		LOG_WARNING("vst_open: expecting string argument!");
	}
}

void vst_close(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	CAST_UNIT->close();
}

void vst_reset(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	bool async = args->geti();
	CAST_UNIT->reset(async);
}

void vst_vis(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	bool show = args->geti();
	CAST_UNIT->showEditor(show);
}

// set parameters given as pairs of index and value
void vst_set(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	auto vst = CAST_UNIT;
	if (vst->check()) {
		while (args->remain() > 0) {
			int32 index = args->geti();
			if (args->remain() > 0 && args->nextTag() == 's') {
				vst->setParam(index, args->gets());
			}
			else {
				vst->setParam(index, args->getf());
			}
		}
	}
}

// set parameters given as triples of index, count and values
void vst_setn(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	auto vst = CAST_UNIT;
	if (vst->check()) {
		int nparam = vst->plugin()->getNumParameters();
		while (args->remain() > 0) {
			int32 index = args->geti();
			int32 count = args->geti();
			for (int i = 0; i < count && args->remain() > 0; ++i) {
				if (args->nextTag() == 's') {
					vst->setParam(index + i, args->gets());
				}
				else {
					vst->setParam(index + i, args->getf());
				}
			}
		}
	}
}

// query parameters starting from index (values + displays)
void vst_param_query(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	int32 index = args->geti();
	int32 count = args->geti();
	CAST_UNIT->queryParams(index, count);
}

// get a single parameter at index (only value)
void vst_get(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	int32 index = args->geti(-1);
	CAST_UNIT->getParam(index);
}

// get a number of parameters starting from index (only values)
void vst_getn(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	int32 index = args->geti();
	int32 count = args->geti();
	CAST_UNIT->getParams(index, count);
}

// map parameters to control busses
void vst_map(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	auto vst = CAST_UNIT;
	if (vst->check()) {
		int nparam = vst->plugin()->getNumParameters();
		while (args->remain() > 0) {
			int32 index = args->geti();
			int32 bus = args->geti(-1);
			int32 numChannels = args->geti();
			for (int i = 0; i < numChannels; ++i) {
				int32 idx = index + i;
				if (idx >= 0 && idx < nparam) {
					vst->mapParam(idx, bus + i);
				}
			}
		}
	}
}

// unmap parameters from control busses
void vst_unmap(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	auto vst = CAST_UNIT;
	if (vst->check()) {
		int nparam = vst->plugin()->getNumParameters();
		if (args->remain() > 0) {
			do {
				int32 index = args->geti();
				if (index >= 0 && index < nparam) {
					vst->unmapParam(index);
				}
			} while (args->remain() > 0);
		}
		else {
			// unmap all parameters:
			for (int i = 0; i < nparam; ++i) {
				vst->unmapParam(i);
			}
		}
		
	}
}

void vst_program_set(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	int32 index = args->geti();
	CAST_UNIT->setProgram(index);
}

// query parameters (values + displays) starting from index
void vst_program_query(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	int32 index = args->geti();
	int32 count = args->geti();
	CAST_UNIT->queryPrograms(index, count);
}

void vst_program_name(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	const char *name = args->gets();
	if (name) {
		CAST_UNIT->setProgramName(name);
	}
	else {
		LOG_WARNING("vst_program_name: expecting string argument!");
	}
}

void vst_program_read(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	const char *path = args->gets();
	if (path) {
		CAST_UNIT->readProgram(path);
	}
	else {
		LOG_WARNING("vst_program_read: expecting string argument!");
	}
}

void vst_program_write(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	const char *path = args->gets();
	if (path) {
		CAST_UNIT->writeProgram(path);
	}
	else {
		LOG_WARNING("vst_program_write: expecting string argument!");
	}
}

void vst_program_data_set(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	int totalSize = args->geti();
	int onset = args->geti();
	int len = args->getbsize();
	if (len > 0) {
		// LATER avoid unnecessary copying
		char *buf = (char *)RTAlloc(unit->mWorld, len);
		if (buf) {
			args->getb(buf, len);
			CAST_UNIT->sendProgramData(totalSize, onset, buf, len);
			RTFree(unit->mWorld, buf);
		}
		else {
			LOG_ERROR("vst_program_data_set: RTAlloc failed!");
		}
	}
	else {
		LOG_WARNING("vst_program_data_set: no data!");
	}
}

void vst_program_data_get(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	int count = args->geti();
	CAST_UNIT->receiveProgramData(count);
}

void vst_bank_read(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	const char *path = args->gets();
	if (path) {
		CAST_UNIT->readBank(path);
	}
	else {
		LOG_WARNING("vst_bank_read: expecting string argument!");
	}
}

void vst_bank_write(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	const char *path = args->gets();
	if (path) {
		CAST_UNIT->writeBank(path);
	}
	else {
		LOG_WARNING("vst_bank_write: expecting string argument!");
	}
}

void vst_bank_data_set(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	int totalSize = args->geti();
	int onset = args->geti();
	int len = args->getbsize();
	if (len > 0) {
		// LATER avoid unnecessary copying
		char *buf = (char *)RTAlloc(unit->mWorld, len);
		if (buf) {
			args->getb(buf, len);
			CAST_UNIT->sendBankData(totalSize, onset, buf, len);
			RTFree(unit->mWorld, buf);
		}
		else {
			LOG_ERROR("vst_bank_data_set: RTAlloc failed!");
		}
	}
	else {
		LOG_WARNING("vst_bank_data_set: no data!");
	}
}

void vst_bank_data_get(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	int count = args->geti();
	CAST_UNIT->receiveBankData(count);
}


void vst_midi_msg(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	char data[4];
	int32 len = args->getbsize();
	if (len > 4) {
		LOG_WARNING("vst_midi_msg: midi message too long (" << len << " bytes)");
	}
	args->getb(data, len);
	CAST_UNIT->sendMidiMsg(data[0], data[1], data[2]);
}

void vst_midi_sysex(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	int len = args->getbsize();
	if (len > 0) {
		// LATER avoid unnecessary copying
		char *buf = (char *)RTAlloc(unit->mWorld, len);
		if (buf) {
			args->getb(buf, len);
			CAST_UNIT->sendSysexMsg(buf, len);
			RTFree(unit->mWorld, buf);
		}
		else {
			LOG_ERROR("vst_midi_sysex: RTAlloc failed!");
		}
	}
	else {
		LOG_WARNING("vst_midi_sysex: no data!");
	}
}

void vst_tempo(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	float bpm = args->getf();
	CAST_UNIT->setTempo(bpm);
}

void vst_time_sig(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	int32 num = args->geti();
	int32 denom = args->geti();
	CAST_UNIT->setTimeSig(num, denom);
}

void vst_transport_play(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	int play = args->geti();
	CAST_UNIT->setTransportPlaying(play);
}

void vst_transport_set(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	float pos = args->getf();
	CAST_UNIT->setTransportPos(pos);
}

void vst_transport_get(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	CAST_UNIT->getTransportPos();
}

void vst_can_do(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	const char* what = args->gets();
	if (what) {
		CAST_UNIT->canDo(what);
	}
}

void vst_vendor_method(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	int32 index = args->geti();
	int32 value = args->geti(); // sc_msg_iter doesn't support 64bit ints...
	int32 size = args->getbsize();
	char *data = nullptr;
	if (size > 0) {
		data = (char *)RTAlloc(unit->mWorld, size);
		if (data) {
			args->getb(data, size);
		}
		else {
			LOG_ERROR("RTAlloc failed!");
			return;
		}
	}
	float opt = args->getf();
	CAST_UNIT->vendorSpecific(index, value, data, opt);
	if (data) {
		RTFree(unit->mWorld, data);
	}
}

/*** plugin command callbacks ***/

// add a user search path
void vst_path_add(World *inWorld, void* inUserData, struct sc_msg_iter *args, void *replyAddr) {
	// LATER make this realtime safe (for now let's assume allocating some strings isn't a big deal)
	if (!isSearching) {
		while (args->remain() > 0) {
			auto path = args->gets();
			if (path) {
				userSearchPaths.push_back(path);
			}
		}
	}
	else {
		LOG_WARNING("currently searching!");
	}
}
// clear all user search paths
void vst_path_clear(World *inWorld, void* inUserData, struct sc_msg_iter *args, void *replyAddr) {
	if (!isSearching) {
		userSearchPaths.clear();
	}
	else {
		LOG_WARNING("currently searching!");
	}
}

bool doProbePlugin(const std::string& path, VSTPluginInfo& info, bool verbose) {
	if (verbose) Print("probing '%s' ... ", path.c_str());
	auto result = probePlugin(path, info);
	if (verbose) {
		if (result == VSTProbeResult::success) {
            Print("ok!\n");
		}
		else if (result == VSTProbeResult::fail) {
			Print("failed!\n");
		}
		else if (result == VSTProbeResult::crash) {
			Print("crashed!\n");
		}
		else if (result == VSTProbeResult::error) {
			Print("error!\n");
		}
	}
	return result == VSTProbeResult::success;
}

// recursively searches directories for VST plugins.
bool cmdSearch(World *inWorld, void* cmdData) {
	auto data = (QueryCmdData *)cmdData;
	bool verbose = data->index;
	bool local = data->buf[0];
	int total = 0;
	std::vector<std::string> searchPaths;
	// list of new plugin keys (so plugins can be queried by index)
	pluginList.clear();
	// use default search paths?
	if (data->value) {
        for (auto& path : getDefaultSearchPaths()) {
			searchPaths.push_back(path);
		}
	}
	// add user search paths
	if (local) {
		// get search paths from file
		std::ifstream file(data->buf);
		if (file.is_open()) {
			std::string path;
			while (std::getline(file, path)) {
				searchPaths.push_back(path);
			}
		}
		else {
			LOG_ERROR("couldn't read plugin info file '" << data->buf << "'!");
		}
	}
	else {
		// use search paths added with '/vst_path_add'
		searchPaths.insert(searchPaths.end(), userSearchPaths.begin(), userSearchPaths.end());
	}
	for (auto& path : searchPaths) {
		LOG_VERBOSE("searching in '" << path << "':");
		int count = 0;
		searchPlugins(path, [&](const std::string& absPath, const std::string& relPath) {
			// check if the plugin hasn't been successfully probed already
			auto it = pluginMap.find(absPath);
			if (it == pluginMap.end()) {
				VSTPluginInfo info;
				if (doProbePlugin(absPath, info, verbose)) {
					// we're lazy and just duplicate the info (we have to change the whole thing later anyway for VST3...)
					pluginMap[absPath] = info;
					pluginMap[info.name] = info;
					pluginList.push_back(info.name);
					count++;
				}
			}
			else {
				auto& info = it->second;
				if (verbose) Print("%s\n", absPath.c_str());
				pluginList.push_back(info.name);
				count++;
			}
		});
		LOG_VERBOSE("found " << count << " plugins.");
		total += count;
	}
	// write new info to file (only for local Servers)
	if (local) {
		std::ofstream file(data->buf);
		if (file.is_open()) {
			LOG_DEBUG("writing plugin info file");
			for (auto& key : pluginList) {
				file << key << "\t";
				pluginMap[key].serialize(file);
				file << "\n"; // seperate plugins with newlines
			}
		}
		else {
			LOG_ERROR("couldn't write plugin info file '" << data->buf << "'!");
		}
	}
	// report the number of plugins
	makeReply(data->reply, sizeof(data->reply), "/vst_search", (int)pluginList.size());
	return true;
}

bool cmdSearchDone(World *inWorld, void *cmdData) {
	isSearching = false;
	// LOG_DEBUG("search done!");
	return true;
}

void vst_search(World *inWorld, void* inUserData, struct sc_msg_iter *args, void *replyAddr) {
	if (isSearching) {
		LOG_WARNING("already searching!");
		return;
	}
	auto useDefault = args->geti();
	auto verbose = args->geti();
	auto path = args->gets();
	auto pathLen = path ? strlen(path) + 1 : 0; // extra char for 0
	auto data = (QueryCmdData *)RTAlloc(inWorld, sizeof(QueryCmdData) + pathLen);
	if (data) {
		isSearching = true;
		data->value = useDefault;
		data->index = verbose;
		if (path) memcpy(data->buf, path, pathLen);
		else data->buf[0] = 0;
		// LOG_DEBUG("start search");
		// set 'cmdName' inside stage2 (/vst_search + numPlugins)
		DoAsynchronousCommand(inWorld, replyAddr, data->reply, data, cmdSearch, cmdSearchDone, 0, cmdRTfree, 0, 0);
	}
	else {
		LOG_ERROR("RTAlloc failed!");
	}
}
// query plugin info
bool cmdQuery(World *inWorld, void *cmdData) {
	auto data = (QueryCmdData *)cmdData;
	bool verbose = data->value;
	std::string key;
	// query by path (probe if necessary)
	if (data->buf[0]) {
		key.assign(data->buf);
		if (!pluginMap.count(key)) {
			VSTPluginInfo info;
			if (doProbePlugin(key, info, verbose)) {
				pluginMap[key] = info;
			}
		}
	}
	// by index (already probed)
	else {
		int index = data->index;
		if (index >= 0 && index < pluginList.size()) {
			key = pluginList[index];
		}
	}
	// find info
	auto it = pluginMap.find(key);
	if (it != pluginMap.end()) {
		auto& info = it->second;
		if (data->reply[0]) {
			// write to file
			std::ofstream file(data->reply);
			if (file.is_open()) {
				file << key << "\t";
				info.serialize(file);
			}
			else {
				LOG_ERROR("couldn't write plugin info file '" << data->reply << "'!");
			}
		}
		// reply with plugin info
		makeReply(data->reply, sizeof(data->reply), "/vst_info", key,
			info.path, info.name, info.vendor, info.category, info.version, info.id, info.numInputs, info.numOutputs,
			(int)info.parameters.size(), (int)info.programs.size(), (int)info.flags);
	}
	else {
		// empty reply
		makeReply(data->reply, sizeof(data->reply), "/vst_info");
	}
	return true;
}

void vst_query(World *inWorld, void* inUserData, struct sc_msg_iter *args, void *replyAddr) {
	if (isSearching) {
		LOG_WARNING("currently searching!");
		return;
	}
	QueryCmdData *data = nullptr;
	if (args->nextTag() == 's') {
		auto path = args->gets(); // plugin path
		auto size = strlen(path) + 1;
		data = (QueryCmdData *)RTAlloc(inWorld, sizeof(QueryCmdData) + size);
		if (data) {
			data->index = -1;
			memcpy(data->buf, path, size);
			auto file = args->gets();
			if (file) {
                auto len = std::min(sizeof(data->reply)-1, strlen(file)+1);
                memcpy(data->reply, file, len);
                data->reply[len] = 0;
			}
			else {
				data->reply[0] = 0;
			}
		}
		else {
			LOG_ERROR("RTAlloc failed!");
			return;
		}
	}
	else {
		data = (QueryCmdData *)RTAlloc(inWorld, sizeof(QueryCmdData));
		if (data) {
			data->index = args->geti();
			data->buf[0] = 0;
		}
		else {
			LOG_ERROR("RTAlloc failed!");
			return;
		}
	}
	// set 'cmdName' inside stage2 (/vst_info + info...)
	DoAsynchronousCommand(inWorld, replyAddr, data->reply, data, cmdQuery, 0, 0, cmdRTfree, 0, 0);
}

// query plugin parameter info
bool cmdQueryParam(World *inWorld, void *cmdData) {
	auto data = (QueryCmdData *)cmdData;
	auto it = pluginMap.find(data->buf);
	if (it != pluginMap.end()) {
		auto& params = it->second.parameters;
		auto reply = data->reply;
		int count = 0;
		int onset = sc_clip(data->index, 0, (int)params.size());
		int num = sc_clip(data->value, 0, (int)params.size() - onset);
		int size = sizeof(data->reply);
		count += doAddArg(reply, size, "/vst_param_info");
		// add plugin key (no need to check for overflow)
		count += doAddArg(reply + count, size - count, it->first);
		for (int i = 0; i < num && count < size; ++i) {
			auto& param = params[i + onset];
			count += doAddArg(reply + count, size - count, param.first); // name
			if (count < size) {
				count += doAddArg(reply + count, size - count, param.second); // label
			}
		}
		reply[count - 1] = '\0'; // remove trailing newline
	}
	else {
		// empty reply
		makeReply(data->reply, sizeof(data->reply), "/vst_param_info");
	}
	return true;
}

void vst_query_param(World *inWorld, void* inUserData, struct sc_msg_iter *args, void *replyAddr) {
	if (isSearching) {
		LOG_WARNING("currently searching!");
		return;
	}
	auto key = args->gets();
	auto size = strlen(key) + 1;
	auto data = (QueryCmdData *)RTAlloc(inWorld, sizeof(QueryCmdData) + size);
	if (data) {
		data->index = args->geti(); // parameter onset
		data->value = args->geti(); // num parameters to query
		memcpy(data->buf, key, size);
		// set 'cmdName' inside stage2 (/vst_param_info + param names/labels...)
		DoAsynchronousCommand(inWorld, replyAddr, data->reply, data, cmdQueryParam, 0, 0, cmdRTfree, 0, 0);
	}
	else {
		LOG_ERROR("RTAlloc failed!");
	}
}

// query plugin default program info
bool cmdQueryProgram(World *inWorld, void *cmdData) {
	auto data = (QueryCmdData *)cmdData;
	auto it = pluginMap.find(data->buf);
	if (it != pluginMap.end()) {
		auto& programs = it->second.programs;
		auto reply = data->reply;
		int count = 0;
		int onset = sc_clip(data->index, 0, programs.size());
		int num = sc_clip(data->value, 0, programs.size() - onset);
		int size = sizeof(data->reply);
		count += doAddArg(reply, size, "/vst_program_info");
		// add plugin key (no need to check for overflow)
		count += doAddArg(reply + count, size - count, it->first);
		for (int i = 0; i < num && count < size; ++i) {
			count += doAddArg(reply + count, size - count, programs[i + onset]); // name
		}
		reply[count - 1] = '\0'; // remove trailing newline
	}
	else {
		// empty reply
		makeReply(data->reply, sizeof(data->reply), "/vst_program_info");
	}

	return true;
}

void vst_query_program(World *inWorld, void* inUserData, struct sc_msg_iter *args, void *replyAddr) {
	if (isSearching) {
		LOG_WARNING("currently searching!");
		return;
	}
	auto key = args->gets();
	auto size = strlen(key) + 1;
	auto data = (QueryCmdData *)RTAlloc(inWorld, sizeof(QueryCmdData) + size);
	if (data) {
		data->index = args->geti(); // program onset
		data->value = args->geti(); // num programs to query
		memcpy(data->buf, key, size);
		// set 'cmdName' inside stage2 (/vst_param_info + param names/labels...)
		DoAsynchronousCommand(inWorld, replyAddr, data->reply, data, cmdQueryProgram, 0, 0, cmdRTfree, 0, 0);
	}
	else {
		LOG_ERROR("RTAlloc failed!");
	}
}

/*** plugin entry point ***/

void VSTPlugin_Ctor(VSTPlugin* unit){
	new(unit)VSTPlugin();
}

void VSTPlugin_Dtor(VSTPlugin* unit){
	unit->~VSTPlugin();
}

#define UnitCmd(x) DefineUnitCmd("VSTPlugin", "/" #x, vst_##x)
#define PluginCmd(x) DefinePlugInCmd("/" #x, x, 0)

PluginLoad(VSTPlugin) {
    // InterfaceTable *inTable implicitly given as argument to the load function
    ft = inTable; // store pointer to InterfaceTable
	DefineDtorCantAliasUnit(VSTPlugin);
	UnitCmd(open);
	UnitCmd(close);
	UnitCmd(reset);
	UnitCmd(vis);
	UnitCmd(set);
	UnitCmd(setn);
	UnitCmd(param_query);
	UnitCmd(get);
	UnitCmd(getn);
	UnitCmd(map);
	UnitCmd(unmap);
	UnitCmd(program_set);
	UnitCmd(program_query);
	UnitCmd(program_name);
	UnitCmd(program_read);
	UnitCmd(program_write);
	UnitCmd(program_data_set);
	UnitCmd(program_data_get);
	UnitCmd(bank_read);
	UnitCmd(bank_write);
	UnitCmd(bank_data_set);
	UnitCmd(bank_data_get);
	UnitCmd(midi_msg);
	UnitCmd(midi_sysex);
	UnitCmd(tempo);
	UnitCmd(time_sig);
	UnitCmd(transport_play);
	UnitCmd(transport_set);
	UnitCmd(transport_get);
	UnitCmd(can_do);
	UnitCmd(vendor_method);

    PluginCmd(vst_search);
	PluginCmd(vst_query);
	PluginCmd(vst_query_param);
	PluginCmd(vst_query_program);
	PluginCmd(vst_path_add);
	PluginCmd(vst_path_clear);
}

