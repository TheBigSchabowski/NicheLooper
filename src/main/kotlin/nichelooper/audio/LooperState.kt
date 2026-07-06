package nichelooper.audio

/** Mirror of the C++ LooperState enum (LooperEngine.h). Ordinals must match. */
enum class LooperState {
    EMPTY,
    RECORDING,
    PLAYING,
    OVERDUBBING,
    STOPPED,
    COUNT_IN;

    companion object {
        fun fromNative(value: Int): LooperState = entries.getOrElse(value) { EMPTY }
    }
}

/** Mirror of the C++ LooperCommand enum (LooperEngine.h). */
enum class LooperCommand(val nativeValue: Int) {
    RECORD(1),
    PLAY(2),
    OVERDUB(3),
    STOP(4),
    CLEAR(5),
}
