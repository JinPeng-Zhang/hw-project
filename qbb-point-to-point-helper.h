#ifndef QBB_POINT_TO_POINT_HELPER_H
#define QBB_POINT_TO_POINT_HELPER_H

#include <string>
#include <cstdint>

#include <ns3/object-factory.h>
#include <ns3/node-container.h>
#include <ns3/net-device-container.h>
#include <ns3/attribute.h>

namespace ns3 {

// Helper：创建两端 QbbNetDevice、连通 Channel、绑定 MacRx、注册 PPP 自定义协议
class QbbPointToPointHelper
{
public:
  QbbPointToPointHelper();

  void SetDeviceAttribute(std::string name, const AttributeValue& v);
  void SetChannelAttribute(std::string name, const AttributeValue& v);
  void SetPfcAttribute(std::string name, const AttributeValue& v);

  NetDeviceContainer Install(Ptr<Node> a, Ptr<Node> b) const;
  NetDeviceContainer Install(NodeContainer c) const;

private:
  ObjectFactory m_deviceFactory;
  ObjectFactory m_channelFactory;
  ObjectFactory m_queueFactory;

  bool      m_pfcEnable{true};
  uint32_t  m_pfcHighPkts{8};
  uint32_t  m_pfcLowPkts{4};
  uint16_t  m_defaultQuanta{65535};
};

} // namespace ns3

#endif // QBB_POINT_TO_POINT_HELPER_H
