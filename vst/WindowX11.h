#pragma once

#include "Interface.h"
#include "Sync.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <unordered_map>

namespace vst {
namespace X11 {

namespace UIThread {

const int updateInterval = 30;

class EventLoop {
 public:
    static EventLoop& instance();

    EventLoop();
    ~EventLoop();

    IPlugin::ptr create(const PluginInfo& info);
    void destroy(IPlugin::ptr plugin);
    bool postClientEvent(::Window window, Atom atom,
                         const char *data = nullptr, size_t size = 0);
    bool sendClientEvent(::Window, Atom atom,
                         const char *data = nullptr, size_t size = 0);
    std::thread::id threadID(){ return thread_.get_id(); }
 private:
    struct PluginData {
        const PluginInfo* info;
        IPlugin::ptr plugin;
        Error err;
    };
    void run();
    void notify();
    void updatePlugins();
    Display *display_ = nullptr;
    ::Window root_;
    std::thread thread_;
    std::mutex mutex_;
    Event event_;
    std::unordered_map<::Window, IPlugin *> pluginMap_;
    std::thread timerThread_;
    std::atomic<bool> timerThreadRunning_{true};
};

} // UIThread

class Window : public IWindow {
 public:
    Window(Display &display, IPlugin& plugin);
    ~Window();

    void* getHandle() override {
        return (void*)window_;
    }

    void setTitle(const std::string& title) override;

    void open() override;
    void close() override;
    void setPos(int x, int y) override;
    void setSize(int w, int h) override;
    void doOpen();
    void doClose();
    void doUpdate();
    void onConfigure(int x, int y, int width, int height);
 private:
    Display *display_;
    IPlugin *plugin_;
    ::Window window_ = 0;
    bool mapped_ = false;
    int x_;
    int y_;
    int width_;
    int height_;
};

} // X11
} // vst
