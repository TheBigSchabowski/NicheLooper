#pragma once

#include <memory>
#include <cstdint>
#include <string>
#include <vector>

/**
 * Three switchable chains of Audio Unit effect plugins (the AU builds of the
 * user's VSTs — NAM, TONEX, Archetypes, … — hosted via the system
 * AudioToolbox API, so no external plugin SDK is needed).
 *
 * The signal path is: mono input → ACTIVE chain → looper/monitor, i.e. the
 * loop records the processed (amp) sound and switching chains re-voices only
 * the live input, never existing loop content.
 *
 * Threading model:
 *  - All editing calls (add/remove/move/prepare/editor) come from app
 *    threads and serialize on an internal mutex.
 *  - processBlock() is called ONLY from the real-time audio callback. It
 *    try-locks that mutex: while an edit is in flight the block passes
 *    through dry instead of blocking the audio thread.
 *  - The active chain index is an atomic — switching (keys A/S/D) is
 *    glitch-free and instant.
 *
 * Editor windows are plain NSWindows hosting the plugin's own Cocoa view
 * (fallback: AUGenericView); they are created/closed via dispatch to the
 * main thread, which the JVM's AppKit integration keeps pumping.
 */
class PluginChainManager {
public:
    static constexpr int kNumChains = 3;

    PluginChainManager();
    ~PluginChainManager();

    PluginChainManager(const PluginChainManager&) = delete;
    PluginChainManager& operator=(const PluginChainManager&) = delete;

    // Re-enumerates installed effect AUs; indices into the returned list are
    // what addPlugin() expects (stable until the next call).
    std::vector<std::string> availablePluginNames();

    // Instantiates plugin [pluginIndex] (from the last enumeration) at the
    // current engine rate and appends it to the chain. Returns false if the
    // AU could not be created or initialized (e.g. unsupported layout).
    bool addPlugin(int chain, int pluginIndex);

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

    // Render/error counters per plugin plus block statistics — the ground
    // truth for "is audio actually flowing through the chain".
    std::string debugReport();

    // Snapshot all 3 chains (plugin identity via AudioComponentDescription +
    // full AU state via kAudioUnitProperty_ClassInfo — incl. the NAM model)
    // as a binary property list. Returns empty on failure.
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
