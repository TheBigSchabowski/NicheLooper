#include "AuPluginChain.h"

#import <AppKit/AppKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AUCocoaUIView.h>
#import <CoreAudioKit/CoreAudioKit.h>
#import <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <array>
#include <pthread.h>

// Compiled WITHOUT ARC (the build uses one clang++ invocation for all
// translation units) — every ObjC object below is managed manually.

namespace {

// ---- Editor windows -------------------------------------------------------
// Windows are owned by this main-thread-only map, keyed by the AU instance,
// so window lifetime never touches C++ plugin object lifetime.

std::map<AudioComponentInstance, NSWindow*>& editorWindows() {
    static auto* windows = new std::map<AudioComponentInstance, NSWindow*>();
    return *windows;
}

void runOnMainThread(bool wait, void (^block)(void)) {
    if (pthread_main_np() != 0) {
        block();
    } else if (wait) {
        dispatch_sync(dispatch_get_main_queue(), block);
    } else {
        dispatch_async(dispatch_get_main_queue(), block);
    }
}

// The plugin's own Cocoa view if it ships one, AUGenericView otherwise.
NSView* createViewForUnit(AudioComponentInstance unit) {
    UInt32 size = 0;
    Boolean writable = false;
    if (AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0,
                                 &size, &writable) == noErr &&
        size >= sizeof(AudioUnitCocoaViewInfo)) {
        auto* info = static_cast<AudioUnitCocoaViewInfo*>(malloc(size));
        if (AudioUnitGetProperty(unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0,
                                 info, &size) == noErr) {
            const unsigned numClasses =
                (size - sizeof(CFURLRef)) / sizeof(CFStringRef);
            NSView* view = nil;
            NSBundle* bundle = [NSBundle
                bundleWithURL:(NSURL*)info->mCocoaAUViewBundleLocation];
            if (bundle != nil && numClasses > 0) {
                Class factoryClass =
                    [bundle classNamed:(NSString*)info->mCocoaAUViewClass[0]];
                if (factoryClass != Nil) {
                    NSObject<AUCocoaUIBase>* factory = [[factoryClass alloc] init];
                    view = [factory uiViewForAudioUnit:unit
                                              withSize:NSMakeSize(900, 600)];
                    [factory release];
                }
            }
            CFRelease(info->mCocoaAUViewBundleLocation);
            for (unsigned i = 0; i < numClasses; ++i) {
                CFRelease(info->mCocoaAUViewClass[i]);
            }
            free(info);
            if (view != nil) {
                return view;  // retained by the window via contentView
            }
        } else {
            free(info);
        }
    }
    return [[[AUGenericView alloc] initWithAudioUnit:unit] autorelease];
}

void openEditorWindow(AudioComponentInstance unit, const std::string& name) {
    NSString* title = [NSString stringWithUTF8String:name.c_str()];
    runOnMainThread(false, ^{
        auto& windows = editorWindows();
        auto it = windows.find(unit);
        if (it != windows.end()) {
            [it->second makeKeyAndOrderFront:nil];
            return;
        }
        NSView* view = createViewForUnit(unit);
        NSRect frame = view.frame;
        if (frame.size.width < 200 || frame.size.height < 100) {
            frame = NSMakeRect(0, 0, 900, 600);
        }
        NSWindow* window = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                 NSWindowStyleMaskMiniaturizable)
                        backing:NSBackingStoreBuffered
                          defer:NO];
        window.releasedWhenClosed = NO;  // the close button only hides
        window.title = title;
        window.contentView = view;
        [window setContentSize:frame.size];
        [window center];
        windows[unit] = window;  // owned here (rc 1 from alloc)
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        std::fprintf(stderr, "NicheLooper: editor window open for %s (visible=%d)\n",
                     name.c_str(), window.isVisible ? 1 : 0);
    });
}

// Synchronous: after this returns the view no longer references the unit,
// so the caller may dispose it.
void closeEditorWindow(AudioComponentInstance unit) {
    runOnMainThread(true, ^{
        auto& windows = editorWindows();
        auto it = windows.find(unit);
        if (it == windows.end()) {
            return;
        }
        NSWindow* window = it->second;
        window.contentView = nil;
        [window close];
        [window release];
        windows.erase(it);
    });
}

// ---- One hosted AU --------------------------------------------------------

class AuPlugin {
public:
    AuPlugin(AudioComponent component, std::string name)
        : mComponent(component), mName(std::move(name)) {}

    ~AuPlugin() {
        if (mUnit != nullptr) {
            closeEditorWindow(mUnit);
            if (mInitialized) {
                AudioUnitUninitialize(mUnit);
            }
            AudioComponentInstanceDispose(mUnit);
        }
    }

    const std::string& name() const { return mName; }

    bool instantiate() {
        return AudioComponentInstanceNew(mComponent, &mUnit) == noErr && mUnit != nullptr;
    }

    // App thread, audio callback not touching this plugin. Tries mono I/O
    // first (NAM & friends), falls back to stereo (in-place dual mono).
    bool prepare(int sampleRate, int maxFrames) {
        if (mUnit == nullptr) {
            return false;
        }
        if (mInitialized) {
            AudioUnitUninitialize(mUnit);
            mInitialized = false;
        }

        mChannels = 0;
        for (int channels : {1, 2}) {
            AudioStreamBasicDescription format{};
            format.mSampleRate = sampleRate;
            format.mFormatID = kAudioFormatLinearPCM;
            format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked |
                                  kAudioFormatFlagIsNonInterleaved;
            format.mBitsPerChannel = 32;
            format.mFramesPerPacket = 1;
            format.mBytesPerFrame = 4;  // per channel (non-interleaved)
            format.mBytesPerPacket = 4;
            format.mChannelsPerFrame = static_cast<UInt32>(channels);
            if (AudioUnitSetProperty(mUnit, kAudioUnitProperty_StreamFormat,
                                     kAudioUnitScope_Input, 0, &format,
                                     sizeof(format)) == noErr &&
                AudioUnitSetProperty(mUnit, kAudioUnitProperty_StreamFormat,
                                     kAudioUnitScope_Output, 0, &format,
                                     sizeof(format)) == noErr) {
                mChannels = channels;
                break;
            }
        }
        if (mChannels == 0) {
            std::fprintf(stderr, "NicheLooper: %s accepts neither mono nor stereo f32\n",
                         mName.c_str());
            return false;
        }

        UInt32 maxSlice = static_cast<UInt32>(maxFrames);
        OSStatus propErr = AudioUnitSetProperty(
            mUnit, kAudioUnitProperty_MaximumFramesPerSlice,
            kAudioUnitScope_Global, 0, &maxSlice, sizeof(maxSlice));
        if (propErr != noErr) {
            std::fprintf(stderr, "NicheLooper: %s MaximumFramesPerSlice failed: %d\n",
                         mName.c_str(), static_cast<int>(propErr));
        }

        AURenderCallbackStruct callback{&AuPlugin::renderInputCallback, this};
        propErr = AudioUnitSetProperty(
            mUnit, kAudioUnitProperty_SetRenderCallback,
            kAudioUnitScope_Input, 0, &callback, sizeof(callback));
        if (propErr != noErr) {
            std::fprintf(stderr, "NicheLooper: %s SetRenderCallback failed: %d\n",
                         mName.c_str(), static_cast<int>(propErr));
            return false;
        }

        mFeed[0].assign(static_cast<size_t>(maxFrames), 0.0f);
        mFeed[1].assign(static_cast<size_t>(maxFrames), 0.0f);
        mOut[0].assign(static_cast<size_t>(maxFrames), 0.0f);
        mOut[1].assign(static_cast<size_t>(maxFrames), 0.0f);
        mSampleTime = 0;

        if (AudioUnitInitialize(mUnit) != noErr) {
            std::fprintf(stderr, "NicheLooper: AudioUnitInitialize failed for %s\n",
                         mName.c_str());
            return false;
        }
        mInitialized = true;
        return true;
    }

    void openEditor() {
        if (mUnit != nullptr) {
            openEditorWindow(mUnit, mName);
        }
    }

    // --- Full state capture/restore (kAudioUnitProperty_ClassInfo) ---
    // ClassInfo carries every parameter value plus the plugin's custom state
    // (e.g. the NAM model loaded into Gateway), so a captured/restored AU
    // comes back identical without reopening its editor. Serialized to a
    // binary-plist CFData so the parent bank plist stays pure plist.

    CFDataRef captureStateData() const {
        if (mUnit == nullptr) return nullptr;
        CFPropertyListRef pl = nullptr;
        UInt32 size = sizeof(pl);
        const OSStatus err = AudioUnitGetProperty(
            mUnit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &pl, &size);
        if (err != noErr || pl == nullptr) return nullptr;
        CFDataRef data = CFPropertyListCreateData(
            kCFAllocatorDefault, pl, kCFPropertyListBinaryFormat_v1_0, 0, nullptr);
        CFRelease(pl);
        return data;  // +1 (caller releases); nullptr if the AU has no state
    }

    bool restoreStateData(const void* data, CFIndex len) {
        if (mUnit == nullptr || data == nullptr || len <= 0) return false;
        CFDataRef cfData = CFDataCreateWithBytesNoCopy(
            kCFAllocatorDefault, static_cast<const UInt8*>(data), len, kCFAllocatorNull);
        CFPropertyListRef pl = CFPropertyListCreateWithData(
            kCFAllocatorDefault, cfData, kCFPropertyListImmutable, nullptr, nullptr);
        CFRelease(cfData);
        if (pl == nullptr) return false;
        const OSStatus err = AudioUnitSetProperty(
            mUnit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &pl, sizeof(pl));
        CFRelease(pl);
        return err == noErr;
    }

    AudioComponentDescription description() const {
        AudioComponentDescription d{};
        if (mComponent != nullptr) AudioComponentGetDescription(mComponent, &d);
        return d;
    }

    uint32_t renderCount() const { return mRenderCount.load(std::memory_order_relaxed); }
    uint32_t errorCount() const { return mErrorCount.load(std::memory_order_relaxed); }
    int32_t lastError() const { return mLastError.load(std::memory_order_relaxed); }
    int channels() const { return mChannels; }
    bool initialized() const { return mInitialized; }

    // Audio thread. In-place on mono; bypasses itself on any render error.
    void process(float* mono, int frames) {
        if (!mInitialized || frames > static_cast<int>(mFeed[0].size())) {
            return;
        }
        const size_t bytes = static_cast<size_t>(frames) * sizeof(float);
        std::memcpy(mFeed[0].data(), mono, bytes);
        if (mChannels == 2) {
            std::memcpy(mFeed[1].data(), mono, bytes);
        }

        AudioUnitRenderActionFlags flags = 0;
        AudioTimeStamp timestamp{};
        timestamp.mFlags = kAudioTimeStampSampleTimeValid;
        timestamp.mSampleTime = mSampleTime;

        auto* bufferList = reinterpret_cast<AudioBufferList*>(mAblStorage);
        bufferList->mNumberBuffers = static_cast<UInt32>(mChannels);
        for (int c = 0; c < mChannels; ++c) {
            bufferList->mBuffers[c].mNumberChannels = 1;
            bufferList->mBuffers[c].mDataByteSize = static_cast<UInt32>(bytes);
            bufferList->mBuffers[c].mData = mOut[c].data();
        }

        const OSStatus err =
            AudioUnitRender(mUnit, &flags, &timestamp, 0,
                            static_cast<UInt32>(frames), bufferList);
        mSampleTime += frames;
        if (err != noErr) {
            mLastError.store(err, std::memory_order_relaxed);
            if (mErrorCount.fetch_add(1, std::memory_order_relaxed) < 3) {
                // Diagnostic only; errors are rare and this path already
                // failed, so the fprintf cannot make things worse.
                std::fprintf(stderr, "NicheLooper: AudioUnitRender(%s) failed: %d\n",
                             mName.c_str(), static_cast<int>(err));
            }
            return;  // dry passthrough
        }
        mRenderCount.fetch_add(1, std::memory_order_relaxed);
        if (mChannels == 1) {
            // Falls das Plugin Stille meldet (mDataByteSize == 0), nichts
            // zurueckkopieren -> trockener Input bleibt erhalten.
            if (bufferList->mBuffers[0].mDataByteSize >= bytes) {
                std::memcpy(mono, bufferList->mBuffers[0].mData, bytes);
            }
        } else {
            if (bufferList->mBuffers[0].mDataByteSize >= bytes &&
                bufferList->mBuffers[1].mDataByteSize >= bytes) {
                const float* left = static_cast<const float*>(bufferList->mBuffers[0].mData);
                const float* right = static_cast<const float*>(bufferList->mBuffers[1].mData);
                for (int i = 0; i < frames; ++i) {
                    mono[i] = 0.5f * (left[i] + right[i]);
                }
            }
        }
    }

private:
    static OSStatus renderInputCallback(void* refCon,
                                        AudioUnitRenderActionFlags* /*flags*/,
                                        const AudioTimeStamp* /*timestamp*/,
                                        UInt32 /*bus*/, UInt32 frames,
                                        AudioBufferList* ioData) {
        auto* self = static_cast<AuPlugin*>(refCon);
        const UInt32 bytes = static_cast<UInt32>(frames * sizeof(float));
        for (UInt32 b = 0; b < ioData->mNumberBuffers && b < 2; ++b) {
            float* source = self->mFeed[b].data();
            // AU-VST-Wrapper lesen mDataByteSize/mNumberChannels aus dem
            // Input-ABL, um zu erfahren, wieviel Sample-Daten vorliegen. Fehlen
            // die Metadaten (oder ist mDataByteSize == 0 = "leer, fuell mich"),
            // bekam das Plugin Stille als Input und produzierte Stille am
            // Ausgang -> kein Audio sobald eine Chain aktiv war.
            //
            // Drei Faelle, alle datenliefernd und niemals ueber die Buffer-
            // kapazitaet hinausschreibend:
            //  (a) Plugin hat keinen Buffer besorgt  -> unseren Feed einhaengen.
            //  (b) Plugin-Buffer ist gross genug    -> hineinkopieren (manche
            //      AUs erwarten Befuellung ihres eigenen Buffers und ignorieren
            //      einen neu gesetzten mData-Zeiger).
            //  (c) Plugin-Buffer zu klein gemeldet   -> ebenfalls unseren Feed
            //      einhaengen; sonst wuerden wir 0 Byte kopieren und danach
            //      mDataByteSize=bytes behaupten -> Plugin liest Nullen -> Stille.
            if (ioData->mBuffers[b].mData == nullptr ||
                ioData->mBuffers[b].mDataByteSize < bytes) {
                ioData->mBuffers[b].mData = source;
            } else {
                std::memcpy(ioData->mBuffers[b].mData, source, bytes);
            }
            ioData->mBuffers[b].mDataByteSize = bytes;
            ioData->mBuffers[b].mNumberChannels = 1;
        }
        return noErr;
    }

    AudioComponent mComponent;
    AudioComponentInstance mUnit = nullptr;
    std::string mName;
    bool mInitialized = false;
    int mChannels = 0;
    std::atomic<uint32_t> mRenderCount{0};
    std::atomic<uint32_t> mErrorCount{0};
    std::atomic<int32_t> mLastError{0};
    std::vector<float> mFeed[2];
    std::vector<float> mOut[2];
    double mSampleTime = 0;
    // Room for a 2-buffer AudioBufferList (the struct declares 1 buffer).
    alignas(AudioBufferList) char mAblStorage[sizeof(AudioBufferList) + sizeof(AudioBuffer)]{};
};

}  // namespace

// ---- Manager --------------------------------------------------------------

struct PluginChainManager::Impl {
    std::mutex lock;
    std::vector<AudioComponent> available;
    std::vector<std::unique_ptr<AuPlugin>> chains[kNumChains];
    std::atomic<int> active{0};
    int sampleRate = 48000;
    int maxFrames = 8192;
    // Diagnostics (audio thread writes, app thread reads).
    std::atomic<uint32_t> blocksProcessed{0};    // blocks with non-empty chain
    std::atomic<uint32_t> blocksSkippedLock{0};  // try_lock lost against an edit
};

PluginChainManager::PluginChainManager() : mImpl(std::make_unique<Impl>()) {}
PluginChainManager::~PluginChainManager() = default;

std::vector<std::string> PluginChainManager::availablePluginNames() {
    std::lock_guard<std::mutex> guard(mImpl->lock);
    mImpl->available.clear();
    std::vector<std::string> names;
    for (OSType type : {kAudioUnitType_Effect, kAudioUnitType_MusicEffect}) {
        AudioComponentDescription wanted{};
        wanted.componentType = type;
        AudioComponent component = nullptr;
        while ((component = AudioComponentFindNext(component, &wanted)) != nullptr) {
            CFStringRef cfName = nullptr;
            if (AudioComponentCopyName(component, &cfName) != noErr || cfName == nullptr) {
                continue;
            }
            char buffer[512];
            if (CFStringGetCString(cfName, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
                mImpl->available.push_back(component);
                names.emplace_back(buffer);
            }
            CFRelease(cfName);
        }
    }
    return names;
}

bool PluginChainManager::addPlugin(int chain, int pluginIndex) {
    if (chain < 0 || chain >= kNumChains) {
        return false;
    }
    AudioComponent component = nullptr;
    std::string name;
    int sampleRate;
    int maxFrames;
    {
        std::lock_guard<std::mutex> guard(mImpl->lock);
        if (pluginIndex < 0 ||
            pluginIndex >= static_cast<int>(mImpl->available.size())) {
            return false;
        }
        component = mImpl->available[static_cast<size_t>(pluginIndex)];
        sampleRate = mImpl->sampleRate;
        maxFrames = mImpl->maxFrames;
    }
    CFStringRef cfName = nullptr;
    if (AudioComponentCopyName(component, &cfName) == noErr && cfName != nullptr) {
        char buffer[512];
        if (CFStringGetCString(cfName, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
            name = buffer;
        }
        CFRelease(cfName);
    }

    // Instantiate + initialize OUTSIDE the lock (can take hundreds of ms for
    // big plugins) so the audio thread keeps processing meanwhile.
    auto plugin = std::make_unique<AuPlugin>(component, name);
    if (!plugin->instantiate() || !plugin->prepare(sampleRate, maxFrames)) {
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
    std::unique_ptr<AuPlugin> removed;
    {
        std::lock_guard<std::mutex> guard(mImpl->lock);
        auto& plugins = mImpl->chains[chain];
        if (slot < 0 || slot >= static_cast<int>(plugins.size())) {
            return false;
        }
        removed = std::move(plugins[static_cast<size_t>(slot)]);
        plugins.erase(plugins.begin() + slot);
    }
    // Destroyed outside the lock: closes the editor window (sync on main)
    // and disposes the AU after the audio thread can no longer see it.
    removed.reset();
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
    if (slot >= 0 && slot < static_cast<int>(plugins.size())) {
        plugins[static_cast<size_t>(slot)]->openEditor();
    }
}

void PluginChainManager::setActiveChain(int index) {
    if (index >= 0 && index < kNumChains) {
        mImpl->active.store(index, std::memory_order_relaxed);
    }
}

int PluginChainManager::activeChain() const {
    return mImpl->active.load(std::memory_order_relaxed);
}

void PluginChainManager::prepare(int sampleRate, int maxFrames) {
    std::lock_guard<std::mutex> guard(mImpl->lock);
    const bool changed =
        sampleRate != mImpl->sampleRate || maxFrames != mImpl->maxFrames;
    mImpl->sampleRate = sampleRate;
    mImpl->maxFrames = maxFrames;
    if (!changed) {
        return;
    }
    for (auto& chain : mImpl->chains) {
        for (auto& plugin : chain) {
            plugin->prepare(sampleRate, maxFrames);
        }
    }
}

void PluginChainManager::processBlock(float* mono, int frames) {
    std::unique_lock<std::mutex> guard(mImpl->lock, std::try_to_lock);
    if (!guard.owns_lock()) {
        mImpl->blocksSkippedLock.fetch_add(1, std::memory_order_relaxed);
        return;  // edit in progress → this block stays dry
    }
    auto& chain = mImpl->chains[mImpl->active.load(std::memory_order_relaxed)];
    if (!chain.empty()) {
        mImpl->blocksProcessed.fetch_add(1, std::memory_order_relaxed);
    }
    for (auto& plugin : chain) {
        plugin->process(mono, frames);
    }
}

std::string PluginChainManager::debugReport() {
    std::lock_guard<std::mutex> guard(mImpl->lock);
    char line[256];
    std::snprintf(line, sizeof(line),
                  "active=%d rate=%d blocks=%u lockSkips=%u\n",
                  mImpl->active.load(std::memory_order_relaxed), mImpl->sampleRate,
                  mImpl->blocksProcessed.load(std::memory_order_relaxed),
                  mImpl->blocksSkippedLock.load(std::memory_order_relaxed));
    std::string report(line);
    for (int c = 0; c < kNumChains; ++c) {
        for (size_t s = 0; s < mImpl->chains[c].size(); ++s) {
            const auto& plugin = mImpl->chains[c][s];
            std::snprintf(line, sizeof(line),
                          "chain%d[%zu] %s ch=%d init=%d renders=%u errors=%u lastErr=%d\n",
                          c, s, plugin->name().c_str(), plugin->channels(),
                          plugin->initialized() ? 1 : 0, plugin->renderCount(),
                          plugin->errorCount(), plugin->lastError());
            report += line;
        }
    }
    return report;
}

// ---- Preset bank (all 3 chains: identity + full AU state) -----------------

namespace {
void cfAddNum(CFMutableDictionaryRef d, CFStringRef key, UInt32 v) {
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &v);
    CFDictionarySetValue(d, key, n);
    CFRelease(n);
}
}  // namespace

std::vector<uint8_t> PluginChainManager::saveBank() {
    std::vector<uint8_t> out;
    std::lock_guard<std::mutex> guard(mImpl->lock);

    CFMutableDictionaryRef top = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const int active = mImpl->active.load(std::memory_order_relaxed);
    CFNumberRef activeNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &active);
    CFDictionarySetValue(top, CFSTR("activeChain"), activeNum);
    CFRelease(activeNum);

    CFMutableArrayRef chainsArr = CFArrayCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    for (int c = 0; c < kNumChains; ++c) {
        CFMutableArrayRef chainArr = CFArrayCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
        for (const auto& plugin : mImpl->chains[c]) {
            CFMutableDictionaryRef pd = CFDictionaryCreateMutable(
                kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFStringRef nameRef = CFStringCreateWithCString(
                kCFAllocatorDefault, plugin->name().c_str(), kCFStringEncodingUTF8);
            CFDictionarySetValue(pd, CFSTR("name"), nameRef);
            CFRelease(nameRef);

            const AudioComponentDescription d = plugin->description();
            CFMutableDictionaryRef desc = CFDictionaryCreateMutable(
                kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            cfAddNum(desc, CFSTR("type"), d.componentType);
            cfAddNum(desc, CFSTR("subtype"), d.componentSubType);
            cfAddNum(desc, CFSTR("manufacturer"), d.componentManufacturer);
            cfAddNum(desc, CFSTR("flags"), d.componentFlags);
            CFDictionarySetValue(pd, CFSTR("desc"), desc);
            CFRelease(desc);

            CFDataRef state = plugin->captureStateData();
            if (state != nullptr) {
                CFDictionarySetValue(pd, CFSTR("state"), state);
                CFRelease(state);
            }
            CFArrayAppendValue(chainArr, pd);
            CFRelease(pd);
        }
        CFArrayAppendValue(chainsArr, chainArr);
        CFRelease(chainArr);
    }
    CFDictionarySetValue(top, CFSTR("chains"), chainsArr);
    CFRelease(chainsArr);

    CFDataRef plist = CFPropertyListCreateData(
        kCFAllocatorDefault, top, kCFPropertyListBinaryFormat_v1_0, 0, nullptr);
    CFRelease(top);
    if (plist != nullptr) {
        const UInt8* bytes = CFDataGetBytePtr(plist);
        const CFIndex len = CFDataGetLength(plist);
        out.assign(bytes, bytes + len);
        CFRelease(plist);
    } else {
        std::fprintf(stderr, "NicheLooper: saveBank konnte keine Plist erzeugen\n");
    }
    return out;
}

bool PluginChainManager::loadBank(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) return false;
    CFDataRef cfData = CFDataCreateWithBytesNoCopy(
        kCFAllocatorDefault, data, static_cast<CFIndex>(size), kCFAllocatorNull);
    CFPropertyListRef pl = CFPropertyListCreateWithData(
        kCFAllocatorDefault, cfData, kCFPropertyListImmutable, nullptr, nullptr);
    CFRelease(cfData);
    if (pl == nullptr || CFGetTypeID(pl) != CFDictionaryGetTypeID()) {
        if (pl != nullptr) CFRelease(pl);
        return false;
    }
    CFDictionaryRef top = static_cast<CFDictionaryRef>(pl);

    // Parse into a local structure so we can release the plist and do the
    // (slow) AU instantiation outside the lock.
    struct PluginSpec {
        std::string name;
        AudioComponentDescription desc{};
        std::vector<uint8_t> state;
    };
    std::array<std::vector<PluginSpec>, kNumChains> chainsSpec;
    int active = 0;
    {
        CFNumberRef an = static_cast<CFNumberRef>(CFDictionaryGetValue(top, CFSTR("activeChain")));
        if (an != nullptr) CFNumberGetValue(an, kCFNumberSInt32Type, &active);
    }
    CFArrayRef chainsArr = static_cast<CFArrayRef>(CFDictionaryGetValue(top, CFSTR("chains")));
    if (chainsArr != nullptr) {
        const CFIndex nChains = std::min<CFIndex>(CFArrayGetCount(chainsArr), kNumChains);
        for (CFIndex c = 0; c < nChains; ++c) {
            CFArrayRef chainArr = static_cast<CFArrayRef>(CFArrayGetValueAtIndex(chainsArr, c));
            if (chainArr == nullptr) continue;
            const CFIndex n = CFArrayGetCount(chainArr);
            for (CFIndex i = 0; i < n; ++i) {
                CFDictionaryRef pd = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(chainArr, i));
                if (pd == nullptr) continue;
                PluginSpec spec;
                CFStringRef nameRef = static_cast<CFStringRef>(CFDictionaryGetValue(pd, CFSTR("name")));
                if (nameRef != nullptr) {
                    const CFIndex cap = CFStringGetLength(nameRef) * 4 + 1;
                    std::vector<char> buf(static_cast<size_t>(cap));
                    if (CFStringGetCString(nameRef, buf.data(), cap, kCFStringEncodingUTF8)) {
                        spec.name = buf.data();
                    }
                }
                CFDictionaryRef desc = static_cast<CFDictionaryRef>(CFDictionaryGetValue(pd, CFSTR("desc")));
                if (desc != nullptr) {
                    CFNumberRef t = static_cast<CFNumberRef>(CFDictionaryGetValue(desc, CFSTR("type")));
                    CFNumberRef su = static_cast<CFNumberRef>(CFDictionaryGetValue(desc, CFSTR("subtype")));
                    CFNumberRef m = static_cast<CFNumberRef>(CFDictionaryGetValue(desc, CFSTR("manufacturer")));
                    if (t) CFNumberGetValue(t, kCFNumberSInt32Type, &spec.desc.componentType);
                    if (su) CFNumberGetValue(su, kCFNumberSInt32Type, &spec.desc.componentSubType);
                    if (m) CFNumberGetValue(m, kCFNumberSInt32Type, &spec.desc.componentManufacturer);
                    spec.desc.componentFlagsMask = 0;
                    spec.desc.componentFlags = 0;
                }
                CFDataRef state = static_cast<CFDataRef>(CFDictionaryGetValue(pd, CFSTR("state")));
                if (state != nullptr) {
                    const UInt8* bytes = CFDataGetBytePtr(state);
                    const CFIndex len = CFDataGetLength(state);
                    spec.state.assign(bytes, bytes + len);
                }
                chainsSpec[c].push_back(std::move(spec));
            }
        }
    }
    CFRelease(pl);

    int sampleRate = 0;
    int maxFrames = 0;
    {
        std::lock_guard<std::mutex> guard(mImpl->lock);
        sampleRate = mImpl->sampleRate;
        maxFrames = mImpl->maxFrames;
    }
    if (sampleRate <= 0 || maxFrames <= 0) {
        std::fprintf(stderr, "NicheLooper: loadBank ohne laufende Engine (rate=%d)\n", sampleRate);
        return false;
    }

    std::array<std::vector<std::unique_ptr<AuPlugin>>, kNumChains> built;
    for (int c = 0; c < kNumChains; ++c) {
        for (const auto& spec : chainsSpec[c]) {
            AudioComponentDescription findDesc = spec.desc;
            findDesc.componentFlagsMask = 0;
            findDesc.componentFlags = 0;
            AudioComponent component = AudioComponentFindNext(nullptr, &findDesc);
            if (component == nullptr) {
                std::fprintf(stderr, "NicheLooper: Plugin nicht (mehr) installiert: %s\n",
                             spec.name.c_str());
                continue;
            }
            auto plugin = std::make_unique<AuPlugin>(component, spec.name);
            if (!plugin->instantiate() || !plugin->prepare(sampleRate, maxFrames)) {
                std::fprintf(stderr, "NicheLooper: Preset-Plugin fehlgeschlagen: %s\n",
                             spec.name.c_str());
                continue;
            }
            if (!spec.state.empty()) {
                plugin->restoreStateData(spec.state.data(), static_cast<CFIndex>(spec.state.size()));
            }
            built[c].push_back(std::move(plugin));
        }
    }

    std::array<std::vector<std::unique_ptr<AuPlugin>>, kNumChains> old;
    {
        std::lock_guard<std::mutex> guard(mImpl->lock);
        for (int c = 0; c < kNumChains; ++c) {
            old[c] = std::move(mImpl->chains[c]);
            mImpl->chains[c] = std::move(built[c]);
        }
        if (active >= 0 && active < kNumChains) {
            mImpl->active.store(active, std::memory_order_relaxed);
        }
    }
    for (int c = 0; c < kNumChains; ++c) {
        old[c].clear();  // editor windows + AUs disposed outside the lock
    }
    return true;
}
