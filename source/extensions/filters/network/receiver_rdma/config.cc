#include "envoy/extensions/filters/network/receiver_rdma/v3/receiver_rdma.pb.h"
#include "envoy/extensions/filters/network/receiver_rdma/v3/receiver_rdma.pb.validate.h"
#include "envoy/registry/registry.h"
#include "envoy/server/filter_config.h"
#include "source/common/common/logger.h"

#include "source/extensions/filters/network/common/factory_base.h"
#include "source/extensions/filters/network/receiver_rdma/receiver_rdma.h"
#include "source/extensions/filters/network/well_known_names.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace ReceiverRDMA {

/**
 * Config registration for the receiver_rdma filter. @see NamedNetworkFilterConfigFactory.
 */
class ReceiverRDMAConfigFactory
    : public Common::FactoryBase<envoy::extensions::filters::network::receiver_rdma::v3::ReceiverRDMA>,
      Logger::Loggable<Logger::Id::filter> {
public:
  ReceiverRDMAConfigFactory() : FactoryBase(NetworkFilterNames::get().ReceiverRDMA) {}

private:
  Network::FilterFactoryCb
  createFilterFactoryFromProtoTyped(const envoy::extensions::filters::network::receiver_rdma::v3::ReceiverRDMA& proto_config,
                                    Server::Configuration::FactoryContext&) override {
    const uint32_t payloadBound = proto_config.payload_bound();
    const uint32_t circleSize = proto_config.circle_size();
    const uint32_t timeToWrite = proto_config.time_to_write();
    const uint32_t sharedBufferSize = proto_config.shared_buffer_size();

    return [payloadBound, circleSize, timeToWrite, sharedBufferSize](Network::FilterManager& filter_manager) -> void {
      auto receiver_rdma_filter = std::make_shared<ReceiverRDMAFilter>(payloadBound, circleSize, timeToWrite, sharedBufferSize);
      filter_manager.addReadFilter(receiver_rdma_filter);
      filter_manager.addWriteFilter(receiver_rdma_filter);
    };  
  }

  bool isTerminalFilterByProtoTyped(const envoy::extensions::filters::network::receiver_rdma::v3::ReceiverRDMA&,
                                    Server::Configuration::ServerFactoryContext&) override {
    return false;
  }
};

/**
 * Static registration for the receiver_rdma filter. @see RegisterFactory.
 */
LEGACY_REGISTER_FACTORY(ReceiverRDMAConfigFactory, Server::Configuration::NamedNetworkFilterConfigFactory,
                        "envoy.receiver_rdma");

} // namespace ReceiverRDMA
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
