#include "pfc-header.h"
#include <ns3/buffer.h>
#include <ns3/log.h>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("PfcHeader");
NS_OBJECT_ENSURE_REGISTERED(PfcHeader);

TypeId
PfcHeader::GetTypeId()
{
  static TypeId tid = TypeId("ns3::PfcHeader")
      .SetParent<Header>()
      .SetGroupName("Network");
  return tid;
}

TypeId
PfcHeader::GetInstanceTypeId() const
{
  return GetTypeId();
}

PfcHeader::PfcHeader()
  : m_opcode(PFC_OPCODE),
    m_classEnable(0),
    m_reserved(0),
    m_quanta{}
{
}

PfcHeader::~PfcHeader() = default;

void
PfcHeader::Print(std::ostream& os) const
{
  os << "opcode=0x" << std::hex << m_opcode << std::dec
     << " mask=0x" << std::hex << static_cast<unsigned>(m_classEnable) << std::dec
     << " quanta=[";
  for (size_t i = 0; i < 8; ++i)
  {
    if (i) os << ",";
    os << m_quanta[i];
  }
  os << "]";
}

uint32_t
PfcHeader::GetSerializedSize() const
{
  return 2 + 1 + 1 + 16; // opcode + mask + reserved + 8*quanta
}

void
PfcHeader::Serialize(Buffer::Iterator i) const
{
  i.WriteHtonU16(m_opcode);
  i.WriteU8(m_classEnable);
  i.WriteU8(m_reserved);
  for (size_t p = 0; p < 8; ++p)
  {
    i.WriteHtonU16(m_quanta[p]);
  }
}

uint32_t
PfcHeader::Deserialize(Buffer::Iterator i)
{
  m_opcode = i.ReadNtohU16();
  m_classEnable = i.ReadU8();
  m_reserved = i.ReadU8();
  for (size_t p = 0; p < 8; ++p)
  {
    m_quanta[p] = i.ReadNtohU16();
  }
  return GetSerializedSize();
}

} // namespace ns3
