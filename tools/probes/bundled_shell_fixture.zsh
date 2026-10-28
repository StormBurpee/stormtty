# Original StormTTY qualification fixture, not a production shell preset.
# Sourced only from the probe's private ZDOTDIR/.zshrc. No global dotfile edits.
unset HISTFILE
SAVEHIST=0
HISTSIZE=0
unsetopt BEEP PROMPT_SP
setopt MONITOR
PROMPT='probe> '
RPROMPT=''
zmodload zsh/parameter
stormtty_probe_emit() { print -r -- "__STORMTTY_${1}__" }
stormtty_probe_tty() {
  if [[ -t 0 && -t 1 && -t 2 && -o interactive && -o monitor ]]; then
    stormtty_probe_emit TTY_OK
  else
    stormtty_probe_emit TTY_FAIL
  fi
  print -r -- "ZSH_VERSION=$ZSH_VERSION"
}
stormtty_probe_unicode() { print -r -- "__STORMTTY_UNICODE:${1}__" }
stormtty_probe_size() { stormtty_probe_emit "SIZE:${COLUMNS}x${LINES}" }
stormtty_probe_cwd() {
  print -r -- "__STORMTTY_CWD:${PWD:t}__"
  # Tests the existing Session CWD effect, not production path translation.
  printf '\033]7;file://stormtty%s\007' "$PWD"
}
stormtty_probe_native() { "$STORMTTY_PROBE_NATIVE" --native-handles }
stormtty_probe_wait() { stormtty_probe_emit "WAITING:${1}"; "$STORMTTY_PROBE_SLEEP" 30 }
stormtty_probe_after_interrupt() {
  local previous=$stormtty_probe_status
  stormtty_probe_emit "INTERRUPTED:${previous}"
}
stormtty_probe_job_stopped() {
  if [[ "${(j: :)jobstates}" == *suspended* ]]; then
    stormtty_probe_emit JOB_STOPPED
  else
    stormtty_probe_emit JOB_NOT_STOPPED
  fi
}
stormtty_probe_job_running() {
  if [[ "${(j: :)jobstates}" == *running* ]]; then
    stormtty_probe_emit JOB_RUNNING
  else
    stormtty_probe_emit JOB_NOT_RUNNING
  fi
}
stormtty_probe_jobs_empty() {
  if (( ${#jobstates} == 0 )); then
    stormtty_probe_emit JOBS_EMPTY
  else
    stormtty_probe_emit JOBS_REMAIN
  fi
}
stormtty_probe_exit() { exit 37 }
# Capture status before prompt hooks run; emit readiness only once ZLE has
# initialized input mode. A stale prompt or a command's output is not readiness.
typeset -gi stormtty_probe_sequence=0 stormtty_probe_status=0
precmd() {
  stormtty_probe_status=$?
  (( ++stormtty_probe_sequence ))
}
stormtty_probe_line_init() {
  print -r -- "__STORMTTY_PROMPT:${stormtty_probe_sequence}:${stormtty_probe_status}__"
  zle reset-prompt
}
zle -N zle-line-init stormtty_probe_line_init
stormtty_probe_emit READY
