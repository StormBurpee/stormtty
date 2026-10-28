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
stormtty_probe_size() {
  stormtty_probe_emit "SIZE:${COLUMNS}x${LINES}"
  stormtty_probe_emit "SIZE_ACK:${stormtty_probe_sequence}:${COLUMNS}x${LINES}"
}
stormtty_probe_cwd() {
  print -r -- "__STORMTTY_CWD:${PWD:t}__"
  # Tests the existing Session CWD effect, not production path translation.
  printf '\033]7;file://stormtty%s\007' "$PWD"
}
stormtty_probe_native() { "$STORMTTY_PROBE_NATIVE" --native-handles }
stormtty_probe_native_size() { "$STORMTTY_PROBE_NATIVE" --native-size "$stormtty_probe_sequence" }
# Invoked only after a qualification failure. Observe before any forced signal
# or corrective action; this is never a replacement for the failed assertion.
stormtty_probe_resize_diagnostic() {
  stormtty_probe_emit "RESIZE_DIAG_BEGIN:${stormtty_probe_sequence}"
  stormtty_probe_emit "RESIZE_DIAG_ZSH_BEFORE:${COLUMNS}x${LINES}"
  local ioctl_size
  ioctl_size=$("$STORMTTY_PROBE_STTY" size)
  local ioctl_result=$?
  stormtty_probe_emit "RESIZE_DIAG_STTY:rc=${ioctl_result}:rows-cols=${ioctl_size}"
  stormtty_probe_emit "RESIZE_DIAG_ZSH_AFTER_STTY:${COLUMNS}x${LINES}"
  "$STORMTTY_PROBE_NATIVE" --native-size "$stormtty_probe_sequence"
  local native_result=$?
  stormtty_probe_emit "RESIZE_DIAG_NATIVE_RC:${native_result}"
  stormtty_probe_emit "RESIZE_DIAG_ZSH_AFTER_NATIVE:${COLUMNS}x${LINES}"
  stormtty_probe_emit "RESIZE_DIAG_END:${stormtty_probe_sequence}"
}
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
  # Do not print behind ZLE's back: its cursor model would still describe the
  # old prompt and reset-prompt could overwrite preceding command output.
  PROMPT="__STORMTTY_PROMPT:${stormtty_probe_sequence}:${stormtty_probe_status}__"$'\n''probe> '
  zle reset-prompt
  stormtty_probe_journal "line-init status=${stormtty_probe_status}"
}
zle -N zle-line-init stormtty_probe_line_init
# Controlled-fixture diagnostic only. Journal what ZLE actually receives, not
# just terminal echo. No terminal output, delays, retries or signal traps added.
# A 256-record in-memory ring avoids per-key filesystem I/O. Records are
# bounded; wrappers preserve the underlying widget's status.
typeset -gi stormtty_probe_journal_count=0 stormtty_probe_journal_enabled=1
typeset -a stormtty_probe_journal_records
stormtty_probe_journal() {
  [[ -n ${STORMTTY_PROBE_JOURNAL-} ]] || return 0
  (( stormtty_probe_sequence >= 5 && stormtty_probe_journal_enabled )) || return 0
  (( ++stormtty_probe_journal_count ))
  local entry="${1[1,384]}"
  local slot=$(( (stormtty_probe_journal_count - 1) % 256 + 1 ))
  stormtty_probe_journal_records[$slot]="${stormtty_probe_journal_count} seq=${stormtty_probe_sequence} size=${COLUMNS}x${LINES} ${entry}"
}
stormtty_probe_journal_reset() {
  stormtty_probe_journal_records=()
  stormtty_probe_journal_count=0
  stormtty_probe_journal_enabled=1
}
stormtty_probe_journal_flush() {
  # Single explicit post-phase/post-failure flush. No per-character file I/O.
  stormtty_probe_journal_enabled=0
  local first=$(( stormtty_probe_journal_count > 256 ? stormtty_probe_journal_count - 255 : 1 ))
  local i slot
  {
    print -r -- "memory-ring total=${stormtty_probe_journal_count} retained-from=${first}"
    for (( i=first; i<=stormtty_probe_journal_count; ++i )); do
      slot=$(( (i - 1) % 256 + 1 ))
      print -r -- "$stormtty_probe_journal_records[$slot]"
    done
  } > "$STORMTTY_PROBE_JOURNAL"
}
stormtty_probe_insert() {
  stormtty_probe_journal "insert-before keys=${(qqqq)KEYS} buffer=${(qqqq)BUFFER}"
  zle .self-insert
  local widget_result=$?
  stormtty_probe_journal "insert-after buffer=${(qqqq)BUFFER}"
  return $widget_result
}
stormtty_probe_accept() {
  stormtty_probe_journal "accept buffer=${(qqqq)BUFFER}"
  zle .accept-line
}
preexec() { stormtty_probe_journal "preexec command=${(qqqq)1}" }
if [[ -n ${STORMTTY_PROBE_JOURNAL-} ]]; then
  zle -N self-insert stormtty_probe_insert
  zle -N accept-line stormtty_probe_accept
fi
stormtty_probe_emit READY
