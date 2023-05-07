#include "envoy/extensions/filters/network/receiver_rdma_write_multip_write/v3/receiver_rdma_write_multip_write.pb.h"
#include "envoy/extensions/filters/network/receiver_rdma_write_multip_write/v3/receiver_rdma_write_multip_write.pb.validate.h"
#include "envoy/registry/registry.h"
#include "envoy/server/filter_config.h"
#include "source/common/common/logger.h"

#include "source/extensions/filters/network/common/factory_base.h"
#include "source/extensions/filters/network/receiver_rdma_write_multip_write/receiver_rdma_write_multip_write.h"
#include "source/extensions/filters/network/well_known_names.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace ReceiverRDMAWriteMultipWrite {

/**
 * Config registration for the receiver_rdma_write_multip_write filter. @see NamedNetworkFilterConfigFactory.
 */
class ReceiverRDMAWriteMultipWriteConfigFactory
    : public Common::FactoryBase<envoy::extensions::filters::network::receiver_rdma_write_multip_write::v3::ReceiverRDMAWriteMultipWrite>,
      Logger::Loggable<Logger::Id::filter> {
public:
  ReceiverRDMAWriteMultipWriteConfigFactory() : FactoryBase(NetworkFilterNames::get().ReceiverRDMAWriteMultipWrite) {}

private:
  Network::FilterFactoryCb
  createFilterFactoryFromProtoTyped(const envoy::extensions::filters::network::receiver_rdma_write_multip_write::v3::ReceiverRDMAWriteMultipWrite&,
                                    Server::Configuration::FactoryContext&) override {
    return [](Network::FilterManager& filter_manager) -> void {
      auto receiver_rdma_write_multip_write_filter = std::make_shared<ReceiverRDMAWriteMultipWriteFilter>();
      filter_manager.addReadFilter(receiver_rdma_write_multip_write_filter);
      filter_manager.addWriteFilter(receiver_rdma_write_multip_write_filter);
    };  
  }

  bool isTerminalFilterByProtoTyped(const envoy::extensions::filters::network::receiver_rdma_write_multip_write::v3::ReceiverRDMAWriteMultipWrite&,
                                    Server::Configuration::ServerFactoryContext&) override {
    return false;
  }
};

/**
 * Static registration for the receiver_rdma_write_multip_write filter. @see RegisterFactory.
 */
LEGACY_REGISTER_FACTORY(ReceiverRDMAWriteMultipWriteConfigFactory, Server::Configuration::NamedNetworkFilterConfigFactory,
                        "envoy.receiver_rdma_write_multip_write");

} // namespace ReceiverRDMAWriteMultipWrite
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
