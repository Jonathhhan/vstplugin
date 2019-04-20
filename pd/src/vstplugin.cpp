#include "vstplugin.h"

#undef pd_class
#define pd_class(x) (*(t_pd *)(x))
#define classname(x) (class_getname(pd_class(x)))

#if !VSTTHREADS // don't use VST GUI threads
# define MAIN_LOOP_POLL_INT 20
static t_clock *mainLoopClock = nullptr;
static void mainLoopTick(void *x){
    IVSTWindow::poll();
    clock_delay(mainLoopClock, MAIN_LOOP_POLL_INT);
}
#endif

// substitute SPACE for NO-BREAK SPACE (e.g. to avoid Tcl errors in the properties dialog)
static void substitute_whitespace(char *buf){
    for (char *c = buf; *c; c++){
        if (*c == ' '){
            *c = 160;
        }
    }
}

template<typename T>
static bool fromHex(const std::string& s, T& u){
    try {
        u = std::stoull(s, 0, 0);
        return true;
    } catch (...){
        return false;
    }
}

template<typename T>
static std::string toHex(T u){
    char buf[MAXPDSTRING];
    snprintf(buf, MAXPDSTRING, "0x%x", (unsigned int)u);
    return buf;
}

/*---------------------- search/probe ----------------------------*/

// map paths to plugin factories
using PluginFactoryDict = std::unordered_map<std::string, std::unique_ptr<IVSTFactory>>;
static PluginFactoryDict pluginFactoryDict;

// map symbols to plugin descriptions (can have aliases)
using PluginDescDict = std::unordered_map<std::string, std::shared_ptr<VSTPluginDesc>>;
static PluginDescDict pluginDescDict;

static IVSTFactory * findFactory(const std::string& path){
    auto factory = pluginFactoryDict.find(path);
    if (factory != pluginFactoryDict.end()){
        return factory->second.get();
    } else {
        return nullptr;
    }
}

static const VSTPluginDesc * findPlugin(const std::string& key){
    auto desc = pluginDescDict.find(key);
    if (desc != pluginDescDict.end()){
        return desc->second.get();
    }
    return nullptr;
}

// VST2: plug-in name
// VST3: plug-in name + ".vst3"
static std::string makeKey(const VSTPluginDesc& desc){
    std::string key;
    auto ext = ".vst3";
    auto onset = std::max<size_t>(0, desc.path.size() - strlen(ext));
    if (desc.path.find(ext, onset) != std::string::npos){
        key = desc.name + ext;
    } else {
        key = desc.name;
    }
    // replace whitespace with underscores so you can type it in Pd
    for (auto& c : key){
        if (c == ' '){
            c = '_';
        }
    }
    return key;
}

static void addPlugins(const IVSTFactory& factory){
    auto plugins = factory.plugins();
    for (auto& plugin : plugins){
        if (plugin->valid()){
            pluginDescDict[makeKey(*plugin)] = plugin;
        }
    }
}

static void clearPlugins(){
    pluginDescDict.clear();
}

static IVSTFactory * probePlugin(const std::string& path){
    if (findFactory(path)){
        LOG_WARNING("probePlugin: '" << path << "' already probed!");
        return nullptr;
    }
    // load factory and probe plugins
    std::stringstream msg;
    msg << "probing '" << path << "'... ";
    LOG_DEBUG("probing " << path);
    auto factory = IVSTFactory::load(path);
    if (!factory){
    #if 1
        msg << "failed!";
        verbose(PD_DEBUG, "%s", msg.str().c_str());
    #endif
        return nullptr;
    }
    factory->probe();
    auto plugins = factory->plugins();

    auto postResult = [](std::stringstream& m, ProbeResult pr){
        switch (pr){
        case ProbeResult::success:
            m << "ok!";
            verbose(PD_DEBUG, "%s", m.str().c_str());
            break;
        case ProbeResult::fail:
            m << "failed!";
            verbose(PD_DEBUG, "%s", m.str().c_str());
            break;
        case ProbeResult::crash:
            m << "crashed!";
            verbose(PD_NORMAL, "%s", m.str().c_str());
            break;
        case ProbeResult::error:
            m << "error!";
            verbose(PD_ERROR, "%s", m.str().c_str());
            break;
        default:
            bug("probePlugin");
            break;
        }
    };

    if (plugins.size() == 1){
        auto& plugin = plugins[0];
        postResult(msg, plugin->probeResult);
        // factories with a single plugin can also be aliased by their file path(s)
        pluginDescDict[plugins[0]->path] = plugins[0];
        pluginDescDict[path] = plugins[0];
    } else {
        verbose(PD_DEBUG, "%s", msg.str().c_str());
        for (auto& plugin : plugins){
            std::stringstream m;
            if (!plugin->name.empty()){
                m << "  '" << plugin->name << "' ";
            } else {
                m << "  plugin ";
            }
            postResult(m, plugin->probeResult);
        }
    }
    addPlugins(*factory);
    return (pluginFactoryDict[path] = std::move(factory)).get();
}

static void searchPlugins(const std::string& path, t_vstplugin *x = nullptr){
    int count = 0;
    std::vector<t_symbol *> pluginList; // list of plug-in keys
    verbose(PD_NORMAL, "searching in '%s' ...", path.c_str());
    vst::search(path, [&](const std::string& absPath, const std::string&) -> bool {
        std::string pluginPath = absPath;
        sys_unbashfilename(&pluginPath[0], &pluginPath[0]);
        // check if module has already been loaded
        IVSTFactory *factory = findFactory(pluginPath);
        if (factory){
            // just post names of valid plugins
            auto plugins = factory->plugins();
            if (plugins.size() == 1){
                auto& plugin = plugins[0];
                if (plugin->valid()){
                    auto& name = plugin->name;
                    auto key = makeKey(*plugin);
                    verbose(PD_DEBUG, "%s %s", path.c_str(), name.c_str());
                    if (x){
                        pluginList.push_back(gensym(key.c_str()));
                    }
                    count++;
                }
            } else {
                verbose(PD_DEBUG, "%s", path.c_str());
                for (auto& plugin : plugins){
                    if (plugin->valid()){
                        auto& name = plugin->name;
                        auto key = makeKey(*plugin);
                        verbose(PD_DEBUG, "  %s", name.c_str());
                        if (x){
                            pluginList.push_back(gensym(key.c_str()));
                        }
                        count++;
                    }
                }
            }
            // (re)add plugins (in case they have been removed by 'search_clear'
            addPlugins(*factory);
        } else {
            // probe (will post results and add plugins)
            if ((factory = probePlugin(pluginPath))){
                for (auto& plugin : factory->plugins()){
                    if (plugin->valid()){
                        if (x){
                            auto key = makeKey(*plugin);
                            pluginList.push_back(gensym(key.c_str()));
                        }
                        count++;
                    }
                }
            }
        }
        return true;
    });
    verbose(PD_NORMAL, "found %d plugin%s", count, (count == 1 ? "." : "s."));
    if (x){
        // sort plugin names alphabetically and case independent
        std::sort(pluginList.begin(), pluginList.end(), [](const auto& lhs, const auto& rhs){
            std::string s1 = lhs->s_name;
            std::string s2 = rhs->s_name;
            for (auto& c : s1) { c = std::tolower(c); }
            for (auto& c : s2) { c = std::tolower(c); }
            return strcmp(s1.c_str(), s2.c_str()) < 0;
        });
        for (auto& plugin : pluginList){
            t_atom msg;
            SETSYMBOL(&msg, plugin);
            outlet_anything(x->x_messout, gensym("plugin"), 1, &msg);
        }
    }
}

// called by [vstsearch]
extern "C" {
    void vst_search(void){
        for (auto& path : getDefaultSearchPaths()){
            searchPlugins(path);
        }
    }
}

/*--------------------- t_vstparam --------------------------*/

static t_class *vstparam_class;

t_vstparam::t_vstparam(t_vstplugin *x, int index)
    : p_owner(x), p_index(index){
    p_pd = vstparam_class;
    char buf[64];
        // slider
    snprintf(buf, sizeof(buf), "%p-hsl-%d", x, index);
    p_slider = gensym(buf);
    pd_bind(&p_pd, p_slider);
        // display
    snprintf(buf, sizeof(buf), "%p-d-%d-snd", x, index);
    p_display_snd = gensym(buf);
    pd_bind(&p_pd, p_display_snd);
    snprintf(buf, sizeof(buf), "%p-d-%d-rcv", x, index);
    p_display_rcv = gensym(buf);
}

t_vstparam::~t_vstparam(){
    pd_unbind(&p_pd, p_slider);
    pd_unbind(&p_pd, p_display_snd);
}

    // this will set the slider and implicitly call vstparam_set
void t_vstparam::set(t_floatarg f){
    pd_vmess(p_slider->s_thing, gensym("set"), (char *)"f", f);
}

    // called when moving a slider in the generic GUI
static void vstparam_float(t_vstparam *x, t_floatarg f){
    x->p_owner->set_param(x->p_index, f, true);
}

    // called when entering something in the symbol atom
static void vstparam_symbol(t_vstparam *x, t_symbol *s){
    x->p_owner->set_param(x->p_index, s->s_name, true);
}

static void vstparam_set(t_vstparam *x, t_floatarg f){
        // this method updates the display next to the label. implicitly called by t_vstparam::set
    IVSTPlugin &plugin = *x->p_owner->x_plugin;
    int index = x->p_index;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", plugin.getParameterDisplay(index).c_str());
    pd_vmess(x->p_display_rcv->s_thing, gensym("set"), (char *)"s", gensym(buf));
}

static void vstparam_setup(){
    vstparam_class = class_new(gensym("__vstparam"), 0, 0, sizeof(t_vstparam), 0, A_NULL);
    class_addfloat(vstparam_class, (t_method)vstparam_float);
    class_addsymbol(vstparam_class, (t_method)vstparam_symbol);
    class_addmethod(vstparam_class, (t_method)vstparam_set, gensym("set"), A_DEFFLOAT, 0);
}

/*-------------------- t_vsteditor ------------------------*/

t_vsteditor::t_vsteditor(t_vstplugin &owner, bool gui)
    : e_owner(&owner){
#if VSTTHREADS
    e_mainthread = std::this_thread::get_id();
#endif
    if (gui){
        pd_vmess(&pd_canvasmaker, gensym("canvas"), (char *)"iiiii", 0, 0, 100, 100, 10);
        e_canvas = (t_canvas *)s__X.s_thing;
        send_vmess(gensym("pop"), "i", 0);
    }
    e_clock = clock_new(this, (t_method)tick);
}

t_vsteditor::~t_vsteditor(){
    clock_free(e_clock);
}

    // post outgoing event (thread-safe if needed)
template<typename T, typename U>
void t_vsteditor::post_event(T& queue, U&& event){
#if VSTTHREADS
        // we only need to lock for GUI windows, but never for the generic Pd editor
    if (e_window){
        e_mutex.lock();
    }
#endif
    queue.push_back(std::forward<U>(event));
#if VSTTHREADS
    if (e_window){
        e_mutex.unlock();
    }
        // sys_lock / sys_unlock are not recursive so we check if we are in the main thread
    auto id = std::this_thread::get_id();
    if (id != e_mainthread){
        // LOG_DEBUG("lock");
        sys_lock();
    }
#endif
    clock_delay(e_clock, 0);
#if VSTTHREADS
    if (id != e_mainthread){
        sys_unlock();
        // LOG_DEBUG("unlocked");
    }
#endif
}

    // parameter automation notification might come from another thread (VST plugin GUI).
void t_vsteditor::parameterAutomated(int index, float value){
    post_event(e_automated, std::make_pair(index, value));
}

    // MIDI and SysEx events might be send from both the audio thread (e.g. arpeggiator) or GUI thread (MIDI controller)
void t_vsteditor::midiEvent(const VSTMidiEvent &event){
    post_event(e_midi, event);
}

void t_vsteditor::sysexEvent(const VSTSysexEvent &event){
    post_event(e_sysex, event);
}

void t_vsteditor::tick(t_vsteditor *x){
    t_outlet *outlet = x->e_owner->x_messout;
#if VSTTHREADS
        // we only need to lock if we have a GUI thread
    if (x->e_window){
        // it's more important to not block than flushing the queues on time
        if (!x->e_mutex.try_lock()){
            LOG_DEBUG("couldn't lock mutex");
            return;
        }
    }
#endif
        // swap parameter, midi and sysex queues.
    std::vector<std::pair<int, float>> paramQueue;
    paramQueue.swap(x->e_automated);
    std::vector<VSTMidiEvent> midiQueue;
    midiQueue.swap(x->e_midi);
    std::vector<VSTSysexEvent> sysexQueue;
    sysexQueue.swap(x->e_sysex);

#if VSTTHREADS
    if (x->e_window){
        x->e_mutex.unlock();
    }
#endif
        // NOTE: it is theoretically possible that outputting messages
        // will cause more messages to be sent (e.g. when being fed back into [vstplugin~]
        // and if there's no mutex involved this would modify a std::vector while being read.
        // one solution is to just double buffer via swap, so subsequent events will go to
        // a new empty queue. Although I *think* this might not be necessary for midi/sysex messages
        // I do it anyway. swapping a std::vector is cheap. also it minimizes the time spent
        // in the critical section.

        // automated parameters:
    for (auto& param : paramQueue){
        int index = param.first;
        float value = param.second;
            // update the generic GUI
        x->param_changed(index, value);
            // send message
        t_atom msg[2];
        SETFLOAT(&msg[0], index);
        SETFLOAT(&msg[1], value);
        outlet_anything(outlet, gensym("param_automated"), 2, msg);
    }
        // midi events:
    for (auto& midi : midiQueue){
        t_atom msg[3];
        SETFLOAT(&msg[0], (unsigned char)midi.data[0]);
        SETFLOAT(&msg[1], (unsigned char)midi.data[1]);
        SETFLOAT(&msg[2], (unsigned char)midi.data[2]);
        outlet_anything(outlet, gensym("midi"), 3, msg);
    }
        // sysex events:
    for (auto& sysex : sysexQueue){
        std::vector<t_atom> msg;
        int n = sysex.data.size();
        msg.resize(n);
        for (int i = 0; i < n; ++i){
            SETFLOAT(&msg[i], (unsigned char)sysex.data[i]);
        }
        outlet_anything(outlet, gensym("midi"), n, msg.data());
    }
}

#if VSTTHREADS
    // create plugin + editor GUI (in another thread)
void t_vsteditor::thread_function(VSTPluginPromise promise, const VSTPluginDesc *desc){
    LOG_DEBUG("enter thread");
    std::shared_ptr<IVSTPlugin> plugin;
    if (desc && desc->valid()){
        plugin = desc->create();
    }
    if (!plugin){
            // signal main thread
        promise.set_value(nullptr);
        LOG_DEBUG("exit thread");
        return;
    }
        // create GUI window (if needed)
    if (plugin->hasEditor()){
        e_window = IVSTWindow::create(*plugin);
    }
        // return plugin to main thread
        // (but keep a reference in the GUI thread)
    promise.set_value(plugin);
        // setup GUI window (if any)
    if (e_window){
        e_window->setTitle(plugin->getPluginName());
        int left, top, right, bottom;
        plugin->getEditorRect(left, top, right, bottom);
        e_window->setGeometry(left, top, right, bottom);

        plugin->openEditor(e_window->getHandle());

        LOG_DEBUG("enter message loop");
            // run the event loop until it gets a quit message
        e_window->run();
        LOG_DEBUG("VST plugin closed");
    }
    LOG_DEBUG("exit thread");
}
#endif

std::shared_ptr<IVSTPlugin> t_vsteditor::open_plugin(const VSTPluginDesc& desc, bool editor){
        // -n flag (no gui)
    if (!e_canvas){
        editor = false;
    }
    if (editor){
        // initialize GUI backend (if needed)
        static bool initialized = false;
        if (!initialized){
            IVSTWindow::initialize();
            initialized = true;
        }
    }
#if VSTTHREADS
        // creates a new thread where the plugin is created and the message loop runs
    if (editor){
        VSTPluginPromise promise;
        auto future = promise.get_future();
        e_thread = std::thread(&t_vsteditor::thread_function, this, std::move(promise), &desc);
            // wait for thread to return the plugin
        return future.get();
    }
#endif
        // create plugin in main thread
    std::shared_ptr<IVSTPlugin> plugin;
    if (desc.valid()){
        plugin = desc.create();
    }
    if (!plugin) return nullptr;
#if !VSTTHREADS
        // create and setup GUI window in main thread (if needed)
    if (editor && plugin->hasEditor()){
        e_window = IVSTWindow::create(*plugin);
        if (e_window){
            e_window->setTitle(plugin->getPluginName());
            int left, top, right, bottom;
            plugin->getEditorRect(left, top, right, bottom);
            e_window->setGeometry(left, top, right, bottom);
            // don't open the editor on macOS (see VSTWindowCocoa.mm)
#ifndef __APPLE__
            plugin->openEditor(e_window->getHandle());
#endif
        }
    }
#endif
    return plugin;
}

void t_vsteditor::close_plugin(){
    if (e_window){
#if VSTTHREADS
            // first release our reference
        e_owner->x_plugin = nullptr;
            // terminate the message loop - this will implicitly release
            // the plugin in the GUI thread (some plugins expect to be
            // released in the same thread where they have been created)
        e_window->quit();
            // now join the thread
        if (e_thread.joinable()){
            e_thread.join();
            LOG_DEBUG("thread joined");
        }
        e_window = nullptr; // finally destroy the window
#else
        e_window = nullptr; // first destroy the window
        e_owner->x_plugin = nullptr; // then release the plugin
        LOG_DEBUG("VST plugin closed");
#endif
    } else {
        vis(0); // close the Pd editor
        e_owner->x_plugin = nullptr; // release the plugin
        LOG_DEBUG("VST plugin closed");
    }
}

const int xoffset = 30;
const int yoffset = 30;
const int maxparams = 16; // max. number of params per column
const int row_width = 128 + 10 + 128; // slider + symbol atom + label
const int col_height = 40;

void t_vsteditor::setup(){
    if (!pd_gui()){
        return;
    }

    send_vmess(gensym("rename"), (char *)"s", gensym(e_owner->x_plugin->getPluginName().c_str()));
    send_mess(gensym("clear"));

    int nparams = e_owner->x_plugin->getNumParameters();
    e_params.clear();
    // reserve to avoid a reallocation (which will call destructors)
    e_params.reserve(nparams);
    for (int i = 0; i < nparams; ++i){
        e_params.emplace_back(e_owner, i);
    }
        // slider: #X obj ...
    char sliderText[] = "25 43 hsl 128 15 0 1 0 0 snd rcv label -2 -8 0 10 -262144 -1 -1 0 1";
    t_binbuf *sliderBuf = binbuf_new();
    binbuf_text(sliderBuf, sliderText, strlen(sliderText));
    t_atom *slider = binbuf_getvec(sliderBuf);
        // display: #X symbolatom ...
    char displayText[] = "165 79 10 0 0 1 label rcv snd";
    t_binbuf *displayBuf = binbuf_new();
    binbuf_text(displayBuf, displayText, strlen(displayText));
    t_atom *display = binbuf_getvec(displayBuf);

    int ncolumns = nparams / maxparams + ((nparams % maxparams) != 0);
    if (!ncolumns) ncolumns = 1; // just to prevent division by zero
    int nrows = nparams / ncolumns + ((nparams % ncolumns) != 0);

    for (int i = 0; i < nparams; ++i){
        int col = i / nrows;
        int row = i % nrows;
        int xpos = xoffset + col * row_width;
        int ypos = yoffset + row * col_height;
            // create slider
        SETFLOAT(slider, xpos);
        SETFLOAT(slider+1, ypos);
        SETSYMBOL(slider+9, e_params[i].p_slider);
        SETSYMBOL(slider+10, e_params[i].p_slider);
        char buf[64];
        snprintf(buf, sizeof(buf), "%d: %s", i, e_owner->x_plugin->getParameterName(i).c_str());
        substitute_whitespace(buf);
        SETSYMBOL(slider+11, gensym(buf));
        send_mess(gensym("obj"), 21, slider);
            // create display
        SETFLOAT(display, xpos + 128 + 10); // slider + space
        SETFLOAT(display+1, ypos);
        SETSYMBOL(display+6, gensym(e_owner->x_plugin->getParameterLabel(i).c_str()));
        SETSYMBOL(display+7, e_params[i].p_display_rcv);
        SETSYMBOL(display+8, e_params[i].p_display_snd);
        send_mess(gensym("symbolatom"), 9, display);
    }
    float width = row_width * ncolumns + 2 * xoffset;
    float height = nrows * col_height + 2 * yoffset;
    if (width > 1000) width = 1000;
    send_vmess(gensym("setbounds"), "ffff", 0.f, 0.f, width, height);
    send_vmess(gensym("vis"), "i", 0);

    update();

    binbuf_free(sliderBuf);
    binbuf_free(displayBuf);
}

void t_vsteditor::update(){
    if (!e_owner->check_plugin()) return;

    if (e_window){
        e_window->update();
    } else if (pd_gui()) {
        int n = e_owner->x_plugin->getNumParameters();
        for (int i = 0; i < n; ++i){
            param_changed(i, e_owner->x_plugin->getParameter(i));
        }
    }
}

    // automated: true if parameter change comes from the (generic) GUI
void t_vsteditor::param_changed(int index, float value, bool automated){
    if (pd_gui() && index >= 0 && index < (int)e_params.size()){
        e_params[index].set(value);
        if (automated){
            parameterAutomated(index, value);
        }
    }
}

void t_vsteditor::vis(bool v){
    if (e_window){
        if (v){
            e_window->bringToTop();
        } else {
            e_window->hide();
        }
    } else if (pd_gui()) {
        send_vmess(gensym("vis"), "i", (int)v);
    }
}

/*---------------- t_vstplugin (public methods) ------------------*/

// close
static void vstplugin_close(t_vstplugin *x){
    x->x_editor->close_plugin();
    x->x_info = nullptr;
    x->x_path = nullptr;
}

// search
static void vstplugin_search(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (argc > 0){
        while (argc--){
            auto sym = atom_getsymbol(argv++);
            char path[MAXPDSTRING];
            canvas_makefilename(x->x_canvas, sym->s_name, path, MAXPDSTRING);
            searchPlugins(path, x);
        }
    } else {  // search in the default VST search paths if no user paths were provided
        for (auto& path : getDefaultSearchPaths()){
            searchPlugins(path.c_str(), x);
        }
    }
}

static void vstplugin_search_clear(t_vstplugin *x){
        // clear the plugin description dictionary
        // (doesn't remove the actual plugin descriptions!)
    clearPlugins();
}

// resolves relative paths to the canvas search paths or VST search paths
static std::string resolvePath(t_canvas *c, const std::string& s){
    std::string path = s;
        // resolve relative path
    if (!sys_isabsolutepath(path.c_str())){
        char buf[MAXPDSTRING+1];
    #ifdef _WIN32
        const char *ext = ".dll";
    #elif defined(__APPLE__)
        const char *ext = ".vst";
    #else
        const char *ext = ".so";
    #endif
        if (path.find(".vst3") == std::string::npos && path.find(ext) == std::string::npos){
            path += ext;
        }
            // first try canvas search paths
        char dirresult[MAXPDSTRING];
        char *name = nullptr;
    #ifdef __APPLE__
        const char *bundleinfo = "/Contents/Info.plist";
        path += bundleinfo; // on MacOS VST plugins are bundles but canvas_open needs a real file
    #endif
        int fd = canvas_open(c, path.c_str(), "", dirresult, &name, MAXPDSTRING, 1);
        if (fd >= 0){
            sys_close(fd);
            snprintf(buf, MAXPDSTRING, "%s/%s", dirresult, name);
    #ifdef __APPLE__
            char *find = strstr(buf, bundleinfo);
            if (find){
                *find = 0; // restore the bundle path
            }
    #endif
            path = buf; // success
        } else {
                // otherwise try default VST paths
            for (auto& vstpath : getDefaultSearchPaths()){
                snprintf(buf, MAXPDSTRING, "%s/%s", vstpath.c_str(), path.c_str());
                LOG_DEBUG("trying " << buf);
                fd = sys_open(buf, 0);
                if (fd >= 0){
                    sys_close(fd);
                #ifdef __APPLE__
                    char *find = strstr(buf, bundleinfo);
                    if (find){
                        *find = 0; // restore the bundle path
                    }
                #endif
                    path = buf; // success
                    break;
                }
            }
        }
    }
    sys_unbashfilename(&path[0], &path[0]);
    return path;
}

// query a plugin by its name or file path and probe if necessary.
static const VSTPluginDesc * queryPlugin(t_vstplugin *x, const std::string& path){
    // query plugin
    auto desc = findPlugin(path);
    if (!desc){
            // try as file path
        std::string abspath = resolvePath(x->x_canvas, path);
        if (!abspath.empty()){
                // plugin descs might have been removed by 'search_clear'
            auto factory = findFactory(abspath);
            if (factory){
                addPlugins(*factory);
            }
            if (!(desc = findPlugin(abspath))){
                    // finally probe plugin
                if (probePlugin(abspath)){
                        // this fails if the module contains several plugins
                        // (so the path is not used as a key)
                    desc = findPlugin(abspath);
                }
            }
        }
   }
   return desc;
}

// open
static void vstplugin_open(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    const VSTPluginDesc *info = nullptr;
    t_symbol *pathsym = nullptr;
    bool editor = false;
        // parse arguments
    while (argc && argv->a_type == A_SYMBOL){
        auto sym = argv->a_w.w_symbol;
        if (*sym->s_name == '-'){ // flag
            const char *flag = sym->s_name;
            if (!strcmp(flag, "-e")){
                editor = true;
            } else {
                pd_error(x, "%s: unknown flag '%s'", classname(x), flag);
            }
            argc--; argv++;
        } else { // file name
            pathsym = sym;
                // don't reopen the same plugin (mainly for -k flag)
            if (pathsym == x->x_path && x->x_editor->vst_gui() == editor){
                return;
            }
            break;
        }
    }
    if (!pathsym){
        pd_error(x, "%s: 'open' needs a symbol argument!", classname(x));
        return;
    }
    if (!(info = queryPlugin(x, pathsym->s_name))){
        pd_error(x, "%s: can't open '%s' - no such file or plugin!", classname(x), pathsym->s_name);
        return;
    }
    if (!info->valid()){
        pd_error(x, "%s: can't use plugin '%s'", classname(x), info->name.c_str());
        return;
    }
        // *now* close the old plugin
    vstplugin_close(x);
        // open the new VST plugin
    std::shared_ptr<IVSTPlugin> plugin = x->x_editor->open_plugin(*info, editor);
    if (plugin){
        x->x_info = info; // plugin descriptions are never removed
        x->x_path = pathsym; // store path symbol (to avoid reopening the same plugin)
        post("loaded VST plugin '%s'", plugin->getPluginName().c_str());
        plugin->suspend();
            // initially, blocksize is 0 (before the 'dsp' message is sent).
            // some plugins might not like 0, so we send some sane default size.
        plugin->setBlockSize(x->x_blocksize > 0 ? x->x_blocksize : 64);
        plugin->setSampleRate(x->x_sr);
        int nin = std::min<int>(plugin->getNumInputs(), x->x_siginlets.size());
        int nout = std::min<int>(plugin->getNumOutputs(), x->x_sigoutlets.size());
        plugin->setNumSpeakers(nin, nout);
        plugin->resume();
        x->x_plugin = std::move(plugin);
            // receive events from plugin
        x->x_plugin->setListener(x->x_editor.get());
        x->update_precision();
        x->update_buffer();
        x->x_editor->setup();
    } else {
            // shouldn't happen...
        pd_error(x, "%s: couldn't open '%s'", classname(x), info->name.c_str());
    }
}

static void sendInfo(t_vstplugin *x, const char *what, const std::string& value){
    t_atom msg[2];
    SETSYMBOL(&msg[0], gensym(what));
    SETSYMBOL(&msg[1], gensym(value.c_str()));
    outlet_anything(x->x_messout, gensym("info"), 2, msg);
}

static void sendInfo(t_vstplugin *x, const char *what, int value){
    t_atom msg[2];
    SETSYMBOL(&msg[0], gensym(what));
    SETFLOAT(&msg[1], value);
    outlet_anything(x->x_messout, gensym("info"), 2, msg);
}

// plugin info (no args: currently loaded plugin, symbol arg: path of plugin to query)
static void vstplugin_info(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    const VSTPluginDesc *info = nullptr;
    if (argc > 0){
        auto path = atom_getsymbol(argv)->s_name;
        if (!(info = queryPlugin(x, path))){
            pd_error(x, "%s: couldn't open '%s' - no such file or plugin!", classname(x), path);
            return;
        }
    } else {
        if (!x->check_plugin()) return;
        info = x->x_info;
    }
    if (info){
        sendInfo(x, "path", info->path);
        sendInfo(x, "name", info->name);
        sendInfo(x, "vendor", info->vendor);
        sendInfo(x, "category", info->category);
        sendInfo(x, "version", info->version);
        sendInfo(x, "inputs", info->numInputs);
        sendInfo(x, "outputs", info->numOutputs);
        sendInfo(x, "id", toHex(info->id));
        sendInfo(x, "editor", info->flags >> HasEditor & 1);
        sendInfo(x, "synth", info->flags >> IsSynth & 1);
        sendInfo(x, "single", info->flags >> SinglePrecision & 1);
        sendInfo(x, "double", info->flags >> DoublePrecision & 1);
        sendInfo(x, "midiin", info->flags >> MidiInput & 1);
        sendInfo(x, "midiout", info->flags >> MidiOutput & 1);
        sendInfo(x, "sysexin", info->flags >> SysexInput & 1);
        sendInfo(x, "sysexout", info->flags >> SysexOutput & 1);
    }
}

// query plugin for capabilities
static void vstplugin_can_do(t_vstplugin *x, t_symbol *s){
    if (!x->check_plugin()) return;
    int result = x->x_plugin->canDo(s->s_name);
    t_atom msg[2];
    SETSYMBOL(&msg[0], s);
    SETFLOAT(&msg[1], result);
    outlet_anything(x->x_messout, gensym("can_do"), 2, msg);
}

// vendor specific action (index, value, opt, data)
static void vstplugin_vendor_method(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;
    int index = 0;
    intptr_t value = 0;

        // get integer argument as number or hex string
    auto getInt = [&](int which, auto& var){
        if (argc > which){
            if (argv->a_type == A_SYMBOL){
                auto c = argv->a_w.w_symbol->s_name;
                if (!fromHex(c, var)){
                    pd_error(x, "%s: couldn't convert '%s'", classname(x), c);
                    return false;
                }
            } else {
                var = atom_getfloat(argv);
            }
        }
        return true;
    };

    if (!getInt(0, index)) return;
    if (!getInt(1, value)) return;
    float opt = atom_getfloatarg(2, argc, argv);
    char *data = nullptr;
    int size = argc - 3;
    if (size > 0){
        data = (char *)getbytes(size);
        for (int i = 0, j = 3; i < size; ++i, ++j){
            data[i] = atom_getfloat(argv + j);
        }
    }
    intptr_t result = x->x_plugin->vendorSpecific(index, value, data, opt);
    t_atom msg[2];
    SETFLOAT(&msg[0], result);
    SETSYMBOL(&msg[1], gensym(toHex(result).c_str()));
    outlet_anything(x->x_messout, gensym("vendor_method"), 2, msg);
    if (data){
        freebytes(data, size);
    }
}

// print plugin info in Pd console
static void vstplugin_print(t_vstplugin *x){
    if (!x->check_plugin()) return;
    post("~~~ VST plugin info ~~~");
    post("name: %s", x->x_info->name.c_str());
    post("path: %s", x->x_info->path.c_str());
    post("vendor: %s", x->x_info->vendor.c_str());
    post("category: %s", x->x_info->category.c_str());
    post("version: %s", x->x_info->version.c_str());
    post("input channels: %d", x->x_info->numInputs);
    post("output channels: %d", x->x_info->numOutputs);
    post("single precision: %s", x->x_plugin->hasPrecision(VSTProcessPrecision::Single) ? "yes" : "no");
    post("double precision: %s", x->x_plugin->hasPrecision(VSTProcessPrecision::Double) ? "yes" : "no");
    post("editor: %s", x->x_plugin->hasEditor() ? "yes" : "no");
    post("number of parameters: %d", x->x_plugin->getNumParameters());
    post("number of programs: %d", x->x_plugin->getNumPrograms());
    post("synth: %s", x->x_plugin->isSynth() ? "yes" : "no");
    post("midi input: %s", x->x_plugin->hasMidiInput() ? "yes" : "no");
    post("midi output: %s", x->x_plugin->hasMidiOutput() ? "yes" : "no");
    post("");
}

// bypass the plugin
static void vstplugin_bypass(t_vstplugin *x, t_floatarg f){
    x->x_bypass = (f != 0);
    if (x->x_plugin){
        if (x->x_bypass){
            x->x_plugin->suspend();
        } else {
            x->x_plugin->resume();
        }
    }
}

// reset the plugin
static void vstplugin_reset(t_vstplugin *x, t_floatarg f){
    if (!x->check_plugin()) return;
    x->x_plugin->suspend();
    x->x_plugin->resume();
}

// show/hide editor window
static void vstplugin_vis(t_vstplugin *x, t_floatarg f){
    if (!x->check_plugin()) return;
    x->x_editor->vis(f);
}

static void vstplugin_click(t_vstplugin *x){
    vstplugin_vis(x, 1);
}

// set processing precision (single or double)
static void vstplugin_precision(t_vstplugin *x, t_symbol *s){
    if (s == gensym("single")){
        x->x_dp = 0;
    } else if (s == gensym("double")){
        x->x_dp = 1;
    } else {
        pd_error(x, "%s: bad argument to 'precision' message - must be 'single' or 'double'", classname(x));
        return;
    }
    x->update_precision();
        // clear the input buffer to avoid garbage in subsequent channels when the precision changes.
    memset(x->x_inbuf.data(), 0, x->x_inbuf.size()); // buffer is char array
}

/*------------------------ transport----------------------------------*/

// set tempo in BPM
static void vstplugin_tempo(t_vstplugin *x, t_floatarg f){
    if (!x->check_plugin()) return;
    if (f > 0){
        x->x_plugin->setTempoBPM(f);
    } else {
        pd_error(x, "%s: tempo must greater than 0", classname(x));
    }
}

// set time signature
static void vstplugin_time_signature(t_vstplugin *x, t_floatarg num, t_floatarg denom){
    if (!x->check_plugin()) return;
    if (num > 0 && denom > 0){
        x->x_plugin->setTimeSignature(num, denom);
    } else {
        pd_error(x, "%s: bad time signature", classname(x));
    }
}

// play/stop
static void vstplugin_play(t_vstplugin *x, t_floatarg f){
    if (!x->check_plugin()) return;
    x->x_plugin->setTransportPlaying(f);
}

// cycle
static void vstplugin_cycle(t_vstplugin *x, t_floatarg f){
    if (!x->check_plugin()) return;
    x->x_plugin->setTransportCycleActive(f);
}

static void vstplugin_cycle_start(t_vstplugin *x, t_floatarg f){
    if (!x->check_plugin()) return;
    x->x_plugin->setTransportCycleStart(f);
}

static void vstplugin_cycle_end(t_vstplugin *x, t_floatarg f){
    if (!x->check_plugin()) return;
    x->x_plugin->setTransportCycleEnd(f);
}

// set transport position (quarter notes)
static void vstplugin_transport_set(t_vstplugin *x, t_floatarg f){
    if (!x->check_plugin()) return;
    x->x_plugin->setTransportPosition(f);
}

// get current transport position
static void vstplugin_transport_get(t_vstplugin *x){
    if (!x->check_plugin()) return;
    t_atom a;
    SETFLOAT(&a, x->x_plugin->getTransportPosition());
    outlet_anything(x->x_messout, gensym("transport"), 1, &a);
}

/*------------------------------------ parameters ------------------------------------------*/

// set parameter by float (0.0 - 1.0) or string (if supported)
static void vstplugin_param_set(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;
    if (argc < 2){
        pd_error(x, "%s: 'param_set' expects two arguments (index + float/symbol)", classname(x));
    }
    int index = atom_getfloat(argv);
    switch (argv[1].a_type){
    case A_FLOAT:
        x->set_param(index, argv[1].a_w.w_float, false);
        break;
    case A_SYMBOL:
        x->set_param(index, argv[1].a_w.w_symbol->s_name, false);
        break;
    default:
        pd_error(x, "%s: second argument for 'param_set' must be a float or symbol", classname(x));
        break;
    }
}

// get parameter state (value + display)
static void vstplugin_param_get(t_vstplugin *x, t_floatarg _index){
    if (!x->check_plugin()) return;
    int index = _index;
    if (index >= 0 && index < x->x_plugin->getNumParameters()){
        t_atom msg[3];
		SETFLOAT(&msg[0], index);
        SETFLOAT(&msg[1], x->x_plugin->getParameter(index));
        SETSYMBOL(&msg[2], gensym(x->x_plugin->getParameterDisplay(index).c_str()));
        outlet_anything(x->x_messout, gensym("param_state"), 3, msg);
	} else {
        pd_error(x, "%s: parameter index %d out of range!", classname(x), index);
	}
}

// get parameter info (name + label + ...)
static void vstplugin_param_info(t_vstplugin *x, t_floatarg _index){
    if (!x->check_plugin()) return;
    int index = _index;
    if (index >= 0 && index < x->x_plugin->getNumParameters()){
        t_atom msg[3];
		SETFLOAT(&msg[0], index);
        SETSYMBOL(&msg[1], gensym(x->x_plugin->getParameterName(index).c_str()));
        SETSYMBOL(&msg[2], gensym(x->x_plugin->getParameterLabel(index).c_str()));
        // LATER add more info
        outlet_anything(x->x_messout, gensym("param_info"), 3, msg);
	} else {
        pd_error(x, "%s: parameter index %d out of range!", classname(x), index);
	}
}

// number of parameters
static void vstplugin_param_count(t_vstplugin *x){
    if (!x->check_plugin()) return;
	t_atom msg;
    SETFLOAT(&msg, x->x_plugin->getNumParameters());
	outlet_anything(x->x_messout, gensym("param_count"), 1, &msg);
}

// list parameters (index + info)
static void vstplugin_param_list(t_vstplugin *x){
    if (!x->check_plugin()) return;
    int n = x->x_plugin->getNumParameters();
	for (int i = 0; i < n; ++i){
        vstplugin_param_info(x, i);
	}
}

// list parameter states (index + value)
static void vstplugin_param_dump(t_vstplugin *x){
    if (!x->check_plugin()) return;
    int n = x->x_plugin->getNumParameters();
    for (int i = 0; i < n; ++i){
        vstplugin_param_get(x, i);
    }
}

/*------------------------------------- MIDI -----------------------------------------*/

// send raw MIDI message
static void vstplugin_midi_raw(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;

    VSTMidiEvent event;
    for (int i = 0; i < 3 && i < argc; ++i){
        event.data[i] = atom_getfloat(argv + i);
    }
    x->x_plugin->sendMidiEvent(event);
}

// helper function
static void vstplugin_midi_mess(t_vstplugin *x, int onset, int channel, int v1, int v2 = 0){
    t_atom atoms[3];
    channel = std::max(1, std::min(16, (int)channel)) - 1;
    v1 = std::max(0, std::min(127, v1));
    v2 = std::max(0, std::min(127, v2));
    SETFLOAT(&atoms[0], channel + onset);
    SETFLOAT(&atoms[1], v1);
    SETFLOAT(&atoms[2], v2);
    vstplugin_midi_raw(x, 0, 3, atoms);
}

// send MIDI messages (convenience methods)
static void vstplugin_midi_noteoff(t_vstplugin *x, t_floatarg channel, t_floatarg pitch, t_floatarg velocity){
    vstplugin_midi_mess(x, 128, channel, pitch, velocity);
}

static void vstplugin_midi_note(t_vstplugin *x, t_floatarg channel, t_floatarg pitch, t_floatarg velocity){
    vstplugin_midi_mess(x, 144, channel, pitch, velocity);
}

static void vstplugin_midi_polytouch(t_vstplugin *x, t_floatarg channel, t_floatarg pitch, t_floatarg pressure){
    vstplugin_midi_mess(x, 160, channel, pitch, pressure);
}

static void vstplugin_midi_cc(t_vstplugin *x, t_floatarg channel, t_floatarg ctl, t_floatarg value){
    vstplugin_midi_mess(x, 176, channel, ctl, value);
}

static void vstplugin_midi_program(t_vstplugin *x, t_floatarg channel, t_floatarg program){
   vstplugin_midi_mess(x, 192, channel, program);
}

static void vstplugin_midi_touch(t_vstplugin *x, t_floatarg channel, t_floatarg pressure){
    vstplugin_midi_mess(x, 208, channel, pressure);
}

static void vstplugin_midi_bend(t_vstplugin *x, t_floatarg channel, t_floatarg bend){
        // map from [-1.f, 1.f] to [0, 16383] (14 bit)
    int val = (bend + 1.f) * 8192.f; // 8192 is the center position
    val = std::max(0, std::min(16383, val));
    vstplugin_midi_mess(x, 224, channel, val & 127, (val >> 7) & 127);
}

// send MIDI SysEx message
static void vstplugin_midi_sysex(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;

    std::string data;
    data.reserve(argc);
    for (int i = 0; i < argc; ++i){
        data.push_back((unsigned char)atom_getfloat(argv+i));
    }

    x->x_plugin->sendSysexEvent(VSTSysexEvent(std::move(data)));
}

/* --------------------------------- programs --------------------------------- */

// set the current program by index
static void vstplugin_program_set(t_vstplugin *x, t_floatarg _index){
    if (!x->check_plugin()) return;
    int index = _index;
    if (index >= 0 && index < x->x_plugin->getNumPrograms()){
        x->x_plugin->setProgram(index);
        x->x_editor->update();
	} else {
        pd_error(x, "%s: program number %d out of range!", classname(x), index);
	}
}

// get the current program index
static void vstplugin_program_get(t_vstplugin *x){
    if (!x->check_plugin()) return;
	t_atom msg;
    SETFLOAT(&msg, x->x_plugin->getProgram());
    outlet_anything(x->x_messout, gensym("program"), 1, &msg);
}

// set the name of the current program
static void vstplugin_program_name_set(t_vstplugin *x, t_symbol* name){
    if (!x->check_plugin()) return;
    x->x_plugin->setProgramName(name->s_name);
}

// get the program name by index. no argument: get the name of the current program.
static void vstplugin_program_name_get(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;
    t_atom msg[2];
    if (argc){
        int index = atom_getfloat(argv);
        SETFLOAT(&msg[0], index);
        SETSYMBOL(&msg[1], gensym(x->x_plugin->getProgramNameIndexed(index).c_str()));
    } else {
        SETFLOAT(&msg[0], x->x_plugin->getProgram());
        SETSYMBOL(&msg[1], gensym(x->x_plugin->getProgramName().c_str()));
    }
    outlet_anything(x->x_messout, gensym("program_name"), 2, msg);
}

// get number of programs
static void vstplugin_program_count(t_vstplugin *x){
    if (!x->check_plugin()) return;
	t_atom msg;
    SETFLOAT(&msg, x->x_plugin->getNumPrograms());
	outlet_anything(x->x_messout, gensym("program_count"), 1, &msg);
}

// list all programs (index + name)
static void vstplugin_program_list(t_vstplugin *x){
    int n = x->x_plugin->getNumPrograms();
    t_atom msg[2];
    for (int i = 0; i < n; ++i){
        SETFLOAT(&msg[0], i);
        SETSYMBOL(&msg[1], gensym(x->x_plugin->getProgramNameIndexed(i).c_str()));
        outlet_anything(x->x_messout, gensym("program_name"), 2, msg);
    }
}

// set program data (list of bytes)
static void vstplugin_program_data_set(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;
    std::string buffer;
    buffer.resize(argc);
    for (int i = 0; i < argc; ++i){
           // first clamp to 0-255, then assign to char (not 100% portable...)
        buffer[i] = (unsigned char)atom_getfloat(argv + i);
    }
    if (x->x_plugin->readProgramData(buffer)){
        x->x_editor->update();
    } else {
        pd_error(x, "%s: bad FX program data", classname(x));
    }
}

// get program data
static void vstplugin_program_data_get(t_vstplugin *x){
    if (!x->check_plugin()) return;
    std::string buffer;
    x->x_plugin->writeProgramData(buffer);
    int n = buffer.size();
    if (!n){
        pd_error(x, "%s: couldn't get program data", classname(x));
        return;
    }
    std::vector<t_atom> atoms;
    atoms.resize(n);
    for (int i = 0; i < n; ++i){
            // first convert to range 0-255, then assign to t_float (not 100% portable...)
        SETFLOAT(&atoms[i], (unsigned char)buffer[i]);
    }
    outlet_anything(x->x_messout, gensym("program_data"), n, atoms.data());
}

// read program file (.FXP)
static void vstplugin_program_read(t_vstplugin *x, t_symbol *s){
    if (!x->check_plugin()) return;
    char dir[MAXPDSTRING], *name;
    int fd = canvas_open(x->x_canvas, s->s_name, "", dir, &name, MAXPDSTRING, 1);
    if (fd < 0){
        pd_error(x, "%s: couldn't find file '%s'", classname(x), s->s_name);
        return;
    }
    sys_close(fd);
    char path[MAXPDSTRING];
    snprintf(path, MAXPDSTRING, "%s/%s", dir, name);
    // sys_bashfilename(path, path);
    if (x->x_plugin->readProgramFile(path)){
        x->x_editor->update();
    } else {
        pd_error(x, "%s: bad FX program file '%s'", classname(x), s->s_name);
    }
}

// write program file (.FXP)
static void vstplugin_program_write(t_vstplugin *x, t_symbol *s){
    if (!x->check_plugin()) return;
    char path[MAXPDSTRING];
    canvas_makefilename(x->x_canvas, s->s_name, path, MAXPDSTRING);
    x->x_plugin->writeProgramFile(path);
}

// set bank data (list of bytes)
static void vstplugin_bank_data_set(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;
    std::string buffer;
    buffer.resize(argc);
    for (int i = 0; i < argc; ++i){
            // first clamp to 0-255, then assign to char (not 100% portable...)
        buffer[i] = (unsigned char)atom_getfloat(argv + i);
    }
    if (x->x_plugin->readBankData(buffer)){
        x->x_editor->update();
    } else {
        pd_error(x, "%s: bad FX bank data", classname(x));
    }
}

// get bank data
static void vstplugin_bank_data_get(t_vstplugin *x){
    if (!x->check_plugin()) return;
    std::string buffer;
    x->x_plugin->writeBankData(buffer);
    int n = buffer.size();
    if (!n){
        pd_error(x, "%s: couldn't get bank data", classname(x));
        return;
    }
    std::vector<t_atom> atoms;
    atoms.resize(n);
    for (int i = 0; i < n; ++i){
            // first convert to range 0-255, then assign to t_float (not 100% portable...)
        SETFLOAT(&atoms[i], (unsigned char)buffer[i]);
    }
    outlet_anything(x->x_messout, gensym("bank_data"), n, atoms.data());
}

// read bank file (.FXB)
static void vstplugin_bank_read(t_vstplugin *x, t_symbol *s){
    if (!x->check_plugin()) return;
    char dir[MAXPDSTRING], *name;
    int fd = canvas_open(x->x_canvas, s->s_name, "", dir, &name, MAXPDSTRING, 1);
    if (fd < 0){
        pd_error(x, "%s: couldn't find file '%s'", classname(x), s->s_name);
        return;
    }
    sys_close(fd);
    char path[MAXPDSTRING];
    snprintf(path, MAXPDSTRING, "%s/%s", dir, name);
    // sys_bashfilename(path, path);
    if (x->x_plugin->readBankFile(path)){
        x->x_editor->update();
    } else {
        pd_error(x, "%s: bad FX bank file '%s'", classname(x), s->s_name);
    }
}

// write bank file (.FXB)
static void vstplugin_bank_write(t_vstplugin *x, t_symbol *s){
    if (!x->check_plugin()) return;
    char path[MAXPDSTRING];
    canvas_makefilename(x->x_canvas, s->s_name, path, MAXPDSTRING);
    x->x_plugin->writeBankFile(path);
}

/*---------------------------- t_vstplugin (internal methods) -------------------------------------*/

static t_class *vstplugin_class;

// automated is true if parameter was set from the (generic) GUI, false if set by message ("param_set")
void t_vstplugin::set_param(int index, float value, bool automated){
    if (index >= 0 && index < x_plugin->getNumParameters()){
        value = std::max(0.f, std::min(1.f, value));
        x_plugin->setParameter(index, value);
        x_editor->param_changed(index, value, automated);
    } else {
        pd_error(this, "%s: parameter index %d out of range!", classname(this), index);
    }
}

void t_vstplugin::set_param(int index, const char *s, bool automated){
    if (index >= 0 && index < x_plugin->getNumParameters()){
        if (!x_plugin->setParameter(index, s)){
            pd_error(this, "%s: bad string value for parameter %d!", classname(this), index);
        }
            // some plugins don't just ignore bad string input but reset the parameter to some value...
        x_editor->param_changed(index, x_plugin->getParameter(index), automated);
    } else {
        pd_error(this, "%s: parameter index %d out of range!", classname(this), index);
    }
}

bool t_vstplugin::check_plugin(){
    if (x_plugin){
        return true;
    } else {
        pd_error(this, "%s: no plugin loaded!", classname(this));
        return false;
    }
}

void t_vstplugin::update_buffer(){
        // this routine is called in the "dsp" method and when a plugin is loaded.
    int nin = x_siginlets.size();
    int nout = x_sigoutlets.size();
    int pin = 0;
    int pout = 0;
    if (x_plugin){
        pin = x_plugin->getNumInputs();
        pout = x_plugin->getNumOutputs();
    }
        // the input/output buffers must be large enough to fit both
        // the number of Pd inlets/outlets and plugin inputs/outputs
    int ninvec = std::max(pin, nin);
    int noutvec = std::max(pout, nout);
        // first clear() so that resize() will zero all values
    x_inbuf.clear();
    x_outbuf.clear();
        // make it large enough for double precision
    x_inbuf.resize(ninvec * sizeof(double) * x_blocksize);
    x_outbuf.resize(noutvec * sizeof(double) * x_blocksize);
    x_invec.resize(ninvec);
    x_outvec.resize(noutvec);
    LOG_DEBUG("vstplugin~: updated buffer");
}

void t_vstplugin::update_precision(){
        // set desired precision
    int dp = x_dp;
        // check precision
    if (x_plugin){
        if (!x_plugin->hasPrecision(VSTProcessPrecision::Single) && !x_plugin->hasPrecision(VSTProcessPrecision::Double)) {
            post("%s: '%s' doesn't support single or double precision, bypassing",
                classname(this), x_plugin->getPluginName().c_str());
            return;
        }
        if (x_dp && !x_plugin->hasPrecision(VSTProcessPrecision::Double)){
            post("%s: '%s' doesn't support double precision, using single precision instead",
                 classname(this), x_plugin->getPluginName().c_str());
            dp = 0;
        }
        else if (!x_dp && !x_plugin->hasPrecision(VSTProcessPrecision::Single)){ // very unlikely...
            post("%s: '%s' doesn't support single precision, using double precision instead",
                 classname(this), x_plugin->getPluginName().c_str());
            dp = 1;
        }
            // set the actual precision
        x_plugin->setPrecision(dp ? VSTProcessPrecision::Double : VSTProcessPrecision::Single);
    }
}

// constructor
// usage: vstplugin~ [flags...] [file] inlets (default=2) outlets (default=2)
t_vstplugin::t_vstplugin(int argc, t_atom *argv){
    bool gui = true; // use GUI?
    bool keep = false; // remember plugin + state?
    int dp = (PD_FLOATSIZE == 64); // use double precision? default to precision of Pd
    t_symbol *file = nullptr; // plugin to open (optional)
    bool editor = false; // open plugin with VST editor?

    while (argc && argv->a_type == A_SYMBOL){
        const char *flag = argv->a_w.w_symbol->s_name;
        if (*flag == '-'){
            if (!strcmp(flag, "-n")){
                gui = false;
            } else if (!strcmp(flag, "-k")){
                keep = true;
            } else if (!strcmp(flag, "-e")){
                editor = true;
            } else if (!strcmp(flag, "-sp")){
                dp = 0;
            } else if (!strcmp(flag, "-dp")){
                dp = 1;
            } else {
                pd_error(this, "%s: unknown flag '%s'", classname(this), flag);
            }
            argc--; argv++;
        } else {
            file = argv->a_w.w_symbol;
            argc--; argv++;
            break;
        }
    }
        // signal inlets (default: 2)
    int in = 2;
    if (argc > 0){
            // min. 1 because of CLASS_MAINSIGNALIN
        in = std::max<int>(1, atom_getfloat(argv));
    }
        // signal outlets (default: 2)
    int out = 2;
    if (argc > 1){
        out = std::max<int>(0, atom_getfloat(argv + 1));
    }

    x_keep = keep;
    x_dp = dp;
    x_canvas = canvas_getcurrent();
    x_editor = std::make_unique<t_vsteditor>(*this, gui);

        // inlets (skip first):
    for (int i = 1; i < in; ++i){
        inlet_new(&x_obj, &x_obj.ob_pd, &s_signal, &s_signal);
	}
    x_siginlets.resize(in);
        // outlets:
	for (int i = 0; i < out; ++i){
        outlet_new(&x_obj, &s_signal);
    }
    x_messout = outlet_new(&x_obj, 0); // additional message outlet
    x_sigoutlets.resize(out);

    if (file){
        t_atom msg[2];
        if (editor){
            SETSYMBOL(&msg[0], gensym("-e"));
            SETSYMBOL(&msg[1], file);
        } else {
            SETSYMBOL(&msg[0], file);
        }
        vstplugin_open(this, 0, (int)editor + 1, msg);
    }
    t_symbol *asym = gensym("#A");
        // bashily unbind #A
    asym->s_thing = 0;
        // now bind #A to us to receive following messages
    pd_bind(&x_obj.ob_pd, asym);
}

static void *vstplugin_new(t_symbol *s, int argc, t_atom *argv){
    auto x = pd_new(vstplugin_class);
        // placement new
    new (x) t_vstplugin(argc, argv);
    return x;
}

// destructor
t_vstplugin::~t_vstplugin(){
    vstplugin_close(this);
    LOG_DEBUG("vstplugin free");
}

static void vstplugin_free(t_vstplugin *x){
    x->~t_vstplugin();
}

// perform routine

// TFloat: processing float type
// this templated method makes some optimization based on whether T and U are equal
template<typename TFloat>
static void vstplugin_doperform(t_vstplugin *x, int n, bool bypass){
    int nin = x->x_siginlets.size();
    t_sample ** sigin = x->x_siginlets.data();
    int nout = x->x_sigoutlets.size();
    t_sample ** sigout = x->x_sigoutlets.data();
    char *inbuf = x->x_inbuf.data();
    int ninvec = x->x_invec.size();
    void ** invec = x->x_invec.data();
    char *outbuf = x->x_outbuf.data();
    int noutvec = x->x_outvec.size();
    void ** outvec = x->x_outvec.data();
    int out_offset = 0;
    auto plugin = x->x_plugin;

    if (!bypass){  // process audio
        int pout = plugin->getNumOutputs();
        out_offset = pout;
            // prepare input buffer + pointers
        for (int i = 0; i < ninvec; ++i){
            TFloat *buf = (TFloat *)inbuf + i * n;
            invec[i] = buf;
            if (i < nin){  // copy from Pd inlets
                t_sample *in = sigin[i];
                for (int j = 0; j < n; ++j){
                    buf[j] = in[j];
                }
            } else if (std::is_same<t_sample, double>::value
                       && std::is_same<TFloat, float>::value){
                    // we only have to zero for this special case: 'bypass' could
                    // have written doubles into the input buffer, leaving garbage in
                    // subsequent channels when the buffer is reinterpreted as floats.
                for (int j = 0; j < n; ++j){
                    buf[j] = 0;
                }
            }
        }
            // set output buffer pointers
        for (int i = 0; i < pout; ++i){
                // if t_sample and TFloat are the same, we can directly write to the outlets.
            if (std::is_same<t_sample, TFloat>::value && i < nout){
                outvec[i] = sigout[i];
            } else {
                outvec[i] = (TFloat *)outbuf + i * n;
            }
        }
            // process
        if (std::is_same<TFloat, float>::value){
            plugin->process((const float **)invec, (float **)outvec, n);
        } else {
            plugin->processDouble((const double **)invec, (double **)outvec, n);
        }

        if (!std::is_same<t_sample, TFloat>::value){
                // copy output buffer to Pd outlets
            for (int i = 0; i < nout && i < pout; ++i){
                t_sample *out = sigout[i];
                double *buf = (double *)outvec[i];
                for (int j = 0; j < n; ++j){
                    out[j] = buf[j];
                }
            }
        }
    } else {  // just pass it through
        t_sample *buf = (t_sample *)inbuf;
        out_offset = nin;
            // copy input
        for (int i = 0; i < nin && i < nout; ++i){
            t_sample *in = sigin[i];
            t_sample *bufptr = buf + i * n;
            for (int j = 0; j < n; ++j){
                bufptr[j] = in[j];
            }
        }
            // write output
        for (int i = 0; i < nin && i < nout; ++i){
            t_sample *out = sigout[i];
            t_sample *bufptr = buf + i * n;
            for (int j = 0; j < n; ++j){
                out[j] = bufptr[j];
            }
        }
    }
        // zero remaining outlets
    for (int i = out_offset; i < nout; ++i){
        t_sample *out = sigout[i];
        for (int j = 0; j < n; ++j){
            out[j] = 0;
        }
    }
}

static t_int *vstplugin_perform(t_int *w){
    t_vstplugin *x = (t_vstplugin *)(w[1]);
    int n = (int)(w[2]);
    auto plugin = x->x_plugin;
    bool dp = x->x_dp;
    bool bypass = plugin ? x->x_bypass : true;

    if (plugin && !bypass) {
            // check processing precision (single or double)
        if (!plugin->hasPrecision(VSTProcessPrecision::Single)
                && !plugin->hasPrecision(VSTProcessPrecision::Double)) { // very unlikely...
            bypass = true;
        } else if (dp && !plugin->hasPrecision(VSTProcessPrecision::Double)){ // possible
            dp = false;
        } else if (!dp && !plugin->hasPrecision(VSTProcessPrecision::Single)){ // pretty unlikely...
            dp = true;
        }
    }
    if (dp){ // double precision
        vstplugin_doperform<double>(x, n, bypass);
    } else { // single precision
        vstplugin_doperform<float>(x, n, bypass);
    }

    return (w+3);
}

// save function
static void vstplugin_save(t_gobj *z, t_binbuf *bb){
    t_vstplugin *x = (t_vstplugin *)z;
    binbuf_addv(bb, "ssff", &s__X, gensym("obj"),
        (float)x->x_obj.te_xpix, (float)x->x_obj.te_ypix);
    binbuf_addbinbuf(bb, x->x_obj.ob_binbuf);
    binbuf_addsemi(bb);
    if (x->x_keep && x->x_plugin){
            // 1) precision
        binbuf_addv(bb, "sss", gensym("#A"), gensym("precision"), gensym(x->x_dp ? "double" : "single"));
        binbuf_addsemi(bb);
            // 2) plugin
        if (x->x_editor->vst_gui()){
            binbuf_addv(bb, "ssss", gensym("#A"), gensym("open"), gensym("-e"), x->x_path);
        } else {
            binbuf_addv(bb, "sss", gensym("#A"), gensym("open"), x->x_path);
        }
        binbuf_addsemi(bb);
            // 3) program number
        binbuf_addv(bb, "ssi", gensym("#A"), gensym("program_set"), x->x_plugin->getProgram());
        binbuf_addsemi(bb);
            // 4) program data
        std::string buffer;
        x->x_plugin->writeProgramData(buffer);
        int n = buffer.size();
        if (n){
            binbuf_addv(bb, "ss", gensym("#A"), gensym("program_data_set"));
            std::vector<t_atom> atoms;
            atoms.resize(n);
            for (int i = 0; i < n; ++i){
                    // first convert to range 0-255, then assign to t_float (not 100% portable...)
                SETFLOAT(&atoms[i], (unsigned char)buffer[i]);
            }
            binbuf_add(bb, n, atoms.data());
            binbuf_addsemi(bb);
        } else {
            pd_error(x, "%s: couldn't save program data", classname(x));
        }
    }
    obj_saveformat(&x->x_obj, bb);
}

// dsp callback
static void vstplugin_dsp(t_vstplugin *x, t_signal **sp){
    int blocksize = sp[0]->s_n;
    t_float sr = sp[0]->s_sr;
    dsp_add(vstplugin_perform, 2, x, blocksize);
    if (blocksize != x->x_blocksize){
        x->x_blocksize = blocksize;
        x->update_buffer();
    }
    x->x_sr = sr;
    if (x->x_plugin){
        x->x_plugin->suspend();
        x->x_plugin->setBlockSize(blocksize);
        x->x_plugin->setSampleRate(sr);
        x->x_plugin->resume();
    }
    int nin = x->x_siginlets.size();
    int nout = x->x_sigoutlets.size();
    for (int i = 0; i < nin; ++i){
        x->x_siginlets[i] = sp[i]->s_vec;
    }
    for (int i = 0; i < nout; ++i){
        x->x_sigoutlets[i] = sp[nin + i]->s_vec;
    }
    // LOG_DEBUG("vstplugin~: got 'dsp' message");
}

// setup function
// this is called by [vstplugin~] which itself is only a stub. this is done to properly share
// the code (and globals!) between [vstplugin~] and [vstsearch]. we can't make a single binary lib
// because [vstsearch] should only be loaded when explicitly requested.
extern "C" {

void vstplugin_setup(void)
{
    vstplugin_class = class_new(gensym("vstplugin~"), (t_newmethod)vstplugin_new, (t_method)vstplugin_free,
        sizeof(t_vstplugin), 0, A_GIMME, A_NULL);
    CLASS_MAINSIGNALIN(vstplugin_class, t_vstplugin, x_f);
    class_setsavefn(vstplugin_class, vstplugin_save);
    class_addmethod(vstplugin_class, (t_method)vstplugin_dsp, gensym("dsp"), A_CANT, A_NULL);
        // plugin
    class_addmethod(vstplugin_class, (t_method)vstplugin_open, gensym("open"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_close, gensym("close"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_search, gensym("search"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_search_clear, gensym("search_clear"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_bypass, gensym("bypass"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_reset, gensym("reset"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_vis, gensym("vis"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_click, gensym("click"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_precision, gensym("precision"), A_SYMBOL, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_info, gensym("info"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_can_do, gensym("can_do"), A_SYMBOL, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_vendor_method, gensym("vendor_method"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_print, gensym("print"), A_NULL);
        // transport
    class_addmethod(vstplugin_class, (t_method)vstplugin_tempo, gensym("tempo"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_time_signature, gensym("time_signature"), A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_play, gensym("play"), A_FLOAT, A_NULL);
#if 0
    class_addmethod(vstplugin_class, (t_method)vstplugin_cycle, gensym("cycle"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_cycle_start, gensym("cycle_start"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_cycle_end, gensym("cycle_end"), A_FLOAT, A_NULL);
#endif
    class_addmethod(vstplugin_class, (t_method)vstplugin_transport_set, gensym("transport_set"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_transport_get, gensym("transport_get"), A_NULL);
        // parameters
    class_addmethod(vstplugin_class, (t_method)vstplugin_param_set, gensym("param_set"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_param_get, gensym("param_get"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_param_info, gensym("param_info"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_param_count, gensym("param_count"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_param_list, gensym("param_list"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_param_dump, gensym("param_dump"), A_NULL);
        // midi
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_raw, gensym("midi_raw"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_note, gensym("midi_note"), A_FLOAT, A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_noteoff, gensym("midi_noteoff"), A_FLOAT, A_FLOAT, A_DEFFLOAT, A_NULL); // third floatarg is optional!
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_cc, gensym("midi_cc"), A_FLOAT, A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_bend, gensym("midi_bend"), A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_program, gensym("midi_program"), A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_polytouch, gensym("midi_polytouch"), A_FLOAT, A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_touch, gensym("midi_touch"), A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_sysex, gensym("midi_sysex"), A_GIMME, A_NULL);
        // programs
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_set, gensym("program_set"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_get, gensym("program_get"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_name_set, gensym("program_name_set"), A_SYMBOL, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_name_get, gensym("program_name_get"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_count, gensym("program_count"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_list, gensym("program_list"), A_NULL);
        // read/write fx programs
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_data_set, gensym("program_data_set"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_data_get, gensym("program_data_get"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_read, gensym("program_read"), A_SYMBOL, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_write, gensym("program_write"), A_SYMBOL, A_NULL);
        // read/write fx banks
    class_addmethod(vstplugin_class, (t_method)vstplugin_bank_data_set, gensym("bank_data_set"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_bank_data_get, gensym("bank_data_get"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_bank_read, gensym("bank_read"), A_SYMBOL, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_bank_write, gensym("bank_write"), A_SYMBOL, A_NULL);

    vstparam_setup();

#if !VSTTHREADS
    mainLoopClock = clock_new(0, (t_method)mainLoopTick);
    clock_delay(mainLoopClock, 0);
#endif
}

} // extern "C"
