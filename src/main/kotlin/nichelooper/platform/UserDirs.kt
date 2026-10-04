package nichelooper.platform

import java.io.File

/**
 * The folders a desktop user knows by name. On Linux the music folder is set in `~/.config/user-dirs.dirs`
 * and follows the language ("Musik"); `Music` is the fallback, and what macOS has.
 */
object UserDirs {
    fun home(): File = File(homeFolder(System.getenv("HOME"), System.getProperty("user.home")))

    fun music(): File = lookup("XDG_MUSIC_DIR", "Music")

    /** Where a Linux program keeps what it saves for the user: `$XDG_DATA_HOME`, else `~/.local/share`. */
    fun dataHome(): File = baseDir(System.getenv("XDG_DATA_HOME"), home(), ".local/share")

    private fun lookup(key: String, fallback: String): File {
        val home = home()
        val config = baseDir(System.getenv("XDG_CONFIG_HOME"), home, ".config")
        val text = File(config, "user-dirs.dirs").takeIf { it.isFile }?.readText()
        return resolve(text, key, home.path)?.let(::File) ?: File(home, fallback)
    }

    // Java reads user.home from the password file, while the XDG rules and the shell's environment go by $HOME.
    internal fun homeFolder(env: String?, property: String): String = env?.takeIf { it.startsWith("/") } ?: property

    // The XDG base directory variables only count when they are absolute paths.
    internal fun baseDir(env: String?, home: File, fallback: String): File =
        env?.takeIf { it.startsWith("/") }?.let(::File) ?: File(home, fallback)

    /**
     * The folder [key] is set to in the text of `user-dirs.dirs`, or null if it is not set. The home
     * folder itself means "no such folder" there, which is null as well.
     */
    internal fun resolve(text: String?, key: String, home: String): String? {
        val value = text?.lineSequence()
            ?.map { it.trim() }
            ?.firstNotNullOfOrNull { line ->
                line.takeIf { it.startsWith("$key=") }?.substringAfter('=')?.trim()?.removeSurrounding("\"")
            }
            ?: return null
        val path = when {
            value.startsWith("\$HOME") -> home + value.removePrefix("\$HOME")
            value.startsWith("\${HOME}") -> home + value.removePrefix("\${HOME}")
            else -> value
        }
        return path.takeIf { it.startsWith("/") && it.trimEnd('/') != home.trimEnd('/') }
    }
}
