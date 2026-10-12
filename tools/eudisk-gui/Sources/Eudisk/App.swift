// The window: partitions and kernels on the left, a partition's files on the
// right with a preview of the selected one.
import AppKit
import SwiftUI

@main
struct EudiskApp: App {
    @State private var model = Model()

    init() {
        // a plain executable (swift build) is not a bundle: be a regular app
        NSApplication.shared.setActivationPolicy(.regular)
        DispatchQueue.main.async { NSApp.activate(ignoringOtherApps: true) }
        // EUDISK_SNAPSHOT=file.png: draw the window into it and quit (for testing)
        if let snapshot = ProcessInfo.processInfo.environment["EUDISK_SNAPSHOT"] {
            DispatchQueue.main.asyncAfter(deadline: .now() + 3) {
                if let view = NSApp.windows.first(where: { $0.isVisible })?.contentView,
                   let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) {
                    view.cacheDisplay(in: view.bounds, to: rep)
                    try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: snapshot))
                }
                NSApp.terminate(nil)
            }
        }
    }

    var body: some Scene {
        WindowGroup("Eudisk") {
            ContentView()
                .environment(model)
                .frame(minWidth: 860, minHeight: 520)
        }
        .defaultSize(width: 1150, height: 720)
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("Open Image…") { model.chooseImage() }.keyboardShortcut("o")
            }
            CommandGroup(after: .newItem) {
                Button("Reload") { Task { await model.reload() } }.keyboardShortcut("r")
            }
        }
    }
}

struct ContentView: View {
    @Environment(Model.self) private var model
    @State private var askFolder = false
    @State private var folderName = ""
    @State private var confirmDelete = false

    var body: some View {
        @Bindable var model = model
        NavigationSplitView {
            Sidebar()
                .navigationSplitViewColumnWidth(min: 230, ideal: 260)
        } detail: {
            if model.image == nil {
                ContentUnavailableView {
                    Label("No disk image", systemImage: "externaldrive")
                } description: {
                    Text("Open disk.img (make run's disk) or build/fs.img.")
                } actions: {
                    Button("Open Image…") { model.chooseImage() }
                }
            } else {
                Browser()
            }
        }
        .navigationTitle(model.image?.lastPathComponent ?? "Eudisk")
        .navigationSubtitle(model.inUse ? "in use by QEMU: read-only" : (model.partition.map { "\($0):\(model.path)" } ?? ""))
        .toolbar {
            ToolbarItemGroup {
                if model.busy { ProgressView().controlSize(.small) }
                Button { model.chooseImport() } label: { Label("Import", systemImage: "square.and.arrow.down") }
                    .help("Copy files or folders from the Mac into this directory")
                    .disabled(model.inUse || model.partition == nil)
                Button { model.exportSelection() } label: { Label("Export", systemImage: "square.and.arrow.up") }
                    .help("Copy the selection out to the Mac")
                    .disabled(model.selection.isEmpty)
                Button { folderName = ""; askFolder = true } label: { Label("New Folder", systemImage: "folder.badge.plus") }
                    .disabled(model.inUse || model.partition == nil)
                Button(role: .destructive) { confirmDelete = true } label: { Label("Delete", systemImage: "trash") }
                    .disabled(model.inUse || model.selection.isEmpty)
                Button { model.fsck() } label: { Label("Check", systemImage: "stethoscope") }
                    .help("fsck every partition")
                    .disabled(model.image == nil)
                Button { Task { await model.reload() } } label: { Label("Reload", systemImage: "arrow.clockwise") }
                    .disabled(model.image == nil)
            }
        }
        .alert("New folder in \(model.path)", isPresented: $askFolder) {
            TextField("Name", text: $folderName)
            Button("Create") { model.newFolder(folderName) }
            Button("Cancel", role: .cancel) {}
        }
        .confirmationDialog("Delete \(model.selection.count == 1 ? model.selectedEntries.first?.name ?? "" : "\(model.selection.count) items") from the image?",
                            isPresented: $confirmDelete) {
            Button("Delete", role: .destructive) { model.deleteSelection() }
        } message: {
            Text("Folders are deleted with everything in them. This cannot be undone.")
        }
        .alert("Eudisk", isPresented: Binding(get: { model.error != nil }, set: { if !$0 { model.error = nil } })) {
            Button("OK") { model.error = nil }
        } message: {
            Text(model.error ?? "")
        }
        .sheet(isPresented: Binding(get: { model.report != nil }, set: { if !$0 { model.report = nil } })) {
            ReportView(title: model.report?.title ?? "", text: model.report?.text ?? "")
        }
    }
}

struct Sidebar: View {
    @Environment(Model.self) private var model

    var body: some View {
        @Bindable var model = model
        List(selection: $model.partition) {
            if let info = model.info {
                Section("Partitions") {
                    ForEach(info.partitions) { p in
                        VStack(alignment: .leading, spacing: 3) {
                            HStack {
                                Image(systemName: p.name.hasPrefix("boot") ? "bolt.horizontal.circle" : "internaldrive")
                                Text(p.name).fontWeight(.medium)
                                Spacer()
                                Text("\(p.megabytes) MiB").foregroundStyle(.secondary).monospacedDigit()
                            }
                            if p.efs {
                                ProgressView(value: p.usedFraction)
                                    .tint(p.usedFraction > 0.9 ? .red : .accentColor)
                                Text("\(p.files ?? 0) files, \(p.dirs ?? 0) folders, \(Int(p.usedFraction * 100))% used")
                                    .font(.caption).foregroundStyle(.secondary)
                                if (p.replayed ?? 0) > 0 {
                                    Text("log holds a committed transaction").font(.caption).foregroundStyle(.orange)
                                }
                            } else {
                                Text("not efs").font(.caption).foregroundStyle(.secondary)
                            }
                        }
                        .padding(.vertical, 2)
                        .tag(p.name)
                        .disabled(!p.efs)
                    }
                }
                if !info.kernels.isEmpty {
                    Section("Kernels") {
                        ForEach(info.kernels) { k in
                            VStack(alignment: .leading, spacing: 2) {
                                Text(k.slot).fontWeight(.medium)
                                if !k.present {
                                    Text("none").font(.caption).foregroundStyle(.secondary)
                                } else {
                                    Text(k.built.isEmpty ? "(no banner)" : k.built).font(.caption)
                                    Text("\(k.size / 1024) KiB\(k.valid ? "" : ", not a Eunuchs kernel")")
                                        .font(.caption).foregroundStyle(k.valid ? Color.secondary : Color.red)
                                }
                            }
                            .selectionDisabled()
                        }
                        HStack {
                            Button("Install…") { model.installKernel() }
                                .help("The kernel in boot moves to boot.old; the one you choose goes to boot")
                            Button("Roll Back") { model.rollbackKernel() }
                                .help("Swap boot and boot.old: the previous kernel boots next")
                        }
                        .disabled(model.inUse)
                        .selectionDisabled()
                    }
                }
                Section("Image") {
                    Text(model.image?.path ?? "").font(.caption).textSelection(.enabled)
                    Text("\(info.size >> 20) MiB, \(info.gpt ? "GPT" : "bare efs")").font(.caption).foregroundStyle(.secondary)
                    if info.in_use {
                        Label("QEMU is using it: read-only until it stops", systemImage: "lock")
                            .font(.caption).foregroundStyle(.orange)
                    }
                }
                .selectionDisabled()
            }
        }
        .listStyle(.sidebar)
    }
}

struct Browser: View {
    @Environment(Model.self) private var model

    var body: some View {
        @Bindable var model = model
        VSplitView {
            VStack(spacing: 0) {
                PathBar()
                Table(model.entries, selection: $model.selection) {
                    TableColumn("Name") { e in
                        Label(e.name, systemImage: e.isDirectory ? "folder" : "doc")
                    }
                    .width(min: 160, ideal: 260)
                    TableColumn("Size") { e in
                        Text(e.isDirectory ? "—" : ByteCountFormatter.string(fromByteCount: Int64(e.size), countStyle: .file))
                            .monospacedDigit().foregroundStyle(.secondary)
                    }
                    .width(80)
                    TableColumn("Mode") { e in Text(e.modeString).font(.system(.body, design: .monospaced)) }
                        .width(95)
                    TableColumn("Owner") { e in Text("\(e.uid):\(e.gid)").monospacedDigit() }
                        .width(70)
                    TableColumn("Links") { e in Text("\(e.nlink)").monospacedDigit() }
                        .width(40)
                    TableColumn("Modified") { e in
                        Text(e.modified.formatted(date: .abbreviated, time: .shortened)).foregroundStyle(.secondary)
                    }
                    .width(min: 120, ideal: 150)
                }
                .contextMenu(forSelectionType: Entry.ID.self) { ids in
                    if !ids.isEmpty {
                        Button("Export…") { model.exportSelection() }
                    }
                } primaryAction: { ids in
                    if ids.count == 1, let e = model.entries.first(where: { $0.id == ids.first }) {
                        model.activate(e)
                    }
                }
                .onChange(of: model.selection) { Task { await model.updatePreview() } }
                .dropDestination(for: URL.self) { urls, _ in
                    guard !model.inUse else { return false }
                    model.importFiles(urls)
                    return true
                }
            }
            .frame(minHeight: 200)
            ScrollView([.vertical, .horizontal]) {
                Text(model.preview ?? (model.selection.count > 1 ? "\(model.selection.count) selected" : "Select a file to see it"))
                    .font(.system(.callout, design: .monospaced))
                    .foregroundStyle(model.preview == nil ? .secondary : .primary)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .topLeading)
                    .padding(8)
            }
            .frame(minHeight: 100, idealHeight: 200)
        }
    }
}

struct PathBar: View {
    @Environment(Model.self) private var model

    var body: some View {
        HStack(spacing: 4) {
            Button { model.go(to: model.path.parentPath) } label: { Image(systemName: "chevron.up") }
                .disabled(model.path == "/")
                .help("Up one directory")
            Text("\(model.partition ?? ""):").foregroundStyle(.secondary)
            let parts = model.path.split(separator: "/").map(String.init)
            Button("/") { model.go(to: "/") }.buttonStyle(.link)
            ForEach(parts.indices, id: \.self) { i in
                Button(parts[i]) { model.go(to: "/" + parts[...i].joined(separator: "/")) }.buttonStyle(.link)
                if i < parts.count - 1 { Text("/").foregroundStyle(.secondary) }
            }
            Spacer()
            Text("\(model.entries.count) items").foregroundStyle(.secondary).font(.caption)
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 5)
        .background(.bar)
    }
}

struct ReportView: View {
    let title: String
    let text: String
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading) {
            Text(title).font(.headline)
            ScrollView {
                Text(text).font(.system(.callout, design: .monospaced)).textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(minWidth: 560, minHeight: 300)
            HStack {
                Spacer()
                Button("OK") { dismiss() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding()
    }
}
