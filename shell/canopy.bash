# Canopy shell integration for bash. Sourced automatically by Canopy via a wrapper rcfile.
# Protocol: see DESIGN.md §5. Safe to source in other terminals (no-ops when CANOPY is unset).
[[ -n "${CANOPY_SHELL_INTEGRATION_LOADED:-}" ]] && return 0
CANOPY_SHELL_INTEGRATION_LOADED=1
[[ -z "${CANOPY:-}" ]] && return 0
[[ $- != *i* ]] && return 0

__canopy_b64() { printf '%s' "$1" | base64 -w0 2>/dev/null || printf '%s' "$1" | base64 | tr -d '\n'; }
__canopy_osc() { printf '\e]%s\a' "$1"; }
__canopy_cwd() { __canopy_osc "7;file://${HOSTNAME}${PWD}"; }

# ---- user-facing command ----
canopy() {
    local cmd="${1:-}"; shift || true
    case "$cmd" in
        new)  __canopy_osc "7770;new;$(__canopy_b64 "${*:-$(basename "$PWD")}")" ;;
        note) __canopy_osc "7770;note;$(__canopy_b64 "$*")" ;;
        name) __canopy_osc "7770;name;$(__canopy_b64 "$*")" ;;
        mark) __canopy_osc "7770;mark;$(__canopy_b64 "${*:-mark}")" ;;
        id)   echo "${CANOPY_ACTIVITY:-?}" ;;
        *) cat <<'USAGE'
canopy new [name]   create a child activity (new shell here) under the current one
canopy note <text>  append a note to the current activity
canopy name <name>  rename the current activity
canopy mark [label] bookmark this spot in the output
canopy id           print the current activity id
USAGE
        ;;
    esac
}

# ---- prompt / command lifecycle ----
# The DEBUG trap fires before every simple command, including the ones that make up
# PROMPT_COMMAND. We only treat it as "command start" when it is the first command after a
# prompt was displayed (__canopy_interactive is set by the last PROMPT_COMMAND entry), the
# same technique bash-preexec uses. Commands run in subshells/pipelines fire the trap in the
# child; the OSC still reaches the terminal, and duplicates are harmless to the tracker.
__canopy_interactive=""
__canopy_in_command=0
__canopy_histcmd=""
__canopy_preexec() {
    [[ -n "${COMP_LINE:-}" ]] && return               # completion
    [[ -z "${__canopy_interactive:-}" ]] && return     # not the first command after a prompt
    (( BASH_SUBSHELL == 0 )) && __canopy_interactive=""
    case "$BASH_COMMAND" in __canopy_*) return ;; esac # our own prompt machinery
    local cmdline
    cmdline=$(HISTTIMEFORMAT= builtin history 1 2>/dev/null)
    cmdline="${cmdline#*[0-9]  }"                     # strip history number
    [[ -z "$cmdline" ]] && cmdline="$BASH_COMMAND"
    (( BASH_SUBSHELL == 0 )) && __canopy_in_command=1
    __canopy_osc "7770;cmd;$(__canopy_b64 "$cmdline")"
    __canopy_osc "133;C"
}
__canopy_precmd() {
    local status=$?
    if (( ! __canopy_in_command )) && [[ -n "$__canopy_histcmd" && "$HISTCMD" != "$__canopy_histcmd" ]]; then
        # A command ran but the DEBUG trap never saw it (bash resets the trap in subshells, so
        # e.g. "(cd x && make)" is invisible). Report it late; Canopy anchors it approximately.
        local cmdline
        cmdline=$(HISTTIMEFORMAT= builtin history 1 2>/dev/null)
        cmdline="${cmdline#*[0-9]  }"
        [[ -n "$cmdline" ]] && __canopy_osc "7770;late;$(__canopy_b64 "$cmdline")"
    fi
    # Always report; Canopy drops the block when no command was started (empty prompt).
    __canopy_osc "133;D;$status"
    __canopy_in_command=0
    __canopy_histcmd=$HISTCMD
    __canopy_cwd
    return $status
}
__canopy_interactive_on() { __canopy_interactive=1; }

# Wrap PS1 with A (prompt start) and B (prompt end). \[ \] mark zero-width.
__canopy_ps1_wrap() {
    [[ "$PS1" == *'133;A'* ]] && return
    PS1='\[\e]133;A\a\]'"$PS1"'\[\e]133;B\a\]'
}

if [[ -n "${PROMPT_COMMAND:-}" ]]; then
    if [[ "$(declare -p PROMPT_COMMAND 2>/dev/null)" == "declare -a"* ]]; then
        PROMPT_COMMAND=(__canopy_precmd "${PROMPT_COMMAND[@]}" __canopy_ps1_wrap __canopy_interactive_on)
    else
        PROMPT_COMMAND="__canopy_precmd;${PROMPT_COMMAND};__canopy_ps1_wrap;__canopy_interactive_on"
    fi
else
    PROMPT_COMMAND="__canopy_precmd;__canopy_ps1_wrap;__canopy_interactive_on"
fi
trap '__canopy_preexec' DEBUG
__canopy_ps1_wrap
__canopy_cwd
