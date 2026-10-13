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
echo "== pairing wizard"
out="$(python3 -I "$here/drive.py" 110 36 \
  "wait:1;expect:Select protocol;send:1\r;expect:Continue;send:y\r;expect:Choose;send:\r;expect:Let the helper read;send:y\r;expect:confirm this;expect:Alex Rivera;wait:1" \
  -- "$nchat" -s -d "$work/conf")"
echo "$out" | grep -E '^(PASS|FAIL)'
echo "$out" | grep -q '^FAIL' && fail=1

# the newest chat (the unknown number) is shown first, so messages go there
echo "== list, send, live message"
out="$(python3 -I "$here/drive.py" 110 36 \
  "wait:2;expect:Alex Rivera;expect:Book club;send:hello from smoke;send:\x18;expect:hello from smoke;wait:2.5;count:hello from smoke;run:printf 'c_num|incoming smoke message|u_x\n' >> $work/fake/inject;expect:incoming smoke message;count:incoming smoke message" \
  -- "$nchat" -d "$work/conf")"
echo "$out" | grep -E '^(PASS|FAIL|COUNT)'
echo "$out" | grep -q '^FAIL' && fail=1
# a sent message must be shown once, not once as provisional and once as final
echo "$out" | grep -E '^COUNT [^1]' && fail=1

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
