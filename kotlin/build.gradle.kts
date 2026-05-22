plugins {
    kotlin("jvm") version "2.2.0"
    application
    id("com.vanniktech.maven.publish") version "0.30.0"
    id("org.jlleitschuh.gradle.ktlint") version "12.1.2"
}

group = "io.github.nyxis"
version = providers.environmentVariable("RELEASE_VERSION").orElse("1.0.0").get()

repositories { mavenCentral() }

dependencies {
    implementation("org.json:json:20240303")
}

application {
    mainClass.set("nxs.TestKt")
}

tasks.register<JavaExec>("bench") {
    group = "application"
    classpath = sourceSets["main"].runtimeClasspath
    mainClass.set("nxs.BenchKt")
    args =
        listOf(
            project.findProperty("benchFixtures") as String? ?: "../../nyxis/site/bench/fixtures",
        )
}

tasks.register<JavaExec>("conformance") {
    group = "verification"
    description = "Run NXS conformance vectors (-PconformanceDir=…, default ../nyxis/conformance in monorepo)"
    classpath = sourceSets["main"].runtimeClasspath
    mainClass.set("nxs.ConformanceKt")
    val conformanceDir =
        project.findProperty("conformanceDir") as String? ?: "../../nyxis/conformance/"
    args(conformanceDir)
}

kotlin {
    jvmToolchain(21)
}

tasks.withType<JavaCompile> {
    options.release.set(21)
}

// ── Maven Central (Sonatype Central Portal) ───────────────────────────────────
// OSSRH (s01.oss.sonatype.org) was sunset 2025-06-30. Publishing now goes
// through https://central.sonatype.com via the vanniktech plugin.

mavenPublishing {
    publishToMavenCentral(com.vanniktech.maven.publish.SonatypeHost.CENTRAL_PORTAL, automaticRelease = true)
    signAllPublications()

    coordinates("io.github.nyxis", "nyxis", version.toString())

    pom {
        name.set("nyxis")
        description.set("Zero-copy reader for the Nyxis (NXS) binary format")
        url.set("https://github.com/nyxis-io/nyxis-drivers")
        licenses {
            license {
                name.set("MIT License")
                url.set("https://spdx.org/licenses/MIT.html")
            }
        }
        developers {
            developer {
                id.set("nyxis")
                name.set("Micael Malta")
            }
        }
        scm {
            connection.set("scm:git:git://github.com/nyxis-io/nyxis-drivers.git")
            developerConnection.set("scm:git:ssh://github.com/nyxis-io/nyxis-drivers.git")
            url.set("https://github.com/nyxis-io/nyxis-drivers")
        }
    }
}
