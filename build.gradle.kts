import org.jetbrains.compose.desktop.application.dsl.TargetFormat
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    kotlin("jvm") version "2.3.21"
    id("org.jetbrains.kotlin.plugin.compose") version "2.3.21"
    id("org.jetbrains.compose") version "1.11.1"
}

repositories {
    mavenCentral()
    google()
}

kotlin {
    jvmToolchain(21)
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_21)
    }
}

dependencies {
    implementation(compose.desktop.currentOs)
    implementation(compose.material3)
}

// ---- Native engine (C++ core + miniaudio/CoreAudio + JNI bridge) ----

// JNI headers: the Gradle JVM (JBR) may not ship them, so fall back to the
// system JDK reported by /usr/libexec/java_home.
fun findJniHome(): File {
    val current = File(System.getProperty("java.home"))
    if (File(current, "include/jni.h").exists()) return current
    val process = ProcessBuilder("/usr/libexec/java_home").start()
    val home = process.inputStream.bufferedReader().readText().trim()
    check(process.waitFor() == 0 && home.isNotEmpty()) {
        "No JDK with JNI headers found (checked \$java.home and /usr/libexec/java_home)"
    }
    return File(home)
}

val nativeResourceDir = layout.buildDirectory.dir("generated/nativeResources")

val buildNative = tasks.register<Exec>("buildNative") {
    val jniHome = findJniHome()
    val sources = listOf(
        "LooperEngine.cpp",
        "RhythmSection.cpp",
        "MacAudioEngine.cpp",
        "AuPluginChain.mm",
        "miniaudio_impl.cpp",
        "jni_bridge.cpp",
    )
    val outputFile = nativeResourceDir.get().file("native/libnichelooper.dylib").asFile

    inputs.files(fileTree("native") { include("*.cpp", "*.h", "*.mm") })
    outputs.file(outputFile)

    doFirst { outputFile.parentFile.mkdirs() }
    workingDir = projectDir
    commandLine(
        listOf(
            "clang++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-dynamiclib",
            "-I", "${jniHome}/include", "-I", "${jniHome}/include/darwin",
        )
            + sources.map { "native/$it" }
            + listOf(
            "-framework", "CoreFoundation",
            "-framework", "CoreAudio",
            "-framework", "AudioToolbox",
            "-framework", "AudioUnit",
            "-framework", "CoreAudioKit",
            "-framework", "Cocoa",
            "-o", outputFile.absolutePath,
        )
    )
}

sourceSets["main"].resources.srcDir(nativeResourceDir)
tasks.named("processResources") { dependsOn(buildNative) }

compose.desktop {
    application {
        mainClass = "nichelooper.MainKt"

        // jpackage lives in the system JDK (the Gradle JBR does not ship it);
        // the packaged app bundles that JDK's runtime via jlink.
        javaHome = findJniHome().absolutePath

        nativeDistributions {
            targetFormats(TargetFormat.Dmg)
            packageName = "NicheLooper"
            packageVersion = "1.0.0"
            macOS {
                bundleID = "com.example.nichelooper"
                infoPlist {
                    extraKeysRawXml = """
                        <key>NSMicrophoneUsageDescription</key>
                        <string>NicheLooper nimmt Audio von deinem Audio-Interface auf.</string>
                    """.trimIndent()
                }
            }
        }
    }
}
