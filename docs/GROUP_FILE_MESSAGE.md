# File messages in a group (voice notes, images, small files)

Status: group service side shipped 2026-11-03 (branch `feat/agentnet-proto-v2`).
Clients still have to send and render it.

## What a member sends

The same single friend message a client sends to an offline friend 1:1
(iOS `docs/OFFLINE-FILE-MESSAGE.md`, Android `CarrierFileHelper.packFilePayload`,
JS `compat/inline-file.ts`), addressed to the group's userid:

```
[u32 big-endian metaLen][meta: UTF-8 JSON][raw file bytes]
meta = {"type":"file","filename":"voice-<ms>-<durMs>.m4a","contentType":"audio/mp4","size":N}
```

- whole message <= 5 242 880 bytes (`CARRIER_LEGACY_MAX_APP_BULKMSG_LEN`); meta <= 4096.
- **Keep it <= 5 242 880 - 1024 in a group.** The service adds about 300 bytes
  of sender fields to the meta; if that would push the message over 5 MiB, the file
  is still relayed but without them (see below).

## What the service does with it

1. Recognises it by the reader rules (two leading zero bytes, metaLen 1..4096,
   JSON object with `"type":"file"`, declared `size` fits). It is never treated as
   text or a `/command`, and trailing `0x00` bytes are kept.
2. Drops it, logged, if it claims to be a file but is truncated or over 5 MiB.
3. Stores it byte for byte (SQLite BLOB) and relays it to every member: right away
   to the online ones, and to each offline member when it comes back online, the
   way text has always been replayed.
4. Rewrites only the meta. The file bytes are untouched.

A DNFT1 frame (`0x1E "DNFT1"`, the 1:1 streamed-transfer handshake) sent to a group
is dropped. Relayed, it only ever became junk text for every member (2026-10-09).

## Offline copies (Express)

A member whose link to the group is down does not lose the message: its core
parks it on an Express node (`lens.beagle.chat` first, then
`express.beagle.chat`). Since 2026-11-03 the group pulls and deletes from both
nodes (native SDK `e8eb0b0`, `CarrierConfig.cpp`) and handles a pulled message
like a live one, text and file alike, with two exceptions:

- **Duplicates.** The core re-sends a live message through Express when no
  receipt comes back in 30 s, so the same message can arrive twice. An Express
  copy is dropped when the same sender's identical bytes were stored from 10 min
  before it was posted (24 h back when the node gives no post time). A real
  repeat of the same text that was parked in that window is dropped too; that is
  the trade-off. The same check absorbs re-deliveries: on 2026-11-03 the nodes
  handed the same parked messages out on every pull (the core's per-node
  watermark stayed below them, so nothing was deleted either); group 1 saw its
  30 backlog messages about every 27 s. Backlog rows are kept once per
  (sender, post time, length).
- **Backlog.** Until 2026-11-03 the group never pulled Express, so up to 14 days
  of parked messages were waiting. Anything posted more than an hour before the
  group first pulled (`service_meta.express_since_us`) goes to
  `express_backlog_table` in the group's DB and is not relayed or run as a
  command. Relaying that backlog later, if wanted, is a separate decision.

The group's own sends get the same fallback: a relay that is not confirmed is
parked on Express for that member by the core.

## What a member receives

```
[u32 BE metaLen'][meta': sender's meta + the keys below][same file bytes]
```

```json
{
  "type": "file", "filename": "voice-1793749000000-12000.m4a",
  "contentType": "audio/mp4", "size": 174080,
  "chat_type": "group",
  "group":  {"userid": "<group userid>", "address": "<group address>", "nickname": "<group name>"},
  "origin": {"userid": "<sender userid>", "nickname": "<sender name in this group>"},
  "sent_at_ms": 1793750296008
}
```

- The message comes from the group's userid, like every group message. The real
  sender is `origin.userid`; `sent_at_ms` is when the service received it.
- Whatever the sender put under `chat_type`, `group`, `origin`, `sent_at_ms` is
  replaced, so a member cannot pose as another member.
- When the sender fields do not fit (meta' > 4096 or the message would exceed
  5 MiB), meta' is the sender's meta with those four keys removed: the file
  arrives, the sender is unknown. Show it as from the group.
- Key order is not fixed; readers must not depend on it. Existing 1:1 readers
  ignore the extra keys, so a client that only knows 1:1 offline files still
  stores and plays the file, in the group conversation.
- Dedup by SHA-256 of the file bytes, as 1:1 does: a replay after reconnect is
  the same file.

Agents (OpenClaw, beagle desktop, `CGP1`) keep the group envelope, so they still
know the group and the real sender. It carries
`message.text = "[file] <filename> (<n> bytes)"` and
`message.file = {filename, contentType, size, data_base64}`. Since 2026-11-05
`data_base64` holds the file bytes (RFC 4648, padded) whenever the whole `CGP1 `
message stays within 5 MiB (about a 3.7 MiB file). A bigger file arrives
without `data_base64`: description only. Agents that do not read `data_base64`
are unaffected. Agents are not sent the binary relay, because an agent such as an
OpenClaw sidecar would take it for a direct message from the group's userid and
lose the group context.

## Text and inline images are unchanged

Text still arrives as `<nickname>: <text> \n[<date>]`, and the 6 KB inline JSON
image (`FileModel`, base64) is text. Fan-out is now binary safe end to end: the
service sends exactly the stored bytes and no longer cuts a message at its first
`0x00`.

## Measured (snoopy, 2026-11-03, `linux/tools/cg_e2e.c`)

Old service (b514883), through a throwaway group, member to member:

| sent | arrived |
|---|---|
| text 100 B .. 5 242 881 B | intact, plus the 29-byte `e2e-B: ` / ` \n[date]` wrap |
| `hello\0world\0tail` | `hello` (cut at the first NUL, in SQLite) |
| DNFT1 frame | `\x1eDNFT1P` as text |
| 170 KB voice / 5 MiB file / any payload starting `00 00` | empty text (`e2e-B:  \n[date]`, 29 bytes) |

New service: text identical; NUL text whole; DNFT1 dropped; 170 KB voice and a
5 242 776-byte file arrive byte-exact (file body SHA-256 equal) at an online
member and at a member that was offline when it was sent.
