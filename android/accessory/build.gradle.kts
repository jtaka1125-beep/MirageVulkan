plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.mirage.accessory"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.mirage.accessory"
        minSdk = 29
        targetSdk = 33
        versionCode = 1
        versionName = "1.0.0"
    }

    signingConfigs {
        create("release") {
            storeFile = file("../mirage.keystore")
            storePassword = System.getenv("MIRAGE_KEYSTORE_PASS") ?: ""
            keyAlias = System.getenv("MIRAGE_KEY_ALIAS") ?: "mirage"
            keyPassword = System.getenv("MIRAGE_KEY_PASS") ?: ""
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
