#include <sysio/signature_provider_manager_plugin/aws_common.hpp>

#include <aws/core/Aws.h>

namespace sysio::sigprov::aws {

namespace {

/// Process-wide AWS SDK lifecycle. Constructed lazily on the first
/// `ensure_aws_sdk_initialized()` call, destroyed at static destruction --
/// after any `region_client_cache` (whose constructor touches this lifecycle
/// first, making the cache the younger Meyers singleton; younger statics are
/// destroyed first). Holding an AWS client shared_ptr inside a long-lived
/// closure is safe because the application object owns the plugins and is
/// destroyed before atexit static teardown; do not hand an AWS-backed closure
/// to an owner that outlives the application.
struct aws_sdk_lifecycle {
   static aws_sdk_lifecycle& instance() {
      static aws_sdk_lifecycle s;
      return s;
   }

   // This is a Meyers singleton: there is exactly one lifecycle per process.
   // Deleting copy / move makes that intent explicit and stops a stray
   // `aws_sdk_lifecycle copy = ...` from compiling and running a second
   // InitAPI / ShutdownAPI pair.
   aws_sdk_lifecycle(const aws_sdk_lifecycle&)            = delete;
   aws_sdk_lifecycle(aws_sdk_lifecycle&&)                 = delete;
   aws_sdk_lifecycle& operator=(const aws_sdk_lifecycle&) = delete;
   aws_sdk_lifecycle& operator=(aws_sdk_lifecycle&&)      = delete;

private:
   aws_sdk_lifecycle()  { Aws::InitAPI(_options); }
   ~aws_sdk_lifecycle() { Aws::ShutdownAPI(_options); }

   // TODO: the default-constructed SDKOptions leaves the AWS SDK's internal
   // logger disabled. To diagnose an AWS-side retry storm or credential-chain
   // failure from the node's own logs -- without restarting the node under the
   // AWS_LOG_LEVEL environment variable -- install an
   // Aws::Utils::Logging::LogSystemInterface here that forwards to fc::log
   // before the Aws::InitAPI call above.
   Aws::SDKOptions _options{};
};

} // namespace

void ensure_aws_sdk_initialized() {
   (void)aws_sdk_lifecycle::instance();
}

} // namespace sysio::sigprov::aws
