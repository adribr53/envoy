#include "envoy/extensions/filters/network/receiver_rdma_write_nomultip_read/v3/receiver_rdma_write_nomultip_read.pb.h"
#include "envoy/extensions/filters/network/receiver_rdma_write_nomultip_read/v3/receiver_rdma_write_nomultip_read.pb.validate.h"
#include "envoy/registry/registry.h"
#include "envoy/server/filter_config.h"
#include "source/common/common/logger.h"

#include "source/extensions/filters/network/common/factory_base.h"
#include "source/extensions/filters/network/receiver_rdma_write_nomultip_read/receiver_rdma_write_nomultip_read.h"
#include "source/extensions/filters/network/well_known_names.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace ReceiverRDMAWriteNomultipRead {

/**
 * Config registration for the receiver_rdma_write_nomultip_read filter. @see NamedNetworkFilterConfigFactory.
 */
class ReceiverRDMAWriteNomultipReadConfigFactory
    : public Common::FactoryBase<envoy::extensions::filters::network::receiver_rdma_write_nomultip_read::v3::ReceiverRDMAWriteNomultipRead>,
      Logger::Loggable<Logger::Id::filter> {
public:
  ReceiverRDMAWriteNomultipReadConfigFactory() : FactoryBase(NetworkFilterNames::get().ReceiverRDMAWriteNomultipRead) {}

private:
  Network::FilterFactoryCb
  createFilterFactoryFromProtoTyped(const envoy::extensions::filters::network::receiver_rdma_write_nomultip_read::v3::ReceiverRDMAWriteNomultipRead&,
                                    Server::Configuration::FactoryContext&) override {
    return [](Network::FilterManager& filter_manager) -> void {
      auto receiver_rdma_write_nomultip_read_filter = std::make_shared<ReceiverRDMAWriteNomultipReadFilter>();
      filter_manager.addReadFilter(receiver_rdma_write_nomultip_read_filter);
      filter_manager.addWriteFilter(receiver_rdma_write_nomultip_read_filter);
    };  
  }

  bool isTerminalFilterByProtoTyped(const envoy::extensions::filters::network::receiver_rdma_write_nomultip_read::v3::ReceiverRDMAWriteNomultipRead&,
                                    Server::Configuration::ServerFactoryContext&) override {
    return false;
  }
};

/**
 * Static registration for the receiver_rdma_write_nomultip_read filter. @see RegisterFactory.
 */
LEGACY_REGISTER_FACTORY(ReceiverRDMAWriteNomultipReadConfigFactory, Server::Configuration::NamedNetworkFilterConfigFactory,
                        "envoy.receiver_rdma_write_nomultip_read");

} // namespace ReceiverRDMAWriteNomultipRead
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
