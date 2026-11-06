# The terminal's shell integration for interactive bash. The terminal starts bash with this file as its --rcfile.
#
# The file first reads the user's startup files: the login files if PRAX_BASH_LOGIN is set, and ~/.bashrc otherwise.
# It then sets up the FinalTerm semantic prompt marks (OSC 133), which tell the terminal where each prompt, command
# input and command output starts, and each command's exit code. At each prompt, it also reports the working directory
# as a file URL (OSC 7).

if [[ -n "${PRAX_BASH_LOGIN-}" ]]; then
    builtin unset PRAX_BASH_LOGIN
    if [[ -r /etc/profile ]]; then
        builtin source /etc/profile
    fi
    for __prax_profile in ~/.bash_profile ~/.bash_login ~/.profile; do
        if [[ -r "$__prax_profile" ]]; then
            builtin source "$__prax_profile"
            break
        fi
    done
    builtin unset __prax_profile
elif [[ -r ~/.bashrc ]]; then
    builtin source ~/.bashrc
fi

if [[ $- == *i* && -z "${__prax_integrated-}" ]]; then
    __prax_integrated=1
    __prax_running=
    __prax_ready=

    # Writes the working directory as a file URL, and percent-encodes each byte that a URL path cannot hold.
    __prax_directory() {
        builtin local LC_ALL=C
        builtin local path="$PWD" url= byte i
        for (( i = 0; i < ${#path}; i++ )); do
            byte="${path:i:1}"
            case "$byte" in
                [A-Za-z0-9/._~-]) url+="$byte" ;;
                *) builtin printf -v byte '%%%02X' "'$byte"; url+="$byte" ;;
            esac
        done
        builtin printf '\e]7;file://%s%s\a' "$HOSTNAME" "$url"
    }

    # Ends the command that ran with its exit code, and adds the prompt start and input start marks to PS1. The marks
    # are added again if the user's startup files or hooks set PS1. It runs first in PROMPT_COMMAND, so that it sees the
    # command's status, and it returns that status for the hooks after it.
    __prax_precmd() {
        builtin local code=$?
        if [[ -n "$__prax_running" ]]; then
            builtin printf '\e]133;D;%s\a' "$code"
            __prax_running=
        fi
        __prax_directory
        if [[ "$PS1" != *'\[\e]133;A\a\]'* ]]; then
            PS1='\[\e]133;A\a\]'"$PS1"'\[\e]133;B\a\]'
        fi
        builtin return $code
    }

    # Marks the start of the output of the command that the user typed. A DEBUG trap runs before every simple command,
    # so only the first one after a prompt is the user's. PROMPT_COMMAND arms the trap as its last step, so that its own
    # commands do not count.
    __prax_preexec() {
        if [[ -n "$__prax_ready" ]]; then
            __prax_ready=
            __prax_running=1
            builtin printf '\e]133;C\a'
        fi
    }

    if [[ -n "${bash_preexec_imported-}${__bp_imported-}" ]]; then
        # bash-preexec owns the DEBUG trap and PROMPT_COMMAND, and runs its hooks only for the user's commands.
        __prax_preexec_hook() {
            __prax_ready=1
            __prax_preexec
        }
        precmd_functions=(__prax_precmd "${precmd_functions[@]}")
        preexec_functions+=(__prax_preexec_hook)
    else
        if [[ "$(builtin declare -p PROMPT_COMMAND 2>/dev/null)" == "declare -a"* ]]; then
            PROMPT_COMMAND=(__prax_precmd "${PROMPT_COMMAND[@]}" "__prax_ready=1")
        else
            PROMPT_COMMAND="__prax_precmd${PROMPT_COMMAND:+; $PROMPT_COMMAND}; __prax_ready=1"
        fi
        # The trap passes $_ as its last argument, so that $_ keeps its value for the user's command. Any DEBUG trap
        # that the user's startup files set runs after it.
        __prax_trap="$(builtin trap -p DEBUG)"
        __prax_trap="${__prax_trap#trap -- \'}"
        __prax_trap="${__prax_trap%\' DEBUG}"
        builtin trap '__prax_preexec "$_"'"${__prax_trap:+; $__prax_trap}" DEBUG
        builtin unset __prax_trap
    fi
fi
