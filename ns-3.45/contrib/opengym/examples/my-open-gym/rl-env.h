#ifndef RL_ENV_H
#define RL_ENV_H

#include "ns3/opengym-interface.h"
#include "ns3/nstime.h"
#include <string>

namespace ns3 {

class MyGymEnv : public OpenGymEnv
{
public:
  MyGymEnv(std::string prot);
  MyGymEnv(Time stepTime);
  virtual ~MyGymEnv();

  virtual Ptr<OpenGymSpace> GetActionSpace() override;

  virtual bool GetGameOver() override;

  virtual float GetReward() override;

  virtual Ptr<OpenGymSpace> GetObservationSpace() override;

  virtual Ptr<OpenGymDataContainer> GetObservation() override;

  virtual bool ExecuteActions(Ptr<OpenGymDataContainer> action) override;

private:
  /**
   * \brief Schedule the next state read
   */
  void ScheduleNextStateRead();

  Time m_interval;           ///< Time interval for state reading
  Time m_timeStep;           ///< Time step for scheduling
  bool m_started;
  double m_total_throughput; ///< Total throughput
  double m_average_delay;    ///< Average delay
  double m_packet_loss_ratio;  ///< Packet loss ratio
  double m_pfc_trigger;      ///< PFC trigger count
  double m_link_load_var;    ///< Link load variance
  float m_envReward;         ///< Environment reward value
  bool m_isGameOver;         ///< Game over flag

};

} // namespace ns3

#endif // RL_ENV_H