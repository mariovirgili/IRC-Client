# Cardputer IRC Client

This project targets the M5Stack Cardputer and implements a compact IRC client with:

- RFC-style IRC registration flow (`PASS`, `NICK`, `USER`)
- Direct TLS support with `WiFiClientSecure`
- Proxy support:
  - SOCKS5
  - HTTP CONNECT
- Generic BNC / ZNC-style `PASS` composition
- CAP negotiation (`CAP LS 302`, `CAP REQ`, `CAP END`)
- IRCv3 support for:
  - `message-tags`
  - `server-time`
  - `sasl` (PLAIN only)
- Channel tabs, query tabs and status tab
- Right-side nick pane for channel tabs
- Per-tab scrollback
- Highlight detection when your nick is mentioned
- Two-row input area for long input buffers
- Persistent tab/session restore on SD
- Per-tab daily logs on SD
- IRC formatting support on screen:
  - bold
  - underline
  - reverse
  - mIRC colors
- Configurable color filtering:
  - `full`
  - `safe`
  - `mono`
- Visible rendering of ASCII control characters when enabled
- Exponential reconnect backoff and ping timeout recovery

## Build

Use PlatformIO with the `m5stack-cardputer` environment.

## SD card layout

Create the following file on the SD card:

`/irc/config.txt`

Example:

```ini
wifi_ssid=YOUR_WIFI
wifi_pass=YOUR_PASSWORD

irc_host=irc.libera.chat
irc_port=6697
irc_use_tls=true
tls_insecure=true

irc_nick=CardADV
irc_user=cardputer
irc_realname=Cardputer IRC
irc_pass=

# Optional auto-join list
#autojoin=#cardputer,#test

# Proxy options: none | socks5 | http
proxy_type=none
proxy_host=
proxy_port=0
proxy_user=
proxy_pass=

# Generic BNC / ZNC-style PASS generation
bnc_enabled=false
bnc_user=
bnc_network=
bnc_pass=

# SASL PLAIN
sasl_enabled=false
sasl_user=
sasl_pass=
sasl_mechanism=PLAIN

# UI / reconnect
nick_pane_enabled=true
color_mode=full
show_control_glyphs=true
persist_tabs=true
reconnect_initial_ms=3000
reconnect_max_ms=60000

log_root=/IRC
```

## Runtime files

The client also stores runtime state in:

`/irc/state.txt`

That file is written automatically when tab state or UI preferences change.

## Commands

- `/join #chan[,#chan2]`
- `/part [#chan] [reason]`
- `/nick newnick`
- `/msg target text`
- `/notice target text`
- `/me action text`
- `/topic [new topic]`
- `/whois nick`
- `/who [mask]`
- `/names [#chan]`
- `/query nick`
- `/close`
- `/users`
- `/nicks`
- `/tabs`
- `/switch N`
- `/switch #chan-or-nick`
- `/next`
- `/prev`
- `/scroll up [n]`
- `/scroll down [n]`
- `/scroll pageup [n]`
- `/scroll pagedown [n]`
- `/scroll top`
- `/scroll bottom`
- `/nicklist [on|off]`
- `/away [reason]`
- `/list [mask]`
- `/colormode full|safe|mono`
- `/quote RAW IRC LINE`
- `/raw RAW IRC LINE`
- `/config`
- `/reconnect`
- `/quit`

Typing plain text sends a `PRIVMSG` to the active channel or query tab.

## Logging

Logs are stored as:

`/IRC/<server>/<tab-folder>/YYYYMMDD.log`

Examples:

- `/IRC/irc.libera.chat/status/20260324.log`
- `/IRC/irc.libera.chat/#cardputer/20260324.log`
- `/IRC/irc.libera.chat/query_someNick/20260324.log`

Channel tabs keep their channel-style folder names when valid, while private queries are stored with a `query_` prefix to avoid ambiguity with channels and the status tab.

The log timestamp uses IRCv3 `server-time` when the server provides it, otherwise the local clock is used.

## Notes and limits

- TLS through proxy is still **not** implemented in this build.
- SASL is implemented for the `PLAIN` mechanism only.
- The two-row input area improves local editing of long lines, but outbound messages are still sent as standard single-line IRC messages.
- The color filter only affects on-screen rendering; the log file always stores plain text without IRC formatting bytes.


## Config page controls

Press `G0 / BtnA` to open or close the config page.

Inside the page:

- `TAB` = next field
- `DEL` = previous field
- `ENTER` = edit / toggle / activate
- `j` or `.` = next field
- `k` or `,` = previous field
- Select `Save+Reconnect` to write `/irc/config.txt` and restart the network session
- Select `Exit/Discard` to leave without saving

Text fields are edited inline from the Cardputer keyboard.
