# BigTermCLI shell setup: your normal ~/.bashrc, plus command history that is
# saved after every command (kept when you quit, end the session or reboot).
# Settings come from DisplayMOD: HISTORY, HIST_SIZE, HIST_DUPS.
# shellcheck disable=SC1091
[ -f "$HOME/.bashrc" ] && . "$HOME/.bashrc"

__dm_conf() { sed -n "s/^$1=//p" /etc/displaymod.conf 2>/dev/null; }
__dm_dir="$HOME/.local/share/displaymod"
__dm_clear="$__dm_dir/bigterm_history.clear"
mkdir -p "$__dm_dir"

HISTSIZE=$(__dm_conf HIST_SIZE); HISTSIZE=${HISTSIZE:-1000}
HISTFILESIZE=$HISTSIZE
if [ "$(__dm_conf HIST_DUPS)" = 1 ]; then HISTCONTROL=ignoreboth:erasedups; else HISTCONTROL=ignorespace; fi
if [ "$(__dm_conf HISTORY)" = 0 ]; then
  unset HISTFILE
else
  HISTFILE="$__dm_dir/bigterm_history"
  shopt -s histappend
fi

# After each command: honor a "clear history" request from DisplayMOD, then save.
__dm_history() {
  if [ -e "$__dm_clear" ]; then history -c; rm -f "$__dm_clear"; fi
  [ -n "$HISTFILE" ] && history -a
}
PROMPT_COMMAND="__dm_history${PROMPT_COMMAND:+; $PROMPT_COMMAND}"
