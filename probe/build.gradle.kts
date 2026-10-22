plugins {
    id("org.jetbrains.kotlin.jvm")
    id("application")
}

java {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
}

kotlin {
    compilerOptions { jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17) }
}

sourceSets {
    main {
        kotlin.srcDirs(rootProject.file("tools/probe"))
        kotlin.srcDir(rootProject.file("app/src/main/java/app/aemu/importer"))
        kotlin {
            exclude("Importer.kt", "Analyzer.kt", "BootImage.kt", "ErofsImage.kt", "StorageList.kt")
        }
    }
}

dependencies {
    implementation("org.apache.commons:commons-compress:1.28.0")
    implementation("org.tukaani:xz:1.10")
    implementation("org.brotli:dec:0.1.2")
}

application {
    mainClass.set("app.aemu.importer.ImportProbeKt")
}

tasks.named<JavaExec>("run") {
    maxHeapSize = "3g"
    args = (project.findProperty("firmware")?.toString()?.let { listOf(it) } ?: emptyList()) +
        listOf(project.findProperty("work")?.toString() ?: "build/probe-work")
}
