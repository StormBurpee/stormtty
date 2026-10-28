# Owned StormTTY managed Oh My Zsh loader for the opt-in bundled Zsh profile.
# Clean managed profile only. Existing-user-configuration profiles preserve theirs.
[[ -o interactive ]] || return
[[ -n ${STORMTTY_SESSION_ID-} ]] || return
# The host distinguishes absent HOME from an explicit empty value before the
# MSYS2 Windows-path import. Do not substitute a home or rewrite explicit paths.
[[ ${STORMTTY_EMPTY_HOME-} == 1 ]] && export HOME=''
unset STORMTTY_EMPTY_HOME
# Only the native managed launch planner may supply this reserved path, after
# overwriting any ambient value and validating its Windows owner/DACL/tree.
typeset +x _stormtty_native_state
typeset _stormtty_native_state=${STORMTTY_NATIVE_ZSH_STATE-}
unset STORMTTY_NATIVE_ZSH_STATE
typeset -gi _stormtty_native_validated=0
# Prevent app bootstrap variables from reinjecting into unrelated descendant shells.
typeset +x ZDOTDIR STORMTTY_SESSION_ID
unsetopt PROMPT_SUBST BEEP
setopt INTERACTIVE_COMMENTS HIST_IGNORE_SPACE
bindkey -e
# Private persistent state/config never lives in the replaceable runtime. An
# explicitly empty/unusable HOME keeps this session ephemeral (no fallback).
typeset -g _stormtty_vendor="${${(%):-%x}:A:h:h}/vendor"
typeset -g _stormtty_state=''
typeset +x STORMTTY_OMZ_LOADED
typeset -g STORMTTY_OMZ_LOADED=0
# An executable cache/config is trusted only after regular-file, owner and
# write-permission checks. Never repair/chmod existing user data implicitly.
_stormtty_safe_file() {
  local -A info
  [[ ! -L $1 ]] || return 1
  [[ -e $1 ]] || return 0
  [[ -f $1 ]] || return 1
  if (( _stormtty_native_validated )); then
    # The complete dedicated tree was checked with Windows handles by the host;
    # synthetic noacl mode/uid values are not a second ACL authority.
    [[ $1 == "$_stormtty_state/"* ]]
  else
    [[ -O $1 ]] || return 1
    zstat -H info -- "$1" 2>/dev/null || return 1
    (( (info[mode] & 8#22) == 0 ))
  fi
}
_stormtty_private_state() {
  local dir
  local -A info
  [[ -n $HOME ]] || return 1
  if [[ $OSTYPE == (msys*|cygwin*) ]]; then
    [[ -n $_stormtty_native_state ]] || return 1
    # Convert the native helper's slash-form drive path using the private
    # runtime's verified /c drive mount contract; preserve UNC spelling.
    case $_stormtty_native_state in
      [A-Za-z]:/*) _stormtty_native_state="/${(L)_stormtty_native_state[1]}${_stormtty_native_state[3,-1]}" ;;
      //?*/*) ;;
      *) return 1 ;;
    esac
    [[ -d $_stormtty_native_state && ! -L $_stormtty_native_state ]] || return 1
    _stormtty_state=$_stormtty_native_state
    _stormtty_native_validated=1
    # New descendants inherit the host-created protected user/SYSTEM DACL.
    (umask 077; command mkdir -p -- "$_stormtty_state/cache/completions") || {
      _stormtty_state=''; _stormtty_native_validated=0; return 1
    }
    return 0
  fi
  [[ -d $HOME && -O $HOME && -w $HOME ]] || return 1
  zmodload zsh/stat || return 1
  zstat -H info -- "$HOME" || return 1
  (( (info[mode] & 8#22) == 0 )) || return 1
  for dir in "$HOME/.stormtty" "$HOME/.stormtty/zsh" "$HOME/.stormtty/zsh/cache" "$HOME/.stormtty/zsh/cache/completions"; do
    [[ ! -L $dir ]] || return 1
    if [[ ! -e $dir ]]; then
      (umask 077; command mkdir -- "$dir") 2>/dev/null || return 1
    fi
    [[ -d $dir && -O $dir && -w $dir ]] || return 1
    zstat -H info -- "$dir" || return 1
    (( (info[mode] & 8#22) == 0 )) || return 1
  done
  local candidate=$HOME/.stormtty/zsh file
  for file in config.zsh config.zsh.zwc history zcompdump-$ZSH_VERSION zcompdump-$ZSH_VERSION.zwc; do
    _stormtty_safe_file "$candidate/$file" || return 1
  done
  _stormtty_state=$candidate
}
_stormtty_private_state
unfunction _stormtty_private_state
unset _stormtty_native_state
# Never inherit user-controlled function directories in the managed profile.
# Linux's versioned layout also permits focused tests of these same source assets.
fpath=(/usr/share/zsh/functions/**/*(N/) /usr/share/zsh/$ZSH_VERSION/functions/**/*(N/))
autoload -Uz add-zsh-hook compinit
typeset -g STORMTTY_EPHEMERAL=1
if [[ -r $_stormtty_vendor/oh-my-zsh/oh-my-zsh.sh ]]; then
  ZSH=$_stormtty_vendor/oh-my-zsh
  ZSH_CUSTOM=$ZSH/custom
  ZSH_CACHE_DIR=/dev/null
  ZSH_COMPDUMP=/dev/null
  HISTFILE=/dev/null
  if [[ -n $_stormtty_state ]]; then
    STORMTTY_EPHEMERAL=0
    ZSH_CACHE_DIR=$_stormtty_state/cache
    ZSH_COMPDUMP=$_stormtty_state/zcompdump-$ZSH_VERSION
    HISTFILE=$_stormtty_state/history
  fi
  zstyle ':omz:update' mode disabled
  ZSH_DISABLE_COMPFIX=false
  ZSH_THEME=''
  plugins=()
  # New executable cache files must be private even under an inherited 0002
  # umask. Restore the user's setting immediately; never chmod existing files.
  typeset _stormtty_saved_umask=$(umask)
  umask 077
  source "$ZSH/oh-my-zsh.sh"
  umask "$_stormtty_saved_umask"
  unset _stormtty_saved_umask
  typeset -g STORMTTY_OMZ_LOADED=1
fi
if [[ $STORMTTY_OMZ_LOADED != 1 ]]; then
  compinit -i -D
fi
if [[ $STORMTTY_EPHEMERAL == 1 ]]; then
  unset HISTFILE
  SAVEHIST=0
else
  HISTSIZE=20000
  SAVEHIST=10000
  setopt HIST_FCNTL_LOCK HIST_SAVE_BY_COPY HIST_IGNORE_SPACE HIST_EXPIRE_DUPS_FIRST
fi
unsetopt PROMPT_SUBST BEEP
setopt INTERACTIVE_COMMENTS
bindkey -e
zstyle ':completion:*' menu select
zstyle ':completion:*:composer:*' fetch-packages no
export LS_COLORS='di=1;34:ln=36:ex=32:so=35:pi=33:bd=33:cd=33:su=31:tw=30;42:ow=34;43'
alias ls='ls --color=auto'
alias ll='ls -lh'
zstyle ':completion:*' list-colors "${(s.:.)LS_COLORS}"
# A dedicated, opt-in user file; never create/overwrite or source ~/.zshrc.
[[ -n $_stormtty_state && -f $_stormtty_state/config.zsh ]] && _stormtty_safe_file "$_stormtty_state/config.zsh" && _stormtty_safe_file "$_stormtty_state/config.zsh.zwc" && source "$_stormtty_state/config.zsh"
[[ ${STORMTTY_PROMPT:-managed} == managed ]] && unsetopt PROMPT_SUBST
# Only packaged modules; no plugin managers and no startup network calls.
if [[ ${STORMTTY_AUTOSUGGESTIONS:-1} != 0 && -r $_stormtty_vendor/zsh-autosuggestions/zsh-autosuggestions.zsh ]]; then
  ZSH_AUTOSUGGEST_STRATEGY=(history)
  ZSH_AUTOSUGGEST_HIGHLIGHT_STYLE='fg=8'
  source "$_stormtty_vendor/zsh-autosuggestions/zsh-autosuggestions.zsh"
fi
zmodload zsh/datetime
zmodload zsh/stat
typeset -gF _stormtty_started=0
typeset -gi _stormtty_exit=0 _stormtty_command_active=0
# Plain structured shell context is the single input to our prompt. A future
# native footer transport must validate/escape it as data, never run its strings.
typeset -gA STORMTTY_CONTEXT
_stormtty_read_small() {
  local -A meta
  REPLY=''
  [[ -f $1 ]] || return 1
  zstat -H meta -- "$1" 2>/dev/null || return 1
  (( meta[size] <= 4096 )) || return 1
  IFS= read -r -u 0 -k 4096 REPLY < "$1"
  REPLY=${REPLY%%$'\n'*}
  [[ -n $REPLY ]]
}
_stormtty_git_context() {
  local dir=$PWD gitdir REPLY depth=0
  STORMTTY_CONTEXT[branch]=''
  # No Git subprocess, status scan, hooks, network, or repository config reads.
  # Resolve normal checkouts, submodules and worktrees, at most 64 ancestors.
  while (( depth++ < 64 )); do
    if [[ -d $dir/.git ]]; then
      gitdir=$dir/.git
      break
    elif [[ -f $dir/.git ]]; then
      _stormtty_read_small "$dir/.git" || return
      [[ $REPLY == 'gitdir: '* ]] || return
      gitdir=${REPLY#gitdir: }
      [[ $gitdir == /* ]] || gitdir=$dir/$gitdir
      break
    fi
    [[ $dir != / ]] || return
    dir=${dir:h}
  done
  [[ -n $gitdir ]] || return
  _stormtty_read_small "$gitdir/HEAD" || return
  case $REPLY in
    'ref: refs/heads/'*) STORMTTY_CONTEXT[branch]=${REPLY#ref: refs/heads/} ;;
    *) [[ -n $REPLY && $REPLY != *[^0-9a-f]* ]] && STORMTTY_CONTEXT[branch]="detached:${REPLY[1,8]}" ;;
  esac
}
_stormtty_prompt_literal() {
  # Zsh prompt percent sequences and terminal controls are independently escaped.
  REPLY=${1//[[:cntrl:]]/?}
  REPLY=${REPLY//\%/%%}
}
_stormtty_preexec() {
  _stormtty_started=$EPOCHREALTIME
  _stormtty_command_active=1
  # Scalar lifecycle only: never emit command text or history.
  printf '\e]133;C\a'
}
_stormtty_precmd() {
  local previous=$?
  local detail='' REPLY path_label
  local -F elapsed=0
  _stormtty_exit=$previous
  # Prompt return ends the foreground command, not any background jobs it spawned.
  if (( _stormtty_command_active )); then
    printf '\e]133;D;%d\a' "$previous"
    _stormtty_command_active=0
  fi
  if (( _stormtty_started > 0 )); then
    (( elapsed = EPOCHREALTIME - _stormtty_started ))
    if (( elapsed >= 1 )); then
      printf -v detail ' %.1fs' "$elapsed"
    fi
  fi
  _stormtty_started=0
  STORMTTY_CONTEXT[cwd]=$PWD
  STORMTTY_CONTEXT[exit]=$previous
  STORMTTY_CONTEXT[duration]=$elapsed
  _stormtty_git_context
  # Zsh supplies the original command status to every hook. Returning that
  # failure would abort later OMZ/ZLE hooks; our bookkeeping must return success.
  [[ ${STORMTTY_PROMPT:-managed} == managed ]] || return 0
  unsetopt PROMPT_SUBST
  path_label=$PWD
  [[ -n $HOME && $PWD == "$HOME" ]] && path_label='~'
  [[ -n $HOME && $PWD == "$HOME/"* ]] && path_label="~/${PWD#"$HOME/"}"
  _stormtty_prompt_literal "$path_label"
  PROMPT=$'%{\e]133;A\a%}'"%B%F{cyan}${REPLY}%f%b"
  if [[ -n ${STORMTTY_CONTEXT[branch]} ]]; then
    _stormtty_prompt_literal "${STORMTTY_CONTEXT[branch]}"
    PROMPT+=" %F{magenta}git:${REPLY}%f"
  fi
  if (( previous != 0 )); then
    PROMPT+=" %F{red}exit:${previous}%f"
  fi
  PROMPT+="%F{yellow}${detail}%f"$'\n''%F{green}>%f '
  PROMPT+=$'%{\e]133;B\a%}'
  return 0
}
add-zsh-hook preexec _stormtty_preexec
add-zsh-hook precmd _stormtty_precmd
# Highlighting is loaded last, after widgets/user configuration.
[[ ${STORMTTY_HIGHLIGHTING:-1} != 0 && -r $_stormtty_vendor/zsh-syntax-highlighting/zsh-syntax-highlighting.zsh ]] && source "$_stormtty_vendor/zsh-syntax-highlighting/zsh-syntax-highlighting.zsh"
typeset +x STORMTTY_MANAGED_ZSH
typeset -gr STORMTTY_MANAGED_ZSH=1
