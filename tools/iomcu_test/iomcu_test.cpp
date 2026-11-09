// license:BSD-3-Clause
// copyright-holders:David Haywood
//
// iomcu_test: smoke test for the standalone TLCS-870 (TMP87PH40AN) core, ported from MAME
// (original core by David Haywood).  Runs the real HNG64 I/O MCU firmware with stub ports.
//
// Build (from D:\Buriki):
//   clang++ -std=c++20 -O2 -Wall -Irecomp/runtime/iomcu tools/iomcu_test/iomcu_test.cpp
//       recomp/runtime/iomcu/tlcs870.cpp recomp/runtime/iomcu/tlcs870_ops.cpp
//       recomp/runtime/iomcu/tlcs870_ops_prefix.cpp -o tools/iomcu_test/iomcu_test.exe
// Usage:  iomcu_test [rom path] [--fix] [--log N]

#include "tlcs870.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
	const char* rom_path = "D:\\Buriki\\roms\\hng64\\tmp87ph40an.bin";
	bool fix = false;
	int max_log = 20;
	for (int i = 1; i < argc; i++)
	{
		if (!std::strcmp(argv[i], "--fix")) fix = true;
		else if (!std::strcmp(argv[i], "--log") && i + 1 < argc) max_log = std::atoi(argv[++i]);
		else rom_path = argv[i];
	}

	std::vector<uint8_t> rom;
	{
		FILE* f = std::fopen(rom_path, "rb");
		if (!f)
		{
			std::printf("cannot open %s\n", rom_path);
			return 2;
		}
		uint8_t buf[4096];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
			rom.insert(rom.end(), buf, buf + n);
		std::fclose(f);
	}
	std::printf("ROM: %s (%zu bytes)%s\n", rom_path, rom.size(), fix ? "  [fix_mame_bugs]" : "  [MAME-faithful]");

	Tlcs870 cpu;
	cpu.fix_mame_bugs = fix;

	uint64_t writes[8] = {};
	uint8_t last_write[8] = {};
	std::set<uint8_t> distinct[8];
	uint64_t reads[8] = {};
	for (int p = 0; p < 8; p++)
	{
		cpu.port_out[p] = [&, p](uint8_t v) { writes[p]++; last_write[p] = v; distinct[p].insert(v); };
	}
	cpu.port_in[0] = [&]() -> uint8_t { reads[0]++; return 0xff; };
	cpu.port_in[3] = [&]() -> uint8_t { reads[3]++; return 0xff; };
	uint64_t serial_bits = 0;
	cpu.serial_out[0] = [&](int) { serial_bits++; };

	std::map<std::string, uint64_t> log_counts;
	int logged = 0;
	cpu.log = [&](const char* s) {
		// collapse identical messages
		if (log_counts[s]++ == 0 && logged < max_log)
		{
			std::printf("  [core] %s\n", s);
			logged++;
		}
	};

	cpu.load_rom(rom.data(), rom.size());
	cpu.reset();
	std::printf("reset PC = %04X\n", cpu.pc());

	// The firmware's power-on RAM test alone takes ~7.9M cycles with MAME's 1 clock per machine
	// cycle timing, and INT0 is ignored (IMF=0) until it finishes.  So: run the requested 8M cycles
	// (with two early INT0 pulses that must be harmlessly ignored), print a checkpoint, then keep
	// going to 16M cycles with INT0 pulses once the main loop is up.
	const uint64_t checkpoint = 8'000'000;
	const uint64_t total_target = 16'000'000;
	const uint64_t pulse_at[] = { 3'000'000, 6'000'000, 9'000'000, 10'000'000, 11'000'000, 12'000'000, 13'000'000 };
	size_t next_pulse = 0;
	uint64_t pulse_off_at = 0;
	bool int0_high = false;

	std::vector<uint32_t> pc_hist(0x10000, 0);
	uint64_t outside_rom = 0, instr = 0, sp_out_of_ram = 0;
	uint16_t first_bad_pc = 0;
	std::map<uint16_t, uint64_t> irq_by_vector;   // vector address -> times taken
	std::vector<uint64_t> int0_entry_cycles;
	const uint16_t int0_vector = uint16_t(rom[rom.size() - 8] | (rom[rom.size() - 7] << 8)); // FFF8
	std::set<uint16_t> pcs_last_quarter;
	uint16_t min_sp = 0xffff, max_sp = 0;
	bool sp_initialised = false;
	bool checkpoint_printed = false;
	uint64_t writes_at_checkpoint[8] = {};

	uint64_t cyc = 0;
	while (cyc < total_target)
	{
		if (!checkpoint_printed && cyc >= checkpoint)
		{
			checkpoint_printed = true;
			std::memcpy(writes_at_checkpoint, writes, sizeof(writes));
			std::printf("checkpoint @%llu cycles: PC=%04X SP=%04X EIR=%04X IL=%04X irqs=%llu writes P0=%llu P1=%llu P3=%llu P7=%llu\n",
				(unsigned long long)cpu.total_cycles(), cpu.pc(), cpu.sp(), cpu.eir(), cpu.il(),
				(unsigned long long)cpu.stats().interrupts_taken, (unsigned long long)writes[0], (unsigned long long)writes[1],
				(unsigned long long)writes[3], (unsigned long long)writes[7]);
		}
		if (next_pulse < std::size(pulse_at) && !int0_high && cyc >= pulse_at[next_pulse])
		{
			cpu.set_input_line(Tlcs870::INPUT_INT0, true);
			int0_high = true;
			pulse_off_at = cyc + 1000;
			next_pulse++;
		}
		if (int0_high && cyc >= pulse_off_at)
		{
			cpu.set_input_line(Tlcs870::INPUT_INT0, false);
			int0_high = false;
		}

		const uint64_t irqs_before = cpu.stats().interrupts_taken;
		cyc += cpu.run(1); // exactly one instruction (interrupt entry + first handler instruction if taken)
		instr++;

		const uint16_t pc = cpu.prev_pc(); // address of the instruction just executed
		pc_hist[pc]++;
		if (cpu.stats().interrupts_taken != irqs_before)
		{
			irq_by_vector[pc]++;
			if (pc == int0_vector) int0_entry_cycles.push_back(cyc);
		}
		if (pc < 0xc000)
		{
			if (!outside_rom) first_bad_pc = pc;
			outside_rom++;
		}
		if (cyc >= total_target * 7 / 8) pcs_last_quarter.insert(pc);

		const uint16_t sp = cpu.sp();
		if (sp >= 0x40 && sp <= 0x23f) sp_initialised = true;
		if (sp_initialised)
		{
			if (sp < min_sp) min_sp = sp;
			if (sp > max_sp) max_sp = sp;
			if (sp < 0x40 || sp > 0x23f) sp_out_of_ram++;
		}
	}

	// Also exercise the bulk run() path for determinism (10M cycles, past boot): a fresh core with the same inputs must
	// reach the same state when driven by large run() slices (only the INT0 pulses are aligned to
	// slice boundaries, so we compare a run without pulses).
	// The fingerprint is a hash of every port write (port, value, cycle timestamp) before 9.9M cycles.
	auto run_bulk = [&](int slice) {
		Tlcs870 c;
		c.fix_mame_bugs = fix;
		c.port_in[0] = [] { return uint8_t(0xff); };
		c.port_in[3] = [] { return uint8_t(0xff); };
		uint32_t h = 2166136261u;
		uint64_t nw = 0;
		for (int p = 0; p < 8; p++)
			c.port_out[p] = [&, p](uint8_t v) {
				if (c.total_cycles() >= 9'900'000) return;
				nw++;
				const uint64_t t = c.total_cycles();
				for (uint64_t x : { uint64_t(p), uint64_t(v), t }) h = (h ^ uint32_t(x ^ (x >> 32))) * 16777619u;
			};
		c.load_rom(rom.data(), rom.size());
		c.reset();
		uint64_t n = 0;
		while (n < 10'000'000) n += c.run(slice);
		return std::make_pair(nw, h);
	};
	auto b1 = run_bulk(1), b2 = run_bulk(997), b3 = run_bulk(100000);

	const Tlcs870::Stats& s = cpu.stats();

	std::printf("\n=== summary ===\n");
	std::printf("cycles run        : %llu (%llu instructions, avg %.2f cycles/instr)\n",
		(unsigned long long)cpu.total_cycles(), (unsigned long long)instr, double(cpu.total_cycles()) / double(instr));
	std::printf("final PC / SP     : %04X / %04X   PSW=%02X  EIR=%04X  IL=%04X\n", cpu.pc(), cpu.sp(), cpu.psw(), cpu.eir(), cpu.il());
	std::printf("SP range seen     : %04X-%04X\n", min_sp, max_sp);
	std::printf("interrupts taken  : %llu   by vector:", (unsigned long long)s.interrupts_taken);
	for (auto& [v, n] : irq_by_vector) std::printf(" %04X=%llu", v, (unsigned long long)n);
	std::printf("\nINT0 (vector %04X): %zu pulses, %zu handler entries, at cycles:", int0_vector, std::size(pulse_at), int0_entry_cycles.size());
	for (uint64_t c : int0_entry_cycles) std::printf(" %llu", (unsigned long long)c);
	std::printf("\nport writes @8M   :");
	for (int p = 0; p < 8; p++) std::printf(" P%d=%llu", p, (unsigned long long)writes_at_checkpoint[p]);
	std::printf("\nport writes total :");
	for (int p = 0; p < 8; p++) std::printf(" P%d=%llu", p, (unsigned long long)writes[p]);
	std::printf("\nport last/distinct:");
	for (int p = 0; p < 8; p++) std::printf(" P%d=%02X/%zu", p, last_write[p], distinct[p].size());
	std::printf("\nport reads (cb)   : P0=%llu P3=%llu\n", (unsigned long long)reads[0], (unsigned long long)reads[3]);
	std::printf("serial0 bits out  : %llu\n", (unsigned long long)serial_bits);

	std::printf("\n--- unimplemented / unhandled events ---\n");
	std::printf("illegal opcodes   : base=%llu src=%llu dst=%llu reg=%llu (last at %04X)\n",
		(unsigned long long)s.illegal_opcode, (unsigned long long)s.illegal_src_opcode,
		(unsigned long long)s.illegal_dst_opcode, (unsigned long long)s.illegal_reg_opcode, s.last_illegal_pc);
	std::printf("unmapped reads    : %llu   unmapped writes: %llu   (last addr %04X)\n",
		(unsigned long long)s.unmapped_read, (unsigned long long)s.unmapped_write, s.last_unmapped_addr);
	for (int a = 0; a < 0x40; a++)
	{
		if (s.sfr_unmapped_read[a] || s.sfr_unmapped_write[a])
			std::printf("   SFR %02X: reads(write-only/reserved)=%llu writes(read-only/reserved)=%llu\n", a,
				(unsigned long long)s.sfr_unmapped_read[a], (unsigned long long)s.sfr_unmapped_write[a]);
	}
	std::printf("ROM writes        : %llu   reads of 8000-BFFF: %llu\n", (unsigned long long)s.rom_write, (unsigned long long)s.rom_lo_access);
	std::printf("SWI executed      : %llu   0-cycle instrs: %llu   IDLE/STOP requests: %llu\n",
		(unsigned long long)s.swi_executed, (unsigned long long)s.zero_cycle_instr, (unsigned long long)s.idle_requests);
	std::printf("watchdog          : clear(4E)=%llu disable(B1)=%llu\n", (unsigned long long)s.wdt_clear, (unsigned long long)s.wdt_disable);
	if (!log_counts.empty())
	{
		std::printf("distinct core log messages: %zu\n", log_counts.size());
		int shown = 0;
		for (auto& [msg, n] : log_counts)
		{
			if (shown++ >= max_log) break;
			std::printf("   %8llux  %s\n", (unsigned long long)n, msg.c_str());
		}
	}

	// hottest PCs
	std::vector<std::pair<uint32_t, uint16_t>> hot;
	for (int a = 0; a < 0x10000; a++)
		if (pc_hist[a]) hot.push_back({ pc_hist[a], uint16_t(a) });
	std::sort(hot.begin(), hot.end(), [](auto& x, auto& y) { return x.first > y.first; });
	std::printf("\ndistinct PCs executed: %zu; hottest:", hot.size());
	for (size_t i = 0; i < hot.size() && i < 8; i++) std::printf(" %04X(%u)", hot[i].second, hot[i].first);
	std::printf("\n");

	std::printf("determinism (port-write trace, run() slices 1/997/100000): %s  [writes/hash %llu/%08X %llu/%08X %llu/%08X]\n",
		(b1 == b2 && b2 == b3) ? "identical" : "DIFFERENT",
		(unsigned long long)b1.first, b1.second, (unsigned long long)b2.first, b2.second, (unsigned long long)b3.first, b3.second);

	const bool pc_ok = outside_rom == 0;
	const bool not_stuck = pcs_last_quarter.size() > 1;
	const bool no_illegal = (s.illegal_opcode + s.illegal_src_opcode + s.illegal_dst_opcode + s.illegal_reg_opcode) == 0;
	const bool sp_ok = sp_out_of_ram == 0;
	std::printf("\nhealth: PC always in ROM (C000-FFFF): %s", pc_ok ? "yes" : "NO");
	if (!pc_ok) std::printf(" (%llu instrs outside, first at %04X)", (unsigned long long)outside_rom, first_bad_pc);
	std::printf("; not stuck (distinct PCs in last 2M cycles = %zu): %s; SP in RAM after init: %s; no illegal ops: %s; INT0 serviced: %s\n",
		pcs_last_quarter.size(), not_stuck ? "yes" : "NO", sp_ok ? "yes" : "NO", no_illegal ? "yes" : "NO",
		int0_entry_cycles.empty() ? "NO" : "yes");
	const bool healthy = pc_ok && not_stuck && sp_ok && no_illegal && !int0_entry_cycles.empty();
	std::printf("RESULT: %s\n", healthy ? "HEALTHY" : "SUSPICIOUS");
	return healthy ? 0 : 1;
}
