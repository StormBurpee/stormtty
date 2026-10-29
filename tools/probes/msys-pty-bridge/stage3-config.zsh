# Qualification-only config.zsh in a fresh native-validator-protected scratch
# state directory. The shipped managed ZDOTDIR/.zshrc loads this file normally.
# Do not replace the managed prompt, options, history, widgets, or addon hooks.

autoload -Uz add-zsh-hook add-zle-hook-widget
typeset -gi bridge_sequence=0 bridge_status=0
_bridge_managed_precmd() {
  bridge_status=$?
  (( ++bridge_sequence ))
  # Returning a command's failure would abort subsequent managed/OMZ hooks.
  return 0
}
_bridge_managed_line_init() {
  # ZLE's message area provides an input-ready marker without changing PROMPT
  # or bypassing the shipped prompt's OSC 133/structured-context behavior.
  zle -M "__MANAGED_READY:${bridge_sequence}:${bridge_status}__"
  return 0
}
add-zsh-hook precmd _bridge_managed_precmd
add-zle-hook-widget line-init _bridge_managed_line_init

bridge_contract() {
  if [[ ${STORMTTY_MANAGED_ZSH-} == 1 && ${STORMTTY_OMZ_LOADED-} == 1 &&
        ${STORMTTY_EPHEMERAL-} == 0 && ${STORMTTY_CONSOLE_UTF8-} == 1 &&
        ${STORMTTY_PROMPT:-managed} == managed && ! -o promptsubst &&
        $PROMPT == *$'\e]133;A\a'* && $PROMPT == *$'\e]133;B\a'* ]] &&
     (( ${+functions[fzf-file-widget]} && ${+functions[z]} && ${+functions[zi]} &&
        ${+functions[_zsh_autosuggest_start]} && ${+functions[_zsh_highlight]} )); then
    print -r -- '__MANAGED_CONTRACT_OK__'
    return 0
  fi
  print -r -- '__MANAGED_CONTRACT_FAIL__'
  return 1
}
bridge_size() { print -r -- "__BRIDGE_SIZE:${COLUMNS}x${LINES}__" }
bridge_unicode() { print -r -- "__BRIDGE_UNICODE:${1}__" }
bridge_length() { print -r -- "__BRIDGE_LENGTH:${#1}__" }
bridge_typing() { print -r -- "__MANAGED_TYPING:${1}__" }
bridge_native() {
  [[ ${STORMTTY_BRIDGE_NATIVE-} == /usr/bin/stormtty-bridge-native.exe ]] || return 125
  "$STORMTTY_BRIDGE_NATIVE"
}
bridge_wait() {
  # The marker comes from the already-foregrounded child, avoiding the race in
  # printing before launching sleep. Fixed synthetic code, no substituted input.
  /usr/bin/zsh -dfc 'print -r -- "__BRIDGE_WAIT__"; exec /usr/bin/sleep 20'
}
