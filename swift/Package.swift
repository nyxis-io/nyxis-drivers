// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "nyxis",
    products: [
        .library(name: "NXS", targets: ["NXS"]),
    ],
    targets: [
        .target(name: "NXS", path: "Sources/NXS"),
        .executableTarget(name: "nxs-test", dependencies: ["NXS"], path: "Sources/Test"),
        .executableTarget(name: "nxs-bench", dependencies: ["NXS"], path: "Sources/Bench"),
        .executableTarget(name: "nxs-conformance", dependencies: ["NXS"], path: "Sources/Conformance"),
    ]
)
