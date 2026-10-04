#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/**
 * Three switchable chains of VST3 effect plugins (the Linux builds installed
 * under ~/.vst3, /usr/lib/vst3, /usr/local/lib/vst3 and friends), hosted via
 * the VST3 SDK (MIT). Counterpart of the macOS AuPluginChain and the Windows
 * VstPluginChain, with the same API.
 *
 * The signal path is: mono input -> ACTIVE chain -> looper/monitor, i.e. the
 * loop records the processed (amp) sound and switching chains re-voices only
 * the live input, never existing loop content.
 *
 * Threading model (identical to the other platforms):
 *  - All editing calls (add/remove/move/prepare/editor) come from app
 *    threads and serialize on an internal mutex.
 *  - processBlock() is called ONLY from the real-time audio callback. It
 *    try-locks that mutex: while an edit is in flight the block passes
 *    through dry instead of blocking the audio thread.
 *  - The active chain index is an atomic - switching (keys A/S/D) is
 *    glitch-free and instant.
 *
 * Plugin UI: a VST3 on Linux is a directory bundle (X.vst3/Contents/<arch>-linux/X.so)
 * whose editor embeds into a host-provided X11 window (XEmbed). Everything that
 * touches a plugin - loading, creating, initialising, destroying, saving state
 * and the editor windows - runs on ONE dedicated internal UI thread. It owns its
 * own X connection and doubles as the plugin's Linux::IRunLoop (file descriptors
 * and timers); the JVM's AWT threads never see a plugin. The close button of an
 * editor window only hides it; it is destroyed when the plugin is removed.
 * Plugin editors need an X server (on Wayland: XWayland).
 */
class PluginChainManager {
public:
    static constexpr int kNumChains = 3;

    PluginChainManager();
    ~PluginChainManager();

    PluginChainManager(const PluginChainManager&) = delete;
    PluginChainManager& operator=(const PluginChainManager&) = delete;

    // Scans the VST3 directories; indices into the returned list are what
    // addPlugin() expects (stable until the next call). Slow (loads every
    // module once) - call from a background thread.
    std::vector<std::string> availablePluginNames();

    // Instantiates plugin [pluginIndex] (from the last scan) at the current
    // engine rate and appends it to the chain. Returns false if the plugin
    // could not be created or initialized (e.g. unsupported layout).
    bool addPlugin(int chain, int pluginIndex);

    // Loads the VST3 module the user picked from disk ([path] is the .vst3
    // bundle directory or the .so inside it) at the current engine rate and
    // appends its first non-instrument audio-effect class to the chain. Returns
    // false if the module cannot be loaded or exposes no usable effect. Like
    // addPlugin(), the slow load + instantiate happens off the audio lock.
    bool addPluginFromPath(int chain, const std::string& path);

    bool removePlugin(int chain, int slot);
    bool movePlugin(int chain, int from, int to);
    std::vector<std::string> chainPluginNames(int chain);

    // Opens (or refocuses) the plugin's editor window.
    void openEditor(int chain, int slot);

    void setActiveChain(int index);
    int activeChain() const;

    // Engine start (app thread, streams stopped): re-initializes every
    // plugin at the new sample rate / block size.
    void prepare(int sampleRate, int maxFrames);

    // Audio thread: processes the active chain in place on the mono buffer.
    void processBlock(float* mono, int frames);

    // Render/error counters per plugin plus block statistics - the ground
    // truth for "is audio actually flowing through the chain".
    std::string debugReport();

    // Snapshot all 3 chains (plugin identity via module path + class UID,
    // full component & controller state) as a binary blob. Returns empty on
    // failure.
    std::vector<uint8_t> saveBank();

    // Replaces all 3 chains from a previously saved bank. Requires a running
    // engine (plugins are instantiated at the current sample rate); plugins
    // no longer installed are skipped (logged). Returns false on a parse
    // error or if the engine is not running.
    bool loadBank(const uint8_t* data, size_t size);

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
