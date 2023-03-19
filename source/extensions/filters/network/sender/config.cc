#include "envoy/extensions/filters/network/sender/v3/sender.pb.h"
#include "envoy/extensions/filters/network/sender/v3/sender.pb.validate.h"
#include "envoy/registry/registry.h"
#include "envoy/server/filter_config.h"
#include "source/common/common/logger.h"

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
    : public Common::FactoryBase<envoy::extensions::filters::network::sender::v3::Sender>,
      Logger::Loggable<Logger::Id::filter> {
public:
  SenderConfigFactory() : FactoryBase(NetworkFilterNames::get().Sender) {}

private:
  Network::FilterFactoryCb
  createFilterFactoryFromProtoTyped(const envoy::extensions::filters::network::sender::v3::Sender& proto_config,
                                    Server::Configuration::FactoryContext& context) override {

    const auto& destination_cluster = proto_config.destination_cluster();
    auto& cluster_manager = context.clusterManager();
    auto* cluster = cluster_manager.getThreadLocalCluster(destination_cluster);

    if (cluster != nullptr) {
      const auto host = cluster->loadBalancer().chooseHost(nullptr);
      if (host != nullptr) {
        const auto address = host->address();
        ENVOY_LOG(info, "DESTINATION ADDRESS: {}:{}", address->ip()->addressAsString(), address->ip()->port());
        
        return [address](Network::FilterManager& filter_manager) -> void {
          auto sender_filter = std::make_shared<SenderFilter>(address->ip()->addressAsString(), address->ip()->port());
          filter_manager.addReadFilter(sender_filter);
          filter_manager.addWriteFilter(sender_filter);
        };
      }
      else {
        throw EnvoyException("Unknown host");
      }
    }
    else {
      throw EnvoyException("Unknown cluster: " + destination_cluster);
    }
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
