# Web API (protocol 2)

UI and firmware may change this freely, they ship together. The station says
which protocol it speaks; a page with another one reloads itself.

## Rules for adding to it

- **State goes into the snapshot, changes go over the websocket.** A list that
  can grow does not belong into a websocket frame.
- **No frame over 3 KB.** Bigger ones are dropped and counted (`ws.oversize`).
- **No big allocations in the web path.** A failed `String` allocation aborts the
  station. Stream through `BdJsonOut`, entry by entry.

## GET /api/snapshot

One JSON document, streamed. Works with `curl`.

| Query | Meaning | Default |
|---|---|---|
| `parts` | comma separated: `weather,tracking,hwinfo,ota,console,repeat,stats` | all |
| `min` | how old `weather` and `tracking` entries may be, minutes | 120 |

```json
{"proto":2,"ts":1793475540,
 "weather":[{...}],"tracking":[{...}],"hwinfo":[{...}],
 "ota_history":[{"id":"BD793C","entries":[...]}],
 "console":[{"ts":1793475540,"msg":"..."}],
 "repeat":{"sources":[...],"events":[...]},
 "stats":{"slot":900,"end":0,"rx":[...],"bat":{...}},
 "complete":true}
```

Only a document that ends with `"complete":true` is whole. `503` means the
station is low on memory, try again after `Retry-After`.

`hwinfo` holds the last five packets per station, oldest first. `repeat.events`
holds the last 120 packets, oldest first; an event with a known `seq` and `ts`
updates that packet. Their fields are described in `radio/bd_repeat.cpp`.

## WebSocket /ws

Requests are `{"cmd":...}`:

| Command | Answer |
|---|---|
| `info` | `{"type":"info",...}` |
| `status` | `{"type":"status",...}`, built when it goes out |
| `config` | `{"type":"config",...}`, then `{"update_channels":[...]}` |
| `save_settings`, `save_wifi`, `wifi_scan`, `check_update`, `reboot` | `{"msg":...}` |
| `set_ota_verbose`, `set_flarm_verbose`, `set_adsl_verbose` | the new value |
| `repeat_reset` | `{"repeat_reset":true}` to every client |

### Write protection

With a password set (`core/bd_auth.cpp`), every command that changes something
needs `"auth":"<token>"` of a session: `save_settings`, `save_wifi`,
`wifi_scan`, `check_update`, `reboot`, `repeat_reset`, `set_*_verbose`,
`set_password`. Without it the answer is `{"error":...,"locked":true}`. `config`
leaves out `sta_password` and `ap_password` and says `"authRequired":true`.
Reading stays open. Without a password everything is open as before.

| Command | Answer |
|---|---|
| `login` `{"password":...}` | `{"auth":{"token":...,"expiresIn":3600}}`; after 5 wrong ones logins pause for 60 s |
| `auth_check` | `{"auth":{"required":...,"valid":...,"expiresIn":...}}` |
| `auth_button` | `{"authButton":120}`; a press of the station's button within that time sends the session: `{"auth":{"token":...,"via":"button"}}` |
| `logout` | `{"auth":{"valid":false}}` |
| `set_password` `{"password":...}` | empty removes it; ends all other sessions |

A session lasts one hour from its start and survives a software restart of the
station (RTC memory), not a power cycle. `POST /update` and `POST /reboot` take the
token as `X-Auth`, or the password as `X-Password` (URL-encoded), which is what
the recovery page sends. The password travels unencrypted; the station keeps a
salted, iterated SHA-256 of it.

Sent without being asked:

| Message | When |
|---|---|
| `{"hello":{"proto":2,"fw":"0.8.0"}}` | on connecting |
| `{"resync":true}` | frames for this client were lost: fetch the snapshot |
| `{"weather":[{...}]}`, `{"tracking":[{...}]}`, `{"hwinfo":[{...}]}` | a packet came in |
| `{"repeat":{"event":{...},"source":{...}}}` | a packet was classified |
| `{"ota_progress":{...}}`, `{"ota_history":[...]}` | sensor update running |
| `{"webconsole":...,"ts":...}`, `{"aprsconsole":...}` | a log line |
| `{"wifi_scan":[...]}`, `{"wifi_scan_error":...}`, `{"update_channels":[...]}`, `{"msg":...}`, `{"error":...}` | as they happen |

A client connects to `/ws` first and fetches the snapshot after that, so nothing
falls between the two. Live frames that arrive during the fetch are applied after
it.

## How frames travel

`web/bd_ws_out.cpp` keeps two fixed rings, 4 KB for replies and 16 KB for live
data, with one read position per client. When a ring is full the oldest frame
goes, and a client that had not received it gets `{"resync":true}`.

`status` reports `heap`, `heapMin`, `heapBlock` (largest free block) and
`ws.clients`, `ws.dropped`, `ws.resyncs`, `ws.oversize`, `ws.jobsLost`. Below 16 KB free heap
live packets are dropped and counted.

`tools/stress_web.py` loads a real station with several clients, one of which
stops reading, and fails if anything goes missing or the station restarts.
