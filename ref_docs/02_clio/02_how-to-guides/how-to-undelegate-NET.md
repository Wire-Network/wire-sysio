## Goal

Undelegate resource for an account or application

Beware that only the account which originally delegated resource can undelegate

## Before you begin

* Install the currently supported version of `clio`

* Ensure the reference system contracts from `sysio.contracts` repository is deployed and used to manage system resources

* Understand the following:
  * What is an account
  * What is network bandwidth
  * What is CPU bandwidth

## Steps

Undelegate 0.01 SYS network bandwidth from account `alice` back to account `bob`:

```sh
clio system undelegatebw bob alice "0 SYS" "0.01 SYS"
```

You should see something below:

```console
executed transaction: e7e7edb6c5556de933f9d663fea8b4a9cd56ece6ff2cebf056ddd0835efa6606  184 bytes  452 us
#         sysio <= sysio::undelegatebw          {"from":"alice","receiver":"bob","unstake_net_quantity":"0.01 SYS","unstake_cpu_qu...
warning: transaction executed locally, but may not be confirmed by the network yet         ]
```
