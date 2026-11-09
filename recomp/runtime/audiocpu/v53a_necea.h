// license:BSD-3-Clause
// copyright-holders:Bryan McPhail
// Ported from MAME src/devices/cpu/nec/necea.h for a standalone V53A component:
// class renamed nec_common_device -> V53A (see v53a.h); otherwise unchanged.

uint32_t V53A::EA_000() { m_EO=Wreg(BW)+Wreg(IX); m_EA=DefaultBase(DS0)+m_EO; return m_EA; }
uint32_t V53A::EA_001() { m_EO=Wreg(BW)+Wreg(IY); m_EA=DefaultBase(DS0)+m_EO; return m_EA; }
uint32_t V53A::EA_002() { m_EO=Wreg(BP)+Wreg(IX); m_EA=DefaultBase(SS)+m_EO; return m_EA; }
uint32_t V53A::EA_003() { m_EO=Wreg(BP)+Wreg(IY); m_EA=DefaultBase(SS)+m_EO; return m_EA; }
uint32_t V53A::EA_004() { m_EO=Wreg(IX); m_EA=DefaultBase(DS0)+m_EO; return m_EA; }
uint32_t V53A::EA_005() { m_EO=Wreg(IY); m_EA=DefaultBase(DS0)+m_EO; return m_EA; }
uint32_t V53A::EA_006() { m_EO=fetch(); m_EO+=fetch()<<8; m_EA=DefaultBase(DS0)+m_EO; return m_EA; }
uint32_t V53A::EA_007() { m_EO=Wreg(BW); m_EA=DefaultBase(DS0)+m_EO; return m_EA; }

uint32_t V53A::EA_100() { m_EO=(Wreg(BW)+Wreg(IX)+(int8_t)fetch()); m_EA=DefaultBase(DS0)+m_EO; return m_EA; }
uint32_t V53A::EA_101() { m_EO=(Wreg(BW)+Wreg(IY)+(int8_t)fetch()); m_EA=DefaultBase(DS0)+m_EO; return m_EA; }
uint32_t V53A::EA_102() { m_EO=(Wreg(BP)+Wreg(IX)+(int8_t)fetch()); m_EA=DefaultBase(SS)+m_EO; return m_EA; }
uint32_t V53A::EA_103() { m_EO=(Wreg(BP)+Wreg(IY)+(int8_t)fetch()); m_EA=DefaultBase(SS)+m_EO; return m_EA; }
uint32_t V53A::EA_104() { m_EO=(Wreg(IX)+(int8_t)fetch()); m_EA=DefaultBase(DS0)+m_EO; return m_EA; }
uint32_t V53A::EA_105() { m_EO=(Wreg(IY)+(int8_t)fetch()); m_EA=DefaultBase(DS0)+m_EO; return m_EA; }
uint32_t V53A::EA_106() { m_EO=(Wreg(BP)+(int8_t)fetch()); m_EA=DefaultBase(SS)+m_EO; return m_EA; }
uint32_t V53A::EA_107() { m_EO=(Wreg(BW)+(int8_t)fetch()); m_EA=DefaultBase(DS0)+m_EO; return m_EA; }

uint32_t V53A::EA_200() { m_E16=fetch(); m_E16+=fetch()<<8; m_EO=Wreg(BW)+Wreg(IX)+(int16_t)m_E16; m_EA=DefaultBase(DS0)+m_EO; return m_EA; }
uint32_t V53A::EA_201() { m_E16=fetch(); m_E16+=fetch()<<8; m_EO=Wreg(BW)+Wreg(IY)+(int16_t)m_E16; m_EA=DefaultBase(DS0)+m_EO; return m_EA; }
uint32_t V53A::EA_202() { m_E16=fetch(); m_E16+=fetch()<<8; m_EO=Wreg(BP)+Wreg(IX)+(int16_t)m_E16; m_EA=DefaultBase(SS)+m_EO; return m_EA; }
uint32_t V53A::EA_203() { m_E16=fetch(); m_E16+=fetch()<<8; m_EO=Wreg(BP)+Wreg(IY)+(int16_t)m_E16; m_EA=DefaultBase(SS)+m_EO; return m_EA; }
uint32_t V53A::EA_204() { m_E16=fetch(); m_E16+=fetch()<<8; m_EO=Wreg(IX)+(int16_t)m_E16; m_EA=DefaultBase(DS0)+m_EO; return m_EA; }
uint32_t V53A::EA_205() { m_E16=fetch(); m_E16+=fetch()<<8; m_EO=Wreg(IY)+(int16_t)m_E16; m_EA=DefaultBase(DS0)+m_EO; return m_EA; }
uint32_t V53A::EA_206() { m_E16=fetch(); m_E16+=fetch()<<8; m_EO=Wreg(BP)+(int16_t)m_E16; m_EA=DefaultBase(SS)+m_EO; return m_EA; }
uint32_t V53A::EA_207() { m_E16=fetch(); m_E16+=fetch()<<8; m_EO=Wreg(BW)+(int16_t)m_E16; m_EA=DefaultBase(DS0)+m_EO; return m_EA; }

const V53A::nec_eahandler V53A::s_GetEA[192]=
{
	&V53A::EA_000, &V53A::EA_001, &V53A::EA_002, &V53A::EA_003, &V53A::EA_004, &V53A::EA_005, &V53A::EA_006, &V53A::EA_007,
	&V53A::EA_000, &V53A::EA_001, &V53A::EA_002, &V53A::EA_003, &V53A::EA_004, &V53A::EA_005, &V53A::EA_006, &V53A::EA_007,
	&V53A::EA_000, &V53A::EA_001, &V53A::EA_002, &V53A::EA_003, &V53A::EA_004, &V53A::EA_005, &V53A::EA_006, &V53A::EA_007,
	&V53A::EA_000, &V53A::EA_001, &V53A::EA_002, &V53A::EA_003, &V53A::EA_004, &V53A::EA_005, &V53A::EA_006, &V53A::EA_007,
	&V53A::EA_000, &V53A::EA_001, &V53A::EA_002, &V53A::EA_003, &V53A::EA_004, &V53A::EA_005, &V53A::EA_006, &V53A::EA_007,
	&V53A::EA_000, &V53A::EA_001, &V53A::EA_002, &V53A::EA_003, &V53A::EA_004, &V53A::EA_005, &V53A::EA_006, &V53A::EA_007,
	&V53A::EA_000, &V53A::EA_001, &V53A::EA_002, &V53A::EA_003, &V53A::EA_004, &V53A::EA_005, &V53A::EA_006, &V53A::EA_007,
	&V53A::EA_000, &V53A::EA_001, &V53A::EA_002, &V53A::EA_003, &V53A::EA_004, &V53A::EA_005, &V53A::EA_006, &V53A::EA_007,

	&V53A::EA_100, &V53A::EA_101, &V53A::EA_102, &V53A::EA_103, &V53A::EA_104, &V53A::EA_105, &V53A::EA_106, &V53A::EA_107,
	&V53A::EA_100, &V53A::EA_101, &V53A::EA_102, &V53A::EA_103, &V53A::EA_104, &V53A::EA_105, &V53A::EA_106, &V53A::EA_107,
	&V53A::EA_100, &V53A::EA_101, &V53A::EA_102, &V53A::EA_103, &V53A::EA_104, &V53A::EA_105, &V53A::EA_106, &V53A::EA_107,
	&V53A::EA_100, &V53A::EA_101, &V53A::EA_102, &V53A::EA_103, &V53A::EA_104, &V53A::EA_105, &V53A::EA_106, &V53A::EA_107,
	&V53A::EA_100, &V53A::EA_101, &V53A::EA_102, &V53A::EA_103, &V53A::EA_104, &V53A::EA_105, &V53A::EA_106, &V53A::EA_107,
	&V53A::EA_100, &V53A::EA_101, &V53A::EA_102, &V53A::EA_103, &V53A::EA_104, &V53A::EA_105, &V53A::EA_106, &V53A::EA_107,
	&V53A::EA_100, &V53A::EA_101, &V53A::EA_102, &V53A::EA_103, &V53A::EA_104, &V53A::EA_105, &V53A::EA_106, &V53A::EA_107,
	&V53A::EA_100, &V53A::EA_101, &V53A::EA_102, &V53A::EA_103, &V53A::EA_104, &V53A::EA_105, &V53A::EA_106, &V53A::EA_107,

	&V53A::EA_200, &V53A::EA_201, &V53A::EA_202, &V53A::EA_203, &V53A::EA_204, &V53A::EA_205, &V53A::EA_206, &V53A::EA_207,
	&V53A::EA_200, &V53A::EA_201, &V53A::EA_202, &V53A::EA_203, &V53A::EA_204, &V53A::EA_205, &V53A::EA_206, &V53A::EA_207,
	&V53A::EA_200, &V53A::EA_201, &V53A::EA_202, &V53A::EA_203, &V53A::EA_204, &V53A::EA_205, &V53A::EA_206, &V53A::EA_207,
	&V53A::EA_200, &V53A::EA_201, &V53A::EA_202, &V53A::EA_203, &V53A::EA_204, &V53A::EA_205, &V53A::EA_206, &V53A::EA_207,
	&V53A::EA_200, &V53A::EA_201, &V53A::EA_202, &V53A::EA_203, &V53A::EA_204, &V53A::EA_205, &V53A::EA_206, &V53A::EA_207,
	&V53A::EA_200, &V53A::EA_201, &V53A::EA_202, &V53A::EA_203, &V53A::EA_204, &V53A::EA_205, &V53A::EA_206, &V53A::EA_207,
	&V53A::EA_200, &V53A::EA_201, &V53A::EA_202, &V53A::EA_203, &V53A::EA_204, &V53A::EA_205, &V53A::EA_206, &V53A::EA_207,
	&V53A::EA_200, &V53A::EA_201, &V53A::EA_202, &V53A::EA_203, &V53A::EA_204, &V53A::EA_205, &V53A::EA_206, &V53A::EA_207
};
