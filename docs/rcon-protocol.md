# RCON protocol

Overseer speaks the [Source RCON protocol](https://developer.valvesoftware.com/wiki/Source_RCON_Protocol),
so existing tools work unchanged.

## Packet

All integers are little-endian `int32`.

| Field | Notes |
| --- | --- |
| `size` | Bytes that follow: `4 + 4 + body + 2`. Accepted range 10–4096. |
| `id` | Chosen by the client, echoed in responses. |
| `type` | `3` AUTH, `2` EXECCOMMAND / AUTH_RESPONSE, `0` RESPONSE_VALUE |
| `body` | UTF-8 (Source uses ASCII; UTF-8 is a superset) followed by `0x00` |
| pad | One more `0x00` |

## Flow

1. Client sends `AUTH` with the password.
2. On success the server sends an empty `RESPONSE_VALUE`, then `AUTH_RESPONSE`
   with the client's id. On failure, `AUTH_RESPONSE` with id `-1`. After 3
   failures (configurable) the connection is closed.
3. Client sends `EXECCOMMAND` packets. Sending a command before authenticating
   closes the connection.
4. Responses longer than 4086 bytes are split across several `RESPONSE_VALUE`
   packets with the same id.

### Detecting the end of a multi-packet response

After a command, send an empty `RESPONSE_VALUE` with a different id. Overseer
echoes it back once the command's response is fully written, so everything
between is the response. `RconClient` does this for you.

## Commands

| Command | Meaning |
| --- | --- |
| `#<anything>` | Native SCUM admin command, passed through to the game (e.g. `#ListPlayers`, `#Announce hi`) |
| `overseer.help` | List Overseer and plugin commands |
| `overseer.version` | Version and engine bridge in use |
| `overseer.plugins` | Loaded plugins |
| `overseer.reload` | Reload all plugins |
| `<plugin command>` | Anything registered by a plugin |

## Security

The protocol is plaintext. Overseer binds to `127.0.0.1` by default; expose it
only behind a firewall allow-list, VPN or SSH tunnel. Idle connections are
closed after 10 minutes and concurrent connections are capped (default 8).
