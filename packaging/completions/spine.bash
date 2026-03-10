# Bash completion for Cacti spine poller
#
# Install locations:
#   Linux:   /usr/share/bash-completion/completions/spine
#   Homebrew: $(brew --prefix)/etc/bash_completion.d/spine

_spine() {
    local cur prev words cword
    _init_completion || return

    case "$prev" in
        -C|--conf)
            _filedir
            return
            ;;
        -D|--log)
            _filedir
            return
            ;;
        -N|--mode)
            COMPREPLY=( $(compgen -W 'online offline recovery' -- "$cur") )
            return
            ;;
        -V|--verbosity)
            COMPREPLY=( $(compgen -W 'NONE LOW MEDIUM HIGH DEBUG 1 2 3 4 5' -- "$cur") )
            return
            ;;
        # Integer arguments: poller id, first id, last id, threads -- no completion
        -p|--poller|-f|--first|-l|--last|-t|--threads|-H|--hostlist|-O|--option)
            return
            ;;
    esac

    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W '
            -h --help
            -v --version
            -f --first
            -l --last
            -H --hostlist
            -p --poller
            -t --threads
            -C --conf
            -O --option
            -M --mibs
            -N --mode
            -R --readonly
            -S --stdout
            -P --pingonly
            -V --verbosity
            -D --log
        ' -- "$cur") )
    fi
}

complete -F _spine spine
