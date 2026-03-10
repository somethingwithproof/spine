# Fish completion for Cacti spine poller
#
# Install locations:
#   Linux:    /usr/share/fish/vendor_completions.d/spine.fish
#   Homebrew: $(brew --prefix)/share/fish/vendor_completions.d/spine.fish

# Disable file completion by default; re-enable explicitly where needed
complete -c spine -f

complete -c spine -s h -l help        -d 'Show help listing'
complete -c spine -s v -l version     -d 'Show version information'

complete -c spine -s f -l first       -d 'Start polling with host id' -x
complete -c spine -s l -l last        -d 'End polling with host id' -x
complete -c spine -s H -l hostlist    -d 'Comma-separated list of host ids to poll' -x
complete -c spine -s p -l poller      -d 'Set the poller id' -x
complete -c spine -s t -l threads     -d 'Override database threads setting' -x

complete -c spine -s C -l conf        -d 'Path to spine configuration file' -F
complete -c spine -s D -l log         -d 'Set log destination file' -F

complete -c spine -s O -l option      -d 'Override DB setting (format setting:value)' -x

complete -c spine -s M -l mibs        -d 'Refresh device system MIB data'
complete -c spine -s R -l readonly    -d 'Do not write output to the database'
complete -c spine -s S -l stdout      -d 'Log to standard output'
complete -c spine -s P -l pingonly    -d 'Ping device and update status only'

complete -c spine -s N -l mode        -d 'Remote poller operating mode' -x \
    -a 'online\tOnline\ mode offline\tOffline\ mode recovery\tRecovery\ mode'

complete -c spine -s V -l verbosity   -d 'Set logging verbosity' -x \
    -a 'NONE\tNo\ logging LOW\tLow MEDIUM\tMedium HIGH\tHigh DEBUG\tDebug 1 2 3 4 5'
