**NOTE: In this example we use the PAIR PESO/EOS, which creates the evotoken EOSPESO. We assume an EOS token located in the eosio.token contract as usual, configured as the system token (every pair's second leg), and a PESO token located in the contract pesocontract as the pair's first leg. You would need to replace these variables depending on your trading pairs.** 

First, let us describe the single actions of the smart contract.

Configure the contract (deployment step, the contract's own authority): the contract-wide fee authority and the system token. Nothing else works before this.

    cleos push action evolutiondex setconfig '["sysio", {"contract":"eosio.token", "sym":"4,EOS"}]' -p evolutiondex

Open a channel in the contract. This channel will store your trading tokens. You need to create one channel for each token you plan to trade. The second input below is the ram payer, and the authorizer must be the ram payer.

    cleos push action evolutiondex openext '["YOUR_ACCOUNT", "YOUR_ACCOUNT", {"contract":"eosio.token", "sym":"4,EOS"}]' -p YOUR_ACCOUNT

Open a channel for the second token you wish to trade in evodex:

    cleos push action evolutiondex openext '["YOUR_ACCOUNT", "YOUR_ACCOUNT", {"contract":"pesocontract", "sym":"4,PESO"}]' -p YOUR_ACCOUNT

Close the contract's channel for a specific token. In case there are funds there,
it returns them to the account "TO".

    cleos push action evolutiondex closeext '["YOUR_ACCOUNT", "TO", {"contract":"eosio.token", "sym":"4,EOS"}, "memo"]' -p YOUR_ACCOUNT

Fill your account with the desired tokens. The contract accepts only the system token and the first legs of existing pairs; anything else is refused. A pair's first leg has no pair yet when its seed is deposited, so that one transfer also carries the contract's authority.

    cleos push action eosio.token transfer '["YOUR_ACCOUNT", "evolutiondex", "100.0000 EOS", "memo"]' -p YOUR_ACCOUNT

    cleos push action pesocontract transfer '["YOUR_ACCOUNT", "evolutiondex", "100.0000 PESO", "pesitos"]' -p YOUR_ACCOUNT -p evolutiondex

Check your open channels and balances:

    cleos get table evolutiondex YOUR_ACCOUNT evodexacnts

Withdraw funds from your opened channels, to the account "TO":

    cleos push action evolutiondex withdraw '["YOUR_ACCOUNT", "TO", {"contract":"eosio.token", "quantity":"1.0000 EOS"}, "memo"]' -p YOUR_ACCOUNT

Create the PESO/EOS evotoken: the pair's own token first, the system token second. Set the initial liquidity, the initial fee for the trading pair (in units of 0.01%, here 0.1%), the fee authority (an empty name adopts the contract-wide authority set at deployment, any other name makes that account the pair's own), the shares to lock, and the yield leg (null for a plain pool). The supply minted is the square root of the product of the two amounts; the locked part is held by nobody and can never be redeemed, which keeps the pool from ever being emptied and bounds how far one share's value can be pushed. It is your call how much to lock, zero included. The contract's authority is required alongside yours, and a token can form only one pair.

    cleos push action evolutiondex inittoken '["YOUR_ACCOUNT", "4,EOSPESO", {"contract":"pesocontract", "quantity":"1.0000 PESO"}, {"contract":"eosio.token", "quantity":"1.0000 EOS"}, 10, "", "0.1000 EOSPESO", null]' -p YOUR_ACCOUNT -p evolutiondex

Create a yield pool instead: name the first leg as a shadow token (a token that pays WIRE yield to its holders).

    cleos push action evolutiondex inittoken '["YOUR_ACCOUNT", "4,SHDEOS", {"contract":"shadowtoken", "quantity":"1.0000 SHD"}, {"contract":"eosio.token", "quantity":"1.0000 EOS"}, 10, "", "0.0000 SHDEOS", {"contract":"shadowtoken", "sym":"4,SHD"}]' -p YOUR_ACCOUNT -p evolutiondex

Set a yield pool's tick parameters, signed by its fee authority: the horizon (seconds) over which queued yield is meant to sell, the ceiling on one clip in basis points of the pool's shadow side, and the least a clip may be, in units of the shadow. Size the floor so the pair's fee on a clip's output reaches a whole unit on its own, which at a 0.1% fee is an output of 1000 units; below that the one-unit minimum swap fee is what a clip actually pays, and a small enough clip is consumed entirely. Erring high only makes sales larger and rarer at the same average rate, but a floor above the depth cap stops the pair selling altogether.

    cleos push action evolutiondex setyield '["SHDEOS", 86400, 3, 1000]' -p sysio

Settle the yield the pool is owed on the shadow it holds into its other leg, minting nothing. This also runs by itself before every addliquidity and remliquidity; anyone may call it in between:

    cleos push action evolutiondex accrueyield '["SHDEOS"]' -p YOUR_ACCOUNT

Queue shadow to be sold through the pool: announce the exact amount, then transfer it. The transfer that matches the announcement fills the pool's reservoir instead of your deposit; while the announcement is pending any other transfer from you is refused, and announcing again replaces it. Both steps fit in one transaction.

    cleos push action evolutiondex fundyield '["YOUR_ACCOUNT", "SHDEOS", "5.0000 SHD"]' -p YOUR_ACCOUNT -p YOUR_ACCOUNT@sysio.payer
    cleos push action shadowtoken transfer '["YOUR_ACCOUNT", "evolutiondex", "5.0000 SHD", ""]' -p YOUR_ACCOUNT

The announcement's row is billed to you, which is why it needs the payer permission. Drop one you are not going to deliver, which refunds the row and lets you deposit normally again:

    cleos push action evolutiondex cancelyield '["YOUR_ACCOUNT"]' -p YOUR_ACCOUNT

See what is queued:

    cleos get table evolutiondex evolutiondex reservoirs -L SHDEOS -U SHDEOS

Sell one clip of the queue through the pool and hand the proceeds to the shadow's holders (the pool gets its share back on the next accrual). Anyone may call this; a crank calls it every block. Requires the pool's tick parameters to be set, and the shadow token's sysio.code seat on the contract's active permission, granted at deployment:

    cleos set account permission evolutiondex active --add-code shadowtoken
    cleos push action evolutiondex tickyield '["SHDEOS"]' -p YOUR_ACCOUNT

Change a pair's fee, signed by its fee authority:

    cleos push action evolutiondex changefee '["EOSPESO", 30]' -p sysio

Check your evotokens balance:

    cleos get table evolutiondex YOUR_ACCOUNT accounts

Add more liquidity to a pool. Set the exact amount of evotoken to obtain, in this case 
1.5000 EOSPESO, and the maximum you are willing to pay of each token of the pair, first leg first.

    cleos push action evolutiondex addliquidity '["YOUR_ACCOUNT", "1.5000 EOSPESO", 
    "2.0000 PESO", "2.0000 EOS"]' -p YOUR_ACCOUNT

Sell your evotokens and retire liquidity. The amount of evotoken is exact and the other two are minima required.

    cleos push action evolutiondex remliquidity '["YOUR_ACCOUNT", "1.0000 EOSPESO", 
    "0.1000 PESO", "1.0000 EOS"]' -p YOUR_ACCOUNT

Exchange your tokens.
There two methods. The first one is to do a transfer to the contract with a memo starting with "exchange:" and followed by the details of your operation, with the format "EVOTOKN, min_expected_asset, memo". Blank spaces before EVOTOKN, min_expected_asset and memo are ignored. The amount to be obtained by the user will be computed by the contract and executed only if it is at least min_expected_asset. 

    cleos push action eosio.token transfer '["YOUR_ACCOUNT", "evolutiondex", "1.0000 EOS", "exchange: EOSPESO, 0.1000 PESO, memo for the transfer"]' -p YOUR_ACCOUNT

The other method operates between funds already deposited in the contract. The structure
of the input is account, evotoken, extended_asset to pay (exact), asset to receive (limiting).

    cleos push action evolutiondex exchange '["YOUR_ACCOUNT", "EOSPESO", 
    {"contract":"eosio.token", "quantity":"1.0000 EOS"}, "0.1000 PESO"]' -p YOUR_ACCOUNT

The amount to pay must be positive and the amount to receive nonnegative; there is no
exact-output form (asking for an exact amount to receive by passing negative amounts).

Read the pair's cumulative prices (a time-weighted average price oracle). Each row holds, for both directions, the running sum of the pool price in Q64.64 fixed point multiplied by the microseconds it held, and the time of the last update:

    cleos get table evolutiondex evolutiondex priceaccum -L EOSPESO -U EOSPESO

Bring the accumulators up to the current block time without trading, so a snapshot taken now is current. Anyone may call this:

    cleos push action evolutiondex sync '["EOSPESO"]' -p YOUR_ACCOUNT

Transfer your evotokens to another account:

    cleos push action evolutiondex transfer '["YOUR_ACCOUNT", "argentinaeos", "0.0001 EOSPESO", "ITS ALIVE"]' -p YOUR_ACCOUNT

See evotoken stats:

    cleos get table evolutiondex EOSPESO stat

In many practical cases, users will prefer to run many actions in a single transaction.
For example, if you want to add liquidity, you will probably prefer to close the accounts in the contract evolutiondex corresponding to the external tokens, to avoid spending RAM. To that end, you may run:

    cleos push transaction addliquidity.json

where the file addliquidity.json contains:

    {
        "actions":
        [
        {
            "account": "evolutiondex",
            "name": "openext",
            "authorization": [{"actor": "YOUR_ACCOUNT","permission": "active"}],
            "data": {
                "user": "YOUR_ACCOUNT",
                "payer": "YOUR_ACCOUNT",
                "ext_symbol": {"contract":"eosio.token", "sym":"4,EOS"}
            }
        },{
            "account": "evolutiondex",
            "name": "openext",
            "authorization": [{"actor": "YOUR_ACCOUNT","permission": "active"}],
            "data": {
                "user": "YOUR_ACCOUNT",
                "payer": "YOUR_ACCOUNT",
                "ext_symbol": {"contract":"pesocontract", "sym":"4,PESO"}
            }
        },{
            "account": "eosio.token",
            "name": "transfer",
            "authorization": [{"actor": "YOUR_ACCOUNT","permission": "active"}],
            "data": {
                "from": "YOUR_ACCOUNT",
                "to": "evolutiondex",
                "quantity": "2.0000 EOS",
                "memo": ""
            }
        },{
            "account": "pesocontract",
            "name": "transfer",
            "authorization": [{"actor": "YOUR_ACCOUNT","permission": "active"}],
            "data": {
                "from": "YOUR_ACCOUNT",
                "to": "evolutiondex",
                "quantity": "2.0000 PESO",
                "memo": ""
            }
        },{
            "account": "evolutiondex",
            "name": "addliquidity",
            "authorization": [{"actor": "YOUR_ACCOUNT","permission": "active"}],
            "data": {
                "user": "YOUR_ACCOUNT",
                "to_buy": "1.5000 EOSPESO",
                "max_asset1": "2.0000 PESO",
                "max_asset2": "2.0000 EOS",                
            }
        },{
            "account": "evolutiondex",
            "name": "closeext",
            "authorization": [{"actor": "YOUR_ACCOUNT","permission": "active"}],
            "data": {
                "user": "YOUR_ACCOUNT",
                "to": "TO",
                "ext_symbol": {"contract":"pesocontract", "sym":"4,PESO"},
                "memo": ""
            }
        },{
            "account": "evolutiondex",
            "name": "closeext",
            "authorization": [{"actor": "YOUR_ACCOUNT","permission": "active"}],
            "data": {
                "user": "YOUR_ACCOUNT",
                "to": "TO",                
                "ext_symbol": {"contract":"eosio.token", "sym":"4,EOS"},
                "memo": ""
            }
        }
        ]
    }    

The same idea applies to the operations of removing liquidity and inittoken.
Typically, a graphical user interface will perform this kind of multiaction transactions.