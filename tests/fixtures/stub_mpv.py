#!/usr/bin/env python3
"""stub_mpv.py — a fake mpv for the P7 pty test (PORT_CPP.md P7 DoD).

Speaks just enough of mpv's JSON IPC (03 §6.3.1) for player::play() to see a
clean play-through: bind+listen on the --input-ipc-server= path (mpv IS the
socket server, player.cpp connects out to it — see connect_ipc), accept one
connection, ignore the observe_property handshake, emit a couple of
property-change events for time-pos (id 1) then duration (id 2), then exit 0
(a clean exit with a meaningful last position is the PlayDone happy path).

Not a general mpv stand-in — only the one behavior this test drives.
"""
import json
import socket
import sys
import time

SOCK_PATH = None
for arg in sys.argv[1:]:
    if arg.startswith("--input-ipc-server="):
        SOCK_PATH = arg.split("=", 1)[1]

if SOCK_PATH is None:
    sys.exit(1)

srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(SOCK_PATH)
srv.listen(1)
conn, _ = srv.accept()

def send_event(id_, data):
    line = json.dumps({"event": "property-change", "id": id_, "data": data}) + "\n"
    conn.sendall(line.encode("utf-8"))

try:
    send_event(2, 24.0)     # duration
    time.sleep(0.05)
    send_event(1, 3.0)      # time-pos, early
    time.sleep(0.05)
    send_event(1, 20.0)     # time-pos, >= 0.80 * duration -> "episode done"
    time.sleep(0.05)
finally:
    conn.close()
    srv.close()

sys.exit(0)
