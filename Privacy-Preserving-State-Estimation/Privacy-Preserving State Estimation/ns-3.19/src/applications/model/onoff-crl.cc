/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
//
// Copyright (c) 2006 Georgia Tech Research Corporation
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License version 2 as
// published by the Free Software Foundation;
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
// Author: George F. Riley<riley@ece.gatech.edu>
//

// ns3 - On/Off Data Source Application class
// George F. Riley, Georgia Tech, Spring 2007
// Adapted from ApplicationOnOff in GTNetS.

#include "ns3/log.h"
#include "ns3/address.h"
#include "ns3/inet-socket-address.h"
#include "ns3/inet6-socket-address.h"
#include "ns3/packet-socket-address.h"
#include "ns3/node.h"
#include "ns3/nstime.h"
#include "ns3/data-rate.h"
#include "ns3/random-variable-stream.h"
#include "ns3/socket.h"
#include "ns3/simulator.h"
#include "ns3/socket-factory.h"
#include "ns3/packet.h"
#include "ns3/uinteger.h"
#include "ns3/trace-source-accessor.h"
#include "onoff-crl.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/string.h"
#include "ns3/pointer.h"
#include "ns3/seq-ts-header.h"

NS_LOG_COMPONENT_DEFINE ("OnOffCRL");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (OnOffCRL);

TypeId
OnOffCRL::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::OnOffCRL")
    .SetParent<Application> ()
    .AddConstructor<OnOffCRL> ()
    .AddAttribute ("DataRate", "The data rate in on state.",
                   DataRateValue (DataRate ("500kb/s")),
                   MakeDataRateAccessor (&OnOffCRL::m_cbrRate),
                   MakeDataRateChecker ())
    .AddAttribute ("PacketSize", "The size of packets sent in on state",
                   UintegerValue (512),
                   MakeUintegerAccessor (&OnOffCRL::m_pktSize),
                   MakeUintegerChecker<uint32_t> (1))
	.AddAttribute ("Interval", "Time interval between two consecutive packet transmission",
                   TimeValue (Seconds (30)),
                   MakeTimeAccessor (&OnOffCRL::m_interval),
                   MakeTimeChecker ())
    .AddAttribute ("Remote", "The address of the destination",
                   AddressValue (),
                   MakeAddressAccessor (&OnOffCRL::m_peer),
                   MakeAddressChecker ())
    .AddAttribute ("OnTime", "A RandomVariableStream used to pick the duration of the 'On' state.",
                   StringValue ("ns3::ConstantRandomVariable[Constant=1.0]"),
                   MakePointerAccessor (&OnOffCRL::m_onTime),
                   MakePointerChecker <RandomVariableStream>())
    .AddAttribute ("OffTime", "A RandomVariableStream used to pick the duration of the 'Off' state.",
                   StringValue ("ns3::ConstantRandomVariable[Constant=1.0]"),
                   MakePointerAccessor (&OnOffCRL::m_offTime),
                   MakePointerChecker <RandomVariableStream>())
    .AddAttribute ("MaxBytes", 
                   "The total number of bytes to send. Once these bytes are sent, "
                   "no packet is sent again, even in on state. The value zero means "
                   "that there is no limit.",
                   UintegerValue (0),
                   MakeUintegerAccessor (&OnOffCRL::m_maxBytes),
                   MakeUintegerChecker<uint32_t> ())
    .AddAttribute ("Protocol", "The type of protocol to use.",
                   TypeIdValue (UdpSocketFactory::GetTypeId ()),
                   MakeTypeIdAccessor (&OnOffCRL::m_tid),
                   MakeTypeIdChecker ())
    .AddAttribute ("FirstSent", "the time of the first data sent",
                    TimeValue (Seconds (100)),
                   MakeTimeAccessor (&OnOffCRL::m_firstSent),
                   MakeTimeChecker ())
    .AddTraceSource ("Tx", "A new packet is created and is sent",
                     MakeTraceSourceAccessor (&OnOffCRL::m_txTrace))
    
  ;
  return tid;
}


OnOffCRL::OnOffCRL ()
  : m_socket (0),
    m_connected (false),
    m_residualBits (0),
    m_lastStartTime (Seconds (0)),
    m_totBytes (0),
    m_seqnum (0),
    m_firstTime (true)
{
  NS_LOG_FUNCTION (this);
}

OnOffCRL::~OnOffCRL()
{
  NS_LOG_FUNCTION (this);
}

void 
OnOffCRL::SetMaxBytes (uint32_t maxBytes)
{
  NS_LOG_FUNCTION (this << maxBytes);
  m_maxBytes = maxBytes;
}

Ptr<Socket>
OnOffCRL::GetSocket (void) const
{
  NS_LOG_FUNCTION (this);
  return m_socket;
}

int64_t 
OnOffCRL::AssignStreams (int64_t stream)
{
  NS_LOG_FUNCTION (this << stream);
  m_onTime->SetStream (stream);
  m_offTime->SetStream (stream + 1);
  return 2;
}

void
OnOffCRL::DoDispose (void)
{
  NS_LOG_FUNCTION (this);

  m_socket = 0;
  // chain up
  Application::DoDispose ();
}

// Application Methods
void OnOffCRL::StartApplication () // Called at time specified by Start
{
  NS_LOG_FUNCTION (this);

  // Create the socket if not already
  if (!m_socket)
    {
      m_socket = Socket::CreateSocket (GetNode (), m_tid);
      if (Inet6SocketAddress::IsMatchingType (m_peer))
        {
          m_socket->Bind6 ();
        }
      else if (InetSocketAddress::IsMatchingType (m_peer) ||
               PacketSocketAddress::IsMatchingType (m_peer))
        {
          m_socket->Bind ();
        }
      m_socket->Connect (m_peer);
      m_socket->SetAllowBroadcast (true);
      m_socket->ShutdownRecv ();

      m_socket->SetConnectCallback (
        MakeCallback (&OnOffCRL::ConnectionSucceeded, this),
        MakeCallback (&OnOffCRL::ConnectionFailed, this));
    }
    ScheduleNextTx ();
  // Insure no pending event
  //CancelEvents ();
  // If we are not yet connected, there is nothing to do here
  // The ConnectionComplete upcall will start timers at that time
  //if (!m_connected) return;
  //ScheduleStartEvent ();
}

void OnOffCRL::StopApplication () // Called at time specified by Stop
{
  NS_LOG_FUNCTION (this);

  CancelEvents ();
  if(m_socket != 0)
    {
      m_socket->Close ();
    }
  else
    {
      NS_LOG_WARN ("OnOffCRL found null socket to close in StopApplication");
    }
}

void OnOffCRL::CancelEvents ()
{
  NS_LOG_FUNCTION (this);

  if (m_sendEvent.IsRunning ())
    { // Cancel the pending send packet event
      // Calculate residual bits since last packet sent
      Time delta (Simulator::Now () - m_lastStartTime);
      int64x64_t bits = delta.To (Time::S) * m_cbrRate.GetBitRate ();
      m_residualBits += bits.GetHigh ();
    }
  Simulator::Cancel (m_sendEvent);
  Simulator::Cancel (m_startStopEvent);
}

// Event handlers
void OnOffCRL::StartSending ()
{
  NS_LOG_FUNCTION (this);
  m_lastStartTime = Simulator::Now ();
  ScheduleNextTx ();  // Schedule the send packet event
  ScheduleStopEvent ();
}

void OnOffCRL::StopSending ()
{
  NS_LOG_FUNCTION (this);
  CancelEvents ();

  ScheduleStartEvent ();
}

// Private helpers
void OnOffCRL::ScheduleNextTx ()
{
  NS_LOG_FUNCTION (this);

  //if (m_maxBytes == 0 || m_totBytes < m_maxBytes)
    //{
   //   if (m_firstTime) {
   //      Time nextTime = m_firstSent;
   //      NS_LOG_LOGIC ("nextTime = " << nextTime);
   //      m_sendEvent = Simulator::Schedule (nextTime,
   //                                         &OnOffCRL::SendPacket, this);
   //      m_firstTime = false;
   //    }
   //   else {
         /*uint32_t bits = m_pktSize * 8 - m_residualBits;
         NS_LOG_LOGIC ("bits = " << bits);
         Time nextTime (Seconds (bits /
                              static_cast<double>(m_cbrRate.GetBitRate ()))); // Time till next packet
         NS_LOG_LOGIC ("nextTime = " << nextTime);*/
         m_sendEvent = Simulator::Schedule (m_firstSent,
                                            &OnOffCRL::SendPacket, this);
  //    }
 /*   }
  else
    { // All done, cancel any pending events
      StopApplication ();
    }*/
}

void OnOffCRL::ScheduleStartEvent ()
{  // Schedules the event to start sending data (switch to the "On" state)
  NS_LOG_FUNCTION (this);

  Time offInterval = Seconds (m_offTime->GetValue ());
  NS_LOG_LOGIC ("start at " << offInterval);
  m_startStopEvent = Simulator::Schedule (offInterval, &OnOffCRL::StartSending, this);
}

void OnOffCRL::ScheduleStopEvent ()
{  // Schedules the event to stop sending data (switch to "Off" state)
  NS_LOG_FUNCTION (this);

  Time onInterval = Seconds (m_onTime->GetValue ());
  NS_LOG_LOGIC ("stop at " << onInterval);
  m_startStopEvent = Simulator::Schedule (onInterval, &OnOffCRL::StopSending, this);
}


void OnOffCRL::SendPacket ()
{
  NS_LOG_FUNCTION (this);

  NS_ASSERT (m_sendEvent.IsExpired ());
  /*unsigned char * sent;
  sent = (unsigned char *)malloc(2);
  
  char command [50];
  sprintf(command, "./integer-fhe & pk.dat 1100110010101010");
  
  int data = 0xCCAA;
  memcpy(sent, reinterpret_cast <unsigned char *> (&data), 2*sizeof(unsigned char));
  Ptr<Packet> packet = Create<Packet> (sent, 2*sizeof(unsigned char));*/
  /*SeqTsHeader seqTs;
  seqTs.SetSeq (m_seqnum);*/
  Ptr<Packet> packet = Create<Packet> (m_pktSize/*-(8+4)*/); // 8+4 : the size of the seqTs header
  //packet->AddHeader (seqTs);
  m_txTrace (packet);
  m_socket->Send (packet);
  m_totBytes += m_pktSize;
  if (InetSocketAddress::IsMatchingType (m_peer))
    {
      //++m_seqnum;
      NS_LOG_INFO (" Tx " << packet->GetSize() 
                   << " " << InetSocketAddress::ConvertFrom(m_peer).GetIpv4 ()
                   <<" Uid " << packet->GetUid () 
                   //<< " Sequence Number: " << seqTs.GetSeq () 
                   <<" Time " << (Simulator::Now ()).GetSeconds ());
/*      NS_LOG_INFO ("At time " << Simulator::Now ().GetSeconds ()
                   << "s on-off application sent "
                   <<  packet->GetSize () << " bytes to "
                   << InetSocketAddress::ConvertFrom(m_peer).GetIpv4 ()
                   << " port " << InetSocketAddress::ConvertFrom (m_peer).GetPort ()
                   << " total Tx " << m_totBytes << " bytes"); */

    }
  else if (Inet6SocketAddress::IsMatchingType (m_peer))
    {
      NS_LOG_INFO ("At time " << Simulator::Now ().GetSeconds ()
                   << "s on-off application sent "
                   <<  packet->GetSize () << " bytes to "
                   << Inet6SocketAddress::ConvertFrom(m_peer).GetIpv6 ()
                   << " port " << Inet6SocketAddress::ConvertFrom (m_peer).GetPort ()
                   << " total Tx " << m_totBytes << " bytes");
    }
  m_lastStartTime = Simulator::Now ();
  m_residualBits = 0;
  //ScheduleNextTx ();
}


void OnOffCRL::ConnectionSucceeded (Ptr<Socket> socket)
{
  NS_LOG_FUNCTION (this << socket);
  m_connected = true;
}

void OnOffCRL::ConnectionFailed (Ptr<Socket> socket)
{
  NS_LOG_FUNCTION (this << socket);
}
void OnOffCRL::SetPacketSize(uint8_t size){
	m_pktSize = size;
}

} // Namespace ns3
