// BOTWRecomp Installer: the window in front of tools/installer/botw_setup.py.
// The player picks their dump (base game, update v208, DLC) and optionally a save; the app checks
// each one with `botw_setup.py check`, then runs `botw_setup.py install --json` and shows its
// progress events. All the work (and every check) happens in botw_setup.py; this file is only UI.
// Built by tools/installer/macos/build.sh with the Command Line Tools' swiftc.
import AppKit
import SwiftUI
import UniformTypeIdentifiers

// MARK: - backend

enum Backend {
    /// Where botw_setup.py lives: the release's Contents/Resources/botwrecomp (tools/release/botw_dmg.py),
    /// a source checkout named in Info.plist (build.sh), or the folder around the app.
    static let root: URL? = {
        let fm = FileManager.default
        func ok(_ u: URL) -> Bool { fm.fileExists(atPath: u.appendingPathComponent("tools/installer/botw_setup.py").path) }
        if let r = Bundle.main.resourceURL?.appendingPathComponent("botwrecomp"), ok(r) { return r }
        if let s = Bundle.main.object(forInfoDictionaryKey: "BotWSourceRoot") as? String, ok(URL(fileURLWithPath: s)) {
            return URL(fileURLWithPath: s)
        }
        var u = Bundle.main.bundleURL
        for _ in 0..<6 {
            u.deleteLastPathComponent()
            if ok(u) { return u }
        }
        return nil
    }()
    static var script: String { root!.appendingPathComponent("tools/installer/botw_setup.py").path }

    static var environment: [String: String] {
        var env = ProcessInfo.processInfo.environment
        env["PATH"] = "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:" + (env["PATH"] ?? "")
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        env["PYTHONUNBUFFERED"] = "1"
        return env
    }

    static func process(_ args: [String]) -> Process {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/python3")
        p.arguments = ["-u", script, "--json"] + args
        p.environment = environment
        p.currentDirectoryURL = root
        return p
    }

    /// Runs a one-shot command (scan, check, requirements) and returns its JSON object.
    static func json(_ args: [String]) async -> Any? {
        await withCheckedContinuation { cont in
            DispatchQueue.global(qos: .userInitiated).async {
                let p = process(args)
                let out = Pipe(), err = Pipe()
                p.standardOutput = out
                p.standardError = err
                p.standardInput = FileHandle.nullDevice
                var errData = Data()
                let g = DispatchGroup()
                g.enter()
                DispatchQueue.global().async { errData = err.fileHandleForReading.readDataToEndOfFile(); g.leave() }
                guard (try? p.run()) != nil else { cont.resume(returning: nil); return }
                let data = out.fileHandleForReading.readDataToEndOfFile()
                p.waitUntilExit()
                g.wait()
                if p.terminationStatus != 0 { NSLog("botw_setup %@: %@", args.first ?? "", String(decoding: errData, as: UTF8.self)) }
                cont.resume(returning: try? JSONSerialization.jsonObject(with: data))
            }
        }
    }
}

// MARK: - model

enum Kind: String, CaseIterable, Identifiable {
    case base, update, dlc, save
    var id: String { rawValue }
    var label: String {
        switch self {
        case .base: return "Base game"
        case .update: return "Update"
        case .dlc: return "DLC"
        case .save: return "Save to import"
        }
    }
    var optional: Bool { self == .dlc || self == .save }
    var hint: String {
        switch self {
        case .base: return "Title 00050000101C9400: the folder with code, content and meta in it"
        case .update: return "Title 0005000E101C9400, version 208"
        case .dlc: return "Title 0005000C101C9400: The Master Trials and The Champions' Ballad"
        case .save: return "For example Cemu's mlc01/usr/save/00050000/101c9400/user/80000001"
        }
    }
    var symbol: String {
        switch self {
        case .base: return "opticaldisc"
        case .update: return "arrow.down.circle"
        case .dlc: return "shippingbox"
        case .save: return "externaldrive"
        }
    }
}

enum Status: String { case ok, warn, error, empty, checking }

struct ItemCheck {
    var status: Status = .empty
    var title = ""
    var detail = ""
    var messages: [String] = []
    var size: Int64 = 0
    init() {}
    init(_ d: [String: Any]) {
        status = Status(rawValue: d["status"] as? String ?? "") ?? .error
        title = d["title"] as? String ?? ""
        detail = d["detail"] as? String ?? ""
        messages = d["messages"] as? [String] ?? []
        size = (d["size"] as? NSNumber)?.int64Value ?? 0
    }
}

struct Requirement: Identifiable {
    var id: String { name }
    let name: String, status: Status, detail: String, fix: String
}

enum StepState { case pending, running, done, skipped, failed }

struct Step: Identifiable {
    let id: String
    let title: String
    var state: StepState = .pending
    var detail = ""
    var seconds: Double?
}

enum Phase { case setup, installing, done, failed }

enum Destination: String, CaseIterable, Identifiable {
    case userApps, apps, other
    var id: String { rawValue }
}

@MainActor
final class Installer: ObservableObject {
    @Published var paths: [Kind: String] = [:]
    @Published var checks: [Kind: ItemCheck] = [:]
    @Published var requirements: [Requirement] = []
    @Published var checkingRequirements = false
    @Published var destination: Destination = .userApps
    @Published var otherFolder: String = ""
    @Published var copyGame = false
    @Published var keepBuild = false
    @Published var scanning = false
    @Published var scanNote = ""

    @Published var phase: Phase = .setup
    @Published var steps: [Step] = []
    @Published var overall: Double = 0
    @Published var eta: Double?
    @Published var detail = ""
    @Published var log: [String] = []
    @Published var startedAt = Date()
    @Published var appPath = ""
    @Published var logPath = ""
    @Published var errorTitle = ""
    @Published var errorMessage = ""
    @Published var errorTail = ""
    @Published var cancelled = false
    // view state (kept here: the Command Line Tools have no plugin for SwiftUI's @State macro)
    @Published var dropTarget: Kind?
    @Published var showLog = true
    @Published var confirmCancel = false

    private var process: Process?
    private var buffer = Data()
    private var checkGeneration = 0
    private var lastEtaUpdate = Date()
    private var sawFinalEvent = false

    let home = FileManager.default.homeDirectoryForCurrentUser.path
    var supportDir: String { home + "/Library/Application Support/BotW" }

    init() {
        // a reinstall starts with the folders of the last installation
        let cfg = supportDir + "/install.plist"
        if let d = NSDictionary(contentsOfFile: cfg) as? [String: Any] {
            for k in [Kind.base, .update, .dlc] {
                if let p = d[k.rawValue] as? String, !p.isEmpty, FileManager.default.fileExists(atPath: p),
                   !p.hasPrefix(supportDir + "/game/") { paths[k] = p }
                else if let p = d[k.rawValue] as? String, p.hasPrefix(supportDir + "/game/"),
                        FileManager.default.fileExists(atPath: p) { paths[k] = p }
            }
            if let app = d["app"] as? String {
                let dir = (app as NSString).deletingLastPathComponent
                if dir == "/Applications" { destination = .apps }
                else if dir != home + "/Applications" { destination = .other; otherFolder = dir }
            }
        }
        recheck()
        refreshRequirements()
    }

    var destPath: String {
        switch destination {
        case .userApps: return home + "/Applications"
        case .apps: return "/Applications"
        case .other: return otherFolder.isEmpty ? home + "/Desktop" : otherFolder
        }
    }

    var gameBytes: Int64 { [Kind.base, .update, .dlc].reduce(0) { $0 + (checks[$1]?.size ?? 0) } }

    var ready: Bool {
        guard Backend.root != nil else { return false }
        for k in Kind.allCases {
            let s = checks[k]?.status ?? .empty
            if s == .error || s == .checking { return false }
            if !k.optional && s != .ok && s != .warn { return false }
        }
        return !checkingRequirements && !requirements.isEmpty && !requirements.contains { $0.status == .error }
    }

    var blocker: String? {
        if Backend.root == nil { return "The port's source folder was not found next to this app." }
        for k in [Kind.base, .update] where (checks[k]?.status ?? .empty) == .empty { return "Choose the \(k.label.lowercased()) first." }
        if Kind.allCases.contains(where: { checks[$0]?.status == .error }) { return "Fix the items marked in red first." }
        if let r = requirements.first(where: { $0.status == .error }) { return "\(r.name): \(r.fix)" }
        return nil
    }

    // MARK: checks

    func set(_ kind: Kind, _ path: String?) {
        paths[kind] = path
        recheck()
        if kind != .save { refreshRequirements() }
    }

    func recheck() {
        checkGeneration += 1
        let gen = checkGeneration
        var args = ["check"]
        for k in Kind.allCases {
            if let p = paths[k], !p.isEmpty {
                args += ["--\(k.rawValue)", p]
                checks[k] = { var c = checks[k] ?? ItemCheck(); c.status = .checking; return c }()
            } else {
                checks[k] = ItemCheck()
            }
        }
        Task {
            let res = await Backend.json(args) as? [String: Any]
            guard gen == checkGeneration else { return }
            for k in Kind.allCases {
                if let d = res?[k.rawValue] as? [String: Any] { checks[k] = ItemCheck(d) }
                else if paths[k] != nil { var c = ItemCheck(); c.status = .error; c.title = "Could not be checked"; checks[k] = c }
            }
            if copyGame { refreshRequirements() }
        }
    }

    func refreshRequirements() {
        guard Backend.root != nil else { return }
        checkingRequirements = true
        let bytes = copyGame ? gameBytes : 0
        Task {
            let res = await Backend.json(["requirements", "--copy-bytes", String(bytes)]) as? [[String: Any]]
            requirements = (res ?? []).map {
                Requirement(name: $0["name"] as? String ?? "", status: Status(rawValue: $0["status"] as? String ?? "") ?? .error,
                            detail: $0["detail"] as? String ?? "", fix: $0["fix"] as? String ?? "")
            }
            if res == nil {
                requirements = [Requirement(name: "Python", status: .error, detail: "the setup script could not run",
                                            fix: "Install Apple's Command Line Tools (xcode-select --install).")]
            }
            checkingRequirements = false
            autostartIfAsked()
        }
    }

    /// `--autostart` (testing): start building as soon as everything checks out.
    private var autostarted = false
    func autostartIfAsked() {
        guard CommandLine.arguments.contains("--autostart"), !autostarted, phase == .setup else { return }
        if ready { autostarted = true; install() }
        else { DispatchQueue.main.asyncAfter(deadline: .now() + 1) { self.autostartIfAsked() } }
    }

    func scan(_ dir: String) {
        scanning = true
        scanNote = ""
        Task {
            let res = await Backend.json(["scan", dir]) as? [String: String] ?? [:]
            scanning = false
            var found: [String] = []
            for k in [Kind.base, .update, .dlc] {
                if let p = res[k.rawValue], !p.isEmpty { paths[k] = p; found.append(k.label.components(separatedBy: " ").first!.lowercased()) }
            }
            scanNote = found.isEmpty ? "No Breath of the Wild folders were found in \((dir as NSString).lastPathComponent)."
                                     : "Found: " + found.joined(separator: ", ") + "."
            recheck()
            refreshRequirements()
        }
    }

    // MARK: install

    func install() {
        var args = ["install", "--dest", destPath]
        for k in Kind.allCases { if let p = paths[k], !p.isEmpty { args += ["--\(k.rawValue)", p] } }
        if copyGame { args.append("--copy-game") }
        if keepBuild { args.append("--keep-build") }
        steps = []
        log = []
        overall = 0
        eta = nil
        detail = ""
        errorTail = ""
        cancelled = false
        sawFinalEvent = false
        startedAt = Date()
        phase = .installing
        let p = Backend.process(args)
        let out = Pipe(), err = Pipe()
        p.standardOutput = out
        p.standardError = err
        p.standardInput = FileHandle.nullDevice
        out.fileHandleForReading.readabilityHandler = { [weak self] h in
            let data = h.availableData
            DispatchQueue.main.async { self?.receive(data) }
        }
        err.fileHandleForReading.readabilityHandler = { [weak self] h in
            let text = String(decoding: h.availableData, as: UTF8.self)
            guard !text.isEmpty else { return }
            DispatchQueue.main.async { self?.appendLog(text.split(separator: "\n").map(String.init)) }
        }
        p.terminationHandler = { [weak self] proc in
            DispatchQueue.main.async {
                out.fileHandleForReading.readabilityHandler = nil
                err.fileHandleForReading.readabilityHandler = nil
                self?.receive(out.fileHandleForReading.readDataToEndOfFile())
                self?.finished(status: proc.terminationStatus)
            }
        }
        do {
            try p.run()
            process = p
            ProcessInfo.processInfo.disableAutomaticTermination("installing")
        } catch {
            fail("The installer could not start", error.localizedDescription)
        }
    }

    func cancel() { process?.terminate() }

    func terminate() {
        if let p = process, p.isRunning { p.terminate(); p.waitUntilExit() }
    }

    private func receive(_ data: Data) {
        guard !data.isEmpty else { return }
        buffer.append(data)
        while let nl = buffer.firstIndex(of: 0x0A) {
            let line = buffer[buffer.startIndex..<nl]
            buffer.removeSubrange(buffer.startIndex...nl)
            guard let ev = try? JSONSerialization.jsonObject(with: line) as? [String: Any] else {
                appendLog([String(decoding: line, as: UTF8.self)])
                continue
            }
            handle(ev)
        }
    }

    private func appendLog(_ lines: [String]) {
        log.append(contentsOf: lines)
        if log.count > 6000 { log.removeFirst(log.count - 5000) }
    }

    private func handle(_ ev: [String: Any]) {
        if let o = ev["overall"] as? Double { overall = max(overall, o) }
        if let e = ev["overall_eta"] as? Double { smoothEta(e) }
        switch ev["event"] as? String {
        case "plan":
            steps = (ev["steps"] as? [[String: Any]] ?? []).map { Step(id: $0["id"] as? String ?? "", title: $0["title"] as? String ?? "") }
        case "step":
            guard let id = ev["id"] as? String, let i = steps.firstIndex(where: { $0.id == id }) else { return }
            switch ev["state"] as? String {
            case "running": steps[i].state = .running; detail = ""
            case "done": steps[i].state = .done
            case "skipped": steps[i].state = .skipped
            default: steps[i].state = .failed
            }
            steps[i].detail = ev["detail"] as? String ?? ""
            steps[i].seconds = ev["seconds"] as? Double
        case "progress":
            detail = ev["detail"] as? String ?? ""
        case "log":
            if let t = ev["text"] as? String { appendLog([t]) }
        case "done":
            sawFinalEvent = true
            appPath = ev["app"] as? String ?? ""
            logPath = ev["log"] as? String ?? ""
            overall = 1
            phase = .done
            NSApp.requestUserAttention(.informationalRequest)
        case "error":
            sawFinalEvent = true
            logPath = ev["log"] as? String ?? ""
            cancelled = ev["cancelled"] as? Bool ?? false
            errorTail = ev["tail"] as? String ?? ""
            fail(ev["title"] as? String ?? "The installation could not finish", ev["message"] as? String ?? "")
        default: break
        }
    }

    private func smoothEta(_ e: Double) {
        let now = Date()
        if let cur = eta {
            let decayed = max(cur - now.timeIntervalSince(lastEtaUpdate), 0)
            eta = decayed * 0.85 + e * 0.15
        } else {
            eta = e
        }
        lastEtaUpdate = now
    }

    private func finished(status: Int32) {
        process = nil
        ProcessInfo.processInfo.enableAutomaticTermination("installing")
        if !sawFinalEvent {
            fail("The installer stopped unexpectedly", "botw_setup.py exited with code \(status). The details are in the log below.")
            errorTail = log.suffix(30).joined(separator: "\n")
        }
    }

    private func fail(_ title: String, _ message: String) {
        errorTitle = title
        errorMessage = message
        phase = .failed
    }

    func openGame() {
        NSWorkspace.shared.openApplication(at: URL(fileURLWithPath: appPath), configuration: NSWorkspace.OpenConfiguration())
    }
}

// MARK: - views

struct StatusIcon: View {
    let status: Status
    var body: some View {
        Group {
            switch status {
            case .ok: Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
            case .warn: Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
            case .error: Image(systemName: "xmark.octagon.fill").foregroundStyle(.red)
            case .empty: Image(systemName: "circle.dashed").foregroundStyle(.tertiary)
            case .checking: ProgressView().controlSize(.small)
            }
        }
        .font(.title2)
        .frame(width: 28, height: 28)
    }
}

struct ItemRow: View {
    @EnvironmentObject var m: Installer
    let kind: Kind

    var body: some View {
        let c = m.checks[kind] ?? ItemCheck()
        let path = m.paths[kind] ?? ""
        HStack(alignment: .top, spacing: 12) {
            StatusIcon(status: c.status)
            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Text(kind.label).font(.headline)
                    if kind.optional { Text("optional").font(.caption).foregroundStyle(.secondary)
                        .padding(.horizontal, 5).padding(.vertical, 1).background(.quaternary, in: Capsule()) }
                }
                if path.isEmpty {
                    Text(kind.hint).font(.callout).foregroundStyle(.secondary)
                } else {
                    if c.status != .checking, !c.title.isEmpty {
                        Text(c.detail.isEmpty ? c.title : "\(c.title) · \(c.detail)").font(.callout)
                    } else if c.status == .checking {
                        Text("Checking…").font(.callout).foregroundStyle(.secondary)
                    }
                    Text(path).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                        .help(path)
                }
                ForEach(c.messages, id: \.self) { msg in
                    Text(msg).font(.callout).foregroundStyle(c.status == .error ? .red : .orange)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            Spacer(minLength: 8)
            if !path.isEmpty && kind.optional {
                Button { m.set(kind, nil) } label: { Image(systemName: "xmark.circle.fill").foregroundStyle(.secondary) }
                    .buttonStyle(.plain).help("Don't use")
            }
            Button(path.isEmpty ? "Choose…" : "Change…") { choose() }
        }
        .padding(10)
        .background(RoundedRectangle(cornerRadius: 8).fill(m.dropTarget == kind ? Color.accentColor.opacity(0.15) : Color.clear))
        .onDrop(of: [.fileURL], isTargeted: Binding(get: { m.dropTarget == kind },
                                                    set: { m.dropTarget = $0 ? kind : (m.dropTarget == kind ? nil : m.dropTarget) })) { providers in
            guard let p = providers.first else { return false }
            _ = p.loadObject(ofClass: URL.self) { url, _ in
                if let url { DispatchQueue.main.async { m.set(kind, url.path) } }
            }
            return true
        }
    }

    func choose() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.message = kind == .save ? "Choose the save folder (Cemu: mlc01/usr/save/00050000/101c9400/user/80000001)"
                                      : "Choose the \(kind.label.lowercased()) folder (it contains code, content and meta)"
        panel.prompt = "Choose"
        if let p = m.paths[kind], !p.isEmpty { panel.directoryURL = URL(fileURLWithPath: p).deletingLastPathComponent() }
        if panel.runModal() == .OK, let u = panel.url { m.set(kind, u.path) }
    }
}

struct Section<Content: View>: View {
    let number: Int
    let title: String
    var trailing: AnyView? = nil
    @ViewBuilder let content: Content
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text("\(number)").font(.caption.bold()).foregroundStyle(.white)
                    .frame(width: 20, height: 20).background(Circle().fill(Color.accentColor))
                Text(title).font(.title3.weight(.semibold))
                Spacer()
                if let trailing { trailing }
            }
            VStack(alignment: .leading, spacing: 0) { content }
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(RoundedRectangle(cornerRadius: 10).fill(Color(nsColor: .controlBackgroundColor)))
                .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Color(nsColor: .separatorColor)))
        }
    }
}

struct Header: View {
    let subtitle: String
    var body: some View {
        HStack(spacing: 14) {
            Image(nsImage: NSApp.applicationIconImage).resizable().frame(width: 64, height: 64)
            VStack(alignment: .leading, spacing: 2) {
                Text("BOTWRecomp").font(.title.bold())
                Text(subtitle).foregroundStyle(.secondary)
            }
            Spacer()
        }
    }
}

struct SetupView: View {
    @EnvironmentObject var m: Installer

    var body: some View {
        VStack(spacing: 0) {
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    Header(subtitle: "Breath of the Wild for Mac, built from your own Wii U dump. Nothing is downloaded.")
                    Section(number: 1, title: "Your game files", trailing: AnyView(scanButton)) {
                        if !m.scanNote.isEmpty {
                            Text(m.scanNote).font(.callout).foregroundStyle(.secondary).padding([.horizontal, .top], 12)
                        }
                        ItemRow(kind: .base)
                        Divider().padding(.leading, 50)
                        ItemRow(kind: .update)
                        Divider().padding(.leading, 50)
                        ItemRow(kind: .dlc)
                    }
                    Section(number: 2, title: "Save") {
                        ItemRow(kind: .save)
                        Text("Without one the game starts fresh. Saves are kept in ~/Library/Application Support/BotW/save; an existing save there is backed up first.")
                            .font(.caption).foregroundStyle(.secondary).padding([.horizontal, .bottom], 12)
                    }
                    Section(number: 3, title: "Where to put the game") { destination }
                    Section(number: 4, title: "Your Mac", trailing: AnyView(refreshButton)) { requirements }
                }
                .padding(24)
            }
            Divider()
            footer
        }
    }

    var scanButton: some View {
        HStack {
            if m.scanning { ProgressView().controlSize(.small) }
            Button("Find in a Folder…") {
                let panel = NSOpenPanel()
                panel.canChooseDirectories = true
                panel.canChooseFiles = false
                panel.message = "Choose the folder that holds your dump (for example the folder with the [Game], [Update] and [DLC] folders, or Cemu's mlc01)"
                panel.prompt = "Search"
                if panel.runModal() == .OK, let u = panel.url { m.scan(u.path) }
            }
        }
    }

    var refreshButton: some View {
        HStack {
            if m.checkingRequirements { ProgressView().controlSize(.small) }
            Button { m.refreshRequirements() } label: { Image(systemName: "arrow.clockwise") }.help("Check again")
        }
    }

    var destination: some View {
        VStack(alignment: .leading, spacing: 10) {
            Picker("Install to", selection: $m.destination) {
                Text("Applications in your home folder (~/Applications)").tag(Destination.userApps)
                Text("Applications for all users (/Applications)").tag(Destination.apps)
                Text(m.otherFolder.isEmpty ? "Other folder…" : "Other: \((m.otherFolder as NSString).abbreviatingWithTildeInPath)").tag(Destination.other)
            }
            .onChange(of: m.destination) { d in
                if d == .other {
                    let panel = NSOpenPanel()
                    panel.canChooseDirectories = true
                    panel.canChooseFiles = false
                    panel.canCreateDirectories = true
                    panel.prompt = "Install Here"
                    if panel.runModal() == .OK, let u = panel.url { m.otherFolder = u.path }
                    else if m.otherFolder.isEmpty { m.destination = .userApps }
                }
            }
            Text("The app: \((m.destPath as NSString).abbreviatingWithTildeInPath)/Breath of the Wild.app")
                .font(.caption).foregroundStyle(.secondary)
            Toggle(isOn: $m.copyGame) {
                VStack(alignment: .leading, spacing: 1) {
                    Text("Copy the game files to this Mac" + (m.gameBytes > 0 ? String(format: " (%.1f GB)", Double(m.gameBytes) / 1e9) : ""))
                    Text("Otherwise the app reads them where they are now, so that folder or drive has to stay connected.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            .onChange(of: m.copyGame) { _ in m.refreshRequirements() }
            Toggle(isOn: $m.keepBuild) {
                VStack(alignment: .leading, spacing: 1) {
                    Text("Keep the build files (about 1.5 GB)")
                    Text("Makes building again after an update of the port faster.").font(.caption).foregroundStyle(.secondary)
                }
            }
        }
        .toggleStyle(.checkbox)
        .padding(12)
    }

    var requirements: some View {
        VStack(alignment: .leading, spacing: 8) {
            if m.requirements.isEmpty {
                Text("Checking…").foregroundStyle(.secondary)
            }
            ForEach(m.requirements) { r in
                HStack(alignment: .top, spacing: 8) {
                    StatusIcon(status: r.status).font(.body).frame(width: 20, height: 18)
                    VStack(alignment: .leading, spacing: 1) {
                        HStack {
                            Text(r.name).frame(width: 150, alignment: .leading)
                            Text(r.detail).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                        }
                        if r.status != .ok, !r.fix.isEmpty {
                            Text(r.fix).font(.caption).foregroundStyle(r.status == .error ? .red : .secondary)
                                .textSelection(.enabled)
                        }
                    }
                    Spacer()
                    if r.name == "Command Line Tools" && r.status == .error {
                        Button("Install…") { run("/usr/bin/xcode-select", ["--install"]) }
                    }
                    if r.name == "CMake" && r.status == .error {
                        Button("cmake.org") { NSWorkspace.shared.open(URL(string: "https://cmake.org/download/")!) }
                    }
                }
            }
        }
        .padding(12)
    }

    var footer: some View {
        HStack {
            if let b = m.blocker {
                Image(systemName: "info.circle").foregroundStyle(.secondary)
                Text(b).font(.callout).foregroundStyle(.secondary).lineLimit(2)
            } else {
                Image(systemName: "clock").foregroundStyle(.secondary)
                Text("Building takes about 5–10 minutes. Keep the Mac plugged in.").font(.callout).foregroundStyle(.secondary)
            }
            Spacer()
            Button("Build Breath of the Wild") { m.install() }
                .keyboardShortcut(.defaultAction)
                .controlSize(.large)
                .disabled(!m.ready)
        }
        .padding(.horizontal, 24).padding(.vertical, 14)
    }
}

func run(_ tool: String, _ args: [String]) {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: tool)
    p.arguments = args
    try? p.run()
}

func formatEta(_ s: Double?) -> String {
    guard let s, s.isFinite else { return "Estimating time…" }
    if s < 45 { return "Less than a minute left" }
    if s < 90 { return "About a minute left" }
    return "About \(Int((s / 60).rounded())) minutes left"
}

func formatDuration(_ s: Double) -> String {
    s < 60 ? "\(Int(s)) s" : "\(Int(s) / 60) min \(Int(s) % 60) s"
}

struct LogView: View {
    let lines: [String]
    var body: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 0) {
                    ForEach(Array(lines.enumerated()), id: \.offset) { i, line in
                        Text(line.isEmpty ? " " : line).font(.system(size: 11, design: .monospaced))
                            .frame(maxWidth: .infinity, alignment: .leading).id(i)
                    }
                }
                .textSelection(.enabled)
                .padding(8)
            }
            .background(Color(nsColor: .textBackgroundColor))
            .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(Color(nsColor: .separatorColor)))
            .onChange(of: lines.count) { n in if n > 0 { proxy.scrollTo(n - 1, anchor: .bottom) } }
            .onAppear { if !lines.isEmpty { proxy.scrollTo(lines.count - 1, anchor: .bottom) } }
        }
    }
}

struct StepList: View {
    let steps: [Step]
    var body: some View {
        VStack(alignment: .leading, spacing: 7) {
            ForEach(steps) { s in
                HStack(spacing: 10) {
                    Group {
                        switch s.state {
                        case .pending: Image(systemName: "circle").foregroundStyle(.tertiary)
                        case .running: ProgressView().controlSize(.small)
                        case .done: Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
                        case .skipped: Image(systemName: "checkmark.circle").foregroundStyle(.green)
                        case .failed: Image(systemName: "xmark.circle.fill").foregroundStyle(.red)
                        }
                    }
                    .frame(width: 18)
                    Text(s.title).foregroundStyle(s.state == .pending ? .secondary : .primary)
                        .fontWeight(s.state == .running ? .semibold : .regular)
                    Spacer()
                    if s.state == .skipped { Text(s.detail).font(.caption).foregroundStyle(.secondary) }
                    else if let t = s.seconds, s.state == .done { Text(formatDuration(t)).font(.caption.monospacedDigit()).foregroundStyle(.secondary) }
                }
            }
        }
    }
}

struct InstallView: View {
    @EnvironmentObject var m: Installer

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Header(subtitle: "Building the game from your dump")
            VStack(alignment: .leading, spacing: 6) {
                ProgressView(value: m.overall)
                HStack {
                    Text(m.steps.first(where: { $0.state == .running })?.title ?? "Starting…").fontWeight(.medium)
                    if !m.detail.isEmpty { Text("— " + m.detail).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle) }
                    Spacer()
                    TimelineView(.periodic(from: .now, by: 1)) { _ in
                        Text(formatEta(m.eta) + " · " + formatDuration(Date().timeIntervalSince(m.startedAt)) + " so far")
                            .font(.callout.monospacedDigit()).foregroundStyle(.secondary)
                    }
                }
                .font(.callout)
            }
            StepList(steps: m.steps).padding(14)
                .background(RoundedRectangle(cornerRadius: 10).fill(Color(nsColor: .controlBackgroundColor)))
            DisclosureGroup("Details", isExpanded: $m.showLog) {
                LogView(lines: m.log).frame(minHeight: 180)
            }
            if !m.showLog { Spacer() }
            HStack {
                Text("You can use your Mac meanwhile; the build uses all its cores.").font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button("Cancel") { m.confirmCancel = true }
            }
        }
        .padding(24)
        .confirmationDialog("Stop building?", isPresented: $m.confirmCancel) {
            Button("Stop", role: .destructive) { m.cancel() }
        } message: { Text("You can start again later; finished steps are reused if you kept the build files.") }
    }
}

struct DoneView: View {
    @EnvironmentObject var m: Installer
    var body: some View {
        VStack(spacing: 18) {
            Spacer()
            Image(systemName: "checkmark.seal.fill").font(.system(size: 64)).foregroundStyle(.green)
            Text("Breath of the Wild is ready").font(.largeTitle.bold())
            Text((m.appPath as NSString).abbreviatingWithTildeInPath).foregroundStyle(.secondary).textSelection(.enabled)
            HStack(spacing: 12) {
                Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: m.appPath)]) }
                Button("Open Game") { m.openGame(); NSApp.terminate(nil) }
                    .keyboardShortcut(.defaultAction).controlSize(.large)
            }
            VStack(alignment: .leading, spacing: 4) {
                Label("Saves: ~/Library/Application Support/BotW/save", systemImage: "externaldrive")
                Label("Game log and crash reports: ~/Library/Logs/BotW", systemImage: "doc.text")
                Label("Moved the game files? Run this installer again and choose them.", systemImage: "arrow.triangle.2.circlepath")
            }
            .font(.callout).foregroundStyle(.secondary).padding(.top, 10)
            Spacer()
            HStack {
                StepList(steps: m.steps).font(.caption).frame(maxWidth: 360)
                Spacer()
                Button("Done") { NSApp.terminate(nil) }
            }
        }
        .padding(24)
    }
}

struct FailedView: View {
    @EnvironmentObject var m: Installer
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 12) {
                Image(systemName: m.cancelled ? "stop.circle.fill" : "exclamationmark.triangle.fill")
                    .font(.system(size: 40)).foregroundStyle(m.cancelled ? Color.secondary : Color.orange)
                VStack(alignment: .leading) {
                    Text(m.errorTitle).font(.title2.bold())
                    if let s = m.steps.first(where: { $0.state == .failed }) {
                        Text("During: \(s.title)").foregroundStyle(.secondary)
                    }
                }
            }
            Text(m.errorMessage).textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
            LogView(lines: m.errorTail.isEmpty ? m.log : m.errorTail.components(separatedBy: "\n"))
            HStack {
                Button("Copy Log") {
                    NSPasteboard.general.clearContents()
                    let text = (try? String(contentsOfFile: m.logPath, encoding: .utf8)) ?? m.log.joined(separator: "\n")
                    NSPasteboard.general.setString(text, forType: .string)
                }
                if !m.logPath.isEmpty {
                    Button("Show Log File") { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: m.logPath)]) }
                }
                Spacer()
                Button("Back") { m.phase = .setup; m.recheck(); m.refreshRequirements() }
                Button("Try Again") { m.install() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(24)
    }
}

struct MissingSourceView: View {
    var body: some View {
        VStack(spacing: 14) {
            Image(systemName: "folder.badge.questionmark").font(.system(size: 48)).foregroundStyle(.orange)
            Text("The port's files were not found").font(.title2.bold())
            Text("This copy of the installer is incomplete. Download BOTWRecomp again and open the installer from the disk image.")
                .multilineTextAlignment(.center).foregroundStyle(.secondary).frame(maxWidth: 420)
            Button("Quit") { NSApp.terminate(nil) }
        }
        .padding(40)
    }
}

struct RootView: View {
    @EnvironmentObject var m: Installer
    var body: some View {
        Group {
            if Backend.root == nil { MissingSourceView() }
            else {
                switch m.phase {
                case .setup: SetupView()
                case .installing: InstallView()
                case .done: DoneView()
                case .failed: FailedView()
                }
            }
        }
        .frame(minWidth: 720, idealWidth: 760, minHeight: 640, idealHeight: 780)
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    var model: Installer?
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let m = model, m.phase == .installing else { return .terminateNow }
        let a = NSAlert()
        a.messageText = "Stop building Breath of the Wild?"
        a.informativeText = "The build is still running. Quitting stops it."
        a.addButton(withTitle: "Keep Building")
        a.addButton(withTitle: "Quit")
        if a.runModal() == .alertFirstButtonReturn { return .terminateCancel }
        MainActor.assumeIsolated { m.terminate() }
        return .terminateNow
    }
}

@main
struct BotWInstallerApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
    @StateObject private var model = Installer()
    var body: some Scene {
        WindowGroup("BOTWRecomp Installer") {
            RootView().environmentObject(model).onAppear { delegate.model = model }
        }
        .windowResizability(.contentMinSize)
        .commands { CommandGroup(replacing: .newItem) {} }
    }
}
