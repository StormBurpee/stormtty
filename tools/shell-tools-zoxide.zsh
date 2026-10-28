# StormTTY Windows/MSYS adapter for zoxide 0.10.0; derived from templates/zsh.txt.
# Copyright 2020 Ajeet D'Souza; MIT notice retained in ucrt64/share/licenses/zoxide.
# Changes: explicit Windows pwd conversion, convert query results back to POSIX,
# defer default DB path conversion until use, preserve existing z/zi definitions,
# use chpwd (not precmd) to avoid a process on every prompt, no generated eval.
_stormtty_zoxide_pwd() {
  local dir=$PWD
  [[ ${_ZO_RESOLVE_SYMLINKS:-0} == 1 ]] && dir=${PWD:A}
  command cygpath -w -- "$dir"
}
_stormtty_zoxide_run() {
  if [[ ${_stormtty_zoxide_default_data:-0} == 1 ]] && (( ! ${+_ZO_DATA_DIR} )); then
    local data
    data=$(command cygpath -w -- "$_stormtty_state") || return
    _ZO_DATA_DIR=$data command zoxide "$@"
  else
    command zoxide "$@"
  fi
}
_stormtty_zoxide_cd() {
  builtin cd -- "$@" || return
  [[ ${_ZO_ECHO:-0} != 1 ]] || _stormtty_zoxide_pwd
}
_stormtty_zoxide_query_cd() {
  local result
  result=$(_stormtty_zoxide_run query "$@") || return
  result=$(command cygpath -u -- "$result") || return
  _stormtty_zoxide_cd "$result"
}
_stormtty_zoxide_hook() {
  local dir
  dir=$(_stormtty_zoxide_pwd) || return 0
  _stormtty_zoxide_run add -- "$dir"
  # A hook failure must not abort later shell hooks.
  return 0
}
_stormtty_zoxide_z() {
  if (( $# == 0 )); then
    _stormtty_zoxide_cd ~
  elif (( $# == 1 )) && [[ $1 == - ]]; then
    _stormtty_zoxide_cd "$OLDPWD"
  elif (( $# == 1 )) && [[ -d $1 ]]; then
    _stormtty_zoxide_cd "$1"
  elif (( $# == 2 )) && [[ $1 == -- ]]; then
    _stormtty_zoxide_cd "$2"
  else
    local here
    here=$(_stormtty_zoxide_pwd) || return
    _stormtty_zoxide_query_cd --exclude "$here" -- "$@"
  fi
}
_stormtty_zoxide_zi() { _stormtty_zoxide_query_cd --interactive -- "$@"; }
if (( ! (${+functions[z]} || ${+aliases[z]} || ${+commands[z]}) )); then
  function z() { _stormtty_zoxide_z "$@"; }
  (( ${+functions[compdef]} )) && compdef _cd z
fi
(( ${+functions[zi]} || ${+aliases[zi]} || ${+commands[zi]} )) || function zi() { _stormtty_zoxide_zi "$@"; }
typeset -ga chpwd_functions
chpwd_functions=("${(@)chpwd_functions:#_stormtty_zoxide_hook}" _stormtty_zoxide_hook)
return 0
