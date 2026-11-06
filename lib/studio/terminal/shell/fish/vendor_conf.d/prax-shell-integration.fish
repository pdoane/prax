# The terminal's shell integration for interactive fish. fish finds this file through XDG_DATA_DIRS.
#
# The file first restores the user's XDG_DATA_DIRS. It then sets up the FinalTerm semantic prompt marks (OSC 133),
# which tell the terminal where each prompt, command input and command output starts, and each command's exit code. At
# each prompt, it also reports the working directory as a file URL (OSC 7).

if set -q PRAX_FISH_XDG_DATA_DIRS
    set -gx XDG_DATA_DIRS "$PRAX_FISH_XDG_DATA_DIRS"
    set -e PRAX_FISH_XDG_DATA_DIRS
else
    set -e XDG_DATA_DIRS
end

if status is-interactive; and not set -q __prax_integrated
    set -g __prax_integrated 1

    function __prax_status
        return $argv[1]
    end

    # Writes the working directory as a file URL.
    function __prax_directory --on-event fish_prompt
        printf '\e]7;file://%s%s\a' $hostname (string escape --style=url -- $PWD)
    end

    # Marks the start of the command's output.
    function __prax_preexec --on-event fish_preexec
        set -g __prax_running 1
        printf '\e]133;C\a'
    end

    # Ends the command that ran, with its exit code.
    function __prax_postexec --on-event fish_postexec
        set -l code $status
        if set -q __prax_running
            set -e __prax_running
            printf '\e]133;D;%s\a' $code
        end
    end

    # Wraps the user's fish_prompt in the prompt start and input start marks. It runs at the first prompt, by which
    # time the user's configuration has defined fish_prompt. The wrapped prompt still sees the previous command's
    # status.
    function __prax_wrap_prompt --on-event fish_prompt
        functions -e __prax_wrap_prompt
        functions -q fish_prompt; or return
        functions -c fish_prompt __prax_user_prompt
        function fish_prompt
            set -l code $status
            printf '\e]133;A\a'
            __prax_status $code
            __prax_user_prompt
            printf '\e]133;B\a'
        end
    end
end
