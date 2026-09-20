plugins {
  alias(libs.plugins.android.application)
  alias(libs.plugins.compose.compiler)
  alias(libs.plugins.kotlin.serialization)
}

android {
    namespace = "com.claudemonitor"
    compileSdk = 36
    defaultConfig {
        applicationId = "com.claudemonitor"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"
    }

    // Підпис релізу.
    //
    // Сховище ключів НЕ лежить у репозиторії: шлях і паролі беруться зі
    // змінних середовища або local.properties, якого немає в git
    // (Частина 3 §23, Частина 6 §38 Master Prompt). Без них збирається
    // непідписаний APK — це помітно одразу, на відміну від мовчазного
    // використання тестового ключа.
    val keystorePath: String? = System.getenv("CM_KEYSTORE")
        ?: project.findProperty("cm.keystore") as String?

    signingConfigs {
        if (keystorePath != null && file(keystorePath).exists()) {
            create("release") {
                storeFile = file(keystorePath)
                storePassword = System.getenv("CM_KEYSTORE_PASSWORD")
                    ?: project.findProperty("cm.keystore.password") as String?
                keyAlias = System.getenv("CM_KEY_ALIAS")
                    ?: project.findProperty("cm.key.alias") as String? ?: "claude-monitor"
                keyPassword = System.getenv("CM_KEY_PASSWORD")
                    ?: project.findProperty("cm.key.password") as String?
            }
        }
    }

    buildTypes {
        release {
            // R8 прибирає невикористаний код і скорочує APK
            // (Частина 4 §30 Master Prompt).
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")

            signingConfig = signingConfigs.findByName("release")
        }

        debug {
            // Окремий суфікс дозволяє тримати обидві збірки на одному
            // телефоні: зручно перевіряти оновлення.
            applicationIdSuffix = ".debug"
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    buildFeatures {
      compose = true
      aidl = false
      buildConfig = false
      shaders = false
    }

    packaging {
      resources {
        excludes += "/META-INF/{AL2.0,LGPL2.1}"
      }
    }
}

kotlin {
    jvmToolchain(17)
}

dependencies {
  val composeBom = platform(libs.androidx.compose.bom)
  implementation(composeBom)
  androidTestImplementation(composeBom)

  // Core Android dependencies
  implementation(libs.androidx.core.ktx)
  implementation(libs.androidx.lifecycle.runtime.ktx)
  implementation(libs.androidx.activity.compose)

  // Arch Components
  implementation(libs.androidx.lifecycle.runtime.compose)
  implementation(libs.androidx.lifecycle.viewmodel.compose)

  // Compose
  implementation(libs.androidx.compose.ui)
  implementation(libs.androidx.compose.ui.tooling.preview)
  implementation(libs.androidx.compose.material3)
  // Tooling
  debugImplementation(libs.androidx.compose.ui.tooling)
  // Instrumented tests
  androidTestImplementation(libs.androidx.compose.ui.test.junit4)
  debugImplementation(libs.androidx.compose.ui.test.manifest)

  // Мережа: єдина зовнішня залежність застосунку
  implementation(libs.okhttp)

  // Local tests: jUnit, coroutines, Android runner
  // Справжня реалізація org.json для тестів на JVM: у складі Android
  // цей пакет є, але в юніт-тестах підставляється заглушка, яка кидає
  // виняток на кожен виклик.
  testImplementation(libs.json.java)
  testImplementation(libs.junit)
  testImplementation(libs.kotlinx.coroutines.test)

  // Instrumented tests: jUnit rules and runners
  androidTestImplementation(libs.androidx.test.core)
  androidTestImplementation(libs.androidx.test.ext.junit)
  androidTestImplementation(libs.androidx.test.runner)
  androidTestImplementation(libs.androidx.test.espresso.core)

}
