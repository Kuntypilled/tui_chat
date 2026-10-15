# How tui_chat works

A plain-language guide for anyone picking this project up. It has no personal
details on purpose. For the reasoning behind choices see `design-notes.md`.

tui_chat is a personal fork of nchat, a terminal chat client. It adds Google
Messages. It is not affiliated with nchat, OmaChat or Google.

## The three pieces

1. **nchat (the screen you look at).** The terminal interface: chat list, message
   history, typing box, key bindings. Almost all of it is unchanged upstream code.
2. **The adapter (`lib/gmchat`).** A small library that nchat loads when it starts.
   It turns "show me the chats" and "send this text" into messages for the helper,
   and turns the helper's answers back into things nchat understands. It does no
   Google work itself.
3. **The helper (`omachatd`).** A separate program from the OmaChat project. It
   does the real work: signing in to Google, pairing with your phone, fetching and
   sending messages. It listens on a socket file that only your user can use.

```
you  ->  nchat screen  ->  adapter  ->  helper  ->  Google Messages
```

## First run

1. Build the helper once with `scripts/build-helper.sh`. It downloads OmaChat at a
   pinned version, builds it, and puts `omachatd` in
   `~/.local/share/tui_chat/`. It needs Go 1.27.0 or newer and a C compiler.
2. Build tui_chat (see "Building" below).
3. Run `nchat -s`, choose GoogleMessages, and follow the prompts. The wizard
   explains each step before it does it, and asks before touching anything.
4. After that, run `nchat`.

## Launcher

`scripts/install-launcher.sh` adds two files, and nothing else:

- `~/.local/bin/tui_chat`, a one-line command that runs the built `nchat`.
- `~/.local/share/applications/tui_chat.desktop`, an entry that opens tui_chat in a
  kitty window of its own (window class `tui_chat`), so it shows up in an
  application launcher.

Run it with `--uninstall` to remove both. It never overwrites a file it did not
write, and it does not enable anything at login. Set `TUI_CHAT_BIN` if the binary is
not at `build/bin/nchat`.

## What starts when

- Nothing starts at login.
- The first time you launch tui_chat after a boot, it starts the helper in the
  background. The helper then stays up until you shut down or log out, so later
  launches are quick.
- If the helper is already running, tui_chat just connects to it.
- If the helper stops while tui_chat is open, tui_chat starts it again.

## Notifications

tui_chat raises a desktop notification with `notify-send` for a message that arrives
while it is open, in a chat that is not the one on screen (or any chat, when the
terminal is not focused). Nothing is raised at login or while tui_chat is closed.

- The title is `New Message - <service> - <first name>`, the number for a sender who
  is not saved, or the group's name for a group. It never contains message text.
- Messages you send from another device never notify, and muted chats stay quiet.
- Each message notifies once.
- Settings are in `ui.conf`: `desktop_notify_enabled` (on), `desktop_notify_command`
  (empty means `notify-send -a tui_chat '%1' '%2'`, with the title as `%1`),
  `desktop_notify_connectivity` (off), `desktop_notify_active_current`,
  `desktop_notify_active_noncurrent` and `desktop_notify_inactive`.

## Photos

In kitty, or another terminal that speaks the kitty graphics protocol, photos are
drawn inside their message box, up to 40 columns and 14 rows. They are fetched when
they come on screen, unless `attachment_prefetch` is `0`.

- A background thread makes a thumbnail with ImageMagick (`magick` must be on the
  path), at most 640 pixels, forcing the decoder from the file's extension so a file's
  content cannot pick a different format. The thumbnails are kept in
  `~/.config/nchat/thumbs/` and removed after a month.
- The photo is handed to the terminal by file path and drawn as ordinary text cells
  (kitty's Unicode placeholders), so it scrolls and clips with the history. Until a
  thumbnail is ready, and in a terminal without graphics, the file name line stays.
- Google Messages photos are fetched through the helper's `media` call. Only the
  first photo of a message is shown inline, others stay as `[Photo]` in the text.
- Settings: `inline_images` in `ui.conf` (on). Not used inside tmux.

## What is stored, and where

- The helper keeps your message content and its own login under
  `~/.local/share/omachat` and `~/.cache/omachat`. Those folders are private to
  your user.
- nchat's own message cache is not used for Google Messages. tui_chat's config
  folder holds settings and the profile, not message text.
- The helper writes warnings to `helper.log` in the profile folder.

## Pairing, in plain words

Google no longer shows a scannable code in newer versions of Messages. So the
helper signs in as your Google account instead, and then your phone shows an emoji
that you confirm.

To sign in, the helper reads your Google sign-in cookies from a Chromium-based
browser (Chrome, Chromium or Brave) on this machine. It looks through every profile
of those browsers and uses the first one that is signed in to Google Messages. The
cookies are encrypted on disk, so the helper asks your desktop keyring for the
browser's key through `secret-tool`. The cookies are read and used by the helper.
tui_chat itself never sees them.

Before pairing, open `messages.google.com/web` in the browser profile you want to
use and let it load.

Risks, stated plainly: the helper uses unofficial client libraries and may stop
working when Google changes things. What Google does about accounts that use them
has not been measured. Try a secondary account first.

## Services across the top, and the mouse

The top bar has one tab per chat service, in alphabetical order. The tab for the
service you are in is shown as a lit block. A number after a service name is how
many of its chats have unread messages. The chat list below shows only the chats of
the selected service.

| to do this | keyboard | mouse |
|---|---|---|
| next or previous service | Ctrl-PageDown, Ctrl-PageUp | click a tab, or the wheel over the top bar |
| next or previous chat | Tab, Shift-Tab | click a chat, or the wheel over the list |
| scroll history | PageUp, PageDown | the wheel over the messages |
| next unread chat | Ctrl-F | none |

Things that behave in a way worth knowing:

- Switching services returns you to the chat you last had open there.
- Ctrl-F looks for an unread chat in the current service first, and only then in the
  others, switching service when it finds one there.
- Opening a chat from the "go to chat" dialog switches to that chat's service.
- A service with no chats yet shows an empty list.

The mouse is on by default. While it is on, selecting text to copy needs Shift held
down in most terminals. Set `mouse_enabled=0` in `ui.conf` to turn it off. The two
service keys can be rebound in `key.conf` as `next_service` and `prev_service`, and
the look of the selected tab is `top_attr_selected` in `color.conf`.

## Writing, and how messages are laid out

- **Enter sends.** Alt+Enter adds a line break. Terminals send the very same bytes
  for Enter and Shift+Enter, so an application cannot tell them apart. Kitty can be
  told to send Alt+Enter for Shift+Enter, only while nchat is the program in the
  window, with one line in `kitty.conf`:

  ```
  map --when-focus-on cmdline:nchat shift+enter send_text all \x1b\r
  ```

  Without that line Shift+Enter just sends, like Enter. Both keys can be rebound as
  `send_msg` and `linebreak` in `key.conf`.
- **Sides.** Messages you sent sit against the right edge of the message pane, and
  received ones against the left. Neither is drawn wider than a share of the pane, so
  the two sides stay visibly apart. `history_sent_align_right=0` puts everything back
  on the left, and `history_max_width_percent` (default 75) sets the share, with 100
  using the whole width.
- **Chat list width.** The name column is 20 columns wide by default. Alt+, and Alt+.
  make it narrower or wider while running, and `list_width` in `ui.conf` is where
  the choice is kept.

## Settings

Each profile has `gmchat.conf` in its folder:

| key | meaning |
|---|---|
| `profile_display_name` | name shown for the service, default "Google Messages" |
| `helper_path` | full path to `omachatd`, empty means search for it |
| `socket_path` | helper socket, empty means the helper's own default |
| `start_helper` | `1` to start the helper when it is not running |

The helper is searched for in this order: `helper_path`, the `TUI_CHAT_HELPER`
environment variable, `~/.local/share/tui_chat/omachatd`, then `PATH`.

## Building

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DHAS_TELEGRAM=OFF -DHAS_WHATSAPP=OFF -DHAS_SIGNAL=OFF
cmake --build build -j8
```

The Telegram and WhatsApp switches only exist to make builds shorter. Turn them on
for those services. Telegram's build is the heaviest and can run out of memory on a
small machine.

For the service selector with Google Messages and WhatsApp, build with
`-DHAS_WHATSAPP=ON` (the WhatsApp library is about 45 MB and compiles Go on first
build, which downloads modules).

## Adding WhatsApp

WhatsApp runs inside nchat itself, with no helper, so it only receives messages and
notifies while tui_chat is open.

1. Run `nchat -s` in a real terminal (not through a tool that has no stdin).
2. Choose WhatsApp. It asks for your own phone number only to name the profile.
3. A QR code is drawn in the terminal. On the phone: WhatsApp, Settings, Linked
   devices, Link a device, then scan it.
4. After that, `nchat` shows WhatsApp as a second tab beside Google Messages.

## Testing without an account

`tests/gmchat/smoke.sh <path-to-nchat>` runs the adapter against a fake helper with
made-up chats. It checks the setup wizard, listing, sending, a live incoming
message, recovery after the helper is killed, and that nchat writes no message text
to disk. It uses a throwaway config folder and never touches a real account.

## How a message travels

1. You press send. nchat asks the adapter to send text to a chat.
2. The adapter sends `send` to the helper over the socket and waits for the answer.
3. The helper sends it to Google and answers with a provisional message.
4. The adapter shows that message right away.
5. Moments later the helper pushes the final copy with its real id. The adapter
   swaps the provisional copy for the final one, so you see it once.

Incoming messages arrive the other way: the helper pushes an event, the adapter
converts it, and nchat shows it.

## Known limits

- SMS and MMS stop at "sent". Google reports "read" only over RCS.
- Photos and other attachments show as a label such as `[Photo]` for now.
- Reactions, voice notes and calls are not supported.
- One Google account per helper.
