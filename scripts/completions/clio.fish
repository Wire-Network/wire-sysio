function __clio_complete
  set -l cmd (commandline -opc)
  set -l cur (commandline -ct)
  set -e cmd[1]
  clio $cmd --_autocomplete=$cur
end

complete -c clio -f -a '(__clio_complete)'
