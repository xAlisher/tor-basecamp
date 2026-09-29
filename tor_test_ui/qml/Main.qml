import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Controls.Material 2.15
import QtQuick.Layouts 1.15

// tor_test_ui — dev harness for the `tor` module. Every call, grouped into the
// four real scenarios: Ready (is tor up), Fetch (client), Host (server), Pair
// (v3 client auth). Not for operators.
Rectangle {
    id: root
    anchors.fill: parent
    color: "#0b0d12"
    Material.theme: Material.Dark
    Material.accent: "#7c3aed"
    Material.primary: "#7c3aed"
    Material.background: "#12151d"
    Material.foreground: "#e8e6f0"

    readonly property color panel:  "#12151d"
    readonly property color border: "#262a35"
    readonly property color ink:    "#e8e6f0"
    readonly property color dim:    "#8b8ba0"
    readonly property color ok:     "#1f7a52"
    readonly property color bad:    "#b5303a"
    readonly property int   pad:    14

    // last state for cross-tab reuse
    property string lastOnion: ""
    property string lastPub: ""
    property string lastPriv: ""

    // ── bridge ──────────────────────────────────────────────────────────────
    function parse(s) {
        try { var t = JSON.parse(s); if (typeof t === "string") { try { return JSON.parse(t) } catch(e) { return t } } return t }
        catch(e) { return { raw: s } }
    }
    // call("method", [args]) -> unwrapped value object; also logs raw to the console.
    function call(method, args) {
        if (typeof logos === "undefined" || !logos.callModule) { log(method, { raw: "bridge unavailable" }); return {} }
        var raw = logos.callModule("tor", method, args || [])
        var r = parse(raw)
        // A failed call carries its message in r.error; r.value is null on that path.
        // Surface the error instead of logging a bare "null" that hides the cause.
        if (r && typeof r === "object" && r.success === false) {
            var e = { ok: false, error: r.error || "(failed, no message)" }
            log(method, e); return e
        }
        var v = (r && typeof r === "object" && r.value !== undefined && r.success !== undefined) ? r.value : r
        log(method, v)
        return v
    }
    // Same IPC as call(), but does NOT write the shared console — for the
    // background status poll, so it can't clobber a result you just clicked.
    function callQuiet(method, args) {
        if (typeof logos === "undefined" || !logos.callModule) return {}
        var r = parse(logos.callModule("tor", method, args || []))
        return (r && typeof r === "object" && r.value !== undefined && r.success !== undefined) ? r.value : r
    }
    function log(method, v) {
        console.log("[tor_test_ui] " + method + " -> " + JSON.stringify(v))
        resultConsole.text = method + "  →\n" + JSON.stringify(v, null, 2)
    }
    // clipboard — TextEdit.copy() is the portable QML route to the system clipboard.
    function copyText(s) { if (!s || !s.length) return; clip.text = "" + s; clip.selectAll(); clip.copy(); clip.deselect(); clip.text = "" }
    TextEdit { id: clip; visible: false; width: 0; height: 0 }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: root.pad
        spacing: root.pad

        // header
        RowLayout {
            Layout.fillWidth: true
            Label { text: "Tor module — test harness"; font.pixelSize: 20; font.bold: true; color: root.ink }
            Item { Layout.fillWidth: true }
            Label { id: readyPill; text: "● checking"; color: root.dim; font.pixelSize: 13 }
        }

        TabBar {
            id: tabs
            Layout.fillWidth: true
            TabButton { text: "Ready" }
            TabButton { text: "Fetch" }
            TabButton { text: "Host" }
            TabButton { text: "Pair" }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabs.currentIndex

            // ══ 1. READY ══════════════════════════════════════════════════════
            ScrollView {
                clip: true
                ColumnLayout {
                    width: parent.width; spacing: root.pad
                    Section { title: "Is tor up? (status · get_socks_endpoint · new_circuit)" }
                    RowLayout {
                        Layout.fillWidth: true; spacing: root.pad
                        ProgressBar { id: bootBar; from: 0; to: 100; value: 0; Layout.fillWidth: true }
                        Label { id: bootLbl; text: "0%"; color: root.dim; font.pixelSize: 13 }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: root.pad
                        Label { id: sockLbl; text: "socks: —"; color: root.dim; font.pixelSize: 13; font.family: "monospace"; Layout.fillWidth: true }
                        CopyBtn { label: "Copy socks"; value: sockLbl.text.indexOf(":") >= 0 ? sockLbl.text.replace("socks: ","") : "" }
                    }
                    Label { id: verLbl;  text: "tor: —";  color: root.dim; font.pixelSize: 13; font.family: "monospace" }
                    RowLayout {
                        spacing: root.pad
                        Button { text: "Refresh status"; onClicked: root.log("status", root.pollStatus()) }
                        Button { text: "get_socks_endpoint"; onClicked: { var e = root.call("get_socks_endpoint", []); sockLbl.text = "socks: " + (e.host||"?") + ":" + (e.port||"?") } }
                        Button { text: "new_circuit"; onClicked: root.call("new_circuit", [JSON.stringify({})]) }
                    }
                }
            }

            // ══ 2. FETCH (client) ═════════════════════════════════════════════
            ScrollView {
                clip: true
                ColumnLayout {
                    width: parent.width; spacing: root.pad
                    Section { title: "Buffered HTTP over Tor (http_request)" }
                    RowLayout {
                        Layout.fillWidth: true; spacing: root.pad
                        ComboBox { id: methodBox; model: ["GET","POST","PUT","PATCH","DELETE","HEAD"] }
                        TextField { id: urlField; Layout.fillWidth: true; placeholderText: "https://check.torproject.org/api/ip  or  http://<id>.onion/path" }
                    }
                    TextField { id: isoField; Layout.fillWidth: true; placeholderText: "isolation_tag (optional — pins its own circuit)" }
                    Label { text: "Headers (one 'Key: Value' per line):"; color: root.dim; font.pixelSize: 12 }
                    TextArea { id: headersArea; Layout.fillWidth: true; Layout.preferredHeight: 60; wrapMode: TextArea.Wrap; background: Rectangle { color: root.panel; border.color: root.border } }
                    Label { text: "Body (sent base64-encoded):"; color: root.dim; font.pixelSize: 12 }
                    TextArea { id: bodyArea; Layout.fillWidth: true; Layout.preferredHeight: 60; wrapMode: TextArea.Wrap; background: Rectangle { color: root.panel; border.color: root.border } }
                    RowLayout {
                        spacing: root.pad
                        Button {
                            text: "Send over Tor"; highlighted: true
                            onClicked: {
                                var hdrs = {}
                                var lines = headersArea.text.split("\n")
                                for (var i = 0; i < lines.length; i++) {
                                    var c = lines[i].indexOf(":"); if (c < 0) continue
                                    hdrs[lines[i].substring(0,c).trim()] = lines[i].substring(c+1).trim()
                                }
                                var req = { method: methodBox.currentText, url: urlField.text, headers: hdrs, timeout_ms: 45000 }
                                if (isoField.text.length) req.isolation_tag = isoField.text
                                if (bodyArea.text.length) req.body_b64 = Qt.btoa(bodyArea.text)
                                var r = root.call("http_request", [JSON.stringify(req)])
                                respLbl.text = "status " + (r.status||0) + "  kind=" + (r.error_kind||"") +
                                    "\n" + (r.body_b64 ? Qt.atob(r.body_b64) : "(empty)")
                            }
                        }
                        Button { text: "new_circuit"; onClicked: root.call("new_circuit", [JSON.stringify({})]) }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: root.pad
                        Label { text: "HTTP response:"; color: root.dim; font.pixelSize: 12; Layout.fillWidth: true }
                        CopyBtn { label: "Copy"; value: respLbl.text }
                    }
                    TextArea { id: respLbl; Layout.fillWidth: true; Layout.preferredHeight: 90; readOnly: true; selectByMouse: true; wrapMode: TextArea.Wrap
                               color: root.ink; background: Rectangle { color: "#0e1017"; border.color: root.border } }
                }
            }

            // ══ 3. HOST (server) ══════════════════════════════════════════════
            ScrollView {
                clip: true
                ColumnLayout {
                    width: parent.width; spacing: root.pad
                    Section { title: "Host a v3 onion (create/status/remove_onion_service)" }
                    RowLayout {
                        Layout.fillWidth: true; spacing: root.pad
                        TextField { id: localPortField; placeholderText: "local_port (e.g. 8099)"; Layout.preferredWidth: 160 }
                        TextField { id: vportField; text: "80"; placeholderText: "virtual_port"; Layout.preferredWidth: 120 }
                        TextField { id: persistField; placeholderText: "persist_id (optional, stable .onion)"; Layout.fillWidth: true }
                    }
                    CheckBox { id: reqAuthBox; text: "require client authorization (V3Auth)" }
                    RowLayout {
                        spacing: root.pad
                        Button {
                            text: "create_onion_service"; highlighted: true
                            onClicked: {
                                var req = { local_port: parseInt(localPortField.text)||0, virtual_port: parseInt(vportField.text)||80, require_auth: reqAuthBox.checked }
                                if (persistField.text.length) req.persist_id = persistField.text
                                var r = root.call("create_onion_service", [JSON.stringify(req)])
                                if (r.onion) { root.lastOnion = r.onion; onionLbl.text = r.onion; pairHostId.text = r.id || "" }
                            }
                        }
                        Button { text: "status"; onClicked: root.call("onion_service_status", [JSON.stringify({ id: root.lastOnion.replace(".onion","") })]) }
                        Button { text: "remove"; onClicked: root.call("remove_onion_service", [JSON.stringify({ id: root.lastOnion.replace(".onion","") })]) }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: root.pad
                        Label { id: onionLbl; text: "onion: —"; color: root.ink; font.pixelSize: 13; font.family: "monospace"; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere }
                        CopyBtn { label: "Copy .onion"; value: root.lastOnion }
                        CopyBtn { label: "Copy http URL"; value: root.lastOnion.length ? ("http://" + root.lastOnion + "/") : "" }
                    }
                }
            }

            // ══ 4. PAIR (v3 client auth) ══════════════════════════════════════
            ScrollView {
                clip: true
                ColumnLayout {
                    width: parent.width; spacing: root.pad
                    Section { title: "Pairing — the two auth halves" }
                    Button {
                        text: "generate_client_auth_keypair"; highlighted: true
                        onClicked: { var r = root.call("generate_client_auth_keypair", []); root.lastPub = r["public"]||""; root.lastPriv = r["private"]||""; kpLbl.text = "public (base32): " + root.lastPub + "\nprivate (base64): " + root.lastPriv }
                    }
                    TextArea { id: kpLbl; Layout.fillWidth: true; Layout.preferredHeight: 60; readOnly: true; selectByMouse: true; wrapMode: TextArea.WrapAnywhere; color: root.ink; background: Rectangle { color: "#0e1017"; border.color: root.border } }
                    RowLayout {
                        spacing: root.pad
                        CopyBtn { label: "Copy public (base32)"; value: root.lastPub }
                        CopyBtn { label: "Copy private (base64)"; value: root.lastPriv }
                    }

                    Section { title: "Server side — authorize a client on a hosted onion" }
                    TextField { id: pairHostId; Layout.fillWidth: true; placeholderText: "hosted service id (from Host tab)" }
                    TextField { id: authPubField; Layout.fillWidth: true; placeholderText: "client_public (base32)"; text: root.lastPub }
                    RowLayout {
                        spacing: root.pad
                        Button { text: "authorize_client"; onClicked: root.call("authorize_client", [JSON.stringify({ id: pairHostId.text, client_public: authPubField.text })]) }
                        Button { text: "deauthorize"; onClicked: root.call("deauthorize_client", [JSON.stringify({ id: pairHostId.text, client_public: authPubField.text })]) }
                        Button { text: "list_authorized_clients"; onClicked: root.call("list_authorized_clients", [JSON.stringify({ id: pairHostId.text })]) }
                    }

                    Section { title: "Client side — register a key to reach an auth onion" }
                    TextField { id: caHostField; Layout.fillWidth: true; placeholderText: "onion_host (e.g. <id>.onion)"; text: root.lastOnion }
                    TextField { id: caPrivField; Layout.fillWidth: true; placeholderText: "private_key (base64)"; text: root.lastPriv }
                    RowLayout {
                        spacing: root.pad
                        Button { text: "register_client_auth"; onClicked: root.call("register_client_auth", [JSON.stringify({ onion_host: caHostField.text, private_key: caPrivField.text })]) }
                        Button { text: "remove_client_auth"; onClicked: root.call("remove_client_auth", [JSON.stringify({ onion_host: caHostField.text })]) }
                        Button { text: "list_client_auth"; onClicked: root.call("list_client_auth", []) }
                    }
                }
            }
        }

        // ── shared response console ──────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true; spacing: root.pad
            Label { text: "Last response:"; color: root.dim; font.pixelSize: 12; Layout.fillWidth: true }
            CopyBtn { label: "Copy"; value: resultConsole.text }
            Button { text: "Clear"; onClicked: { resultConsole.text = ""; respLbl.text = "" } }
        }
        // ScrollView caps the height (TextArea's content-height acts as a floor in a
        // Layout, so a bare TextArea grows off-screen); now it scrolls internally.
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 130; Layout.maximumHeight: 130
            clip: true
            background: Rectangle { color: "#0e1017"; border.color: root.border }
            TextArea {
                id: resultConsole
                readOnly: true; selectByMouse: true; persistentSelection: true
                wrapMode: TextArea.WrapAnywhere; font.family: "monospace"; font.pixelSize: 12
                color: root.ink
                text: "Responses appear here."
            }
        }
    }

    // status poller — updates the Ready-tab widgets only (quiet: never touches
    // the shared console). Returns the status so the manual button can log it.
    function pollStatus() {
        var s = root.callQuiet("status", [])
        var p = s.progress || 0
        bootBar.value = p; bootLbl.text = p + "%"
        readyPill.text = s.bootstrapped ? "● tor up" : ("● bootstrapping " + p + "%")
        readyPill.color = s.bootstrapped ? root.ok : root.dim
        if (s.socks_port) sockLbl.text = "socks: " + (s.socks_host||"127.0.0.1") + ":" + s.socks_port
        if (s.tor_version) verLbl.text = "tor: " + s.tor_version
        if (s.bootstrapped) pollTimer.running = false   // stop churning once up; manual Refresh still works
        return s
    }
    Timer { id: pollTimer; interval: 3000; running: true; repeat: true; triggeredOnStart: true; onTriggered: root.pollStatus() }

    // tiny helpers as inline components
    component Section : Label { property string title; text: title; color: root.ink; font.pixelSize: 15; font.bold: true; Layout.topMargin: root.pad }
    // one-click copy for a value; disables itself when there's nothing to copy, and
    // flashes "Copied ✓" briefly so the click is confirmed.
    // Plain Material button (matches every other button's size); the earlier
    // "empty pill" was a forced implicitHeight clipping the label, not the style.
    component CopyBtn : Button {
        id: cb
        property string value: ""
        property string label: "Copy"
        text: label
        enabled: value.length > 0
        onClicked: { root.copyText(cb.value); cb.text = "Copied ✓"; copiedTimer.restart() }
        Timer { id: copiedTimer; interval: 900; onTriggered: cb.text = cb.label }
    }
}
