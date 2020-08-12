#pragma once

#include "Interface.h"
#include "Sync.h"

#include <windows.h>
#include <mutex>

namespace vst {
namespace Win32 {

enum Message {
    WM_CALL = WM_USER + 100,
    WM_OPEN_EDITOR,
    WM_CLOSE_EDITOR,
    WM_EDITOR_POS,
    WM_EDITOR_SIZE
};

class EventLoop {
 public:
    static  const int updateInterval = 30;

    static EventLoop& instance();

    EventLoop();
    ~EventLoop();

    bool postMessage(UINT msg, void *data1 = nullptr, void *data2 = nullptr); // non-blocking
    bool sendMessage(UINT msg, void *data1 = nullptr, void *data2 = nullptr); // blocking

    bool checkThread();
 private:
    static DWORD WINAPI run(void *user);
    LRESULT WINAPI procedure(HWND hWnd, UINT Msg,
                        WPARAM wParam, LPARAM lParam);
    void notify();
    HANDLE thread_;
    DWORD threadID_;
    std::mutex mutex_;
    Event event_;
};

class Window : public IWindow {
 public:
    static LRESULT WINAPI procedure(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);

    Window(IPlugin& plugin);
    ~Window();

    void* getHandle() override {
        return hwnd_;
    }

    void setTitle(const std::string& title);

    void open() override;
    void close() override;
    void setPos(int x, int y) override;
    void setSize(int w, int h) override;
    void update();
    IPlugin* plugin() { return plugin_; }
 private:
    void doOpen();
    void doClose();
    static const UINT_PTR timerID = 0x375067f6;
    static void CALLBACK updateEditor(HWND hwnd, UINT msg, UINT_PTR id, DWORD time);
    HWND hwnd_ = nullptr;
    IPlugin* plugin_ = nullptr;
};

} // Win32
} // vst
