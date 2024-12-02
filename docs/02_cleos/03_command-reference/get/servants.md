## Description
Retrieve accounts which are servants of a given account 

## Info

**Command**

```sh
clio get servants
```
**Output**

```console
Usage: clio get servants account

Positionals:
  account TEXT                The name of the controlling account
```

## Command

```sh
clio get servants inita
```

## Output

```json
{
  "controlled_accounts": [
    "tester"
  ]
}
```
