// V53A standalone component smoke tests.
//
// Build (from D:\Buriki):
//   clang++ -std=c++20 -O2 -I recomp/runtime/audiocpu tools/v53_test/v53_test.cpp
//       recomp/runtime/audiocpu/v53a.cpp recomp/runtime/audiocpu/v53a_core.cpp
//       recomp/runtime/audiocpu/v53a_pit8254.cpp recomp/runtime/audiocpu/v53a_pic8259.cpp
//       recomp/runtime/audiocpu/v53a_dmau.cpp recomp/runtime/audiocpu/v53a_scu.cpp
//       -o tools/v53_test/v53_test.exe

#include "v53a.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond, ...) do { \
	if (cond) { std::printf("  PASS: "); std::printf(__VA_ARGS__); std::printf("\n"); } \
	else { std::printf("  FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); g_failures++; } \
} while (0)

// tiny hand assembler for the handful of instructions the tests use
struct Asm
{
	uint8_t *ram;
	uint32_t base;   // linear address of offset 0 of the segment
	uint16_t pc;     // offset within the segment

	Asm(uint8_t *r, uint16_t seg, uint16_t off) : ram(r), base(uint32_t(seg) << 4), pc(off) {}

	void b(uint8_t v) { ram[(base + pc) & 0xfffff] = v; pc++; }
	void w(uint16_t v) { b(uint8_t(v)); b(uint8_t(v >> 8)); }
	template <typename... T> void bytes(T... v) { (b(uint8_t(v)), ...); }

	uint16_t here() const { return pc; }
	void rel8_to(uint16_t target) { int d = int(target) - int(pc + 1); b(uint8_t(int8_t(d))); }

	void mov_r16(int r, uint16_t imm) { b(0xb8 + r); w(imm); }
	void outb(uint16_t port, uint8_t val) { mov_r16(2, port); bytes(0xb0, val); b(0xee); } // mov dx,port; mov al,val; out dx,al
};

enum { AX = 0, CX, DX, BX, SP, BP, SI, DI };

struct Board
{
	std::vector<uint8_t> ram = std::vector<uint8_t>(0x100000, 0);
	v53::V53A cpu;
	std::vector<std::pair<uint16_t, uint16_t>> io_writes16;   // port, value
	std::vector<std::pair<uint16_t, uint8_t>> io_writes8;
	std::vector<std::pair<uint64_t, uint16_t>> io_write_times;

	Board()
	{
		cpu.cb.mem_read8 = [this](uint32_t a) { return ram[a & 0xfffff]; };
		cpu.cb.mem_write8 = [this](uint32_t a, uint8_t d) { ram[a & 0xfffff] = d; };
		cpu.cb.mem_read16 = [this](uint32_t a) { a &= 0xffffe; return uint16_t(ram[a] | (ram[a + 1] << 8)); };
		cpu.cb.mem_write16 = [this](uint32_t a, uint16_t d) { a &= 0xffffe; ram[a] = uint8_t(d); ram[a + 1] = uint8_t(d >> 8); };
		cpu.cb.io_read8 = [](uint16_t) -> uint8_t { return 0xff; };
		cpu.cb.io_read16 = [](uint16_t) -> uint16_t { return 0xffff; };
		cpu.cb.io_write8 = [this](uint16_t p, uint8_t d) { io_writes8.push_back({p, d}); };
		cpu.cb.io_write16 = [this](uint16_t p, uint16_t d) { io_writes16.push_back({p, d}); io_write_times.push_back({cpu.total_cycles(), p}); };
		cpu.cb.log = [](const char *m) { std::printf("  [v53 log] %s", m); };
		// HNG64 glue: dma_hreq_cb() always calls hack_w(1)
		cpu.cb.dma_hreq = [this](int) { cpu.hack_w(1); };
	}

	uint16_t rd16(uint32_t a) const { return uint16_t(ram[a] | (ram[a + 1] << 8)); }

	// reset vector FFFF0: jmp far seg:off
	void reset_vector(uint16_t seg, uint16_t off)
	{
		Asm a(ram.data(), 0xffff, 0x0000);
		a.b(0xea); a.w(off); a.w(seg);
	}
};

// ---------------------------------------------------------------------------
// Test 1: arithmetic, string ops, mul/div, stack, call/ret, I/O
// ---------------------------------------------------------------------------
void test_cpu()
{
	std::printf("Test 1: CPU core (loop, stosw, rep movsw, lodsw, mul/div/idiv, push/pop, call/ret, out)\n");
	auto bd = std::make_unique<Board>();
	Board &B = *bd;
	B.reset_vector(0x1000, 0x0100);

	Asm a(B.ram.data(), 0x1000, 0x0100);
	a.b(0xfa);                         // cli
	a.mov_r16(AX, 0x2000);
	a.bytes(0x8e, 0xd0);               // mov ss,ax
	a.mov_r16(SP, 0x1000);
	a.mov_r16(AX, 0x3000);
	a.bytes(0x8e, 0xd8);               // mov ds,ax
	a.bytes(0x8e, 0xc0);               // mov es,ax
	a.bytes(0x31, 0xff);               // xor di,di
	a.mov_r16(CX, 16);
	a.mov_r16(AX, 1);
	uint16_t fill = a.here();
	a.b(0xab);                         // stosw
	a.b(0x05); a.w(3);                 // add ax,3
	a.b(0xe2); a.rel8_to(fill);        // loop fill
	a.bytes(0x31, 0xf6);               // xor si,si
	a.mov_r16(DI, 0x0100);
	a.mov_r16(CX, 16);
	a.b(0xfc);                         // cld
	a.bytes(0xf3, 0xa5);               // rep movsw
	a.mov_r16(SI, 0x0100);
	a.mov_r16(CX, 16);
	a.bytes(0x31, 0xdb);               // xor bx,bx
	uint16_t sum = a.here();
	a.b(0xad);                         // lodsw
	a.bytes(0x01, 0xc3);               // add bx,ax
	a.b(0xe2); a.rel8_to(sum);         // loop sum
	a.b(0x53);                         // push bx
	a.b(0xe8); uint16_t call_fix = a.here(); a.w(0); // call func
	a.b(0x5a);                         // pop dx          (dx = bx = 376)
	a.bytes(0x89, 0xc3);               // mov bx,ax
	a.bytes(0x01, 0xd3);               // add bx,dx
	a.bytes(0x89, 0xd8);               // mov ax,bx
	a.mov_r16(DX, 0x0040);
	a.b(0xef);                         // out dx,ax       -> port 0x40
	a.mov_r16(AX, uint16_t(-1000));
	a.b(0x99);                         // cwd
	a.mov_r16(CX, 7);
	a.bytes(0xf7, 0xf9);               // idiv cx
	a.bytes(0xe7, 0x44);               // out 0x44,ax
	a.bytes(0x89, 0xd0);               // mov ax,dx
	a.bytes(0xe7, 0x46);               // out 0x46,ax
	a.b(0xf4);                         // hlt
	uint16_t func = a.here();
	{
		int16_t rel = int16_t(func - (call_fix + 2));
		B.ram[(0x10000 + call_fix) & 0xfffff] = uint8_t(rel);
		B.ram[(0x10000 + call_fix + 1) & 0xfffff] = uint8_t(uint16_t(rel) >> 8);
	}
	a.b(0x55);                         // push bp
	a.bytes(0x89, 0xe5);               // mov bp,sp
	a.bytes(0x8b, 0x46, 0x04);         // mov ax,[bp+4]
	a.mov_r16(CX, 7);
	a.bytes(0xf7, 0xe1);               // mul cx          (2632)
	a.bytes(0x31, 0xd2);               // xor dx,dx
	a.mov_r16(CX, 5);
	a.bytes(0xf7, 0xf1);               // div cx          (526 r 2)
	a.bytes(0x01, 0xd0);               // add ax,dx       (528)
	a.b(0x5d);                         // pop bp
	a.b(0xc3);                         // ret

	B.cpu.reset();
	CHECK(B.cpu.cs() == 0xffff && B.cpu.ip() == 0x0000, "reset vector CS:IP = %04X:%04X", B.cpu.cs(), B.cpu.ip());

	int ran = B.cpu.run(20000);
	std::printf("  ran %d cycles, CS:IP=%04X:%04X halted=%d\n", ran, B.cpu.cs(), B.cpu.ip(), B.cpu.in_hlt());

	bool copy_ok = true;
	for (int i = 0; i < 16; i++)
		if (B.rd16(0x30000 + 0x100 + i * 2) != 1 + 3 * i)
			copy_ok = false;
	CHECK(copy_ok, "rep movsw copied 16 words correctly");

	auto port_val = [&](uint16_t p, uint16_t &v) {
		for (auto &w : B.io_writes16) if (w.first == p) { v = w.second; return true; }
		return false;
	};
	uint16_t v40 = 0, v44 = 0, v46 = 0;
	bool h40 = port_val(0x40, v40), h44 = port_val(0x44, v44), h46 = port_val(0x46, v46);
	CHECK(h40 && v40 == 904, "port 0x40 = %u (expected 904 = 376 + (376*7)/5 + (376*7)%%5)", v40);
	CHECK(h44 && v44 == 0xff72, "port 0x44 = %04X (expected FF72 = -1000/7)", v44);
	CHECK(h46 && v46 == 0xfffa, "port 0x46 = %04X (expected FFFA = -1000%%7)", v46);
	CHECK(B.cpu.in_hlt(), "CPU halted at the end (HLT)");
	CHECK(B.cpu.reg(SP) == 0x1000, "SP balanced (%04X)", B.cpu.reg(SP));
	for (auto &t : B.io_write_times)
		std::printf("  OUT %02Xh,AX at cycle %llu\n", t.second, (unsigned long long)t.first);
}

// common: map the V53 internal peripherals: OPHA=0x02 -> block at I/O 0x200-0x2ff
// 16-bit mode (SCTL bit 0 = 0): ICU at 0x210/0x212, TCU at 0x220/22/24/26, DMAU at 0x240-0x24f
void emit_v53_setup(Asm &a, uint8_t opsel)
{
	a.outb(0xfffe, 0x00);   // SCTL: 16-bit I/O mapping, uPD71071 DMA mode
	a.outb(0xfffc, 0x02);   // OPHA
	a.outb(0xfffa, 0x10);   // IULA
	a.outb(0xfff9, 0x20);   // TULA
	a.outb(0xfffb, 0x40);   // DULA
	a.outb(0xfff8, 0x60);   // SULA
	a.outb(0xfff0, 0x00);   // TCKS: all timers on clock/4 = 8 MHz
	a.outb(0xfffd, opsel);  // OPSEL
}

// ---------------------------------------------------------------------------
// Test 2: TCU timer 2 -> (board wiring) -> ICU IR2 -> interrupt handler
// ---------------------------------------------------------------------------
void test_timer_irq()
{
	std::printf("Test 2: TCU timer 2 (mode 3, count 1000 @ 8 MHz) -> IR2 -> ICU -> INT 22h handler\n");
	auto bd = std::make_unique<Board>();
	Board &B = *bd;
	B.reset_vector(0x1000, 0x0100);
	int tout2_edges = 0;
	B.cpu.cb.tout = [&](int timer, int state) {
		if (timer == 2)
		{
			if (state) tout2_edges++;
			B.cpu.set_input_line(v53::V53A::LINE_IRQ2, state != 0); // hng64 tcu_tm2_cb
		}
	};

	// IVT entry 0x22 -> 1000:0400
	B.ram[0x22 * 4 + 0] = 0x00; B.ram[0x22 * 4 + 1] = 0x04;
	B.ram[0x22 * 4 + 2] = 0x00; B.ram[0x22 * 4 + 3] = 0x10;

	Asm a(B.ram.data(), 0x1000, 0x0100);
	a.b(0xfa);                         // cli
	a.mov_r16(AX, 0x2000);
	a.bytes(0x8e, 0xd0);               // mov ss,ax
	a.mov_r16(SP, 0x1000);
	emit_v53_setup(a, 0x07);           // DMAU, ICU, TCU enabled
	// ICU: ICW1 = edge, single, ICW4 needed; ICW2 = vector base 0x20; ICW4 = x86 mode; OCW1 = only IR2 unmasked
	a.outb(0x210, 0x13);
	a.outb(0x212, 0x20);
	a.outb(0x212, 0x01);
	a.outb(0x212, 0xfb);
	// TCU timer 2: mode 3, LSB/MSB, count 1000
	a.outb(0x226, 0xb6);
	a.outb(0x224, 0xe8);
	a.outb(0x224, 0x03);
	a.b(0xfb);                         // sti
	uint16_t idle = a.here();
	a.b(0xf4);                         // hlt
	a.b(0xeb); a.rel8_to(idle);        // jmp idle

	Asm h(B.ram.data(), 0x1000, 0x0400);
	h.b(0x50);                         // push ax
	h.b(0x52);                         // push dx
	h.b(0x1e);                         // push ds
	h.mov_r16(AX, 0x3000);
	h.bytes(0x8e, 0xd8);               // mov ds,ax
	h.bytes(0xff, 0x06, 0x00, 0x00);   // inc word [0000]
	h.outb(0x210, 0x20);               // OCW2: non-specific EOI
	h.b(0x1f);                         // pop ds
	h.b(0x5a);                         // pop dx
	h.b(0x58);                         // pop ax
	h.b(0xcf);                         // iret

	B.cpu.reset();
	B.cpu.run(2000);  // setup code
	uint16_t c0 = B.rd16(0x30000);
	uint64_t t0 = B.cpu.total_cycles();
	std::printf("  after setup: cycle %llu, counter %u, ICU imr=%02X isr=%02X irr=%02X\n",
		(unsigned long long)t0, c0, B.cpu.icu().imr(), B.cpu.icu().isr(), B.cpu.icu().irr());

	const int span = 400000; // 100 periods of 4000 clocks
	B.cpu.run(span);
	uint16_t c1 = B.rd16(0x30000);
	uint64_t t1 = B.cpu.total_cycles();
	B.cpu.run(span);
	uint16_t c2 = B.rd16(0x30000);
	uint64_t t2 = B.cpu.total_cycles();

	double rate1 = double(c1 - c0) * 4000.0 / double(t1 - t0);
	std::printf("  counter %u -> %u -> %u over %llu / %llu clocks (expected ~1 per 4000 clocks = 8 kHz at 32 MHz)\n",
		c0, c1, c2, (unsigned long long)(t1 - t0), (unsigned long long)(t2 - t1));
	std::printf("  tout2 rising edges seen: %d\n", tout2_edges);
	CHECK(c1 - c0 >= 99 && c1 - c0 <= 101, "first window: %d interrupts (expected 100 +- 1, rate %.4f)", c1 - c0, rate1);
	CHECK(c2 - c1 == 100, "second window: %d interrupts (expected exactly 100)", c2 - c1);
	CHECK(int(c2) >= tout2_edges - 1 && int(c2) <= tout2_edges,
		"every timer edge produced one interrupt (counter %u, edges %d)", c2, tout2_edges);
	CHECK(B.cpu.icu().isr() == 0, "ICU ISR cleared by EOI (%02X)", B.cpu.icu().isr());
}

// ---------------------------------------------------------------------------
// Test 3: DMA channel 3, memory -> 16-bit I/O on dreq(3)
// ---------------------------------------------------------------------------
void test_dma()
{
	std::printf("Test 3: DMAU channel 3, 16-bit memory -> I/O, demand mode, 8 transfers on dreq(3)\n");
	auto bd = std::make_unique<Board>();
	Board &B = *bd;
	B.reset_vector(0x1000, 0x0100);

	// source data at 0x50000
	for (int i = 0; i < 16; i++)
	{
		B.ram[0x50000 + i * 2] = uint8_t(0x11 * (i + 1));
		B.ram[0x50000 + i * 2 + 1] = uint8_t(0xa0 + i);
	}

	std::vector<std::pair<uint32_t, uint16_t>> got;
	std::vector<uint64_t> got_time;
	int hreq_calls = 0, eop_rise = 0;
	B.cpu.cb.dma_mem_read16 = [&](uint32_t offset) { return B.rd16((offset << 1) & 0xffffe); };
	B.cpu.cb.dma_mem_read8 = [&](uint32_t addr) { return B.ram[addr & 0xfffff]; };
	B.cpu.cb.dma_io_write16 = [&](int ch, uint32_t offset, uint16_t data) {
		if (ch == 3)
		{
			got.push_back({offset, data});
			got_time.push_back(B.cpu.total_cycles());
			B.cpu.dreq(3, false); // the L7A1045 drops DRQ in dma_w16_cb
		}
	};
	B.cpu.cb.dma_hreq = [&](int) { hreq_calls++; B.cpu.hack_w(1); };
	B.cpu.cb.dma_eop = [&](int s) { if (s) eop_rise++; };

	Asm a(B.ram.data(), 0x1000, 0x0100);
	a.b(0xfa);                         // cli
	emit_v53_setup(a, 0x07);
	a.outb(0x248, 0x00);               // device control low: enabled, normal timing
	a.outb(0x241, 0x03);               // select channel 3 (base + current)
	a.outb(0x242, 0x07);               // count low  (8 transfers = count + 1)
	a.outb(0x243, 0x00);               // count high
	a.outb(0x244, 0x00);               // address low
	a.outb(0x245, 0x00);               // address mid
	a.outb(0x246, 0x05);               // address high -> 0x050000
	a.outb(0x24a, 0x09);               // mode: word, memory read -> I/O write, demand
	a.outb(0x24f, 0x07);               // mask: unmask channel 3
	a.b(0xf4);                         // hlt (DMA keeps running)

	B.cpu.reset();
	B.cpu.run(3000);
	std::printf("  setup done at cycle %llu, CPU halted=%d, mask=%02X ch3 addr=%06X count=%04X\n",
		(unsigned long long)B.cpu.total_cycles(), B.cpu.in_hlt(), B.cpu.dmau().mask(),
		B.cpu.dmau().channel_address(3), B.cpu.dmau().channel_count(3) & 0xffff);

	// the "DSP" raises DRQ every 1000 clocks, 12 times (only 8 must be served)
	for (int i = 0; i < 12; i++)
	{
		B.cpu.dreq(3, true);
		B.cpu.run(1000);
	}

	CHECK(got.size() == 8, "%zu transfers received (expected 8, then terminal count)", got.size());
	bool ok = got.size() == 8;
	for (size_t i = 0; i < got.size() && i < 8; i++)
	{
		uint16_t expect = uint16_t((0xa0 + i) << 8 | uint8_t(0x11 * (i + 1)));
		uint32_t expect_off = (0x50000 >> 1) + uint32_t(i);
		std::printf("    #%zu offset %05X data %04X (cycle %llu)%s\n", i, got[i].first, got[i].second,
			(unsigned long long)got_time[i], (got[i].second == expect && got[i].first == expect_off) ? "" : "  <-- mismatch");
		if (got[i].second != expect || got[i].first != expect_off)
			ok = false;
	}
	CHECK(ok, "all values and word offsets arrived in order");
	CHECK(eop_rise == 1, "EOP asserted once at terminal count (%d)", eop_rise);
	CHECK((B.cpu.dmau().mask() & 0x08) != 0, "channel 3 masked after terminal count (mask %02X)", B.cpu.dmau().mask());
	CHECK((B.cpu.dmau().status() & 0x08) != 0, "TC bit for channel 3 set in status (%02X)", B.cpu.dmau().status());
	std::printf("  hreq callbacks: %d\n", hreq_calls);
}

// ---------------------------------------------------------------------------
// Test 4: determinism and reset/halt lines
// ---------------------------------------------------------------------------
void test_determinism()
{
	std::printf("Test 4: determinism (same program twice, different run() slicing) + HALT/RESET lines\n");
	auto make = [](Board &B) {
		B.reset_vector(0x1000, 0x0100);
		B.cpu.cb.tout = [&B](int timer, int state) { if (timer == 2) B.cpu.set_input_line(2, state != 0); };
		B.ram[0x22 * 4 + 0] = 0x00; B.ram[0x22 * 4 + 1] = 0x04;
		B.ram[0x22 * 4 + 2] = 0x00; B.ram[0x22 * 4 + 3] = 0x10;
		Asm a(B.ram.data(), 0x1000, 0x0100);
		a.b(0xfa);
		a.mov_r16(AX, 0x2000); a.bytes(0x8e, 0xd0); a.mov_r16(SP, 0x1000);
		a.mov_r16(AX, 0x3000); a.bytes(0x8e, 0xd8);
		emit_v53_setup(a, 0x07);
		a.outb(0x210, 0x13); a.outb(0x212, 0x20); a.outb(0x212, 0x01); a.outb(0x212, 0xfb);
		a.outb(0x226, 0xb4);           // timer 2 mode 2 (rate generator)
		a.outb(0x224, 0x7b); a.outb(0x224, 0x00); // count 123
		a.b(0xfb);
		uint16_t loop = a.here();
		a.bytes(0xff, 0x06, 0x02, 0x00); // inc word [0002] (busy loop, counts iterations)
		a.b(0xeb); a.rel8_to(loop);
		Asm h(B.ram.data(), 0x1000, 0x0400);
		h.b(0x50); h.b(0x52);
		h.bytes(0xff, 0x06, 0x00, 0x00);
		h.outb(0x210, 0x20);
		h.b(0x5a); h.b(0x58); h.b(0xcf);
		B.cpu.reset();
	};
	auto b1 = std::make_unique<Board>(); make(*b1);
	auto b2 = std::make_unique<Board>(); make(*b2);
	b1->cpu.run(1000000);
	int slices = 0;
	while (b2->cpu.total_cycles() < 1000000)
	{
		// irregular slicing, same final target
		int n = int(1 + (b2->cpu.total_cycles() * 7919) % 3001);
		n = std::min<int>(n, int(1000000 - b2->cpu.total_cycles()));
		b2->cpu.run(n);
		slices++;
	}
	std::printf("  run B used %d run() calls\n", slices);
	std::printf("  run A: %llu clocks, irq count %u, loop count %u, CS:IP %04X:%04X\n",
		(unsigned long long)b1->cpu.total_cycles(), b1->rd16(0x30000), b1->rd16(0x30002), b1->cpu.cs(), b1->cpu.ip());
	std::printf("  run B: %llu clocks, irq count %u, loop count %u, CS:IP %04X:%04X\n",
		(unsigned long long)b2->cpu.total_cycles(), b2->rd16(0x30000), b2->rd16(0x30002), b2->cpu.cs(), b2->cpu.ip());
	CHECK(b1->rd16(0x30000) > 0 && b1->rd16(0x30000) == b2->rd16(0x30000) && b1->rd16(0x30002) == b2->rd16(0x30002) &&
		b1->cpu.total_cycles() == b2->cpu.total_cycles() && b1->cpu.ip() == b2->cpu.ip(),
		"state identical across different run() slicings (irqs %u vs %u, loops %u vs %u)",
		b1->rd16(0x30000), b2->rd16(0x30000), b1->rd16(0x30002), b2->rd16(0x30002));
	// irq every 123*4 = 492 clocks -> ~2032 in 1M clocks
	CHECK(std::abs(int(b1->rd16(0x30000)) - 1000000 / 492) <= 2, "mode 2 rate: %u irqs (expected ~%d)", b1->rd16(0x30000), 1000000 / 492);

	// same slicing twice must be bit-identical
	auto b3 = std::make_unique<Board>(); make(*b3);
	b3->cpu.run(1000000);
	CHECK(b3->cpu.total_cycles() == b1->cpu.total_cycles() && b3->rd16(0x30000) == b1->rd16(0x30000) &&
		b3->rd16(0x30002) == b1->rd16(0x30002) && b3->cpu.ip() == b1->cpu.ip(), "identical rerun is bit-exact");

	// HALT line: CPU stops, timer keeps running; interrupts are latched in the ICU
	uint16_t loops = b1->rd16(0x30002);
	uint16_t irqs = b1->rd16(0x30000);
	b1->cpu.set_halt_line(true);
	b1->cpu.run(100000);
	CHECK(b1->rd16(0x30002) == loops && b1->rd16(0x30000) == irqs, "HALT line stops execution");
	b1->cpu.set_halt_line(false);
	b1->cpu.run(10000);
	CHECK(b1->rd16(0x30002) != loops, "execution resumes after HALT released");

	// RESET line: hold, then release -> CPU restarts at FFFF:0000 with peripherals reset
	b1->cpu.set_reset_line(true);
	b1->cpu.run(1000);
	b1->cpu.set_reset_line(false);
	CHECK(b1->cpu.cs() == 0xffff && b1->cpu.ip() == 0 && b1->cpu.opsel() == 0, "RESET release resets CPU + V53 registers (CS:IP %04X:%04X OPSEL %02X)",
		b1->cpu.cs(), b1->cpu.ip(), b1->cpu.opsel());
	b1->cpu.run(10000);
	CHECK(b1->cpu.cs() == 0x1000, "runs again after reset (CS:IP %04X:%04X)", b1->cpu.cs(), b1->cpu.ip());
}

// ---------------------------------------------------------------------------
// Test 5: 8-bit internal mapping (SCTL bit 0), OPHA hole forwarding, XA translation
// ---------------------------------------------------------------------------
void test_mapping_xa()
{
	std::printf("Test 5: IOAG 8-bit peripheral mapping, internal-block hole forwarding, BRKXA/RETXA translation\n");
	auto bd = std::make_unique<Board>();
	Board &B = *bd;
	B.reset_vector(0x1000, 0x0100);
	std::vector<uint16_t> ext_reads;
	B.cpu.cb.io_read8 = [&](uint16_t p) -> uint8_t { ext_reads.push_back(p); return uint8_t(p ^ 0xa5); };

	B.ram[0x30000] = 0x34; B.ram[0x30001] = 0x12;   // logical DS:0 without XA
	B.ram[0x50000] = 0xcd; B.ram[0x50001] = 0xab;   // physical page 20, mapped at logical page 12 with XA
	// IVT 0x40 -> 1000:0300 (BRKXA), 0x41 -> 1000:0320 (RETXA)
	B.ram[0x40 * 4 + 0] = 0x00; B.ram[0x40 * 4 + 1] = 0x03; B.ram[0x40 * 4 + 2] = 0x00; B.ram[0x40 * 4 + 3] = 0x10;
	B.ram[0x41 * 4 + 0] = 0x20; B.ram[0x41 * 4 + 1] = 0x03; B.ram[0x41 * 4 + 2] = 0x00; B.ram[0x41 * 4 + 3] = 0x10;

	Asm a(B.ram.data(), 0x1000, 0x0100);
	a.b(0xfa);
	a.mov_r16(AX, 0x2000); a.bytes(0x8e, 0xd0); a.mov_r16(SP, 0x1000);
	a.mov_r16(AX, 0x3000); a.bytes(0x8e, 0xd8);
	a.outb(0xfffe, 0x01);              // SCTL: IOAG = 8-bit peripheral mapping
	a.outb(0xfffc, 0x02);              // OPHA
	a.outb(0xfffa, 0x10);              // IULA
	a.outb(0xfffd, 0x02);              // OPSEL: ICU only
	a.outb(0x210, 0x13); a.outb(0x211, 0x20); a.outb(0x211, 0x01); a.outb(0x211, 0x5a); // ICW1,2,4, OCW1 = 5A
	a.mov_r16(DX, 0x211); a.b(0xec);   // in al,dx  (IMR)
	a.bytes(0xe6, 0x4e);               // out 4Eh,al
	a.mov_r16(DX, 0x230); a.b(0xec);   // in al,dx  (hole in the internal block -> external I/O)
	a.bytes(0xe6, 0x4f);               // out 4Fh,al
	a.mov_r16(DX, 0xfffd); a.b(0xec);  // in al,dx  (OPSEL readback)
	a.bytes(0xe6, 0x50);               // out 50h,al
	// identity translation table, then logical page 12 (0x30000) -> physical page 20 (0x50000)
	a.mov_r16(DX, 0xff00); a.mov_r16(AX, 0); a.mov_r16(CX, 64);
	uint16_t tl = a.here();
	a.b(0xef);                         // out dx,ax
	a.bytes(0x83, 0xc2, 0x02);         // add dx,2
	a.b(0x40);                         // inc ax
	a.b(0xe2); a.rel8_to(tl);          // loop
	a.mov_r16(DX, 0xff18); a.mov_r16(AX, 20); a.b(0xef);
	a.bytes(0x0f, 0xe0, 0x40);         // BRKXA 40h

	Asm x(B.ram.data(), 0x1000, 0x0300);
	x.b(0xa1); x.w(0x0000);            // mov ax,[0000] -> physical 0x50000
	x.bytes(0xe7, 0x48);               // out 48h,ax
	x.mov_r16(DX, 0xff80); x.b(0xed);  // in ax,dx (XAM)
	x.bytes(0xe7, 0x4a);               // out 4Ah,ax
	x.bytes(0x0f, 0xf0, 0x41);         // RETXA 41h

	Asm y(B.ram.data(), 0x1000, 0x0320);
	y.b(0xa1); y.w(0x0000);            // mov ax,[0000] -> 0x30000
	y.bytes(0xe7, 0x4c);               // out 4Ch,ax
	y.b(0xf4);

	B.cpu.reset();
	B.cpu.run(10000);

	auto find16 = [&](uint16_t p, int &v) { for (auto &w : B.io_writes16) if (w.first == p) { v = w.second; return; } v = -1; };
	auto find8 = [&](uint16_t p, int &v) { for (auto &w : B.io_writes8) if (w.first == p) { v = w.second; return; } v = -1; };
	int imr, hole, opsel, xaval, xam, noxa;
	find8(0x4e, imr); find8(0x4f, hole); find8(0x50, opsel);
	find16(0x48, xaval); find16(0x4a, xam); find16(0x4c, noxa);
	CHECK(imr == 0x5a, "ICU IMR read back through 8-bit mapping at 0x211: %02X", imr);
	CHECK(hole == (0x30 ^ 0xa5) && !ext_reads.empty() && ext_reads.back() == 0x230,
		"unmapped internal-block port 0x230 forwarded to external I/O (%02X)", hole);
	CHECK(opsel == 0x02, "OPSEL readback at FFFDh: %02X", opsel);
	CHECK(xaval == 0xabcd, "XA on: DS:0000 (logical 30000) read from physical 50000: %04X", xaval);
	CHECK(xam == 1, "XAM register (FF80h) = %d while XA on", xam);
	CHECK(noxa == 0x1234 && !B.cpu.xa(), "RETXA: translation off again, DS:0000 = %04X", noxa);
}

} // anonymous namespace

int main()
{
	test_cpu();
	test_timer_irq();
	test_dma();
	test_determinism();
	test_mapping_xa();
	std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL TESTS PASSED", g_failures, g_failures == 1 ? "" : "s");
	return g_failures ? 1 : 0;
}
