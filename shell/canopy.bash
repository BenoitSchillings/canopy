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
__canopy_in_command=0
__canopy_preexec() {
    # DEBUG trap fires for every simple command; only act on the first one after a prompt.
    [[ "$BASH_COMMAND" == "__canopy_precmd" ]] && return
    [[ -n "${COMP_LINE:-}" ]] && return           # completion
    (( __canopy_in_command )) && return
    __canopy_in_command=1
    local cmdline
    cmdline=$(HISTTIMEFORMAT= builtin history 1 2>/dev/null)
    cmdline="${cmdline#*[0-9]  }"                  # strip history number
    [[ -z "$cmdline" ]] && cmdline="$BASH_COMMAND"
    __canopy_osc "7770;cmd;$(__canopy_b64 "$cmdline")"
    __canopy_osc "133;C"
}
__canopy_precmd() {
    local status=$?
    if (( __canopy_in_command )); then
        __canopy_osc "133;D;$status"
    else
        __canopy_osc "133;D"                      # empty prompt (just Enter)
    fi
    __canopy_in_command=0
    __canopy_cwd
    return $status
}

# Wrap PS1 with A (prompt start) and B (prompt end). \[ \] mark zero-width.
__canopy_ps1_wrap() {
    [[ "$PS1" == *'133;A'* ]] && return
    PS1='\[\e]133;A\a\]'"$PS1"'\[\e]133;B\a\]'
}

if [[ -n "${PROMPT_COMMAND:-}" ]]; then
    if [[ "$(declare -p PROMPT_COMMAND 2>/dev/null)" == "declare -a"* ]]; then
        PROMPT_COMMAND=(__canopy_precmd "${PROMPT_COMMAND[@]}" __canopy_ps1_wrap)
    else
        PROMPT_COMMAND="__canopy_precmd;${PROMPT_COMMAND};__canopy_ps1_wrap"
    fi
else
    PROMPT_COMMAND="__canopy_precmd;__canopy_ps1_wrap"
fi
trap '__canopy_preexec' DEBUG
__canopy_ps1_wrap
__canopy_cwd
