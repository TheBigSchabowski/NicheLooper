package nichelooper

import nichelooper.platform.UserDirs
import java.io.File
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull

class UserDirsTest {
    private val home = "/home/linus"

    private val german = """
        # This file is written by xdg-user-dirs-update
        XDG_DESKTOP_DIR="${'$'}HOME/Schreibtisch"
        XDG_DOWNLOAD_DIR="${'$'}HOME/Downloads"
        XDG_MUSIC_DIR="${'$'}HOME/Musik"
        XDG_PUBLICSHARE_DIR="/mnt/musik"
        XDG_VIDEOS_DIR="${'$'}HOME/"
    """.trimIndent()

    @Test
    fun aFolderBelowHomeIsReadWithItsTranslatedName() {
        assertEquals("/home/linus/Musik", UserDirs.resolve(german, "XDG_MUSIC_DIR", home))
    }

    @Test
    fun anAbsolutePathIsTakenAsItIs() {
        assertEquals("/mnt/musik", UserDirs.resolve(german, "XDG_PUBLICSHARE_DIR", home))
    }

    @Test
    fun theHomeFolderItselfMeansTheFolderIsSwitchedOff() {
        assertNull(UserDirs.resolve(german, "XDG_VIDEOS_DIR", home))
    }

    @Test
    fun aMissingKeyOrAMissingFileIsNull() {
        assertNull(UserDirs.resolve(german, "XDG_PICTURES_DIR", home))
        assertNull(UserDirs.resolve(null, "XDG_MUSIC_DIR", home))
    }

    @Test
    fun bracesAroundHomeAndCommentsAreFine() {
        val text = "# XDG_MUSIC_DIR=\"/nope\"\nXDG_MUSIC_DIR=\"${'$'}{HOME}/Sounds\"\n"
        assertEquals("/home/linus/Sounds", UserDirs.resolve(text, "XDG_MUSIC_DIR", home))
    }

    @Test
    fun theHomeVariableWinsOverJavasOwnGuess() {
        assertEquals("/home/linus", UserDirs.homeFolder("/home/linus", "/root"))
        assertEquals("/root", UserDirs.homeFolder(null, "/root"))
        assertEquals("/root", UserDirs.homeFolder("", "/root"))
        assertEquals("/root", UserDirs.homeFolder("relative/dir", "/root"))
    }

    @Test
    fun baseDirectoriesCountOnlyWhenAbsolute() {
        val homeDir = File(home)
        assertEquals(File("/data/share"), UserDirs.baseDir("/data/share", homeDir, ".local/share"))
        assertEquals(File("/home/linus/.local/share"), UserDirs.baseDir(null, homeDir, ".local/share"))
        assertEquals(File("/home/linus/.local/share"), UserDirs.baseDir("", homeDir, ".local/share"))
        assertEquals(File("/home/linus/.local/share"), UserDirs.baseDir("share", homeDir, ".local/share"))
    }
}
