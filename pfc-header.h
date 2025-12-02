#ifndef PFC_HEADER_H
#define PFC_HEADER_H

#include <array>
#include <cstdint>
#include <ostream>

#include <ns3/header.h>
#include <ns3/type-id.h>

namespace ns3 {

// 自定义 PPP 协议号承载 PFC 控制帧
#define PFC_PPP_PROTO 0x9000

// 类 802.1Qbb PFC 的 opcode（可读性）
#define PFC_OPCODE 0x0101

class PfcHeader : public Header
{
public:
  static TypeId GetTypeId();
  TypeId GetInstanceTypeId() const override;

  PfcHeader();
  ~PfcHeader() override;

  void Print(std::ostream& os) const override;
  uint32_t GetSerializedSize() const override;
  void Serialize(Buffer::Iterator i) const override;
  uint32_t Deserialize(Buffer::Iterator i) override;

  void SetClassEnable(uint8_t mask) { m_classEnable = mask; }
  uint8_t GetClassEnable() const { return m_classEnable; }

  void SetPauseQuanta(uint8_t prio, uint16_t quanta)
  {
    if (prio < 8) { m_quanta[prio] = quanta; }
  }
  uint16_t GetPauseQuanta(uint8_t prio) const
  {
    return (prio < 8) ? m_quanta[prio] : 0;
  }

private:
  uint16_t m_opcode;
  uint8_t  m_classEnable;
  uint8_t  m_reserved;
  uint16_t m_quanta[8];
};

} // namespace ns3

#endif // PFC_HEADER_H
