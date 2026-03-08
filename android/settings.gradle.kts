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
// include(":accessory")   // Merged into :capture on 2026-03-08. Sources remain for reference.   // MirageAccessory - AOA + command receiving
