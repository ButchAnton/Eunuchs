// What the window shows: one image, a partition in it and a directory in
// that, kept in step with eudisk.
import AppKit
import Foundation
import Observation

@MainActor
@Observable
final class Model {
    var image: URL?
    var eudisk: Eudisk?
    var info: ImageInfo?
    var partition: String? {
        didSet { if partition != oldValue { path = "/"; Task { await listDirectory() } } }
    }
    var path = "/"
    var entries: [Entry] = []
    var selection = Set<Entry.ID>()
    var preview: String?
    var error: String?
    var report: (title: String, text: String)?
    var busy = false

    var inUse: Bool { info?.in_use ?? false }
    var selectedEntries: [Entry] { entries.filter { selection.contains($0.id) } }

    init() {
        let args = CommandLine.arguments.dropFirst().filter { !$0.hasPrefix("-") }
        if let first = args.first {
            open(URL(fileURLWithPath: first))
        } else if let last = UserDefaults.standard.string(forKey: "image"),
                  FileManager.default.fileExists(atPath: last) {
            open(URL(fileURLWithPath: last))
        }
    }

    func open(_ url: URL) {
        guard let tool = Eudisk.find(near: url) else {
            error = "Cannot find the eudisk tool: build it (make build/tools/eudisk) or set EUDISK."
            return
        }
        image = url
        eudisk = Eudisk(tool: tool, image: url)
        UserDefaults.standard.set(url.path, forKey: "image")
        partition = nil
        Task { await reload() }
    }

    func chooseImage() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.message = "A Eunuchs disk image (disk.img, build/fs.img, ...)"
        if panel.runModal() == .OK, let url = panel.url {
            open(url)
        }
    }

    /// Run something; report a failure in the window.
    func attempt(_ what: () async throws -> Void) async {
        busy = true
        defer { busy = false }
        do {
            try await what()
        } catch {
            self.error = error.localizedDescription
        }
    }

    func reload() async {
        guard let eudisk else { return }
        await attempt {
            let i = try await eudisk.info()
            info = i
            if partition == nil || !i.partitions.contains(where: { $0.name == partition }) {
                partition = i.partitions.first(where: { $0.name == "root" && $0.efs })?.name
                    ?? i.partitions.first(where: { $0.efs })?.name
            }
        }
        await listDirectory()
    }

    func listDirectory() async {
        guard let eudisk, let partition else { entries = []; return }
        await attempt {
            entries = try await eudisk.list(partition, path)
            selection = selection.filter { id in entries.contains { $0.id == id } }
        }
        await updatePreview()
    }

    func go(to newPath: String) {
        path = newPath
        selection = []
        Task { await listDirectory() }
    }

    func activate(_ entry: Entry) {
        if entry.isDirectory { go(to: entry.path) }
    }

    func updatePreview() async {
        guard let eudisk, let partition, selection.count == 1,
              let e = selectedEntries.first, !e.isDirectory else { preview = nil; return }
        if e.size > 4 << 20 {
            preview = "(\(e.size) bytes: too big to show here; Export it)"
            return
        }
        do {
            let data = try await eudisk.cat(partition, e.path)
            preview = Self.describe(data)
        } catch {
            preview = error.localizedDescription
        }
    }

    /// Text as it is; anything else as a hex dump of its start.
    static func describe(_ data: Data) -> String {
        if let s = String(data: data, encoding: .utf8), !s.contains("\0") { return s }
        var out = "binary, \(data.count) bytes\(data.starts(with: [0x7f, 0x45, 0x4c, 0x46]) ? " (ELF)" : "")\n\n"
        let shown = data.prefix(1024)
        for row in stride(from: 0, to: shown.count, by: 16) {
            let bytes = shown[shown.startIndex + row ..< shown.startIndex + min(row + 16, shown.count)]
            let hex = bytes.map { String(format: "%02x", $0) }.joined(separator: " ")
            let ascii = String(bytes.map { $0 >= 32 && $0 < 127 ? Character(UnicodeScalar($0)) : "." })
            out += String(format: "%06x  ", row) + hex.padding(toLength: 48, withPad: " ", startingAt: 0) + "  " + ascii + "\n"
        }
        return out
    }

    // ---- changes ----

    func change(_ args: [String], title: String? = nil) async {
        guard let eudisk else { return }
        await attempt {
            let out = try await eudisk.text(args)
            if let title, !out.isEmpty { report = (title, out) }
        }
        await reload()
    }

    func importFiles(_ urls: [URL]) {
        guard let eudisk, let partition, !urls.isEmpty else { return }
        Task {
            for url in urls {
                var isDir: ObjCBool = false
                FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir)
                await change(["put"] + (isDir.boolValue ? ["-r"] : []) + [url.path, eudisk.place(partition, path)])
            }
        }
    }

    func chooseImport() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = true
        panel.canChooseDirectories = true
        panel.message = "Copy into \(partition ?? ""):\(path)"
        if panel.runModal() == .OK { importFiles(panel.urls) }
    }

    func exportSelection() {
        guard let eudisk, let partition, !selection.isEmpty else { return }
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.canCreateDirectories = true
        panel.prompt = "Export Here"
        panel.message = "Copy \(selection.count == 1 ? selectedEntries[0].name : "\(selection.count) items") out to"
        guard panel.runModal() == .OK, let dest = panel.url else { return }
        let items = selectedEntries
        Task {
            await attempt {
                for e in items {
                    _ = try await eudisk.run(["get"] + (e.isDirectory ? ["-r"] : []) + [eudisk.place(partition, e.path), dest.path])
                }
            }
            NSWorkspace.shared.activateFileViewerSelecting([dest])
        }
    }

    func newFolder(_ name: String) {
        guard let eudisk, let partition, !name.isEmpty else { return }
        Task { await change(["mkdir", eudisk.place(partition, path.joined(name))]) }
    }

    func deleteSelection() {
        guard let eudisk, let partition else { return }
        let items = selectedEntries
        Task {
            for e in items {
                await change(["rm"] + (e.isDirectory ? ["-r"] : []) + [eudisk.place(partition, e.path)])
            }
            selection = []
        }
    }

    func fsck() {
        guard let eudisk else { return }
        Task {
            busy = true
            defer { busy = false }
            do {
                report = ("fsck", try await eudisk.text(["fsck", eudisk.image.path]))
            } catch {
                report = ("fsck found problems", error.localizedDescription)
            }
        }
    }

    func installKernel() {
        guard let eudisk else { return }
        let panel = NSOpenPanel()
        panel.message = "A kernel to boot (build/kernel.elf, build/selfhost/stage2/kernel/kernel.elf, ...); the one in boot moves to boot.old"
        panel.prompt = "Install"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        Task { await change(["kernel", "install", eudisk.image.path, url.path], title: "Kernel installed") }
    }

    func rollbackKernel() {
        guard let eudisk else { return }
        Task { await change(["kernel", "rollback", eudisk.image.path], title: "Kernels swapped") }
    }
}
