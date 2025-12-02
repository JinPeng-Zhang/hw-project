#include "qbb-net-device.h"
#include "pfc-header.h"

#include <ns3/log.h>
#include <ns3/ppp-header.h>
#include <ns3/simulator.h>
#include <ns3/node.h>
#include <ns3/boolean.h>
#include <ns3/uinteger.h>
#include <ns3/assert.h>
#include <ns3/data-rate.h>
#include <ns3/node-list.h>
#include <ns3/queue.h>
#include <ns3/queue-size.h>
#include <ns3/ipv4.h>
#include <ns3/ipv4-header.h>
#include <ns3/ipv4-routing-protocol.h>
#include <ns3/ipv4-route.h>
#include <ns3/ipv4-l3-protocol.h>
#include <ns3/socket.h>

#include <deque>
#include <vector>
#include <iostream>
#include <algorithm>

namespace ns3 {

// ==================== TokenBucket ====================

TokenBucket::TokenBucket(double pps, uint32_t burst)
  : m_pps(pps), m_capacity(burst), m_tokens(burst), m_lastRefill(Simulator::Now())
{}

void TokenBucket::Refill()
{
  if (m_pps <= 0.0) return;
  Time now = Simulator::Now();
  double elapsed = (now - m_lastRefill).GetSeconds();
  m_tokens = std::min(m_tokens + elapsed * m_pps, static_cast<double>(m_capacity));
  m_lastRefill = now;
}

bool TokenBucket::TryConsume(uint32_t pkts)
{
  Refill();
  if (m_tokens >= pkts) {
    m_tokens -= pkts;
    return true;
  }
  return false;
}

// ==================== EgressTokenManager ====================

NS_OBJECT_ENSURE_REGISTERED(EgressTokenManager);

TypeId EgressTokenManager::GetTypeId()
{
  static TypeId tid = TypeId("ns3::EgressTokenManager")
    .SetParent<Object>()
    .SetGroupName("PointToPoint")
    .AddConstructor<EgressTokenManager>();
  return tid;
}

EgressTokenManager::EgressTokenManager() = default;
EgressTokenManager::~EgressTokenManager() = default;

void EgressTokenManager::RegisterEgressPort(uint32_t portId, DataRate rate, uint32_t avgPktSize)
{
  if (m_buckets.find(portId) != m_buckets.end()) return;

  double pps = (avgPktSize > 0) ? (static_cast<double>(rate.GetBitRate()) / (avgPktSize * 8.0)) : 0.0;
  uint32_t burst = std::max(8u, static_cast<uint32_t>(pps * 0.001));
  m_buckets[portId] = std::make_unique<TokenBucket>(pps, burst);
}

bool EgressTokenManager::TryConsumeToken(uint32_t portId, uint32_t pkts)
{
  auto it = m_buckets.find(portId);
  if (it == m_buckets.end()) return true;
  return it->second->TryConsume(pkts);
}

// ==================== QbbNetDevice ====================

NS_LOG_COMPONENT_DEFINE("QbbNetDevice");
NS_OBJECT_ENSURE_REGISTERED(QbbNetDevice);

TypeId QbbNetDevice::GetTypeId()
{
  static TypeId tid = TypeId("ns3::QbbNetDevice")
    .SetParent<PointToPointNetDevice>()
    .SetGroupName("PointToPoint")
    .AddConstructor<QbbNetDevice>()
    .AddAttribute("PfcEnable", "Enable PFC",
                  BooleanValue(true),
                  MakeBooleanAccessor(&QbbNetDevice::m_pfcEnable),
                  MakeBooleanChecker())
    .AddAttribute("PfcHighPkts", "High watermark (packets)",
                  UintegerValue(8),
                  MakeUintegerAccessor(&QbbNetDevice::m_pfcHighPkts),
                  MakeUintegerChecker<uint32_t>())
    .AddAttribute("PfcLowPkts", "Low watermark (packets)",
                  UintegerValue(4),
                  MakeUintegerAccessor(&QbbNetDevice::m_pfcLowPkts),
                  MakeUintegerChecker<uint32_t>())
    .AddAttribute("DefaultQuanta", "Pause quanta",
                  UintegerValue(65535),
                  MakeUintegerAccessor(&QbbNetDevice::m_defaultQuanta),
                  MakeUintegerChecker<uint16_t>())
    .AddAttribute("AvgPktSize", "Avg packet size for token calc (bytes on wire)",
                  UintegerValue(1500),
                  MakeUintegerAccessor(&QbbNetDevice::m_avgPktSize),
                  MakeUintegerChecker<uint32_t>());
  return tid;
}

QbbNetDevice::QbbNetDevice()
{
  m_paused.fill(false);
  m_pauseUntil.fill(Seconds(0));
  m_rxOccPkts.fill(0);
  m_localCongested.fill(false);
}

QbbNetDevice::~QbbNetDevice() = default;

void QbbNetDevice::NotifyNewAggregate()
{
  PointToPointNetDevice::NotifyNewAggregate();

  Ptr<Node> nd = GetNode();
  if (!nd) return;
  Ptr<Ipv4L3Protocol> ipv4 = nd->GetObject<Ipv4L3Protocol>();
  if (ipv4 && !m_l3DivertInstalled)
  {
    Simulator::ScheduleNow(&QbbNetDevice::HookL3Divert, this);
    Simulator::Schedule(MicroSeconds(1), &QbbNetDevice::HookL3Divert, this);
  }
}

void QbbNetDevice::HookL3Divert()
{
  SetReceiveCallback(MakeCallback(&QbbNetDevice::L3Swallow, this));
  m_l3DivertInstalled = true;
}

bool QbbNetDevice::L3Swallow(Ptr<NetDevice>, Ptr<const Packet>, uint16_t, const Address&)
{
  return true;
}

uint8_t QbbNetDevice::GetPrioFromPacket(Ptr<const Packet> p) const
{
  Ptr<Packet> cp = p->Copy();

  PppHeader ppp;
  if (cp->PeekHeader(ppp) && ppp.GetProtocol() == 0x0021) {
    cp->RemoveHeader(ppp);
  }

  Ipv4Header ip;
  if (cp->PeekHeader(ip)) {
    uint8_t tos = ip.GetTos();
    return static_cast<uint8_t>((tos >> 5) & 0x7);
  }
  return 0;
}

bool QbbNetDevice::Send(Ptr<Packet> p, const Address& dest, uint16_t protocol)
{
  uint8_t pr = GetPrioFromPacket(p);
  m_txq[pr].push(TxItem{p->Copy(), dest, protocol});
  
  if (m_txEvent.IsExpired()) {
    m_txEvent = Simulator::ScheduleNow(&QbbNetDevice::TryDequeue, this);
  }
  return true;
}

bool QbbNetDevice::SendFrom(Ptr<Packet> p, const Address&, const Address& dst, uint16_t proto)
{
  return Send(p, dst, proto);
}

int QbbNetDevice::PickNextPrio() const
{
  Time now = Simulator::Now();
  for (int pr = 7; pr >= 0; --pr) {
    if (m_txq[pr].empty()) continue;
    if (m_paused[pr] && now < m_pauseUntil[pr]) continue;
    return pr;
  }
  return -1;
}

void QbbNetDevice::TryDequeue()
{
  int pr = PickNextPrio();
  if (pr < 0) return;

  auto item = m_txq[pr].front();
  
  if (m_paused[pr] && Simulator::Now() < m_pauseUntil[pr]) {
    m_txEvent = Simulator::Schedule(MicroSeconds(10), &QbbNetDevice::TryDequeue, this);
    return;
  }

  // Copy 一份用于发送，保护队列中的原始数据
  Ptr<Packet> pktToSend = item.p->Copy();
  
  if (!PointToPointNetDevice::Send(pktToSend, item.dst, item.proto)) {
    // 发送失败，item.p 未被修改，可以重试
    m_txEvent = Simulator::Schedule(MicroSeconds(10), &QbbNetDevice::TryDequeue, this);
    return;
  }

  // 发送成功，移除队头
  m_txq[pr].pop();
  
  // 继续处理下一个包
  m_txEvent = Simulator::ScheduleNow(&QbbNetDevice::TryDequeue, this);
}

void QbbNetDevice::EnsureTokenManager()
{
  if (m_tokenMgr) return;
  Ptr<Node> nd = GetNode();
  if (!nd) return;

  m_tokenMgr = nd->GetObject<EgressTokenManager>();
  if (!m_tokenMgr) {
    m_tokenMgr = CreateObject<EgressTokenManager>();
    nd->AggregateObject(m_tokenMgr);
  }
  for (uint32_t i = 0; i < nd->GetNDevices(); ++i) {
    Ptr<QbbNetDevice> qbb = DynamicCast<QbbNetDevice>(nd->GetDevice(i));
    if (!qbb) continue;
    DataRateValue dv; qbb->GetAttribute("DataRate", dv);
    m_tokenMgr->RegisterEgressPort(i, dv.Get(), m_avgPktSize);
  }
}

uint32_t QbbNetDevice::GetEgressPortForPacket(Ptr<const Packet> pkt) const
{
  Ptr<Node> node = GetNode();
  if (!node) return 0;
  Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();
  if (!ipv4) return 0;

  Ptr<Packet> cp = pkt->Copy();
  PppHeader ppp; if (cp->PeekHeader(ppp) && ppp.GetProtocol() == 0x0021) cp->RemoveHeader(ppp);
  Ipv4Header ip; if (!cp->PeekHeader(ip)) return 0;

  Ptr<Ipv4RoutingProtocol> rp = ipv4->GetRoutingProtocol();
  if (!rp) return 0;

  Ipv4Header hdr = ip;
  Ptr<NetDevice> outDev = nullptr;
  Socket::SocketErrno err;
  Ptr<Ipv4Route> route = rp->RouteOutput(cp, hdr, outDev, err);
  if (!route) return 0;

  Ptr<NetDevice> routeOutDev = route->GetOutputDevice();
  if (!routeOutDev) return 0;

  for (uint32_t i = 0; i < node->GetNDevices(); ++i) {
    if (node->GetDevice(i) == routeOutDev) return i;
  }
  return 0;
}

void QbbNetDevice::HandleMacRx(Ptr<const Packet> p)
{
  Ptr<Packet> cp = p->Copy();

  // PFC 控制帧
  PppHeader ppp;
  if (cp->PeekHeader(ppp) && ppp.GetProtocol() == PFC_PPP_PROTO) {
    cp->RemoveHeader(ppp);
    PfcHeader ph;
    if (cp->PeekHeader(ph) && ph.GetClassEnable() != 0) {
      cp->RemoveHeader(ph);
      uint8_t mask = ph.GetClassEnable();

      for (uint8_t pr = 0; pr < 8; ++pr) {
        if (!(mask & (1u << pr))) continue;
        uint16_t q = ph.GetPauseQuanta(pr);
        if (q == 0) {
          m_pfcRxXon[pr]++;
          m_paused[pr] = false;
          m_pauseUntil[pr] = Simulator::Now();
          if (m_refillEvent[pr].IsRunning()) m_refillEvent[pr].Cancel();
          RefillMacQueueFromHold(pr);
        } else {
          m_pfcRxXoff[pr]++;
          double bt = GetBitTime();
          m_paused[pr] = true;
          m_pauseUntil[pr] = Simulator::Now() + Seconds(q * 512.0 * bt);

          PurgeMacQueueForPrio(pr);

          if (m_refillEvent[pr].IsRunning()) m_refillEvent[pr].Cancel();
          m_refillEvent[pr] = Simulator::Schedule(m_pauseUntil[pr] - Simulator::Now(),
                                                  &QbbNetDevice::ResumeFromPause, this, pr);
        }
      }
      if (m_txEvent.IsExpired()) {
        m_txEvent = Simulator::ScheduleNow(&QbbNetDevice::TryDequeue, this);
      }
    }
    return;
  }

  // 数据帧
  EnsureTokenManager();
  uint8_t pr = GetPrioFromPacket(p);

  m_ingressQ[pr].push_back(cp);
  OnDataRx(pr);

  if (m_ingressDrainEv.IsExpired()) {
    m_ingressDrainEv = Simulator::ScheduleNow(&QbbNetDevice::DoIngressDrain, this);
  }
}

bool QbbNetDevice::IsLocalDelivery(Ptr<const Packet> pkt) const
{
  Ptr<Packet> cp = pkt->Copy();
  PppHeader ppp;
  if (cp->PeekHeader(ppp) && ppp.GetProtocol() == 0x0021) {
    cp->RemoveHeader(ppp);
  }
  Ipv4Header ip;
  if (!cp->PeekHeader(ip)) {
    // 非 IPv4 或解析失败：视为无需按出端口限速，避免误扣端口0
    return true;
  }

  Ipv4Address dst = ip.GetDestination();
  if (dst.IsBroadcast() || dst.IsMulticast()) return true;

  Ptr<Node> node = GetNode();
  Ptr<Ipv4> ipv4 = node ? node->GetObject<Ipv4>() : nullptr;
  if (!ipv4) return true;

  for (uint32_t i = 0; i < ipv4->GetNInterfaces(); ++i) {
    for (uint32_t j = 0; j < ipv4->GetNAddresses(i); ++j) {
      Ipv4InterfaceAddress ifa = ipv4->GetAddress(i, j);
      if (ifa.GetLocal() == dst || ifa.GetBroadcast() == dst) return true;
    }
  }
  return false;
}

uint32_t QbbNetDevice::GetEgressPortOrInvalid(Ptr<const Packet> pkt) const
{
  if (IsLocalDelivery(pkt)) return kInvalidPortId;

  Ptr<Node> node = GetNode();
  if (!node) return kInvalidPortId;
  Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();
  if (!ipv4) return kInvalidPortId;

  Ptr<Packet> cp = pkt->Copy();
  PppHeader ppp; if (cp->PeekHeader(ppp) && ppp.GetProtocol() == 0x0021) cp->RemoveHeader(ppp);
  Ipv4Header ip; if (!cp->PeekHeader(ip)) return kInvalidPortId;

  Ptr<Ipv4RoutingProtocol> rp = ipv4->GetRoutingProtocol();
  if (!rp) return kInvalidPortId;

  Ipv4Header hdr = ip;
  Ptr<NetDevice> outDev = nullptr;
  Socket::SocketErrno err;
  Ptr<Ipv4Route> route = rp->RouteOutput(cp, hdr, outDev, err);
  if (!route) return kInvalidPortId;

  Ptr<NetDevice> routeOutDev = route->GetOutputDevice();
  if (!routeOutDev) return kInvalidPortId;

  for (uint32_t i = 0; i < node->GetNDevices(); ++i) {
    if (node->GetDevice(i) == routeOutDev) return i;
  }
  return kInvalidPortId;
}

void QbbNetDevice::DoIngressDrain()
{
  int pick = -1;
  for (int pr = 7; pr >= 0; --pr) {
    if (!m_ingressQ[pr].empty()) { pick = pr; break; }
  }
  if (pick < 0) return;

  Ptr<Packet> pkt = m_ingressQ[pick].front();

  // 仅对“需要转发”的包按出端口令牌限速
  uint32_t egressPort = GetEgressPortOrInvalid(pkt);
  if (egressPort != kInvalidPortId) {
    if (!m_tokenMgr->TryConsumeToken(egressPort, 1)) {
      m_ingressDrainEv = Simulator::Schedule(MicroSeconds(10), &QbbNetDevice::DoIngressDrain, this);
      return;
    }
  }

  m_ingressQ[pick].pop_front();
  if (m_rxOccPkts[pick] > 0) {
    m_rxOccPkts[pick] -= 1;
  }
  if (m_pfcEnable && m_localCongested[pick] && m_rxOccPkts[pick] <= m_pfcLowPkts) {
    m_localCongested[pick] = false;
    SendPfcXon(static_cast<uint8_t>(pick));
  }

  PppHeader ppp;
  if (pkt->PeekHeader(ppp) && ppp.GetProtocol() == 0x0021) {
    pkt->RemoveHeader(ppp);
  }

  Ptr<Node> node = GetNode();
  if (node) {
    Ptr<Ipv4L3Protocol> ipv4 = node->GetObject<Ipv4L3Protocol>();
    if (ipv4) {
      ipv4->Receive(this, pkt, 0x0800, GetAddress(), GetAddress(), NetDevice::PACKET_HOST);
    }
  }

  m_ingressDrainEv = Simulator::ScheduleNow(&QbbNetDevice::DoIngressDrain, this);
}

void QbbNetDevice::OnDataRx(uint8_t prio)
{
  if (!m_pfcEnable) return;

  m_rxOccPkts[prio] += 1;

  if (!m_localCongested[prio] && m_rxOccPkts[prio] >= m_pfcHighPkts) {
    m_localCongested[prio] = true;
    SendPfcXoff(prio, m_defaultQuanta);
  }
  if (m_localCongested[prio] && m_rxOccPkts[prio] <= m_pfcLowPkts) {
    m_localCongested[prio] = false;
    SendPfcXon(prio);
  }
}

void QbbNetDevice::PurgeMacQueueForPrio(uint8_t prio)
{
  Ptr<Queue<Packet>> macq = GetQueue();
  if (!macq) return;

  std::vector<Ptr<Packet>> keep;
  while (macq->GetCurrentSize().GetValue() > 0) {
    Ptr<Packet> pkt = macq->Dequeue();
    if (!pkt) break;

    Ptr<Packet> cp = pkt->Copy();
    PppHeader ppp;
    if (cp->PeekHeader(ppp) && ppp.GetProtocol() == PFC_PPP_PROTO) {
      keep.push_back(pkt);
      continue;
    }

    if (GetPrioFromPacket(pkt) == prio) {
      m_macHold[prio].push_back(pkt);
    } else {
      keep.push_back(pkt);
    }
  }

  for (auto& pkt : keep) {
    macq->Enqueue(pkt);
  }
}

void QbbNetDevice::RefillMacQueueFromHold(uint8_t prio)
{
  if (m_paused[prio] && Simulator::Now() < m_pauseUntil[prio]) return;

  if (m_paused[prio] && Simulator::Now() >= m_pauseUntil[prio]) {
    m_paused[prio] = false;
  }

  Ptr<Queue<Packet>> macq = GetQueue();
  if (!macq) return;

  while (!m_macHold[prio].empty()) {
    Ptr<Packet> pkt = m_macHold[prio].front();
    
    if (macq->Enqueue(pkt)) {
      m_macHold[prio].pop_front();
    } else {
      break;
    }
  }

  if (!m_macHold[prio].empty()) {
    m_refillEvent[prio] = Simulator::Schedule(MicroSeconds(5),
                                              &QbbNetDevice::RefillMacQueueFromHold, this, prio);
  } else {
    if (m_txEvent.IsExpired()) {
      m_txEvent = Simulator::ScheduleNow(&QbbNetDevice::TryDequeue, this);
    }
  }
}

void QbbNetDevice::ResumeFromPause(uint8_t prio)
{
  if (Simulator::Now() < m_pauseUntil[prio]) return;
  m_paused[prio] = false;
  RefillMacQueueFromHold(prio);
}

void QbbNetDevice::SendPfcXoff(uint8_t prio, uint16_t quanta)
{
  if (prio < 8) m_pfcTxXoff[prio]++;

  PfcHeader ph;
  ph.SetClassEnable(1u << prio);
  ph.SetPauseQuanta(prio, quanta);

  Ptr<Packet> ctrl = Create<Packet>(ph.GetSerializedSize());
  ctrl->AddHeader(ph);

  PointToPointNetDevice::Send(ctrl, Address(), PFC_PPP_PROTO);
}

void QbbNetDevice::SendPfcXon(uint8_t prio)
{
  if (prio < 8) m_pfcTxXon[prio]++;

  PfcHeader ph;
  ph.SetClassEnable(1u << prio);
  ph.SetPauseQuanta(prio, 0);

  Ptr<Packet> ctrl = Create<Packet>(ph.GetSerializedSize());
  ctrl->AddHeader(ph);

  PointToPointNetDevice::Send(ctrl, Address(), PFC_PPP_PROTO);
}

void QbbNetDevice::BroadcastPfcXoff(uint8_t prio, uint16_t quanta) { SendPfcXoff(prio, quanta); }
void QbbNetDevice::BroadcastPfcXon(uint8_t prio) { SendPfcXon(prio); }

double QbbNetDevice::GetBitTime() const
{
  DataRateValue dv; GetAttribute("DataRate", dv);
  double br = dv.Get().GetBitRate();
  return (br > 0) ? (1.0 / br) : 1e-9;
}

void QbbNetDevice::PrintAllPfcCounters()
{
  using std::cout; using std::endl;
  cout << "==== PFC Counters (per-prio) ====" << endl;
  for (uint32_t ni = 0; ni < NodeList::GetNNodes(); ++ni) {
    Ptr<Node> node = NodeList::GetNode(ni);
    for (uint32_t di = 0; di < node->GetNDevices(); ++di) {
      Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(node->GetDevice(di));
      if (!dev) continue;
      bool any = false;
      for (uint8_t pr = 0; pr < 8; ++pr) {
        if (dev->m_pfcTxXoff[pr] || dev->m_pfcTxXon[pr] || dev->m_pfcRxXoff[pr] || dev->m_pfcRxXon[pr]) {
          if (!any) { cout << "node=" << ni << " dev=" << di << endl; any = true; }
          cout << "  prio" << unsigned(pr)
               << " txXOFF=" << dev->m_pfcTxXoff[pr]
               << " txXON="  << dev->m_pfcTxXon[pr]
               << " rxXOFF=" << dev->m_pfcRxXoff[pr]
               << " rxXON="  << dev->m_pfcRxXon[pr] << endl;
        }
      }
    }
  }
  cout << "==== End ====" << endl;
}

} // namespace ns3
