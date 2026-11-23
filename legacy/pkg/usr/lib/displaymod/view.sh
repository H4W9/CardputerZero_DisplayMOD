#!/bin/sh
# DisplayMOD viewer. Runs on the EXT display's console as the launcher user.
# Shows the shared "dm" tmux session read-only, sized to the EXT display.
S=dm
tmux has-session -t "$S" 2>/dev/null || tmux new-session -d -s "$S" /usr/lib/displaymod/bigterm-shell
# shellcheck disable=SC2046
set -- $(stty size 2>/dev/null)
rows=${1:-15}
cols=${2:-40}
tmux set-option -t "$S" status off
tmux set-option -t "$S" window-size manual
tmux resize-window -t "$S" -x "$cols" -y "$rows"
# Same 16 colors as the launcher's CLI app (Linux console palette)
for c in 0D1117 FF5F56 27C93F FFBD2E 2F81F7 BC8CFF 39C5CF F0F6FC \
         6E7681 FFA198 56D364 E3B341 79C0FF D2A8FF 56D4DD FFFFFF; do
  printf '\033]P%x%s' "${i:-0}" "$c"
  i=$(( ${i:-0} + 1 ))
done
printf '\033[0m\033[H\033[2J'
exec tmux attach-session -r -t "$S"
