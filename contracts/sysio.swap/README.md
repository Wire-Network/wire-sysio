![EVODEX](evodex.png)

This is a smart contract for EOSIO that allows the creation of continuous
liquidity pools for any pair of tokens in the chain. It facilitates the decentralization of
exchanges and offers an interesting financial position for the liquidity providers.

Evolutiondex follows the line initiated by Bancor and Uniswap, but with some design improvements that we explain below.

1- Evotokens. For each registered pair there will be a standard token backed by the assets in the corresponding pool. These new tokens can be freely transferred, facilitating the access and management of the investment position. We can call these tokens "evotokens".

2- Initial fee and fee governance. A fee value in [0, 99.99%] is set at initialization of each trading pair. Every pair has a fee authority, the account whose signature `changefee` requires: by default the contract-wide one set at deployment with `setconfig` (WIRE governance executes approved proposals as `sysio`, so that is `sysio`), or a specific account named when the pair is created. A tiny fee of 0.01% is charged when providing liquidity in order
to protect previous liquidity providers from attacks to the fee value.
The action of removing liquidity is free of charge.

**On the formulas determining prices**

We chose to follow the usual criterion: after an exchange operation for a given pair, the product of the amounts in the pools of the corresponding pair must remain equal before the fee is charged. After the fee, that product will rise accordingly. The curve output is rounded downward in favour of the pools, so the product can never fall and the system cannot be gamed through rounding; the fee taken from that output is rounded downward as well, following the fee convention shared with the WIRE reserves (`sysio.opp.common/amm_math.hpp`, which also supplies the constant-product curve itself), except that a nonzero fee always collects at least one unit of the output token so no trade is ever fee-free. This criterion completely determines
the price behaviour. Notice that whenever the amount to exchange is small compared to the pool sizes the price is approximately equal to the quotient between the amounts in the pools.

When adding or removing liquidity, the (again standard) criterion is to keep fixed the ratios between minted evotokens and the amounts in the pools that back their value. Actually a small correction is in order for the case of adding liquidity: 
the 0.01% fee charged will slightly increase the value of the evotoken afterwards.

A pair is seeded with an evotoken supply equal to the square root of the product of the two initial amounts (integer, rounded down), so the unit's value does not depend on the ratio the creator picks. The creator may lock part of that supply: locked shares belong to nobody and can never be redeemed, so the pools always retain the value they represent and the pair can never be emptied and re-seeded at a different unit. Seed-time attacks only victimise the creator, so the size of the lock (zero included) is the creator's decision.


**Time-weighted average prices**

Every pair keeps two cumulative-price accumulators in the `priceaccum` table, one per direction: the running sum of the pool's spot price (the other side's balance over this side's, in Q64.64 fixed point) multiplied by the microseconds that price held. They advance immediately before any operation that changes the pools, and on the permissionless `sync` action, so an interval is always weighted at the price that actually prevailed during it. A reader records an accumulator `r0` at time `t0` and reads `r` at `t`; `(r - r0) / (t - t0)` is the time-weighted average price over the window. A trade that moves the spot price inside a block contributes nothing until time passes at the moved price, which is what makes the average expensive to manipulate. The accumulators are 256 bits wide and cannot wrap; the arithmetic lives in `sysio.opp.common/twap.hpp`, which readers can use for the subtraction and division.

**Yield pools**

A pair may be created as a *yield pool* by naming one of its legs as the pair's shadow token (`yield_leg` in `inittoken`; an empty value makes a plain pool). A shadow token (`sysio.opp.common/shadow_yield.hpp`) pays WIRE yield to its holders through a cumulative index, and the contract, holding the pool's shadow, is such a holder. What it is owed is computed from the token's public state and settled into the pool's other leg with no shares minted, so every existing share appreciates. Settlement runs immediately before every mint and burn, so yield always belongs to the shares that existed when it was earned, and anyone may run it between them with `accrueyield`. The mechanics are deliberately strict: the contract credits the pool, records a receipt for the exact amount keyed by the shadow contract, and calls the token's `claim`; the transfer that delivers the payout must match the receipt, in the same transaction, or the transaction fails. Deposit accounts are not yield-bearing: the contract's whole shadow holding, deposits included, earns for the pool. At most one yield pool may exist per shadow symbol (`yieldpairs` table). A pair's fee authority sets its tick parameters with `setyield`: the horizon over which queued yield is meant to sell and the ceiling on one clip as basis points of the pool's shadow side.

**Some considerations from the perspective of liquidity providers**

Being a liquidity provider is a financial position that deserves a
careful analysis. It is necessary to understand the exposure to
gains and losses for the different future scenarios. This will be addressed
in a different article.

Fee governance is an important tool, since the optimal fee parameter
is expected to change according to the mood of the market. For example,
high volatility suggests high fee. Another relevant factor is the competition from
other exchange opportunities. Without the ability to change the fee, liquidity
providers might want to move their funds from one place to the other, thus
discouraging them to invest in the first place. Therefore a dynamic fee offers
a practical way to benefit from the large exchange activity in the cryptocurrency
space.

**From the perspective of exchangers**

Continuous liquidity pools offer the advantages of decentralized exchange.
There is no need to trust funds to an institution. The prices are computed
algorithmically according to the available liquidity. High liquidity
will translate to low price slippage (this is the price variation as
the exchanged amount varies).
Conversely, if the liquidity pools are small, there will be considerable
slippage and the exchange will only be practical for tiny amounts.

A list of Cleos commands to interact with this contract can be found at [Commands.md](Commands.md)

**References:** Articles from [Bancor](https://about.bancor.network/protocol/), [Uniswap](https://uniswap.org), and [Eos Argentina](https://steemit.com/eosio/@yuhjtman/why-bancor-like-exchanges-are-expected-to-have-fees) (2018).
