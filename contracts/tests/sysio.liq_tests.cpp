#include "sysio_liq_tester.hpp"

BOOST_AUTO_TEST_SUITE(sysio_liq_tests)

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(custody_settlement_preserves_supply_yield_and_ignores_callbacks, sysio_liq_tester) try {
   BOOST_REQUIRE_EQUAL(success(), mint(SYND_ACCOUNT, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), mint("bob"_n, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 10 * UNIT));
   const auto custody_owed = owed(SYND_ACCOUNT);
   const auto bob_owed = owed("bob"_n);
   const auto before_supply = supply();
   auto settle = [&](name signer, name custodian, name beneficiary, asset quantity) {
      return push_liq(signer, "settle"_n, mvo()("custodian", custodian)("beneficiary", beneficiary)("quantity", quantity));
   };
   BOOST_REQUIRE(mentions(settle("alice"_n, "alice"_n, "bob"_n, asset(UNIT, LIQSOL_SYM)), "unsupported custodian"));
   BOOST_REQUIRE(mentions(settle("alice"_n, SYND_ACCOUNT, "bob"_n, asset(UNIT, LIQSOL_SYM)), "missing authority"));
   BOOST_REQUIRE(mentions(settle(SYND_ACCOUNT, SYND_ACCOUNT, "bob"_n, asset(0, LIQSOL_SYM)), "positive"));
   BOOST_REQUIRE(mentions(settle(SYND_ACCOUNT, SYND_ACCOUNT, "bob"_n, asset(101 * UNIT, LIQSOL_SYM)), "overdrawn"));
   BOOST_REQUIRE(mentions(settle(SYND_ACCOUNT, SYND_ACCOUNT, "bob"_n, asset(UNIT, symbol::from_string("4,LIQSOL"))), "precision"));
   BOOST_REQUIRE_EQUAL(success(), settle(SYND_ACCOUNT, SYND_ACCOUNT, SYND_ACCOUNT, asset(100 * UNIT, LIQSOL_SYM)));
   BOOST_REQUIRE_EQUAL(100 * UNIT, shadow_balance(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(custody_owed, owed(SYND_ACCOUNT));
   BOOST_REQUIRE(mentions(transfer_shadow(SYND_ACCOUNT, SYND_ACCOUNT, UNIT), "cannot transfer to self"));
   set_code("bob"_n, contracts::util::block_transfer_wasm());
   produce_blocks();
   BOOST_REQUIRE(transfer_shadow(SYND_ACCOUNT, "bob"_n, UNIT) != success());
   BOOST_REQUIRE_EQUAL(success(), settle(SYND_ACCOUNT, SYND_ACCOUNT, "bob"_n, asset(40 * UNIT, LIQSOL_SYM)));
   BOOST_REQUIRE_EQUAL(60 * UNIT, shadow_balance(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(140 * UNIT, shadow_balance("bob"_n));
   BOOST_REQUIRE_EQUAL(before_supply, supply());
   BOOST_REQUIRE_EQUAL(custody_owed, owed(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(bob_owed, owed("bob"_n));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(create_binds_an_active_liq_token, sysio_liq_tester) try {
   const auto st = stat_row();
   BOOST_REQUIRE_EQUAL(0, st["supply"].as<asset>().get_amount());
   BOOST_REQUIRE_EQUAL(SOLANA, st["chain_code"].as_string());
   BOOST_REQUIRE_EQUAL(LIQSOL, st["token_code"].as_string());

   BOOST_REQUIRE_EQUAL(wasm_assert_msg("token_code already has a shadow symbol"),
                       create(symbol::from_string("9,LIQSOLB"), SOLANA, LIQSOL));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("token_code is not an active registry token"),
                       create(LIQETH_SYM, ETH, "NOPE"));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("token_code is not a liq token"),
                       create(symbol::from_string("9,USDCSOL"), SOLANA, "USDCSOL"));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("symbol precision must match the token's depot precision"),
                       create(symbol::from_string("4,LIQETH"), ETH, LIQETH));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("token_code is not bound to chain_code"),
                       create(LIQETH_SYM, SOLANA, LIQETH));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("symbol already exists"),
                       create(LIQSOL_SYM, ETH, LIQETH));
   BOOST_REQUIRE(mentions(create(LIQETH_SYM, ETH, LIQETH, "alice"_n), "missing authority of sysio.liq"));
   BOOST_REQUIRE_EQUAL(success(), create(LIQETH_SYM, ETH, LIQETH));
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Inbound: SYNDICATE_LIQ
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(mint_credits_a_holder_and_refuses_what_it_must, sysio_liq_tester) try {
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, 100 * UNIT));
   auto row = account_row("alice"_n);
   BOOST_REQUIRE_EQUAL(100 * UNIT, row["balance"].as<asset>().get_amount());
   BOOST_REQUIRE_EQUAL(fc::uint128_t{ 0 }, row["index_checkpoint"].as_uint128());
   BOOST_REQUIRE_EQUAL(0u, row["owed_wire"].as_uint64());
   BOOST_REQUIRE_EQUAL(100 * UNIT, supply());

   // Every refusal aborts and changes nothing: an unknown token, a zero amount, an account that
   // does not exist, and any signer but sysio.synd.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("token_code has no shadow symbol"), mint("alice"_n, 5 * UNIT, "NOPE"));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("amount must be positive"), mint("alice"_n, 0));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("to account does not exist"), mint("nobody"_n, 5 * UNIT));
   BOOST_REQUIRE(mentions(mint("alice"_n, UNIT, LIQSOL, MSGCH_ACCOUNT), "missing authority of sysio.synd"));
   BOOST_REQUIRE_EQUAL(100 * UNIT, shadow_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(100 * UNIT, supply());
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// The token: settle before mutate
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(settle_before_mutate_conserves_every_subunit, sysio_liq_tester) try {
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), mint("bob"_n, 300 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(25 * UNIT, owed("alice"_n));
   BOOST_REQUIRE_EQUAL(75 * UNIT, owed("bob"_n));

   // A transfer mid-period banks each side's accrual and restamps both rows.
   BOOST_REQUIRE_EQUAL(success(), transfer_shadow("alice"_n, "bob"_n, 50 * UNIT));
   auto alice = account_row("alice"_n);
   auto bob   = account_row("bob"_n);
   BOOST_REQUIRE_EQUAL(uint64_t(25 * UNIT), alice["owed_wire"].as_uint64());
   BOOST_REQUIRE_EQUAL(uint64_t(75 * UNIT), bob["owed_wire"].as_uint64());
   BOOST_REQUIRE_EQUAL(index(), alice["index_checkpoint"].as_uint128());
   BOOST_REQUIRE_EQUAL(index(), bob["index_checkpoint"].as_uint128());

   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(25 * UNIT + 12'500'000'000, owed("alice"_n));   // 50 of 400
   BOOST_REQUIRE_EQUAL(75 * UNIT + 87'500'000'000, owed("bob"_n));     // 350 of 400
   BOOST_REQUIRE_EQUAL(owed("alice"_n) + owed("bob"_n), 200 * UNIT);

   // Both claims pay what the spec says and empty the pot.
   const int64_t alice_before = wire_balance("alice"_n), bob_before = wire_balance("bob"_n);
   const int64_t alice_owed = owed("alice"_n), bob_owed = owed("bob"_n);
   BOOST_REQUIRE_EQUAL(success(), claim("alice"_n));
   BOOST_REQUIRE_EQUAL(success(), claim("bob"_n));
   BOOST_REQUIRE_EQUAL(alice_before + alice_owed, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(bob_before + bob_owed, wire_balance("bob"_n));
   BOOST_REQUIRE_EQUAL(0u, pot());
   BOOST_REQUIRE_EQUAL(0, owed("alice"_n));

   // A row opened after the index moved is stamped, not zeroed: nothing is owed,
   // claiming sends nothing, and a later credit accrues only from here.
   BOOST_REQUIRE_EQUAL(success(), open("dave"_n, LIQSOL_SYM, "dave"_n));
   BOOST_REQUIRE_EQUAL(index(), account_row("dave"_n)["index_checkpoint"].as_uint128());
   const int64_t dave_before = wire_balance("dave"_n);
   BOOST_REQUIRE_EQUAL(success(), claim("dave"_n));
   BOOST_REQUIRE_EQUAL(dave_before, wire_balance("dave"_n));
   BOOST_REQUIRE_EQUAL(success(), mint("dave"_n, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(0, owed("dave"_n));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 50 * UNIT));   // 500 supply: dave holds a fifth
   BOOST_REQUIRE_EQUAL(10 * UNIT, owed("dave"_n));

   BOOST_REQUIRE_EQUAL(wasm_assert_msg("overdrawn balance"), transfer_shadow("dave"_n, "alice"_n, 101 * UNIT));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("no balance object found"), transfer_shadow("carol"_n, "alice"_n, UNIT));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("yield must be in WIRE"),
                       push_liq("carol"_n, "addyield"_n, mvo()("from", "carol"_n)("quantity", asset(UNIT, LIQSOL_SYM))
                                                             ("target", LIQSOL_SYM.to_symbol_code())));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(a_one_unit_distribution_carries_its_remainder, sysio_liq_tester) try {
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, 3));   // three subunits
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 1));
   BOOST_REQUIRE_EQUAL(1u, index_row()["carry"].as_uint64());
   BOOST_REQUIRE_EQUAL(0, owed("alice"_n));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 1));
   BOOST_REQUIRE_EQUAL(2u, index_row()["carry"].as_uint64());
   BOOST_REQUIRE_EQUAL(1, owed("alice"_n));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 1));
   BOOST_REQUIRE_EQUAL(0u, index_row()["carry"].as_uint64());
   BOOST_REQUIRE_EQUAL(fc::uint128_t{ yield_reference::Scale }, index());
   BOOST_REQUIRE_EQUAL(3, owed("alice"_n));
   BOOST_REQUIRE_EQUAL(3u, pot());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(claim_with_nothing_owed_sends_nothing, sysio_liq_tester) try {
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, 100 * UNIT));
   const int64_t before = wire_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(0, owed("alice"_n));
   BOOST_REQUIRE_EQUAL(success(), claim("alice"_n));
   BOOST_REQUIRE_EQUAL(before, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(0u, account_row("alice"_n)["owed_wire"].as_uint64());
   BOOST_REQUIRE_EQUAL(100 * UNIT, shadow_balance("alice"_n));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(close_refuses_while_yield_is_owed, sysio_liq_tester) try {
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("Cannot close because the balance is not zero."), close("alice"_n, LIQSOL_SYM));
   BOOST_REQUIRE_EQUAL(success(), transfer_shadow("alice"_n, "bob"_n, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(0, shadow_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(10 * UNIT, owed("alice"_n));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("Cannot close because yield is still owed; claim first."), close("alice"_n, LIQSOL_SYM));
   BOOST_REQUIRE_EQUAL(success(), claim("alice"_n));
   BOOST_REQUIRE_EQUAL(success(), close("alice"_n, LIQSOL_SYM));
   BOOST_REQUIRE(account_row("alice"_n).is_null());
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("no balance object found"), claim("alice"_n));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(the_index_grows_past_64_bits, sysio_liq_tester) try {
   // One subunit of supply and 2e7 subunits of yield move the index by 2e19.
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, 1));
   const int64_t donation = 20'000'000;
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, donation));
   BOOST_REQUIRE_LT(yield_reference::wide(std::numeric_limits<uint64_t>::max()), yield_reference::wide(index()));
   BOOST_REQUIRE_EQUAL(donation, owed("alice"_n));
   const int64_t before = wire_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), claim("alice"_n));
   BOOST_REQUIRE_EQUAL(before + donation, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(index(), account_row("alice"_n)["index_checkpoint"].as_uint128());
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, donation));
   BOOST_REQUIRE_EQUAL(donation, owed("alice"_n));
} FC_LOG_AND_RETHROW()

/// Swap proceeds and donations distribute exactly their funding without drawing from T5.
BOOST_FIXTURE_TEST_CASE(addyield_distributes_only_supplied_wire, sysio_liq_tester) try {
   constexpr int64_t intake = 100 * UNIT;
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), openext("alice"_n, "alice"_n, extended_symbol{WIRE_SYM, TOKEN_ACCOUNT}));
   BOOST_REQUIRE_EQUAL(success(), transfer_wire("alice"_n, SWAP_ACCOUNT, intake));
   const auto treasury_before = wire_balance(SYSIO_ACCOUNT);
   const auto holder_before = wire_balance("alice"_n);
   for (const auto from : {SWAP_ACCOUNT, "carol"_n}) {
      const auto source_before = wire_balance(from);
      const auto trace = base_tester::push_action(LIQ_ACCOUNT, "addyield"_n, from, mvo()
         ("from", from)("quantity", asset(intake, WIRE_SYM))("target", LIQSOL_SYM.to_symbol_code()));
      BOOST_REQUIRE(trace && !trace->except);
      produce_block();
      for (const auto& action : trace->action_traces) {
         BOOST_CHECK(action.act.account != SYSIO_ACCOUNT);
         BOOST_CHECK(action.act.name != "addkicker"_n);
      }
      BOOST_CHECK_EQUAL(source_before - intake, wire_balance(from));
      BOOST_CHECK_EQUAL(treasury_before, wire_balance(SYSIO_ACCOUNT));
   }
   BOOST_REQUIRE_EQUAL(2 * intake, owed("alice"_n));
   BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(2 * intake), pot());
   BOOST_REQUIRE_EQUAL(success(), claim("alice"_n));
   BOOST_CHECK_EQUAL(holder_before + 2 * intake, wire_balance("alice"_n));
   BOOST_CHECK_EQUAL(0, pot());
   BOOST_CHECK_EQUAL(0, owed("alice"_n));
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Yield in: LIQ_YIELD -> pending -> the swap's reservoir -> clips -> holders
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(queued_yield_sells_through_the_pool_and_pays_holders, sysio_liq_tester) try {
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, 100 * UNIT));

   // Nothing pending: queueing is a no-op, and the pool does not exist yet.
   BOOST_REQUIRE_EQUAL(success(), queueyield(LIQSOL_SYM));
   BOOST_REQUIRE_EQUAL(success(), mintyield(SOLANA, LIQSOL, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(10 * UNIT, pending_row()["quantity"].as<asset>().get_amount());
   BOOST_REQUIRE_EQUAL(100 * UNIT, supply());   // pending yield is outside supply
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("no yield pool registered for this shadow"), queueyield(LIQSOL_SYM));

   // The bootstrap seeds the pool: the LCO liq minted to sysio and deposited
   // with the earmark WIRE, the pair created with the shadow as its yield leg.
   const int64_t treasury_before = wire_balance(SYSIO_ACCOUNT);
   BOOST_REQUIRE_EQUAL(success(), regliqpool(SOLANA, LIQSOL, POOL_SYM, 1000 * UNIT, 1000 * UNIT));
   BOOST_REQUIRE_EQUAL("LIQPOOL", stat_row()["pair_symbol"].as_string());
   BOOST_REQUIRE_EQUAL(1100 * UNIT, supply());
   BOOST_REQUIRE_EQUAL(0, shadow_balance(SYSIO_ACCOUNT));
   BOOST_REQUIRE_EQUAL(treasury_before - 1000 * UNIT, wire_balance(SYSIO_ACCOUNT));
   const auto pair = swap_row("stat"_n, "currency_stats", SWAP_ACCOUNT, POOL_SYM.to_symbol_code().value);
   BOOST_REQUIRE_EQUAL(1000 * UNIT, pair["pool1"]["quantity"].as<asset>().get_amount());
   BOOST_REQUIRE_EQUAL(1000 * UNIT, pair["pool2"]["quantity"].as<asset>().get_amount());
   BOOST_REQUIRE_EQUAL(LIQ_ACCOUNT.to_string(), pair["yield_leg"]["contract"].as_string());
   BOOST_REQUIRE_EQUAL(SYSIO_ACCOUNT.to_string(), pair["fee_authority"].as_string());
   BOOST_REQUIRE_EQUAL(0, reservoir());
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("the shadow already has a yield pool"),
                       regliqpool(SOLANA, LIQSOL, symbol::from_string("9,POOLB"), UNIT, UNIT));

   // Queueing mints the pending yield to the contract and hands it straight to
   // the reservoir: the contract keeps no balance and accrues nothing.
   BOOST_REQUIRE_EQUAL(success(), queueyield(LIQSOL_SYM));
   BOOST_REQUIRE(pending_row().is_null());
   BOOST_REQUIRE_EQUAL(1110 * UNIT, supply());
   BOOST_REQUIRE_EQUAL(10 * UNIT, reservoir());
   BOOST_REQUIRE_EQUAL(0, shadow_balance(LIQ_ACCOUNT));
   BOOST_REQUIRE_EQUAL(0, owed(LIQ_ACCOUNT));
   BOOST_REQUIRE(swap_row("yieldfunds"_n, "fund_receipt", SWAP_ACCOUNT, LIQ_ACCOUNT.to_uint64_t()).is_null());
   BOOST_REQUIRE_EQUAL(success(), queueyield(LIQSOL_SYM));   // nothing left: a no-op
   BOOST_REQUIRE_EQUAL(10 * UNIT, reservoir());

   // A later report pends again; one of another chain is dropped.
   BOOST_REQUIRE_EQUAL(success(), mintyield(ETH, LIQSOL, 10 * UNIT));
   BOOST_REQUIRE(pending_row().is_null());
   BOOST_REQUIRE_EQUAL(success(), mintyield(SOLANA, LIQSOL, 2 * UNIT));
   BOOST_REQUIRE_EQUAL(2 * UNIT, pending_row()["quantity"].as<asset>().get_amount());

   // The tick sells a clip of the reservoir through the pool and pays the
   // proceeds in through addyield: the index moves, and alice, a holder, is
   // owed her share of them.
   produce_blocks(40);
   const int64_t alice_before = wire_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), tickyield());
   BOOST_REQUIRE_LT(reservoir(), 10 * UNIT);
   BOOST_REQUIRE_LT(0u, pot());
   BOOST_REQUIRE_LT(fc::uint128_t{ 0 }, index());
   const int64_t alice_owed = owed("alice"_n);
   BOOST_REQUIRE_LT(0, alice_owed);
   BOOST_REQUIRE_EQUAL(success(), claim("alice"_n));
   BOOST_REQUIRE_EQUAL(alice_before + alice_owed, wire_balance("alice"_n));
} FC_LOG_AND_RETHROW()

// Supply plus pending yield never exceeds the asset range: every supply-growing path
// measures its headroom net of what is parked in liqpending, so queueyield can always
// mint it. A report past that headroom is dropped whole, so no value is clipped.
BOOST_FIXTURE_TEST_CASE(pending_yield_is_reserved_against_the_asset_range, sysio_liq_tester) try {
   // The pool first, while the range is open: regliqpool mints the LCO seed. Then one
   // syndication takes supply to 100 base units under the range.
   BOOST_REQUIRE_EQUAL(success(), regliqpool(SOLANA, LIQSOL, POOL_SYM, 1000 * UNIT, 1000 * UNIT));
   const uint64_t range = static_cast<uint64_t>(asset::max_amount);
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, range - 100 - 1000 * UNIT));
   BOOST_REQUIRE_EQUAL(asset::max_amount - 100, supply());

   // 60 fits and parks. A second 60 exceeds the 40 left and is dropped; 40 then fills
   // the range exactly.
   BOOST_REQUIRE_EQUAL(success(), mintyield(SOLANA, LIQSOL, 60));
   BOOST_REQUIRE_EQUAL(60, pending_row()["quantity"].as<asset>().get_amount());
   BOOST_REQUIRE_EQUAL(success(), mintyield(SOLANA, LIQSOL, 60));
   BOOST_REQUIRE_EQUAL(60, pending_row()["quantity"].as<asset>().get_amount());
   BOOST_REQUIRE_EQUAL(success(), mintyield(SOLANA, LIQSOL, 40));
   BOOST_REQUIRE_EQUAL(100, pending_row()["quantity"].as<asset>().get_amount());
   BOOST_REQUIRE_EQUAL(asset::max_amount - 100, supply());   // pending stays outside supply

   // The reservation binds every other supply-growing path: a mint of one base unit and
   // the governance recredit are both refused.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("supply exceeds the asset range"), mint("bob"_n, 1));
   BOOST_REQUIRE_EQUAL(0, shadow_balance("bob"_n));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("supply exceeds the asset range"), recredit("alice"_n, 1));

   // Queueing always fits: the pending yield mints to exactly the range.
   BOOST_REQUIRE_EQUAL(success(), queueyield(LIQSOL_SYM));
   BOOST_REQUIRE(pending_row().is_null());
   BOOST_REQUIRE_EQUAL(asset::max_amount, supply());
   BOOST_REQUIRE_EQUAL(100, reservoir());
   // Nothing can grow the supply now, and a report says why before it is dropped.
   BOOST_REQUIRE_EQUAL(success(), mintyield(SOLANA, LIQSOL, 1));
   BOOST_REQUIRE(pending_row().is_null());
} FC_LOG_AND_RETHROW()

// The other exit: a holder sells shadow into the yield pool for WIRE on the depot, no
// outpost round trip. The pool is an ordinary pair, so alice opens her two deposit rows,
// deposits shadow by transfer, exchanges it for WIRE and withdraws the WIRE to her wallet.
// Both legs conserve exactly: the pool gains what she sold and she receives what the pool
// lost; the shadow supply is untouched, since it moved and nothing burned; the fee and the
// price impact keep her proceeds below par.
BOOST_FIXTURE_TEST_CASE(a_holder_can_sell_shadow_into_the_yield_pool_for_wire, sysio_liq_tester) try {
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), regliqpool(SOLANA, LIQSOL, POOL_SYM, 1000 * UNIT, 1000 * UNIT));

   const extended_symbol shadow_ext{ LIQSOL_SYM, LIQ_ACCOUNT };
   const extended_symbol wire_ext{ WIRE_SYM, TOKEN_ACCOUNT };
   BOOST_REQUIRE_EQUAL(success(), openext("alice"_n, "alice"_n, shadow_ext));
   BOOST_REQUIRE_EQUAL(success(), openext("alice"_n, "alice"_n, wire_ext));

   constexpr int64_t sold = 50 * UNIT;
   BOOST_REQUIRE_EQUAL(success(), transfer_shadow("alice"_n, SWAP_ACCOUNT, sold));
   BOOST_REQUIRE_EQUAL(50 * UNIT, shadow_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(1050 * UNIT, shadow_balance(SWAP_ACCOUNT));   // the pool's 1000 plus her deposit

   const auto    before             = swap_pool_row(POOL_SYM);
   const int64_t pool_shadow_before = before["pool1"]["quantity"].as<asset>().get_amount();
   const int64_t pool_wire_before   = before["pool2"]["quantity"].as<asset>().get_amount();
   BOOST_REQUIRE_EQUAL(success(), exchange("alice"_n, POOL_SYM, extended_asset{ asset(sold, LIQSOL_SYM), LIQ_ACCOUNT },
                                           asset(1, WIRE_SYM)));
   const auto    after    = swap_pool_row(POOL_SYM);
   const int64_t received = pool_wire_before - after["pool2"]["quantity"].as<asset>().get_amount();
   BOOST_REQUIRE_EQUAL(pool_shadow_before + sold, after["pool1"]["quantity"].as<asset>().get_amount());
   BOOST_REQUIRE_LT(0, received);
   BOOST_REQUIRE_LT(received, sold);
   BOOST_REQUIRE_EQUAL(1100 * UNIT, supply());

   const int64_t wallet_before = wire_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), withdraw("alice"_n, "alice"_n, extended_asset{ asset(received, WIRE_SYM), TOKEN_ACCOUNT }));
   BOOST_REQUIRE_EQUAL(wallet_before + received, wire_balance("alice"_n));
   // The deposit held exactly the proceeds: nothing is left to withdraw.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("insufficient funds"),
                       withdraw("alice"_n, "alice"_n, extended_asset{ asset(1, WIRE_SYM), TOKEN_ACCOUNT }));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(regliqpool_refusals, sysio_liq_tester) try {
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("token_code has no shadow symbol"), regliqpool(ETH, LIQETH, POOL_SYM, UNIT, UNIT));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("token_code belongs to another chain"), regliqpool(ETH, LIQSOL, POOL_SYM, UNIT, UNIT));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("both seeds must be positive"), regliqpool(SOLANA, LIQSOL, POOL_SYM, 0, UNIT));
   BOOST_REQUIRE(mentions(regliqpool(SOLANA, LIQSOL, POOL_SYM, UNIT, UNIT, 30, 0, 86400, 300, 1000, "alice"_n),
                          "missing authority of sysio.liq"));
   BOOST_REQUIRE_EQUAL("", stat_row()["pair_symbol"].as_string());
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Governance reconciliation
// ---------------------------------------------------------------------------

/// Exceptional supply repair preserves existing yield and requires the LIQ contract authority.
BOOST_FIXTURE_TEST_CASE(privileged_supply_repair_preserves_existing_yield, sysio_liq_tester) try {
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, 60 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 6 * UNIT));

   BOOST_REQUIRE(mentions(recredit("alice"_n, 10 * UNIT, "alice"_n), "missing authority of sysio.liq"));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("quantity must be positive"), recredit("alice"_n, 0));
   BOOST_REQUIRE_EQUAL(success(), recredit("alice"_n, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(70 * UNIT, shadow_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(70 * UNIT, supply());
   // The row settled before it grew: the yield earned on the 60 stays, nothing is credited for the 10.
   BOOST_REQUIRE_EQUAL(6 * UNIT, owed("alice"_n));
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// The emergency stop (sysio.andon)
// ---------------------------------------------------------------------------

// Review focus 2, sysio.liq: while the cord is pulled a transfer INTO a custody contract succeeds and one to
// anyone else is refused; a holder's `claim` and `queueyield` are refused, a custody contract's own claim
// runs, and `mint` runs. With no sysio.andon deployed (every other case here) the cord reads clear. After
// the clear everything refused succeeds.
BOOST_FIXTURE_TEST_CASE(a_freeze_admits_transfers_into_custody_and_refuses_the_rest, sysio_liq_tester) try {
   namespace andon = sysio_system::test_support::andon;
   const auto frozen = wasm_assert_msg(andon::frozen_message);
   create_accounts({ "sysio.bond"_n, "sysio.opreg"_n });
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), mintyield(SOLANA, LIQSOL, UNIT));

   abi_serializer andon_abi_ser;
   andon::deploy(*this, andon_abi_ser, contracts::andon_wasm(), contracts::andon_abi());
   BOOST_REQUIRE_EQUAL(success(), andon::pull(*this, andon_abi_ser));

   // In: into sysio.bond, sysio.synd and sysio.opreg.
   BOOST_REQUIRE_EQUAL(success(), transfer_shadow("alice"_n, "sysio.bond"_n, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), transfer_shadow("alice"_n, SYND_ACCOUNT, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), transfer_shadow("alice"_n, "sysio.opreg"_n, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(70 * UNIT, shadow_balance("alice"_n));
   // Out: to a holder, and back out of a custody contract to a holder.
   BOOST_REQUIRE_EQUAL(frozen, transfer_shadow("alice"_n, "bob"_n, UNIT));
   BOOST_REQUIRE_EQUAL(frozen, transfer_shadow("sysio.bond"_n, "bob"_n, UNIT));
   BOOST_REQUIRE_EQUAL(frozen, transfer_shadow("alice"_n, SWAP_ACCOUNT, UNIT));
   BOOST_REQUIRE_EQUAL(0, shadow_balance("bob"_n));
   for (auto beneficiary : {"bob"_n, SYND_ACCOUNT, "sysio.bond"_n}) {
      BOOST_REQUIRE_EQUAL(frozen, push_liq(SYND_ACCOUNT, "settle"_n,
         mvo()("custodian", SYND_ACCOUNT)("beneficiary", beneficiary)("quantity", asset(UNIT, LIQSOL_SYM))));
   }
   // A holder's claim and queueyield are refused; a custody contract claims its own row.
   const int64_t alice_owed = owed("alice"_n);
   BOOST_REQUIRE_LT(0, alice_owed);
   BOOST_REQUIRE_EQUAL(frozen, claim("alice"_n));
   BOOST_REQUIRE_EQUAL(frozen, queueyield(LIQSOL_SYM));
   BOOST_REQUIRE_EQUAL(success(), claim("sysio.bond"_n));
   // Mint runs.
   BOOST_REQUIRE_EQUAL(success(), mint("bob"_n, UNIT));
   BOOST_REQUIRE_EQUAL(UNIT, shadow_balance("bob"_n));

   BOOST_REQUIRE_EQUAL(success(), andon::clear(*this, andon_abi_ser));
   BOOST_REQUIRE_EQUAL(success(), transfer_shadow("alice"_n, "bob"_n, UNIT));
   BOOST_REQUIRE_EQUAL(2 * UNIT, shadow_balance("bob"_n));
   const int64_t alice_wire = wire_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), claim("alice"_n));
   BOOST_REQUIRE_EQUAL(alice_wire + alice_owed, wire_balance("alice"_n));
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
