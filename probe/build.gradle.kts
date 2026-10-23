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
        kotlin.srcDir(rootProject.file("tools/probe/ImportProbe.kt"))
        kotlin.srcDir(rootProject.file("tools/probe/HuaweiProbe.kt"))
        kotlin.srcDir(rootProject.file("app/src/main/java/app/aemu/importer"))
        kotlin {
            exclude("Importer.kt", "Analyzer.kt", "BootImage.kt", "StorageList.kt")
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
    systemProperty("file.encoding", "UTF-8")
    args = (project.findProperty("firmware")?.toString()?.let { listOf(it) } ?: emptyList()) +
        listOf(project.findProperty("work")?.toString() ?: "build/probe-work")
}

tasks.register<JavaExec>("probeHuawei") {
    group = "verification"
    mainClass.set("HuaweiProbeKt")
    classpath = sourceSets["main"].runtimeClasspath
    maxHeapSize = "3g"
    systemProperty("file.encoding", "UTF-8")
    args = (project.findProperty("firmware")?.toString()?.let { listOf(it) } ?: emptyList()) +
        listOf(project.findProperty("work")?.toString() ?: "build/probe-work")
}