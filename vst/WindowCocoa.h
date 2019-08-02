#include "Interface.h"

#import <Cocoa/Cocoa.h>

@interface CocoaEditorWindow : NSWindow {
    vst::IWindow *_owner;
}

@property (nonatomic, readwrite) vst::IWindow *owner;

- (BOOL)windowShouldClose:(id)sender;
- (void)windowDidMiniaturize:(NSNotification *)notification;
- (void)windowDidDeminiaturize:(NSNotification *)notification;
- (void)windowDidMove:(NSNotification *)notification;

@end

namespace vst {
namespace Cocoa {
    
namespace UIThread {

class EventLoop {
 public:
    static EventLoop& instance();

    EventLoop();
    ~EventLoop();

    IPlugin::ptr create(const PluginInfo& info);
    void destroy(IPlugin::ptr plugin);
#if VSTTHREADS
    bool postMessage();
 private:
    bool haveNSApp_ = false;
#endif
};

} // UIThread

class Window : public IWindow {
 public:
    Window(IPlugin& plugin);
    ~Window();

    void* getHandle() override;

    void setTitle(const std::string& title) override;
    void setGeometry(int left, int top, int right, int bottom) override;

    void show() override;
    void hide() override;
    void minimize() override;
    void restore() override;
    void bringToTop() override;
    
    void doOpen();
    void onClose();
 private:
    CocoaEditorWindow * window_ = nullptr;
    IPlugin * plugin_;
    NSPoint origin_;
};

} // Cocoa
} // vst