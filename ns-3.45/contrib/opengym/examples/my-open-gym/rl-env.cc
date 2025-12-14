#include "rl-env.h"
#include "ns3/tcp-header.h"
#include "ns3/object.h"
#include "ns3/core-module.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include <vector>
#include <numeric>
#include "ns3/traffic-control-layer.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/csma-net-device.h"
#include "ns3/ipv4-l3-protocol.h"
#include "ns3/lte-net-device.h"
#include "ns3/flow-monitor-helper.h"
#include "ns3/probe.h"
#include "ns3/object-probe.h"

#include "ns3/sim-stats-collector.h"
#include <iostream>
#include <vector>
#include <iomanip>
#include "ns3/qbb-net-device.h"

namespace ns3 {
NS_LOG_COMPONENT_DEFINE ("ns3::MyGymEnv");
NS_OBJECT_ENSURE_REGISTERED (MyGymEnv);

MyGymEnv::MyGymEnv(std::string prot)
{
  NS_LOG_FUNCTION (this);
  SetOpenGymInterface(OpenGymInterface::Get());
}

MyGymEnv::MyGymEnv (Time stepTime)
{
  NS_LOG_FUNCTION (this);
  m_interval = stepTime;
  Simulator::Schedule (Seconds(0.0), &MyGymEnv::ScheduleNextStateRead, this);
}

void
MyGymEnv::ScheduleNextStateRead ()
{
  NS_LOG_FUNCTION (this);
  Simulator::Schedule (m_timeStep, &MyGymEnv::ScheduleNextStateRead, this);
  Notify();
}

MyGymEnv::~MyGymEnv ()
{
  NS_LOG_FUNCTION (this);
}

Ptr<OpenGymSpace>
MyGymEnv::GetActionSpace()
{
  uint32_t parameterNum = 2;
  std::vector<uint32_t> shape = {parameterNum,};
  std::string dtype = TypeNameGet<uint32_t> ();

  Ptr<OpenGymBoxSpace> box = CreateObject<OpenGymBoxSpace> (low, high, shape, dtype);
  NS_LOG_INFO ("MyGetActionSpace: " << box);
  return box;
}


bool
MyGymEnv::GetGameOver()
{
  m_isGameOver = false;
  bool test = false;
  static float stepCounter = 0.0;
  stepCounter += 1;
  if (stepCounter == 10 && test) {
      m_isGameOver = true;
  }
  NS_LOG_INFO ("MyGetGameOver: " << m_isGameOver);
  return m_isGameOver;
}

float
MyGymEnv::GetReward()
{
  NS_LOG_INFO("MyGetReward: " << m_envReward);
  return m_envReward;
}

std::string
MyGymEnv::GetExtraInfo()
{
  NS_LOG_INFO("MyGetExtraInfo: " << m_info);
  return m_info;
}

/*
Execute received actions
*/
bool
MyGymEnv::ExecuteActions(Ptr<OpenGymDataContainer> action)
{
  Ptr<OpenGymBoxContainer<uint32_t> > box = DynamicCast<OpenGymBoxContainer<uint32_t> >(action);
  // 动作是每次调整所有节点的链路负载分配比例
  m_ratio = box->GetValue();
  NS_LOG_INFO ("MyExecuteActions: " << action);
  return true;
}

/*
Define observation space
*/
Ptr<OpenGymSpace>
MyGymEnv::GetObservationSpace()
{
  uint32_t parameterNum = 9;
  //
  float low = 0.0;
  float high = 1000000000.0;
  std::vector<uint32_t> shape = {parameterNum,};
  std::string dtype = TypeNameGet<double> ();
  Ptr<OpenGymBoxSpace> box = CreateObject<OpenGymBoxSpace> (low, high, shape, dtype);
  NS_LOG_INFO ("MyGetObservationSpace: " << box);
  return box;
}


Ptr<OpenGymDataContainer>
MyGymEnv::GetObservation()
{
  uint32_t parameterNum = 9;
  std::vector<uint32_t> shape = {parameterNum,};

  Ptr<OpenGymBoxContainer<double> > box = CreateObject<OpenGymBoxContainer<double> >(shape);

  Ptr<SimStatsCollector> stats = CreateObject<SimStatsCollector>();
  stats->Setup(builder.GetLinks(), builder.GetFlows(), builder.GetAppsStopTime());
  
  SimResult results = stats->CollectAndPrint();
  double m_total_throughput = results.global.throughputMbps;
  double m_average_delay = results.global.avgDelayMs;
  double m_packet_loss_ratio = results.global.lossRatePct;

  //获取全部链路负载数据，并求方差
  std::vector<double> arrUtil ;
  for (size_t i = 0; i < results.links.size(); ++i) {
      LinkTimeSeries linkData = results.links[i];
      double link_load = linkData.utilSnapshots;
      // 向arrUtil添加一条链路负载数据
      arrUtil.push_back(link_load);
  }
  // 链路负载方差
  double link_load_var = arrUtil.variance();

  // 队列长度
  // std::vector<double> arrQueueA = myLinkData.queueSnapshotsA;
  // std::vector<double> arrQueueB = myLinkData.queueSnapshotsB;
  // double linkload_sum = std::accumulate(arrUtil.begin(), arrUtil.end(), 0.0);

  // 获取pfc触发次数
  // 创建一个qbb-net-device
  Ptr<QbbNetDevice> qbbDev = CreateObject<QbbNetDevice>();
  NS_LOG_INFO("PFC_Counter: " << qbbDev->PrintAllPfcCounters());
  double m_pfc_trigger = qbbDev->PrintAllPfcCounters();

  //将类中的成员变量作为观测值返回
  box->AddValue(m_total_throughput);   // 总吞吐量
  box->AddValue(m_average_delay);      // 平均延迟
  box->AddValue(m_packet_loss_ratio);   // 丢包率
  box->AddValue(m_pfc_trigger);        // PFC触发次数
  box->AddValue(m_link_load_var);          // 链路负载率
  
  // Print data
  // NS_LOG_INFO ("MyGetObservation: " << box);

  return box;
} // namespace ns3
