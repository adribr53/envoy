#include "envoy/extensions/filters/network/sender_rdma/v3/sender_rdma.pb.h"
#include "envoy/extensions/filters/network/sender_rdma/v3/sender_rdma.pb.validate.h"
#include "envoy/registry/registry.h"
#include "envoy/server/filter_config.h"

#include "source/extensions/filters/network/common/factory_base.h"
#include "source/extensions/filters/network/sender_rdma/sender_rdma.h"
#include "source/extensions/filters/network/well_known_names.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace SenderRDMA {

/**
 * Config registration for the sender_rdma filter. @see NamedNetworkFilterConfigFactory.
 */
class SenderRDMAConfigFactory
    : public Common::FactoryBase<envoy::extensions::filters::network::sender_rdma::v3::SenderRDMA>,
      Logger::Loggable<Logger::Id::filter> {
public:
  SenderRDMAConfigFactory() : FactoryBase(NetworkFilterNames::get().SenderRDMA) {}

private:
  Network::FilterFactoryCb
  createFilterFactoryFromProtoTyped(const envoy::extensions::filters::network::sender_rdma::v3::SenderRDMA&,
                                    Server::Configuration::FactoryContext&) override {
    return [](Network::FilterManager& filter_manager) -> void {
      auto sender_rdma_filter = std::make_shared<SenderRDMAFilter>();
      filter_manager.addReadFilter(sender_rdma_filter);
      filter_manager.addWriteFilter(sender_rdma_filter);
    };
  }

  bool isTerminalFilterByProtoTyped(const envoy::extensions::filters::network::sender_rdma::v3::SenderRDMA&,
                                    Server::Configuration::ServerFactoryContext&) override {
    return false;
  }
};

/**
 * Static registration for the sender_rdma filter. @see RegisterFactory.
 */
LEGACY_REGISTER_FACTORY(SenderRDMAConfigFactory, Server::Configuration::NamedNetworkFilterConfigFactory,
                        "envoy.sender_rdma");

} // namespace SenderRDMA
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
