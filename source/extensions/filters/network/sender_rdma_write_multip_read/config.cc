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
  createFilterFactoryFromProtoTyped(const envoy::extensions::filters::network::sender_rdma_write_multip_read::v3::SenderRDMAWriteMultipRead& proto_config,
                                    Server::Configuration::FactoryContext&) override {
    const uint32_t payloadBound = proto_config.payload_bound();
    const uint32_t circleSize = proto_config.circle_size();
    const uint32_t timeToWrite = proto_config.time_to_write();
    const uint32_t sharedBufferSize = proto_config.shared_buffer_size();

    return [payloadBound, circleSize, timeToWrite, sharedBufferSize](Network::FilterManager& filter_manager) -> void {
      auto sender_rdma_write_multip_read_filter = std::make_shared<SenderRDMAWriteMultipReadFilter>(payloadBound, circleSize, timeToWrite, sharedBufferSize);
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
