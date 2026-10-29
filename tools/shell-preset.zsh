# Owned StormTTY managed Oh My Zsh loader for the opt-in bundled Zsh profile.
# Clean managed profile only. Existing-user-configuration profiles preserve theirs.
[[ -o interactive ]] || return
[[ -n ${STORMTTY_SESSION_ID-} ]] || return
# The host distinguishes absent HOME from an explicit empty value before the
# MSYS2 Windows-path import. Do not substitute a home or rewrite explicit paths.
[[ ${STORMTTY_EMPTY_HOME-} == 1 ]] && export HOME=''
unset STORMTTY_EMPTY_HOME
# Consume the native host's fixed app-owned sidecar before user configuration.
# Only this console changes; other sessions and system codepages are untouched.
typeset +x _stormtty_console_setup STORMTTY_CONSOLE_UTF8
typeset _stormtty_console_setup=${STORMTTY_NATIVE_CONSOLE_SETUP-}
unset STORMTTY_NATIVE_CONSOLE_SETUP
typeset -gi STORMTTY_CONSOLE_UTF8=0
if [[ $OSTYPE == (msys*|cygwin*) ]]; then
  if [[ $_stormtty_console_setup == [A-Za-z]:/* ]]; then
    _stormtty_console_setup="/${(L)_stormtty_console_setup[1]}${_stormtty_console_setup[3,-1]}"
  fi
  if [[ $_stormtty_console_setup == /* && -f $_stormtty_console_setup && ! -L $_stormtty_console_setup ]] &&
      "$_stormtty_console_setup"; then
    STORMTTY_CONSOLE_UTF8=1
  else
    print -ru2 -- 'StormTTY: native Unicode input setup failed; console input may lose characters.'
  fi
fi
unset _stormtty_console_setup
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
# MSYS accepts a Windows-form ZDOTDIR when opening .zshrc, but Zsh's :A
# path modifier must receive a POSIX path. Normalize the source spelling first
# so vendor lookup does not resolve a drive prefix beneath the working folder.
typeset _stormtty_source=${(%):-%x}
if [[ $OSTYPE == (msys*|cygwin*) && $_stormtty_source == [A-Za-z]:/* ]]; then
  _stormtty_source="/${(L)_stormtty_source[1]}${_stormtty_source[3,-1]}"
fi
typeset -g _stormtty_vendor="${_stormtty_source:A:h:h}/vendor"
unset _stormtty_source
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
    # The host still validates the complete tree on every launch. Do not fork
    # mkdir again when its validated completion directory already exists.
    [[ -d $_stormtty_state/cache/completions ]] ||
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
[[ -r $_stormtty_vendor/shell-tools/init.zsh ]] && source "$_stormtty_vendor/shell-tools/init.zsh"
# Only packaged modules; no plugin managers and no startup network calls.
if [[ ${STORMTTY_AUTOSUGGESTIONS:-1} != 0 && -r $_stormtty_vendor/zsh-autosuggestions/zsh-autosuggestions.zsh ]]; then
  (( ${+ZSH_AUTOSUGGEST_STRATEGY} )) || ZSH_AUTOSUGGEST_STRATEGY=(history)
  typeset _stormtty_autosuggest_async_explicit=${+ZSH_AUTOSUGGEST_USE_ASYNC}
  ZSH_AUTOSUGGEST_HIGHLIGHT_STYLE='fg=8'
  source "$_stormtty_vendor/zsh-autosuggestions/zsh-autosuggestions.zsh"
  # The bundled history strategy is an in-process lookup in zsh's bounded
  # history table. On MSYS, upstream async mode otherwise forks and reads a
  # child PID for every character with no matching suggestion, then redraws
  # again on its response. Typing inside an existing suggestion skips that
  # path, which makes ordinary/quoted new text disproportionately expensive.
  # Avoid the fork/console handoff here, not by delaying terminal input or
  # filtering authoritative cursor updates. POSIX keeps upstream's policy.
  if [[ $OSTYPE == (msys*|cygwin*) && $_stormtty_autosuggest_async_explicit == 0 &&
        ${(j: :)ZSH_AUTOSUGGEST_STRATEGY} == history ]]; then
    unset ZSH_AUTOSUGGEST_USE_ASYNC
  fi
  unset _stormtty_autosuggest_async_explicit
fi
zmodload zsh/datetime
zmodload zsh/stat
typeset -gF _stormtty_started=0
typeset -gi _stormtty_exit=0 _stormtty_command_active=0
# Plain structured shell context feeds the prompt and bounded native footer
# snapshot. The host treats every field as untrusted presentation data.
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
  STORMTTY_CONTEXT[repository_root]=''
  STORMTTY_CONTEXT[head]=''
  STORMTTY_CONTEXT[kind]=none
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
      # Git for Windows writes drive-absolute worktree metadata paths. MSYS
      # and Cygwin accept these directly; do not turn C:/... into $PWD/C:/....
      # Forward-slash UNC paths already satisfy /*. Drive-relative C:foo
      # remains relative, and a POSIX filename containing ':' stays relative.
      if [[ $gitdir != /* && ! ( $OSTYPE == (msys*|cygwin*) && $gitdir == [a-zA-Z]:/* ) ]]; then
        gitdir=$dir/$gitdir
      fi
      break
    fi
    [[ $dir != / ]] || return
    dir=${dir:h}
  done
  [[ -n $gitdir ]] || return
  _stormtty_read_small "$gitdir/HEAD" || return
  case $REPLY in
    'ref: refs/heads/'*)
      [[ -n ${REPLY#ref: refs/heads/} ]] || return
      STORMTTY_CONTEXT[head]=${REPLY#ref: refs/heads/}
      STORMTTY_CONTEXT[branch]=${STORMTTY_CONTEXT[head]}
      STORMTTY_CONTEXT[kind]=branch ;;
    *)
      [[ ${#REPLY} -ge 7 && ${#REPLY} -le 64 && $REPLY != *[^0-9a-fA-F]* ]] || return
      STORMTTY_CONTEXT[head]=${REPLY[1,8]}
      STORMTTY_CONTEXT[branch]="detached:${STORMTTY_CONTEXT[head]}"
      STORMTTY_CONTEXT[kind]=detached ;;
  esac
  STORMTTY_CONTEXT[repository_root]=$dir
}
_stormtty_display_directory() {
  local value=$1 home=${HOME%/}
  [[ $HOME == / ]] && home=/
  if [[ -n $home && $value == "$home" ]]; then
    REPLY='~'
  elif [[ -n $home && $home != / && $value == "$home/"* ]]; then
    REPLY="~/${value#"$home/"}"
  elif [[ $home == / && $value == /* ]]; then
    REPLY="~${value}"
  elif [[ $OSTYPE == (msys*|cygwin*) && $value == /[A-Za-z](|/*) ]]; then
    REPLY="${(U)value[2]}:/${value[4,-1]}"
  else
    REPLY=$value
  fi
}
_stormtty_encode_context() {
  # Byte-wise UTF-8 encoding via Zsh builtins only. No eval, external encoder,
  # or substitution of field data into the printf format. Consumer validates
  # UTF-8; controls are rejected here before any OSC is emitted.
  emulate -L zsh
  unsetopt MULTIBYTE
  local value=$1 byte encoded
  local -i i code
  REPLY=''
  (( ${#value} <= $2 )) || return 1
  for (( i=1; i<=${#value}; ++i )); do
    byte=$value[i]
    builtin printf -v code '%d' "'$byte"
    (( code >= 32 && code != 127 )) || return 1
    case $byte in
      [a-zA-Z0-9._~-]|/|:) REPLY+=$byte ;;
      *) builtin printf -v encoded '%%%02X' $code; REPLY+=$encoded ;;
    esac
  done
  return 0
}
_stormtty_emit_context() {
  local REPLY uri display root key encoded_display encoded_root encoded_head payload
  if [[ ${STORMTTY_SHELL_CONTEXT:-1} == 0 ]]; then
    builtin printf '\e]7400;stormtty-context;1;clear\a'
    return 0
  fi
  # Empty authority is deliberate: no hostname lookup or claim that the host
  # should open this path. Ghostty retains this exact opaque OSC7 URI.
  if [[ $PWD != /* ]] || ! _stormtty_encode_context "$PWD" 2048; then
    builtin printf '\e]7400;stormtty-context;1;clear\a'; return 0
  fi
  uri="file://$REPLY"
  _stormtty_display_directory "$PWD"; display=$REPLY
  _stormtty_display_directory "${STORMTTY_CONTEXT[repository_root]}"; root=$REPLY
  # Validate the opaque URI before emitting the ordinary directory update.
  # Git/display rejection must not leave the host showing the previous CWD.
  if ! _stormtty_encode_context "$uri" 2048; then
    builtin printf '\e]7400;stormtty-context;1;clear\a'; return 0
  fi
  key=$REPLY
  builtin printf '\e]7;%s\a' "$uri"
  if ! _stormtty_encode_context "$display" 2048; then
    builtin printf '\e]7400;stormtty-context;1;clear\a'; return 0
  fi
  encoded_display=$REPLY
  if ! _stormtty_encode_context "$root" 2048; then
    builtin printf '\e]7400;stormtty-context;1;clear\a'; return 0
  fi
  encoded_root=$REPLY
  if ! _stormtty_encode_context "${STORMTTY_CONTEXT[head]}" 256; then
    builtin printf '\e]7400;stormtty-context;1;clear\a'; return 0
  fi
  encoded_head=$REPLY
  payload="7400;stormtty-context;1;set;$key;$encoded_display;$encoded_root;${STORMTTY_CONTEXT[kind]};$encoded_head"
  if (( ${#payload} > 8192 )); then
    builtin printf '\e]7400;stormtty-context;1;clear\a'; return 0
  fi
  builtin printf '\e]%s\a' "$payload"
  return 0
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
  _stormtty_emit_context
  # Zsh supplies the original command status to every hook. Returning that
  # failure would abort later OMZ/ZLE hooks; our bookkeeping must return success.
  [[ ${STORMTTY_PROMPT:-managed} == managed ]] || return 0
  unsetopt PROMPT_SUBST
  _stormtty_display_directory "$PWD"
  path_label=$REPLY
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
