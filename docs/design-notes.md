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
    machine, not one the user picks.

## Planned work, in order

1. Done: adapter that connects, lists conversations, shows history with paging,
   sends text, marks read, recovers when the helper restarts, and has a pairing
   wizard. Tested against a fake helper and against the real helper up to the point
   of pairing. Not yet tried with a real paired account.
2. Service selector in the top bar, keyboard cycling, then mouse.
3. Launcher entry, then notifications, then photos.

## Further choices confirmed

1. Services in the selector: Google Messages, plus WhatsApp and/or Telegram through
   nchat's own support. Which of the two is still to be confirmed. These run inside
   nchat itself, so they only receive and notify while tui_chat is open.
2. Notification title for a sender with no saved name: the number.
3. Notification title in a group chat: the group name.
4. A message arriving in the chat that is open on screen does not notify. Messages
   in any other chat do.
5. Messages sent from another device never notify.

## Open questions

1. Whether WhatsApp, Telegram or both go in the selector.
2. Where the service selector's cycling keys should sit. To be chosen by testing
   which keys the terminal actually delivers, and rebindable.
