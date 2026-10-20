// The eudisk command-line tool (tools/eudisk), run as a child process: every
// read and change goes through it, so the window and the command line agree.
import Foundation

struct EudiskError: LocalizedError {
    let message: String
    var errorDescription: String? { message }
}

struct Partition: Decodable, Identifiable, Hashable {
    let name: String
    let start: UInt64
    let sectors: UInt64
    let efs: Bool
    let swap: Bool?
    let swap_pages: Int?
    let label: String?
    let files: Int?
    let dirs: Int?
    let blocks: Int?
    let used_blocks: Int?
    let free_inodes: Int?
    let replayed: Int?
    var id: String { name }
    var isSwap: Bool { swap ?? false }
    var megabytes: UInt64 { (sectors * 512) >> 20 }
    var usedFraction: Double {
        guard let b = blocks, let u = used_blocks, b > 0 else { return 0 }
        return Double(u) / Double(b)
    }
}

struct Kernel: Decodable, Identifiable, Hashable {
    let slot: String
    let present: Bool
    let valid: Bool
    let size: UInt64
    let built: String
    var id: String { slot }
}

struct ImageInfo: Decodable {
    let image: String
    let size: UInt64
    let gpt: Bool
    let in_use: Bool
    let partitions: [Partition]
    let kernels: [Kernel]
}

struct Entry: Decodable, Identifiable, Hashable {
    let name: String
    let path: String
    let type: String
    let mode: Int
    let uid: Int
    let gid: Int
    let nlink: Int
    let size: UInt64
    let mtime: TimeInterval
    let ino: Int
    var id: String { path }
    var isDirectory: Bool { type == "dir" }
    var modeString: String {
        let bits = Array("rwxrwxrwx")
        var s = isDirectory ? "d" : "-"
        for i in 0..<9 { s.append(mode & (0o400 >> i) != 0 ? bits[i] : "-") }
        return s
    }
    var modified: Date { Date(timeIntervalSince1970: mtime) }
}

struct Eudisk {
    let tool: URL
    let image: URL

    /// The eudisk binary: $EUDISK, beside this program, or build/tools/eudisk
    /// in a directory above the image or this program.
    static func find(near image: URL?) -> URL? {
        let fm = FileManager.default
        if let env = ProcessInfo.processInfo.environment["EUDISK"], fm.isExecutableFile(atPath: env) {
            return URL(fileURLWithPath: env)
        }
        var starts: [URL] = []
        let me = URL(fileURLWithPath: CommandLine.arguments[0]).resolvingSymlinksInPath().deletingLastPathComponent()
        if fm.isExecutableFile(atPath: me.appendingPathComponent("eudisk").path) {
            return me.appendingPathComponent("eudisk")
        }
        if let image { starts.append(image.deletingLastPathComponent()) }
        starts.append(me)
        for start in starts {
            var dir = start
            for _ in 0..<8 {
                let candidate = dir.appendingPathComponent("build/tools/eudisk")
                if fm.isExecutableFile(atPath: candidate.path) { return candidate }
                dir = dir.deletingLastPathComponent()
            }
        }
        return nil
    }

    /// Run eudisk with these arguments; its standard output, or an error with
    /// what it said on standard error.
    func run(_ args: [String]) async throws -> Data {
        try await withCheckedThrowingContinuation { cont in
            DispatchQueue.global(qos: .userInitiated).async {
                let p = Process()
                p.executableURL = tool
                p.arguments = args
                let out = Pipe(), err = Pipe()
                p.standardOutput = out
                p.standardError = err
                do {
                    try p.run()
                } catch {
                    cont.resume(throwing: error)
                    return
                }
                // read both before waiting, so a full pipe cannot stall it
                var outData = Data(), errData = Data()
                let group = DispatchGroup()
                group.enter()
                DispatchQueue.global().async {
                    errData = err.fileHandleForReading.readDataToEndOfFile()
                    group.leave()
                }
                outData = out.fileHandleForReading.readDataToEndOfFile()
                group.wait()
                p.waitUntilExit()
                if p.terminationStatus == 0 {
                    cont.resume(returning: outData)
                } else {
                    let msg = String(decoding: errData.isEmpty ? outData : errData, as: UTF8.self)
                        .trimmingCharacters(in: .whitespacesAndNewlines)
                    cont.resume(throwing: EudiskError(message: msg.isEmpty ? "eudisk failed (status \(p.terminationStatus))" : msg))
                }
            }
        }
    }

    func text(_ args: [String]) async throws -> String {
        String(decoding: try await run(args), as: UTF8.self)
    }

    /// IMAGE:PART:PATH
    func place(_ part: String, _ path: String) -> String {
        "\(image.path):\(part):\(path)"
    }

    func info() async throws -> ImageInfo {
        try JSONDecoder().decode(ImageInfo.self, from: try await run(["info", "--json", image.path]))
    }

    func list(_ part: String, _ path: String) async throws -> [Entry] {
        try JSONDecoder().decode([Entry].self, from: try await run(["ls", "--json", place(part, path)]))
    }

    func cat(_ part: String, _ path: String) async throws -> Data {
        try await run(["cat", place(part, path)])
    }
}

extension String {
    /// /a/b + c = /a/b/c
    func joined(_ name: String) -> String {
        self == "/" ? "/" + name : self + "/" + name
    }
    var parentPath: String {
        guard self != "/", let slash = lastIndex(of: "/") else { return "/" }
        let p = String(self[..<slash])
        return p.isEmpty ? "/" : p
    }
}
