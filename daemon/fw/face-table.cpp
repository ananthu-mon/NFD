/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * Copyright (C) 2014 Named Data Networking Project
 * See COPYING for copyright and distribution information.
 */

#include "face-table.hpp"
#include "forwarder.hpp"

namespace nfd {

NFD_LOG_INIT("FaceTable");

FaceTable::FaceTable(Forwarder& forwarder)
  : m_forwarder(forwarder)
  , m_lastFaceId(0)
{
}

void
FaceTable::add(shared_ptr<Face> face)
{
  FaceId faceId = ++m_lastFaceId;
  face->setId(faceId);
  m_faces[faceId] = face;
  NFD_LOG_INFO("addFace id=" << faceId);

  face->onReceiveInterest += bind(&Forwarder::onInterest,
                             &m_forwarder, boost::ref(*face), _1);
  face->onReceiveData     += bind(&Forwarder::onData,
                             &m_forwarder, boost::ref(*face), _1);
}

void
FaceTable::remove(shared_ptr<Face> face)
{
  FaceId faceId = face->getId();
  m_faces.erase(faceId);
  face->setId(INVALID_FACEID);
  NFD_LOG_INFO("removeFace id=" << faceId);

  // XXX This clears all subscriptions, because EventEmitter
  //     does not support only removing Forwarder's subscription
  face->onReceiveInterest.clear();
  face->onReceiveData    .clear();

  m_forwarder.getFib().removeNextHopFromAllEntries(face);
}

} // namespace nfd