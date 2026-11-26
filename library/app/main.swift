// mproton Library.app: a native window around the mproton Library page.
//
// Starts library/mproton-library (the Python server) unless one is already answering on the
// port, shows http://127.0.0.1:<port>/ in a WKWebView, and stops the server it started when
// the app quits. Games keep running: the server starts them in their own sessions.
// Built by library/make-app.sh: from a checkout it writes the checkout's script path into
// Info.plist (MprotonLibraryScript); with --bundle (releases) it copies launcher/ and library/
// into Contents/Resources/mproton instead.

import AppKit
import WebKit

let port = Int(ProcessInfo.processInfo.environment["MPROTON_LIBRARY_PORT"] ?? "") ?? 8417
let pageURL = URL(string: "http://127.0.0.1:\(port)/")!
let logURL = FileManager.default.homeDirectoryForCurrentUser
    .appendingPathComponent("Library/Logs/mproton-library.log")

final class AppDelegate: NSObject, NSApplicationDelegate, WKNavigationDelegate, WKUIDelegate {
    var window: NSWindow!
    var web: WKWebView!
    var server: Process?          // only set when this app started the server
    var quitting = false

    func applicationDidFinishLaunching(_ note: Notification) {
        buildMenu()
        let cfg = WKWebViewConfiguration()
        cfg.websiteDataStore = .default()  // keeps the page's localStorage (selected game)
        web = WKWebView(frame: .zero, configuration: cfg)
        web.navigationDelegate = self
        web.uiDelegate = self
        web.setValue(false, forKey: "drawsBackground")  // no white flash in dark mode

        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1180, height: 800),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered, defer: false)
        window.title = "mproton Library"
        window.minSize = NSSize(width: 640, height: 420)
        window.contentView = web
        window.center()
        window.setFrameAutosaveName("mproton Library")
        showMessage("Starting mproton Library…")
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)

        DispatchQueue.global().async { self.startServerAndLoad() }
    }

    // MARK: server

    func serverAnswers() -> Bool {
        var req = URLRequest(url: pageURL.appendingPathComponent("api/status"), timeoutInterval: 1)
        req.cachePolicy = .reloadIgnoringLocalCacheData
        let done = DispatchSemaphore(value: 0)
        var ok = false
        URLSession.shared.dataTask(with: req) { _, resp, _ in
            ok = (resp as? HTTPURLResponse)?.statusCode == 200
            done.signal()
        }.resume()
        done.wait()
        return ok
    }

    func startServerAndLoad() {
        if !serverAnswers() {
            guard let script = libraryScript() else {
                return fail("This app can't find library/mproton-library. Build it again with library/make-app.sh from your mproton checkout.")
            }
            guard pythonAvailable() else {
                return DispatchQueue.main.async { self.showPythonMissing() }
            }
            do {
                try startServer(script)
            } catch {
                return fail("Couldn't start \(script): \(error.localizedDescription)")
            }
            let deadline = Date().addingTimeInterval(20)
            while !serverAnswers() {
                if server?.isRunning == false {
                    return fail("The library server stopped while starting (exit code \(server!.terminationStatus)). Its output is in ~/Library/Logs/mproton-library.log.")
                }
                if Date() > deadline {
                    return fail("The library server didn't answer on port \(port) within 20 seconds. Its output is in ~/Library/Logs/mproton-library.log.")
                }
                Thread.sleep(forTimeInterval: 0.2)
            }
        }
        DispatchQueue.main.async { self.web.load(URLRequest(url: pageURL)) }
    }

    /// A checkout build (make-app.sh) names the checkout's script in Info.plist; a release build
    /// (make-app.sh --bundle) carries launcher/ and library/ in Contents/Resources/mproton.
    func libraryScript() -> String? {
        var candidates: [String] = []
        if let s = Bundle.main.object(forInfoDictionaryKey: "MprotonLibraryScript") as? String { candidates.append(s) }
        if let r = Bundle.main.resourceURL {
            candidates.append(r.appendingPathComponent("mproton/library/mproton-library").path)
        }
        return candidates.first { FileManager.default.isExecutableFile(atPath: $0) }
    }

    /// The same Pythons library/mproton-library tries. /usr/bin/python3 is only a stub that
    /// offers to install the Command Line Tools unless they (or Xcode) are installed.
    func pythonAvailable() -> Bool {
        let env = ProcessInfo.processInfo.environment
        if let p = env["MPROTON_PYTHON"], !p.isEmpty { return FileManager.default.isExecutableFile(atPath: p) }
        if ["/opt/homebrew/bin/python3", "/usr/local/bin/python3"].contains(where: {
            FileManager.default.isExecutableFile(atPath: $0) }) { return true }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/xcode-select")
        p.arguments = ["-p"]
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        guard (try? p.run()) != nil else { return false }
        p.waitUntilExit()
        return p.terminationStatus == 0
    }

    func showPythonMissing() {
        let html = """
        <!doctype html><meta charset="utf-8"><style>
        :root { color-scheme: light dark; }
        body { font: 14px/1.5 -apple-system, sans-serif; display: flex; align-items: center; justify-content: center;
          height: 100vh; margin: 0; padding: 0 40px; }
        div { max-width: 520px; } h2 { font-size: 18px; } code { font: 13px ui-monospace, Menlo, monospace; }
        a.button { display: inline-block; background: #0a66d8; color: #fff; padding: 7px 16px; border-radius: 7px;
          text-decoration: none; font-weight: 600; margin: 6px 0; }
        </style><body><div>
        <h2>mproton Library needs Python 3</h2>
        <p>The library and the mproton launcher are Python programs. The easiest way to get Python on a Mac
        is Apple's Command Line Tools, a free download from Apple of about 1 GB.</p>
        <p><a class="button" href="mproton-app:install-clt">Install the Command Line Tools…</a></p>
        <p>This opens Apple's installer (the same as running <code>xcode-select --install</code> in Terminal).
        When it has finished, choose <b>View → Reload</b> (⌘R). Python from Homebrew works too.</p>
        </div></body>
        """
        web.loadHTMLString(html, baseURL: nil)
    }

    func startServer(_ script: String) throws {
        try FileManager.default.createDirectory(at: logURL.deletingLastPathComponent(),
                                                withIntermediateDirectories: true)
        if !FileManager.default.fileExists(atPath: logURL.path) {
            FileManager.default.createFile(atPath: logURL.path, contents: nil)
        }
        let log = try FileHandle(forWritingTo: logURL)
        log.seekToEndOfFile()
        let p = Process()
        p.executableURL = URL(fileURLWithPath: script)
        p.arguments = ["--no-browser", "--port", String(port)]
        var env = ProcessInfo.processInfo.environment
        // Lets the library find the `wine` folder unpacked next to the app.
        env["MPROTON_APP_PATH"] = Bundle.main.bundlePath
        // The launcher's Python modules may sit inside this signed app; don't write
        // __pycache__ into it (games inherit this through the library's environment).
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        p.environment = env
        p.standardOutput = log
        p.standardError = log
        p.standardInput = FileHandle.nullDevice
        p.terminationHandler = { [weak self] proc in
            // The page's Quit button stops the server; close the app with it.
            DispatchQueue.main.async {
                guard let self = self, !self.quitting else { return }
                if proc.terminationReason == .exit && proc.terminationStatus == 0 {
                    NSApp.terminate(nil)
                } else {
                    self.fail("The library server stopped (exit code \(proc.terminationStatus)). Its output is in ~/Library/Logs/mproton-library.log.")
                }
            }
        }
        try p.run()
        server = p
    }

    func applicationWillTerminate(_ note: Notification) {
        quitting = true
        if let p = server, p.isRunning {
            p.terminate()  // SIGTERM to Python; running games are in their own sessions
            p.waitUntilExit()
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ app: NSApplication) -> Bool { true }

    // MARK: page

    func showMessage(_ text: String, error: Bool = false) {
        let esc = text.replacingOccurrences(of: "&", with: "&amp;").replacingOccurrences(of: "<", with: "&lt;")
        let html = """
        <!doctype html><meta charset="utf-8"><style>
        :root { color-scheme: light dark; }
        body { font: 14px -apple-system, sans-serif; display: flex; align-items: center; justify-content: center;
          height: 100vh; margin: 0; color: \(error ? "#c4231c" : "gray"); padding: 0 40px; text-align: center; }
        </style><body><p>\(esc)</p></body>
        """
        web.loadHTMLString(html, baseURL: nil)
    }

    func fail(_ text: String) {
        DispatchQueue.main.async { self.showMessage(text, error: true) }
    }

    @objc func reloadPage(_ sender: Any?) {
        if web.url?.host == "127.0.0.1" { web.reload() } else {
            showMessage("Starting mproton Library…")
            DispatchQueue.global().async { self.startServerAndLoad() }
        }
    }

    @objc func showLog(_ sender: Any?) {
        NSWorkspace.shared.open(logURL)
    }

    // Only the library page itself loads in the window; anything else (steam://, web links)
    // goes to the app macOS assigns it.
    func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction,
                 decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        guard let url = action.request.url else { return decisionHandler(.cancel) }
        if url.absoluteString == "mproton-app:install-clt" {
            let p = Process()
            p.executableURL = URL(fileURLWithPath: "/usr/bin/xcode-select")
            p.arguments = ["--install"]
            try? p.run()
            return decisionHandler(.cancel)
        }
        if url.scheme == "about" || (url.host == "127.0.0.1" && url.port == port) {
            return decisionHandler(.allow)
        }
        NSWorkspace.shared.open(url)
        decisionHandler(.cancel)
    }

    func webView(_ webView: WKWebView, createWebViewWith cfg: WKWebViewConfiguration,
                 for action: WKNavigationAction, windowFeatures: WKWindowFeatures) -> WKWebView? {
        if let url = action.request.url { NSWorkspace.shared.open(url) }
        return nil
    }

    func webView(_ webView: WKWebView, didFail nav: WKNavigation!, withError error: Error) {
        showMessage("Couldn't load the library page: \(error.localizedDescription)", error: true)
    }

    func webView(_ webView: WKWebView, didFailProvisionalNavigation nav: WKNavigation!, withError error: Error) {
        showMessage("Couldn't load the library page: \(error.localizedDescription)", error: true)
    }

    // WKWebView answers confirm()/alert() with "no" unless the app shows them.
    func webView(_ webView: WKWebView, runJavaScriptConfirmPanelWithMessage message: String,
                 initiatedByFrame frame: WKFrameInfo, completionHandler: @escaping (Bool) -> Void) {
        let a = NSAlert()
        a.messageText = message
        a.addButton(withTitle: "OK")
        a.addButton(withTitle: "Cancel")
        a.beginSheetModal(for: window) { completionHandler($0 == .alertFirstButtonReturn) }
    }

    func webView(_ webView: WKWebView, runJavaScriptAlertPanelWithMessage message: String,
                 initiatedByFrame frame: WKFrameInfo, completionHandler: @escaping () -> Void) {
        let a = NSAlert()
        a.messageText = message
        a.beginSheetModal(for: window) { _ in completionHandler() }
    }

    // MARK: menus (the Edit menu is what makes ⌘C/⌘V work in the page's text fields)

    func buildMenu() {
        let main = NSMenu()
        func menu(_ title: String, _ items: [NSMenuItem]) {
            let item = NSMenuItem()
            let m = NSMenu(title: title)
            items.forEach { m.addItem($0) }
            item.submenu = m
            main.addItem(item)
        }
        func item(_ title: String, _ sel: Selector?, _ key: String, _ mods: NSEvent.ModifierFlags = .command) -> NSMenuItem {
            let i = NSMenuItem(title: title, action: sel, keyEquivalent: key)
            i.keyEquivalentModifierMask = mods
            return i
        }
        menu("mproton Library", [
            item("About mproton Library", #selector(NSApplication.orderFrontStandardAboutPanel(_:)), ""),
            .separator(),
            item("Hide mproton Library", #selector(NSApplication.hide(_:)), "h"),
            item("Hide Others", #selector(NSApplication.hideOtherApplications(_:)), "h", [.command, .option]),
            item("Show All", #selector(NSApplication.unhideAllApplications(_:)), ""),
            .separator(),
            item("Quit mproton Library", #selector(NSApplication.terminate(_:)), "q"),
        ])
        menu("Edit", [
            item("Undo", Selector(("undo:")), "z"),
            item("Redo", Selector(("redo:")), "z", [.command, .shift]),
            .separator(),
            item("Cut", #selector(NSText.cut(_:)), "x"),
            item("Copy", #selector(NSText.copy(_:)), "c"),
            item("Paste", #selector(NSText.paste(_:)), "v"),
            item("Select All", #selector(NSText.selectAll(_:)), "a"),
        ])
        menu("View", [
            item("Reload", #selector(reloadPage(_:)), "r"),
            item("Show Server Log", #selector(showLog(_:)), "l", [.command, .shift]),
        ])
        menu("Window", [
            item("Minimize", #selector(NSWindow.performMiniaturize(_:)), "m"),
            item("Close", #selector(NSWindow.performClose(_:)), "w"),
        ])
        NSApp.mainMenu = main
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
// SIGTERM (kill, some logout paths) would skip applicationWillTerminate and leave the server
// running; turn it into a normal quit.
signal(SIGTERM, SIG_IGN)
let sigterm = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
sigterm.setEventHandler { NSApp.terminate(nil) }
sigterm.resume()
app.setActivationPolicy(.regular)
app.run()
