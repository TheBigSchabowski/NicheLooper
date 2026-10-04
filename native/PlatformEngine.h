#pragma once

// The audio engine of the platform this library is built for. The JNI bridge only
// talks to this alias, so it stays the same on every platform.
#if defined(__APPLE__)
#include "MacAudioEngine.h"
using PlatformAudioEngine = MacAudioEngine;
#elif defined(__linux__)
#include "LinuxAudioEngine.h"
using PlatformAudioEngine = LinuxAudioEngine;
#else
#error "NicheLooper: no audio engine for this platform"
#endif
