/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * Copyright (c) 2014-2016,  Regents of the University of California,
 *                           Arizona Board of Regents,
 *                           Colorado State University,
 *                           University Pierre & Marie Curie, Sorbonne University,
 *                           Washington University in St. Louis,
 *                           Beijing Institute of Technology,
 *                           The University of Memphis.
 *
 * This file is part of NFD (Named Data Networking Forwarding Daemon).
 * See AUTHORS.md for complete list of NFD authors and contributors.
 *
 * NFD is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * NFD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * NFD, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "face-system.hpp"
#include "core/logger.hpp"
#include "core/network-interface.hpp"
#include "core/network-interface-predicate.hpp"
#include "fw/face-table.hpp"

// ProtocolFactory includes, sorted alphabetically
#ifdef HAVE_LIBPCAP
#include "ethernet-factory.hpp"
#include "ethernet-transport.hpp"
#endif // HAVE_LIBPCAP
#include "tcp-factory.hpp"
#include "udp-factory.hpp"
#ifdef HAVE_UNIX_SOCKETS
#include "unix-stream-factory.hpp"
#endif // HAVE_UNIX_SOCKETS
#ifdef HAVE_WEBSOCKET
#include "websocket-factory.hpp"
#endif // HAVE_WEBSOCKET

namespace nfd {
namespace face {

NFD_LOG_INIT("FaceSystem");

FaceSystem::FaceSystem(FaceTable& faceTable)
  : m_faceTable(faceTable)
{
}

std::set<const ProtocolFactory*>
FaceSystem::listProtocolFactories() const
{
  std::set<const ProtocolFactory*> factories;
  for (const auto& p : m_factories) {
    factories.insert(p.second.get());
  }
  return factories;
}

ProtocolFactory*
FaceSystem::getProtocolFactory(const std::string& scheme)
{
  auto found = m_factories.find(scheme);
  return found == m_factories.end() ? nullptr : found->second.get();
}

void
FaceSystem::setConfigFile(ConfigFile& configFile)
{
  configFile.addSectionHandler("face_system", bind(&FaceSystem::processConfig, this, _1, _2, _3));
}

void
FaceSystem::processConfig(const ConfigSection& configSection, bool isDryRun, const std::string& filename)
{
  std::set<std::string> seenSections;
  auto nicList = listNetworkInterfaces();

  for (const auto& item : configSection) {
    if (!seenSections.insert(item.first).second) {
      BOOST_THROW_EXCEPTION(ConfigFile::Error("Duplicate \"" + item.first + "\" section"));
    }

    if (item.first == "unix") {
      processSectionUnix(item.second, isDryRun);
    }
    else if (item.first == "tcp") {
      processSectionTcp(item.second, isDryRun);
    }
    else if (item.first == "udp") {
      processSectionUdp(item.second, isDryRun, nicList);
    }
    else if (item.first == "ether") {
      processSectionEther(item.second, isDryRun, nicList);
    }
    else if (item.first == "websocket") {
      processSectionWebSocket(item.second, isDryRun);
    }
    else {
      BOOST_THROW_EXCEPTION(ConfigFile::Error("Unrecognized option \"" + item.first + "\""));
    }
  }
}

void
FaceSystem::processSectionUnix(const ConfigSection& configSection, bool isDryRun)
{
  // ; the unix section contains settings of Unix stream faces and channels
  // unix
  // {
  //   path /var/run/nfd.sock ; Unix stream listener path
  // }

#if defined(HAVE_UNIX_SOCKETS)
  std::string path = "/var/run/nfd.sock";

  for (const auto& i : configSection) {
    if (i.first == "path") {
      path = i.second.get_value<std::string>();
    }
    else {
      BOOST_THROW_EXCEPTION(ConfigFile::Error("Unrecognized option \"" +
                                              i.first + "\" in \"unix\" section"));
    }
  }

  if (!isDryRun) {
    if (m_factories.count("unix") > 0) {
      return;
    }

    auto factory = make_shared<UnixStreamFactory>();
    m_factories.emplace("unix", factory);

    auto channel = factory->createChannel(path);
    channel->listen(bind(&FaceTable::add, &m_faceTable, _1), nullptr);
  }
#else
  BOOST_THROW_EXCEPTION(ConfigFile::Error("NFD was compiled without Unix sockets support, "
                                          "cannot process \"unix\" section"));
#endif // HAVE_UNIX_SOCKETS
}

void
FaceSystem::processSectionTcp(const ConfigSection& configSection, bool isDryRun)
{
  // ; the tcp section contains settings of TCP faces and channels
  // tcp
  // {
  //   listen yes ; set to 'no' to disable TCP listener, default 'yes'
  //   port 6363 ; TCP listener port number
  // }

  uint16_t port = 6363;
  bool needToListen = true;
  bool enableV4 = true;
  bool enableV6 = true;

  for (const auto& i : configSection) {
    if (i.first == "port") {
      port = ConfigFile::parseNumber<uint16_t>(i, "tcp");
      NFD_LOG_TRACE("TCP port set to " << port);
    }
    else if (i.first == "listen") {
      needToListen = ConfigFile::parseYesNo(i, "tcp");
    }
    else if (i.first == "enable_v4") {
      enableV4 = ConfigFile::parseYesNo(i, "tcp");
    }
    else if (i.first == "enable_v6") {
      enableV6 = ConfigFile::parseYesNo(i, "tcp");
    }
    else {
      BOOST_THROW_EXCEPTION(ConfigFile::Error("Unrecognized option \"" +
                                              i.first + "\" in \"tcp\" section"));
    }
  }

  if (!enableV4 && !enableV6) {
    BOOST_THROW_EXCEPTION(ConfigFile::Error("IPv4 and IPv6 TCP channels have been disabled."
                                            " Remove \"tcp\" section to disable TCP channels or"
                                            " re-enable at least one channel type."));
  }

  if (!isDryRun) {
    if (m_factories.count("tcp") > 0) {
      return;
    }

    auto factory = make_shared<TcpFactory>();
    m_factories.emplace("tcp", factory);

    if (enableV4) {
      tcp::Endpoint endpoint(boost::asio::ip::tcp::v4(), port);
      shared_ptr<TcpChannel> v4Channel = factory->createChannel(endpoint);
      if (needToListen) {
        v4Channel->listen(bind(&FaceTable::add, &m_faceTable, _1), nullptr);
      }

      m_factories.emplace("tcp4", factory);
    }

    if (enableV6) {
      tcp::Endpoint endpoint(boost::asio::ip::tcp::v6(), port);
      shared_ptr<TcpChannel> v6Channel = factory->createChannel(endpoint);
      if (needToListen) {
        v6Channel->listen(bind(&FaceTable::add, &m_faceTable, _1), nullptr);
      }

      m_factories.emplace("tcp6", factory);
    }
  }
}

void
FaceSystem::processSectionUdp(const ConfigSection& configSection, bool isDryRun,
                              const std::vector<NetworkInterfaceInfo>& nicList)
{
  // ; the udp section contains settings of UDP faces and channels
  // udp
  // {
  //   port 6363 ; UDP unicast port number
  //   idle_timeout 600 ; idle time (seconds) before closing a UDP unicast face
  //   keep_alive_interval 25 ; interval (seconds) between keep-alive refreshes

  //   ; NFD creates one UDP multicast face per NIC
  //   mcast yes ; set to 'no' to disable UDP multicast, default 'yes'
  //   mcast_port 56363 ; UDP multicast port number
  //   mcast_group 224.0.23.170 ; UDP multicast group (IPv4 only)
  // }

  uint16_t port = 6363;
  bool enableV4 = true;
  bool enableV6 = true;
  size_t timeout = 600;
  size_t keepAliveInterval = 25;
  bool useMcast = true;
  auto mcastGroup = boost::asio::ip::address_v4::from_string("224.0.23.170");
  uint16_t mcastPort = 56363;

  for (const auto& i : configSection) {
    if (i.first == "port") {
      port = ConfigFile::parseNumber<uint16_t>(i, "udp");
      NFD_LOG_TRACE("UDP unicast port set to " << port);
    }
    else if (i.first == "enable_v4") {
      enableV4 = ConfigFile::parseYesNo(i, "udp");
    }
    else if (i.first == "enable_v6") {
      enableV6 = ConfigFile::parseYesNo(i, "udp");
    }
    else if (i.first == "idle_timeout") {
      try {
        timeout = i.second.get_value<size_t>();
      }
      catch (const boost::property_tree::ptree_bad_data&) {
        BOOST_THROW_EXCEPTION(ConfigFile::Error("Invalid value for option \"" +
                                                i.first + "\" in \"udp\" section"));
      }
    }
    else if (i.first == "keep_alive_interval") {
      try {
        keepAliveInterval = i.second.get_value<size_t>();
        /// \todo Make use of keepAliveInterval
        (void)(keepAliveInterval);
      }
      catch (const boost::property_tree::ptree_bad_data&) {
        BOOST_THROW_EXCEPTION(ConfigFile::Error("Invalid value for option \"" +
                                                i.first + "\" in \"udp\" section"));
      }
    }
    else if (i.first == "mcast") {
      useMcast = ConfigFile::parseYesNo(i, "udp");
    }
    else if (i.first == "mcast_port") {
      mcastPort = ConfigFile::parseNumber<uint16_t>(i, "udp");
      NFD_LOG_TRACE("UDP multicast port set to " << mcastPort);
    }
    else if (i.first == "mcast_group") {
      boost::system::error_code ec;
      mcastGroup = boost::asio::ip::address_v4::from_string(i.second.get_value<std::string>(), ec);
      if (ec) {
        BOOST_THROW_EXCEPTION(ConfigFile::Error("Invalid value for option \"" +
                                                i.first + "\" in \"udp\" section"));
      }
      NFD_LOG_TRACE("UDP multicast group set to " << mcastGroup);
    }
    else {
      BOOST_THROW_EXCEPTION(ConfigFile::Error("Unrecognized option \"" +
                                              i.first + "\" in \"udp\" section"));
    }
  }

  if (!enableV4 && !enableV6) {
    BOOST_THROW_EXCEPTION(ConfigFile::Error("IPv4 and IPv6 UDP channels have been disabled."
                                            " Remove \"udp\" section to disable UDP channels or"
                                            " re-enable at least one channel type."));
  }
  else if (useMcast && !enableV4) {
    BOOST_THROW_EXCEPTION(ConfigFile::Error("IPv4 multicast requested, but IPv4 channels"
                                            " have been disabled (conflicting configuration options set)"));
  }

  if (!isDryRun) {
    shared_ptr<UdpFactory> factory;
    bool isReload = false;
    if (m_factories.count("udp") > 0) {
      isReload = true;
      factory = static_pointer_cast<UdpFactory>(m_factories["udp"]);
    }
    else {
      factory = make_shared<UdpFactory>();
      m_factories.emplace("udp", factory);
    }

    if (!isReload && enableV4) {
      udp::Endpoint endpoint(boost::asio::ip::udp::v4(), port);
      shared_ptr<UdpChannel> v4Channel = factory->createChannel(endpoint, time::seconds(timeout));
      v4Channel->listen(bind(&FaceTable::add, &m_faceTable, _1), nullptr);

      m_factories.emplace("udp4", factory);
    }

    if (!isReload && enableV6) {
      udp::Endpoint endpoint(boost::asio::ip::udp::v6(), port);
      shared_ptr<UdpChannel> v6Channel = factory->createChannel(endpoint, time::seconds(timeout));
      v6Channel->listen(bind(&FaceTable::add, &m_faceTable, _1), nullptr);

      m_factories.emplace("udp6", factory);
    }

    std::set<shared_ptr<Face>> multicastFacesToRemove;
    for (const auto& i : factory->getMulticastFaces()) {
      multicastFacesToRemove.insert(i.second);
    }

    if (useMcast && enableV4) {
      std::vector<NetworkInterfaceInfo> ipv4MulticastInterfaces;
      for (const auto& nic : nicList) {
        if (nic.isUp() && nic.isMulticastCapable() && !nic.ipv4Addresses.empty()) {
          ipv4MulticastInterfaces.push_back(nic);
        }
      }

      bool isNicNameNecessary = false;
#if defined(__linux__)
      if (ipv4MulticastInterfaces.size() > 1) {
        // On Linux if we have more than one MulticastUdpFace
        // we need to specify the name of the interface
        isNicNameNecessary = true;
      }
#endif

      udp::Endpoint mcastEndpoint(mcastGroup, mcastPort);
      for (const auto& nic : ipv4MulticastInterfaces) {
        udp::Endpoint localEndpoint(nic.ipv4Addresses[0], mcastPort);
        auto newFace = factory->createMulticastFace(localEndpoint, mcastEndpoint,
                                                    isNicNameNecessary ? nic.name : "");
        m_faceTable.add(newFace);
        multicastFacesToRemove.erase(newFace);
      }
    }

    for (const auto& face : multicastFacesToRemove) {
      face->close();
    }
  }
}

void
FaceSystem::processSectionEther(const ConfigSection& configSection, bool isDryRun,
                                const std::vector<NetworkInterfaceInfo>& nicList)
{
  // ; the ether section contains settings of Ethernet faces and channels
  // ether
  // {
  //   ; NFD creates one Ethernet multicast face per NIC
  //   mcast yes ; set to 'no' to disable Ethernet multicast, default 'yes'
  //   mcast_group 01:00:5E:00:17:AA ; Ethernet multicast group
  // }

#if defined(HAVE_LIBPCAP)
  NetworkInterfacePredicate nicPredicate;
  bool useMcast = true;
  ethernet::Address mcastGroup(ethernet::getDefaultMulticastAddress());

  for (const auto& i : configSection) {
    if (i.first == "mcast") {
      useMcast = ConfigFile::parseYesNo(i, "ether");
    }
    else if (i.first == "mcast_group") {
      mcastGroup = ethernet::Address::fromString(i.second.get_value<std::string>());
      if (mcastGroup.isNull()) {
        BOOST_THROW_EXCEPTION(ConfigFile::Error("Invalid value for option \"" +
                                                i.first + "\" in \"ether\" section"));
      }
      NFD_LOG_TRACE("Ethernet multicast group set to " << mcastGroup);
    }
    else if (i.first == "whitelist") {
      nicPredicate.parseWhitelist(i.second);
    }
    else if (i.first == "blacklist") {
      nicPredicate.parseBlacklist(i.second);
    }
    else {
      BOOST_THROW_EXCEPTION(ConfigFile::Error("Unrecognized option \"" +
                                              i.first + "\" in \"ether\" section"));
    }
  }

  if (!isDryRun) {
    shared_ptr<EthernetFactory> factory;
    if (m_factories.count("ether") > 0) {
      factory = static_pointer_cast<EthernetFactory>(m_factories["ether"]);
    }
    else {
      factory = make_shared<EthernetFactory>();
      m_factories.emplace("ether", factory);
    }

    std::set<shared_ptr<Face>> multicastFacesToRemove;
    for (const auto& i : factory->getMulticastFaces()) {
      multicastFacesToRemove.insert(i.second);
    }

    if (useMcast) {
      for (const auto& nic : nicList) {
        if (nic.isUp() && nic.isMulticastCapable() && nicPredicate(nic)) {
          try {
            auto newFace = factory->createMulticastFace(nic, mcastGroup);
            m_faceTable.add(newFace);
            multicastFacesToRemove.erase(newFace);
          }
          catch (const EthernetFactory::Error& factoryError) {
            NFD_LOG_ERROR(factoryError.what() << ", continuing");
          }
          catch (const EthernetTransport::Error& faceError) {
            NFD_LOG_ERROR(faceError.what() << ", continuing");
          }
        }
      }
    }

    for (const auto& face : multicastFacesToRemove) {
      face->close();
    }
  }
#else
  BOOST_THROW_EXCEPTION(ConfigFile::Error("NFD was compiled without libpcap, cannot process \"ether\" section"));
#endif // HAVE_LIBPCAP
}

void
FaceSystem::processSectionWebSocket(const ConfigSection& configSection, bool isDryRun)
{
  // ; the websocket section contains settings of WebSocket faces and channels
  // websocket
  // {
  //   listen yes ; set to 'no' to disable WebSocket listener, default 'yes'
  //   port 9696 ; WebSocket listener port number
  //   enable_v4 yes ; set to 'no' to disable listening on IPv4 socket, default 'yes'
  //   enable_v6 yes ; set to 'no' to disable listening on IPv6 socket, default 'yes'
  // }

#if defined(HAVE_WEBSOCKET)
  uint16_t port = 9696;
  bool needToListen = true;
  bool enableV4 = true;
  bool enableV6 = true;

  for (const auto& i : configSection) {
    if (i.first == "port") {
      port = ConfigFile::parseNumber<uint16_t>(i, "websocket");
      NFD_LOG_TRACE("WebSocket port set to " << port);
    }
    else if (i.first == "listen") {
      needToListen = ConfigFile::parseYesNo(i, "websocket");
    }
    else if (i.first == "enable_v4") {
      enableV4 = ConfigFile::parseYesNo(i, "websocket");
    }
    else if (i.first == "enable_v6") {
      enableV6 = ConfigFile::parseYesNo(i, "websocket");
    }
    else {
      BOOST_THROW_EXCEPTION(ConfigFile::Error("Unrecognized option \"" +
                                              i.first + "\" in \"websocket\" section"));
    }
  }

  if (!enableV4 && !enableV6) {
    BOOST_THROW_EXCEPTION(ConfigFile::Error("IPv4 and IPv6 WebSocket channels have been disabled."
                                            " Remove \"websocket\" section to disable WebSocket channels or"
                                            " re-enable at least one channel type."));
  }

  if (!enableV4 && enableV6) {
    BOOST_THROW_EXCEPTION(ConfigFile::Error("NFD does not allow pure IPv6 WebSocket channel."));
  }

  if (!isDryRun) {
    if (m_factories.count("websocket") > 0) {
      return;
    }

    auto factory = make_shared<WebSocketFactory>();
    m_factories.emplace("websocket", factory);

    shared_ptr<WebSocketChannel> channel;

    if (enableV6 && enableV4) {
      websocket::Endpoint endpoint(boost::asio::ip::address_v6::any(), port);
      channel = factory->createChannel(endpoint);

      m_factories.emplace("websocket46", factory);
    }
    else if (enableV4) {
      websocket::Endpoint endpoint(boost::asio::ip::address_v4::any(), port);
      channel = factory->createChannel(endpoint);

      m_factories.emplace("websocket4", factory);
    }

    if (channel && needToListen) {
      channel->listen(bind(&FaceTable::add, &m_faceTable, _1));
    }
  }
#else
  BOOST_THROW_EXCEPTION(ConfigFile::Error("NFD was compiled without WebSocket, "
                                          "cannot process \"websocket\" section"));
#endif // HAVE_WEBSOCKET
}

} // namespace face
} // namespace nfd
