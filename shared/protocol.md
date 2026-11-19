# ScreenLink Protocol v0.1 (prototype only)
Transport: TCP port 48765, UTF-8, one JSON object per line (JSON Lines). Maximum accepted line: 4096 bytes (per line; several lines may arrive in one read). `\r\n` line endings are accepted. Messages are parsed as real JSON, so whitespace and key order do not matter. No authentication or encryption: LAN lab only.

`HELLO` must carry `"version":1`; any other version gets `{"type":"ERROR","message":"unsupported protocol version"}`. Malformed lines get `invalid JSON`, a missing `type` gets `missing type`.

Client -> Server:
`{"type":"HELLO","client":"android","version":1}`
`{"type":"PING"}`
`{"type":"SET_MODE","mode":"MIRROR"}` (also `EXTEND`, `REMOTE`)

Server -> Client:
`{"type":"WELCOME","server":"screenlink-windows","version":1}`
`{"type":"PONG"}`
`{"type":"MODE_ACK","mode":"MIRROR","implemented":false}`
`{"type":"ERROR","message":"..."}`

Messages do not yet imply actual screen streaming. Future production protocol should add session IDs, capability negotiation, authenticated pairing, encryption, versioning, replay resistance and authorization.


## v1 additions (0.2–0.4)
- Video: TCP 48766. Client sends first line `{"t":"cfg","fps":30|60,"bitrate":N,"max_width":N,"display":"primary"|"virtual","width":W,"height":H,"encoder":"auto"|"software"}`.
  Server replies `SLV1` u32 width u32 height u32 fps, then frames `u32 len, u64 pts_us, H.264 Annex-B`; or `SLER` u32 len + UTF-8 message on error.
- Input (client -> server, JSON lines on the video socket): `p` pointer d/m/u/r, `s` scroll, `k` text, `v` key, `c` combo, `pen` h/d/m/u/x with p (0..1), tx/ty (deg), e (eraser), b (barrel).
- Discovery: UDP broadcast `SCREENLINK_DISCOVER` to port 48767; reply `{"type":"HERE","host_name":...,"port":48765,"video_port":48766,"version":1}`.

## 1.0 additions
- Audio: TCP 48768. Server sends header `SLA1` + u32 sample_rate + u32 channels (2) + u32 bits (16), then chunks of u32 length + interleaved PCM16 little-endian (~10 ms each). The client closes the socket to stop. The client drops buffered audio beyond ~120 ms to keep latency low.
- Pen input on the control channel: `{"t":"pen","a":"h|d|m|u|x","x":..,"y":..,"p":0..1,"tx":deg,"ty":deg,"e":bool,"b":bool}` (h = hover, x = leave, e = eraser, b = barrel button).
- Video error: instead of `SLV1` the server may send `SLER` + u32 length + UTF-8 message, then close.

## 1.2 additions
Control channel (TCP 48765):
- HELLO may carry `"id"` (stable per device) and `"device"`. The server then sends `{"type":"INFO","mac":"AA:BB:..","ip":"..","auth":"ok|pin","nonce":"<32 hex>"}` (`nonce` only while not yet accepted; new for every connection).
- Wi-Fi clients (not 127.0.0.1) must pass one of these before anything else (2.0.1):
  - First time, with the 6-digit PIN: `{"type":"AUTH","pin":"123456","id":..,"device":..}` → `{"type":"AUTH_OK","key":"<64 hex>"}`. The device keeps `key` for this PC (by its `mac`); the PC keeps it encrypted with DPAPI in HKCU\Software\ScreenLink\Trusted (names in ...\TrustedNames).
  - Later, with the key: `{"type":"AUTH","proof":hex(HMAC-SHA256(key, nonce + id)),"id":..,"device":..}` → `AUTH_OK`, or `{"type":"AUTH_FAIL","reset":true}` (the device forgets the key and asks for the PIN).
  - A wrong PIN → `{"type":"AUTH_FAIL","wait":seconds}` (a key proof is never made to wait). Every 5 wrong PINs from one address lock it (30 s, then longer, up to 15 min); at most one wrong PIN a second overall; after 20 wrong PINs in total the PC makes a new PIN and shows a notice.
  - The device id alone no longer lets a device in. Devices paired before 2.0.1 enter the PIN once more.
- Video and audio connections are refused from addresses that have not passed.
- `{"type":"STREAM_STOPPED"}` (server → client): the PC stopped the stream on purpose; the client closes the stream screen instead of reconnecting. A stream that drops otherwise is retried by the client for up to a minute.
- `{"type":"ACTION","name":"key","vk":175}`, `"combo"` (`keys`), `"lock"`, `"sleep"`, `"restart"`, `"shutdown"`, `"open"` (`app`: calc, notepad, explorer, paint, taskmgr, settings, browser) → `ACTION_DONE`.
- `{"type":"CLIP","text":".."}` both ways (≤1500 characters).
- Server → client every 2 s: `{"type":"STATS","cpu":12,"ram":64,"ram_used_gb":9.9,"ram_total_gb":15.3}`.

Input on the video connection: `rel` (dx, dy), `btn` (b: l/r/m, a: d/u/c), `kd`/`ku` (vk), `zoom` (x, y, dy).

## 2.0 additions
- cfg: `"codec":"hevc"` (answer `SLV2` + u32 codec: 0 H.264, 1 H.265), `"fps"` up to 120, `"window":<hwnd>` (one window), `"display":"pad"` (drawing pad: header only, no frames; input mapped to the main screen). Taller-than-wide extend sizes turn the virtual screen upright.
- Video channel client → server: `{"t":"br","bitrate":N}` (adaptive quality), `{"t":"kf"}` (keyframe now).
- Control: `WINDOWS` → `{"type":"WINDOWS","list":[{"id","title"}]}`; `VOLUMES` → master + per-program; `VOLUME` (pid, v, m); `INPUT` (`e` = an input JSON); actions `screenshot`, `annotate` (mode on/off/clear/toggle), `color` → `COLOR`. Server pushes `FILE` offers and `APP` (foreground exe).
- Files TCP 48769: `put`, `get`, `speed` (size → that many bytes).
