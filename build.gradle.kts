import org.jetbrains.compose.desktop.application.dsl.TargetFormat
import org.jetbrains.compose.desktop.application.tasks.AbstractJPackageTask
import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import java.util.concurrent.Callable
import java.util.concurrent.Executors

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
    // .m4a loops on systems without macOS's afconvert (see M4aDecoder.kt)
    implementation("com.tianscar.javasound:jaad:0.9.4")

    testImplementation(kotlin("test"))
}

tasks.test { useJUnitPlatform() }

// ---- Native engine (C++ core + miniaudio + JNI bridge) ----
// macOS: CoreAudio + Audio Unit hosting (clang++). Linux: PulseAudio/ALSA/JACK + VST3 hosting
// (g++, VST3 SDK from the third_party/vst3sdk submodule).

val onMac = System.getProperty("os.name").startsWith("Mac")
val onLinux = System.getProperty("os.name") == "Linux"

// JNI headers: the Gradle JVM (JBR) may not ship them, so fall back to JAVA_HOME, the system JDK
// reported by /usr/libexec/java_home (macOS) or the JDKs under /usr/lib/jvm (Linux).
fun findJniHome(): File {
    val current = File(System.getProperty("java.home"))
    if (File(current, "include/jni.h").exists()) return current
    val env = System.getenv("JAVA_HOME")
    if (env != null && File(env, "include/jni.h").exists()) return File(env)
    val macJavaHome = File("/usr/libexec/java_home")
    if (macJavaHome.canExecute()) {
        val process = ProcessBuilder(macJavaHome.path).start()
        val home = process.inputStream.bufferedReader().readText().trim()
        if (process.waitFor() == 0 && home.isNotEmpty()) return File(home)
    }
    File("/usr/lib/jvm").listFiles()
        ?.filter { File(it, "include/jni.h").exists() }
        ?.maxByOrNull { it.name }
        ?.let { return it }
    error(
        "No JDK with JNI headers found (checked \$java.home, \$JAVA_HOME, /usr/libexec/java_home and " +
            "/usr/lib/jvm) - on Debian/Ubuntu: sudo apt install openjdk-21-jdk",
    )
}

val nativeResourceDir = layout.buildDirectory.dir("generated/nativeResources")

if (onMac) {
    tasks.register<Exec>("buildNative") {
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
} else if (onLinux) {
    // Our sources plus the VST3 SDK hosting/base translation units the host needs.
    val vst3SdkDir = file("third_party/vst3sdk")
    val linuxSources = listOf(
        "LooperEngine.cpp",
        "RhythmSection.cpp",
        "LinuxAudioEngine.cpp",
        "LinuxVstPluginChain.cpp",
        "miniaudio_impl.cpp",
        "jni_bridge.cpp",
    )
    val vst3SdkSources = listOf(
        "pluginterfaces/base/conststringtable.cpp",
        "pluginterfaces/base/coreiids.cpp",
        "pluginterfaces/base/funknown.cpp",
        "pluginterfaces/base/ustring.cpp",
        "base/source/baseiids.cpp",
        "base/source/fbuffer.cpp",
        "base/source/fdebug.cpp",
        "base/source/fdynlib.cpp",
        "base/source/fobject.cpp",
        "base/source/fstreamer.cpp",
        "base/source/fstring.cpp",
        "base/source/timer.cpp",
        "base/source/updatehandler.cpp",
        "base/thread/source/fcondition.cpp",
        "base/thread/source/flock.cpp",
        "public.sdk/source/common/commoniids.cpp",
        "public.sdk/source/common/commonstringconvert.cpp",
        "public.sdk/source/common/memorystream.cpp",
        "public.sdk/source/common/threadchecker_linux.cpp",
        "public.sdk/source/vst/vstinitiids.cpp",
        "public.sdk/source/vst/utility/stringconvert.cpp",
        "public.sdk/source/vst/hosting/connectionproxy.cpp",
        "public.sdk/source/vst/hosting/eventlist.cpp",
        "public.sdk/source/vst/hosting/hostclasses.cpp",
        "public.sdk/source/vst/hosting/module.cpp",
        "public.sdk/source/vst/hosting/module_linux.cpp",
        "public.sdk/source/vst/hosting/parameterchanges.cpp",
        "public.sdk/source/vst/hosting/pluginterfacesupport.cpp",
        "public.sdk/source/vst/hosting/plugprovider.cpp",
        "public.sdk/source/vst/hosting/processdata.cpp",
    )
    val outputFile = nativeResourceDir.get().file("native/libnichelooper.so").asFile
    val objectDir = layout.buildDirectory.dir("native-obj").get().asFile

    tasks.register("buildNative") {
        inputs.files(fileTree("native") { include("*.cpp", "*.h") })
        inputs.files(vst3SdkSources.map { File(vst3SdkDir, it) })
        outputs.file(outputFile)

        doLast {
            check(File(vst3SdkDir, "pluginterfaces/base/funknown.h").isFile) {
                "VST3 SDK missing - run: git submodule update --init --depth 1 third_party/vst3sdk && " +
                    "git -C third_party/vst3sdk submodule update --init --depth 1 base pluginterfaces public.sdk"
            }
            val jniHome = findJniHome()
            val compiler = System.getenv("CXX") ?: "g++"

            // The SDK is compiled quietly; our own code with warnings.
            val commonFlags = listOf(
                "-std=c++17", "-O2", "-fPIC", "-fvisibility=hidden", "-pthread", "-DNDEBUG", "-DRELEASE=1",
                "-I${file("native").absolutePath}",
                "-I${vst3SdkDir.absolutePath}",
                "-I${jniHome}/include",
                "-I${jniHome}/include/linux",
            )

            fun run(command: List<String>): Pair<Int, String> =
                try {
                    val process = ProcessBuilder(command)
                        .directory(projectDir)
                        .redirectErrorStream(true)
                        .start()
                    val output = process.inputStream.bufferedReader().readText()
                    process.waitFor() to output
                } catch (e: java.io.IOException) {
                    127 to "Cannot run '${command.first()}' (${e.message}) - on Debian/Ubuntu: " +
                        "sudo apt install g++ libx11-dev"
                }

            objectDir.deleteRecursively()
            objectDir.mkdirs()
            outputFile.parentFile.mkdirs()

            val units = linuxSources.map { File(file("native"), it) to false } +
                vst3SdkSources.map { File(vst3SdkDir, it) to true }
            val pool = Executors.newFixedThreadPool(Runtime.getRuntime().availableProcessors().coerceIn(1, 8))
            val objects = mutableListOf<File>()
            val failures = mutableListOf<String>()
            try {
                val jobs = units.map { (source, quiet) ->
                    val objectFile = File(objectDir, source.relativeTo(projectDir).path.replace('/', '_') + ".o")
                    objects += objectFile
                    pool.submit(
                        Callable {
                            val flags = commonFlags + if (quiet) listOf("-w") else listOf("-Wall", "-Wextra")
                            val (exit, output) = run(
                                listOf(compiler) + flags + listOf("-c", source.absolutePath, "-o", objectFile.absolutePath),
                            )
                            if (exit != 0) "${source.name}:\n$output" else null
                        },
                    )
                }
                jobs.forEach { job -> job.get()?.let { failures += it } }
            } finally {
                pool.shutdown()
            }
            if (failures.isNotEmpty()) {
                error("Native build failed:\n" + failures.joinToString("\n"))
            }

            val (exit, output) = run(
                listOf(compiler, "-shared", "-o", outputFile.absolutePath) +
                    objects.map { it.absolutePath } +
                    listOf("-pthread", "-ldl", "-lX11", "-Wl,--no-undefined"),
            )
            if (exit != 0 || !outputFile.isFile) {
                error("Linking ${outputFile.name} failed (exit $exit):\n$output")
            }
            if (output.isNotBlank()) logger.warn(output)
            logger.lifecycle("Built ${outputFile.name} (${outputFile.length() / 1024} KiB)")
        }
    }
} else {
    tasks.register("buildNative") {
        doLast { error("The native engine builds on macOS and Linux only (Windows: WNicheLooper).") }
    }
}

sourceSets["main"].resources.srcDir(nativeResourceDir)
tasks.named("processResources") { dependsOn("buildNative") }

compose.desktop {
    application {
        mainClass = "nichelooper.MainKt"

        // jpackage lives in the system JDK (the Gradle JBR does not ship it);
        // the packaged app bundles that JDK's runtime via jlink.
        javaHome = findJniHome().absolutePath

        nativeDistributions {
            // Each format can only be built on its own operating system: the DMG on the Mac, the DEB on Linux.
            targetFormats(TargetFormat.Dmg, TargetFormat.Deb)
            packageName = "NicheLooper"
            packageVersion = "1.1.2"
            description = "Live-Looper mit Metronom, Drums und Plugin-Effektketten für Gitarre"
            vendor = "TheBigSchabowski"
            // The AAC decoder (jaad) logs through java.logging; jpackage only bundles what it is told to.
            modules("java.desktop", "java.logging")
            macOS {
                bundleID = "com.example.nichelooper"
                infoPlist {
                    extraKeysRawXml = """
                        <key>NSMicrophoneUsageDescription</key>
                        <string>NicheLooper nimmt Audio von deinem Audio-Interface auf.</string>
                    """.trimIndent()
                }
            }
            linux {
                iconFile.set(project.file("icon.png"))
                packageName = "nichelooper"
                debMaintainer = "TheBigSchabowski@users.noreply.github.com"
                appCategory = "sound"
                menuGroup = "AudioVideo;Audio;"
            }
        }
    }
}

// jpackage works out the .deb dependencies from the libraries installed on the build machine. A server or
// container without the X11 and sound libraries the JDK and our native engine load would leave them out, and the
// app would not start on a desktop without them. No spaces in the list: jpackage reads its arguments from a file
// that splits at them.
tasks.withType<AbstractJPackageTask>().configureEach {
    if (targetFormat == TargetFormat.Deb) {
        freeArgs.addAll("--linux-package-deps", "libasound2t64|libasound2,libx11-6,libxtst6,libxi6,libxrandr2")
    }
}

// The same icon.png is the window icon on Linux (see Main.kt).
tasks.processResources { from("icon.png") }
