#include "tcp-rl.h"
#include "tcp-rl-env.h"
#include "ns3/tcp-header.h"
#include "ns3/object.h"
#include "ns3/node-list.h"
#include "ns3/core-module.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/tcp-socket-base.h"
#include "ns3/tcp-l4-protocol.h"


namespace ns3 {
//定义自己用的RlBase类，继承自
//方法包括构造函数，析构函数，生成UUID，创建Gym环境，连接Socket回调等

NS_LOG_COMPONENT_DEFINE ("ns3::TcpRlBase");
NS_OBJECT_ENSURE_REGISTERED (TcpRlBase);

TypeId
TcpRlBase::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::TcpRlBase")
  //继承类
    .SetParent<> ()
    .SetGroupName ("Internet")
    .AddConstructor<TcpRlBase> ()
  ;
  return tid;
}

TcpRlBase::TcpRlBase (void)
  : TcpCongestionOps ()
{
  NS_LOG_FUNCTION (this);
  m_tcpSocket = 0;
  m_tcpGymEnv = 0;
}

TcpRlBase::TcpRlBase (const TcpRlBase& sock)
  : TcpCongestionOps (sock)
{
  NS_LOG_FUNCTION (this);
  m_tcpSocket = 0;
  m_tcpGymEnv = 0;
}

TcpRlBase::~TcpRlBase (void)
{
  m_tcpSocket = 0;
  m_tcpGymEnv = 0;
}

uint64_t
TcpRlBase::GenerateUuid ()
{
  static uint64_t uuid = 0;
  uuid++;
  return uuid;
}

void
TcpRlBase::CreateGymEnv()
{
  NS_LOG_FUNCTION (this);
  // should never be called, only child classes: TcpRl and TcpRlTimeBased
}

void
TcpRlBase::ConnectSocketCallbacks()
{
  NS_LOG_FUNCTION (this);

  bool foundSocket = false;
  Ptr<TcpSocketBase> rxTcpSocket = nullptr;
  int nodeId = -1;
  for (NodeList::Iterator i = NodeList::Begin (); i != NodeList::End (); ++i) {
    Ptr<Node> node = *i;
    Ptr<TcpL4Protocol> tcp = node->GetObject<TcpL4Protocol> ();

    ObjectVectorValue socketVec;
    tcp->GetAttribute ("SocketList", socketVec);
    NS_LOG_UNCOND("Node: " << node->GetId() << " TCP socket num: " << socketVec.GetN());

    uint32_t sockNum = socketVec.GetN();
    for (uint32_t j=0; j<sockNum; j++) {
      Ptr<Object> sockObj = socketVec.Get(j);
      Ptr<TcpSocketBase> tcpSocket = DynamicCast<TcpSocketBase> (sockObj);
      NS_LOG_INFO("Node: " << node->GetId() << " TCP Socket: " << tcpSocket);
      if(!tcpSocket) { continue; }

      Ptr<TcpSocketDerived> dtcpSocket = StaticCast<TcpSocketDerived>(tcpSocket);
      Ptr<TcpCongestionOps> ca = dtcpSocket->GetCongestionControlAlgorithm();
      NS_LOG_INFO("CA name: " << ca->GetName());
      Ptr<TcpRlBase> rlCa = DynamicCast<TcpRlBase>(ca);
      if (rlCa == this) {
        NS_LOG_UNCOND("Found TcpRl CA!");
        foundSocket = true;
        m_tcpSocket = tcpSocket;

        int rxNodeId = node->GetId() + 1; // TODO: replace 1 with num_flows
        Ptr<Node> rxNode = NodeList::GetNode(rxNodeId);
        Ptr<TcpL4Protocol> rxTcp = rxNode->GetObject<TcpL4Protocol> ();
        ObjectVectorValue rxSocketVec;
        rxTcp->GetAttribute ("SocketList", rxSocketVec);
        NS_LOG_INFO("RX Node: " << rxNode->GetId() << " RX TCP socket num: " << rxSocketVec.GetN());
        Ptr<Object> rxSockObj = rxSocketVec.Get(1);
        rxTcpSocket = DynamicCast<TcpSocketBase> (rxSockObj);
        
        break;
      }
    }

    if (foundSocket) {
      nodeId = node->GetId();
      break;
    }
  }

  NS_ASSERT_MSG(m_tcpSocket, "TCP socket was not found.");

  if(m_tcpSocket)
  {
    //这里来实现所有traceconnect，以及在rl-env中自己写的更新函数
    NS_LOG_INFO("Found TCP Socket: " << m_tcpSocket);
    m_tcpSocket->TraceConnectWithoutContext ("Tx", MakeCallback (&TcpGymEnv::TxPktTrace, m_tcpGymEnv));
    Config::ConnectWithoutContext("/NodeList/" + std::to_string(nodeId) + "/$ns3::TcpL4Protocol/SocketList/0/Rtt", MakeCallback (&TcpGymEnv::RttTrace, m_tcpGymEnv));
    
    m_tcpGymEnv->SetNodeId(m_tcpSocket->GetNode()->GetId());
  }
}

std::string
TcpRlBase::GetName () const
{
  return "TcpRlBase";
}

uint32_t
TcpRlBase::GetSsThresh (Ptr<const TcpSocketState> state,
                         uint32_t bytesInFlight)
{
  NS_LOG_FUNCTION (this << state << bytesInFlight);

  if (!m_tcpGymEnv) {
    CreateGymEnv();
  }

  uint32_t newSsThresh = 0;
  if (m_tcpGymEnv) {
      newSsThresh = m_tcpGymEnv->GetSsThresh(state, bytesInFlight);
  }

  return newSsThresh;
}

void
TcpRlBase::IncreaseWindow (Ptr<TcpSocketState> tcb, uint32_t segmentsAcked)
{
  NS_LOG_FUNCTION (this << tcb << segmentsAcked);

  if (!m_tcpGymEnv) {
    CreateGymEnv();
  }

  if (m_tcpGymEnv) {
     m_tcpGymEnv->IncreaseWindow(tcb, segmentsAcked);
  }
}

void
TcpRlBase::PktsAcked (Ptr<TcpSocketState> tcb, uint32_t segmentsAcked, const Time& rtt)
{
  NS_LOG_FUNCTION (this);

  if (!m_tcpGymEnv) {
    CreateGymEnv();
  }

  if (m_tcpGymEnv) {
     m_tcpGymEnv->PktsAcked(tcb, segmentsAcked, rtt);
  }
}

void
TcpRlBase::CongestionStateSet (Ptr<TcpSocketState> tcb, const TcpSocketState::TcpCongState_t newState)
{
  NS_LOG_FUNCTION (this);

  if (!m_tcpGymEnv) {
    CreateGymEnv();
  }

  if (m_tcpGymEnv) {
     m_tcpGymEnv->CongestionStateSet(tcb, newState);
  }
}

Ptr<TcpCongestionOps>
TcpRlBase::Fork ()
{
  return CopyObject<TcpRlBase> (this);
}

//定义TcpRlTimeBased类，继承自TcpRlBase，负责基于时间步长的RL控制

NS_OBJECT_ENSURE_REGISTERED (TcpRlTimeBased);

TypeId
TcpRlTimeBased::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::TcpRlTimeBased")
    .SetParent<TcpRlBase> ()
    .SetGroupName ("Internet")
    .AddConstructor<TcpRlTimeBased> ()
    .AddAttribute ("Protocol", "Underlying protocol.",
                   StringValue ("ns3::TcpBic"),
                   MakeStringAccessor(&TcpRlTimeBased::m_prot),
                   MakeStringChecker())
    .AddAttribute ("TimeStep", "Time step.",
                   TimeValue (MilliSeconds (100)),
                   MakeTimeAccessor(&TcpRlTimeBased::m_timeStep),
                   MakeTimeChecker())
  ;
  return tid;
}

TcpRlTimeBased::TcpRlTimeBased (void) : TcpRlBase ()
{
  NS_LOG_FUNCTION (this);
}

TcpRlTimeBased::TcpRlTimeBased (const TcpRlTimeBased& sock)
  : TcpRlBase (sock)
{
  NS_LOG_FUNCTION (this);
}

TcpRlTimeBased::~TcpRlTimeBased (void)
{
}

std::string
TcpRlTimeBased::GetName () const
{
  return "TcpRlTimeBased";
}

void
TcpRlTimeBased::CreateGymEnv()
{
  NS_LOG_FUNCTION (this);
  Ptr<TcpTimeStepGymEnv> env = CreateObject<TcpTimeStepGymEnv>(m_prot, m_timeStep);
  env->SetSocketUuid(TcpRlBase::GenerateUuid());
  m_tcpGymEnv = env;

  ConnectSocketCallbacks();
}

} // namespace ns3
