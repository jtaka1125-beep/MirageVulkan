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
// :app (Legacy unified monolith) excluded 2026-02-24. Replaced by :capture + :accessory.
// Sources remain in android/app/ for reference. Do NOT re-include without discussion.
include(":capture")     // MirageCapture - screen capture + video sending
include(":accessory")   // MirageAccessory - AOA + command receiving
