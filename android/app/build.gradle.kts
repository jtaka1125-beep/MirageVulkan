plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.mirage.android"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.mirage.android"
        minSdk = 29  // Android 10+ for AudioPlaybackCapture
        targetSdk = 34
        versionCode = 2
        versionName = "2.1.0-audio"
    }

    signingConfigs {
        create("release") {
            storeFile = file("../mirage.keystore")
            storePassword = "mirage123"
            keyAlias = "mirage"
            keyPassword = "mirage123"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfig = signingConfigs.getByName("release")
        }
        debug {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.12.0")
    implementation("androidx.appcompat:appcompat:1.6.1")
    implementation("com.google.android.material:material:1.11.0")
    implementation("androidx.localbroadcastmanager:localbroadcastmanager:1.1.0")
}
