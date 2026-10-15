# tui_chat design notes

Working notes on how tui_chat should feel and how it is built. Written so a person
or a later session can pick the project up without the original conversation.

tui_chat is a personal fork of nchat. It is not affiliated with, endorsed by or
connected to nchat or OmaChat.

## Choices confirmed by the user

1. Keyboard first, with mouse support as well.
2. Reading and replying matter equally, so neither is tuned at the other's expense.
3. A service selector across the top, in the style of OmaChat: one tab per service,
   cycled with a key, with the chats of the selected service listed below it.
4. The helper (`omachatd`) is started the first time tui_chat is launched after each
   boot and stays up until shutdown. Nothing is enabled at login, so no
   notifications arrive on a day the app has not been opened.
5. Notification titles read "New Message - <service> - <first name>" and never
   contain message text.

## How the pieces fit

    tui_chat (nchat UI)
        |  uses the Protocol interface from lib/common/src/protocol.h
        v
    adapter library (new, one directory beside lib/duchat and lib/wmchat)
        |  newline-delimited JSON over a Unix socket
        v
    omachatd (separate program, built from upstream OmaChat, not vendored)
        |
        v
    Google Messages

The adapter only translates between nchat's request and notify messages and the
helper's JSON. All pairing and protocol work stays in the helper.

## Facts that shape the design

Each of these was read in the source or observed by running the program.

1. nchat loads each chat protocol as a shared library at run time. A new adapter is a
   new library enabled by one CMake option.
2. A protocol's setup runs as a plain terminal wizard before the full-screen UI
   starts, so printing a pairing prompt and waiting for the user is straightforward.
3. nchat message times are milliseconds. The helper's times appear to be
   microseconds and need converting.
4. A chat's display name comes from a contact record with the same id as the chat.
5. All services share one flat, sorted chat list. When more than one service is
   configured, each chat shows an " @ <service>" suffix.
6. The top bar is currently only the application name and a status string.
7. nchat has no mouse handling today. Mouse support is new work.
8. The helper starts with every service disabled. Enabling one makes the helper
   exit on purpose after replying, so whatever launches it must start it again.
9. The helper drops a reply if the client closes its sending side, so the adapter
   keeps one long-lived connection.
10. The helper only raises a desktop notification when a client asks it to, and its
    fixed text cannot carry a sender name. tui_chat sends its own.
11. nchat silently skips any profile whose name has no underscore, so the profile id
    is `GoogleMessages_1`.
12. After a failed pairing the helper stays in an error state, with a message and a
    hint, until pairing is tried again.
13. Pairing from the browser tries every Chromium-based browser profile on the
    machine, not one the user picks. It knows only Chrome, Chromium and Brave.
14. ncurses holds up all input for about a second after a mouse event that was not
    requested in the mouse mask, so every button event is requested and only the
    left press and the wheel are acted on.
15. A reverse-video background cannot be switched off for part of a window, so the
    top bar applies its attribute to the text and not to the background.
16. A helper that failed one pairing attempt stays in its error state until asked
    again. A client must forget the state it saw before a new attempt, or the old
    error looks like the new attempt's result. The wizard did exactly that at first.
17. If the pairing wizard quits before showing the emoji, the phone still gets the
    prompt but the person cannot match it, and the helper logs "user cancelled
    pairing on phone".
18. Terminals send the same bytes for Enter and Shift+Enter. ncurses matches a key
    sequence before it turns a return into a linefeed, so Alt+Enter is escape plus
    carriage return in both line modes. Turning on the kitty keyboard protocol would
    separate the two keys, but it also re-encodes every Ctrl and Alt combination,
    which nchat's key bindings depend on.

## Planned work, in order

1. Done: adapter that connects, lists conversations, shows history with paging,
   sends text, marks read, recovers when the helper restarts, and has a pairing
   wizard. Tested against a fake helper and against the real helper up to the point
   of pairing. Not yet tried with a real paired account.
2. Done: service selector in the top bar with keyboard cycling and mouse support,
   covered by the smoke test with the dummy protocol as the second service.
3. WhatsApp through nchat's own support, then the launcher entry, notifications
   and photos.

## Further choices confirmed

1. Services in the selector: Google Messages, plus WhatsApp through nchat's own
   support. WhatsApp runs inside nchat itself, so it only receives and notifies while
   tui_chat is open.
2. Notification title for a sender with no saved name: the number.
3. Notification title in a group chat: the group name.
4. A message arriving in the chat that is open on screen does not notify. Messages
   in any other chat do.
5. Messages sent from another device never notify.
6. Each message sits in a box with hard corners. The sender's name and the time are
   cut into the top border, with a space either side and a dash before them, as the
   chat name is on the border above the prompt in Claude Code. Sent boxes end at the
   right edge of the pane, received ones start at the left. A box is as wide as its
   longest line or its label, up to the same share of the pane as before.
7. Sent text and names use the terminal's own text colour. The other side's name is
   the accent (magenta), and muted elements use a fixed mid grey, because ANSI bright
   black is often close to the background in a theme.
8. The application is called tui_chat in the top bar and `--version`. Its config
   folder keeps the name `~/.config/nchat` so nothing moves. WhatsApp's tab and
   notification name is "WhatsApp".


## Open questions

1. How to pair the Google account, given the helper does not read the user's main
   browser. Chromium has been installed for the purpose.
