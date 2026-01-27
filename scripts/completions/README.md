# clio shell completion

These scripts wire clio to CLI11's built-in `--_autocomplete` support.

## Bash

```bash
source /path/to/wire-sysio/scripts/completions/clio.bash
```

## Zsh

```zsh
fpath=(/path/to/wire-sysio/scripts/completions $fpath)
autoload -Uz compinit && compinit
```

If you already have compinit enabled, only add the `fpath` line.

## Fish

```fish
cp /path/to/wire-sysio/scripts/completions/clio.fish ~/.config/fish/completions/
```
