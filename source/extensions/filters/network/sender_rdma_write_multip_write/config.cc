#include "envoy/extensions/filters/network/sender_rdma_write_multip_write/v3/sender_rdma_write_multip_write.pb.h"
#include "envoy/extensions/filters/network/sender_rdma_write_multip_write/v3/sender_rdma_write_multip_write.pb.validate.h"
#include "envoy/registry/registry.h"
#include "envoy/server/filter_config.h"

#include "source/extensions/filters/network/common/factory_base.h"
#include "source/extensions/filters/network/sender_rdma_write_multip_write/sender_rdma_write_multip_write.h"
#include "source/extensions/filters/network/well_known_names.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace SenderRDMAWriteMultipWrite {

/**
 * Config registration for the sender_rdma_write_multip_write filter. @see NamedNetworkFilterConfigFactory.
 */
class SenderRDMAWriteMultipWriteConfigFactory
    : public Common::FactoryBase<envoy::extensions::filters::network::sender_rdma_write_multip_write::v3::SenderRDMAWriteMultipWrite>,
      Logger::Loggable<Logger::Id::filter> {
public:
  SenderRDMAWriteMultipWriteConfigFactory() : FactoryBase(NetworkFilterNames::get().SenderRDMAWriteMultipWrite) {}

private:
  Network::FilterFactoryCb
  createFilterFactoryFromProtoTyped(const envoy::extensions::filters::network::sender_rdma_write_multip_write::v3::SenderRDMAWriteMultipWrite&,
                                    Server::Configuration::FactoryContext&) override {
    return [](Network::FilterManager& filter_manager) -> void {
      auto sender_rdma_write_multip_write_filter = std::make_shared<SenderRDMAWriteMultipWriteFilter>();
      filter_manager.addReadFilter(sender_rdma_write_multip_write_filter);
      filter_manager.addWriteFilter(sender_rdma_write_multip_write_filter);
    };
  }

  bool isTerminalFilterByProtoTyped(const envoy::extensions::filters::network::sender_rdma_write_multip_write::v3::SenderRDMAWriteMultipWrite&,
                                    Server::Configuration::ServerFactoryContext&) override {
    return false;
  }
};

/**
 * Static registration for the sender_rdma_write_multip_write filter. @see RegisterFactory.
 */
LEGACY_REGISTER_FACTORY(SenderRDMAWriteMultipWriteConfigFactory, Server::Configuration::NamedNetworkFilterConfigFactory,
                        "envoy.sender_rdma_write_multip_write");

} // namespace SenderRDMAWriteMultipWrite
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
