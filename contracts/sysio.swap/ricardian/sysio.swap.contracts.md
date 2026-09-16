<h1 class="contract">openext</h1>

---
spec_version: "0.2.0"
title: Open extended Balance
summary: 'Open a zero quantity extended balance for {{nowrap user}}'
---

{{ram_payer}} agrees to establish a zero quantity extended balance for {{user}} 
for the {{ext_symbol}} extended symbol. 

If {{user}} does not have an extended balance for {{ext_symbol}}, {{ram_payer}} will be designated as the RAM payer of {{user}}'s extended balance for {{ext_symbol}}. As a result, RAM will be deducted from {{ram_payer}}’s resources to create the necessary records.

The authorization of {{ram_payer}} is required.


<h1 class="contract">closeext</h1>

---
spec_version: "0.2.0"
title: Close Extended Balance
summary: 'Close {{nowrap user}}’s extended balance'
---

{{user}} agree to close their extended balance for {{ext_symbol}}.

If the extended balance is nonzero, it will be transfered to {{to}} before closing it, with a memo equal to {{memo}}. This transfer action corresponds to the contract of {{ext_symbol}}. 

In order to function properly, it is necessary that the contract of {{ext_symbol}} permanently has a transfer action that satisfies the conditions (1), (2), (6) of the present contract's transfer action.

RAM will be refunded to the RAM payer of {{user}}'s extended balance for {{ext_symbol}}.

The authorization of {{user}} is required.


<h1 class="contract">ontransfer</h1>

---
spec_version: "0.2.0"
title: On transfer
summary: 'Deposit or exchange upon a transfer from {{nowrap user}}'
---

This action is executed as a response to a notification of a transfer action with the input {{from}}, {{to}}, {{quantity}}, {{memo}} in that order. 

The transfer is accepted only if the extended symbol formed by the transfer's contract and the symbol {{asset_to_symbol quantity}} is the system token set by setconfig, or is the first leg of the pair it forms with the system token, or the transfer carries this contract's authority (the authority that creates pairs, so that a pair's first leg can be seeded before the pair exists). Any other transfer fails.

The default response is to deposit {{quantity}} to {{from}}'s extended balance for that extended symbol. This is only possible if such extended balance previously exists.

If {{memo}} starts with "deposit to:", the account {{from}} will be replaced by the subsequent content of {{memo}} whenever it is possible.

Two typed routes take precedence over the memo, and each accepts exactly one transfer. If {{from}} is a shadow token from which this contract has a yield payout outstanding (see accrueyield), {{quantity}} must be that payout, which was already credited to the pool; the receipt is retired and nothing else is deposited. Otherwise, if {{from}} has a yield funding pending (see fundyield), {{quantity}} must be the announced amount, which fills the announced pair's reservoir. In either case any other transfer from {{from}} fails.

If {{memo}} starts with "exchange:", the subsequent content of the memo is expected
to have the form "EVOTOKEN,min_expected_asset,optional memo". An exchange operation will be processed with this data, following the same conversion rules as in the exchange action for the input {{from}}, {{EVOTOKEN}}, {{quantity}}, {{min_expected_asset}}. If the output asset is at least equal to {{min_expected_asset}}, it will be transfered from this contract
to {{user}}, with {{optional memo}} as memo.

In order to function properly, it is necessary that both pool contracts associated to {{EVOTOKEN}}, permanently have a transfer action that satisfies the conditions (1), (2), (6) of the present contract's transfer action.


<h1 class="contract">withdraw</h1>

---
spec_version: "0.2.0"
title: Withdraw
summary: 'Withdraw funds from extended balance'
---

{{user}} agree to withdraw the extended asset {{to_withdraw}} from their account.

In order to function properly, it is necessary that the contract of {{to_withdraw}} permanently has a transfer action that satisfies the conditions (1), (2), (6) of the present contract's transfer action.


<h1 class="contract">inittoken</h1>

---
spec_version: "0.2.0"
title: Initialize token
summary: 'Initializes an evotoken by setting initial pair of token pools'
---

{{user}} agrees to initialize a pair token with symbol {{new_symbol}}, with the following initial parameters: pool1 = {{initial_pool1}}, pool2 = {{initial_pool2}}, fee = {{initial_fee}} (in units of 1/10000, at most 9999), fee_authority = {{fee_authority}}. {{initial_pool2}} must be in the system token set by setconfig; {{initial_pool1}} is the pair's own token, and a token can form only one pair. The extended assets {{initial_pool1}} and {{initial_pool2}} will be deducted from the corresponding extended balances of {{user}}.

The fee authority is the account whose authorization the changefee action requires for this pair. An empty {{fee_authority}} adopts the contract-wide fee authority set by setconfig; any other name makes that account the pair's own.

The pair token supply minted is the square root of the product of the two initial pool amounts, rounded downward. Of it, {{locked_shares}} are held by no account and can never be redeemed, so the pools always retain the value those shares represent; the remainder is credited to {{user}}. {{locked_shares}} must be less than the amount minted.

If {{yield_leg}} is given it must be the first leg, and the pair becomes a yield pool on that shadow token: the yield the contract is owed on the shadow it holds is settled into the second leg without minting, before every addliquidity and remliquidity and on accrueyield. Without {{yield_leg}} the pair is a plain pool.

RAM will be deducted from {{user}}’s resources to create the necessary records.
Authorization of {{user}} and of the contract is required.


<h1 class="contract">addliquidity</h1>

---
spec_version: "0.2.0"
title: Add liquidity
summary: '{{nowrap user}} buys an evotoken by adding liquidity to pools'
---

{{user}} agrees to buy the asset {{to_buy}} by paying no more than {{max_asset1}} and {{max_asset2}} to be deducted from {{user}}'s extended balances and added to the token
{{asset_to_symbol_code to_buy}} pools. The contracts of the assets that {{user}} pays will match the ones of the pools. The asset {{to_buy}} is minted by the contract.

The first asset to be paid by {{user}} is computed as x + y, where

x = pool1 * {{to_buy}} / supply, up to the precision of the symbol of pool1 rounded upward.
y = x / 10000, up to the same precision as x, again rounded upward.
The variables pool1 and supply denote the values pool1 and supply associated to 
the token {{asset_to_symbol_code to_buy}} respectively, at the moment of operation.

The second asset to be paid by {{user}} is computed analogously.

Authorization of {{user}} is required.
The operation is executed only if the amounts to be paid by {{user}} are at most those indicated by {{user}}. 


<h1 class="contract">remliquidity</h1>

---
spec_version: "0.2.0"
title: Remove liquidity
summary: '{{nowrap user}} sells an evotoken, removing liquidity from pools'
---

{{user}} agrees to sell the asset {{to_sell}} by receiving at least {{min_asset1}} and {{min_asset2}} to be added to {{user}}'s extended balances and removed from the token
{{asset_to_symbol_code to_sell}} pools. The asset {{to_sell}} is retired
from circulation by the contract.

The first asset to be received by {{user}} is computed as

pool1 * {{to_sell}} / supply, up to the precision of the symbol of pool1 rounded downward.

The variables pool1 and supply denote the parameters pool1 and supply associated to the token {{asset_to_symbol_code to_sell}} respectively, at the moment of operation.

The second asset to be received by {{user}} is computed analogously. 

Authorization of {{user}} is required.
The operation is executed only if the amounts to be received by {{user}} are at least
those indicated by {{user}}. 


<h1 class="contract">exchange</h1>

---
spec_version: "0.2.0"
title: Exchange
summary: 'Exchange token through a specific pair'
---

{{user}} agree to substract {{ext_asset_in}} and to add at least {{min_expected}} to their extended balances. {{ext_asset_in}} must be positive and {{min_expected}} nonnegative. The extended symbol of {{ext_asset_in}} must match one of the pools associated to the token {{pair_token}}. The contract of {{min_expected}} is given by the other pool of that pair. These extended assets will be respectively added to and substracted from the corresponding pools.

The extended asset to be added to {{users}}'s extended balance as a result of the exchange operation, is computed as x - y, where

x = pool_out * {{ext_asset_in}} / (pool_in + {{ext_asset_in}}), up to the precision of the symbol of pool_out rounded downward.
y = x * fee / 10000, up to the same precision as x, again rounded downward, but never less than one unit of that precision when both x and fee are positive. The fee y remains in the pools.

The variable pool_in denotes the corresponding extended asset pool1 or pool2 associated to the token {{pair_token}}; namely, the one whose extended symbol matches that of {{ext_asset_in}}. The variable pool_out is the extended asset pool1 or pool2, the one that is not pool_in. The variable fee is the integer fee associated to the token {{pair_token}}.
The values of these three variables must be taken at the moment of operation.

Authorization of {{user}} is required.
The operation is executed only if the extended asset to be added to {{user}} is at least
that indicated by {{user}}. 


<h1 class="contract">sync</h1>

---
spec_version: "0.2.0"
title: Sync price accumulators
summary: 'Bring the cumulative prices of {{nowrap pair_token}} up to the current time'
---

The cumulative-price accumulators of the token {{pair_token}} are advanced to the current block time: each accumulator grows by the pool price that has held since the previous update, multiplied by the time elapsed. The pools are not modified. No authorization is required.

The accumulators also advance in the same way immediately before any operation that changes the pools of {{pair_token}}. A reader that records the accumulators at two times obtains the time-weighted average price between them as the difference of the accumulators divided by the elapsed time.


<h1 class="contract">setyield</h1>

---
spec_version: "0.2.0"
title: Set yield parameters
summary: 'Set the yield tick parameters of {{nowrap pair_token}}'
---

The fee authority associated to the token {{pair_token}}, which must be a yield pool, authorizes to set the horizon of {{conversion_horizon_sec}} seconds over which the pool's queued yield is meant to sell, the ceiling of {{depth_cap_bps}} basis points (at most 10000) of the pool's shadow side on one clip, and the least a clip may be, {{clip_floor}}, in units of the shadow token. All three must be nonzero before the pool's yield tick can run. The pair's tick clock restarts now.


<h1 class="contract">accrueyield</h1>

---
spec_version: "0.2.0"
title: Accrue yield
summary: 'Settle the yield owed to the pool of {{nowrap pair_token}}'
---

The token {{pair_token}} must be a yield pool. The WIRE the contract is owed by the pool's shadow token, computed from that token's public distribution state, is credited to the pool's other leg with no pair tokens minted, and the shadow token's claim action is called to deliver it. The delivery must match the credited amount exactly within the same transaction; otherwise the transaction fails. When nothing is owed the pools are not modified.

The same settlement is performed immediately before every addliquidity and remliquidity on {{pair_token}}. No authorization is required.


<h1 class="contract">fundyield</h1>

---
spec_version: "0.2.0"
title: Fund yield
summary: 'Announce {{nowrap quantity}} of shadow for the reservoir of {{nowrap pair_token}}'
---

{{from}} announces the transfer of exactly {{quantity}}, in the shadow symbol of the yield pool {{pair_token}}, to this contract. The transfer of {{quantity}} from {{from}} that follows, in this transaction or a later one, is added to the reservoir of {{pair_token}}, the shadow queued to be sold through the pool, and not to {{from}}'s extended balance. While the announcement is pending, any other transfer from {{from}} to this contract is refused. A new announcement by {{from}} replaces the pending one. When the delivery fills a reservoir that was empty, the pair's tick clock restarts.

RAM for the record of the announcement will be deducted from {{from}}'s resources, and returned by cancelyield or by the delivery.

Authorization of {{from}} is required.


<h1 class="contract">cancelyield</h1>

---
spec_version: "0.2.0"
title: Cancel yield funding
summary: 'Drop the pending funding announcement of {{nowrap from}}'
---

{{from}} agrees to withdraw their pending fundyield announcement. The record is erased and its RAM returned to {{from}}, and transfers from {{from}} to this contract are treated as ordinary deposits again. The action fails if {{from}} has no pending announcement.

Authorization of {{from}} is required.


<h1 class="contract">tickyield</h1>

---
spec_version: "0.2.0"
title: Tick yield
summary: 'Sell one clip of the reservoir of {{nowrap pair_token}} through the pool'
---

The token {{pair_token}} must be a yield pool whose tick parameters have been set. The yield owed to the pool is settled first, as in accrueyield. Then a clip of the reservoir is exchanged through the pool for the other leg, under the same conversion rules and fee as the exchange action with no minimum: the clip is the reservoir multiplied by the time elapsed since the pair's tick clock last advanced and divided by the conversion horizon, rounded downward, but at most the depth cap and at most the reservoir. The depth cap is the pair's basis points of the pool's shadow side as it stands now or as it stood at the last setyield or selling tick, whichever of the two is smaller, so that a shadow side enlarged since that tick cannot widen it. The clock then advances to now. The other-leg proceeds are handed to the shadow token's addyield action, which distributes them to every holder of the shadow; this contract, holding the pool's shadow, receives its share on a later accrual.

The exchange happens only if two conditions hold. The clip must have reached the pair's clip floor, or the whole of the reservoir if that is smaller. And, unless the whole reservoir is what is being sold, what the clip would fetch must be enough that the pair's own fee rate is the fee it pays rather than the one-unit minimum. When either fails, and when the reservoir is empty or no time has elapsed, the pools, the reservoir and the clock are all left unmodified; in particular the clock is not advanced, so the elapsed time counts toward the next clip instead of being discarded. No authorization is required.


<h1 class="contract">changefee</h1>

---
spec_version: "0.2.0"
title: Change fee
summary: 'Change the fee value associated to a pair'
---

The fee authority associated to the token {{pair_token}} authorizes
to change the fee parameter associated to the same token, to the value {{newfee}}, in units of 1/10000 and at most 9999.


<h1 class="contract">setconfig</h1>

---
spec_version: "0.2.0"
title: Set configuration
summary: 'Set the fee authority to {{nowrap fee_authority}} and the system token to {{nowrap system_token}}'
---

The contract sets {{fee_authority}} as the account whose authorization the changefee action requires for every pair created afterwards without a fee authority of its own, and {{system_token}} as the system token: the second leg of every pair, and the one token accepted in a transfer without a pair of its own. Pairs already created keep the authority they were created with. Until this action has run, no transfer is accepted and no pair can be created.

The authorization of the contract is required.


<h1 class="contract">close</h1>

---
spec_version: "0.2.0"
title: Close Token Balance
summary: 'Close {{nowrap owner}}’s zero quantity balance'
---

{{owner}} agree to close their zero quantity balance for the {{symbol_to_symbol_code symbol}} token.

RAM will be refunded to the RAM payer of the {{symbol_to_symbol_code symbol}} token balance for {{owner}}.


<h1 class="contract">open</h1>

---
spec_version: "0.2.0"
title: Open Token Balance
summary: 'Open a zero quantity balance for {{nowrap owner}}'
---

{{ram_payer}} agrees to establish a zero quantity balance for {{owner}} for the {{symbol_to_symbol_code symbol}} token.

If {{owner}} does not have a balance for {{symbol_to_symbol_code symbol}}, {{ram_payer}} will be designated as the RAM payer of the {{symbol_to_symbol_code symbol}} token balance for {{owner}}. As a result, RAM will be deducted from {{ram_payer}}’s resources to create the necessary records.


<h1 class="contract">transfer</h1>

---
spec_version: "0.2.0"
title: Transfer Tokens
summary: 'Send {{nowrap quantity}} from {{nowrap from}} to {{nowrap to}}'
---

(1) {{from}} sends the asset {{quantity}} to {{to}}. {{to}} receives exactly {{quantity}}.The authorization of {{from}} is required.

(2) {{#if memo}} There is a memo attached to the transfer stating: {{memo}}
{{/if}}

(3) If {{from}} is not already the RAM payer of their {{asset_to_symbol_code quantity}} token balance, {{from}} will be designated as such. As a result, RAM will be deducted from {{from}}’s resources to refund the original RAM payer.

(4) If {{to}} does not have a balance for {{asset_to_symbol_code quantity}}, {{from}} will be designated as the RAM payer of the {{asset_to_symbol_code quantity}} token balance for {{to}}. As a result, RAM will be deducted from {{from}}’s resources to create the necessary records.

(5) The account {{from}} is notified.
(6) The account {{to}} is notified.