# Cardputer IRC Client

This project targets the M5Stack Cardputer and implements a compact IRC client with:

Current release: `v0.2`

## New in v0.2

- Graphical battery indicator centered in the top bar
- Screen timeout after inactivity with automatic wake on input
- Configurable screen timeout directly from the on-device config menu
- Configurable screen brightness from `0` to `10` in the config menu
- Chat scroll shortcuts with `Fn + ; . , /` to read older messages while new ones arrive

- RFC-style IRC registration flow (`PASS`, `NICK`, `USER`)
- Direct TLS support with `WiFiClientSecure`
- Built-in IRC network presets with persistent selection
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
- Centered graphical battery indicator in the header
- Idle screen timeout with automatic wake on activity
- Configurable screen brightness from `0` to `10`
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
- Selectable chat overflow behavior: horizontal marquee scroll or wrapped multi-line rendering

## Build

Use PlatformIO with the `m5stack-stamps3` environment.

## SD card layout

Create the following file on the SD card:

`/irc/config.txt`

Example:

```ini
wifi_ssid=YOUR_WIFI
wifi_pass=YOUR_PASSWORD

irc_server_preset=libera
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
chat_overflow_mode=scroll
persist_tabs=true
screen_timeout_sec=10
screen_brightness=10
reconnect_initial_ms=3000
reconnect_max_ms=60000

log_root=/IRC
```

If `wifi_ssid` is left as `YOUR_WIFI`, the device opens the on-device config page at boot instead of attempting a connection.

Available `irc_server_preset` values:

- `libera`
- `oftc`
- `efnet`
- `ircnet`
- `dalnet`
- `undernet`
- `quakenet`
- `custom`

When a preset other than `custom` is selected, the client syncs `irc_host`, `irc_port`, and `irc_use_tls` to that network's recommended defaults.

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
- `/chatmode scroll|wrap`
- `/quote RAW IRC LINE`
- `/raw RAW IRC LINE`
- `/config`
- `/reconnect`
- `/quit`

Typing plain text sends a `PRIVMSG` to the active channel or query tab.

## Controls

- Press `` ` `` to open the server channel list and press it again to close it.
- In config and channel-list pages, use `;` for up and `.` for down.
- In chat tabs, hold `Fn` and use `;` / `.` for line-by-line scroll and `,` / `/` for page scroll. In `wrap` mode these move by visible wrapped rows.
- Press `Enter` on a channel-list item to join it immediately.

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
- `chat_overflow_mode=scroll` preserves the original one-line marquee behavior; `wrap` uses multi-line rendering for long chat messages.
- The color filter only affects on-screen rendering; the log file always stores plain text without IRC formatting bytes.
- `screen_timeout_sec=0` disables automatic screen sleep.
- `screen_brightness` accepts values from `0` (off) to `10` (max).


## Config page controls

Press `G0 / BtnA` to open or close the config page.

Inside the page:

- `TAB` = next field
- `DEL` = previous field
- `ENTER` = edit / toggle / activate
- `.` = next field
- `;` = previous field
- Select `Save+Reconnect` to write `/irc/config.txt` and restart the network session
- Select `Exit/Discard` to leave without saving

Text fields are edited inline from the Cardputer keyboard.
