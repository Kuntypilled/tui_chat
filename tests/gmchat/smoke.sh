#!/usr/bin/env bash
# Smoke test for the Google Messages adapter against a fake helper.
#
# Uses fictional chats only, a throwaway config directory, and a private runtime
# directory. It never touches a real account or the real helper's data.
#
# usage: tests/gmchat/smoke.sh <path-to-nchat-binary>

set -u

nchat="${1:?usage: smoke.sh <path-to-nchat-binary>}"
here="$(cd "$(dirname "$0")" && pwd)"
work="$(mktemp -d)"
# unix socket paths are limited to about 100 bytes, so keep the runtime dir short
runtime="/run/user/$(id -u)/tui-chat-smoke"
rm -rf "$runtime"
mkdir -p "$runtime" "$work/conf"
chmod 700 "$runtime"

export XDG_RUNTIME_DIR="$runtime"
export FAKE_STATE_DIR="$work/fake"
export TUI_CHAT_HELPER="$here/fake_omachatd.py"

cleanup() {
  [ -f "$work/fake/pid" ] && kill "$(cat "$work/fake/pid")" 2>/dev/null
  rm -rf "$runtime" "$work"
}
trap cleanup EXIT

fail=0

# Each driver run is a fresh process, so state that must carry over (the paired
# helper and the config directory) lives in $work.
# A first attempt that fails leaves the helper in its error state, which stays
# until pairing is tried again. The second attempt must not mistake that old state
# for its own result.
mkdir -p "$work/fake"
touch "$work/fake/fail_next"
echo "== pairing wizard, first attempt fails with the helper's own hint"
out="$(python3 -I "$here/drive.py" 110 36 \
  "wait:1;expect:Select protocol;send:1\r;expect:Continue;send:y\r;expect:Choose;send:\r;expect:Let the helper read;send:y\r;expect:did not get as far;expect:Not signed in to Google Messages;expect:Open messages.google.com/web;wait:0.5" \
  -- "$nchat" -s -d "$work/conf")"
echo "$out" | grep -E '^(PASS|FAIL)'
echo "$out" | grep -q '^FAIL' && fail=1

echo "== pairing wizard, second attempt works"
out="$(python3 -I "$here/drive.py" 110 36 \
  "wait:1;expect:Select protocol;send:1\r;expect:Continue;send:y\r;expect:Choose;send:\r;expect:Let the helper read;send:y\r;expect:confirm this;expect:Alex Rivera;wait:1" \
  -- "$nchat" -s -d "$work/conf")"
echo "$out" | grep -E '^(PASS|FAIL)'
echo "$out" | grep -q '^FAIL' && fail=1

# the newest chat (the unknown number) is shown first, so messages go there
echo "== list, send with Enter, line break with Alt+Enter, live message, sides"
# Enter sends. Alt+Enter, which is what a terminal sends for Shift+Enter with a one line
# mapping, adds a line break and must not send. Sent messages sit on the right of the
# message pane and received ones on the left.
out="$(python3 -I "$here/drive.py" 110 36 \
  "wait:2;expect:Alex Rivera;expect:Book club;send:hello from smoke;send:\r;expect:hello from smoke;wait:2.5;count:hello from smoke;rightof:60:hello from smoke;send:first of two;send:\x1b\r;send:second of two;wait:1.5;send:\r;expect:second of two;run:printf 'c_num|incoming smoke message|u_x\n' >> $work/fake/inject;expect:incoming smoke message;leftof:40:incoming smoke message;count:incoming smoke message" \
  -- "$nchat" -d "$work/conf")"
echo "$out" | grep -E '^(PASS|FAIL|COUNT)'
echo "$out" | grep -q '^FAIL' && fail=1
# a sent message must be shown once, not once as provisional and once as final
echo "$out" | grep -E '^COUNT [^1]' && fail=1
sends="$(grep -c '^send ' "$work/fake/requests.log")"
if [ "$sends" = "2" ]; then
  echo "PASS exactly two messages were sent, so Alt+Enter did not send"
else
  echo "FAIL expected 2 sends, the helper saw $sends"
  fail=1
fi

# Each message sits in a box with hard corners. The name and time are cut into the top
# border with a space either side, and sent boxes end at the right edge of the pane.
echo "== messages in boxes with the label on the top border"
out="$(python3 -I "$here/drive.py" 110 36 \
  "wait:2;expect:Alex Rivera;expect:hello from smoke;expect:incoming smoke message;snap:boxes" \
  -- "$nchat" -d "$work/conf")"
echo "$out" | sed -n '/^---- screen: boxes/,/^----$/p'
check_boxes() {
  local what="$1" pattern="$2"
  if echo "$out" | grep -Eq -- "$pattern"; then
    echo "PASS $what"
  else
    echo "FAIL $what"
    fail=1
  fi
}
check_boxes "top border with the label inline" '┌─ [^─│┌┐└┘]+ ─+┐'
check_boxes "bottom border" '└─+┘'
check_boxes "text between side borders" '│ hello from smoke +│'
if echo "$out" | grep -q '[╭╮╯╰]'; then
  echo "FAIL rounded corners found"
  fail=1
else
  echo "PASS no rounded corners"
fi

# In a narrow window the name and time do not both fit on the top border. The name
# gives way with an ellipsis and the time stays, and the box still closes.
echo "== narrow window shortens the name and keeps the time"
out="$(python3 -I "$here/drive.py" 40 24 \
  "wait:2;expect:Alex Rivera;expect:incoming smoke;snap:narrow" \
  -- "$nchat" -d "$work/conf")"
echo "$out" | sed -n '/^---- screen: narrow/,/^----$/p' | sed 's/ *$//'
check_boxes "name shortened with an ellipsis, time kept, border closed" '┌─ [^ ─]+… \([0-9:]+\) ─*┐'

# A message in a chat that is not open raises a notification titled
# "New Message - <service> - <first name>", or the group's name for a group, and never
# carries message text. A message in the open chat stays quiet.
echo "== notifications"
notify_log="$work/notify.log"
rm -f "$notify_log"
sed -i "s|^desktop_notify_command=.*|desktop_notify_command=printf '%s\\\\n' '%1 / %2' >> $notify_log|" "$work/conf/ui.conf"
sed -i 's|^desktop_notify_enabled=.*|desktop_notify_enabled=1|' "$work/conf/ui.conf"
out="$(python3 -I "$here/drive.py" 110 24 \
  "wait:2;expect:Alex Rivera;run:printf 'c_alex|secret text one|u_alex\n' >> $work/fake/inject;wait:1.5;run:printf 'c_club|secret text two|u_sam\n' >> $work/fake/inject;wait:1.5;run:printf 'c_num|secret text three|u_x\n' >> $work/fake/inject;expect:secret text three;wait:1.5" \
  -- "$nchat" -d "$work/conf")"
echo "$out" | grep -E '^(PASS|FAIL)'
echo "$out" | grep -q '^FAIL' && fail=1
check_notify() {
  local what="$1" pattern="$2"
  if grep -Fxq -- "$pattern" "$notify_log" 2>/dev/null; then
    echo "PASS $what"
  else
    echo "FAIL $what (log: $(tr '\n' '|' < "$notify_log" 2>/dev/null))"
    fail=1
  fi
}
check_notify "person: service and first name" "New Message - Google Messages - Alex / "
check_notify "group: the group's name" "New Message - Google Messages - Book club / "
if grep -q "secret" "$notify_log" 2>/dev/null; then
  echo "FAIL message text reached a notification"
  fail=1
else
  echo "PASS no message text in any notification"
fi
if [ "$(wc -l < "$notify_log" 2>/dev/null)" = "2" ]; then
  echo "PASS the open chat stayed quiet"
else
  echo "FAIL expected exactly 2 notifications"
  fail=1
fi

echo "== helper restart is survived"
out="$(python3 -I "$here/drive.py" 110 20 \
  "wait:2;expect:Online;run:kill \$(cat $work/fake/pid);wait:1;expect:Online;run:printf 'c_num|after the restart|u_x\n' >> $work/fake/inject;expect:after the restart" \
  -- "$nchat" -d "$work/conf")"
echo "$out" | grep -E '^(PASS|FAIL)'
echo "$out" | grep -q '^FAIL' && fail=1

echo "== service selector with a second service (keyboard and mouse)"
# the dummy protocol is a second service that needs no account
out="$(python3 -I "$here/drive.py" 110 30 \
  "wait:1;expect:Select protocol;send:0\r;expect:phone number;send:+15555550100\r;wait:2;send:\x11;wait:0.5" \
  -- "$nchat" -s -d "$work/conf")"
if echo "$out" | grep -q '^FAIL'; then
  echo "SKIP no dummy protocol in this build"
else
  # SGR mouse events: ESC [ < button ; column ; row, with 0 left press, 64 and 65 the wheel
  click() { printf 'send:\\x1b[<0\\x3b%d\\x3b%dM;send:\\x1b[<0\\x3b%d\\x3b%dm' "$1" "$2" "$1" "$2"; }
  out="$(python3 -I "$here/drive.py" 120 24 \
    "wait:3;expect:Dummy;expect:Google Messages;expect:@ Dummy;send:\x1b[6\x3b5~;expect:@ Google Messages;send:\x1b[5\x3b5~;expect:@ Dummy;$(click 22 1);expect:@ Google Messages;$(click 10 1);expect:@ Dummy;send:\x1b[6\x3b5~;expect:@ Google Messages;send:\t;expect:Book club @ Google Messages" \
    -- "$nchat" -d "$work/conf")"
  echo "$out" | grep -E '^(PASS|FAIL)'
  echo "$out" | grep -q '^FAIL' && fail=1
fi

echo "== no message text written by nchat itself"
if grep -rl "hello from smoke\|incoming smoke message" "$work/conf" >/dev/null 2>&1; then
  echo "FAIL message text found under the config directory"
  fail=1
else
  echo "PASS none found"
fi

[ "$fail" -eq 0 ] && echo "ALL PASSED" || echo "FAILURES"
exit "$fail"
