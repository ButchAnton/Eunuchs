// swift-tools-version:5.9
// Eudisk: a Mac window onto Eunuchs disk images, over tools/eudisk.
// `make gui` builds it and opens disk.img.
import PackageDescription

let package = Package(
    name: "Eudisk",
    platforms: [.macOS(.v14)],
    targets: [
        .executableTarget(name: "Eudisk", path: "Sources/Eudisk")
    ]
)
