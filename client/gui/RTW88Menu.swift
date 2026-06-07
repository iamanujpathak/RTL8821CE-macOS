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
}

// MARK: - model

struct WifiNet: Identifiable {
    let ssid: String
    let bssid: String
    let channel: Int
    let band: String
    let beacons: Int
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

    var connected: Bool { connectedSSID != nil }

    func bg(_ work: @escaping () -> Void) {
        busy = true
        DispatchQueue.global().async {
            work()
            DispatchQueue.main.async { self.busy = false }
        }
    }

    func scan() {
        status = "scanning…"
        bg {
            let r = Rtwd.request("scan")
            DispatchQueue.main.async {
                guard let r else { self.daemonUp = false; self.status = "rtwd not reachable"; return }
                self.daemonUp = true
                if let arr = r["networks"] as? [[String: Any]] {
                    self.nets = arr.map {
                        WifiNet(ssid: $0["ssid"] as? String ?? "",
                                bssid: $0["bssid"] as? String ?? "",
                                channel: $0["channel"] as? Int ?? 0,
                                band: $0["band"] as? String ?? "",
                                beacons: $0["beacons"] as? Int ?? 0)
                    }
                    .filter { !$0.ssid.isEmpty }
                    .sorted { $0.beacons > $1.beacons }
                    self.status = "\(self.nets.count) networks"
                } else {
                    self.status = (r["error"] as? String) ?? "scan failed"
                }
            }
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

    func refreshStatus() {
        bg {
            let r = Rtwd.request("status")
            DispatchQueue.main.async {
                guard let r else { self.daemonUp = false; return }
                self.daemonUp = true
                if (r["connected"] as? Bool) == true {
                    self.connectedSSID = r["ssid"] as? String
                    self.ifname = r["ifname"] as? String ?? ""
                    self.ip = r["ip"] as? String ?? ""
                } else {
                    self.connectedSSID = nil; self.ifname = ""; self.ip = ""
                }
            }
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
                    .buttonStyle(.plain).help("Rescan")
            }

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
                            if selected?.id == net.id { selected = nil }
                            else { selected = net; password = "" }
                        }
                        if selected?.id == net.id && net.ssid != vm.connectedSSID {
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

            Divider()
            HStack {
                Text(vm.status).font(.caption2).foregroundColor(.secondary).lineLimit(1)
                Spacer()
                Button("Quit") { NSApplication.shared.terminate(nil) }
                    .controlSize(.small).buttonStyle(.plain)
            }
        }
        .padding(10)
        .frame(width: 320)
        .onAppear { vm.refreshStatus(); vm.scan() }
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
