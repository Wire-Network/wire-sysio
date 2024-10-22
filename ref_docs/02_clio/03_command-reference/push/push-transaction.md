## Description

Push an arbitrary JSON transaction

## Positional Parameters
- `transaction` (text) The JSON of the transaction to push, or the name of a JSON file containing the transaction

## Options
This command has no options

` -h,--help` - Print this help message and exit
 
`-x,--expiration` - set the time in seconds before a transaction expires, defaults to 30s
 
`-f,--force-unique` - force the transaction to be unique. this will consume extra bandwidth and remove any protections against accidently issuing the same transaction multiple times

` -s,--skip-sign` - Specify if unlocked wallet keys should be used to sign transaction

`-j,--json` - print result as json

`-d,--dont-broadcast` - don't broadcast transaction to the network (just print to stdout)

`--return-packed` - used in conjunction with --dont-broadcast to get the packed transaction

`-r,--ref-block` - set the reference block num or block id used for TAPOS (Transaction as Proof-of-Stake)

`--use-old-rpc` - use old RPC push_transaction, rather than new RPC send_transaction

`--use-old-send-rpc` - Use old RPC send_transaction, rather than new RPC /v1/chain/send_transaction2

`-p,--permission` _Type: Text_ - An account and permission level to authorize, as in 'account@permission'

`--max-cpu-usage-ms` _UINT_ - set an upper limit on the milliseconds of cpu usage budget, for the execution of the transaction (defaults to 0 which means no limit)

`--max-net-usage` _UINT_ - set an upper limit on the net usage budget, in bytes, for the transaction (defaults to 0 which means no limit)

`--delay-sec` _UINT_ - set the delay_sec seconds, defaults to 0s

`-t,--return-failure-trace` - Return partial traces on failed transactions

`--retry-irreversible` - Request node to retry transaction until it is irreversible or expires, blocking call

`--retry-num-blocks` - Request node to retry transaction until in a block of given height, blocking call

`-o,--read-only` - Specify a transaction is read-only.

## Example


```sh
clio push transaction {}
```
