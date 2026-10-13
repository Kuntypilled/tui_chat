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

## Planned work, in order

1. Adapter that connects, shows status and lists conversations.
2. Messages: read history, page back, send text, mark read.
3. Pairing wizard.
4. Service selector in the top bar, keyboard cycling, then mouse.
5. Launcher entry, then notifications, then photos.

## Open questions

1. Which services belong in the selector beyond Google Messages.
2. Notification edge cases: sender with no saved name, group chats, messages sent
   from another device, and whether to notify while that chat is open.
