#compdef spine
# Zsh completion for Cacti spine poller
#
# Install locations:
#   Linux:    /usr/share/zsh/vendor-completions/_spine
#   Homebrew: $(brew --prefix)/share/zsh/site-functions/_spine

_spine() {
    _arguments -s \
        '(-h --help)'{-h,--help}'[show help listing]' \
        '(-v --version)'{-v,--version}'[show version information]' \
        '(-f --first)'{-f+,--first=}'[start polling with host id]:host id: ' \
        '(-l --last)'{-l+,--last=}'[end polling with host id]:host id: ' \
        '(-H --hostlist)'{-H+,--hostlist=}'[comma-separated list of host ids to poll]:host id list: ' \
        '(-p --poller)'{-p+,--poller=}'[set the poller id]:poller id: ' \
        '(-t --threads)'{-t+,--threads=}'[override database threads setting]:thread count: ' \
        '(-C --conf)'{-C+,--conf=}'[path to spine configuration file]:config file:_files' \
        '(-O --option)'{-O+,--option=}'[override DB setting (format setting:value)]:setting\:value: ' \
        '(-M --mibs)'{-M,--mibs}'[refresh device system MIB data]' \
        '(-N --mode)'{-N+,--mode=}'[remote poller operating mode]:mode:(online offline recovery)' \
        '(-R --readonly --read-only)'{-R,--readonly,--read-only}'[do not write output to the database]' \
        '(-S --stdout)'{-S,--stdout}'[log to standard output]' \
        '(-P --pingonly)'{-P,--pingonly}'[ping device and update status only]' \
        '(-V --verbosity)'{-V+,--verbosity=}'[set logging verbosity]:verbosity:(NONE LOW MEDIUM HIGH DEBUG 1 2 3 4 5)' \
        '(-D --log)'{-D+,--log=}'[set log destination]:log file:_files' \
        ':first host id: ' \
        ':last host id: '
}

_spine "$@"
