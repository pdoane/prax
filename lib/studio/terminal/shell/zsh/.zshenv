# The first startup file of a zsh that the terminal starts with its shell integration. zsh finds it through ZDOTDIR.
#
# The file restores the user's ZDOTDIR, so that zsh reads the user's remaining startup files as usual. It then reads
# the user's .zshenv, and loads the integration if the shell is interactive.

if [[ -n "${PRAX_ZSH_ZDOTDIR+set}" ]]; then
    builtin export ZDOTDIR="$PRAX_ZSH_ZDOTDIR"
    builtin unset PRAX_ZSH_ZDOTDIR
else
    builtin unset ZDOTDIR
fi

# The directory of this file and the integration, computed before the user's .zshenv runs.
typeset -g __prax_integration_dir="${${(%):-%x}:A:h}"

if [[ -r "${ZDOTDIR:-$HOME}/.zshenv" ]]; then
    builtin source "${ZDOTDIR:-$HOME}/.zshenv"
fi

if [[ -o interactive && -r "$__prax_integration_dir/prax.zsh" ]]; then
    builtin source "$__prax_integration_dir/prax.zsh"
fi
builtin unset __prax_integration_dir
