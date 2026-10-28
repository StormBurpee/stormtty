# Original StormTTY session-only preset for the opt-in bundled Zsh profile.
# Clean managed profile only. Existing-user-configuration profiles preserve theirs.
[[ -o interactive ]] || return
[[ -n ${STORMTTY_SESSION_ID-} ]] || return
# The host distinguishes absent HOME from an explicit empty value before the
# MSYS2 Windows-path import. Do not substitute a home or rewrite explicit paths.
[[ ${STORMTTY_EMPTY_HOME-} == 1 ]] && export HOME=''
unset STORMTTY_EMPTY_HOME
# Prevent app bootstrap variables from reinjecting into unrelated descendant shells.
typeset +x ZDOTDIR STORMTTY_SESSION_ID
unsetopt PROMPT_SUBST BEEP
setopt INTERACTIVE_COMMENTS HIST_IGNORE_SPACE
bindkey -e
# Use only this private runtime's packaged functions, not an inherited FPATH.
fpath=(/usr/share/zsh/functions/**/*(N/))
autoload -Uz add-zsh-hook compinit
zmodload -i zsh/complist
# Bundled fpath only in the launch plan. Ignore insecure directories, write no cache.
compinit -i -D
zstyle ':completion:*' menu select
# Never trigger network package-name completion as part of the shipped preset.
zstyle ':completion:*:composer:*' fetch-packages no
zmodload zsh/datetime
typeset -gF _stormtty_started=0
typeset -gi _stormtty_exit=0
_stormtty_preexec() { _stormtty_started=$EPOCHREALTIME }
_stormtty_precmd() {
  local previous=$?
  local detail=''
  local -F elapsed=0
  _stormtty_exit=$previous
  if (( _stormtty_started > 0 )); then
    (( elapsed = EPOCHREALTIME - _stormtty_started ))
    if (( elapsed >= 1 )); then
      printf -v detail ' %.1fs' "$elapsed"
    fi
  fi
  _stormtty_started=0
  # %~ is handled by Zsh itself; untrusted path/branch data is never eval'd.
  PROMPT='%F{blue}%~%f'
  if (( previous != 0 )); then
    PROMPT+=" %F{red}exit:${previous}%f"
  fi
  PROMPT+="%F{242}${detail}%f"$'\n''%F{blue}>%f '
  return $previous
}
add-zsh-hook preexec _stormtty_preexec
add-zsh-hook precmd _stormtty_precmd
# No Git subprocess in precmd. The host-owned Git module must supply a bounded,
# sanitized, plain-data snapshot through its qualified helper before it is enabled.
# No history writes until the launch plan supplies an app-owned state location.
unset HISTFILE
SAVEHIST=0
# Host smoke can distinguish the managed preset from a bare Zsh launch.
typeset +x STORMTTY_MANAGED_ZSH
typeset -gr STORMTTY_MANAGED_ZSH=1
