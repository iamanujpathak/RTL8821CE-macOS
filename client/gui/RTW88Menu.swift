// RTW88Menu — a HeliPort-style menu-bar app for the RTL8821CE driver.
//
// Pure UI. All privileged work (kext user client, DHCP) is done by the root
// daemon `rtwd`; this app just speaks the line/JSON protocol over the Unix socket
// at /var/run/rtw88d.sock. No private API, runs as the console user.
//
// Build: client/gui/build-app.sh  ->  build/client/RTW88Menu.app

import SwiftUI
import Darwin

// MARK: - rtwd socket client

enum Rtwd {
    static let sockPath = "/var/run/rtw88d.sock"

    /// Send one command line, read the full JSON reply (daemon closes after replying).
    /// Blocking — call off the main thread.
    static func request(_ line: String) -> [String: Any]? {
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        if fd < 0 { return nil }
        defer { close(fd) }

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
        if rc != 0 { return nil }

        var req = line
        if !req.hasSuffix("\n") { req += "\n" }
        _ = req.withCString { send(fd, $0, strlen($0), 0) }

        var data = Data()
        var buf = [UInt8](repeating: 0, count: 16384)
        while true {
            let n = recv(fd, &buf, buf.count, 0)
            if n <= 0 { break }
            data.append(contentsOf: buf[0..<n])
        }
        guard !data.isEmpty,
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else { return nil }
        return obj
    }

    /// Send one command and invoke `onObject` for each newline-delimited JSON
    /// object as it arrives (used by the streaming scan). Blocking — call off the
    /// main thread. Returns once the daemon closes the connection.
    static func stream(_ line: String, onObject: @escaping ([String: Any]) -> Void) {
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        if fd < 0 { return }
        defer { close(fd) }

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
        if rc != 0 { return }

        var req = line
        if !req.hasSuffix("\n") { req += "\n" }
        _ = req.withCString { send(fd, $0, strlen($0), 0) }

        var pending = ""
        var buf = [UInt8](repeating: 0, count: 16384)
        while true {
            let n = recv(fd, &buf, buf.count, 0)
            if n <= 0 { break }
            pending += String(decoding: buf[0..<n], as: UTF8.self)
            while let nl = pending.firstIndex(of: "\n") {
                let lineStr = String(pending[pending.startIndex..<nl])
                pending = String(pending[pending.index(after: nl)...])
                if let d = lineStr.data(using: .utf8),
                   let obj = try? JSONSerialization.jsonObject(with: d) as? [String: Any] {
                    onObject(obj)
                }
            }
        }
    }
}

// MARK: - model

struct WifiNet: Identifiable {
    let ssid: String
    let bssid: String
    let channel: Int
    let band: String
    let beacons: Int
    let saved: Bool          // rtwd has a stored password for this SSID
    var id: String { bssid }
    // crude signal proxy: more beacons heard => stronger. 0..3 bars.
    var bars: Int { beacons >= 30 ? 3 : beacons >= 12 ? 2 : beacons >= 3 ? 1 : 0 }
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

    var connected: Bool { connectedSSID != nil }

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
        if !powered { return }
        busy = true; status = "scanning…"; nets = []
        DispatchQueue.global().async {
            var found: [WifiNet] = []
            Rtwd.stream("scan") { obj in
                if let net = obj["network"] as? [String: Any] {
                    let w = WifiNet(ssid: net["ssid"] as? String ?? "",
                                    bssid: net["bssid"] as? String ?? "",
                                    channel: net["channel"] as? Int ?? 0,
                                    band: net["band"] as? String ?? "",
                                    beacons: net["beacons"] as? Int ?? 0,
                                    saved: net["saved"] as? Bool ?? false)
                    if w.ssid.isEmpty || found.contains(where: { $0.bssid == w.bssid }) { return }
                    found.append(w)
                    let snap = found.sorted { $0.beacons > $1.beacons }
                    DispatchQueue.main.async { self.daemonUp = true; self.nets = snap; self.status = "\(snap.count) networks…" }
                } else if (obj["done"] as? Bool) == true {
                    DispatchQueue.main.async { self.daemonUp = true; self.status = "\(found.count) networks" }
                } else if let err = obj["error"] as? String {
                    DispatchQueue.main.async { self.status = err }
                }
            }
            DispatchQueue.main.async {
                self.busy = false
                if found.isEmpty && self.daemonUp == false { self.status = "rtwd not reachable" }
            }
        }
    }

    func setPower(_ on: Bool) {
        status = on ? "powering on…" : "powering off…"
        bg {
            _ = Rtwd.request("power\t\(on ? "on" : "off")")
            DispatchQueue.main.async {
                self.powered = on
                if !on { self.connectedSSID = nil; self.ifname = ""; self.ip = ""; self.nets = [] }
                self.status = on ? "radio on" : "radio off"
            }
            if on { self.fetchStatus() }
        }
    }

    func connect(_ ssid: String, _ pass: String) {
        status = "connecting to \(ssid)…"
        bg {
            // TAB-separated: connect <ssid> <pass>
            let r = Rtwd.request("connect\t\(ssid)\t\(pass)")
            DispatchQueue.main.async {
                guard let r else { self.status = "rtwd not reachable"; return }
                if (r["ok"] as? Bool) == true {
                    self.connectedSSID = ssid
                    self.ifname = r["ifname"] as? String ?? ""
                    self.status = "connected (\(self.ifname))"
                    self.refreshStatus()
                } else {
                    self.status = (r["error"] as? String) ?? "connect failed"
                }
            }
        }
    }

    func disconnect() {
        status = "disconnecting…"
        bg {
            _ = Rtwd.request("disconnect")
            DispatchQueue.main.async {
                self.connectedSSID = nil; self.ifname = ""; self.ip = ""
                self.status = "disconnected"
            }
        }
    }

    func refreshStatus() { bg { self.fetchStatus() } }

    /// Quiet status fetch (no busy spinner) — used by the background poll so the
    /// menu reflects connections made via the CLI or rtwd's auto-connect. Runs off
    /// the main actor (blocking socket I/O); UI updates hop back to main below.
    nonisolated private func fetchStatus() {
        let r = Rtwd.request("status")
        DispatchQueue.main.async {
            guard let r else { self.daemonUp = false; return }
            self.daemonUp = true
            self.powered = (r["powered"] as? Bool) ?? true
            if (r["connected"] as? Bool) == true {
                self.connectedSSID = r["ssid"] as? String
                self.ifname = r["ifname"] as? String ?? ""
                self.ip = r["ip"] as? String ?? ""
            } else {
                self.connectedSSID = nil; self.ifname = ""; self.ip = ""
            }
        }
    }

    private var pollTimer: Timer?
    /// Poll the daemon every few seconds so the GUI stays in sync with the kext
    /// (the source of truth) regardless of who initiated the connection.
    func startPolling() {
        if pollTimer != nil { return }
        pollTimer = Timer.scheduledTimer(withTimeInterval: 3.0, repeats: true) { _ in
            DispatchQueue.global().async { self.fetchStatus() }
        }
    }

    func forget(_ ssid: String) {
        bg {
            _ = Rtwd.request("forget\t\(ssid)")
            DispatchQueue.main.async { self.scan() }
        }
    }
}

// MARK: - UI

struct NetRow: View {
    let net: WifiNet
    let isCurrent: Bool
    let onTap: () -> Void
    var body: some View {
        Button(action: onTap) {
            HStack(spacing: 8) {
                Image(systemName: isCurrent ? "checkmark.circle.fill" : "wifi")
                    .foregroundColor(isCurrent ? .green : .primary)
                VStack(alignment: .leading, spacing: 1) {
                    Text(net.ssid).fontWeight(isCurrent ? .semibold : .regular)
                    Text("\(net.band) GHz · ch \(net.channel)")
                        .font(.caption2).foregroundColor(.secondary)
                }
                Spacer()
                if net.saved {
                    Image(systemName: "key.fill").font(.caption2).foregroundColor(.secondary)
                        .help("Saved network")
                }
                Image(systemName: "lock.fill").font(.caption2).foregroundColor(.secondary)
                signalBars
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
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

struct ContentView: View {
    @ObservedObject var vm: WifiVM
    @State private var selected: WifiNet? = nil
    @State private var password: String = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("RTL8821CE Wi-Fi").font(.headline)
                Spacer()
                if vm.busy { ProgressView().controlSize(.small) }
                Button { vm.scan() } label: { Image(systemName: "arrow.clockwise") }
                    .buttonStyle(.plain).help("Scan").disabled(!vm.powered)
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
                Label("rtwd not running (sudo rtwd / load the LaunchDaemon)", systemImage: "exclamationmark.triangle")
                    .font(.caption).foregroundColor(.orange)
            }

            ScrollView {
                VStack(alignment: .leading, spacing: 2) {
                    ForEach(vm.nets) { net in
                        NetRow(net: net, isCurrent: net.ssid == vm.connectedSSID) {
                            if net.ssid == vm.connectedSSID { return }
                            if net.saved {
                                // known network: rtwd supplies the saved password
                                selected = nil; vm.connect(net.ssid, "")
                            } else if selected?.id == net.id {
                                selected = nil
                            } else {
                                selected = net; password = ""
                            }
                        }
                        .contextMenu {
                            if net.saved {
                                Button("Forget Network") { vm.forget(net.ssid) }
                            }
                        }
                        // password entry only for UNSAVED networks
                        if selected?.id == net.id && !net.saved && net.ssid != vm.connectedSSID {
                            HStack(spacing: 6) {
                                SecureField("Password", text: $password)
                                    .textFieldStyle(.roundedBorder)
                                    .onSubmit { connectSelected(net) }
                                Button("Join") { connectSelected(net) }
                                    .keyboardShortcut(.defaultAction)
                            }
                            .padding(.vertical, 2).padding(.leading, 24)
                        }
                    }
                }
            }
            .frame(maxHeight: 280)
            }   // end: powered

            Divider()
            HStack {
                Text(vm.status).font(.caption2).foregroundColor(.secondary).lineLimit(1)
                Spacer()
            }
        }
        .padding(10)
        .frame(width: 320)
        // no auto-scan on open — the Scan button (top-right) triggers it.
        .onAppear { vm.startPolling(); vm.refreshStatus() }
    }

    func connectSelected(_ net: WifiNet) {
        vm.connect(net.ssid, password)
        selected = nil; password = ""
    }
}

// MARK: - app

@main
struct RTW88MenuApp: App {
    @StateObject private var vm = WifiVM()
    var body: some Scene {
        MenuBarExtra {
            ContentView(vm: vm)
        } label: {
            Image(systemName: vm.connected ? "wifi" : "wifi.slash")
        }
        .menuBarExtraStyle(.window)
    }
}
