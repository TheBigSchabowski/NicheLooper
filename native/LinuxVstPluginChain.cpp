#include "LinuxVstPluginChain.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"

// X11 last: its macros (Status, Bool, None, True, False, Success, ...) clash with the SDK headers.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

using Steinberg::FUnknown;
using Steinberg::IPlugFrame;
using Steinberg::IPlugView;
using Steinberg::IPtr;
using Steinberg::kInvalidArgument;
using Steinberg::kNoInterface;
using Steinberg::kResultFalse;
using Steinberg::kResultOk;
using Steinberg::kResultTrue;
using Steinberg::owned;
using Steinberg::tresult;
using Steinberg::TUID;
using Steinberg::uint32;
using Steinberg::ViewRect;
namespace Vst = Steinberg::Vst;
namespace Hosting = VST3::Hosting;
namespace LinuxVst = Steinberg::Linux;

namespace {

constexpr uint32_t kBankMagic = 0x314B4C4Eu;  // "NLK1", little-endian
constexpr int32_t kBankVersion = 1;

// ---------------------------------------------------------------------------
// Logging and paths
// ---------------------------------------------------------------------------

__attribute__((format(printf, 1, 2))) void logLine(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    std::fprintf(stderr, "%s\n", buffer);
}

bool isDirectory(const std::string& path) {
    struct stat info;
    return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool hasSuffix(const std::string& text, const char* suffix) {
    const size_t length = std::strlen(suffix);
    return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
}

// The last path component that ends in ".vst3" (empty if there is none).
std::string bundleNameIn(const std::string& path) {
    size_t end = path.size();
    while (end > 0) {
        const size_t slash = path.rfind('/', end - 1);
        const size_t start = slash == std::string::npos ? 0 : slash + 1;
        const std::string part = path.substr(start, end - start);
        if (hasSuffix(part, ".vst3")) {
            return part;
        }
        if (slash == std::string::npos) {
            break;
        }
        end = slash;
    }
    return std::string();
}

// Accepts the bundle directory itself or any file/folder inside it (for example the .so).
bool bundleDirectoryFor(const std::string& path, std::string* bundle) {
    std::string current = path;
    while (current.size() > 1 && current.back() == '/') {
        current.pop_back();
    }
    while (!current.empty()) {
        if (hasSuffix(current, ".vst3") && isDirectory(current)) {
            *bundle = current;
            return true;
        }
        const size_t slash = current.find_last_of('/');
        if (slash == std::string::npos || slash == 0) {
            return false;
        }
        current.resize(slash);
    }
    return false;
}

// Bundle directory for a saved or user-picked path; if it vanished, a same-named bundle from
// the standard VST3 folders stands in for it.
std::string resolveBundlePath(const std::string& path) {
    std::string bundle;
    if (bundleDirectoryFor(path, &bundle)) {
        return bundle;
    }
    const std::string wanted = bundleNameIn(path);
    if (!wanted.empty()) {
        for (const std::string& candidate : Hosting::Module::getModulePaths()) {
            const size_t slash = candidate.find_last_of('/');
            const std::string name = slash == std::string::npos ? candidate : candidate.substr(slash + 1);
            if (name == wanted) {
                return candidate;
            }
        }
    }
    return path;
}

// Implemented in the X11 section; the UI loop below needs them.
int xConnectionFd();
void xPumpEvents();
bool xEventsQueued();
void xFlush();

// ---------------------------------------------------------------------------
// Plugin UI thread: job queue, X pump, and the Linux::IRunLoop that plugins use for timers and
// file descriptors. Everything that touches a plugin runs here.
// ---------------------------------------------------------------------------

struct FdEntry {
    const void* owner;
    IPtr<LinuxVst::IEventHandler> handler;
    int fd;
};

struct TimerEntry {
    const void* owner;
    IPtr<LinuxVst::ITimerHandler> handler;
    uint64_t id;
    std::chrono::milliseconds interval;
    std::chrono::steady_clock::time_point next;
};

class UiLoop {
public:
    using Clock = std::chrono::steady_clock;

    static UiLoop& instance() {
        // Leaked on purpose: the thread keeps running through static destruction.
        static UiLoop* loop = new UiLoop();
        return *loop;
    }

    bool onThread() const { return std::this_thread::get_id() == mThreadId; }

    // Runs fn on the UI thread (inline when already there). With wait, returns once fn finished.
    void run(bool wait, std::function<void()> fn) {
        if (onThread()) {
            fn();
            return;
        }
        auto job = std::make_shared<Job>();
        job->fn = std::move(fn);
        {
            std::lock_guard<std::mutex> guard(mMutex);
            mJobs.push_back(job);
        }
        wake();
        if (!wait) {
            return;
        }
        std::unique_lock<std::mutex> guard(mMutex);
        mJobDone.wait(guard, [&job] { return job->done; });
    }

    tresult registerEventHandler(const void* owner, LinuxVst::IEventHandler* handler, int fd) {
        if (handler == nullptr || fd < 0) {
            return kInvalidArgument;
        }
        {
            std::lock_guard<std::mutex> guard(mRegistryMutex);
            for (const FdEntry& entry : mFds) {
                if (entry.handler.get() == handler && entry.fd == fd) {
                    return kInvalidArgument;
                }
            }
            mFds.push_back(FdEntry{owner, IPtr<LinuxVst::IEventHandler>(handler), fd});
        }
        wake();
        return kResultOk;
    }

    // Removes every file descriptor of this handler (JUCE registers one handler for many fds).
    tresult unregisterEventHandler(LinuxVst::IEventHandler* handler) {
        std::vector<FdEntry> removed;
        {
            std::lock_guard<std::mutex> guard(mRegistryMutex);
            for (auto it = mFds.begin(); it != mFds.end();) {
                if (it->handler.get() == handler) {
                    removed.push_back(*it);
                    it = mFds.erase(it);
                } else {
                    ++it;
                }
            }
        }
        wake();
        return removed.empty() ? kResultFalse : kResultTrue;
    }

    tresult registerTimer(const void* owner, LinuxVst::ITimerHandler* handler, LinuxVst::TimerInterval milliseconds) {
        if (handler == nullptr || milliseconds == 0) {
            return kInvalidArgument;
        }
        const std::chrono::milliseconds interval(static_cast<long long>(std::min<uint64_t>(milliseconds, 3600000)));
        {
            std::lock_guard<std::mutex> guard(mRegistryMutex);
            bool replaced = false;
            for (TimerEntry& entry : mTimers) {
                if (entry.handler.get() == handler) {
                    entry.interval = interval;
                    entry.next = Clock::now() + interval;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                mTimers.push_back(TimerEntry{owner, IPtr<LinuxVst::ITimerHandler>(handler), mNextTimerId++, interval,
                                             Clock::now() + interval});
            }
        }
        wake();
        return kResultOk;
    }

    tresult unregisterTimer(LinuxVst::ITimerHandler* handler) {
        std::vector<TimerEntry> removed;
        {
            std::lock_guard<std::mutex> guard(mRegistryMutex);
            for (auto it = mTimers.begin(); it != mTimers.end();) {
                if (it->handler.get() == handler) {
                    removed.push_back(*it);
                    it = mTimers.erase(it);
                } else {
                    ++it;
                }
            }
        }
        wake();
        return removed.empty() ? kResultFalse : kResultTrue;
    }

    // Drops everything a destroyed plugin forgot to unregister.
    void purge(const void* owner) {
        std::vector<FdEntry> removedFds;
        std::vector<TimerEntry> removedTimers;
        {
            std::lock_guard<std::mutex> guard(mRegistryMutex);
            for (auto it = mFds.begin(); it != mFds.end();) {
                if (it->owner == owner) {
                    removedFds.push_back(*it);
                    it = mFds.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = mTimers.begin(); it != mTimers.end();) {
                if (it->owner == owner) {
                    removedTimers.push_back(*it);
                    it = mTimers.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (!removedFds.empty() || !removedTimers.empty()) {
            logLine("NicheLooper: plugin left %zu fd handler(s) and %zu timer(s) registered", removedFds.size(),
                    removedTimers.size());
        }
    }

    void wake() {
        if (mWakeFd < 0) {
            return;
        }
        const uint64_t one = 1;
        if (::write(mWakeFd, &one, sizeof(one)) < 0) {
            // Counter saturated or interrupted: the loop is going to wake up anyway.
        }
    }

private:
    struct Job {
        std::function<void()> fn;
        bool done = false;  // guarded by mMutex
    };

    UiLoop() {
        std::thread([this] { threadMain(); }).detach();
        std::unique_lock<std::mutex> guard(mMutex);
        mStartedCv.wait(guard, [this] { return mStarted; });
    }

    void threadMain();

    void runJobs() {
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::lock_guard<std::mutex> guard(mMutex);
                if (mJobs.empty()) {
                    return;
                }
                job = std::move(mJobs.front());
                mJobs.pop_front();
            }
            try {
                job->fn();
            } catch (...) {
                logLine("NicheLooper: a plugin job threw an exception");
            }
            {
                std::lock_guard<std::mutex> guard(mMutex);
                job->done = true;
            }
            mJobDone.notify_all();
        }
    }

    // Fires due timers; returns the poll timeout in ms until the next one (-1 = none).
    int runTimers() {
        const Clock::time_point now = Clock::now();
        std::vector<uint64_t> due;
        {
            std::lock_guard<std::mutex> guard(mRegistryMutex);
            for (const TimerEntry& entry : mTimers) {
                if (entry.next <= now) {
                    due.push_back(entry.id);
                }
            }
        }
        for (uint64_t id : due) {
            IPtr<LinuxVst::ITimerHandler> handler;
            {
                std::lock_guard<std::mutex> guard(mRegistryMutex);
                auto it = std::find_if(mTimers.begin(), mTimers.end(),
                                       [id](const TimerEntry& entry) { return entry.id == id; });
                if (it == mTimers.end()) {
                    continue;
                }
                it->next = now + it->interval;  // before the callback, which may unregister itself
                handler = it->handler;
            }
            try {
                handler->onTimer();
            } catch (...) {
                logLine("NicheLooper: a plugin timer threw an exception");
            }
        }
        std::lock_guard<std::mutex> guard(mRegistryMutex);
        if (mTimers.empty()) {
            return -1;
        }
        Clock::time_point soonest = mTimers.front().next;
        for (const TimerEntry& entry : mTimers) {
            soonest = std::min(soonest, entry.next);
        }
        const Clock::time_point after = Clock::now();
        if (soonest <= after) {
            return 0;
        }
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(soonest - after).count();
        return static_cast<int>(std::min<long long>((micros + 999) / 1000, 60000));
    }

    bool isRegistered(LinuxVst::IEventHandler* handler, int fd) {
        std::lock_guard<std::mutex> guard(mRegistryMutex);
        for (const FdEntry& entry : mFds) {
            if (entry.handler.get() == handler && entry.fd == fd) {
                return true;
            }
        }
        return false;
    }

    void dropFd(int fd) {
        std::vector<FdEntry> removed;
        {
            std::lock_guard<std::mutex> guard(mRegistryMutex);
            for (auto it = mFds.begin(); it != mFds.end();) {
                if (it->fd == fd) {
                    removed.push_back(*it);
                    it = mFds.erase(it);
                } else {
                    ++it;
                }
            }
        }
        logLine("NicheLooper: dropped %zu handler(s) of invalid fd %d", removed.size(), fd);
    }

    void dispatchPluginFds(const std::vector<pollfd>& fds, size_t first) {
        for (size_t i = first; i < fds.size(); ++i) {
            if (fds[i].revents == 0) {
                continue;
            }
            const int fd = fds[i].fd;
            if ((fds[i].revents & POLLNVAL) != 0) {
                dropFd(fd);
                continue;
            }
            std::vector<IPtr<LinuxVst::IEventHandler>> handlers;
            {
                std::lock_guard<std::mutex> guard(mRegistryMutex);
                for (const FdEntry& entry : mFds) {
                    if (entry.fd == fd) {
                        handlers.push_back(entry.handler);
                    }
                }
            }
            for (IPtr<LinuxVst::IEventHandler>& handler : handlers) {
                if (!isRegistered(handler.get(), fd)) {
                    continue;
                }
                try {
                    handler->onFDIsSet(fd);
                } catch (...) {
                    logLine("NicheLooper: a plugin fd handler threw an exception");
                }
            }
        }
    }

    std::mutex mMutex;  // jobs + startup handshake
    std::condition_variable mStartedCv;
    std::condition_variable mJobDone;
    std::deque<std::shared_ptr<Job>> mJobs;
    bool mStarted = false;
    std::thread::id mThreadId;
    int mWakeFd = -1;

    std::mutex mRegistryMutex;  // fds + timers; never held while calling into a plugin
    std::vector<FdEntry> mFds;
    std::vector<TimerEntry> mTimers;
    uint64_t mNextTimerId = 1;
};

void runOnPluginThread(std::function<void()> fn) {
    UiLoop::instance().run(true, std::move(fn));
}

// Linux::IRunLoop handed to one plugin (through its IPlugFrame and its host context).
class RunLoopFacade final : public LinuxVst::IRunLoop {
public:
    explicit RunLoopFacade(const void* owner) : mOwner(owner) {}

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, LinuxVst::IRunLoop::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1000; }
    uint32 PLUGIN_API release() override { return 1000; }

    tresult PLUGIN_API registerEventHandler(LinuxVst::IEventHandler* handler, LinuxVst::FileDescriptor fd) override {
        return UiLoop::instance().registerEventHandler(mOwner, handler, fd);
    }
    tresult PLUGIN_API unregisterEventHandler(LinuxVst::IEventHandler* handler) override {
        return UiLoop::instance().unregisterEventHandler(handler);
    }
    tresult PLUGIN_API registerTimer(LinuxVst::ITimerHandler* handler, LinuxVst::TimerInterval milliseconds) override {
        return UiLoop::instance().registerTimer(mOwner, handler, milliseconds);
    }
    tresult PLUGIN_API unregisterTimer(LinuxVst::ITimerHandler* handler) override {
        return UiLoop::instance().unregisterTimer(handler);
    }

private:
    const void* mOwner;
};

// IHostApplication that also answers Linux::IRunLoop, as the VST3 docs ask of a Linux host.
class HostContext final : public Vst::HostApplication {
public:
    explicit HostContext(const void* owner) : mRunLoop(owner) {}

    tresult PLUGIN_API getName(Vst::String128 name) override {
        static const char kName[] = "NicheLooper";
        size_t i = 0;
        for (; kName[i] != '\0' && i < 127; ++i) {
            name[i] = static_cast<Vst::TChar>(kName[i]);
        }
        name[i] = 0;
        return kResultTrue;
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, LinuxVst::IRunLoop::iid)) {
            *obj = static_cast<LinuxVst::IRunLoop*>(&mRunLoop);
            return kResultOk;
        }
        return Vst::HostApplication::queryInterface(iid, obj);
    }

private:
    RunLoopFacade mRunLoop;
};

// Context for the factory and the fallback for PluginContextFactory; never destroyed.
HostContext& immortalHostContext() {
    static HostContext* context = new HostContext(nullptr);
    return *context;
}

// ---------------------------------------------------------------------------
// X11: one connection owned by the UI thread
// ---------------------------------------------------------------------------

constexpr long kXEmbedEmbeddedNotify = 0;
constexpr long kXEmbedWindowActivate = 1;
constexpr long kXEmbedWindowDeactivate = 2;
constexpr long kXEmbedRequestFocus = 3;
constexpr long kXEmbedFocusIn = 4;
constexpr long kXEmbedFocusOut = 5;
constexpr long kXEmbedFocusCurrent = 0;
constexpr unsigned long kXEmbedMapped = 1UL << 0;

class EditorWindow;

struct XState {
    Display* display = nullptr;
    Window root = 0;
    Atom xembed = 0;
    Atom xembedInfo = 0;
    Atom wmProtocols = 0;
    Atom wmDeleteWindow = 0;
    Atom netWmName = 0;
    Atom utf8String = 0;
    Atom netWmPid = 0;
    Atom netActiveWindow = 0;
    Atom netClientList = 0;
    std::unordered_map<Window, EditorWindow*> windows;
};

XState& xstate() {
    static XState* state = new XState();  // leaked: the UI thread outlives static destruction
    return *state;
}

Display* gErrorDisplay = nullptr;
int (*gPreviousErrorHandler)(Display*, XErrorEvent*) = nullptr;
int (*gXlibDefaultErrorHandler)(Display*, XErrorEvent*) = nullptr;
std::atomic<int> gForeignErrors{0};

// The error handler is process-wide and AWT has one of its own. Errors of our connection are logged and
// swallowed; errors of other connections go to AWT's handler. Plugins open connections of their own, and Xlib's
// default handler (which is what "previous" is in a process without AWT) would exit the whole process on a
// stale window of such a connection - e.g. when the plugin reparents its window while we tear the editor down.
int hostErrorHandler(Display* display, XErrorEvent* event) {
    char text[128] = {0};
    if (display == gErrorDisplay) {
        XGetErrorText(display, event->error_code, text, sizeof(text));
        logLine("NicheLooper: X error on the plugin connection: %s (request %d.%d, resource 0x%lx)", text,
                static_cast<int>(event->request_code), static_cast<int>(event->minor_code),
                static_cast<unsigned long>(event->resourceid));
        return 0;
    }
    if (gPreviousErrorHandler != nullptr && gPreviousErrorHandler != gXlibDefaultErrorHandler) {
        return gPreviousErrorHandler(display, event);
    }
    if (gForeignErrors.fetch_add(1) < 10) {
        XGetErrorText(display, event->error_code, text, sizeof(text));
        logLine("NicheLooper: ignoring X error on a plugin's own connection: %s (request %d.%d, resource 0x%lx)",
                text, static_cast<int>(event->request_code), static_cast<int>(event->minor_code),
                static_cast<unsigned long>(event->resourceid));
    }
    return 0;
}

bool ensureDisplay() {
    XState& state = xstate();
    if (state.display != nullptr) {
        return true;
    }
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        const char* name = std::getenv("DISPLAY");
        logLine("NicheLooper: no X11 display (DISPLAY=%s) - plugin editors are unavailable",
                name != nullptr ? name : "unset");
        return false;
    }
    state.root = DefaultRootWindow(display);
    state.xembed = XInternAtom(display, "_XEMBED", False);
    state.xembedInfo = XInternAtom(display, "_XEMBED_INFO", False);
    state.wmProtocols = XInternAtom(display, "WM_PROTOCOLS", False);
    state.wmDeleteWindow = XInternAtom(display, "WM_DELETE_WINDOW", False);
    state.netWmName = XInternAtom(display, "_NET_WM_NAME", False);
    state.utf8String = XInternAtom(display, "UTF8_STRING", False);
    state.netWmPid = XInternAtom(display, "_NET_WM_PID", False);
    state.netActiveWindow = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    state.netClientList = XInternAtom(display, "_NET_CLIENT_LIST", False);
    gErrorDisplay = display;
    // XSetErrorHandler(nullptr) installs Xlib's default and returns what was set before; the second call then
    // returns that default, so hostErrorHandler can tell it apart from a real previous handler (AWT's).
    gPreviousErrorHandler = XSetErrorHandler(nullptr);
    gXlibDefaultErrorHandler = XSetErrorHandler(hostErrorHandler);
    state.display = display;
    logLine("NicheLooper: X11 connection for plugin editors open");
    return true;
}

void sendXEmbed(Window target, long message, long detail, long data1, long data2) {
    XState& state = xstate();
    XEvent event;
    std::memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.window = target;
    event.xclient.message_type = state.xembed;
    event.xclient.format = 32;
    event.xclient.data.l[0] = CurrentTime;
    event.xclient.data.l[1] = message;
    event.xclient.data.l[2] = detail;
    event.xclient.data.l[3] = data1;
    event.xclient.data.l[4] = data2;
    XSendEvent(state.display, target, False, NoEventMask, &event);
}

bool readXEmbedInfo(Window window, unsigned long* version, unsigned long* flags) {
    XState& state = xstate();
    Atom type = 0;
    int format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    const int result = XGetWindowProperty(state.display, window, state.xembedInfo, 0, 2, False, state.xembedInfo, &type,
                                          &format, &count, &remaining, &data);
    bool found = false;
    if (result == Success && data != nullptr) {
        // Xlib hands format-32 properties over as an array of unsigned long, even on 64-bit.
        if (type == state.xembedInfo && format == 32 && count >= 2) {
            const unsigned long* values = reinterpret_cast<const unsigned long*>(data);
            *version = values[0];
            *flags = values[1];
            found = true;
        }
        XFree(data);
    }
    return found;
}

bool windowHasPid(Window window, long pid) {
    XState& state = xstate();
    Atom type = 0;
    int format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    bool match = false;
    if (XGetWindowProperty(state.display, window, state.netWmPid, 0, 1, False, XA_CARDINAL, &type, &format, &count,
                           &remaining, &data) == Success &&
        data != nullptr) {
        if (type == XA_CARDINAL && format == 32 && count >= 1) {
            match = static_cast<long>(*reinterpret_cast<const unsigned long*>(data)) == pid;
        }
        XFree(data);
    }
    return match;
}

// The application's own biggest visible top-level window (via the window manager's client list).
Window findApplicationWindow() {
    XState& state = xstate();
    Atom type = 0;
    int format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    std::vector<Window> candidates;
    if (XGetWindowProperty(state.display, state.root, state.netClientList, 0, 4096, False, XA_WINDOW, &type, &format,
                           &count, &remaining, &data) == Success &&
        data != nullptr) {
        if (type == XA_WINDOW && format == 32) {
            const Window* windows = reinterpret_cast<const Window*>(data);
            candidates.assign(windows, windows + count);
        }
        XFree(data);
    }
    const long ownPid = static_cast<long>(::getpid());
    Window best = 0;
    long bestArea = 0;
    for (Window candidate : candidates) {
        if (state.windows.count(candidate) != 0 || !windowHasPid(candidate, ownPid)) {
            continue;
        }
        XWindowAttributes attributes;
        if (XGetWindowAttributes(state.display, candidate, &attributes) == 0 || attributes.map_state != IsViewable ||
            attributes.override_redirect) {
            continue;
        }
        const long area = static_cast<long>(attributes.width) * attributes.height;
        if (area > bestArea) {
            best = candidate;
            bestArea = area;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Editor window: a top-level X window with an XEmbed parent that the plugin view attaches to.
// ---------------------------------------------------------------------------

class EditorWindow final : public IPlugFrame {
public:
    EditorWindow(const void* owner, const IPtr<IPlugView>& view, const std::string& title, bool resizable, int width,
                 int height)
        : mRunLoop(owner), mView(view), mTitle(title), mResizable(resizable), mWidth(width), mHeight(height) {}

    ~EditorWindow() { destroy(); }

    EditorWindow(const EditorWindow&) = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;

    bool createWindows() {
        XState& state = xstate();
        Display* display = state.display;
        const int screen = DefaultScreen(display);
        const unsigned long black = BlackPixel(display, screen);
        mTop = XCreateSimpleWindow(display, state.root, 0, 0, mWidth, mHeight, 0, black, black);
        if (mTop == 0) {
            return false;
        }
        mParent = XCreateSimpleWindow(display, mTop, 0, 0, mWidth, mHeight, 0, black, black);
        if (mParent == 0) {
            XDestroyWindow(display, mTop);
            mTop = 0;
            return false;
        }
        state.windows[mTop] = this;
        state.windows[mParent] = this;
        XSelectInput(display, mTop, StructureNotifyMask | FocusChangeMask);
        XSelectInput(display, mParent, SubstructureNotifyMask | PropertyChangeMask);
        applyWindowProperties();
        XMapWindow(display, mParent);
        return true;
    }

    // Frame first, then attach: the plugin may already ask for its run loop and a resize in attached().
    bool attach() {
        XState& state = xstate();
        XSync(state.display, False);
        mView->setFrame(this);
        mFrameSet = true;
        const tresult result = mView->attached(reinterpret_cast<void*>(static_cast<uintptr_t>(mParent)),
                                               Steinberg::kPlatformTypeX11EmbedWindowID);
        if (result != kResultOk) {
            logLine("NicheLooper: %s attached(X11EmbedWindowID) FAILED (=%d)", mTitle.c_str(),
                    static_cast<int>(result));
            mView->setFrame(nullptr);
            mFrameSet = false;
            return false;
        }
        mAttached = true;
        return true;
    }

    // Maps, raises and asks the window manager to focus the window.
    void show() {
        if (mTop == 0) {
            return;
        }
        XState& state = xstate();
        XMapRaised(state.display, mTop);
        mVisible = true;
        XEvent event;
        std::memset(&event, 0, sizeof(event));
        event.xclient.type = ClientMessage;
        event.xclient.window = mTop;
        event.xclient.message_type = state.netActiveWindow;
        event.xclient.format = 32;
        event.xclient.data.l[0] = 1;  // source indication: application
        event.xclient.data.l[1] = CurrentTime;
        XSendEvent(state.display, state.root, False, SubstructureRedirectMask | SubstructureNotifyMask, &event);
        XFlush(state.display);
    }

    // Closing only hides: the view stays attached until the plugin is removed.
    void hide() {
        if (mTop == 0 || !mVisible) {
            return;
        }
        XState& state = xstate();
        XWithdrawWindow(state.display, mTop, DefaultScreen(state.display));
        mVisible = false;
        XFlush(state.display);
    }

    Window topWindow() const { return mTop; }

    // Order matters: detach the view while its host window still exists, then destroy the windows.
    void destroy() {
        if (mDestroyed) {
            return;
        }
        mDestroying = true;
        XState& state = xstate();
        state.windows.erase(mTop);
        state.windows.erase(mParent);
        if (mPlug != 0) {
            state.windows.erase(mPlug);
        }
        if (mAttached) {
            mView->removed();
            mAttached = false;
        }
        if (mFrameSet) {
            mView->setFrame(nullptr);
            mFrameSet = false;
        }
        if (state.display != nullptr) {
            if (mParent != 0) {
                XDestroyWindow(state.display, mParent);
            }
            if (mTop != 0) {
                XDestroyWindow(state.display, mTop);
            }
            XFlush(state.display);
        }
        mParent = 0;
        mTop = 0;
        mPlug = 0;
        mView = nullptr;
        mDestroyed = true;
    }

    // --- IPlugFrame ---------------------------------------------------------
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, IPlugFrame::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<IPlugFrame*>(this);
            return kResultOk;
        }
        if (Steinberg::FUnknownPrivate::iidEqual(iid, LinuxVst::IRunLoop::iid)) {
            *obj = static_cast<LinuxVst::IRunLoop*>(&mRunLoop);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1000; }
    uint32 PLUGIN_API release() override { return 1000; }

    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) override {
        if (view == nullptr || newSize == nullptr || mDestroying || mDestroyed || mTop == 0) {
            return kInvalidArgument;
        }
        const int width = newSize->getWidth();
        const int height = newSize->getHeight();
        if (width < 1 || height < 1 || width > 16384 || height > 16384) {
            return kInvalidArgument;
        }
        // Remember the size first, so the ConfigureNotify echo of our own resize is ignored.
        mWidth = width;
        mHeight = height;
        updateSizeHints();
        applyGeometry();
        if (!mInOnSize) {
            ViewRect rect(*newSize);
            mInOnSize = true;
            mView->onSize(&rect);
            mInOnSize = false;
        }
        return kResultTrue;
    }

    // --- X events (UI thread) -----------------------------------------------
    void handleEvent(const XEvent& event) {
        if (mDestroying || mDestroyed) {
            return;
        }
        XState& state = xstate();
        switch (event.type) {
            case CreateNotify:
                if (event.xcreatewindow.parent == mParent && !event.xcreatewindow.override_redirect) {
                    adoptPlug(event.xcreatewindow.window);
                }
                break;
            case ReparentNotify:
                if (event.xreparent.parent == mParent && !event.xreparent.override_redirect) {
                    adoptPlug(event.xreparent.window);
                }
                break;
            case DestroyNotify:
                if (event.xdestroywindow.window == mPlug && mPlug != 0) {
                    state.windows.erase(mPlug);
                    mPlug = 0;
                    mPlugXEmbed = false;
                }
                break;
            case ConfigureNotify:
                if (event.xconfigure.window == mTop) {
                    onTopConfigured(event.xconfigure.width, event.xconfigure.height);
                }
                break;
            case FocusIn:
                if (event.xfocus.window == mTop && event.xfocus.detail != NotifyInferior && mPlug != 0 && mPlugXEmbed) {
                    sendXEmbed(mPlug, kXEmbedWindowActivate, 0, 0, 0);
                    sendXEmbed(mPlug, kXEmbedFocusIn, kXEmbedFocusCurrent, 0, 0);
                    XFlush(state.display);
                }
                break;
            case FocusOut:
                if (event.xfocus.window == mTop && event.xfocus.detail != NotifyInferior && mPlug != 0 && mPlugXEmbed) {
                    sendXEmbed(mPlug, kXEmbedFocusOut, 0, 0, 0);
                    sendXEmbed(mPlug, kXEmbedWindowDeactivate, 0, 0, 0);
                    XFlush(state.display);
                }
                break;
            case PropertyNotify:
                if (event.xproperty.window == mPlug && mPlug != 0 && event.xproperty.atom == state.xembedInfo &&
                    event.xproperty.state != PropertyDelete) {
                    unsigned long version = 0;
                    unsigned long flags = 0;
                    if (readXEmbedInfo(mPlug, &version, &flags)) {
                        if ((flags & kXEmbedMapped) != 0) {
                            XMapWindow(state.display, mPlug);
                        } else {
                            XUnmapWindow(state.display, mPlug);
                        }
                        XFlush(state.display);
                    }
                }
                break;
            case ClientMessage:
                if (event.xclient.message_type == state.wmProtocols && event.xclient.format == 32 &&
                    static_cast<Atom>(event.xclient.data.l[0]) == state.wmDeleteWindow) {
                    hide();
                } else if (event.xclient.message_type == state.xembed && event.xclient.window == mParent &&
                           event.xclient.format == 32 && event.xclient.data.l[1] == kXEmbedRequestFocus &&
                           mPlug != 0) {
                    sendXEmbed(mPlug, kXEmbedFocusIn, kXEmbedFocusCurrent, 0, 0);
                    XFlush(state.display);
                }
                break;
            default:
                break;
        }
    }

private:
    void fillSizeHints(XSizeHints* hints) const {
        hints->flags = PSize | PMinSize;
        hints->width = mWidth;
        hints->height = mHeight;
        if (mResizable) {
            hints->min_width = 50;
            hints->min_height = 50;
        } else {
            hints->flags |= PMaxSize;
            hints->min_width = hints->max_width = mWidth;
            hints->min_height = hints->max_height = mHeight;
        }
    }

    // A fixed-size window must be told about every size the plugin picks, or the WM clamps it back.
    void updateSizeHints() {
        XSizeHints* hints = XAllocSizeHints();
        if (hints == nullptr) {
            return;
        }
        fillSizeHints(hints);
        XSetWMNormalHints(xstate().display, mTop, hints);
        XFree(hints);
    }

    void applyWindowProperties() {
        XState& state = xstate();
        Display* display = state.display;

        XSizeHints* hints = XAllocSizeHints();
        XWMHints* wmHints = XAllocWMHints();
        XClassHint* classHint = XAllocClassHint();
        if (hints != nullptr) {
            fillSizeHints(hints);
        }
        if (wmHints != nullptr) {
            wmHints->flags = InputHint;
            wmHints->input = True;
        }
        if (classHint != nullptr) {
            classHint->res_name = const_cast<char*>("nichelooper");
            classHint->res_class = const_cast<char*>("NicheLooper");
        }
        Xutf8SetWMProperties(display, mTop, mTitle.c_str(), mTitle.c_str(), nullptr, 0, hints, wmHints, classHint);
        if (hints != nullptr) {
            XFree(hints);
        }
        if (wmHints != nullptr) {
            XFree(wmHints);
        }
        if (classHint != nullptr) {
            XFree(classHint);
        }

        XChangeProperty(display, mTop, state.netWmName, state.utf8String, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(mTitle.c_str()), static_cast<int>(mTitle.size()));
        const long pid = static_cast<long>(::getpid());
        XChangeProperty(display, mTop, state.netWmPid, XA_CARDINAL, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(&pid), 1);
        Atom protocols[1] = {state.wmDeleteWindow};
        XSetWMProtocols(display, mTop, protocols, 1);

        const Window application = findApplicationWindow();
        if (application != 0) {
            XSetTransientForHint(display, mTop, application);
        }
    }

    // Resizes the three windows to mWidth x mHeight.
    void applyGeometry() {
        XState& state = xstate();
        XResizeWindow(state.display, mTop, static_cast<unsigned>(mWidth), static_cast<unsigned>(mHeight));
        XResizeWindow(state.display, mParent, static_cast<unsigned>(mWidth), static_cast<unsigned>(mHeight));
        if (mPlug != 0) {
            XResizeWindow(state.display, mPlug, static_cast<unsigned>(mWidth), static_cast<unsigned>(mHeight));
        }
        XFlush(state.display);
    }

    // The plugin created (or reparented) its window into our XEmbed parent: embed it.
    void adoptPlug(Window plug) {
        if (mPlug != 0) {
            return;
        }
        XState& state = xstate();
        mPlug = plug;
        state.windows[plug] = this;
        XSelectInput(state.display, plug, PropertyChangeMask);
        unsigned long version = 0;
        unsigned long flags = 0;
        mPlugXEmbed = readXEmbedInfo(plug, &version, &flags);
        if (mPlugXEmbed) {
            sendXEmbed(plug, kXEmbedEmbeddedNotify, 0, static_cast<long>(mParent), 0);
        }
        if (!mPlugXEmbed || (flags & kXEmbedMapped) != 0) {
            XMapWindow(state.display, plug);
        }
        XResizeWindow(state.display, plug, static_cast<unsigned>(mWidth), static_cast<unsigned>(mHeight));
        if (mPlugXEmbed) {
            sendXEmbed(plug, kXEmbedWindowActivate, 0, 0, 0);
            sendXEmbed(plug, kXEmbedFocusIn, kXEmbedFocusCurrent, 0, 0);
        }
        XFlush(state.display);
    }

    // The user (or the window manager) resized the top-level window.
    void onTopConfigured(int width, int height) {
        if (!mResizable || (width == mWidth && height == mHeight) || width < 1 || height < 1) {
            return;
        }
        ViewRect rect(0, 0, width, height);
        if (mView->checkSizeConstraint(&rect) != kResultTrue) {
            mView->getSize(&rect);
        }
        if ((rect.getWidth() != width || rect.getHeight() != height) && mConstraintRetries < 3) {
            // Ask for the size the plugin accepts and wait for the matching ConfigureNotify.
            ++mConstraintRetries;
            XState& state = xstate();
            XResizeWindow(state.display, mTop, static_cast<unsigned>(std::max(rect.getWidth(), 1)),
                          static_cast<unsigned>(std::max(rect.getHeight(), 1)));
            XFlush(state.display);
            return;
        }
        mConstraintRetries = 0;
        mWidth = width;
        mHeight = height;
        applyGeometry();
        ViewRect accepted(0, 0, width, height);
        mInOnSize = true;
        mView->onSize(&accepted);
        mInOnSize = false;
    }

    RunLoopFacade mRunLoop;
    IPtr<IPlugView> mView;
    std::string mTitle;
    bool mResizable;
    int mWidth;
    int mHeight;
    Window mTop = 0;
    Window mParent = 0;
    Window mPlug = 0;
    bool mPlugXEmbed = false;
    bool mFrameSet = false;
    bool mAttached = false;
    bool mVisible = false;
    bool mInOnSize = false;
    bool mDestroying = false;
    bool mDestroyed = false;
    int mConstraintRetries = 0;
};

int xConnectionFd() {
    XState& state = xstate();
    return state.display != nullptr ? ConnectionNumber(state.display) : -1;
}

void xPumpEvents() {
    XState& state = xstate();
    while (state.display != nullptr && XPending(state.display) > 0) {
        XEvent event;
        XNextEvent(state.display, &event);
        auto it = state.windows.find(event.xany.window);
        if (it == state.windows.end()) {
            continue;
        }
        EditorWindow* editor = it->second;
        try {
            editor->handleEvent(event);
        } catch (...) {
            logLine("NicheLooper: an editor event handler threw an exception");
        }
    }
}

bool xEventsQueued() {
    XState& state = xstate();
    return state.display != nullptr && XEventsQueued(state.display, QueuedAlready) > 0;
}

void xFlush() {
    XState& state = xstate();
    if (state.display != nullptr) {
        XFlush(state.display);
    }
}

void UiLoop::threadMain() {
    pthread_setname_np(pthread_self(), "nl-plugin-ui");
    XInitThreads();  // harmless if AWT already did it; must precede every other Xlib call
    mWakeFd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (mWakeFd < 0) {
        logLine("NicheLooper: eventfd failed (errno %d) - the plugin UI thread polls every 20 ms", errno);
    }
    {
        std::lock_guard<std::mutex> guard(mMutex);
        mThreadId = std::this_thread::get_id();
        mStarted = true;
    }
    mStartedCv.notify_all();
    logLine("NicheLooper: plugin UI thread ready");

    std::vector<pollfd> fds;
    for (;;) {
        xPumpEvents();
        runJobs();
        int timeoutMs = runTimers();
        bool jobsPending = false;
        {
            std::lock_guard<std::mutex> guard(mMutex);
            jobsPending = !mJobs.empty();
        }
        if (jobsPending || xEventsQueued()) {
            timeoutMs = 0;
        }
        if (mWakeFd < 0) {
            timeoutMs = timeoutMs < 0 ? 20 : std::min(timeoutMs, 20);
        }
        xFlush();

        fds.clear();
        if (mWakeFd >= 0) {
            fds.push_back(pollfd{mWakeFd, POLLIN, 0});
        }
        const int xFd = xConnectionFd();
        if (xFd >= 0) {
            fds.push_back(pollfd{xFd, POLLIN, 0});
        }
        const size_t firstPluginFd = fds.size();
        {
            std::lock_guard<std::mutex> guard(mRegistryMutex);
            for (const FdEntry& entry : mFds) {
                bool known = false;
                for (size_t i = firstPluginFd; i < fds.size(); ++i) {
                    if (fds[i].fd == entry.fd) {
                        known = true;
                        break;
                    }
                }
                if (!known) {
                    fds.push_back(pollfd{entry.fd, POLLIN, 0});
                }
            }
        }

        const int ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeoutMs);
        if (ready < 0) {
            if (errno != EINTR) {
                logLine("NicheLooper: poll failed (errno %d)", errno);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            continue;
        }
        if (ready == 0) {
            continue;
        }
        if (mWakeFd >= 0 && fds[0].revents != 0) {
            uint64_t counter = 0;
            while (::read(mWakeFd, &counter, sizeof(counter)) > 0) {
            }
        }
        dispatchPluginFds(fds, firstPluginFd);
    }
}

// ---------------------------------------------------------------------------
// Plugin instance
// ---------------------------------------------------------------------------

// Parameter edits from the plugin's own GUI, forwarded to the processor on the next block.
class ComponentHandler final : public Vst::IComponentHandler {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Vst::IComponentHandler::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    // Owned by its VstPlugin, never by the plugin: constant reference counts.
    uint32 PLUGIN_API addRef() override { return 1000; }
    uint32 PLUGIN_API release() override { return 1000; }

    tresult PLUGIN_API beginEdit(Vst::ParamID) override { return kResultOk; }

    tresult PLUGIN_API performEdit(Vst::ParamID id, Vst::ParamValue valueNormalized) override {
        std::lock_guard<std::mutex> guard(mLock);
        mEdits.emplace_back(id, valueNormalized);
        return kResultOk;
    }

    tresult PLUGIN_API endEdit(Vst::ParamID) override { return kResultOk; }

    // Latency/IO changes are ignored: we run a fixed mono/stereo layout at a fixed block size and
    // re-prepare on every engine start.
    tresult PLUGIN_API restartComponent(Steinberg::int32) override { return kResultOk; }

    // Audio thread: never blocks; edits that cannot be taken now are picked up on the next block.
    void drainInto(Vst::ParameterChanges& changes) {
        std::unique_lock<std::mutex> guard(mLock, std::try_to_lock);
        if (!guard.owns_lock()) {
            return;
        }
        for (const auto& edit : mEdits) {
            Steinberg::int32 index = 0;
            if (Vst::IParamValueQueue* queue = changes.addParameterData(edit.first, index)) {
                Steinberg::int32 pointIndex = 0;
                queue->addPoint(0, edit.second, pointIndex);
            }
        }
        mEdits.clear();
    }

private:
    std::mutex mLock;
    std::vector<std::pair<Vst::ParamID, Vst::ParamValue>> mEdits;
};

class VstPlugin {
public:
    VstPlugin(Hosting::Module::Ptr module, Hosting::ClassInfo classInfo, std::string name)
        : mHost(this), mModule(std::move(module)), mClassInfo(std::move(classInfo)), mName(std::move(name)) {}

    // UI thread only.
    ~VstPlugin() {
        closeEditor();
        if (mProcessing && mProcessor != nullptr) {
            mProcessor->setProcessing(false);
            mProcessing = false;
        }
        if (mActive && mComponent != nullptr) {
            mComponent->setActive(false);
            mActive = false;
        }
        mProcessData.unprepare();
        if (mController != nullptr) {
            mController->setComponentHandler(nullptr);
        }
        mProcessor = nullptr;
        mComponent = nullptr;
        mController = nullptr;
        mProvider = nullptr;  // terminates and disconnects component and controller
        UiLoop::instance().purge(this);
        mModule = nullptr;
    }

    VstPlugin(const VstPlugin&) = delete;
    VstPlugin& operator=(const VstPlugin&) = delete;

    const std::string& name() const { return mName; }
    const std::string& modulePath() const { return mModule != nullptr ? mModule->getPath() : mEmptyPath; }
    const VST3::UID& uid() const { return mClassInfo.ID(); }

    bool instantiate() {
        Vst::PluginContextFactory& contexts = Vst::PluginContextFactory::instance();
        contexts.setPluginContext(&mHost);
        mProvider = owned(new Vst::PlugProvider(mModule->getFactory(), mClassInfo, true));
        const bool initialized = mProvider->initialize();
        contexts.setPluginContext(&immortalHostContext());  // the context holds a raw pointer
        if (!initialized) {
            logLine("NicheLooper: PlugProvider init failed for %s", mName.c_str());
            mProvider = nullptr;
            return false;
        }
        mComponent = mProvider->getComponentPtr();
        mController = mProvider->getControllerPtr();
        if (mComponent == nullptr) {
            return false;
        }
        mProcessor = Steinberg::FUnknownPtr<Vst::IAudioProcessor>(mComponent);
        if (mProcessor == nullptr) {
            logLine("NicheLooper: %s has no IAudioProcessor", mName.c_str());
            return false;
        }
        if (mController != nullptr) {
            mController->setComponentHandler(&mHandler);
            Steinberg::MemoryStream stream;
            if (mComponent->getState(&stream) == kResultOk) {
                stream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
                mController->setComponentState(&stream);
            }
        }
        return true;
    }

    // (Re)configures the plugin for a mono or stereo f32 stream and starts processing.
    bool prepare(int sampleRate, int maxFrames) {
        if (mProcessor == nullptr || mComponent == nullptr) {
            return false;
        }
        if (mProcessing) {
            mProcessor->setProcessing(false);
            mProcessing = false;
        }
        if (mActive) {
            mComponent->setActive(false);
            mActive = false;
        }
        mInitialized = false;

        const int numInBuses = mComponent->getBusCount(Vst::kAudio, Vst::kInput);
        const int numOutBuses = mComponent->getBusCount(Vst::kAudio, Vst::kOutput);
        if (numInBuses < 1 || numOutBuses < 1) {
            logLine("NicheLooper: %s has no main audio bus (in=%d out=%d)", mName.c_str(), numInBuses, numOutBuses);
            return false;
        }

        mChannels = 0;
        for (int channels : {1, 2}) {
            const Vst::SpeakerArrangement arrangement =
                channels == 1 ? Vst::SpeakerArr::kMono : Vst::SpeakerArr::kStereo;
            std::vector<Vst::SpeakerArrangement> inArrangements(static_cast<size_t>(numInBuses), Vst::SpeakerArr::kEmpty);
            std::vector<Vst::SpeakerArrangement> outArrangements(static_cast<size_t>(numOutBuses), Vst::SpeakerArr::kEmpty);
            inArrangements[0] = arrangement;
            outArrangements[0] = arrangement;
            if (mProcessor->setBusArrangements(inArrangements.data(), numInBuses, outArrangements.data(),
                                               numOutBuses) == kResultTrue) {
                mChannels = channels;
                break;
            }
        }
        if (mChannels == 0) {
            Vst::SpeakerArrangement actual = 0;
            if (mProcessor->getBusArrangement(Vst::kOutput, 0, actual) == kResultTrue) {
                const int channels = Vst::SpeakerArr::getChannelCount(actual);
                if (channels == 1 || channels == 2) {
                    mChannels = channels;
                }
            }
        }
        if (mChannels == 0) {
            logLine("NicheLooper: %s accepts neither mono nor stereo f32", mName.c_str());
            return false;
        }

        for (int i = 0; i < numInBuses; ++i) {
            mComponent->activateBus(Vst::kAudio, Vst::kInput, i, i == 0);
        }
        for (int i = 0; i < numOutBuses; ++i) {
            mComponent->activateBus(Vst::kAudio, Vst::kOutput, i, i == 0);
        }
        const int numEventIn = mComponent->getBusCount(Vst::kEvent, Vst::kInput);
        const int numEventOut = mComponent->getBusCount(Vst::kEvent, Vst::kOutput);
        for (int i = 0; i < numEventIn; ++i) {
            mComponent->activateBus(Vst::kEvent, Vst::kInput, i, false);
        }
        for (int i = 0; i < numEventOut; ++i) {
            mComponent->activateBus(Vst::kEvent, Vst::kOutput, i, false);
        }

        Vst::ProcessSetup setup{};
        setup.processMode = Vst::kRealtime;
        setup.symbolicSampleSize = Vst::kSample32;
        setup.maxSamplesPerBlock = maxFrames;
        setup.sampleRate = static_cast<Vst::SampleRate>(sampleRate);
        if (mProcessor->setupProcessing(setup) != kResultOk) {
            logLine("NicheLooper: setupProcessing failed for %s", mName.c_str());
            return false;
        }

        mProcessData.unprepare();
        if (!mProcessData.prepare(*mComponent, maxFrames, Vst::kSample32)) {
            logLine("NicheLooper: process buffers failed for %s", mName.c_str());
            return false;
        }
        mInputChanges.setMaxParameters(kMaxParameterEdits);
        mOutputChanges.setMaxParameters(kMaxParameterEdits);
        mProcessData.inputParameterChanges = &mInputChanges;
        mProcessData.outputParameterChanges = &mOutputChanges;

        std::memset(&mProcessContext, 0, sizeof(mProcessContext));
        mProcessContext.state =
            Vst::ProcessContext::kPlaying | Vst::ProcessContext::kTempoValid | Vst::ProcessContext::kTimeSigValid;
        mProcessContext.sampleRate = sampleRate;
        mProcessContext.tempo = 120.0;
        mProcessContext.timeSigNumerator = 4;
        mProcessContext.timeSigDenominator = 4;
        mProcessData.processContext = &mProcessContext;

        mMaxFrames = maxFrames;
        mSampleTime = 0;

        if (mComponent->setActive(true) != kResultOk) {
            logLine("NicheLooper: setActive failed for %s", mName.c_str());
            return false;
        }
        mActive = true;
        mProcessor->setProcessing(true);  // optional per spec, so the result is ignored
        mProcessing = true;
        mInitialized = true;
        return true;
    }

    // Audio thread: processes the mono buffer in place; on any problem the block stays dry.
    void process(float* mono, int frames) {
        if (!mInitialized || frames > mMaxFrames || mProcessData.numOutputs < 1 ||
            mProcessData.outputs[0].numChannels < 1) {
            return;
        }
        const size_t bytes = static_cast<size_t>(frames) * sizeof(float);

        mInputChanges.clearQueue();
        mOutputChanges.clearQueue();
        mHandler.drainInto(mInputChanges);

        if (mProcessData.numInputs > 0) {
            Vst::AudioBusBuffers& in = mProcessData.inputs[0];
            for (int c = 0; c < in.numChannels && c < 2; ++c) {
                if (in.channelBuffers32[c] != nullptr) {
                    std::memcpy(in.channelBuffers32[c], mono, bytes);
                }
            }
            in.silenceFlags = 0;
        }
        mProcessData.numSamples = frames;
        mProcessContext.projectTimeSamples = mSampleTime;

        const tresult result = mProcessor->process(mProcessData);
        mSampleTime += frames;
        if (result != kResultOk) {
            mLastError.store(static_cast<int32_t>(result), std::memory_order_relaxed);
            if (mErrorCount.fetch_add(1, std::memory_order_relaxed) < 3) {
                logLine("NicheLooper: process(%s) failed: %d", mName.c_str(), static_cast<int>(result));
            }
            return;
        }
        mRenderCount.fetch_add(1, std::memory_order_relaxed);

        Vst::AudioBusBuffers& out = mProcessData.outputs[0];
        if (mChannels >= 2 && out.numChannels >= 2 && out.channelBuffers32[0] != nullptr &&
            out.channelBuffers32[1] != nullptr) {
            const float* left = out.channelBuffers32[0];
            const float* right = out.channelBuffers32[1];
            for (int i = 0; i < frames; ++i) {
                mono[i] = 0.5f * (left[i] + right[i]);
            }
        } else if (out.channelBuffers32[0] != nullptr) {
            std::memcpy(mono, out.channelBuffers32[0], bytes);
        }
    }

    // Any thread; the work happens on the UI thread.
    void openEditor() {
        if (mController == nullptr) {
            logLine("NicheLooper: %s has no edit controller", mName.c_str());
            return;
        }
        UiLoop::instance().run(false, [this] { openEditorOnUiThread(); });
    }

    // Complete component + controller state for the bank. UI thread.
    bool captureState(std::vector<uint8_t>& componentState, std::vector<uint8_t>& controllerState) const {
        componentState.clear();
        controllerState.clear();
        if (mComponent == nullptr) {
            return false;
        }
        Steinberg::MemoryStream compStream;
        if (mComponent->getState(&compStream) == kResultOk && compStream.getSize() > 0) {
            const uint8_t* data = reinterpret_cast<const uint8_t*>(compStream.getData());
            componentState.assign(data, data + compStream.getSize());
        }
        if (mController != nullptr) {
            Steinberg::MemoryStream ctrlStream;
            if (mController->getState(&ctrlStream) == kResultOk && ctrlStream.getSize() > 0) {
                const uint8_t* data = reinterpret_cast<const uint8_t*>(ctrlStream.getData());
                controllerState.assign(data, data + ctrlStream.getSize());
            }
        }
        return true;
    }

    bool restoreState(const std::vector<uint8_t>& componentState, const std::vector<uint8_t>& controllerState) {
        if (mComponent == nullptr || componentState.empty()) {
            return false;
        }
        Steinberg::MemoryStream compStream(const_cast<uint8_t*>(componentState.data()),
                                           static_cast<Steinberg::TSize>(componentState.size()));
        mComponent->setState(&compStream);  // some plugins return kResultFalse yet apply the state
        if (mController != nullptr) {
            compStream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
            mController->setComponentState(&compStream);
            if (!controllerState.empty()) {
                Steinberg::MemoryStream ctrlStream(const_cast<uint8_t*>(controllerState.data()),
                                                   static_cast<Steinberg::TSize>(controllerState.size()));
                mController->setState(&ctrlStream);
            }
        }
        return true;
    }

    uint32_t renderCount() const { return mRenderCount.load(std::memory_order_relaxed); }
    uint32_t errorCount() const { return mErrorCount.load(std::memory_order_relaxed); }
    int32_t lastError() const { return mLastError.load(std::memory_order_relaxed); }
    int channels() const { return mChannels; }
    bool initialized() const { return mInitialized; }

private:
    static constexpr Steinberg::int32 kMaxParameterEdits = 64;

    void openEditorOnUiThread() {
        logLine("NicheLooper: openEditor job for %s (existing=%d)", mName.c_str(), mEditor != nullptr ? 1 : 0);
        if (mEditor != nullptr) {
            logLine("NicheLooper: %s refocus existing editor", mName.c_str());
            mEditor->show();
            return;
        }
        if (!ensureDisplay()) {
            return;
        }
        IPlugView* rawView = mController->createView(Vst::ViewType::kEditor);
        if (rawView == nullptr) {
            logLine("NicheLooper: %s createView(kEditor) returned null - no GUI", mName.c_str());
            return;
        }
        IPtr<IPlugView> view = owned(rawView);
        if (view->isPlatformTypeSupported(Steinberg::kPlatformTypeX11EmbedWindowID) != kResultTrue) {
            logLine("NicheLooper: %s has no X11 embedding editor", mName.c_str());
            return;
        }

        ViewRect size{};
        const tresult getSizeResult = view->getSize(&size);
        logLine("NicheLooper: %s getSize=%d %dx%d canResize=%d", mName.c_str(), static_cast<int>(getSizeResult),
                size.getWidth(), size.getHeight(), static_cast<int>(view->canResize()));
        if (getSizeResult != kResultOk || size.getWidth() < 50 || size.getHeight() < 50) {
            size = ViewRect(0, 0, 900, 600);
        }
        const bool resizable = view->canResize() == kResultTrue;

        std::unique_ptr<EditorWindow> editor(
            new EditorWindow(this, view, mName, resizable, size.getWidth(), size.getHeight()));
        if (!editor->createWindows()) {
            logLine("NicheLooper: %s editor window could not be created", mName.c_str());
            return;
        }
        if (!editor->attach()) {
            editor->destroy();
            return;
        }
        editor->show();
        logLine("NicheLooper: editor window open for %s (%dx%d, resizable=%d, window=0x%lx)", mName.c_str(),
                size.getWidth(), size.getHeight(), resizable ? 1 : 0, static_cast<unsigned long>(editor->topWindow()));
        mEditor = editor.release();
    }

    // Always a synchronous UI job: a pending openEditor job may still sit in the queue ahead of
    // it, and FIFO order makes it run first while this object is alive.
    void closeEditor() {
        UiLoop::instance().run(true, [this] {
            EditorWindow* editor = mEditor;
            mEditor = nullptr;
            if (editor != nullptr) {
                editor->destroy();
                delete editor;
            }
        });
    }

    HostContext mHost;  // outlives provider/component, which hold pointers into it
    Hosting::Module::Ptr mModule;
    Hosting::ClassInfo mClassInfo;
    std::string mName;
    std::string mEmptyPath;
    IPtr<Vst::PlugProvider> mProvider;
    IPtr<Vst::IComponent> mComponent;
    IPtr<Vst::IAudioProcessor> mProcessor;
    IPtr<Vst::IEditController> mController;
    ComponentHandler mHandler;
    Vst::HostProcessData mProcessData;
    Vst::ParameterChanges mInputChanges;
    Vst::ParameterChanges mOutputChanges;
    Vst::ProcessContext mProcessContext{};
    EditorWindow* mEditor = nullptr;
    int mChannels = 0;
    int mMaxFrames = 0;
    Steinberg::int64 mSampleTime = 0;
    bool mActive = false;
    bool mProcessing = false;
    bool mInitialized = false;
    std::atomic<uint32_t> mRenderCount{0};
    std::atomic<uint32_t> mErrorCount{0};
    std::atomic<int32_t> mLastError{0};
};

// ---------------------------------------------------------------------------
// Scanning and loading (UI thread)
// ---------------------------------------------------------------------------

struct AvailablePlugin {
    std::string displayName;  // "Vendor: Name"
    std::string path;         // bundle directory
    VST3::UID uid;
};

// Modules stay loaded for the life of the process: unloading plugin libraries (ModuleExit +
// dlclose) is a known crash source, and a rescan then costs almost nothing.
Hosting::Module::Ptr loadModule(const std::string& bundlePath, std::string& error) {
    static auto* cache = new std::map<std::string, Hosting::Module::Ptr>();
    auto it = cache->find(bundlePath);
    if (it != cache->end()) {
        return it->second;
    }
    Hosting::Module::Ptr module = Hosting::Module::create(bundlePath, error);
    if (module == nullptr) {
        return nullptr;
    }
    module->getFactory().setHostContext(&immortalHostContext());
    cache->emplace(bundlePath, module);
    return module;
}

std::string displayNameFor(const Hosting::ClassInfo& classInfo) {
    return classInfo.vendor().empty() ? classInfo.name() : classInfo.vendor() + ": " + classInfo.name();
}

bool isUsableEffect(const Hosting::ClassInfo& classInfo) {
    if (classInfo.category() != kVstAudioEffectClass) {
        return false;
    }
    return classInfo.subCategoriesString().find("Instrument") == std::string::npos;
}

std::vector<AvailablePlugin> scanInstalledPlugins() {
    std::vector<AvailablePlugin> found;
    std::set<std::string> seen;
    for (const std::string& modulePath : Hosting::Module::getModulePaths()) {
        std::string error;
        Hosting::Module::Ptr module = loadModule(modulePath, error);
        if (module == nullptr) {
            logLine("NicheLooper: skipping %s: %s", modulePath.c_str(), error.c_str());
            continue;
        }
        for (const Hosting::ClassInfo& classInfo : module->getFactory().classInfos()) {
            if (!isUsableEffect(classInfo)) {
                continue;
            }
            if (!seen.insert(classInfo.ID().toString()).second) {
                continue;  // same plugin reachable through several folders or symlinks
            }
            found.push_back(AvailablePlugin{displayNameFor(classInfo), modulePath, classInfo.ID()});
        }
    }
    std::sort(found.begin(), found.end(), [](const AvailablePlugin& a, const AvailablePlugin& b) {
        return a.displayName < b.displayName;
    });
    return found;
}

std::unique_ptr<VstPlugin> buildPlugin(const std::string& path, const VST3::UID& uid, const std::string& displayName) {
    const std::string bundle = resolveBundlePath(path);
    std::string error;
    Hosting::Module::Ptr module = loadModule(bundle, error);
    if (module == nullptr) {
        logLine("NicheLooper: module load failed (%s): %s", bundle.c_str(), error.c_str());
        return nullptr;
    }
    for (const Hosting::ClassInfo& classInfo : module->getFactory().classInfos()) {
        if (classInfo.category() != kVstAudioEffectClass || !(classInfo.ID() == uid)) {
            continue;
        }
        auto plugin = std::make_unique<VstPlugin>(module, classInfo, displayName);
        if (!plugin->instantiate()) {
            return nullptr;
        }
        return plugin;
    }
    logLine("NicheLooper: class not found in %s", bundle.c_str());
    return nullptr;
}

std::unique_ptr<VstPlugin> buildFirstEffectFromPath(const std::string& path) {
    const std::string bundle = resolveBundlePath(path);
    std::string error;
    Hosting::Module::Ptr module = loadModule(bundle, error);
    if (module == nullptr) {
        logLine("NicheLooper: add-from-path load failed (%s): %s", bundle.c_str(), error.c_str());
        return nullptr;
    }
    for (const Hosting::ClassInfo& classInfo : module->getFactory().classInfos()) {
        if (!isUsableEffect(classInfo)) {
            continue;
        }
        auto plugin = std::make_unique<VstPlugin>(module, classInfo, displayNameFor(classInfo));
        if (!plugin->instantiate()) {
            return nullptr;
        }
        return plugin;
    }
    logLine("NicheLooper: no audio-effect class in %s", bundle.c_str());
    return nullptr;
}

// Loads (by class uid, or the first effect when uid is null) and prepares on the UI thread.
std::unique_ptr<VstPlugin> buildAndPrepare(const std::string& path, const VST3::UID* uid,
                                           const std::string& displayName, int sampleRate, int maxFrames) {
    std::unique_ptr<VstPlugin> plugin;
    runOnPluginThread([&] {
        plugin = uid != nullptr ? buildPlugin(path, *uid, displayName) : buildFirstEffectFromPath(path);
        if (plugin != nullptr && !plugin->prepare(sampleRate, maxFrames)) {
            plugin.reset();
        }
    });
    return plugin;
}

void destroyOnPluginThread(std::unique_ptr<VstPlugin> plugin) {
    if (plugin == nullptr) {
        return;
    }
    runOnPluginThread([&plugin] { plugin.reset(); });
}

void destroyOnPluginThread(std::vector<std::unique_ptr<VstPlugin>> plugins) {
    if (plugins.empty()) {
        return;
    }
    runOnPluginThread([&plugins] { plugins.clear(); });
}

// ---------------------------------------------------------------------------
// Bank serialization (little-endian)
// ---------------------------------------------------------------------------

void putInt(std::vector<uint8_t>& out, int32_t value) {
    const uint32_t bits = static_cast<uint32_t>(value);
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((bits >> (8 * i)) & 0xFFu));
    }
}

void putBlob(std::vector<uint8_t>& out, const void* data, size_t size) {
    putInt(out, static_cast<int32_t>(size));
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    out.insert(out.end(), bytes, bytes + size);
}

struct BankReader {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;

    bool readInt(int32_t* value) {
        if (size - pos < 4) {
            return false;
        }
        uint32_t bits = 0;
        for (int i = 0; i < 4; ++i) {
            bits |= static_cast<uint32_t>(data[pos + static_cast<size_t>(i)]) << (8 * i);
        }
        pos += 4;
        *value = static_cast<int32_t>(bits);
        return true;
    }

    bool readBlob(std::vector<uint8_t>* blob) {
        int32_t length = 0;
        if (!readInt(&length) || length < 0 || static_cast<size_t>(length) > size - pos) {
            return false;
        }
        blob->assign(data + pos, data + pos + length);
        pos += static_cast<size_t>(length);
        return true;
    }

    bool readString(std::string* text) {
        std::vector<uint8_t> blob;
        if (!readBlob(&blob)) {
            return false;
        }
        text->assign(blob.begin(), blob.end());
        return true;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// PluginChainManager
// ---------------------------------------------------------------------------

struct PluginChainManager::Impl {
    std::mutex lock;
    std::vector<AvailablePlugin> available;
    std::vector<std::unique_ptr<VstPlugin>> chains[kNumChains];
    std::atomic<int> active{0};
    int sampleRate = 48000;
    int maxFrames = 8192;
    std::atomic<uint32_t> blocksProcessed{0};
    std::atomic<uint32_t> blocksSkippedLock{0};
};

PluginChainManager::PluginChainManager() : mImpl(std::make_unique<Impl>()) {}

PluginChainManager::~PluginChainManager() {
    for (auto& chain : mImpl->chains) {
        destroyOnPluginThread(std::move(chain));
    }
}

std::vector<std::string> PluginChainManager::availablePluginNames() {
    std::vector<AvailablePlugin> scanned;
    runOnPluginThread([&scanned] { scanned = scanInstalledPlugins(); });
    std::vector<std::string> names;
    names.reserve(scanned.size());
    for (const AvailablePlugin& plugin : scanned) {
        names.push_back(plugin.displayName);
    }
    std::lock_guard<std::mutex> guard(mImpl->lock);
    mImpl->available = std::move(scanned);
    return names;
}

bool PluginChainManager::addPlugin(int chain, int pluginIndex) {
    if (chain < 0 || chain >= kNumChains) {
        return false;
    }
    AvailablePlugin spec;
    int sampleRate = 0;
    int maxFrames = 0;
    {
        std::lock_guard<std::mutex> guard(mImpl->lock);
        if (pluginIndex < 0 || pluginIndex >= static_cast<int>(mImpl->available.size())) {
            return false;
        }
        spec = mImpl->available[static_cast<size_t>(pluginIndex)];
        sampleRate = mImpl->sampleRate;
        maxFrames = mImpl->maxFrames;
    }
    // Slow, so outside the lock: the audio thread keeps passing through meanwhile.
    std::unique_ptr<VstPlugin> plugin = buildAndPrepare(spec.path, &spec.uid, spec.displayName, sampleRate, maxFrames);
    if (plugin == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> guard(mImpl->lock);
    mImpl->chains[chain].push_back(std::move(plugin));
    return true;
}

bool PluginChainManager::addPluginFromPath(int chain, const std::string& path) {
    if (chain < 0 || chain >= kNumChains) {
        return false;
    }
    int sampleRate = 0;
    int maxFrames = 0;
    {
        std::lock_guard<std::mutex> guard(mImpl->lock);
        sampleRate = mImpl->sampleRate;
        maxFrames = mImpl->maxFrames;
    }
    std::unique_ptr<VstPlugin> plugin = buildAndPrepare(path, nullptr, std::string(), sampleRate, maxFrames);
    if (plugin == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> guard(mImpl->lock);
    mImpl->chains[chain].push_back(std::move(plugin));
    return true;
}

bool PluginChainManager::removePlugin(int chain, int slot) {
    if (chain < 0 || chain >= kNumChains) {
        return false;
    }
    std::unique_ptr<VstPlugin> removed;
    {
        std::lock_guard<std::mutex> guard(mImpl->lock);
        auto& plugins = mImpl->chains[chain];
        if (slot < 0 || slot >= static_cast<int>(plugins.size())) {
            return false;
        }
        removed = std::move(plugins[static_cast<size_t>(slot)]);
        plugins.erase(plugins.begin() + slot);
    }
    destroyOnPluginThread(std::move(removed));
    return true;
}

bool PluginChainManager::movePlugin(int chain, int from, int to) {
    if (chain < 0 || chain >= kNumChains) {
        return false;
    }
    std::lock_guard<std::mutex> guard(mImpl->lock);
    auto& plugins = mImpl->chains[chain];
    const int count = static_cast<int>(plugins.size());
    if (from < 0 || from >= count || to < 0 || to >= count || from == to) {
        return false;
    }
    std::swap(plugins[static_cast<size_t>(from)], plugins[static_cast<size_t>(to)]);
    return true;
}

std::vector<std::string> PluginChainManager::chainPluginNames(int chain) {
    std::vector<std::string> names;
    if (chain < 0 || chain >= kNumChains) {
        return names;
    }
    std::lock_guard<std::mutex> guard(mImpl->lock);
    for (const auto& plugin : mImpl->chains[chain]) {
        names.push_back(plugin->name());
    }
    return names;
}

void PluginChainManager::openEditor(int chain, int slot) {
    if (chain < 0 || chain >= kNumChains) {
        return;
    }
    std::lock_guard<std::mutex> guard(mImpl->lock);
    auto& plugins = mImpl->chains[chain];
    if (slot < 0 || slot >= static_cast<int>(plugins.size())) {
        return;
    }
    plugins[static_cast<size_t>(slot)]->openEditor();
}

void PluginChainManager::setActiveChain(int index) {
    if (index < 0 || index >= kNumChains) {
        return;
    }
    mImpl->active.store(index, std::memory_order_relaxed);
}

int PluginChainManager::activeChain() const {
    return mImpl->active.load(std::memory_order_relaxed);
}

void PluginChainManager::prepare(int sampleRate, int maxFrames) {
    std::lock_guard<std::mutex> guard(mImpl->lock);
    const bool changed = sampleRate != mImpl->sampleRate || maxFrames != mImpl->maxFrames;
    mImpl->sampleRate = sampleRate;
    mImpl->maxFrames = maxFrames;
    if (!changed) {
        return;
    }
    bool anyPlugin = false;
    for (const auto& chain : mImpl->chains) {
        anyPlugin = anyPlugin || !chain.empty();
    }
    if (!anyPlugin) {
        return;
    }
    runOnPluginThread([this, sampleRate, maxFrames] {
        for (auto& chain : mImpl->chains) {
            for (auto& plugin : chain) {
                plugin->prepare(sampleRate, maxFrames);
            }
        }
    });
}

void PluginChainManager::processBlock(float* mono, int frames) {
    std::unique_lock<std::mutex> guard(mImpl->lock, std::try_to_lock);
    if (!guard.owns_lock()) {
        mImpl->blocksSkippedLock.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto& plugins = mImpl->chains[mImpl->active.load(std::memory_order_relaxed)];
    if (!plugins.empty()) {
        mImpl->blocksProcessed.fetch_add(1, std::memory_order_relaxed);
    }
    for (auto& plugin : plugins) {
        plugin->process(mono, frames);
    }
}

std::string PluginChainManager::debugReport() {
    std::lock_guard<std::mutex> guard(mImpl->lock);
    char line[512];
    std::snprintf(line, sizeof(line), "active=%d rate=%d blocks=%u lockSkips=%u\n",
                  mImpl->active.load(std::memory_order_relaxed), mImpl->sampleRate,
                  mImpl->blocksProcessed.load(std::memory_order_relaxed),
                  mImpl->blocksSkippedLock.load(std::memory_order_relaxed));
    std::string report = line;
    for (int c = 0; c < kNumChains; ++c) {
        const auto& plugins = mImpl->chains[c];
        for (size_t i = 0; i < plugins.size(); ++i) {
            const VstPlugin& plugin = *plugins[i];
            std::snprintf(line, sizeof(line), "chain%d[%zu] %s ch=%d init=%d renders=%u errors=%u lastErr=%d\n", c, i,
                          plugin.name().c_str(), plugin.channels(), plugin.initialized() ? 1 : 0,
                          plugin.renderCount(), plugin.errorCount(), static_cast<int>(plugin.lastError()));
            report += line;
        }
    }
    return report;
}

std::vector<uint8_t> PluginChainManager::saveBank() {
    std::vector<uint8_t> out;
    std::lock_guard<std::mutex> guard(mImpl->lock);
    putInt(out, static_cast<int32_t>(kBankMagic));
    putInt(out, kBankVersion);
    putInt(out, mImpl->active.load(std::memory_order_relaxed));
    runOnPluginThread([this, &out] {
        for (const auto& chain : mImpl->chains) {
            putInt(out, static_cast<int32_t>(chain.size()));
            for (const auto& plugin : chain) {
                std::vector<uint8_t> componentState;
                std::vector<uint8_t> controllerState;
                plugin->captureState(componentState, controllerState);
                const std::string uid = plugin->uid().toString();
                putBlob(out, plugin->name().data(), plugin->name().size());
                putBlob(out, plugin->modulePath().data(), plugin->modulePath().size());
                putBlob(out, uid.data(), uid.size());
                putBlob(out, componentState.data(), componentState.size());
                putBlob(out, controllerState.data(), controllerState.size());
            }
        }
    });
    return out;
}

bool PluginChainManager::loadBank(const uint8_t* data, size_t size) {
    struct PluginSpec {
        std::string name;
        std::string path;
        std::string uid;
        std::vector<uint8_t> componentState;
        std::vector<uint8_t> controllerState;
    };

    if (data == nullptr || size < 12) {
        return false;
    }
    BankReader reader{data, size};
    int32_t magic = 0;
    int32_t version = 0;
    int32_t active = 0;
    if (!reader.readInt(&magic) || magic != static_cast<int32_t>(kBankMagic) || !reader.readInt(&version) ||
        version != kBankVersion || !reader.readInt(&active)) {
        return false;
    }
    if (active < 0 || active >= kNumChains) {
        active = 0;
    }

    std::array<std::vector<PluginSpec>, kNumChains> specs;
    for (auto& chainSpecs : specs) {
        int32_t count = 0;
        if (!reader.readInt(&count) || count < 0 || count > 64) {
            return false;
        }
        for (int32_t i = 0; i < count; ++i) {
            PluginSpec spec;
            if (!reader.readString(&spec.name) || !reader.readString(&spec.path) || !reader.readString(&spec.uid) ||
                !reader.readBlob(&spec.componentState) || !reader.readBlob(&spec.controllerState)) {
                return false;
            }
            chainSpecs.push_back(std::move(spec));
        }
    }

    int sampleRate = 0;
    int maxFrames = 0;
    {
        std::lock_guard<std::mutex> guard(mImpl->lock);
        sampleRate = mImpl->sampleRate;
        maxFrames = mImpl->maxFrames;
    }
    if (sampleRate <= 0) {
        logLine("NicheLooper: loadBank without a running engine (rate=%d)", sampleRate);
        return false;
    }

    // One job for the whole bank: every plugin is created, prepared and restored on the UI thread.
    std::array<std::vector<std::unique_ptr<VstPlugin>>, kNumChains> loaded;
    runOnPluginThread([&] {
        for (int c = 0; c < kNumChains; ++c) {
            for (const PluginSpec& spec : specs[static_cast<size_t>(c)]) {
                const auto uid = VST3::UID::fromString(spec.uid);
                if (!uid) {
                    logLine("NicheLooper: bad plugin id for %s", spec.name.c_str());
                    continue;
                }
                std::unique_ptr<VstPlugin> plugin = buildPlugin(spec.path, *uid, spec.name);
                if (plugin == nullptr) {
                    continue;
                }
                if (!plugin->prepare(sampleRate, maxFrames)) {
                    logLine("NicheLooper: preset plugin failed: %s", spec.name.c_str());
                    continue;
                }
                if (!spec.componentState.empty()) {
                    plugin->restoreState(spec.componentState, spec.controllerState);
                }
                loaded[static_cast<size_t>(c)].push_back(std::move(plugin));
            }
        }
    });

    std::array<std::vector<std::unique_ptr<VstPlugin>>, kNumChains> old;
    {
        std::lock_guard<std::mutex> guard(mImpl->lock);
        for (int c = 0; c < kNumChains; ++c) {
            old[static_cast<size_t>(c)] = std::move(mImpl->chains[c]);
            mImpl->chains[c] = std::move(loaded[static_cast<size_t>(c)]);
        }
        mImpl->active.store(active, std::memory_order_relaxed);
    }
    for (auto& chain : old) {
        destroyOnPluginThread(std::move(chain));
    }
    return true;
}
