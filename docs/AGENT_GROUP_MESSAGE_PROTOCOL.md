# Agent Group Message Protocol (Draft v1)

## Problem

Group messages are sent to agents from the **group Carrier userid/address**, so without extra metadata an agent cannot reliably know:

- who originally sent the message in the group
- which group context to reply to when the same agent is attached to multiple groups

## Goals

- Keep group-side friend relationship model (group service adds/removes agent friend).
- Preserve original group sender metadata for agent consumers.
- Keep reply routing simple: agent replies to group service userid, group service fans out to group members.
- Support multiple agents per group.
- Be backward-compatible with plain-text agent replies.

## Outbound (Group Service -> Agent)

For recipients registered in `agent_table`, group service sends an envelope:

- Prefix: `CGP1 `
- Payload: JSON

```json
{
  "type": "carrier_group_message",
  "version": 1,
  "chat_type": "group",
  "source": "carrier_group_service",
  "group": {
    "userid": "<group_userid>",
    "address": "<group_carrier_address>",
    "nickname": "<group_nickname>"
  },
  "origin": {
    "userid": "<original_sender_userid>",
    "friendid": "<original_sender_userid>",
    "nickname": "<original_sender_nickname>",
    "user_info": {
      "userid": "<origin_userid>",
      "name": "<origin_name>",
      "description": "<origin_description>",
      "has_avatar": 0,
      "gender": "",
      "phone": "",
      "email": "",
      "region": ""
    },
    "friend_info": {
      "label": "<group_local_label_for_origin>",
      "status": 0,
      "status_text": "connected",
      "presence": 0,
      "presence_text": "none"
    }
  },
  "message": {
    "text": "<original_text>",
    "timestamp": 1739972765
  },
  "render": {
    "plain": "<nickname>: <text> [yyyy-mm-dd hh:mm:ss]"
  }
}
```

Non-agent recipients continue receiving plain text.

When the group message is a file (`[u32 metaLen][JSON][bytes]`, see
`GROUP_FILE_MESSAGE.md`), the agent gets no bytes: `message.text` is
`[file] <filename> (<n> bytes)` and `message.file` is
`{"filename", "contentType", "size"}`. Human members get the file message itself.

Notes:

- `origin.user_info` and `origin.friend_info` come from Carrier `ElaFriendInfo` when available.
- If Carrier runtime cannot load `ElaFriendInfo`, payload still includes minimal `origin.user_info.userid` and `origin.user_info.name`.
- `origin.user_info` / `origin.friend_info` are resolved at relay time (current profile/status), not a historical snapshot at sender-post time.

## Group vs DM Detection (for sidecar/channel)

Treat an incoming Carrier friend message as **group-forwarded** only when all checks pass:

1. Text starts with `CGP1 ` prefix.
2. JSON parses successfully.
3. `type == "carrier_group_message"`.
4. `chat_type == "group"` and `source == "carrier_group_service"` (recommended strict check).
5. `group.address` and `origin.userid` are present.

If any check fails, treat as normal DM/plain message.

### Mobile client fallback classification (when message is not `CGP1`)

Carrier IDs are base58-style IDs and are not guaranteed to have a stable prefix (for example, do not assume `G...` means group).

Recommended order:

1. If payload is valid `CGP1`/`CGR1`/`CGS1` with `chat_type=group`, classify as group.
2. Else if payload is valid `BGS1` with `chat_type=direct`, classify as DM.
3. Else if chat peer id is in local known-group identity set, classify as group.
4. Else classify as DM.

Known-group identity set should include both:

- `group.userid` (group service friend userid)
- `group.address` (group carrier address)

The set is populated from trusted sources such as group list/create/join APIs and prior valid `CGP1/CGR1/CGS1` payloads.

Do not classify by string heuristics such as length/prefix rules.

### Field ownership (do not overload `friend_info`)

- Group identity fields: `group.userid`, `group.address`
- Group sender identity field: `origin.userid`
- `origin.friend_info` and `origin.user_info` are sender profile/presence metadata only.

Do not repurpose unused `friend_info` fields to carry group address or chat-type hints; clients should read group identity from `group.*` and sender identity from `origin.*`.

Trust model:

- Without a known/allowed group sender identity list, this classification is format-based only (can be forged by any DM sender).
- For strong trust, keep an allowlist keyed by group sender userid/address, or add signed envelopes in a future protocol revision.

## Inbound (Agent -> Group Service)

Agent may send:

- Plain text (backward compatible), or
- Envelope reply:
  - Prefix: `CGR1 `
  - Payload JSON with:
    - `type = "carrier_group_reply"`
    - optional `group.address`
    - `message.text`
    - optional `reply_to.userid`

Group service behavior:

- If `CGR1` and `group.address` is present and mismatched, ignore structured fields and fallback to plain body.
- Extract `message.text` and store/relay it as normal group message.
- If `reply_to.userid` is valid member, prepend `@<nickname> ` to text for human visibility.
- Parser compatibility accepted by group service:
  - `type = carrier_group_reply` or `carriergroupreply`
  - `chat_type` or `chattype`
  - `reply_to.userid` / `reply_to.userId` / `reply_to.friendid`
  - stray ASCII control bytes in JSON payload are sanitized before parse fallback.
- Human group members continue receiving plain text relay, not raw `CGR1` JSON.

## Agent Lifecycle

- Add: `/agent add <address>` or HTTP `/agent/add`
- List: `/agent list` or HTTP `/agent/list`
- Remove: `/agent del <userid>` or HTTP `/agent/remove`

`agent_table(UserId, Address)` tracks which friends are designated as agents.

## Component Responsibilities

- **CarrierGroup service**
  - Build `CGP1` envelope for agent recipients.
  - Parse optional `CGR1` replies.
  - Persist agent role metadata in `agent_table`.
- **beagle-sidecar / beagle-channel**
  - Parse `CGP1` payload and map to channel-group model.
  - When relaying agent response back, send `CGR1` payload (or plain text fallback).
- **OpenClaw agent/channel**
  - Use `origin.userid` + `group.address` for thread-safe group context.
  - Reply to group service friend (same Carrier peer), not a direct member.

## Rollout

1. Deploy CarrierGroup service with `CGP1/CGR1` support.
2. Update beagle-sidecar/beagle-channel parser + formatter.
3. Validate:
   - sender attribution is preserved
   - replies fan out to group correctly
   - multiple groups sharing one agent remain isolated by `group.address`
