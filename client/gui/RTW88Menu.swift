// RTW88Menu — a HeliPort-style menu-bar app for the RTL8821CE driver.
//
// Pure UI. All privileged work (kext user client, DHCP) is done by the root
// daemon `rtwd`; this app just speaks the line/JSON protocol over the Unix socket
// at /var/run/rtw88d.sock. No private API, runs as the console user.
//
// Shell: AppKit NSStatusItem + NSPopover hosting a SwiftUI view. The popover is
// anchored to the status-item button, so it positions correctly even when the
// menu bar auto-hides in full-screen spaces (SwiftUI's MenuBarExtra .window
// style detached/mis-positioned there).
//
// Build: client/gui/build-app.sh  ->  build/client/RTW88Menu.app

import AppKit
import SwiftUI
import Darwin

// MARK: - rtwd socket client

enum Rtwd {
    static let sockPath = "/var/run/rtw88d.sock"

    /// Open + connect a socket to rtwd with send/recv timeouts so a wedged
    /// daemon can never hang a GUI thread indefinitely. The timeout must exceed a
    /// normal connect: the daemon sends nothing until the whole chain finishes —
    /// ~11s scan + the always-expiring 6s 5GHz IQK poll + bring-up + up-to-4s DHCP
    /// — routinely 20-25s. 45s keeps a genuine slow join from being misreported as
    /// "rtwd not reachable" while still bounding a truly wedged daemon.
    private static func dial(timeoutSec: Int = 45) -> Int32 {
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        if fd < 0 { return -1 }
        var tv = timeval(tv_sec: timeoutSec, tv_usec: 0)
        _ = withUnsafePointer(to: &tv) {
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, $0, socklen_t(MemoryLayout<timeval>.size))
        }
        _ = withUnsafePointer(to: &tv) {
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, $0, socklen_t(MemoryLayout<timeval>.size))
        }
        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let cap = MemoryLayout.size(ofValue: addr.sun_path)
        withUnsafeMutablePointer(to: &addr.sun_path) { raw in
            raw.withMemoryRebound(to: CChar.self, capacity: cap) { dst in
                _ = sockPath.withCString { strncpy(dst, $0, cap - 1) }
            }
        }
        let len = socklen_t(MemoryLayout<sockaddr_un>.size)
        let rc = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(fd, $0, len) }
        }
        if rc != 0 { close(fd); return -1 }
        return fd
    }

    /// Send one command and invoke `onObject` for each newline-delimited JSON
    /// object as it arrives. Lines are split on the raw 0x0A BYTE before UTF-8
    /// decoding, so a multi-byte SSID character spanning two recv() chunks can't
    /// corrupt a line. Blocking — call off the main thread. Returns false when
    /// the daemon was unreachable.
    @discardableResult
    static func stream(_ line: String, onObject: @escaping ([String: Any]) -> Void) -> Bool {
        let fd = dial()
        if fd < 0 { return false }
        defer { close(fd) }

        var req = line
        if !req.hasSuffix("\n") { req += "\n" }
        _ = req.withCString { send(fd, $0, strlen($0), 0) }

        var pending = Data()
        var buf = [UInt8](repeating: 0, count: 16384)
        var sawAny = false
        while true {
            let n = recv(fd, &buf, buf.count, 0)
            if n <= 0 { break }
            pending.append(contentsOf: buf[0..<n])
            while let nl = pending.firstIndex(of: 0x0A) {
                let lineData = pending.subdata(in: pending.startIndex..<nl)
                pending.removeSubrange(pending.startIndex...nl)
                guard !lineData.isEmpty else { continue }
                sawAny = true
                // a single non-UTF-8 SSID must only skip ITS line, not kill the scan
                if let obj = try? JSONSerialization.jsonObject(with: lineData) as? [String: Any] {
                    onObject(obj)
                }
            }
        }
        return sawAny
    }

    /// Send one command, return the last JSON reply (daemon closes after replying),
    /// or nil when the daemon was unreachable.
    static func request(_ line: String) -> [String: Any]? {
        var last: [String: Any]? = nil
        let ok = stream(line) { last = $0 }
        return ok ? (last ?? [:]) : nil
    }
}

// MARK: - model

struct WifiNet: Identifiable {
    let ssid: String
    let bssid: String
    let channel: Int
    let band: String
    let beacons: Int
    let secure: Bool         // beacon Privacy bit: needs a password
    var saved: Bool          // rtwd has a stored password for this SSID
    var id: String { bssid }
    // crude signal proxy until RSSI is plumbed through the scan ABI. 0..3 bars.
    var bars: Int { beacons >= 8 ? 3 : beacons >= 4 ? 2 : beacons >= 2 ? 1 : 0 }
}

@MainActor
final class WifiVM: ObservableObject {
    @Published var nets: [WifiNet] = []
    @Published var connectedSSID: String? = nil
    @Published var ifname: String = ""
    @Published var ip: String = ""
    @Published var busy: Bool = false
    @Published var status: String = "idle"
    @Published var daemonUp: Bool = true
    @Published var powered: Bool = true
    @Published var lastError: String? = nil   // shown inline under the affected row

    /// status-item icon refresh hook (set by AppDelegate)
    var onStateChange: (() -> Void)? = nil

    var connected: Bool { connectedSSID != nil }

    private var scanEpoch = 0   // discard rows from a superseded scan

    func bg(_ work: @escaping () -> Void) {
        busy = true
        DispatchQueue.global().async {
            work()
            DispatchQueue.main.async { self.busy = false }
        }
    }

    /// Streaming scan: rows appear as each channel chunk completes (rtwd emits one
    /// JSON line per network), instead of waiting for the whole ~11s sweep.
    func scan() {
        if !powered || busy { return }
        scanEpoch += 1
        let epoch = scanEpoch
        busy = true; status = "scanning…"; nets = []; lastError = nil
        DispatchQueue.global().async {
            var found: [WifiNet] = []
            let ok = Rtwd.stream("scan") { obj in
                if let net = obj["network"] as? [String: Any] {
                    let w = WifiNet(ssid: net["ssid"] as? String ?? "",
                                    bssid: net["bssid"] as? String ?? "",
                                    channel: net["channel"] as? Int ?? 0,
                                    band: net["band"] as? String ?? "",
                                    beacons: net["beacons"] as? Int ?? 0,
                                    secure: net["secure"] as? Bool ?? true,
                                    saved: net["saved"] as? Bool ?? false)
                    if w.ssid.isEmpty || found.contains(where: { $0.bssid == w.bssid }) { return }
                    found.append(w)
                    let snap = found.sorted { $0.beacons > $1.beacons }
                    DispatchQueue.main.async {
                        guard epoch == self.scanEpoch else { return }
                        self.daemonUp = true; self.nets = snap; self.status = "\(snap.count) networks…"
                    }
                } else if (obj["done"] as? Bool) == true {
                    DispatchQueue.main.async {
                        guard epoch == self.scanEpoch else { return }
                        self.daemonUp = true; self.status = "\(found.count) networks"
                    }
                } else if let err = obj["error"] as? String {
                    DispatchQueue.main.async {
                        guard epoch == self.scanEpoch else { return }
                        self.status = err
                    }
                }
            }
            DispatchQueue.main.async {
                guard epoch == self.scanEpoch else { return }
                self.busy = false
                if !ok { self.daemonUp = false; self.status = "rtwd not reachable" }
            }
        }
    }

    func setPower(_ on: Bool) {
        status = on ? "powering on…" : "powering off…"
        bg {
            let r = Rtwd.request("power\t\(on ? "on" : "off")")
            DispatchQueue.main.async {
                guard let r else { self.daemonUp = false; self.status = "rtwd not reachable"; return }
                if (r["ok"] as? Bool) == true {
                    self.powered = on
                    if !on { self.connectedSSID = nil; self.ifname = ""; self.ip = ""; self.nets = [] }
                    self.status = on ? "radio on" : "radio off"
                } else {
                    self.status = (r["error"] as? String) ?? "power change failed"
                }
                self.onStateChange?()
            }
            if on { self.fetchStatus() }
        }
    }

    /// args must not carry protocol separators (an SSID is attacker-named).
    private static func protocolSafe(_ s: String) -> Bool {
        !s.contains("\t") && !s.contains("\n") && !s.contains("\r")
    }

    func connect(_ ssid: String, _ pass: String, remember: Bool = true) {
        guard WifiVM.protocolSafe(ssid), WifiVM.protocolSafe(pass) else {
            lastError = "name/password contains an unsupported character"
            return
        }
        status = "connecting to \(ssid)…"; lastError = nil
        bg {
            // TAB-separated: connect <ssid> <pass> [nosave]
            let r = Rtwd.request("connect\t\(ssid)\t\(pass)" + (remember ? "" : "\tnosave"))
            DispatchQueue.main.async {
                guard let r else { self.daemonUp = false; self.status = "rtwd not reachable"; return }
                if (r["ok"] as? Bool) == true {
                    self.connectedSSID = ssid
                    self.ifname = r["ifname"] as? String ?? ""
                    self.status = "connected (\(self.ifname))"
                    self.lastError = nil
                    if remember, let i = self.nets.firstIndex(where: { $0.ssid == ssid }) {
                        self.nets[i].saved = true
                    }
                    self.refreshStatus()
                } else {
                    let err = (r["error"] as? String) ?? "connect failed"
                    self.status = err
                    self.lastError = err      // surfaced inline; the password is kept
                }
                self.onStateChange?()
            }
        }
    }

    func disconnect() {
        status = "disconnecting…"
        bg {
            let r = Rtwd.request("disconnect")
            DispatchQueue.main.async {
                guard let r else { self.daemonUp = false; self.status = "rtwd not reachable"; return }
                if (r["ok"] as? Bool) == true {
                    self.connectedSSID = nil; self.ifname = ""; self.ip = ""
                    self.status = "disconnected"
                } else {
                    self.status = (r["error"] as? String) ?? "disconnect failed"
                }
                self.onStateChange?()
            }
        }
    }

    func forget(_ ssid: String) {
        guard WifiVM.protocolSafe(ssid) else { return }
        bg {
            let r = Rtwd.request("forget\t\(ssid)")
            DispatchQueue.main.async {
                guard let r else { self.daemonUp = false; return }
                if (r["ok"] as? Bool) == true {
                    // update rows in place — no need to burn a full off-channel scan
                    for i in self.nets.indices where self.nets[i].ssid == ssid {
                        self.nets[i].saved = false
                    }
                    self.status = "forgot \(ssid)"
                } else {
                    self.status = (r["error"] as? String) ?? "forget failed"
                }
            }
        }
    }

    func refreshStatus() { bg { self.fetchStatus() } }

    /// Quiet status fetch (no busy spinner) — used by the background poll so the
    /// menu reflects connections made via the CLI or rtwd's auto-connect. Runs off
    /// the main actor (blocking socket I/O); UI updates hop back to main below.
    nonisolated func fetchStatus() {
        let r = Rtwd.request("status")
        DispatchQueue.main.async {
            guard let r else { self.daemonUp = false; self.onStateChange?(); return }
            self.daemonUp = true
            self.powered = (r["powered"] as? Bool) ?? true
            if (r["connected"] as? Bool) == true {
                self.connectedSSID = r["ssid"] as? String
                self.ifname = r["ifname"] as? String ?? ""
                self.ip = r["ip"] as? String ?? ""
            } else {
                self.connectedSSID = nil; self.ifname = ""; self.ip = ""
            }
            self.onStateChange?()
        }
    }

    private var pollTimer: Timer?
    /// Poll the daemon every few seconds so the GUI (and the status-bar icon)
    /// stays in sync with the kext regardless of who initiated the connection.
    /// Started once at app launch — NOT on first popover open, or the icon shows
    /// a stale state until the user clicks it.
    func startPolling() {
        if pollTimer != nil { return }
        pollTimer = Timer.scheduledTimer(withTimeInterval: 3.0, repeats: true) { [weak self] _ in
            guard let self else { return }
            DispatchQueue.global().async { self.fetchStatus() }
        }
    }

    func stopPolling() {
        pollTimer?.invalidate()
        pollTimer = nil
    }
}

// MARK: - UI

struct NetRow: View {
    let net: WifiNet
    let isCurrent: Bool
    let isExpanded: Bool      // password panel open below (secure + unsaved only)
    let busy: Bool
    let onConnect: () -> Void // saved -> connect now; secure+unsaved -> toggle password panel
    let onForget: () -> Void

    /// open + unsaved networks aren't joinable yet (CCMP-only in-kext path)
    var joinable: Bool { net.secure || net.saved }

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: isCurrent ? "checkmark.circle.fill" : "wifi")
                .foregroundColor(isCurrent ? .green : .primary)
            VStack(alignment: .leading, spacing: 1) {
                Text(net.ssid).fontWeight(isCurrent ? .semibold : .regular)
                Text("\(net.band) GHz · ch \(net.channel)")
                    .font(.caption2).foregroundColor(.secondary)
            }
            Spacer()
            if net.secure {
                Image(systemName: "lock.fill").font(.caption2).foregroundColor(.secondary)
                    .help("Password protected")
            }
            signalBars
            if !isCurrent {
                if net.saved {
                    // bin icon = forget (replaces the old "Forget" button / context menu)
                    Button { onForget() } label: { Image(systemName: "trash") }
                        .buttonStyle(.borderless).controlSize(.small)
                        .foregroundColor(.secondary).help("Forget this network")
                }
                if joinable {
                    // inline Connect on the row itself. For a secure+unsaved network this
                    // opens the password panel below; otherwise it connects immediately.
                    Button(isExpanded ? "Cancel" : "Connect") { onConnect() }
                        .controlSize(.small).disabled(busy)
                } else {
                    Text("WPA2 only").font(.caption2).foregroundColor(.secondary)
                        .help("Open networks aren't supported yet")
                }
            }
        }
        .padding(.vertical, 1)
        .contentShape(Rectangle())
    }
    var signalBars: some View {
        HStack(spacing: 1) {
            ForEach(0..<3) { i in
                RoundedRectangle(cornerRadius: 1)
                    .fill(i < net.bars ? Color.primary : Color.secondary.opacity(0.25))
                    .frame(width: 3, height: CGFloat(5 + i * 4))
            }
        }
    }
}

/// password entry shown below a secure, unsaved row when its Connect is pressed:
/// field + show/hide eye + remember + Join.
struct PasswordPanel: View {
    @Binding var password: String
    @Binding var showPassword: Bool
    @Binding var remember: Bool
    let error: String?
    let onJoin: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 6) {
                Group {
                    if showPassword { TextField("Password", text: $password) }
                    else            { SecureField("Password", text: $password) }
                }
                .textFieldStyle(.roundedBorder)
                .onSubmit { onJoin() }
                Button { showPassword.toggle() } label: {
                    Image(systemName: showPassword ? "eye.slash" : "eye")
                }
                .buttonStyle(.plain)
                .help(showPassword ? "Hide password" : "Show password")
            }
            HStack {
                Toggle("Remember this network", isOn: $remember)
                    .font(.caption).toggleStyle(.checkbox)
                Spacer()
                Button("Join") { onJoin() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(password.isEmpty)
            }
            if let error {
                Label(error, systemImage: "exclamationmark.triangle.fill")
                    .font(.caption2).foregroundColor(.orange).lineLimit(2)
            }
        }
        .padding(.vertical, 4).padding(.leading, 24)
    }
}

struct ContentView: View {
    @ObservedObject var vm: WifiVM
    @State private var selected: WifiNet? = nil
    @State private var password: String = ""
    @State private var showPassword: Bool = false
    @State private var remember: Bool = true

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("RTL8821CE Wi-Fi").font(.headline)
                Spacer()
                if vm.busy { ProgressView().controlSize(.small) }
                Button { vm.scan() } label: { Image(systemName: "arrow.clockwise") }
                    .buttonStyle(.plain).help("Scan").disabled(!vm.powered || vm.busy)
                // Wi-Fi power switch (separate from connect): off tears down the link
                Toggle("", isOn: Binding(get: { vm.powered }, set: { vm.setPower($0) }))
                    .labelsHidden().toggleStyle(.switch).controlSize(.small).help("Wi-Fi power")
            }

            if !vm.powered {
                Spacer().frame(height: 4)
                Label("Wi-Fi is off", systemImage: "wifi.slash").foregroundColor(.secondary)
                Text("Turn it on with the switch above.").font(.caption2).foregroundColor(.secondary)
            } else {
                if let ssid = vm.connectedSSID {
                    HStack {
                        Image(systemName: "wifi").foregroundColor(.green)
                        VStack(alignment: .leading, spacing: 0) {
                            Text("Connected: \(ssid)").font(.subheadline).fontWeight(.semibold)
                            Text("\(vm.ifname)\(vm.ip.isEmpty ? "" : " · \(vm.ip)")")
                                .font(.caption2).foregroundColor(.secondary)
                        }
                        Spacer()
                        Button("Disconnect") { vm.disconnect() }.controlSize(.small)
                    }
                    Divider()
                }

                if !vm.daemonUp {
                    Label("rtwd not running (sudo rtwd / load the LaunchDaemon)",
                          systemImage: "exclamationmark.triangle")
                        .font(.caption).foregroundColor(.orange)
                }

                ScrollView {
                    VStack(alignment: .leading, spacing: 2) {
                        ForEach(vm.nets) { net in
                            let expanded = selected?.id == net.id
                            NetRow(net: net,
                                   isCurrent: net.ssid == vm.connectedSSID,
                                   isExpanded: expanded,
                                   busy: vm.busy,
                                   onConnect: {
                                       if net.ssid == vm.connectedSSID { return }
                                       if net.secure && !net.saved {
                                           // toggle the inline password panel
                                           if expanded { selected = nil }
                                           else { selected = net; password = ""; showPassword = false
                                                  remember = true; vm.lastError = nil }
                                       } else {
                                           // saved (secure) -> connect with the saved password now
                                           selected = nil
                                           vm.connect(net.ssid, "")
                                       }
                                   },
                                   onForget: { vm.forget(net.ssid); if expanded { selected = nil } })
                            if expanded && net.secure && !net.saved && net.ssid != vm.connectedSSID {
                                PasswordPanel(password: $password,
                                              showPassword: $showPassword,
                                              remember: $remember,
                                              error: vm.lastError,
                                              onJoin: { vm.connect(net.ssid, password, remember: remember) })
                            }
                        }
                        if vm.nets.isEmpty && !vm.busy {
                            Text("No networks — press ⟳ to scan.")
                                .font(.caption).foregroundColor(.secondary)
                                .padding(.vertical, 8)
                        }
                    }
                }
                .frame(maxHeight: 300)
            }   // end: powered

            Divider()
            HStack {
                Text(vm.status).font(.caption2).foregroundColor(.secondary).lineLimit(1)
                Spacer()
                Button("Quit") { NSApp.terminate(nil) }
                    .buttonStyle(.plain).font(.caption2).foregroundColor(.secondary)
            }
        }
        .padding(10)
        .frame(width: 320)
        // no auto-scan on open — the Scan button (top-right) triggers it.
        .onAppear { vm.refreshStatus() }
    }
}

// MARK: - app shell (AppKit: status item + anchored popover)

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate, NSPopoverDelegate {
    private var statusItem: NSStatusItem!
    private var popover: NSPopover!
    private let vm = WifiVM()

    func applicationDidFinishLaunching(_ notification: Notification) {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        if let button = statusItem.button {
            button.image = NSImage(systemSymbolName: "wifi.slash",
                                   accessibilityDescription: "RTW88 Wi-Fi")
            button.action = #selector(togglePopover(_:))
            button.target = self
        }

        popover = NSPopover()
        popover.behavior = .transient        // closes on outside click, like the system Wi-Fi menu
        popover.delegate = self
        popover.contentViewController = NSHostingController(rootView: ContentView(vm: vm))

        // icon reflects reality from launch (poll-driven), not from first click
        vm.onStateChange = { [weak self] in self?.updateIcon() }
        vm.startPolling()
        DispatchQueue.global().async { [vm] in vm.fetchStatus() }
    }

    func applicationWillTerminate(_ notification: Notification) {
        vm.stopPolling()
    }

    private func updateIcon() {
        let name = !vm.powered ? "wifi.slash" : (vm.connected ? "wifi" : "wifi.exclamationmark")
        statusItem.button?.image = NSImage(systemSymbolName: name,
                                           accessibilityDescription: "RTW88 Wi-Fi")
    }

    @objc private func togglePopover(_ sender: Any?) {
        guard let button = statusItem.button else { return }
        if popover.isShown {
            popover.performClose(sender)
        } else {
            // Anchoring to the status button keeps the popover attached to the icon
            // even when the menu bar overlays a full-screen space (it only appears
            // while the bar is revealed, exactly like the system menus).
            popover.show(relativeTo: button.bounds, of: button, preferredEdge: .minY)
            popover.contentViewController?.view.window?.makeKey()
            NSApp.activate(ignoringOtherApps: true)
        }
    }
}

@main
enum RTW88MenuMain {
    @MainActor static var delegate: AppDelegate?   // NSApplication.delegate is unretained

    @MainActor static func main() {
        let app = NSApplication.shared
        app.setActivationPolicy(.accessory)   // menu-bar only (matches LSUIElement)
        let d = AppDelegate()
        delegate = d
        app.delegate = d
        app.run()
    }
}
