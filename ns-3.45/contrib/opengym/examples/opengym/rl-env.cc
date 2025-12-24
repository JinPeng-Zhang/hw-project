#include "rl-env.h"
#include "ns3/tcp-header.h"
#include "ns3/object.h"
#include "ns3/core-module.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/traffic-control-layer.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/flow-monitor-helper.h"

#include "ns3/sim-stats-collector.h"
#include "ns3/qbb-net-device.h"
#include "ns3/qbb-point-to-point-helper.h"

#include <iostream>
#include <vector>
#include <numeric>
#include <iomanip>

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
  uint32_t parameterNum = 464;
  std::vector<uint32_t> shape = {parameterNum,4};
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

/*
Define observation space
*/
Ptr<OpenGymSpace>
MyGymEnv::GetObservationSpace()
{
  uint32_t parameterNum = 5;
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
  uint32_t parameterNum = 5;
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
  double m_link_load_var = arrUtil.variance();

  // 队列长度
  // std::vector<double> arrQueueA = myLinkData.queueSnapshotsA;
  // std::vector<double> arrQueueB = myLinkData.queueSnapshotsB;
  // double linkload_sum = std::accumulate(arrUtil.begin(), arrUtil.end(), 0.0);

  // 获取pfc触发次数
  // 创建一个qbb-net-device
  Ptr<QbbNetDevice> qbbDev = CreateObject<QbbNetDevice>();
  NS_LOG_INFO("PFC_Counter: " << qbbDev->QbbNetDevice::PrintAllPfcCounters());
  double m_pfc_trigger = qbbDev->QbbNetDevice::PrintAllPfcCounters();

  //将类中的成员变量作为观测值返回
  box->AddValue(m_total_throughput);   // 总吞吐量
  box->AddValue(m_average_delay);      // 平均延迟
  box->AddValue(m_packet_loss_ratio);  // 丢包率
  box->AddValue(m_pfc_trigger);        // PFC触发次数
  box->AddValue(m_link_load_var);      // 链路负载率
  
  m_envReward = 0;
  // Print data
  // NS_LOG_INFO ("MyGetObservation: " << box);

  return box;
}

float
MyGymEnv::GetReward()
{
  NS_LOG_INFO("MyGetReward: " << m_envReward);
  return m_envReward;
}

bool
MyGymEnv::ExecuteActions(Ptr<OpenGymDataContainer> action)
{
  // action是一个464*4的矩阵，表示每个节点的ECMP概率分布,使用一个二维数组存储
  Ptr<OpenGymBoxContainer<uint32_t> > box = DynamicCast<OpenGymBoxContainer<uint32_t> >(action);
  std::vector<uint32_t> m_ratio;
  for (uint32_t i = 0; i < box->GetSize(); ++i) {
    for (uint32_t j = 0; j < box->GetSize(); ++j) {
      m_ratio.push_back(box->GetValue()[i][j]);
    }
  }
  // 将m_ratio写入文件
  std::ofstream outFile("ecmpProbability.txt");
  if (outFile.is_open()) {
      for (size_t i = 0; i < m_ratio.size(); ++i) {
          outFile << m_ratio[i] << std::endl;
      }
      outFile.close();
  } else {
      NS_LOG_ERROR("无法打开文件 ecmpProbability.txt 进行写入！");
  }
  return true;
}

std::string
MyGymEnv::GetExtraInfo()
{
  NS_LOG_INFO("MyGetExtraInfo: " << m_info);
  return m_info;
}

} // namespace ns3
