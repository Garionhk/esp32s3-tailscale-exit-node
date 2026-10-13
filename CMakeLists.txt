cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.3.2")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# lwIP NAPT tuning for exit-node traffic. These are #ifndef defaults inside
# lwip_napt.h with no Kconfig knob, so they are set build-wide here.
#
# Port range: lwIP's default 49152-61439 contains 51820, the port MicroLink
# uses for WireGuard/DISCO on the Wi-Fi address. ip_napt_new_port() can hand
# out a client's own source port without checking for a local listener, so a
# flow that happened to pick 51820 would capture the tunnel's replies. This
# range sits clear of it and of lwIP's own ephemeral range (49152+).
#
# UDP timeout: the default 2 s drops idle QUIC/VoIP mappings mid-session.
idf_build_set_property(COMPILE_DEFINITIONS "IP_NAPT_PORT_RANGE_START=20000" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "IP_NAPT_PORT_RANGE_END=39999" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "IP_NAPT_TIMEOUT_MS_UDP=30000" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "IP_NAPT_TIMEOUT_MS_ICMP=5000" APPEND)

project(ts-exit-node)
