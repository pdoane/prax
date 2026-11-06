# The terminal's shell integration for interactive zsh. It sets up the FinalTerm semantic prompt marks (OSC 133), which
# tell the terminal where each prompt, command input and command output starts, and each command's exit code. At each
# prompt, it also reports the working directory as a file URL (OSC 7).

[[ -o interactive && -z "${__prax_integrated-}" ]] || builtin return 0
typeset -g __prax_integrated=1
typeset -g __prax_running=

builtin autoload -Uz add-zsh-hook

# Writes the working directory as a file URL, and percent-encodes each byte that a URL path cannot hold.
__prax_directory() {
    builtin emulate -L zsh -o extended_glob -o no_multibyte
    builtin print -rn -- $'\e]7;file://'"${HOST}${PWD//(#m)[^A-Za-z0-9\/._~-]/%${(l:2::0:)$(( [##16] #MATCH ))}}"$'\a'
}

# Ends the command that ran with its exit code. It is added before the user's .zshrc adds its hooks, so it normally
# runs first and sees the command's status. It returns that status for the hooks after it.
__prax_precmd() {
    builtin local code=$?
    if [[ -n "$__prax_running" ]]; then
        builtin print -rn -- $'\e]133;D;'"$code"$'\a'
        __prax_running=
    fi
    __prax_directory
    builtin return $code
}

# Adds the prompt start and input start marks to PS1. It moves itself to the end of the precmd hooks, because the
# user's hooks may set PS1 again. A prompt that zsh redraws writes the marks again.
__prax_prompt() {
    builtin local start=$'%{\e]133;A\a%}'
    if [[ "$PS1" != *"$start"* ]]; then
        PS1="$start$PS1"$'%{\e]133;B\a%}'
    fi
    if [[ "${precmd_functions[-1]}" != __prax_prompt ]]; then
        precmd_functions=(${precmd_functions:#__prax_prompt} __prax_prompt)
    fi
}

# Marks the start of the command's output.
__prax_preexec() {
    __prax_running=1
    builtin print -rn -- $'\e]133;C\a'
}

add-zsh-hook precmd __prax_precmd
add-zsh-hook precmd __prax_prompt
add-zsh-hook preexec __prax_preexec
