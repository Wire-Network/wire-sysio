#pragma once

#include <sysio/chain/types.hpp>
#include <sysio/chain/webassembly/common.hpp>
#include <sysio/chain/webassembly/return_codes.hpp>
#include <fc/crypto/sha1.hpp>
#include <boost/hana/string.hpp>

namespace sysio { namespace chain {
class apply_context;
namespace webassembly {

   class interface {
      public:
         interface(apply_context& ctx) : context(ctx) {}

         inline apply_context& get_context() { return context; }
         inline const apply_context& get_context() const { return context; }

         /**
          * Retrieve the signed_transaction.context_free_data[index].
          *
          * @ingroup context-free
          * @param index - the index of the context_free_data entry to retrieve.
          * @param[out] buffer - output buffer of the context_free_data entry.
          *
          * @retval -1 if the index is not valid.
          * @retval size of the cfd if the buffer is empty, otherwise return the amount of data copied onto the buffer.
         */
         int32_t get_context_free_data(uint32_t index, span<char> buffer) const;

         /**
          * Allows a privileged smart contract, e.g. the system contract, to pre-activate a consensus protocol upgrade feature.
          *
          * @ingroup privileged
          * @param feature_digest - 256-bit digest representing the feature to pre-activate.
         */
         void preactivate_feature(aligned_ptr<const digest_type> feature_digest);

         /**
          * Set the resource limits of an account.
          *
          * @ingroup privileged
          *
          * @param account - name of the account whose resource limit to be set.
          * @param ram_bytes - ram limit in absolute bytes.
          * @param net_weight - fractionally proportionate net limit of available resources based on (weight / total_weight_of_all_accounts).
          * @param cpu_weight - fractionally proportionate cpu limit of available resources based on (weight / total_weight_of_all_accounts).
         */
         void set_resource_limits(account_name account, int64_t ram_bytes, int64_t net_weight, int64_t cpu_weight);

         /**
          * Get the resource limits of an account
          *
          * @ingroup privileged
          *
          * @param account - name of the account whose resource limit to get.
          * @param[out] ram_bytes - output to hold retrieved ram limit in absolute bytes.
          * @param[out] net_weight - output to hold net weight.
          * @param[out] cpu_weight - output to hold cpu weight.
         */
         void get_resource_limits(account_name account, aligned_ptr<int64_t, 8> ram_bytes, aligned_ptr<int64_t, 8> net_weight, aligned_ptr<int64_t, 8> cpu_weight) const;

          /**
           * Get the current wasm limits configuration.
           *
           * The structure of the parameters is as follows:
           *
           * - max_mutable_global_bytes
           * The maximum total size (in bytes) used for mutable globals.
           * i32 and f32 consume 4 bytes and i64 and f64 consume 8 bytes.
           * Const globals are not included in this count.
           *
           * - max_table_elements
           * The maximum number of elements of a table.
           *
           * - max_section_elements
           * The maximum number of elements in each section.
           *
           * - max_linear_memory_init
           * The size (in bytes) of the range of memory that may be initialized.
           * Data segments may use the range [0, max_linear_memory_init).
           *
           * - max_func_local_bytes
           * The maximum total size (in bytes) used by parameters and local variables in a function.
           *
           * - max_nested_structures
           * The maximum nesting depth of structured control instructions.
           * The function itself is included in this count.
           *
           * - max_symbol_bytes
           * The maximum size (in bytes) of names used for import and export.
           *
           * - max_module_bytes
           * The maximum total size (in bytes) of a wasm module.
           *
           * - max_code_bytes
           * The maximum size (in bytes) of each function body.
           *
           * - max_pages
           * The maximum number of 64 KiB pages of linear memory that a contract can use.
           * Enforced when an action is executed. The initial size of linear memory is also checked at setcode.
           *
           * - max_call_depth
           * The maximum number of functions that may be on the stack. Enforced when an action is executed.
           *
           * @ingroup privileged
           *
           * @param[out] packed_parameters the ouput for the parameters.
           * @param max_version must be 0. Any other value aborts with wasm_config_unknown_version.
           *
           * @return the size of the packed parameters if packed_parameters is empty, otherwise it returns the amount of data written in packed_parameters.
         */
         uint32_t get_wasm_parameters_packed( span<char> packed_parameters, uint32_t max_version ) const;

         /**
           * Set the configuration for wasm limits.
           *
           * See get_wasm_parameters_packed documentation for more details on the structure of the packed_parameters.
           *
           * @ingroup privileged
           *
           * @param packed_parameters - a span containing the packed configuration to set. Trailing bytes abort.
         */
         void set_wasm_parameters_packed( span<const char> packed_parameters );

         /**
          * Proposes a schedule change using the legacy producer key format.
          *
          * @ingroup privileged
          *
          * @param packed_producer_schedule - vector of producer keys. Trailing bytes abort.
          *
          * @return pre-savanna:  -1 if proposing a new producer schedule was unsuccessful, otherwise returns the version of the new proposed schedule.
          *         post-savanna: -1 if proposing a new producer schedule was unsuccessful, otherwise returns max uint32_t
         */
         int64_t set_proposed_producers(span<const char> packed_producer_schedule);

         /**
          * Proposes a schedule change with extended features.
          *
          * Valid formats:
          * 0 : serialized array of producer_keys. Using this format is exactly equivalent to set_proposed_producers.
          * 1 : serialized array of producer_authority's.
          *
          * @ingroup privileged
          *
          * @param packed_producer_format - format of the producer data blob. Any other value aborts.
          * @param packed_producer_schedule - packed data of representing the producer schedule in the format indicated.
          *                                   Trailing bytes abort.
          *
          * @return pre-savanna:  -1 if proposing a new producer schedule was unsuccessful, otherwise returns the version of the new proposed schedule.
          *         post-savanna: -1 if proposing a new producer schedule was unsuccessful, otherwise returns max uint32_t
         */
         int64_t set_proposed_producers_ex(uint64_t packed_producer_format, span<const char> packed_producer_schedule);

         /**
          * Submits a finalizer set change.
          *
          *  // V0 format for packed finalizer_policy
          *  struct finalizer_authority {
          *     std::string              description;
          *     uint64_t                 weight = 0; // weight that this finalizer's vote has for meeting fthreshold
          *     std::vector<uint8_t>     public_key; // Affine little endian non-montgomery g1, cdt/abi_serializer has issues with std::array, size 96
          *  };
          *  struct finalizer_policy {
          *     uint64_t                          threshold = 0;
          *     std::vector<finalizer_authority>  finalizers;
          *  };
          *
          * Valid formats:
          * 0 : serialized finalizer_policy
          *
          * @ingroup privileged
          *
          * @param packed_finalizer_format - format of the finalizer_policy data blob. Any other value aborts.
          * @param packed_finalizer_policy - a serialized finalizer_policy object. Trailing bytes abort.
         */
         void set_finalizers(uint64_t packed_finalizer_format, span<const char> packed_finalizer_policy);

         /**
          * Retrieve the blockchain config parameters.
          *
          * @ingroup privileged
          *
          * @param[out] packed_blockchain_parameters - output buffer of the blockchain parameters.
          *
          * return the number of bytes copied to the buffer, or number of bytes required if the buffer is empty.
         */
         uint32_t get_blockchain_parameters_packed(span<char> packed_blockchain_parameters) const;

         /**
          * Set the blockchain parameters.
          *
          * @ingroup privileged
          *
          * @param packed_blockchain_parameters - a span containing the packed blockchain config parameters. Trailing
          *                                       bytes abort.
         */
         void set_blockchain_parameters_packed(span<const char> packed_blockchain_parameters);

         /**
          * Retrieve the blockchain config parameters.
          * The input buffer is a packed data stream which represents an encoded sequence of parameter_id pairs with the following format:
          * |varuint32:sequence_length | varuint32:parameter_id | ...
          * The output buffer is a packed data stream which represents an encoded sequence of parameter_id:paramter_value pairs with the following format:
          * |varuint32:sequence_length | varuint32:parameter_id | <various>:parameter_value | ...
          * The encoding of parameter_values should be specific to the parameter being set
          * The output buffer format should be valid input for set_parameters_packed.
          * For each known parameter_id in the input sequence there should be an associated entry in the output sequence with the current encoded parameter_value.
          *
          * @brief Retrieve the blockchain config parameters.
          * @ingroup privileged
          *
          * @param packed_parameter_ids - the input buffer with the format as described above. Trailing bytes abort.
          * @param[out] packed_parameters - the output buffer with the format as described above.
         */
         uint32_t get_parameters_packed( span<const char> packed_parameter_ids, span<char> packed_parameters) const;

         /**
          * Set the blockchain parameters.
          * It allows a system contract the ability to set parameters in a flexible manner.
          * The input buffer is a packed data stream which represents an encoded sequence of parameter_id:paramter_value pairs with the following format:
          * |varuint32:sequence_length | varuint32:parameter_id | <various>:parameter_value | ...
          * The encoding of parameter_values should be specific to the parameter being set.
          * Having duplicate parameter_ids encoded in the sequence should result in aborting the transaction context.
          * The presence of a parameter_id which is unknown OR which is known but tied to an unactivated consensus protocol
          * should result in aborting the transaction context.
          * There are no requirement for the ordering of items in the sequence.
          *
          * @brief Set the blockchain parameters in a flexible manner.
          * @ingroup privileged
          *
          * @param packed_parameters - buffer to hold the packed data with the format described above. Trailing bytes
          *                            abort.
         */
         void set_parameters_packed( span<const char> packed_parameters );

         /**
          * Check if an account is privileged.
          *
          * @ingroup privileged
          * @param account - name of the account to be checked.
          *
          * @retval true if the account is privileged
          * @retval false otherwise
         */
         bool is_privileged(account_name account) const;

         /**
          * Set the privileged status of an account.
          *
          * @ingroup privileged
          * @param account - name of the account that we want to give the privileged status.
          * @param is_priv - privileged status: 1 or 0. Any other value aborts.
         */
         void set_privileged(account_name account, bool is_priv);

         // softfloat api
         float _sysio_f32_add(float, float) const;
         float _sysio_f32_sub(float, float) const;
         float _sysio_f32_div(float, float) const;
         float _sysio_f32_mul(float, float) const;
         float _sysio_f32_min(float, float) const;
         float _sysio_f32_max(float, float) const;
         float _sysio_f32_copysign(float, float) const;
         float _sysio_f32_abs(float) const;
         float _sysio_f32_neg(float) const;
         float _sysio_f32_sqrt(float) const;
         float _sysio_f32_ceil(float) const;
         float _sysio_f32_floor(float) const;
         float _sysio_f32_trunc(float) const;
         float _sysio_f32_nearest(float) const;
         bool _sysio_f32_eq(float, float) const;
         bool _sysio_f32_ne(float, float) const;
         bool _sysio_f32_lt(float, float) const;
         bool _sysio_f32_le(float, float) const;
         bool _sysio_f32_gt(float, float) const;
         bool _sysio_f32_ge(float, float) const;
         double _sysio_f64_add(double, double) const;
         double _sysio_f64_sub(double, double) const;
         double _sysio_f64_div(double, double) const;
         double _sysio_f64_mul(double, double) const;
         double _sysio_f64_min(double, double) const;
         double _sysio_f64_max(double, double) const;
         double _sysio_f64_copysign(double, double) const;
         double _sysio_f64_abs(double) const;
         double _sysio_f64_neg(double) const;
         double _sysio_f64_sqrt(double) const;
         double _sysio_f64_ceil(double) const;
         double _sysio_f64_floor(double) const;
         double _sysio_f64_trunc(double) const;
         double _sysio_f64_nearest(double) const;
         bool _sysio_f64_eq(double, double) const;
         bool _sysio_f64_ne(double, double) const;
         bool _sysio_f64_lt(double, double) const;
         bool _sysio_f64_le(double, double) const;
         bool _sysio_f64_gt(double, double) const;
         bool _sysio_f64_ge(double, double) const;
         double _sysio_f32_promote(float) const;
         float _sysio_f64_demote(double) const;
         int32_t _sysio_f32_trunc_i32s(float) const;
         int32_t _sysio_f64_trunc_i32s(double) const;
         uint32_t _sysio_f32_trunc_i32u(float) const;
         uint32_t _sysio_f64_trunc_i32u(double) const;
         int64_t _sysio_f32_trunc_i64s(float) const;
         int64_t _sysio_f64_trunc_i64s(double) const;
         uint64_t _sysio_f32_trunc_i64u(float) const;
         uint64_t _sysio_f64_trunc_i64u(double) const;
         float _sysio_i32_to_f32(int32_t) const;
         float _sysio_i64_to_f32(int64_t) const;
         float _sysio_ui32_to_f32(uint32_t) const;
         float _sysio_ui64_to_f32(uint64_t) const;
         double _sysio_i32_to_f64(int32_t) const;
         double _sysio_i64_to_f64(int64_t) const;
         double _sysio_ui32_to_f64(uint32_t) const;
         double _sysio_ui64_to_f64(uint64_t) const;

         /**
          * Get the list of active producer names.
          *
          * @ingroup producer
          * @param[out] producers - output byte buffer receiving the packed names of the
          *    current active producers. The CDT-side ABI passes the buffer length in
          *    bytes, so the parameter is a byte span; declaring it as a span of
          *    account_name would validate (and bound the copy by) the raw length
          *    multiplied by sizeof(account_name).
          *
          * @return number of bytes required (if the buffer is empty), or the number of bytes written to the buffer.
         */
         int32_t get_active_producers(span<char> producers) const;

         /**
          * Tests a given public key with the recovered public key from digest and signature.
          *
          * @ingroup crypto
          * @param digest - digest of the message that was signed.
          * @param sig - signature. Trailing bytes abort.
          * @param pub - public key. Trailing bytes abort.
         */
         void assert_recover_key(aligned_ptr<const fc::sha256> digest, span<const char> sig, span<const char> pub) const;

         /**
          * Calculates the public key used for a given signature on a given digest.
          *
          * @ingroup crypto
          * @param digest - digest of the message that was signed.
          * @param sig - signature. Trailing bytes are a malformed signature and return -1.
          * @param[out] pub - output buffer for the public key result.
          *
          * @return the packed size of the public key, or -1 if no key could be recovered.
         */
         int32_t recover_key(aligned_ptr<const fc::sha256> digest, span<const char> sig, span<char> pub) const;

         /**
          * Verify one detached Ed25519 signature over unchanged message bytes.
          * @param sig Exactly 64 signature bytes, without an embedded public key.
          * @param pub Exactly 32 public-key bytes.
          * @param msg Raw message, at most config::max_ed25519_message_size bytes; may be empty.
          * @return return_code::success (0) on success, return_code::failure (-1) for invalid sizes,
          *         signatures, or keys. WASM memory and resource exceptions still abort the transaction.
          * @note A single size-limited verification bounds uninterrupted host work. The runtime enforces
          *       deadlines when WASM resumes; callers perform independent verifications in a WASM loop.
          * @ingroup crypto
          */
         int32_t ed25519_verify(span<const char> sig, span<const char> pub, span<const char> msg) const;

         /**
          * Tests if the sha256 hash generated from data matches the provided digest.
          *
          * @ingroup crypto
          * @param data - a span containing the data you want to hash.
          * @param hash_val - digest to compare to.
         */
         void assert_sha256(span<const char> data, aligned_ptr<const fc::sha256> hash_val) const;

         /**
          * Tests if the sha1 hash generated from data matches the provided digest.
          *
          * @ingroup crypto
          * @param data - a span containing the data you want to hash.
          * @param hash_val - digest to compare to.
         */
         void assert_sha1(span<const char> data, aligned_ptr<const fc::sha1> hash_val) const;

         /**
          * Tests if the sha512 hash generated from data matches the provided digest.
          *
          * @ingroup crypto
          * @param data - a span containing the data you want to hash.
          * @param hash_val - digest to compare to.
         */
         void assert_sha512(span<const char> data, aligned_ptr<const fc::sha512> hash_val) const;

         /**
          * Tests if the ripemd160 hash generated from data matches the provided digest.
          *
          * @ingroup crypto
          * @param data - a span containing the data you want to hash.
          * @param hash_val - digest to compare to.
         */
         void assert_ripemd160(span<const char> data, aligned_ptr<const fc::ripemd160> hash_val) const;

         /**
          * Hashes data using SHA256.
          *
          * @ingroup crypto
          * @param data - a span containing the data.
          * @param[out] hash_val - the resulting digest.
         */
         void sha256(span<const char> data, aligned_ptr<fc::sha256> hash_val) const;

         /**
          * Hashes data using SHA1.
          *
          * @ingroup crypto
          * @param data - a span containing the data.
          * @param[out] hash_val - the resulting digest.
         */
         void sha1(span<const char> data, aligned_ptr<fc::sha1> hash_val) const;

         /**
          * Hashes data using SHA512.
          *
          * @ingroup crypto
          * @param data - a span containing the data.
          * @param[out] hash_val - the hash
         */
         void sha512(span<const char> data, aligned_ptr<fc::sha512> hash_val) const;

         /**
          * Hashes data using RIPEMD160.
          *
          * @ingroup crypto
          * @param data - a span containing the data.
          * @param[out] hash_val - computed digest.
         */
         void ripemd160(span<const char> data, aligned_ptr<fc::ripemd160> hash_val) const;

         /**
          * Checks if a transaction is authorized by a provided set of keys and permissions.
          *
          * @ingroup permission
          * @param trx_data - serialized transaction. Trailing bytes and transaction extensions abort.
          * @param pubkeys_data - serialized vector of provided public keys. Trailing bytes abort.
          * @param perms_data - serialized vector of provided permissions (empty permission name acts as wildcard).
          *                     Trailing bytes abort.
          *
          * @retval true if transaction is authorized.
          * @retval false otherwise.
         */
         bool check_transaction_authorization(span<const char> trx_data, span<const char> pubkeys_data, span<const char> perms_data) const;

         /**
          * Checks if a permission is authorized by a provided set of keys and permissions.
          *
          * @ingroup permission
          * @param account - the account owner of the permission.
          * @param permission - the name of the permission to check for authorization.
          * @param pubkeys_data - serialized vector of provided public keys. Trailing bytes abort.
          * @param perms_data - serialized vector of provided permissions (empty permission name acts as wildcard).
          *                     Trailing bytes abort.
          *
          * @retval true if permission is authorized.
          * @retval false otherwise.
         */
         bool check_permission_authorization(account_name account, permission_name permission, span<const char> pubkeys_data, span<const char> perms_data) const;

         /**
          * Look up the permission of an account with the smallest name >= the given permission name.
          *
          * @ingroup permission
          * @param account - the account to search.
          * @param permission - the lower bound permission name.
          * @param[out] buffer - output buffer for serialized permission_record
          *   (perm_name, parent_name, last_updated, authority).
          *
          * @retval -1 if no permission found for the given account.
          * @retval size of serialized data otherwise. Copies min(buffer.size(), data_size) bytes into buffer.
          */
         int32_t get_permission_lower_bound(account_name account, permission_name permission, span<char> buffer);

         /**
          * Verifies that an account exists in the set of provided auths on an action. Fails if not found.
          *
          * @ingroup authorization
          * @param account - the name of the account to be verified.
         */
         void require_auth(account_name account) const;

         /**
          * Verifies that an account with a specific permission exists in the set of provided auths on an action,
          *
          * @ingroup authorization
          * @param account - the name of the account to be verified.
          * @param permission - the name of the permission to be verified.
         */
         void require_auth2(account_name account, permission_name permission) const;

         /**
          * Test whether an account exists in the set of provided auths on an action.
          *
          * @ingroup authorization
          * @param account - name of the account to be tested.
          *
          * @retval true if the action has an auth with the account name.
          * @retval false otherwise.
         */
         bool has_auth(account_name account) const;

         /**
          * Add the specified account to set of accounts to be notified.
          *
          * @ingroup authorization
          * @param recipient - account to be notified.
         */
         void require_recipient(account_name recipient);

         /**
          * Verifies that n is an existing account.
          *
          * @ingroup authorization
          * @param account - name of the account to check.
          *
          * @return true if the account exists.
          * @return false otherwise.
         */
         bool is_account(account_name account) const;

         /**
          * Retrieves the code hash for an account, if any.
          *
          * The result is the packed version of this struct:
          *
          * struct {
          *    varuint32 struct_version;
          *    uint64_t code_sequence;
          *    fc::sha256 code_hash;
          *    uint8_t vm_type;
          *    uint8_t vm_version;
          * } result;
          *
          * @ingroup authorization
          * @param account - name of the account to check.
          * @param struct_version - must be 0. Any other value aborts.
          * @param packed_result - receives the packed result.
          *
          * @return the size of the packed result.
         */
         uint32_t get_code_hash(
            account_name account,
            uint32_t struct_version,
            vm::span<char> packed_result) const;

         /**
          * Returns the time in microseconds from 1970 of the current block.
          *
          * @ingroup system
          *
          * @return time in microseconds from 1970 of the current block.
         */
         uint64_t current_time() const;

         /**
          * Returns the current block number.
          *
          * @ingroup system
          *
          * @return current block number.
         */
         uint32_t get_block_num() const;

         /**
          * Returns the transaction's publication time.
          *
          * @ingroup system
          *
          * @return time in microseconds from 1970 of the publication_time.
         */
         uint64_t publication_time() const;

         /**
          * Check if specified protocol feature has been activated.
          *
          * @ingroup system
          * @param feature_digest - digest of the protocol feature.
          *
          * @retval true if the specified protocol feature has been activated.
          * @retval false otherwise.
         */
         bool is_feature_activated(aligned_ptr<const digest_type> feature_digest) const;

         /**
          * Return the name of the account that sent the current inline action.
          *
          * @ingroup system
          * @return name of account that sent the current inline action (empty name if not called from inline action).
         */
         name get_sender() const;

        /**
         * Get the ram usage of an account
         *
         * @param account - name of the account whose ram usage to get.
         * @return number of bytes currently used by the account.
        */
        int64_t get_ram_usage(account_name account) const;

         /**
          * Aborts processing of this action and unwinds all pending changes.
          *
          * @ingroup context-free
         */
         void abort() const;

         /**
          * Aborts processing of this action if the test condition is false.
          *
          * @ingroup context-free
          * @param condition - test condition: 1 or 0. Any other value aborts.
          * @param msg - string explaining the reason for failure.
         */
         void sysio_assert(bool condition, null_terminated_ptr msg) const;

         /**
          * Aborts processing of this action if the test condition is false.
          *
          * @ingroup context-free
          * @param condition - test condition: 1 or 0. Any other value aborts.
          * @param msg - string explaining the reason for failure.
          */
         void sysio_assert_message(bool condition, span<const char> msg) const;

         /**
          * Aborts processing of this action if the test condition is false.
          * It can be used to provide an error code rather than a message string on assertion checks.
          * If the assertion fails, the provided error code will be made available through the exception message.
          *
          * @ingroup context-free
          * @param condition - test condition: 1 or 0. Any other value aborts.
          * @param error_code - the error code associated. A reserved code aborts whatever the condition is.
         */
         void sysio_assert_code(bool condition, uint64_t error_code) const;

         /**
          * This method will abort execution of wasm without failing the contract.
          *
          * @ingroup context-free
          * @param code - must be 0. Any other value aborts the action. The host gives the code no meaning.
         */
         void sysio_exit(int32_t code) const;

         /**
          * Copy up to length bytes of the current action data to the specified location.
          *
          * @ingroup action
          * @param memory - a pointer where up to length bytes of the current action data will be copied.
          *
          * @return the number of bytes copied to msg, or number of bytes that can be copied if an empty span is passed.
         */
         int32_t read_action_data(span<char> memory) const;

         /**
          * Get the length of the current action's data field. This method is useful for dynamically sized actions.
          *
          * @ingroup action
          * @return the length of the current action's data field
         */
         int32_t action_data_size() const;

         /**
          * Get the current receiver of the action.
          *
          * @ingroup action
          * @return the name of the receiver
         */
         name current_receiver() const;

         /**
          * Sets a value (packed blob char array) to be included in the action receipt.
          *
          * @ingroup action
          * @param packed_blob - the packed blob
         */
         void set_action_return_value(span<const char> packed_blob);

         /**
          * Print a string.
          *
          * @ingroup console
          * @param str - the string to print
         */
         void prints(null_terminated_ptr str);

         /**
          * Prints string up to given length.
          *
          * @ingroup console
          * @param str - the string to print.
          */
         void prints_l(span<const char> str);

         /**
          * Prints value as a 64 bit signed integer.
          *
          * @ingroup console
          * @param val - 64 bit signed integer to be printed.
          */
         void printi(int64_t val);

         /**
          * Prints value as a 64 bit unsigned integer.
          *
          * @ingroup console
          * @param val - 64 bit unsigned integer to be printed.
          */
         void printui(uint64_t val);

         /**
          * Prints value as a 128 bit signed integer.
          *
          * @ingroup console
          * @param val - 128 bit signed integer to be printed.
          */
         void printi128(aligned_ptr<const __int128> val);

         /**
          * Prints value as a 128 bit unsigned integer.
          *
          * @ingroup console
          * @param val - 128 bit unsigned integer to be printed.
          */
         void printui128(aligned_ptr<const unsigned __int128> val);

         /**
          * Prints value as single-precision floating point number.
          *
          * @ingroup console
          * @param val - single-precision floating point number to be printed.
          */
         void printsf(softfloat32_t val);

         /**
          * Prints value as double-precision floating point number.
          *
          * @ingroup console
          * @param val - double-precision floating point number to be printed
          */
         void printdf(softfloat64_t val);

         /**
          * Prints value as quadruple-precision floating point number.
          *
          * @ingroup console
          * @param val - a pointer to the quadruple-precision floating point number to be printed
          */
         void printqf(aligned_ptr<const softfloat128_t> val);

         /**
          * Prints a 64 bit names as base32 encoded string.
          *
          * @ingroup console
          * @param value - 64 bit name to be printed
          */
         void printn(name value);

         /**
          * Prints a 64 bit names as base32 encoded string
          *
          * @ingroup console
          * @param data - Hex name to be printed.
          */
         void printhex(span<const char> data);

         // ---- KV Database API ----
         //
         // A state miss is returned and misuse throws. Calls that return a status return a kv_status: 0 ok, 1 end,
         // 2 no_row, 3 exists. Calls that return a size or a handle use -1 for none. Bad arguments, size limits and
         // the iterator limit throw.
         //
         // table_id is a table's namespace: the low 16 bits of DJB2 over the 8 big-endian bytes of name::raw. A
         // secondary index has its own table_id, from two chained passes over the table's name::raw and then the
         // index's (position + 1 for multi_index). The chain does not enforce uniqueness; CDT codegen checks within one
         // ABI. A value above 65535 throws kv_invalid_table_id.
         //
         // Keys are arbitrary non-empty byte sequences, compared as unsigned bytes with a shorter prefix first. An empty
         // key throws kv_empty_key on every call that takes a key, since no row can have one. A prefix or a seek is not
         // a key and may be empty. Index entries sort by sec_key, then pri_key. max_kv_key_size and
         // max_kv_secondary_key_size default to 256 bytes and max_kv_value_size to 256 KiB. Governance can raise the key
         // sizes to 512 bytes and the value size to 1 MiB. A key a call only looks up has no size limit.
         //
         // Reads copy into caller buffers. A zero-size buffer skips the copy and a short buffer truncates it. When a
         // row is found, the size reported is the field's full size either way, so a contract can size a buffer from
         // it. Offsets exist on value reads only: an offset at or past the end copies nothing.
         //
         // Writes apply to the executing contract's (`receiver`) tables. The RAM check is per action, for unprivileged
         // receivers, and a payer other than the receiver must authorize with sysio.payer. Payer 0 (the CDT
         // `same_payer` sentinel, name{}) keeps the current payer when the row exists and throws `invalid_table_payer`
         // when the call would create one. A payer change refunds the old payer in full and charges the new one in full.

         // ---- KV Database API -- Rows ----

         /**
          * Write a row in the executing contract's KV table.
          *
          * @param table_id - table namespace
          * @param payer - account to bill for RAM, or 0 to keep the current payer
          * @param key - the key bytes
          * @param value - the value bytes
          * @param mode - 0 upsert, 1 insert, 2 update
          * @return kv_status: ok; exists if mode is insert and the key is taken; no_row if mode is update and the key
          *   is free
          * @throws kv_invalid_mode if mode is not 0, 1 or 2
          */
         int32_t  kv_set(uint32_t table_id, uint64_t payer, span<const char> key, span<const char> value, uint32_t mode);

         /**
          * Erase a row from the executing contract's KV table. Its RAM is refunded to the row's current payer.
          *
          * @param table_id - table namespace
          * @param key - the key bytes to erase
          * @return kv_status: ok, or no_row if the key is free
          */
         int32_t  kv_erase(uint32_t table_id, span<const char> key);

         /**
          * Read a row's value by key from any contract's KV table.
          *
          * @param table_id - table namespace
          * @param code - account whose KV table to read from
          * @param key - the key bytes to look up
          * @param offset - byte offset within the value to start reading
          * @param value - destination buffer
          * @return the value's full size in bytes, or -1 if the key is free
          * @throws kv_empty_key if key is empty
          */
         int32_t  kv_get(uint32_t table_id, uint64_t code, span<const char> key, uint32_t offset, span<char> value);

         // ---- KV Database API -- Index Entries ----
         //
         // A secondary index holds (sec_key, pri_key) entries in a table namespace of its own. The executing contract
         // manages entries explicitly; the CDT table wrappers automate this.

         /**
          * Insert a secondary index entry mapping a secondary key to a primary key. Both keys must be non-empty.
          *
          * @param table_id - secondary index identifier
          * @param payer - account to bill for RAM
          * @param pri_key - primary key bytes this entry points to
          * @param sec_key - secondary key bytes (order-preserving encoded)
          * @return kv_status: ok, or exists if the (sec_key, pri_key) pair is taken
          */
         int32_t  kv_idx_store(uint32_t table_id, uint64_t payer, span<const char> pri_key, span<const char> sec_key);

         /**
          * Change a secondary index entry's key in place. All three keys must be non-empty.
          *
          * @param table_id - secondary index identifier
          * @param payer - account to bill for RAM, or 0 to keep the current payer
          * @param pri_key - primary key (unchanged)
          * @param old_sec_key - current secondary key to replace
          * @param new_sec_key - new secondary key value
          * @return kv_status: ok; no_row if the entry does not exist; exists if (new_sec_key, pri_key) is taken
          */
         int32_t  kv_idx_update(uint32_t table_id, uint64_t payer, span<const char> pri_key,
                                span<const char> old_sec_key, span<const char> new_sec_key);

         /**
          * Erase a secondary index entry. Its RAM is refunded to the entry's current payer. Both keys must be non-empty.
          *
          * @param table_id - secondary index identifier
          * @param pri_key - primary key associated with the entry
          * @param sec_key - secondary key to erase
          * @return kv_status: ok, or no_row if the entry does not exist
          */
         int32_t  kv_idx_erase(uint32_t table_id, span<const char> pri_key, span<const char> sec_key);

         // ---- KV Database API -- Primary Iterators ----
         //
         // An iterator's position is a key. Moves land on a row or at end, and return ok or end. Reads return the row
         // stored at the current key now, or no_row if there is none, so a row erased and stored again at that key
         // reads back. A move from a position with no row goes strictly past that key.
         //
         // A prefix bounds an iterator: moves past either edge of the prefix reach end. A seek below the prefix starts
         // from the prefix, and an empty seek is the first row. Moving back from end lands on the last row in the
         // prefix. Moving back from the first row goes to end.
         //
         // Up to config::max_kv_iterators (1024) live iterator handles are available per pool. A handle lasts for one
         // contract's execution: every open handle is released before the next notified contract runs, and inline
         // actions start with empty pools. Destroy handles with kv_it_destroy when no longer needed.

         /**
          * Create an iterator over keys matching a prefix and seek it. Allocates a handle whatever it lands on.
          *
          * @param table_id - table namespace
          * @param code - account whose KV table to iterate
          * @param prefix - key prefix bounding the iteration (may be empty for all keys)
          * @param seek - first key to land at or above (may be empty for the first row)
          * @return iterator handle
          * @throws kv_iterator_limit_exceeded if the iterator pool is exhausted
          */
         int32_t  kv_it_create(uint32_t table_id, uint64_t code, span<const char> prefix, span<const char> seek);

         /**
          * Move to the next key and read the row there.
          *
          * @param handle - iterator handle
          * @param key - destination for the key
          * @param value - destination for the value
          * @param key_size - [out] full key size, 0 at end
          * @param value_size - [out] full value size, 0 at end. Must not overlap the other size
          * @return kv_status: ok or end
          */
         int32_t  kv_it_next(int32_t handle, span<char> key, span<char> value,
                             aligned_ptr<uint32_t> key_size, aligned_ptr<uint32_t> value_size);

         /**
          * Move to the previous key and read the row there.
          *
          * @param handle - iterator handle
          * @param key - destination for the key
          * @param value - destination for the value
          * @param key_size - [out] full key size, 0 at end
          * @param value_size - [out] full value size, 0 at end. Must not overlap the other size
          * @return kv_status: ok or end
          */
         int32_t  kv_it_prev(int32_t handle, span<char> key, span<char> value,
                             aligned_ptr<uint32_t> key_size, aligned_ptr<uint32_t> value_size);

         /**
          * Move to the first key at or above `seek` and read the row there.
          *
          * @param handle - iterator handle
          * @param seek - key to seek to
          * @param key - destination for the key
          * @param value - destination for the value
          * @param key_size - [out] full key size, 0 at end
          * @param value_size - [out] full value size, 0 at end. Must not overlap the other size
          * @return kv_status: ok or end
          */
         int32_t  kv_it_lower_bound(int32_t handle, span<const char> seek, span<char> key, span<char> value,
                                    aligned_ptr<uint32_t> key_size, aligned_ptr<uint32_t> value_size);

         /**
          * Read the row stored at the iterator's current key.
          *
          * @param handle - iterator handle
          * @param offset - byte offset within the value to start reading
          * @param key - destination for the key
          * @param value - destination for the value
          * @param key_size - [out] full key size, 0 unless the status is ok
          * @param value_size - [out] full value size, 0 unless the status is ok. Must not overlap the other size
          * @return kv_status: ok; end; no_row if no row is stored at the current key
          */
         int32_t  kv_it_read(int32_t handle, uint32_t offset, span<char> key, span<char> value,
                             aligned_ptr<uint32_t> key_size, aligned_ptr<uint32_t> value_size);

         /**
          * Release an iterator handle back to the pool. -1 is not a handle and throws kv_invalid_iterator.
          *
          * @param handle - iterator handle from kv_it_create
          */
         void     kv_it_destroy(int32_t handle);

         // ---- KV Database API -- Secondary Iterators ----
         //
         // These work as the primary iterators do, over index entries in (sec_key, pri_key) order. Prefix and seek apply
         // to sec_key. A position is the (sec_key, pri_key) pair.

         /**
          * Create an iterator over index entries whose secondary key matches a prefix, and seek it. Allocates a handle
          * whatever it lands on.
          *
          * @param table_id - secondary index identifier
          * @param code - account whose indices to iterate
          * @param prefix - secondary key prefix bounding the iteration (may be empty for all entries)
          * @param seek - first secondary key to land at or above (may be empty for the first entry)
          * @return secondary iterator handle
          * @throws kv_iterator_limit_exceeded if the iterator pool is exhausted
          */
         int32_t  kv_idx_create(uint32_t table_id, uint64_t code, span<const char> prefix, span<const char> seek);

         /**
          * Move to the next entry and read it.
          *
          * @param handle - secondary iterator handle
          * @param sec_key - destination for the secondary key
          * @param pri_key - destination for the primary key
          * @param sec_size - [out] full secondary key size, 0 at end
          * @param pri_size - [out] full primary key size, 0 at end. Must not overlap the other size
          * @return kv_status: ok or end
          */
         int32_t  kv_idx_next(int32_t handle, span<char> sec_key, span<char> pri_key,
                              aligned_ptr<uint32_t> sec_size, aligned_ptr<uint32_t> pri_size);

         /**
          * Move to the previous entry and read it.
          *
          * @param handle - secondary iterator handle
          * @param sec_key - destination for the secondary key
          * @param pri_key - destination for the primary key
          * @param sec_size - [out] full secondary key size, 0 at end
          * @param pri_size - [out] full primary key size, 0 at end. Must not overlap the other size
          * @return kv_status: ok or end
          */
         int32_t  kv_idx_prev(int32_t handle, span<char> sec_key, span<char> pri_key,
                              aligned_ptr<uint32_t> sec_size, aligned_ptr<uint32_t> pri_size);

         /**
          * Move to the first entry whose secondary key is at or above `seek`, and read it.
          *
          * @param handle - secondary iterator handle
          * @param seek - secondary key to seek to
          * @param sec_key - destination for the secondary key
          * @param pri_key - destination for the primary key
          * @param sec_size - [out] full secondary key size, 0 at end
          * @param pri_size - [out] full primary key size, 0 at end. Must not overlap the other size
          * @return kv_status: ok or end
          */
         int32_t  kv_idx_lower_bound(int32_t handle, span<const char> seek, span<char> sec_key, span<char> pri_key,
                                     aligned_ptr<uint32_t> sec_size, aligned_ptr<uint32_t> pri_size);

         /**
          * Read the entry stored at the iterator's current position.
          *
          * @param handle - secondary iterator handle
          * @param sec_key - destination for the secondary key
          * @param pri_key - destination for the primary key
          * @param sec_size - [out] full secondary key size, 0 unless the status is ok
          * @param pri_size - [out] full primary key size, 0 unless the status is ok. Must not overlap the other size
          * @return kv_status: ok; end; no_row if no entry is stored at the current position
          */
         int32_t  kv_idx_read(int32_t handle, span<char> sec_key, span<char> pri_key,
                              aligned_ptr<uint32_t> sec_size, aligned_ptr<uint32_t> pri_size);

         /**
          * Release a secondary iterator handle back to the pool. -1 is not a handle and throws kv_invalid_iterator.
          *
          * @param handle - secondary iterator handle from kv_idx_create or kv_idx_find
          */
         void     kv_idx_destroy(int32_t handle);

         /**
          * Find an exact secondary key match and return an iterator on the first entry with that key.
          *
          * @param table_id - secondary index identifier
          * @param code - account whose indices to search
          * @param prefix - secondary key prefix bounding the iterator's later moves
          * @param sec_key - secondary key to find, prefix included
          * @return secondary iterator handle, or -1 with no handle allocated if no entry has that key
          * @throws kv_empty_key if sec_key is empty
          * @throws kv_key_outside_prefix if sec_key does not start with prefix
          */
         int32_t  kv_idx_find(uint32_t table_id, uint64_t code, span<const char> prefix, span<const char> sec_key);

         // memory api
         void* memcpy(memcpy_params) const;
         void* memmove(memcpy_params) const;
         int32_t memcmp(memcmp_params) const;
         void* memset(memset_params) const;

         /**
          * Send an inline action in the context of the parent transaction of this operation.
          *
          * @ingroup transaction
          * @param data - the inline action to be sent. Trailing bytes abort.
         */
         void send_inline(span<const char> data);

         /**
          * Send a context free inline action in the context of the parent transaction of this operation.
          *
          * @ingroup transaction
          * @param data - the packed free inline action to be sent. Trailing bytes abort.
         */
         void send_context_free_inline(span<const char> data);

         /**
          * Access a copy of the currently executing transaction.
          *
          * @ingroup context-free-transaction
          * @param[out] data - the currently executing transaction (packed).
          *
          * @retval false if transaction was not found.
          * @retval true if transaction was canceled.
         */
         int32_t read_transaction(span<char> data) const;

         /**
          * Gets the size of the currently executing transaction.
          *
          * @ingroup context-free-transaction
          *
          * @return size of the currently executing transaction.
         */
         int32_t transaction_size() const;

         /**
          * Gets the expiration of the currently executing transaction.
          *
          * @ingroup context-free-transaction
          *
          * @return expiration of the currently executing transaction in seconds since Unix epoch.
         */
         int32_t expiration() const;

         /**
          * Gets the block number used for TAPOS on the currently executing transaction.
          *
          * @ingroup context-free-transaction
          *
          * @return block number used for TAPOS on the currently executing transaction.
         */
         int32_t tapos_block_num() const;

         /**
          * Gets the block prefix used for TAPOS on the currently executing transaction.
          *
          * @ingroup context-free-transaction
          *
          * @return block prefix used for TAPOS on the currently executing transaction.
         */
         int32_t tapos_block_prefix() const;

         /**
          * Retrieve the indicated action from the active transaction.
          *
          * @ingroup context-free-transaction
          *
          * @param type - 0 for context free action, 1 for action.
          * @param index - the index of the requested action.
          * @param[out] buffer - the action we want (packed).
          *
          * @return the number of bytes written on the buffer or -1 if there was an error.
         */
         int32_t get_action(uint32_t type, uint32_t index, span<char> buffer) const;

         /**
          * Host function for addition on the elliptic curve alt_bn128
          *
          * @ingroup crypto
          * @param op1 - a span containing the first operand G1 point.
          * @param op2 - a span containing the second operand G1 point.
          * @param[out] result - the result op1 + op2. Must be exactly 64 bytes.
          * @return -1 if there was an error, 0 otherwise
         */
         int32_t alt_bn128_add(span<const char> op1, span<const char> op2, span<char> result) const;

         /**
          * Host function for scalar multiplication on the elliptic curve alt_bn128
          *
          * @ingroup crypto
          * @param g1_point - a span containing G1 point.
          * @param scalar   - a span containing the scalar.
          * @param[out] result - g1 * scalar. Must be exactly 64 bytes.
          * @return -1 if there was an error, 0 otherwise
         */
         int32_t alt_bn128_mul(span<const char> g1_point, span<const char> scalar, span<char> result) const;

         /**
          * Host function for optimal ate pairing check on the elliptic curve alt_bn128
          *
          * @ingroup crypto
          * @param g1_g2_pairs - a span containing pairs of G1,G2 points. (2 * 32 bytes) + (2 * 64 bytes)
          * @return -1 if there was an error, 1 if false and 0 if true
         */
         int32_t alt_bn128_pair(span<const char> g1_g2_pairs) const;

         /**
          * Big integer modular exponentiation
          *
          * <BASE> <EXPONENT> <MODULUS>
          * returns an output (BASE**EXPONENT) % MODULUS as a byte array {{{{ with the same length as the modulus }}}}
          *
          * @ingroup crypto
          * @param base        - a span containing BASE.
          * @param exp         - a span containing EXPONENT.
          * @param modulus     - a span containing MODULUS.
          * @param[out] out    - the result (BASE**EXPONENT) % MODULUS. Must be exactly the size of modulus.
          * @return              -1 if there was an error, 0 otherwise
         */
         int32_t mod_exp(span<const char> base, span<const char> exp, span<const char> modulus, span<char> out) const;

         /**
          * BLAKE2 compression function `F`
          * https://eips.ethereum.org/EIPS/eip-152
          * Precompiled contract which implements the compression function F used in the BLAKE2 cryptographic hashing algorithm.
          *
          * @ingroup crypto
          * @param rounds        - the number of rounds - 32-bit unsigned big-endian word
          * @param state         - a span containing the state vector - 8 unsigned 64-bit little-endian words
          * @param message       - a span containing the message block vector - 16 unsigned 64-bit little-endian words
          * @param t0_offset     - offset counters - unsigned 64-bit little-endian word
          * @param t1_offset     - offset counters - unsigned 64-bit little-endian word
          * @param final         - the final block indicator flag: 1 or 0. Any other value aborts.
          * @param[out] result   - the result. Must be exactly 64 bytes.
          * @return                -1 if there was an error, 0 otherwise
         */
         int32_t blake2_f( uint32_t rounds, span<const char> state, span<const char> message, span<const char> t0_offset, span<const char> t1_offset, bool final, span<char> result) const;

         /**
          * BLAKE2b-256 hash
          *
          * @ingroup crypto
          * @param data    - a span pointing at the input bytes to hash
          * @param result  - a span pointing at 32 bytes of output space
          * @return        -1 on error (e.g. wrong span sizes), 0 on success
          */
         int32_t blake2b_256( span<const char> data, span<char> result ) const;

         /**
          * Hashes data using SHA3.
          *
          * @ingroup crypto
          * @param data - a span containing the data.
          * @param[out] hash_val - the resulting digest. Must be exactly 32 bytes.
          * @param keccak - use keccak version: 1 or 0. Any other value aborts.
         */
         void sha3( span<const char> data, span<char> hash_val, bool keccak) const;

         /**
          * Calculates the uncompressed public key used for a given signature on a given digest.
          *
          * @ingroup crypto
          * @param signatue - signature.
          * @param digest - digest of the message that was signed.
          * @param[out] pub - output buffer for the public key result. Must be exactly 65 bytes.
          *
          * @return -1 if there was an error, 0 otherwise.
         */
         int32_t k1_recover( span<const char> signature, span<const char> digest, span<char> pub) const;

         /**
          * Host function for G1 addition on the elliptic curve bls12-381
          *
          * @ingroup crypto
          * @param op1 - a span containing the affine coordinates of the first operand G1 point - 96 bytes little-endian.
          * @param op2 - a span containing the affine coordinates of the second operand G1 point - 96 bytes little-endian.
          * @param[out] result - the result op1 + op2 - affine coordinates 96 bytes little-endian.
          * @return -1 if there was an error, 0 otherwise
         */
         int32_t bls_g1_add(span<const char> op1, span<const char> op2, span<char> result) const;

         /**
          * Host function for G2 addition on the elliptic curve bls12-381
          *
          * @ingroup crypto
          * @param op1 - a span containing the affine coordinates of the first operand G2 point - 192 bytes little-endian.
          * @param op2 - a span containing the affine coordinates of the second operand G2 point - 192 bytes little-endian.
          * @param[out] result - the result op1 + op2 - affine coordinates 192 bytes little-endian.
          * @return -1 if there was an error, 0 otherwise
         */
         int32_t bls_g2_add(span<const char> op1, span<const char> op2, span<char> result) const;

         /**
          * Host function for G1 weighted sum on the elliptic curve bls12-381
          *
          * @ingroup crypto
          * @param points - a span containing a list of G1 points (P0, P1, P2... Pn) - affine coordinates 96*n bytes little-endian.
          * @param scalars - a span containing a list of 32 byte scalars (s0, s1, s2... sn) - 32*n bytes little-endian.
          * @param n - the number of elements in the lists.
          * @param[out] result - the result s0 * P0 + s1 * P1 + ... + sn * Pn. - affine coordinates 96 bytes little-endian.
          * @return -1 if there was an error, 0 otherwise
         */
         int32_t bls_g1_weighted_sum(span<const char> points, span<const char> scalars, const uint32_t n, span<char> result) const;

         /**
          * Host function for G2 weighted sum on the elliptic curve bls12-381
          *
          * @ingroup crypto
          * @param points - a span containing a list of G2 points (P0, P1, P2... Pn) - affine coordinates 192*n bytes little-endian.
          * @param scalars - a span containing a list of 32 byte scalars (s0, s1, s2... sn) - 32*n bytes little-endian.
          * @param n - the number of elements in the lists.
          * @param[out] result - the result s0 * P0 + s1 * P1 + ... + sn * Pn - affine coordinates 192 bytes little-endian.
          * @return -1 if there was an error, 0 otherwise
         */
         int32_t bls_g2_weighted_sum(span<const char> points, span<const char> scalars, const uint32_t n, span<char> result) const;

         /**
          * Host function to calculate the pairing of (G1, G2) pairs on the elliptic curve bls12-381
          *
          * @ingroup crypto
          * @param g1_points - a span containing a list of G1 points (P0, P1, P2... Pn) - affine coordinates 96*n bytes little-endian.
          * @param g2_points - a span containing a list of G2 points (P0, P1, P2... Pn) - affine coordinates 192*n bytes little-endian..
          * @param n - the number of elements in the lists.
          * @param[out] result - the result e(g1_0, g2_0) * e(g1_1, g2_1) * ... * e(g1_n, g2_n) - 576 bytes little-endian.
          * @return -1 if there was an error, 0 otherwise
         */
         int32_t bls_pairing(span<const char> g1_points, span<const char> g2_points, const uint32_t n, span<char> result) const;

         /**
          * Host function for mapping fp to G1 on the elliptic curve bls12-381
          *
          * @ingroup crypto
          * @param e - a span containing the field element fp to be mapped - 48 bytes little-endian.
          * @param[out] result - the resulting element in G1 - affine coordinates 96 bytes little-endian.
          * @return -1 if there was an error, 0 otherwise
         */
         int32_t bls_g1_map(span<const char> e, span<char> result) const;

         /**
          * Host function for mapping fp2 to G2 on the elliptic curve bls12-381
          *
          * @ingroup crypto
          * @param e - a span containing the field element fp2 to be mapped - 96 bytes little-endian.
          * @param[out] result - the resulting element in G2 - affine coordinates 192 bytes little-endian.
          * @return -1 if there was an error, 0 otherwise
         */
         int32_t bls_g2_map(span<const char> e, span<char> result) const;

         /**
          * Host function for modular reduction of 64 bytes wide scalar to a field element (fp, 48 bytes) of the elliptic curve bls12-381
          *
          * @ingroup crypto
          * @param s - a span containing the 64 bytes little-endian wide scalar to be reduced.
          * @param[out] result - the resulting field element fp - 48 bytes little-endian.
          * @return -1 if there was an error, 0 otherwise
         */
         int32_t bls_fp_mod(span<const char> s, span<char> result) const;

         /**
          * Host function for multiplication of field elements (fp, 48 bytes) of the elliptic curve bls12-381
          *
          * @ingroup crypto
          * @param op1 - a span containing the first operand fp point - 48 bytes little-endian.
          * @param op2 - a span containing the second operand fp point - 48 bytes little-endian.
          * @param[out] result - the result op1 * op2 - 48 bytes little-endian.
          * @return -1 if there was an error, 0 otherwise
         */
         int32_t bls_fp_mul(span<const char> op1, span<const char> op2, span<char> result) const;

         /**
          * Host function for exponentiation of field elements (fp, 48 bytes) of the elliptic curve bls12-381
          *
          * @ingroup crypto
          * @param base - a span containing the base fp point - 48 bytes little-endian.
          * @param exp - a span containing the 64 bytes little-endian wide scalar as exponent.
          * @param[out] result - the result of base to the power of exp - 48 bytes little-endian.
          * @return -1 if there was an error, 0 otherwise
         */
         int32_t bls_fp_exp(span<const char> base, span<const char> exp, span<char> result) const;

      private:
         apply_context& context;
   };

}}} // ns sysio::chain::webassembly
