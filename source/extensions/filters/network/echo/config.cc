#include "envoy/extensions/filters/network/echo/v3/echo.pb.h"
#include "envoy/extensions/filters/network/echo/v3/echo.pb.validate.h"
#include "envoy/registry/registry.h"
#include "envoy/server/filter_config.h"
#include "source/common/common/logger.h"

#include "source/extensions/filters/network/common/factory_base.h"
#include "source/extensions/filters/network/echo/echo.h"
#include "source/extensions/filters/network/well_known_names.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace Echo {

/**
 * Config registration for the echo filter. @see NamedNetworkFilterConfigFactory.
 */
class EchoConfigFactory
    : public Common::FactoryBase<envoy::extensions::filters::network::echo::v3::Echo>,
      Logger::Loggable<Logger::Id::filter> {
public:
  EchoConfigFactory() : FactoryBase(NetworkFilterNames::get().Echo) {}

private:
  Network::FilterFactoryCb
  createFilterFactoryFromProtoTyped(const envoy::extensions::filters::network::echo::v3::Echo& proto_config,
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
          auto echo_filter = std::make_shared<EchoFilter>(address->ip()->addressAsString(), address->ip()->port());
          filter_manager.addReadFilter(echo_filter);
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

  bool isTerminalFilterByProtoTyped(const envoy::extensions::filters::network::echo::v3::Echo&,
                                    Server::Configuration::ServerFactoryContext&) override {
    return true;
  }
};

/**
 * Static registration for the echo filter. @see RegisterFactory.
 */
LEGACY_REGISTER_FACTORY(EchoConfigFactory, Server::Configuration::NamedNetworkFilterConfigFactory,
                        "envoy.echo");

} // namespace Echo
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
