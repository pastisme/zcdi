#pragma once
#include <cstdint>
#include <string>

namespace zcdi {
namespace mamecdi {

// 30 MHz crystal divided by 2; Musashi's
// timing tables count these 15 MHz cycles.
constexpr int kCpuClockHz = 15000000;

// On-chip peripheral registers (byte addresses) and their bits.
namespace scc68070 {

constexpr uint32_t kLir = 0x80001001;

constexpr uint32_t kI2cIdr = 0x80002001;
constexpr uint32_t kI2cIar = 0x80002003;
constexpr uint32_t kI2cIsr = 0x80002005; // read-only, fixed "idle" value
constexpr uint32_t kI2cIcr = 0x80002007;
constexpr uint32_t kI2cIccr = 0x80002009;
constexpr uint8_t kI2cIsrIdle = 0x10; // ISR_PIN: no I2C interrupt pending

constexpr uint32_t kUartUmr = 0x80002011;
constexpr uint32_t kUartUsr = 0x80002013;
constexpr uint32_t kUartUcsr = 0x80002015;
constexpr uint32_t kUartUcr = 0x80002017;
constexpr uint32_t kUartUth = 0x80002019;
constexpr uint32_t kUartUrh = 0x8000201b;
constexpr uint8_t kUsrRxRdy = 0x01;
constexpr uint8_t kUsrTxRdy = 0x04;
constexpr uint8_t kUsrTxEmt = 0x08;

constexpr uint32_t kTimerTsrTcr = 0x80002020; // word: hi=TSR, lo=TCR
constexpr uint32_t kTimerReload = 0x80002022;
constexpr uint32_t kTimer0 = 0x80002024;
constexpr uint32_t kTimer1 = 0x80002026;
constexpr uint32_t kTimer2 = 0x80002028;
constexpr uint8_t kTsrOv0 = 0x80;
constexpr uint8_t kTsrMa1 = 0x40;
constexpr uint8_t kTsrMa2 = 0x08;
constexpr uint8_t kTsrFlags = 0xfe; // bit 0 reserved
constexpr uint8_t kTcrMatch = 0x1; // T1 mode in bits 5:4, T2 in bits 1:0

constexpr uint32_t kPicr1 = 0x80002045;
constexpr uint32_t kPicr2 = 0x80002047;

// DMA channel 0 at 0x80004000, channel 1 (VMPEG's) one stride on; register
// offsets within a channel.
constexpr uint32_t kDmaBase = 0x80004000;
constexpr uint32_t kDmaChannelStride = 0x40;
constexpr uint32_t kDmaEnd = kDmaBase + 2 * kDmaChannelStride;
constexpr uint32_t kDmaCsr = 0x00; // hi=channel_status, lo=channel_error
constexpr uint32_t kDmaOcr = 0x04; // hi=device_control, lo=operation_control
constexpr uint32_t kDmaCcr = 0x06; // hi=sequence_control, lo=channel_control
constexpr uint32_t kDmaMtc = 0x0a; // transfer_counter (word)
constexpr uint32_t kDmaMac = 0x0c; // memory_address_counter (32-bit, hi word then lo word)
constexpr uint8_t kDmaCsrCoc = 0x80; // channel operation complete
constexpr uint8_t kDmaCsrNdt = 0x20; // normal device termination
constexpr uint8_t kDmaCsrErr = 0x10;
constexpr uint8_t kDmaCcrStart = 0x80; // start operation

} // namespace scc68070

// One DMA channel's registers.
struct DmaChannel {
    uint8_t csr = 0, cer = 0; // channel status / error
    uint8_t dcr = 0, ocr = 0; // device / operation control
    uint8_t scr = 0, ccr = 0; // sequence / channel control
    uint16_t mtc = 0; // memory transfer counter
    uint32_t mac = 0; // memory address counter
};

struct Scc68070SaveState {
    // UART (0x80002011-0x8000201b)
    uint8_t uart_umr_ = 0;
    uint8_t uart_ucsr_ = 0;
    uint8_t uart_ucr_ = 0;
    uint8_t uart_rhr_ = 0;
    bool uart_rx_valid_ = false;

    // Transmit-complete interrupt; period 0 = not armed until a UCSR write.
    bool uart_tx_int_ = false;
    int uart_tx_period_cycles_ = 0;
    int uart_tx_cycle_accum_ = 0;

    // I2C (0x80002001-0x80002009): status only, rest are plain latches
    uint8_t i2c_idr_ = 0, i2c_iar_ = 0, i2c_icr_ = 0, i2c_iccr_ = 0;

    // Interrupt controller
    uint8_t lir_ = 0; // bits6:4 int1 level, bits2:0 int2 level (pin tied low)
    bool int1_asserted_ = false;
    uint8_t picr1_ = 0; // bits6:4 i2c level (no I2C interrupt source), bits2:0 timer level
    uint8_t picr2_ = 0; // bits6:4 uart-rx level, bits2:0 uart-tx level
    bool in2_ = false;
    bool in4_ = false;

    // Timers (0x80002020-0x80002029)
    uint8_t timer_tsr_ = 0;
    uint8_t timer_tcr_ = 0;
    uint16_t timer_reload_ = 0;
    uint16_t timer0_ = 0;
    uint16_t timer1_ = 0;
    uint16_t timer2_ = 0;
    int timer0_cycle_accum_ = 0; // fractional cycles carried between tick calls

    // DMA channel 0 (0x80004000-0x8000400f, CDIC) and 1 (0x80004040-0x8000404f, VMPEG)
    DmaChannel dma_[2] = {};
};

class Scc68070Periph : private Scc68070SaveState {
public:
    Scc68070Periph();
    // For debuggers (MachineInspector).
    const Scc68070SaveState& state() const { return *this; }

    template <class T>
    void state_io(T& archive);

    uint8_t read_byte(uint32_t addr);
    void write_byte(uint32_t addr, uint8_t v);

    // Called after every CPU chunk.
    void tick(int cpu_cycles);

    // Level mirrors: never latched, cleared only by their source.
    void set_int1(bool asserted);

    // Fixed-level external pins: in2 = 2, autovectored (SLAVE); in4 = 4,
    // vectored by CDIC itself (Bus::interrupt_ack).
    void set_in2(bool asserted);
    void set_in4(bool asserted);

    int current_ipl() const;

    // -1 = autovector (M68K_INT_ACK_AUTOVECTOR), else 0x38+level.
    int iack(int level);

    std::string drain_uart_output();

    void uart_receive(uint8_t v);
    bool uart_receiver_enabled() const { return (uart_ucr_ & 3) == 1; }

    // Channel 0 (CDIC), read and advanced by Bus::perform_cdic_dma.
    uint8_t dma_operation_control() const { return dma_[0].ocr; }
    uint16_t dma_transfer_counter() const { return dma_[0].mtc; }
    uint32_t dma_memory_address_counter() const { return dma_[0].mac; }
    void advance_dma_memory_address_counter(uint32_t delta) { dma_[0].mac += delta; }
    // Channel 1 (VMPEG), used by Bus::perform_vmpeg_dma.
    uint8_t dma_ch1_operation_control() const { return dma_[1].ocr; }
    uint16_t dma_ch1_transfer_counter() const { return dma_[1].mtc; }
    uint32_t dma_ch1_memory_address_counter() const { return dma_[1].mac; }
    void advance_dma_ch1_memory_address_counter(uint32_t delta) { dma_[1].mac += delta; }

    // MTC reaching 0 sets COC, alongside the CCR_SO -> COC strobe.
    void complete_dma_ch1_burst(uint32_t words_transferred);

    bool in4_asserted() const { return in4_; }

private:
    uint8_t read_dma(uint32_t addr) const;
    void write_dma(uint32_t addr, uint8_t v);

    std::string uart_output_;
};

} // namespace mamecdi
} // namespace zcdi
