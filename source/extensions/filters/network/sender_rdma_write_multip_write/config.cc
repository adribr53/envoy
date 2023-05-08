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
  createFilterFactoryFromProtoTyped(const envoy::extensions::filters::network::sender_rdma_write_multip_write::v3::SenderRDMAWriteMultipWrite& proto_config,
                                    Server::Configuration::FactoryContext&) override {
    const uint32_t payloadBound = proto_config.payload_bound();
    const uint32_t circleSize = proto_config.circle_size();
    const uint32_t timeToWrite = proto_config.time_to_write();
    const uint32_t sharedBufferSize = proto_config.shared_buffer_size();

    return [payloadBound, circleSize, timeToWrite, sharedBufferSize](Network::FilterManager& filter_manager) -> void {
      auto sender_rdma_write_multip_write_filter = std::make_shared<SenderRDMAWriteMultipWriteFilter>(payloadBound, circleSize, timeToWrite, sharedBufferSize);
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
