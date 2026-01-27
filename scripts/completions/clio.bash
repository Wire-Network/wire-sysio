_clio_complete() {
  local IFS=$'\n'
  local cur="${COMP_WORDS[COMP_CWORD]}"
  local args=("${COMP_WORDS[@]:1:COMP_CWORD}")
  COMPREPLY=( $(compgen -W "$(clio "${args[@]}" --_autocomplete="${cur}")" -- "$cur") )
}

complete -F _clio_complete clio
