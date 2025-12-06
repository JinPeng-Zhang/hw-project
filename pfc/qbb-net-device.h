#ifndef QBB_NET_DEVICE_H
#define QBB_NET_DEVICE_H

#include <ns3/point-to-point-net-device.h>
#include <ns3/event-id.h>
#include <ns3/data-rate.h>

#include <array>
#include <queue>
#include <deque>
#include <memory>
#include <map>
#include <set>

namespace ns3 {

// 简单按包粒度的令牌桶
class TokenBucket
{
public:
  TokenBucket(double pps, uint32_t burst);
  void Refill();
  bool TryConsume(uint32_t pkts = 1);
  void Refund(uint32_t pkts = 1);  // 新增：令牌退回

private:
  double m_pps;
  uint32_t m_capacity;
  double m_tokens;
  Time m_lastRefill;
};

// 每个出端口一个令牌桶 + 公平轮询仲裁器
class EgressTokenManager : public Object
{
public:
  static TypeId GetTypeId();
  EgressTokenManager();
  ~EgressTokenManager() override;

  void RegisterEgressPort(uint32_t portId, DataRate rate, uint32_t avgPktSize);
  bool TryConsumeToken(uint32_t portId, uint32_t pkts = 1);

  // 新增：公平令牌分配接口
  void RegisterDemand(uint32_t egressPortId, uint32_t ingressPortId);
  void UnregisterDemand(uint32_t egressPortId, uint32_t ingressPortId);
  bool TryConsumeTokenFair(uint32_t egressPortId, uint32_t ingressPortId, uint32_t pkts = 1);

private:
  std::map<uint32_t, std::unique_ptr<TokenBucket>> m_buckets;
  
  // 新增：每个出端口的轮询仲裁器
  struct PortArbiter {
    std::set<uint32_t> activeIngressPorts;  // 活跃入端口集合
    uint32_t lastServedIngress = 0;         // 上次授予的入端口id
  };
  std::map<uint32_t, PortArbiter> m_arbiters;
  
  uint32_t SelectNextIngress(PortArbiter& arbiter);
};

class QbbNetDevice : public PointToPointNetDevice
{
public:
  static TypeId GetTypeId();
  QbbNetDevice();
  ~QbbNetDevice() override;

  bool Send(Ptr<Packet> p, const Address& dest, uint16_t protocol) override;
  bool SendFrom(Ptr<Packet> p, const Address& src, const Address& dst, uint16_t proto) override;

  // MacRx trace 回调
  void HandleMacRx(Ptr<const Packet> p);

  static void PrintAllPfcCounters();

  // 用于调试的访问器
  uint32_t GetRxOccupancy(uint8_t prio) const { return m_rxOccPkts[prio]; }
  bool IsPaused(uint8_t prio) const { return m_paused[prio]; }
  Time GetPauseUntil(uint8_t prio) const { return m_pauseUntil[prio]; }
  uint64_t GetTxXoffCount(uint8_t prio) const { return m_pfcTxXoff[prio]; }
  uint64_t GetRxXoffCount(uint8_t prio) const { return m_pfcRxXoff[prio]; }
  uint64_t GetTxXonCount(uint8_t prio) const { return m_pfcTxXon[prio]; }
  uint64_t GetRxXonCount(uint8_t prio) const { return m_pfcRxXon[prio]; }

protected:
  // 覆盖聚合通知：在 Ipv4 绑定后安装"吞掉"回调，关闭基类上交
  void NotifyNewAggregate() override;

  // 解析优先级（IPv4 TOS 高3位）；安全处理 PPP 是否存在
  uint8_t GetPrioFromPacket(Ptr<const Packet> p) const;

private:
  static constexpr uint32_t kInvalidPortId = 0xFFFFFFFFu;
  bool IsLocalDelivery(Ptr<const Packet> pkt) const;
  uint32_t GetEgressPortOrInvalid(Ptr<const Packet> pkt) const;
  struct TxItem { Ptr<Packet> p; Address dst; uint16_t proto; };

  // 安装/重申"吞掉"上行回调（关闭基类上交）
  void HookL3Divert();

  // 吞掉回调：返回 true 表示"已处理"，阻断 Node/Ipv4（短签名）
  bool L3Swallow(Ptr<NetDevice> dev,
                 Ptr<const Packet> p,
                 uint16_t protocol,
                 const Address& from);

  // 发送侧：按优先级出队、Pause 门控
  void TryDequeue();
  int PickNextPrio() const;

  // 入端口排队与放行
  void DoIngressDrain();
  void OnDataRx(uint8_t prio);

  // 出端口速率门控
  void EnsureTokenManager();
  uint32_t GetEgressPortForPacket(Ptr<const Packet> pkt) const;

  // PFC 控制帧处理相关：MAC 队列清理/回填、到时恢复
  void PurgeMacQueueForPrio(uint8_t prio);
  void RefillMacQueueFromHold(uint8_t prio);
  void ResumeFromPause(uint8_t prio);

  // 发送 PFC 控制帧
  void SendPfcXoff(uint8_t prio, uint16_t quanta);
  void SendPfcXon(uint8_t prio);
  void BroadcastPfcXoff(uint8_t prio, uint16_t quanta);
  void BroadcastPfcXon(uint8_t prio);

  double GetBitTime() const;

  // 配置
  bool m_pfcEnable{true};
  uint32_t m_pfcHighPkts{8};
  uint32_t m_pfcLowPkts{4};
  uint16_t m_defaultQuanta{65535};
  uint32_t m_avgPktSize{1500};

  // 发送侧：8个优先级逻辑队列
  std::array<std::queue<TxItem>, 8> m_txq;

  // 入端口：8个优先级队列（方案B）
  std::array<std::deque<Ptr<Packet>>, 8> m_ingressQ;

  // MAC 队列 hold：8个优先级队列（Pause 残包暂存）
  std::array<std::deque<Ptr<Packet>>, 8> m_macHold;

  // Pause 状态
  std::array<bool, 8> m_paused;
  std::array<Time, 8> m_pauseUntil;

  // 入端口占用（单位：包）
  std::array<uint32_t, 8> m_rxOccPkts;
  std::array<bool, 8> m_localCongested;

  // PFC 计数
  std::array<uint64_t, 8> m_pfcTxXoff{};
  std::array<uint64_t, 8> m_pfcTxXon{};
  std::array<uint64_t, 8> m_pfcRxXoff{};
  std::array<uint64_t, 8> m_pfcRxXon{};

  // 事件
  EventId m_txEvent;
  EventId m_ingressDrainEv;
  std::array<EventId, 8> m_refillEvent; // 复用：回填/到时恢复

  Ptr<EgressTokenManager> m_tokenMgr;

  // 是否已安装"吞掉"回调
  bool m_l3DivertInstalled{false};
};

} // namespace ns3

#endif // QBB_NET_DEVICE_H
