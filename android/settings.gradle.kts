pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "MirageAndroid"
include(":app")         // Legacy unified app (deprecated)
include(":capture")     // MirageCapture - screen capture + video sending
include(":accessory")   // MirageAccessory - AOA + command receiving
