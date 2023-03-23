load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_extension",
    "envoy_cc_library",
    "envoy_extension_package",
)

licenses(["notice"])  # Apache 2

# Echo L4 network filter. This is primarily a simplistic example.
# Public docs: https://envoyproxy.io/docs/envoy/latest/configuration/listeners/network_filters/echo_filter

envoy_extension_package()

envoy_cc_library(
    name = "receiver",
    srcs = ["receiver.cc"],
    hdrs = ["receiver.h"],
    deps = [
        "//envoy/buffer:buffer_interface",
        "//envoy/network:connection_interface",
        "//envoy/network:filter_interface",
        "//source/common/common:assert_lib",
        "//source/common/common:minimal_logger_lib",
        "//source/common/buffer:buffer_lib",
        "//envoy/event:dispatcher_interface"
    ],
)

envoy_cc_extension(
    name = "config",
    srcs = ["config.cc"],
    # The echo filter is used in integration tests which don't need an upstream.
    extra_visibility = [
        "//test/integration:__subpackages__",
    ],
    deps = [
        ":receiver",
        "//envoy/registry",
        "//envoy/server:filter_config_interface",
        "//source/extensions/filters/network:well_known_names",
        "//source/extensions/filters/network/common:factory_base_lib",
        "@envoy_api//envoy/extensions/filters/network/receiver/v3:pkg_cc_proto",
    ],
)
