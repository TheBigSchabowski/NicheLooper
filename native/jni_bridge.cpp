#include <jni.h>

#include <string>
#include <vector>

#include "MacAudioEngine.h"

namespace {
MacAudioEngine gEngine;

std::vector<float> toVector(JNIEnv* env, jfloatArray array) {
    std::vector<float> v(static_cast<size_t>(env->GetArrayLength(array)));
    env->GetFloatArrayRegion(array, 0, static_cast<jsize>(v.size()), v.data());
    return v;
}

jobjectArray toStringArray(JNIEnv* env, const std::vector<std::string>& strings) {
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray result = env->NewObjectArray(
            static_cast<jsize>(strings.size()), stringClass, nullptr);
    for (size_t i = 0; i < strings.size(); ++i) {
        jstring s = env->NewStringUTF(strings[i].c_str());
        env->SetObjectArrayElement(result, static_cast<jsize>(i), s);
        env->DeleteLocalRef(s);
    }
    return result;
}
}  // namespace

extern "C" {

JNIEXPORT jboolean JNICALL
Java_nichelooper_audio_AudioEngine_nativeRefreshDevices(JNIEnv* /*env*/, jobject /*thiz*/) {
    return gEngine.refreshDevices() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jobjectArray JNICALL
Java_nichelooper_audio_AudioEngine_nativeGetInputDeviceNames(JNIEnv* env, jobject /*thiz*/) {
    return toStringArray(env, gEngine.inputDeviceNames());
}

JNIEXPORT jobjectArray JNICALL
Java_nichelooper_audio_AudioEngine_nativeGetOutputDeviceNames(JNIEnv* env, jobject /*thiz*/) {
    return toStringArray(env, gEngine.outputDeviceNames());
}

JNIEXPORT jint JNICALL
Java_nichelooper_audio_AudioEngine_nativeGetDefaultInputIndex(JNIEnv* /*env*/, jobject /*thiz*/) {
    return gEngine.defaultInputIndex();
}

JNIEXPORT jint JNICALL
Java_nichelooper_audio_AudioEngine_nativeGetDefaultOutputIndex(JNIEnv* /*env*/, jobject /*thiz*/) {
    return gEngine.defaultOutputIndex();
}

JNIEXPORT jboolean JNICALL
Java_nichelooper_audio_AudioEngine_nativeStart(
        JNIEnv* /*env*/, jobject /*thiz*/, jint inputIndex, jint outputIndex) {
    return gEngine.start(inputIndex, outputIndex) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_nichelooper_audio_AudioEngine_nativeStop(JNIEnv* /*env*/, jobject /*thiz*/) {
    gEngine.stop();
}

JNIEXPORT jboolean JNICALL
Java_nichelooper_audio_AudioEngine_nativeIsRunning(JNIEnv* /*env*/, jobject /*thiz*/) {
    return gEngine.isRunning() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_nichelooper_audio_AudioEngine_nativeIsDisconnected(JNIEnv* /*env*/, jobject /*thiz*/) {
    return gEngine.isDisconnected() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_nichelooper_audio_AudioEngine_nativeSendCommand(
        JNIEnv* /*env*/, jobject /*thiz*/, jint command) {
    gEngine.looper().sendCommand(static_cast<LooperCommand>(command));
}

JNIEXPORT jint JNICALL
Java_nichelooper_audio_AudioEngine_nativeGetState(JNIEnv* /*env*/, jobject /*thiz*/) {
    return static_cast<jint>(gEngine.looper().state());
}

JNIEXPORT jint JNICALL
Java_nichelooper_audio_AudioEngine_nativeGetPositionFrames(JNIEnv* /*env*/, jobject /*thiz*/) {
    return gEngine.looper().positionFrames();
}

JNIEXPORT jint JNICALL
Java_nichelooper_audio_AudioEngine_nativeGetLoopLengthFrames(JNIEnv* /*env*/, jobject /*thiz*/) {
    return gEngine.looper().loopLengthFrames();
}

JNIEXPORT jint JNICALL
Java_nichelooper_audio_AudioEngine_nativeGetMaxLoopFrames(JNIEnv* /*env*/, jobject /*thiz*/) {
    return gEngine.looper().maxLoopFrames();
}

JNIEXPORT jint JNICALL
Java_nichelooper_audio_AudioEngine_nativeGetSampleRate(JNIEnv* /*env*/, jobject /*thiz*/) {
    return gEngine.sampleRate();
}

JNIEXPORT jint JNICALL
Java_nichelooper_audio_AudioEngine_nativeGetFramesPerBurst(JNIEnv* /*env*/, jobject /*thiz*/) {
    return gEngine.framesPerBurst();
}

JNIEXPORT jboolean JNICALL
Java_nichelooper_audio_AudioEngine_nativeLoadLoop(
        JNIEnv* env, jobject /*thiz*/, jfloatArray samples) {
    const jsize length = env->GetArrayLength(samples);
    jfloat* ptr = env->GetFloatArrayElements(samples, nullptr);
    if (ptr == nullptr) {
        return JNI_FALSE;
    }
    const bool ok = gEngine.loadLoop(ptr, static_cast<int32_t>(length));
    env->ReleaseFloatArrayElements(samples, ptr, JNI_ABORT);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_nichelooper_audio_AudioEngine_nativeCopyLoop(
        JNIEnv* env, jobject /*thiz*/, jfloatArray dest) {
    const jsize capacity = env->GetArrayLength(dest);
    jfloat* ptr = env->GetFloatArrayElements(dest, nullptr);
    if (ptr == nullptr) {
        return 0;
    }
    const int32_t copied = gEngine.copyLoop(ptr, static_cast<int32_t>(capacity));
    env->ReleaseFloatArrayElements(dest, ptr, 0);
    return copied;
}

JNIEXPORT jfloat JNICALL
Java_nichelooper_audio_AudioEngine_nativeReadInputPeak(JNIEnv* /*env*/, jobject /*thiz*/) {
    return gEngine.readInputPeak();
}

JNIEXPORT jfloat JNICALL
Java_nichelooper_audio_AudioEngine_nativeReadFxPeak(JNIEnv* /*env*/, jobject /*thiz*/) {
    return gEngine.readFxPeak();
}

JNIEXPORT jfloat JNICALL
Java_nichelooper_audio_AudioEngine_nativeReadOutputPeak(JNIEnv* /*env*/, jobject /*thiz*/) {
    return gEngine.readOutputPeak();
}

JNIEXPORT void JNICALL
Java_nichelooper_audio_AudioEngine_nativeSetMonitor(
        JNIEnv* /*env*/, jobject /*thiz*/, jboolean enabled) {
    gEngine.setMonitorEnabled(enabled == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_nichelooper_audio_AudioEngine_nativeSetInputGain(
        JNIEnv* /*env*/, jobject /*thiz*/, jfloat gain) {
    gEngine.setInputGain(gain);
}

JNIEXPORT void JNICALL
Java_nichelooper_audio_AudioEngine_nativeSetOutputGain(
        JNIEnv* /*env*/, jobject /*thiz*/, jfloat gain) {
    gEngine.setOutputGain(gain);
}

JNIEXPORT void JNICALL
Java_nichelooper_audio_AudioEngine_nativeSetBpm(
        JNIEnv* /*env*/, jobject /*thiz*/, jint bpm) {
    gEngine.looper().rhythm().setBpm(bpm);
}

JNIEXPORT void JNICALL
Java_nichelooper_audio_AudioEngine_nativeSetMetronome(
        JNIEnv* /*env*/, jobject /*thiz*/, jboolean enabled) {
    gEngine.looper().rhythm().setMetronomeEnabled(enabled == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_nichelooper_audio_AudioEngine_nativeSetDrums(
        JNIEnv* /*env*/, jobject /*thiz*/, jboolean enabled) {
    gEngine.looper().rhythm().setDrumsEnabled(enabled == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_nichelooper_audio_AudioEngine_nativeSetDrumsMuted(
        JNIEnv* /*env*/, jobject /*thiz*/, jboolean muted) {
    gEngine.looper().rhythm().setDrumsMuted(muted == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_nichelooper_audio_AudioEngine_nativeSetTimeSignature(
        JNIEnv* /*env*/, jobject /*thiz*/, jint index) {
    gEngine.looper().rhythm().setTimeSignature(index);
}

JNIEXPORT void JNICALL
Java_nichelooper_audio_AudioEngine_nativeSetGroove(
        JNIEnv* /*env*/, jobject /*thiz*/, jint index) {
    gEngine.looper().rhythm().setGroove(index);
}

// The four calls below read compile-time tables, not engine state, so they
// work before start() — the UI builds its menus from them.

JNIEXPORT jobjectArray JNICALL
Java_nichelooper_audio_AudioEngine_nativeGetTimeSignatureNames(
        JNIEnv* env, jobject /*thiz*/) {
    std::vector<std::string> names;
    names.reserve(RhythmSection::kNumTimeSignatures);
    for (int32_t i = 0; i < RhythmSection::kNumTimeSignatures; ++i) {
        names.emplace_back(RhythmSection::timeSignatureName(i));
    }
    return toStringArray(env, names);
}

JNIEXPORT jintArray JNICALL
Java_nichelooper_audio_AudioEngine_nativeGetBeatsPerBar(JNIEnv* env, jobject /*thiz*/) {
    jint beats[RhythmSection::kNumTimeSignatures];
    for (int32_t i = 0; i < RhythmSection::kNumTimeSignatures; ++i) {
        beats[i] = RhythmSection::beatsPerBar(i);
    }
    jintArray result = env->NewIntArray(RhythmSection::kNumTimeSignatures);
    env->SetIntArrayRegion(result, 0, RhythmSection::kNumTimeSignatures, beats);
    return result;
}

JNIEXPORT jobjectArray JNICALL
Java_nichelooper_audio_AudioEngine_nativeGetGrooveNames(JNIEnv* env, jobject /*thiz*/) {
    std::vector<std::string> names;
    names.reserve(RhythmSection::kNumGrooves);
    for (int32_t i = 0; i < RhythmSection::kNumGrooves; ++i) {
        names.emplace_back(RhythmSection::grooveName(i));
    }
    return toStringArray(env, names);
}

JNIEXPORT jintArray JNICALL
Java_nichelooper_audio_AudioEngine_nativeGetGrooveTimeSignatures(
        JNIEnv* env, jobject /*thiz*/) {
    jint sigs[RhythmSection::kNumGrooves];
    for (int32_t i = 0; i < RhythmSection::kNumGrooves; ++i) {
        sigs[i] = RhythmSection::grooveTimeSignature(i);
    }
    jintArray result = env->NewIntArray(RhythmSection::kNumGrooves);
    env->SetIntArrayRegion(result, 0, RhythmSection::kNumGrooves, sigs);
    return result;
}

JNIEXPORT void JNICALL
Java_nichelooper_audio_AudioEngine_nativeSetRhythmVolume(
        JNIEnv* /*env*/, jobject /*thiz*/, jfloat volume) {
    gEngine.looper().rhythm().setVolume(volume);
}

JNIEXPORT void JNICALL
Java_nichelooper_audio_AudioEngine_nativeSetCountIn(
        JNIEnv* /*env*/, jobject /*thiz*/, jboolean enabled) {
    gEngine.looper().setCountInEnabled(enabled == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_nichelooper_audio_AudioEngine_nativeSetLoopVolume(
        JNIEnv* /*env*/, jobject /*thiz*/, jfloat gain) {
    gEngine.looper().setLoopGain(gain);
}

JNIEXPORT void JNICALL
Java_nichelooper_audio_AudioEngine_nativeSetAutoLoopBars(
        JNIEnv* /*env*/, jobject /*thiz*/, jint bars) {
    gEngine.looper().setAutoLoopBars(bars);
}

JNIEXPORT void JNICALL
Java_nichelooper_audio_AudioEngine_nativeSetDrumSamples(
        JNIEnv* env, jobject /*thiz*/, jfloatArray kick, jfloatArray snare,
        jfloatArray hat, jint sourceRate) {
    gEngine.setDrumSamples(toVector(env, kick), toVector(env, snare),
                           toVector(env, hat), sourceRate);
}

// ---- Plugin chains (Audio Unit hosting) ----

JNIEXPORT jobjectArray JNICALL
Java_nichelooper_audio_AudioEngine_nativeGetEffectPluginNames(JNIEnv* env, jobject /*thiz*/) {
    return toStringArray(env, gEngine.plugins().availablePluginNames());
}

JNIEXPORT jboolean JNICALL
Java_nichelooper_audio_AudioEngine_nativeChainAddPlugin(
        JNIEnv* /*env*/, jobject /*thiz*/, jint chain, jint pluginIndex) {
    return gEngine.plugins().addPlugin(chain, pluginIndex) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_nichelooper_audio_AudioEngine_nativeChainRemovePlugin(
        JNIEnv* /*env*/, jobject /*thiz*/, jint chain, jint slot) {
    return gEngine.plugins().removePlugin(chain, slot) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_nichelooper_audio_AudioEngine_nativeChainMovePlugin(
        JNIEnv* /*env*/, jobject /*thiz*/, jint chain, jint from, jint to) {
    return gEngine.plugins().movePlugin(chain, from, to) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jobjectArray JNICALL
Java_nichelooper_audio_AudioEngine_nativeChainGetPluginNames(
        JNIEnv* env, jobject /*thiz*/, jint chain) {
    return toStringArray(env, gEngine.plugins().chainPluginNames(chain));
}

JNIEXPORT void JNICALL
Java_nichelooper_audio_AudioEngine_nativeOpenPluginEditor(
        JNIEnv* /*env*/, jobject /*thiz*/, jint chain, jint slot) {
    gEngine.plugins().openEditor(chain, slot);
}

JNIEXPORT void JNICALL
Java_nichelooper_audio_AudioEngine_nativeSetActiveChain(
        JNIEnv* /*env*/, jobject /*thiz*/, jint index) {
    gEngine.plugins().setActiveChain(index);
}

JNIEXPORT jint JNICALL
Java_nichelooper_audio_AudioEngine_nativeGetActiveChain(JNIEnv* /*env*/, jobject /*thiz*/) {
    return gEngine.plugins().activeChain();
}

JNIEXPORT jstring JNICALL
Java_nichelooper_audio_AudioEngine_nativeGetChainDebugReport(JNIEnv* env, jobject /*thiz*/) {
    return env->NewStringUTF(gEngine.plugins().debugReport().c_str());
}

// ---- Preset bank (all 3 chains: identity + full AU state) ----

JNIEXPORT jbyteArray JNICALL
Java_nichelooper_audio_AudioEngine_nativeSaveBank(JNIEnv* env, jobject /*thiz*/) {
    const auto bytes = gEngine.plugins().saveBank();
    jbyteArray arr = env->NewByteArray(static_cast<jsize>(bytes.size()));
    if (arr != nullptr && !bytes.empty()) {
        env->SetByteArrayRegion(arr, 0, static_cast<jsize>(bytes.size()),
                                reinterpret_cast<const jbyte*>(bytes.data()));
    }
    return arr;
}

JNIEXPORT jboolean JNICALL
Java_nichelooper_audio_AudioEngine_nativeLoadBank(
        JNIEnv* env, jobject /*thiz*/, jbyteArray data) {
    const jsize len = env->GetArrayLength(data);
    jbyte* ptr = env->GetByteArrayElements(data, nullptr);
    if (ptr == nullptr) {
        return JNI_FALSE;
    }
    const bool ok = gEngine.plugins().loadBank(
            reinterpret_cast<const uint8_t*>(ptr), static_cast<size_t>(len));
    env->ReleaseByteArrayElements(data, ptr, JNI_ABORT);
    return ok ? JNI_TRUE : JNI_FALSE;
}

}  // extern "C"
