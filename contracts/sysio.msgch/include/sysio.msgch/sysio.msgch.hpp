#pragma once

#include <sysio/sysio.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio/slug_name.hpp>
#include <sysio.opp.common/opp_table_types.hpp>
#include <sysio.opp.common/opp_keys.hpp>
#include <magic_enum/magic_enum.hpp>

namespace sysio {


   class [[sysio::contract("sysio.msgch")]] msgch : public contract {
   public:
      using contract::contract;

      // -----------------------------------------------------------------------
      //  Envelope sizing
      // -----------------------------------------------------------------------

      /// Hard cap on an encoded envelope in both directions, mirroring the Solana
      /// (`opp_outpost::MAX_ENVELOPE_BYTES`) and Ethereum (`OPP.MAX_ENVELOPE_BYTES`) caps. Ethereum is
      /// the binding constraint: a cold `emitOutboundEnvelope` of a near-64-KiB envelope exceeds the
      /// EIP-7825 per-transaction gas cap. Inbound, `deliver` rejects anything larger.
      static constexpr size_t MAX_ENVELOPE_BYTES = 32'768;

      /// Per-attestation framing `buildenv` budgets for (tags, length prefixes, type and data-size
      /// fields). Over-counts the real encoding by a few bytes.
      static constexpr size_t ATTESTATION_OVERHEAD_BYTES = 24;

      /// Envelope and message header `buildenv` budgets for, with margin for length prefixes.
      /// Over-counts the real encoding.
      static constexpr size_t ENVELOPE_BASELINE_BYTES = 512;

      /// Largest attestation `data` an otherwise empty envelope can carry. `queueout` rejects anything
      /// larger, so every READY row can ship.
      static constexpr size_t MAX_ATTESTATION_DATA_BYTES =
         MAX_ENVELOPE_BYTES - ENVELOPE_BASELINE_BYTES - ATTESTATION_OVERHEAD_BYTES;

      /// Most attestations one envelope carries, so the rows `buildenv` reads and erases per outpost
      /// per epoch stay fixed whatever their sizes. Neither outpost needs it: both resume dispatch
      /// across calls.
      static constexpr size_t MAX_ENVELOPE_ATTESTATIONS = 64;

      // -----------------------------------------------------------------------
      //  Outbound queue
      // -----------------------------------------------------------------------

      /// Most general-lane attestations an outpost's READY queue holds. `queueout` refuses another
      /// until `buildenv` ships some, so the producer's transaction fails instead of the queue growing
      /// without bound. Schedule-lane rows are exempt: `sysio.epoch` queues them inside `advance`.
      static constexpr uint32_t MAX_QUEUED_ATTESTATIONS = 512;

      /// Lane of an outbound attestation. `buildenv` ships the schedule lane first, so a backlog never
      /// delays the rosters an outpost needs to admit the next epoch's deliveries.
      enum class queue_lane : uint8_t {
         schedule = 0, ///< OPERATORS and BATCH_OPERATOR_GROUPS
         general  = 1, ///< every other type, in `queueout` order
      };

      /// The lane `type` queues in.
      static constexpr queue_lane lane_of(opp::types::AttestationType type) {
         return type == opp::types::ATTESTATION_TYPE_OPERATORS ||
                      type == opp::types::ATTESTATION_TYPE_BATCH_OPERATOR_GROUPS
                   ? queue_lane::schedule
                   : queue_lane::general;
      }

      /// Most envelope bytes one epoch's schedule-lane rows take, counted as `buildenv` estimates them
      /// (`data` plus ATTESTATION_OVERHEAD_BYTES each). The largest window's OPERATORS and
      /// BATCH_OPERATOR_GROUPS come to about 6.8 KB, and 15.4 KB even at Solana's 16 per group; 24 KiB also
      /// matches Solana's OPERATORS payload cap.
      static constexpr size_t SCHEDULE_LANE_BYTES = 24 * 1024;

      /// Largest general-lane `data` that fits beside a full schedule lane, which every envelope carries
      /// first. `queueout` rejects anything larger, so no general row can stall the rows behind it.
      static constexpr size_t MAX_GENERAL_ATTESTATION_DATA_BYTES =
         MAX_ENVELOPE_BYTES - ENVELOPE_BASELINE_BYTES - SCHEDULE_LANE_BYTES - ATTESTATION_OVERHEAD_BYTES;

      /// `byqueue` key: outpost, then status, then lane. Rows with equal keys sort by id, so each lane
      /// is FIFO. 128 bits keep the full slug range of `chain_code` (see `opp::outpost_epoch_key`).
      static constexpr uint128_t queue_key(uint64_t chain_code, opp::types::AttestationStatus status,
                                           queue_lane lane) {
         return (static_cast<uint128_t>(chain_code) << 16) |
                (static_cast<uint128_t>(magic_enum::enum_integer(status)) << 8) |
                static_cast<uint128_t>(magic_enum::enum_integer(lane));
      }

      // -----------------------------------------------------------------------
      //  Actions
      // -----------------------------------------------------------------------

      /// Bootstrap: call at epoch 0 to trigger the first advance.
      [[sysio::action]]
      void bootstrap();

      /// Batch operator delivers inbound OPP data for a specific outpost.
      /// Computes sha256 checksum trustlessly, stores in envelopes table,
      /// then calls evalcons inline to check consensus.
      ///
      /// `chain_code` is the originating chain's slug_name value (the underlying
      /// `uint64` of `sysio::slug_name`). The depot looks the row up directly
      /// on `sysio.chains::chains` keyed by `code.value == chain_code`; the
      /// numeric value IS the slug_name, and `sysio.epoch::advance` uses the
      /// same convention when fanning out `queueout` / `buildenv` per outpost.
      [[sysio::action]]
      void deliver(name batch_op_name, uint64_t chain_code, std::vector<char> data);

      /// Evaluate consensus on inbound envelopes for an outpost+epoch.
      /// Called inline from deliver. On consensus: unpacks envelope,
      /// stores messages + attestations, records per-outpost consensus.
      ///
      /// `chain_code` is the originating chain's slug_name value
      /// (see `deliver` for the convention).
      [[sysio::action]]
      void evalcons(uint64_t chain_code, uint32_t epoch_index);

      /// Check if all-outpost consensus is reached AND next_epoch_start
      /// has passed. If yes, reset consensus and call advance.
      /// Called by the batch operator after the time window elapses.
      [[sysio::action]]
      void chkcons();

      /// Dispatch the winning envelope of a resolved OPP dispute. Called inline by
      /// `sysio.chalg::chkdispute` once the Tier-1 vote selects a canonical checksum. Locates the
      /// winning envelope's raw bytes among this (outpost, epoch)'s deliveries and runs the shared
      /// consensus path (store + dispatch attestations, audit log, record the winner on `outpcons`).
      [[sysio::action]]
      void resolvedisp(uint64_t chain_code, uint32_t epoch_index, checksum256 winning_checksum);

      /// Queue an outbound attestation for an outpost.
      /// Writes to the attestations table with status READY. Rejects `data` larger than
      /// MAX_ATTESTATION_DATA_BYTES (MAX_GENERAL_ATTESTATION_DATA_BYTES for a general-lane row), so every
      /// READY row ships, and a general-lane row once the outpost already holds MAX_QUEUED_ATTESTATIONS
      /// of them.
      ///
      /// `chain_code` is the destination outpost's chain slug_name value
      /// (uint64). Called by sibling system contracts that need to send
      /// targeted depot → outpost envelopes:
      ///   * `sysio.epoch::advance` — `OPERATORS`, `BATCH_OPERATOR_GROUPS`
      ///     fanout to every active outpost.
      ///   * `sysio.synd::desyndicate` — `DESYNDICATE_LIQ` releases from
      ///     the destination's syndicated LIQ custody.
      ///
      /// Gated to sysio.epoch, sysio.opreg, sysio.synd, and msgch itself:
      /// each sends under its own
      /// {self, active} authority. The gate is required because a forged
      /// READY attestation rides out inside the next group-signed outbound
      /// envelope, which the outpost authenticates by the group signature —
      /// not per-attestation origin — so an ungated queueout would let any
      /// account inject depot-authorised outbound commands.
      [[sysio::action]]
      void queueout(uint64_t chain_code,
                    opp::types::AttestationType attest_type,
                    std::vector<char> data);

      /// Build outbound envelope from READY attestations for an outpost.
      /// Walks only this outpost's READY rows, schedule lane first, and stops at MAX_ENVELOPE_BYTES or
      /// MAX_ENVELOPE_ATTESTATIONS, so its cost does not grow with the backlog. Packs the rows into an
      /// OPP Envelope, stores it in outenvelopes and erases them.
      /// Stamps the envelope's route endpoints (start = WIRE, end = the
      /// destination's `sysio.chains` `{kind, external_chain_id}`) so the
      /// receiving outpost can reject an envelope a misconfigured relay
      /// delivered to the wrong same-kind chain.
      [[sysio::action]]
      void buildenv(uint64_t chain_code);

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// Auto-incrementing primary key shared by id-keyed msgch tables.
      struct id_key {
         uint64_t id;
         uint64_t primary_key() const { return id; }
         SYSLIB_SERIALIZE(id_key, (id))
      };

      /// Inbound envelope delivery — one row per batch-op per outpost per epoch.
      /// Consensus is evaluated by comparing checksums across operators.
      ///
      /// Working state, not an audit trail (that is `envlog`). `deliver` prunes rows older than the
      /// current and previous epoch (`INBOUND_ENVELOPE_RETENTION_EPOCHS` in `src/sysio.msgch.cpp`);
      /// every on-chain reader only touches the current epoch.
      struct [[sysio::table("envelopes")]] envelope_entry {
         uint64_t                  id;
         uint64_t                  chain_code;
         uint32_t                  epoch_index;
         name                      batch_op_name;
         opp::types::ChainKind     chain_kind;
         checksum256               checksum;        ///< sha256 of the delivered bytes
         /// The delivered bytes. Cleared on the rows present when the epoch's winner is accepted and
         /// never stored for a late confirmation of it; the retention prune bounds any others.
         std::vector<char>         raw_data;
         time_point                received_at{};

         uint128_t by_outpost_epoch() const {
            return opp::outpost_epoch_key(chain_code, epoch_index);
         }

         SYSLIB_SERIALIZE(envelope_entry,
            (id)(chain_code)(epoch_index)(batch_op_name)(chain_kind)
            (checksum)(raw_data)(received_at))
      };

      using envelopes_t = sysio::kv::table<"envelopes"_n, id_key, envelope_entry,
         sysio::kv::index<"byoutepoch"_n,
            sysio::const_mem_fun<envelope_entry, uint128_t, &envelope_entry::by_outpost_epoch>>
      >;

      /// Individual message extracted from a consensus-verified envelope.
      struct [[sysio::table("messages")]] message_entry {
         uint64_t                        id;
         uint64_t                        chain_code;
         uint32_t                        epoch_index;
         checksum256                     message_id;
         checksum256                     previous_message_id;
         opp::types::MessageDirection    direction;
         opp::types::MessageStatus       status;
         std::vector<char>               raw_payload;
         time_point                      received_at{};
         time_point                      processed_at{};

         uint64_t    by_status() const { return static_cast<uint64_t>(status); }
         uint64_t    by_epoch()  const { return epoch_index; }
         checksum256 by_msg_id() const { return message_id; }

         SYSLIB_SERIALIZE(message_entry,
            (id)(chain_code)(epoch_index)(message_id)(previous_message_id)
            (direction)(status)(raw_payload)(received_at)(processed_at))
      };

      using messages_t = sysio::kv::table<"messages"_n, id_key, message_entry,
         sysio::kv::index<"bystatus"_n,
            sysio::const_mem_fun<message_entry, uint64_t, &message_entry::by_status>>,
         sysio::kv::index<"byepoch"_n,
            sysio::const_mem_fun<message_entry, uint64_t, &message_entry::by_epoch>>,
         sysio::kv::index<"bymsgid"_n,
            sysio::const_mem_fun<message_entry, checksum256, &message_entry::by_msg_id>>
      >;

      /// Attestation extracted from a consensus-verified message, or
      /// queued outbound attestation awaiting envelope packing.
      struct [[sysio::table("attestations")]] attestation_entry {
         uint64_t                        id;
         uint64_t                        chain_code;
         uint32_t                        epoch_index;
         opp::types::AttestationType     type;
         opp::types::AttestationStatus   status;
         std::vector<char>               data;
         uint64_t                        pending_timestamp;
         uint64_t                        ready_timestamp;
         uint64_t                        processed_timestamp;

         uint128_t by_queue() const { return queue_key(chain_code, status, lane_of(type)); }

         SYSLIB_SERIALIZE(attestation_entry,
            (id)(chain_code)(epoch_index)(type)(status)(data)
            (pending_timestamp)(ready_timestamp)(processed_timestamp))
      };

      using attestations_t = sysio::kv::table<"attestations"_n, id_key, attestation_entry,
         sysio::kv::index<"byqueue"_n,
            sysio::const_mem_fun<attestation_entry, uint128_t, &attestation_entry::by_queue>>
      >;

      /// Outbound envelope table. One-deep per outpost: `buildenv` erases every older row for the
      /// destination `chain_code` after inserting the new emit, so the surviving row doubles as the
      /// per-outpost chain tip.
      ///
      /// `envelope_hash` is the canonical epoch digest: keccak256 over the canonical
      /// field-complete encoding with the in-envelope `envelope_hash` field blanked (equal to
      /// keccak256(`raw_envelope`), which is emitted in exactly that canonical form; see
      /// opp_canonical_codec.hpp). It matches the digest the receiving outpost computes
      /// (`OPPCommon.epochHash`) and is what the next emit for this outpost carries in its
      /// `previous_envelope_hash`.
      struct [[sysio::table("outenvelopes")]] outbound_envelope {
         uint64_t    id;
         uint64_t    chain_code;
         uint32_t    epoch_index;
         checksum256 envelope_hash;
         opp::types::EnvelopeStatus status;
         std::vector<char> raw_envelope;
         /// The `message_id` of the last (today: only) message in this envelope — the message
         /// stream tip for this outpost. The next `buildenv` for the outpost carries it in its
         /// header's `previous_message_id` and continues the big-endian sequence number embedded
         /// in its first 8 bytes (see opp_canonical_codec.hpp `derive_message_id`).
         checksum256 last_message_id;

         uint64_t by_outpost() const { return chain_code; }
         uint128_t by_outpost_epoch() const {
            return opp::outpost_epoch_key(chain_code, epoch_index);
         }

         SYSLIB_SERIALIZE(outbound_envelope,
            (id)(chain_code)(epoch_index)(envelope_hash)(status)(raw_envelope)(last_message_id))
      };

      using outenvelopes_t = sysio::kv::table<"outenvelopes"_n, id_key, outbound_envelope,
         sysio::kv::index<"byoutpost"_n,
            sysio::const_mem_fun<outbound_envelope, uint64_t, &outbound_envelope::by_outpost>>,
         sysio::kv::index<"byoutepoch"_n,
            sysio::const_mem_fun<outbound_envelope, uint128_t, &outbound_envelope::by_outpost_epoch>>
      >;

      /// Per-outpost consensus primary key.
      struct outpost_consensus_key {
         uint64_t chain_code;
         SYSLIB_SERIALIZE(outpost_consensus_key, (chain_code))
      };

      /// Per-outpost consensus tracking for the current epoch.
      /// One row per outpost. Rows reused (not erased) to avoid RAM churn.
      ///
      /// `winning_checksum` is the winning delivery checksum (sha256 of the raw delivered bytes)
      /// for `epoch_index`, recorded when consensus is reached (majority/unanimous path) or when
      /// a Tier-1 dispute vote resolves (via `resolvedisp`). `sysio.epoch::advance` reads it to
      /// classify each operator's delivery: a matching checksum is a hit, a non-matching delivered
      /// checksum is slashed. Zero until a winner exists for the current epoch.
      ///
      /// `envelope_digest` is the inbound ENVELOPE chain tip: the canonical epoch digest (keccak256
      /// over the canonical field-complete encoding with `envelope_hash` blanked; see
      /// opp_canonical_codec.hpp) of the last ACCEPTED envelope from this outpost. The next
      /// accepted envelope's `previous_envelope_hash` must continue from it. Zero until the first
      /// envelope from this outpost is accepted. Unlike `epoch_index`/`winning_checksum` it is a
      /// running tip, not per-epoch state.
      ///
      /// `message_tip` is the inbound MESSAGE chain tip: the `message_id` of the last ACCEPTED
      /// message from this outpost. The next accepted message's `previous_message_id` must equal
      /// it, which — with the per-message sequence splice validated in `semantic_headers_ok` —
      /// makes the message stream strictly monotonic and non-replayable. The envelope chain orders
      /// envelopes but does not by itself bind the messages inside them, so without this a
      /// correctly envelope-chained successor could replay an earlier valid `Message` verbatim and
      /// re-dispatch its attestations. Zero until the first message from this outpost is accepted
      /// (which may lag `envelope_digest` if the first accepted envelopes are empty-message acks).
      struct [[sysio::table("outpcons")]] outpost_consensus_entry {
         uint64_t    chain_code;
         uint32_t    epoch_index;
         bool        consensus_reached;
         checksum256 winning_checksum;
         checksum256 envelope_digest;
         checksum256 message_tip;

         SYSLIB_SERIALIZE(outpost_consensus_entry,
            (chain_code)(epoch_index)(consensus_reached)(winning_checksum)(envelope_digest)
            (message_tip))
      };

      using outpost_consensus_t =
         sysio::kv::table<"outpcons"_n, outpost_consensus_key, outpost_consensus_entry>;

      /// Per-outpost outbound queue primary key.
      struct outbound_queue_key {
         uint64_t chain_code;
         SYSLIB_SERIALIZE(outbound_queue_key, (chain_code))
      };

      /// The general-lane READY attestations queued for an outpost. `queueout` counts them in and
      /// refuses one past MAX_QUEUED_ATTESTATIONS; `buildenv` counts the shipped ones out.
      struct [[sysio::table("outqueue")]] outbound_queue_entry {
         uint64_t chain_code;
         uint32_t queued;

         SYSLIB_SERIALIZE(outbound_queue_entry, (chain_code)(queued))
      };

      using outqueue_t = sysio::kv::table<"outqueue"_n, outbound_queue_key, outbound_queue_entry>;

      /// Singleton holding the monotonic next-attestation-id counter.
      ///
      /// Why a singleton instead of `atts.available_primary_key()`:
      /// `buildenv` erases the rows it ships for the destination
      /// `chain_code`, and that outpost's inbound `deliver()`-inserted rows,
      /// which carry `status = PROCESSED` (they go straight to dispatch).
      /// Once the atts table is empty, `available_primary_key()` resets to
      /// 0, our `std::max(1, ...)` floor bumps it to 1, reusing a prior
      /// attestation's audit identity. Monotonic ids also keep each
      /// `byqueue` lane FIFO.
      ///
      /// `att_seq` is a one-row table holding `next` — the next id to
      /// mint. `mint_att_id()` reads + bumps it atomically. Cleanup
      /// passes through.
      struct att_seq_key {
         uint64_t id;
         SYSLIB_SERIALIZE(att_seq_key, (id))
      };

      struct [[sysio::table("attseq")]] att_seq_entry {
         uint64_t id;     // always 0 (singleton row)
         uint64_t next;   // next attestation_id to mint

         SYSLIB_SERIALIZE(att_seq_entry, (id)(next))
      };

      using att_seq_t = sysio::kv::table<"attseq"_n, att_seq_key, att_seq_entry>;

      /// Audit-trail row for the durable envelope log. Pure metadata —
      /// `endpoints` (start/end ChainId pair from the inbound or outbound
      /// envelope), the `epoch_index` it corresponds to, the `checksum`
      /// (the envelope's canonical epoch digest, keccak256 per
      /// opp_canonical_codec.hpp, for outbound emits and accepted inbound
      /// envelopes alike), and the `emitted_at` timestamp. Raw payload is
      /// consumed inline by the consensus + dispatch path and never stored.
      /// Off-chain audit reconstruction is out of scope.
      ///
      /// Total row count is capped at
      /// `active_outposts * 2 * epoch_retention_envelope_log_count`
      /// (one inbound + one outbound row per active outpost per epoch).
      /// Eviction is head-first on overflow — see
      /// `write_envelope_log` in `src/sysio.msgch.cpp`.
      struct [[sysio::table("envlog")]] envelope_log_entry {
         uint64_t        id;                ///< monotonic auto-increment PK
         opp::Endpoints  endpoints;         ///< start + end ChainId
         uint32_t        epoch_index;
         checksum256     checksum;          ///< sha256/keccak of envelope bytes
         time_point      emitted_at;

         uint64_t primary_key() const { return id; }

         SYSLIB_SERIALIZE(envelope_log_entry,
            (id)(endpoints)(epoch_index)(checksum)(emitted_at))
      };

      using envelope_log_t = sysio::kv::table<"envlog"_n, id_key, envelope_log_entry>;

   private:

      using ChainKind          = opp::types::ChainKind;
      using MessageDirection    = opp::types::MessageDirection;
      using MessageStatus       = opp::types::MessageStatus;
      using EnvelopeStatus      = opp::types::EnvelopeStatus;
      using AttestationType     = opp::types::AttestationType;
      using AttestationStatus   = opp::types::AttestationStatus;
   };

} // namespace sysio
