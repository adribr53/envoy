#include "envoy/extensions/filters/network/sender_rdma_write_multip_read/v3/sender_rdma_write_multip_read.pb.h"
#include "envoy/extensions/filters/network/sender_rdma_write_multip_read/v3/sender_rdma_write_multip_read.pb.validate.h"
#include "envoy/registry/registry.h"
#include "envoy/server/filter_config.h"

#include "source/extensions/filters/network/common/factory_base.h"
#include "source/extensions/filters/network/sender_rdma_write_multip_read/sender_rdma_write_multip_read.h"
#include "source/extensions/filters/network/well_known_names.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace SenderRDMAWriteMultipRead {

/**
 * Config registration for the sender_rdma_write_multip_read filter. @see NamedNetworkFilterConfigFactory.
 */
class SenderRDMAWriteMultipReadConfigFactory
    : public Common::FactoryBase<envoy::extensions::filters::network::sender_rdma_write_multip_read::v3::SenderRDMAWriteMultipRead>,
      Logger::Loggable<Logger::Id::filter> {
public:
  SenderRDMAWriteMultipReadConfigFactory() : FactoryBase(NetworkFilterNames::get().SenderRDMAWriteMultipRead) {}

private:
  Network::FilterFactoryCb
  createFilterFactoryFromProtoTyped(const envoy::extensions::filters::network::sender_rdma_write_multip_read::v3::SenderRDMAWriteMultipRead&,
                                    Server::Configuration::FactoryContext&) override {
    return [](Network::FilterManager& filter_manager) -> void {
      auto sender_rdma_write_multip_read_filter = std::make_shared<SenderRDMAWriteMultipReadFilter>();
      filter_manager.addReadFilter(sender_rdma_write_multip_read_filter);
      filter_manager.addWriteFilter(sender_rdma_write_multip_read_filter);
    };
  }

  bool isTerminalFilterByProtoTyped(const envoy::extensions::filters::network::sender_rdma_write_multip_read::v3::SenderRDMAWriteMultipRead&,
                                    Server::Configuration::ServerFactoryContext&) override {
    return false;
  }
};

/**
 * Static registration for the sender_rdma_write_multip_read filter. @see RegisterFactory.
 */
LEGACY_REGISTER_FACTORY(SenderRDMAWriteMultipReadConfigFactory, Server::Configuration::NamedNetworkFilterConfigFactory,
                        "envoy.sender_rdma_write_multip_read");

} // namespace SenderRDMAWriteMultipRead
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
