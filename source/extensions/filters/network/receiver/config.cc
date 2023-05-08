#include "envoy/extensions/filters/network/receiver/v3/receiver.pb.h"
#include "envoy/extensions/filters/network/receiver/v3/receiver.pb.validate.h"
#include "envoy/registry/registry.h"
#include "envoy/server/filter_config.h"
#include "source/common/common/logger.h"

#include "source/extensions/filters/network/common/factory_base.h"
#include "source/extensions/filters/network/receiver/receiver.h"
#include "source/extensions/filters/network/well_known_names.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace Receiver {

/**
 * Config registration for the receiver filter. @see NamedNetworkFilterConfigFactory.
 */
class ReceiverConfigFactory
    : public Common::FactoryBase<envoy::extensions::filters::network::receiver::v3::Receiver>,
      Logger::Loggable<Logger::Id::filter> {
public:
  ReceiverConfigFactory() : FactoryBase(NetworkFilterNames::get().Receiver) {}

private:
  Network::FilterFactoryCb
  createFilterFactoryFromProtoTyped(const envoy::extensions::filters::network::receiver::v3::Receiver& proto_config,
                                    Server::Configuration::FactoryContext&) override {
    const uint32_t payloadBound = proto_config.payload_bound();
    const uint32_t circleSize = proto_config.circle_size();
    const uint32_t timeToWrite = proto_config.time_to_write();
    const uint32_t sharedBufferSize = proto_config.shared_buffer_size();

    return [payloadBound, circleSize, timeToWrite, sharedBufferSize](Network::FilterManager& filter_manager) -> void {
      auto receiver_filter = std::make_shared<ReceiverFilter>(payloadBound, circleSize, timeToWrite, sharedBufferSize);
      filter_manager.addReadFilter(receiver_filter);
      filter_manager.addWriteFilter(receiver_filter);
    };  
  }

  bool isTerminalFilterByProtoTyped(const envoy::extensions::filters::network::receiver::v3::Receiver&,
                                    Server::Configuration::ServerFactoryContext&) override {
    return false;
  }
};

/**
 * Static registration for the receiver filter. @see RegisterFactory.
 */
LEGACY_REGISTER_FACTORY(ReceiverConfigFactory, Server::Configuration::NamedNetworkFilterConfigFactory,
                        "envoy.receiver");

} // namespace Receiver
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
