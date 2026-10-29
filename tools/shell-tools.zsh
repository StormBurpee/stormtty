# StormTTY build-shipped fzf/zoxide integration. Source AFTER user config and
# completion setup, BEFORE syntax highlighting. No startup tool invocation or generated-code eval.
[[ -o interactive ]] || return
[[ ${STORMTTY_SHELL_TOOLS:-1} != 0 ]] || return
# The runtime is an immutable, hash-inventoried application asset, not user cache.
typeset -g _stormtty_tools_root=${${(%):-%x}:A:h:h:h:h}
# Native fzf must run custom preview/walker commands in this shell by default.
# Keep an explicit user SHELL (including empty) untouched.
(( ${+SHELL} )) || export SHELL="$_stormtty_tools_root/usr/bin/zsh.exe"
# Append rather than prepend: explicit user tool selection remains authoritative.
(( ${path[(Ie)$_stormtty_tools_root/ucrt64/bin]} )) || path+=("$_stormtty_tools_root/ucrt64/bin")
if [[ ${STORMTTY_FZF:-1} != 0 ]]; then
  [[ ${STORMTTY_FZF_COMPLETION:-1} == 0 ]] || source "$_stormtty_tools_root/ucrt64/share/fzf/completion.zsh"
  [[ ${STORMTTY_FZF_KEYS:-1} == 0 ]] || source "$_stormtty_tools_root/ucrt64/share/fzf/key-bindings.zsh"
fi
if [[ ${STORMTTY_ZOXIDE:-1} != 0 ]] && (( ${+functions[compdef]} && ! ${+functions[_zoxide]} )); then
  source "$_stormtty_tools_root/ucrt64/share/zsh/site-functions/_zoxide"
fi
# No database or hook in an ephemeral shell. A user may still invoke the tool.
if [[ ${STORMTTY_ZOXIDE:-1} != 0 && ${STORMTTY_EPHEMERAL:-1} != 1 && -n $_stormtty_state ]]; then
  # POSIX spelling is converted when the hook/query is used, not during startup.
  # An explicit override (including empty) belongs to the user and is not changed.
  if (( ! ${+_ZO_DATA_DIR} )); then
    typeset -g _stormtty_zoxide_default_data=1
  fi
  source "$_stormtty_tools_root/stormtty/vendor/shell-tools/zoxide.zsh"
fi
