#include "qbb-point-to-point-helper.h"
#include "qbb-net-device.h"
#include "pfc-header.h"

#include <ns3/node.h>
#include <ns3/queue.h>
#include <ns3/point-to-point-channel.h>
#include <ns3/uinteger.h>
#include <ns3/boolean.h>
#include <ns3/config.h>

namespace ns3 {

namespace {
// L3 空处理：注册 PPP 自定义协议号，避免未知协议断言；真实处理在设备 Receive/HandlePfcFrame
void PfcL3Stub(ns3::Ptr<ns3::NetDevice> /*dev*/,
               ns3::Ptr<const ns3::Packet> /*p*/,
               uint16_t /*protocol*/,
               const ns3::Address& /*src*/,
               const ns3::Address& /*dst*/,
               ns3::NetDevice::PacketType /*type*/)
{
  // no-op
}
}

QbbPointToPointHelper::QbbPointToPointHelper()
{
  m_deviceFactory.SetTypeId("ns3::QbbNetDevice");
  m_channelFactory.SetTypeId("ns3::PointToPointChannel");
  m_queueFactory.SetTypeId("ns3::DropTailQueue<Packet>");
}

void
QbbPointToPointHelper::SetDeviceAttribute(std::string name, const AttributeValue& v)
{
  m_deviceFactory.Set(name, v);
}

void
QbbPointToPointHelper::SetChannelAttribute(std::string name, const AttributeValue& v)
{
  m_channelFactory.Set(name, v);
}

void
QbbPointToPointHelper::SetPfcAttribute(std::string name, const AttributeValue& v)
{
  if (name == "PfcEnable") {
    const BooleanValue* bv = dynamic_cast<const BooleanValue*>(&v);
    if (bv) m_pfcEnable = bv->Get();
  } else if (name == "PfcHighPkts") {
    const UintegerValue* uv = dynamic_cast<const UintegerValue*>(&v);
    if (uv) m_pfcHighPkts = uv->Get();
  } else if (name == "PfcLowPkts") {
    const UintegerValue* uv = dynamic_cast<const UintegerValue*>(&v);
    if (uv) m_pfcLowPkts = uv->Get();
  } else if (name == "DefaultQuanta") {
    const UintegerValue* uv = dynamic_cast<const UintegerValue*>(&v);
    if (uv) m_defaultQuanta = static_cast<uint16_t>(uv->Get());
  }
}

NetDeviceContainer
QbbPointToPointHelper::Install(Ptr<Node> a, Ptr<Node> b) const
{
  NetDeviceContainer devices;

  Ptr<QbbNetDevice> devA = m_deviceFactory.Create<QbbNetDevice>();
  Ptr<QbbNetDevice> devB = m_deviceFactory.Create<QbbNetDevice>();

  devA->SetAttribute("PfcEnable", BooleanValue(m_pfcEnable));
  devA->SetAttribute("PfcHighPkts", UintegerValue(m_pfcHighPkts));
  devA->SetAttribute("PfcLowPkts", UintegerValue(m_pfcLowPkts));
  devA->SetAttribute("DefaultQuanta", UintegerValue(m_defaultQuanta));

  devB->SetAttribute("PfcEnable", BooleanValue(m_pfcEnable));
  devB->SetAttribute("PfcHighPkts", UintegerValue(m_pfcHighPkts));
  devB->SetAttribute("PfcLowPkts", UintegerValue(m_pfcLowPkts));
  devB->SetAttribute("DefaultQuanta", UintegerValue(m_defaultQuanta));

  Ptr<Queue<Packet>> qA = m_queueFactory.Create<Queue<Packet>>();
  Ptr<Queue<Packet>> qB = m_queueFactory.Create<Queue<Packet>>();
  devA->SetQueue(qA);
  devB->SetQueue(qB);

  a->AddDevice(devA);
  b->AddDevice(devB);

  Ptr<PointToPointChannel> channel = m_channelFactory.Create<PointToPointChannel>();
  devA->Attach(channel);
  devB->Attach(channel);

  // 绑定设备自身的 MacRx 用于数据帧统计（PFC 帧在 Receive 中已处理）
  devA->TraceConnectWithoutContext("MacRx", MakeCallback(&QbbNetDevice::HandleMacRx, devA));
  devB->TraceConnectWithoutContext("MacRx", MakeCallback(&QbbNetDevice::HandleMacRx, devB));

  // 注册 PPP 自定义协议：PFC_PPP_PROTO
  a->RegisterProtocolHandler(MakeCallback(&PfcL3Stub), PFC_PPP_PROTO, 0, false);
  b->RegisterProtocolHandler(MakeCallback(&PfcL3Stub), PFC_PPP_PROTO, 0, false);

  devices.Add(devA);
  devices.Add(devB);
  return devices;
}

NetDeviceContainer
QbbPointToPointHelper::Install(NodeContainer c) const
{
  NS_ASSERT(c.GetN() == 2);
  return Install(c.Get(0), c.Get(1));
}

} // namespace ns3
