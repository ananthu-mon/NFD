/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * Copyright (c) 2014-2017,  Regents of the University of California,
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

#include "tcp-factory.hpp"

namespace nfd {
namespace face {

namespace ip = boost::asio::ip;

NFD_LOG_INIT("TcpFactory");
NFD_REGISTER_PROTOCOL_FACTORY(TcpFactory);

const std::string&
TcpFactory::getId()
{
  static std::string id("tcp");
  return id;
}

void
TcpFactory::processConfig(OptionalConfigSection configSection,
                          FaceSystem::ConfigContext& context)
{
  // tcp
  // {
  //   listen yes
  //   port 6363
  //   enable_v4 yes
  //   enable_v6 yes
  // }

  if (!configSection) {
    if (!context.isDryRun && !m_channels.empty()) {
      NFD_LOG_WARN("Cannot disable tcp4 and tcp6 channels after initialization");
    }
    return;
  }

  bool wantListen = true;
  uint16_t port = 6363;
  bool enableV4 = true;
  bool enableV6 = true;

  for (const auto& pair : *configSection) {
    const std::string& key = pair.first;

    if (key == "listen") {
      wantListen = ConfigFile::parseYesNo(pair, "face_system.tcp");
    }
    else if (key == "port") {
      port = ConfigFile::parseNumber<uint16_t>(pair, "face_system.tcp");
    }
    else if (key == "enable_v4") {
      enableV4 = ConfigFile::parseYesNo(pair, "face_system.tcp");
    }
    else if (key == "enable_v6") {
      enableV6 = ConfigFile::parseYesNo(pair, "face_system.tcp");
    }
    else {
      BOOST_THROW_EXCEPTION(ConfigFile::Error("Unrecognized option face_system.tcp." + key));
    }
  }

  if (!enableV4 && !enableV6) {
    BOOST_THROW_EXCEPTION(ConfigFile::Error(
      "IPv4 and IPv6 TCP channels have been disabled. Remove face_system.tcp section to disable "
      "TCP channels or enable at least one channel type."));
  }

  if (!context.isDryRun) {
    providedSchemes.insert("tcp");

    if (enableV4) {
      tcp::Endpoint endpoint(ip::tcp::v4(), port);
      shared_ptr<TcpChannel> v4Channel = this->createChannel(endpoint);
      if (wantListen && !v4Channel->isListening()) {
        v4Channel->listen(context.addFace, nullptr);
      }
      providedSchemes.insert("tcp4");
    }
    else if (providedSchemes.count("tcp4") > 0) {
      NFD_LOG_WARN("Cannot close tcp4 channel after its creation");
    }

    if (enableV6) {
      tcp::Endpoint endpoint(ip::tcp::v6(), port);
      shared_ptr<TcpChannel> v6Channel = this->createChannel(endpoint);
      if (wantListen && !v6Channel->isListening()) {
        v6Channel->listen(context.addFace, nullptr);
      }
      providedSchemes.insert("tcp6");
    }
    else if (providedSchemes.count("tcp6") > 0) {
      NFD_LOG_WARN("Cannot close tcp6 channel after its creation");
    }
  }
}

void
TcpFactory::createFace(const FaceUri& remoteUri,
                       const ndn::optional<FaceUri>& localUri,
                       ndn::nfd::FacePersistency persistency,
                       bool wantLocalFieldsEnabled,
                       const FaceCreatedCallback& onCreated,
                       const FaceCreationFailedCallback& onFailure)
{
  BOOST_ASSERT(remoteUri.isCanonical());

  if (localUri) {
    NFD_LOG_TRACE("Cannot create unicast TCP face with LocalUri");
    onFailure(406, "Unicast TCP faces cannot be created with a LocalUri");
    return;
  }

  if (persistency == ndn::nfd::FACE_PERSISTENCY_ON_DEMAND) {
    NFD_LOG_TRACE("createFace does not support FACE_PERSISTENCY_ON_DEMAND");
    onFailure(406, "Outgoing TCP faces do not support on-demand persistency");
    return;
  }

  tcp::Endpoint endpoint(ip::address::from_string(remoteUri.getHost()),
                         boost::lexical_cast<uint16_t>(remoteUri.getPort()));

  if (endpoint.address().is_multicast()) {
    NFD_LOG_TRACE("createFace does not support multicast faces");
    onFailure(406, "Cannot create multicast TCP faces");
    return;
  }

  if (m_prohibitedEndpoints.find(endpoint) != m_prohibitedEndpoints.end()) {
    NFD_LOG_TRACE("Requested endpoint is prohibited "
                  "(reserved by NFD or disallowed by face management protocol)");
    onFailure(406, "Requested endpoint is prohibited");
    return;
  }

  if (wantLocalFieldsEnabled && !endpoint.address().is_loopback()) {
    NFD_LOG_TRACE("createFace cannot create non-local face with local fields enabled");
    onFailure(406, "Local fields can only be enabled on faces with local scope");
    return;
  }

  // very simple logic for now
  for (const auto& i : m_channels) {
    if ((i.first.address().is_v4() && endpoint.address().is_v4()) ||
        (i.first.address().is_v6() && endpoint.address().is_v6())) {
      i.second->connect(endpoint, persistency, wantLocalFieldsEnabled, onCreated, onFailure);
      return;
    }
  }

  NFD_LOG_TRACE("No channels available to connect to " + boost::lexical_cast<std::string>(endpoint));
  onFailure(504, "No channels available to connect");
}

void
TcpFactory::prohibitEndpoint(const tcp::Endpoint& endpoint)
{
  if (endpoint.address().is_v4() &&
      endpoint.address() == ip::address_v4::any()) {
    prohibitAllIpv4Endpoints(endpoint.port());
  }
  else if (endpoint.address().is_v6() &&
           endpoint.address() == ip::address_v6::any()) {
    prohibitAllIpv6Endpoints(endpoint.port());
  }

  NFD_LOG_TRACE("prohibiting TCP " << endpoint);
  m_prohibitedEndpoints.insert(endpoint);
}

void
TcpFactory::prohibitAllIpv4Endpoints(uint16_t port)
{
  ///\todo prohibited endpoints need to react to dynamic NIC changes
  for (const NetworkInterfaceInfo& nic : listNetworkInterfaces()) {
    for (const auto& addr : nic.ipv4Addresses) {
      if (addr != ip::address_v4::any()) {
        prohibitEndpoint(tcp::Endpoint(addr, port));
      }
    }
  }
}

void
TcpFactory::prohibitAllIpv6Endpoints(uint16_t port)
{
  ///\todo prohibited endpoints need to react to dynamic NIC changes
  for (const NetworkInterfaceInfo& nic : listNetworkInterfaces()) {
    for (const auto& addr : nic.ipv6Addresses) {
      if (addr != ip::address_v6::any()) {
        prohibitEndpoint(tcp::Endpoint(addr, port));
      }
    }
  }
}

shared_ptr<TcpChannel>
TcpFactory::createChannel(const tcp::Endpoint& endpoint)
{
  auto channel = findChannel(endpoint);
  if (channel)
    return channel;

  channel = make_shared<TcpChannel>(endpoint);
  m_channels[endpoint] = channel;
  prohibitEndpoint(endpoint);
  return channel;
}

shared_ptr<TcpChannel>
TcpFactory::createChannel(const std::string& localIp, const std::string& localPort)
{
  tcp::Endpoint endpoint(ip::address::from_string(localIp),
                         boost::lexical_cast<uint16_t>(localPort));
  return createChannel(endpoint);
}

std::vector<shared_ptr<const Channel>>
TcpFactory::getChannels() const
{
  return getChannelsFromMap(m_channels);
}

shared_ptr<TcpChannel>
TcpFactory::findChannel(const tcp::Endpoint& localEndpoint) const
{
  auto i = m_channels.find(localEndpoint);
  if (i != m_channels.end())
    return i->second;
  else
    return nullptr;
}

} // namespace face
} // namespace nfd
