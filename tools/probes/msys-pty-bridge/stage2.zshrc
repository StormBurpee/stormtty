# Synthetic stage-2 fixture. Installed only as a scratch ZDOTDIR/.zshrc.
# The host launches bundled Zsh interactively with global startup files disabled.
# No user startup scripts, history writes, remote commands, or AI programs.
unset HISTFILE
SAVEHIST=0
HISTSIZE=0
unsetopt BEEP PROMPT_SP PROMPT_SUBST
setopt MONITOR
bindkey -e
PROMPT='bridge> '
RPROMPT=''

bridge_tty() {
  if [[ -t 0 && -t 1 && -t 2 && -o interactive && -o monitor ]]; then
    print -r -- '__BRIDGE_TTY_OK__'
  else
    print -r -- '__BRIDGE_TTY_FAIL__'
    return 1
  fi
}
bridge_size() { print -r -- "__BRIDGE_SIZE:${COLUMNS}x${LINES}__" }
bridge_length() { print -r -- "__BRIDGE_LENGTH:${#1}__" }
bridge_unicode() { print -r -- "__BRIDGE_UNICODE:${1}__" }
bridge_native() {
  [[ -n ${STORMTTY_BRIDGE_NATIVE-} ]] || return 125
  "$STORMTTY_BRIDGE_NATIVE"
}
bridge_wait() {
  # Emit from the already-foregrounded child, not before the parent launches it.
  # This fixed command has no substituted input and skips all per-user rc files.
  /usr/bin/zsh -dfc 'print -r -- "__BRIDGE_WAIT__"; exec /usr/bin/sleep 20'
}

# A prompt rendered by ZLE init establishes input readiness, unlike precmd alone.
# Retain the previous command's status before prompt bookkeeping changes it.
typeset -gi bridge_sequence=0 bridge_status=0
precmd() {
  bridge_status=$?
  (( ++bridge_sequence ))
}
bridge_line_init() {
  PROMPT="__BRIDGE_READY:${bridge_sequence}:${bridge_status}__"$'\n''bridge> '
  zle reset-prompt
}
zle -N zle-line-init bridge_line_init
