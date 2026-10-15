#!/usr/bin/env python3
"""Fake omachatd for testing tui_chat. Same wire protocol, fictional data only.

Flags: -socket PATH, -log-level LEVEL (accepted and ignored), -version
Env:   FAKE_STATE_DIR  where state, the request log and the inject file live
       FAKE_PAIR_FAIL  when set, pairFromBrowser fails with a hint
A file named fail_next in FAKE_STATE_DIR makes the next pairing attempt fail,
and is removed when it does.
Inject an incoming message by appending a line "chatId|text|senderId" to
$FAKE_STATE_DIR/inject.
"""
import json, os, socket, struct, subprocess, sys, threading, time, uuid, zlib

args = sys.argv[1:]
if "-version" in args:
    print("0.5.5-fake")
    sys.exit(0)

sock_path = None
if "-socket" in args:
    sock_path = args[args.index("-socket") + 1]
if not sock_path:
    sock_path = os.path.join(os.environ["XDG_RUNTIME_DIR"], "omachat", "daemon.sock")

state_dir = os.environ["FAKE_STATE_DIR"]
os.makedirs(state_dir, exist_ok=True)
state_file = os.path.join(state_dir, "state.json")
log_file = os.path.join(state_dir, "requests.log")
inject_file = os.path.join(state_dir, "inject")

lock = threading.RLock()
clients = []


def load_state():
    try:
        with open(state_file) as f:
            return json.load(f)
    except Exception:
        return {"enabled": [], "paired": False}


def save_state(st):
    with open(state_file, "w") as f:
        json.dump(st, f)


def log(line):
    with open(log_file, "a") as f:
        f.write(line + "\n")


state = load_state()
transient_state = None  # "gaiaPairing" or "error" while pairing runs
transient_emoji = ""
transient_error = ("", "")

NOW = int(time.time() * 1_000_000)
ME = {"id": "me1", "name": "You", "number": "+15550100", "isMe": True, "initials": "Y"}
ALEX = {"id": "u_alex", "name": "Alex Rivera", "number": "+15550111", "isMe": False, "initials": "AR"}
SAM = {"id": "u_sam", "name": "Sam Okafor", "number": "+15550122", "isMe": False, "initials": "SO"}
JO = {"id": "u_jo", "name": "Jo Lindgren", "number": "+15550133", "isMe": False, "initials": "JL"}
STRANGER = {"id": "u_x", "name": "", "number": "+1 555 0142", "isMe": False, "initials": "#"}

convs = {
    "c_alex": {"id": "c_alex", "name": "Alex Rivera", "isGroup": False, "unread": False, "participants": [ME, ALEX]},
    "c_club": {"id": "c_club", "name": "Book club", "isGroup": True, "unread": False, "participants": [ME, SAM, JO]},
    "c_num": {"id": "c_num", "name": "+1 555 0142", "isGroup": False, "unread": True, "participants": [ME, STRANGER]},
}
messages = {k: [] for k in convs}
counter = [0]


def add_message(chat, text, from_me, sender, minutes_ago, attachments=None, mid=None, tmp=""):
    counter[0] += 1
    m = {
        "id": mid or "m%04d" % counter[0],
        "conversationID": chat,
        "text": text,
        "timestamp": NOW - minutes_ago * 60 * 1_000_000 + counter[0],
        "fromMe": from_me,
        "senderID": "" if from_me else sender["id"],
        "senderName": "" if from_me else sender["name"],
        "failed": False,
        "pending": False,
        "deleted": False,
        "delivery": "delivered" if from_me else "",
    }
    if tmp:
        m["tmpID"] = tmp
    if attachments:
        m["attachments"] = attachments
    messages[chat].append(m)
    return m


for i in range(120):  # enough to need paging
    add_message("c_alex", "alex message %03d" % i, i % 3 == 0, ALEX, 600 - i)
for i, (who, text) in enumerate([
    (SAM, "who has the next pick"), (JO, "I can do the 14th"), (SAM, "great, the 14th it is"),
]):
    add_message("c_club", text, False, who, 90 - i)
add_message("c_club", "", False, JO, 80, attachments=[{
    "key": "k1", "mediaID": "x1", "name": "cover.jpg", "mimeType": "image/jpeg",
    "isImage": True, "isGif": False, "isAudio": False, "isVideo": False}])
# an iPhone photo: the helper names a type it does not know ".bin"
add_message("c_club", "", False, SAM, 70, attachments=[{
    "key": "k2", "mediaID": "x2", "name": "IMG_2817.heic", "mimeType": "image/heic",
    "isImage": True, "isGif": False, "isAudio": False, "isVideo": False}])
add_message("c_num", "Hi, is this the right number for the plumber?", False, STRANGER, 5)


def conv_json(c):
    msgs = messages[c["id"]]
    last = max(msgs, key=lambda m: m["timestamp"]) if msgs else None
    return {
        "id": c["id"], "name": c["name"],
        "preview": last["text"] if last else "", "previewMine": bool(last and last["fromMe"]),
        "timestamp": last["timestamp"] if last else 0,
        "unread": c["unread"], "isGroup": c["isGroup"], "readOnly": False, "pinned": False,
        "avatarColor": "#888888", "initials": "".join(p["initials"] for p in c["participants"][1:2]),
        "outgoingID": "me1", "participants": c["participants"],
    }


def current_state():
    if transient_state:
        return transient_state
    if "gmessages" not in state["enabled"]:
        return "disabled"
    return "connected" if state["paired"] else "unpaired"


def status_json(network="gmessages"):
    if network != "gmessages":
        return {"network": network, "state": "disabled", "unread": 0, "phoneOK": True}
    st = {"network": "gmessages", "state": current_state(), "unread": sum(1 for c in convs.values() if c["unread"]),
          "phoneOK": True}
    if transient_emoji:
        st["emoji"] = transient_emoji
    if transient_state == "error":
        st["error"], st["hint"] = transient_error
    return st


def send_to(conn, obj):
    try:
        conn.sendall((json.dumps(obj, ensure_ascii=False) + "\n").encode())
    except OSError:
        pass


def broadcast(obj):
    with lock:
        for c in list(clients):
            send_to(c, obj)


def broadcast_status():
    broadcast({"event": "status", "network": "gmessages", "data": status_json()})


def config_json(restart=False):
    return {"enabledServices": state["enabled"], "serviceSelectionRequired": not state["enabled"],
            "restartRequired": restart, "uiScale": 1, "notificationsEnabled": True}


def pair_thread():
    global transient_state, transient_emoji, transient_error
    # Like the real helper, answer first and only then move to the pairing state, so
    # a client that trusts the state it saw before asking is caught out. The state
    # left behind by an earlier failed attempt stays visible until here.
    time.sleep(0.3)
    transient_state = "gaiaPairing"
    broadcast_status()
    time.sleep(0.5)
    fail_file = os.path.join(state_dir, "fail_next")
    fail = bool(os.environ.get("FAKE_PAIR_FAIL")) or os.path.exists(fail_file)
    if os.path.exists(fail_file):
        os.remove(fail_file)
    if fail:
        transient_state = "error"
        transient_error = ("Not signed in to Google Messages", "Open messages.google.com/web in a browser, then try again.")
        broadcast_status()
        return
    transient_emoji = "\U0001F984"
    broadcast({"event": "emoji", "network": "gmessages", "data": {"emoji": transient_emoji}})
    broadcast_status()
    time.sleep(2.0)
    with lock:
        state["paired"] = True
        save_state(state)
    transient_state = None
    transient_emoji = ""
    broadcast_status()


def finalize_send(tmp, chat, text):
    time.sleep(1.0)
    with lock:
        m = add_message(chat, text, True, ME, 0, mid="real_" + uuid.uuid4().hex[:8], tmp=tmp)
        m["delivery"] = "sent"
        broadcast({"event": "message", "network": "gmessages", "data": m, "notify": False})
        broadcast({"event": "conversation", "network": "gmessages", "data": conv_json(convs[chat])})


def make_png(path, w=480, h=300):
    """A real PNG with a gradient and a bright block, so a thumbnail has something to show."""
    rows = []
    for y in range(h):
        row = bytearray([0])
        for x in range(w):
            if 120 <= x < 360 and 75 <= y < 225:
                row += bytes((250, 200, 40))
            else:
                row += bytes((x * 255 // w, y * 255 // h, 120))
        rows.append(bytes(row))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def handle(req):
    global transient_state
    method = req.get("method", "")
    params = req.get("params") or {}
    rid = req.get("id", "")
    # log the method and which keys it carried, never the values
    log("%s %s" % (method, ",".join(sorted(params.keys()))))

    def ok(result=None):
        return {"id": rid, "ok": True, "result": result}

    def fail(msg):
        return {"id": rid, "ok": False, "error": msg}

    if method == "buildInfo":
        return ok({"sourceID": "fake"})
    if method == "config":
        return ok(config_json())
    if method == "status":
        return ok(status_json())
    if method == "setEnabledServices":
        new = params.get("enabledServices", [])
        changed = set(new) != set(state["enabled"])
        with lock:
            state["enabled"] = new
            save_state(state)
        if changed:
            threading.Timer(0.4, lambda: os._exit(0)).start()  # the real helper restarts itself
        return ok(config_json(restart=changed))
    if "gmessages" not in state["enabled"]:
        return fail("service disabled; enable it in Settings before using it")
    if method == "pairFromBrowser":
        threading.Thread(target=pair_thread, daemon=True).start()
        return ok(None)
    if method == "startPairing":
        return fail("QR pairing is not offered by this fake")
    if current_state() != "connected":
        return fail("not connected to Google Messages (state: %s)" % current_state())
    if method == "media":
        key = params.get("key", "")
        if not key:
            return fail("missing attachment key")
        if key == "k2":
            path = os.path.join(state_dir, "media_k2.bin")
            if not os.path.exists(path):
                src = os.path.join(state_dir, "media_k2_src.png")
                make_png(src, 240, 320)
                out = subprocess.run(["magick", src, "heic:" + path], capture_output=True)
                if out.returncode != 0:
                    return fail("cannot make a heic: " + out.stderr.decode()[:80])
        else:
            path = os.path.join(state_dir, "media_%s.png" % key)
            if not os.path.exists(path):
                make_png(path)
        with open(os.path.join(state_dir, "media_served.log"), "a") as served:
            served.write(key + "\n")
        return ok({"key": key, "path": path})
    if method == "conversations":
        count = params.get("count", 0)
        out = sorted((conv_json(c) for c in convs.values()), key=lambda c: -c["timestamp"])
        return ok(out[:count] if count else out)
    if method == "messages":
        chat = params.get("conversationID")
        if chat not in convs:
            return fail("unknown conversation")
        count = int(params.get("count") or 40)
        cursor_time = params.get("cursorTime")
        msgs = sorted(messages[chat], key=lambda m: m["timestamp"])
        pool = [m for m in msgs if m["timestamp"] < cursor_time] if cursor_time else msgs
        page = pool[-count:]
        older = len(pool) - len(page)
        res = {"conversationID": chat, "messages": page, "hasMore": older > 0}
        if page:
            res["cursorID"], res["cursorTime"] = page[0]["id"], page[0]["timestamp"]
        return ok(res)
    if method == "send":
        chat = params.get("conversationID")
        text = params.get("text", "")
        if chat not in convs:
            return fail("unknown conversation %r" % chat)
        if not text:
            return fail("empty message")
        tmp = str(uuid.uuid4())
        prov = {"id": tmp, "tmpID": tmp, "provisional": True, "conversationID": chat, "text": text,
                "timestamp": int(time.time() * 1_000_000), "fromMe": True, "pending": True, "failed": False,
                "deleted": False}
        threading.Thread(target=finalize_send, args=(tmp, chat, text), daemon=True).start()
        return ok(prov)
    if method == "markRead":
        chat = params.get("conversationID")
        if chat in convs and convs[chat]["unread"]:
            convs[chat]["unread"] = False
            broadcast({"event": "conversation", "network": "gmessages", "data": conv_json(convs[chat])})
        return ok(None)
    return fail("unknown method %r" % method)


def client_thread(conn):
    with lock:
        clients.append(conn)
    for net in ("gmessages", "whatsapp", "telegram", "messenger"):
        send_to(conn, {"event": "status", "network": net, "data": status_json(net)})
    buf = b""
    try:
        while True:
            data = conn.recv(65536)
            if not data:
                break
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if not line.strip():
                    continue
                try:
                    req = json.loads(line)
                except ValueError:
                    continue
                send_to(conn, handle(req))
    finally:
        with lock:
            if conn in clients:
                clients.remove(conn)
        conn.close()


def inject_watcher():
    while True:
        time.sleep(0.3)
        if not os.path.exists(inject_file):
            continue
        with open(inject_file) as f:
            lines = [l.rstrip("\n") for l in f if l.strip()]
        os.remove(inject_file)
        for line in lines:
            chat, text, sender = (line.split("|") + ["", ""])[:3]
            who = {"u_sam": SAM, "u_jo": JO, "u_alex": ALEX, "u_x": STRANGER}.get(sender, ALEX)
            with lock:
                if chat in ("c_num",):
                    convs[chat]["unread"] = True
                else:
                    convs[chat]["unread"] = True
                m = add_message(chat, text, False, who, 0)
                # a message that arrives now carries the time it arrives, not the helper's start
                m["timestamp"] = int(time.time() * 1_000_000)
                broadcast({"event": "message", "network": "gmessages", "data": m, "notify": True})
                broadcast({"event": "conversation", "network": "gmessages", "data": conv_json(convs[chat])})


os.makedirs(os.path.dirname(sock_path), exist_ok=True)
try:
    os.unlink(sock_path)
except FileNotFoundError:
    pass
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(sock_path)
srv.listen(8)
with open(os.path.join(state_dir, "pid"), "w") as f:
    f.write(str(os.getpid()))
log("started")
threading.Thread(target=inject_watcher, daemon=True).start()
while True:
    conn, _ = srv.accept()
    threading.Thread(target=client_thread, args=(conn,), daemon=True).start()
