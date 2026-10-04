package nichelooper.platform

object Platform {
    private val osName: String = System.getProperty("os.name").orEmpty()

    val isMac: Boolean = osName.startsWith("Mac")
    val isLinux: Boolean = osName == "Linux"
}
