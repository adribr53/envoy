#include "envoy/extensions/filters/network/receiver/v3/receiver.pb.h"
#include "envoy/extensions/filters/network/receiver/v3/receiver.pb.validate.h"
#include "envoy/registry/registry.h"
#include "envoy/server/filter_config.h"
#include "source/common/common/logger.h"

#include "source/extensions/filters/network/common/factory_base.h"
#include "source/extensions/filters/network/receiver/receiver.h"
#include "source/extensions/filters/network/well_known_names.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace Receiver {

/**
 * Config registration for the receiver filter. @see NamedNetworkFilterConfigFactory.
 */
class ReceiverConfigFactory
    : public Common::FactoryBase<envoy::extensions::filters::network::receiver::v3::Receiver>,
      Logger::Loggable<Logger::Id::filter> {
public:
  ReceiverConfigFactory() : FactoryBase(NetworkFilterNames::get().Receiver) {}

private:
  Network::FilterFactoryCb
  createFilterFactoryFromProtoTyped(const envoy::extensions::filters::network::receiver::v3::Receiver& proto_config,
                                    Server::Configuration::FactoryContext& context) override {

    const auto& destination_cluster = proto_config.destination_cluster();
    auto& cluster_manager = context.clusterManager();
    auto* cluster = cluster_manager.getThreadLocalCluster(destination_cluster);

    Envoy::Thread::ThreadFactory& thread_factory = context.api().threadFactory();

    if (cluster != nullptr) {
      const auto host = cluster->loadBalancer().chooseHost(nullptr);
      if (host != nullptr) {
        const auto address = host->address();
        ENVOY_LOG(info, "DESTINATION ADDRESS: {}:{}", address->ip()->addressAsString(), address->ip()->port());
        
        return [address, &thread_factory](Network::FilterManager& filter_manager) -> void {
          auto receiver_filter = std::make_shared<ReceiverFilter>(address->ip()->addressAsString(), address->ip()->port(), thread_factory);
          filter_manager.addReadFilter(receiver_filter);
          filter_manager.addWriteFilter(receiver_filter);
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

  bool isTerminalFilterByProtoTyped(const envoy::extensions::filters::network::receiver::v3::Receiver&,
                                    Server::Configuration::ServerFactoryContext&) override {
    return false;
  }
};

/**
 * Static registration for the receiver filter. @see RegisterFactory.
 */
LEGACY_REGISTER_FACTORY(ReceiverConfigFactory, Server::Configuration::NamedNetworkFilterConfigFactory,
                        "envoy.receiver");

} // namespace Receiver
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
