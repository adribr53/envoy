#include "envoy/extensions/filters/network/sender/v3/sender.pb.h"
#include "envoy/extensions/filters/network/sender/v3/sender.pb.validate.h"
#include "envoy/registry/registry.h"
#include "envoy/server/filter_config.h"

#include "source/extensions/filters/network/common/factory_base.h"
#include "source/extensions/filters/network/sender/sender.h"
#include "source/extensions/filters/network/well_known_names.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace Sender {

/**
 * Config registration for the sender filter. @see NamedNetworkFilterConfigFactory.
 */
class SenderConfigFactory
    : public Common::FactoryBase<envoy::extensions::filters::network::sender::v3::Sender> {
public:
  SenderConfigFactory() : FactoryBase(NetworkFilterNames::get().Sender) {}

private:
  Network::FilterFactoryCb
  createFilterFactoryFromProtoTyped(const envoy::extensions::filters::network::sender::v3::Sender&,
                                    Server::Configuration::FactoryContext& context) override {
    Envoy::Thread::ThreadFactory& thread_factory = context.api().threadFactory();
    return [&thread_factory](Network::FilterManager& filter_manager) -> void {
      auto sender_filter = std::make_shared<SenderFilter>(thread_factory);
      filter_manager.addReadFilter(sender_filter);
      filter_manager.addWriteFilter(sender_filter);
    };
  }

  bool isTerminalFilterByProtoTyped(const envoy::extensions::filters::network::sender::v3::Sender&,
                                    Server::Configuration::ServerFactoryContext&) override {
    return false;
  }
};

/**
 * Static registration for the sender filter. @see RegisterFactory.
 */
LEGACY_REGISTER_FACTORY(SenderConfigFactory, Server::Configuration::NamedNetworkFilterConfigFactory,
                        "envoy.sender");

} // namespace Sender
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
