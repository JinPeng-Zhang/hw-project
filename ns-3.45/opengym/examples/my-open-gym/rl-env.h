#ifndef RL_ENV_H
#define RL_ENV_H

#include "ns3/opengym-interface.h"
#include "ns3/opengym-space.h"
#include "ns3/opengym-data-container.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"

namespace ns3 {

class MyGymEnv : public OpenGymEnv {
public:
  // 构造函数和析构函数
  MyGymEnv(std::string prot);
  MyGymEnv(std::string prot, Time timeStep);
  virtual ~MyGymEnv();

  // 类型注册相关
  static TypeId GetTypeId(void);

  // OpenGym接口实现
  virtual Ptr<OpenGymSpace> GetActionSpace();
  virtual Ptr<OpenGymSpace> GetObservationSpace();
  virtual Ptr<OpenGymDataContainer> GetObservation();
  virtual bool GetGameOver();
  virtual float GetReward();
  virtual std::string GetExtraInfo();
  virtual bool ExecuteActions(Ptr<OpenGymDataContainer> action);

  // 其他公共方法
  void ScheduleNextStateRead();

  // Trace函数
  double LinkloadTrace();
  double QueueSizeTrace();
  double FlowDistributionTrace();
  double EcnMarkRatioTrace();
  double PacketLossRatioTrace();
  double OutOfOrderRatioTrace();
  double PfcTriggerTrace();
  double AverageDelayTrace();
  double TotalThroughputTrace();

protected:
  virtual void DoDispose();

private:
  bool m_isGameOver;
  float m_envReward;
  std::string m_info;
  uint32_t m_new_ratio;
  Time m_timeStep;
  
  // 观察数据相关
  double link_load;
  double m_ratio;
  double m_ecnMarkRatio;
  double m_pfcTriggers;
  double m_endToEndDelay;
  double m_outOfOrderRatio;
  double m_packetLossRatio;
  
  
  // 统计数据
  uint64_t m_totalBytesTx;
  uint64_t m_bytesTx;
  uint64_t m_bytesRx;
  
  // NS3对象指针（需要在实现中初始化）
  Ptr<Node> node;
  Ptr<NetDevice> dev;
  Ptr<FlowMonitor> monitor;
  double simulationTime;
};

} // namespace ns3

#endif // RL_ENV_H