#include "envoy/extensions/filters/network/receiver_rdma_write_multip_read/v3/receiver_rdma_write_multip_read.pb.h"
#include "envoy/extensions/filters/network/receiver_rdma_write_multip_read/v3/receiver_rdma_write_multip_read.pb.validate.h"
#include "envoy/registry/registry.h"
#include "envoy/server/filter_config.h"
#include "source/common/common/logger.h"

#include "source/extensions/filters/network/common/factory_base.h"
#include "source/extensions/filters/network/receiver_rdma_write_multip_read/receiver_rdma_write_multip_read.h"
#include "source/extensions/filters/network/well_known_names.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace ReceiverRDMAWriteMultipRead {

/**
 * Config registration for the receiver_rdma_write_multip_read filter. @see NamedNetworkFilterConfigFactory.
 */
class ReceiverRDMAWriteMultipReadConfigFactory
    : public Common::FactoryBase<envoy::extensions::filters::network::receiver_rdma_write_multip_read::v3::ReceiverRDMAWriteMultipRead>,
      Logger::Loggable<Logger::Id::filter> {
public:
  ReceiverRDMAWriteMultipReadConfigFactory() : FactoryBase(NetworkFilterNames::get().ReceiverRDMAWriteMultipRead) {}

private:
  Network::FilterFactoryCb
  createFilterFactoryFromProtoTyped(const envoy::extensions::filters::network::receiver_rdma_write_multip_read::v3::ReceiverRDMAWriteMultipRead&,
                                    Server::Configuration::FactoryContext&) override {
    return [](Network::FilterManager& filter_manager) -> void {
      auto receiver_rdma_write_multip_read_filter = std::make_shared<ReceiverRDMAWriteMultipReadFilter>();
      filter_manager.addReadFilter(receiver_rdma_write_multip_read_filter);
      filter_manager.addWriteFilter(receiver_rdma_write_multip_read_filter);
    };  
  }

  bool isTerminalFilterByProtoTyped(const envoy::extensions::filters::network::receiver_rdma_write_multip_read::v3::ReceiverRDMAWriteMultipRead&,
                                    Server::Configuration::ServerFactoryContext&) override {
    return false;
  }
};

/**
 * Static registration for the receiver_rdma_write_multip_read filter. @see RegisterFactory.
 */
LEGACY_REGISTER_FACTORY(ReceiverRDMAWriteMultipReadConfigFactory, Server::Configuration::NamedNetworkFilterConfigFactory,
                        "envoy.receiver_rdma_write_multip_read");

} // namespace ReceiverRDMAWriteMultipRead
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
