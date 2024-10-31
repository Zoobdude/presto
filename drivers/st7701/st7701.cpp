#include "st7701.hpp"

#include <cstdlib>
#include <math.h>
#include <pico/sync.h>
#include "hardware/structs/xip_ctrl.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/platform_defs.h"

#ifndef NO_QSTR
#include "st7701_parallel.pio.h"
#include "st7701_timing.pio.h"
#endif

namespace pimoroni {
  enum dcx {
    CMD = 0x00,
    DATA = 0x01
  };

  enum reg {
    SWRESET = 0x01, // Software Reset
    SLPOUT = 0x11,  // Sleep Out
    PTLON = 0x12,   // Partial Display Mode On
    NORON = 0x13,   // Normal Display Mode On
    INVOFF = 0x20,  // Display Inversion Off
    INVON = 0x21,   // Display Inversion On
    ALLPOFF = 0x22, // All Pixels Off
    ALLPON = 0x23,  // All Pixels On
    GAMSET = 0x26,  // Gamma Set
    DISPOFF = 0x28, // Display Off
    DISPON = 0x29,  // Display On
    TEOFF = 0x34,   // Tearing Effect Line Off (kinda vsync)
    TEON = 0x35,    // Tearing Effect Line On (kinda vsync)
    MADCTL = 0x36,  // Display data access control
    IDMOFF = 0x38,  // Idle Mode Off
    IDMON = 0x39,   // Idle Mode On
    COLMOD = 0x3A,  // Interface Pixel Format
    GSL = 0x45,     // Get Scan Line
    // Command2_BK0
    PVGAMCTRL = 0xB0,  // Positive Voltage Gamma Control
    NVGAMCTRL = 0xB1,  // Negative Voltage Gamma Control
    DGMEN = 0xB8,   // Digital Gamma Enable
    DGMLUTR = 0xB9, // Digital Gamma LUT for Red
    DGMLUTB = 0xBA, // Digital Gamma Lut for Blue
    LNESET = 0xC0,  // Display Line Setting
    PORCTRL = 0xC1, // Porch Control
    INVSET = 0xC2,  // Inversion Selection & Frame Rate Control
    RGBCTRL = 0xC3, // RGB Control
    PARCTRL = 0xC5, // Partial Mode Control
    SDIR = 0xC7,    // X-direction Control
    PDOSET = 0xC8,  // Pseudo-Dot Inversion Diving Settign
    COLCTRL = 0xCD, // Colour Control
    SRECTRL = 0xE0, // Sunlight Readable Enhancement
    NRCTRL = 0xE1,  // Noise Reduce Control
    SECTRL = 0xE2,  // Sharpness Control
    CCCTRL = 0xE3,  // Color Calibration Control
    SKCTRL = 0xE4,  // Skin Tone Preservation Control
    // Command2_BK1
    VHRS = 0xB0,    // Vop amplitude
    VCOMS = 0xB1,   // VCOM amplitude
    VGHSS = 0xB2,   // VGH voltage
    TESTCMD = 0xB3, // TEST command
    VGLS = 0xB5,    // VGL voltage
    VRHDV = 0xB6,   // VRH_DV voltage
    PWCTRL1 = 0xB7, // Power Control 1
    PWCTRL2 = 0xB8, // Power Control 2
    PCLKS1 = 0xBA,  // Power pumping clock selection 1
    PCLKS2 = 0xBC,  // Power pumping clock selection 2
    PDR1 = 0xC1,    // Source pre_drive timing set 1
    PDR2 = 0xC2,    // Source pre_drive timing set 2
    // Command2_BK3
    NVMEN = 0xC8,    // NVM enable
    NVMSET = 0xCA,   // NVM manual control
    PROMACT = 0xCC,  // NVM program active
    // Other
    CND2BKxSEL = 0xFF,
  };

#define DISPLAY_HEIGHT   480
#define TIMING_V_PULSE   8
#define TIMING_V_BACK    (5 + TIMING_V_PULSE)
#define TIMING_V_DISPLAY (DISPLAY_HEIGHT + TIMING_V_BACK)
#define TIMING_V_FRONT   (5 + TIMING_V_DISPLAY)
#define TIMING_H_FRONT   4
#define TIMING_H_PULSE   25
#define TIMING_H_BACK    30
#define TIMING_H_DISPLAY 480

static ST7701* st7701_inst;

// This ISR is triggered whenever the timing SM's FIFO is not full
void __no_inline_not_in_flash_func(timing_isr)() {
    st7701_inst->drive_timing();
}

void __no_inline_not_in_flash_func(ST7701::drive_timing)()
{
    while (!pio_sm_is_tx_fifo_full(st_pio, timing_sm)) {
        uint32_t instr;
        switch (timing_phase) {
            case 0:
                // Front Porch
                instr = 0x4000B042u;  // HSYNC high, NOP
                if (timing_row >= TIMING_V_PULSE) instr |= 0x80000000u;  // VSYNC high if not in VSYNC pulse
                instr |= (TIMING_H_FRONT - 3) << 16;
                pio_sm_put(st_pio, timing_sm, instr);
                break;

            case 1:
                // HSYNC
                instr = 0x0000B042u;  // HSYNC low, NOP
                if (timing_row >= TIMING_V_PULSE) instr |= 0x80000000u;  // VSYNC high if not in VSYNC pulse
                instr |= (TIMING_H_PULSE - 3) << 16;
                pio_sm_put(st_pio, timing_sm, instr);
                break;

            case 2:
                // Back Porch, trigger pixel channels if in display window
                instr = 0x40000000u;  // HSYNC high
                if (timing_row >= TIMING_V_PULSE) instr |= 0x80000000u;  // VSYNC high if not in VSYNC pulse
                if (timing_row >= TIMING_V_BACK && timing_row < TIMING_V_DISPLAY) instr |= 0xD004u;  // IRQ 4, triggers the data SM
                else instr |= 0xB042u;  // NOP
                instr |= (TIMING_H_BACK - 3) << 16;
                pio_sm_put(st_pio, timing_sm, instr);
                //printf(".\n");
                break;

            case 3:
                // Display, trigger next frame at frame end
                instr = 0x40000000u;  // HSYNC high
                if (timing_row == TIMING_V_DISPLAY) instr |= 0xD001u;  // irq 1, to trigger queueing DMA for a new frame 
                else if (timing_row >= TIMING_V_BACK - 1 && timing_row < TIMING_V_DISPLAY) instr |= 0xD000u;  // irq 0, to trigger queueing DMA for a new line 
                else instr |= 0xB042u;  // NOP
                if (timing_row >= TIMING_V_PULSE) instr |= 0x80000000u;  // VSYNC high if not in VSYNC pulse
                instr |= (TIMING_H_DISPLAY - 3) << 16;
                pio_sm_put(st_pio, timing_sm, instr);

                if (++timing_row >= TIMING_V_FRONT) timing_row = 0;
                break;
        }

        timing_phase = (timing_phase + 1) & 3;
    }
}

// This ISR is triggered at the end of each line transferred
void __no_inline_not_in_flash_func(end_of_line_isr()) {
    st7701_inst->handle_end_of_line();
}

void __no_inline_not_in_flash_func(ST7701::handle_end_of_line())
{
    if (st_pio->irq & 0x2) start_frame_xfer();
    else start_line_xfer();
}

void __no_inline_not_in_flash_func(ST7701::start_line_xfer())
{
    hw_clear_bits(&st_pio->irq, 0x1);

    ++display_row;
    if (display_row == DISPLAY_HEIGHT) next_line_addr = 0;
    else next_line_addr = &framebuffer[width * (display_row >> row_shift)];
}

void ST7701::start_frame_xfer()
{
    hw_clear_bits(&st_pio->irq, 0x2);

    if (next_framebuffer) {
        framebuffer = next_framebuffer;
        next_framebuffer = nullptr;

        init_framebuffer();
    }

    next_line_addr = 0;
    dma_channel_abort(st_dma);
    dma_channel_wait_for_finish_blocking(st_dma);
    pio_sm_set_enabled(st_pio, parallel_sm, false);
    pio_sm_clear_fifos(st_pio, parallel_sm);
    pio_sm_exec_wait_blocking(st_pio, parallel_sm, pio_encode_mov(pio_osr, pio_null));
    pio_sm_exec_wait_blocking(st_pio, parallel_sm, pio_encode_out(pio_null, 32));
    pio_sm_exec_wait_blocking(st_pio, parallel_sm, pio_encode_jmp(parallel_offset));
    pio_sm_set_enabled(st_pio, parallel_sm, true);
    display_row = 0;
    next_line_addr = framebuffer;
    dma_channel_set_read_addr(st_dma, framebuffer, true);  

    waiting_for_vsync = false;
    __sev();
}

void ST7701::init_framebuffer() {
    // If framebuffer not in cache XIP memory, do nothing.
    if ((intptr_t)framebuffer >= 0x14000000) return;

    // Flush the frame buffer from cache
    uint8_t* framebuffer_maintenance = (uint8_t*)framebuffer + 0x8000000;
    for (int i = 3; i < width*height << 1; i += 8)
        framebuffer_maintenance[i] = 0; // Clean

    // Switch to uncached reads
    framebuffer += 0x2000000;
}

  ST7701::ST7701(uint16_t width, uint16_t height, Rotation rotation, SPIPins control_pins, uint16_t* framebuffer,
      uint d0, uint hsync, uint vsync, uint lcd_de, uint lcd_dot_clk) :
            DisplayDriver(width, height, rotation),
            spi(control_pins.spi),
            spi_cs(control_pins.cs), spi_sck(control_pins.sck), spi_dat(control_pins.mosi), lcd_bl(control_pins.bl),
            d0(d0), hsync(hsync), vsync(vsync), lcd_de(lcd_de), lcd_dot_clk(lcd_dot_clk),
            framebuffer(framebuffer)
  {
      st7701_inst = this;
  }

  void ST7701::init() {
      irq_handler_t current = nullptr;
  
      init_framebuffer();

      st_pio = pio2;
      parallel_sm = pio_claim_unused_sm(st_pio, true);

      if      (width == 480) parallel_offset = pio_add_program(st_pio, &st7701_parallel_program);
      else if (width == 240) parallel_offset = pio_add_program(st_pio, &st7701_parallel_double_program);
      else {
        printf("Unsupported width\n");
        return;
      }

      if (height == 240) row_shift = 1;

      timing_sm = pio_claim_unused_sm(st_pio, true);
      timing_offset = pio_add_program(st_pio, &st7701_timing_program);

      spi_init(spi, SPI_BAUD);
      gpio_set_function(spi_cs, GPIO_FUNC_SIO);
      gpio_set_dir(spi_cs, GPIO_OUT);
      gpio_set_function(spi_dat, GPIO_FUNC_SPI);
      gpio_set_function(spi_sck, GPIO_FUNC_SPI);

      // ST7701 3-line Serial Interface
      // 9th bit = D/CX
      // low = command
      // high = data
      spi_set_format(spi, 9, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
  
      //gpio_init(wr_sck);
      //gpio_set_dir(wr_sck, GPIO_OUT);
      //gpio_set_function(wr_sck, GPIO_FUNC_SIO);
      pio_gpio_init(st_pio, hsync);
      pio_gpio_init(st_pio, vsync);
      pio_gpio_init(st_pio, lcd_de);
      pio_gpio_init(st_pio, lcd_dot_clk);

      for(auto i = 0u; i < 16; i++) {
        pio_gpio_init(st_pio, d0 + i);
      }
      for(auto i = 16u; i < 18; i++) {
        gpio_init(d0 + i);
        gpio_set_dir(d0 + i, GPIO_OUT);
        gpio_put(d0 + i, false);
      }

      pio_sm_set_consecutive_pindirs(st_pio, parallel_sm, d0, 16, true);
      pio_sm_set_consecutive_pindirs(st_pio, parallel_sm, hsync, 4, true);

      pio_sm_config c;
      if (width == 480) c = st7701_parallel_program_get_default_config(parallel_offset);
      else              c = st7701_parallel_double_program_get_default_config(parallel_offset);

      sm_config_set_out_pins(&c, d0, 16);
      sm_config_set_sideset_pins(&c, lcd_de);
      sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
      sm_config_set_out_shift(&c, true, true, 32);
      sm_config_set_in_shift(&c, false, false, 32);
      
      // Determine clock divider
      uint32_t max_pio_clk;
      if ((intptr_t)framebuffer >= 0x14000000 || width <= 240) {
        // Frame buffer in internal RAM or pixel doubling, go as fast as possible
        max_pio_clk = 34 * MHZ;
      }
      else {
        // Frame buffer in PSRAM and not pixel doubling, slow down a bit
        max_pio_clk = 25 * MHZ;
      }
      const uint32_t sys_clk_hz = clock_get_hz(clk_sys);
      const uint32_t clk_div = (sys_clk_hz + max_pio_clk - 1) / max_pio_clk;
      sm_config_set_clkdiv(&c, clk_div);
      
      pio_sm_init(st_pio, parallel_sm, parallel_offset, &c);
      pio_sm_exec(st_pio, parallel_sm, pio_encode_out(pio_y, 32));
      pio_sm_put(st_pio, parallel_sm, (width >> 1) - 1);
      pio_sm_set_enabled(st_pio, parallel_sm, true);

      c = st7701_timing_program_get_default_config(timing_offset);

      sm_config_set_out_pins(&c, hsync, 2);
      sm_config_set_sideset_pins(&c, lcd_dot_clk);
      sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
      sm_config_set_out_shift(&c, false, true, 32);
      sm_config_set_clkdiv(&c, clk_div);
      
      pio_sm_init(st_pio, timing_sm, timing_offset, &c);
      pio_sm_set_enabled(st_pio, timing_sm, true);

      st_dma = dma_claim_unused_channel(true);
      st_dma2 = dma_claim_unused_channel(true);

      dma_channel_config config = dma_channel_get_default_config(st_dma);
      channel_config_set_transfer_data_size(&config, DMA_SIZE_32);
      channel_config_set_high_priority(&config, true);
      channel_config_set_dreq(&config, pio_get_dreq(st_pio, parallel_sm, true));
      channel_config_set_bswap(&config, true);
      channel_config_set_chain_to(&config, st_dma2);
      dma_channel_configure(st_dma, &config, &st_pio->txf[parallel_sm], nullptr, width >> 1, false);

      config = dma_channel_get_default_config(st_dma2);
      channel_config_set_transfer_data_size(&config, DMA_SIZE_32);
      channel_config_set_read_increment(&config, false);
      dma_channel_configure(st_dma2, &config, &dma_hw->ch[st_dma].al3_read_addr_trig, &next_line_addr, 1, false);

      hw_set_bits(&bus_ctrl_hw->priority, (BUSCTRL_BUS_PRIORITY_PROC1_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS | BUSCTRL_BUS_PRIORITY_DMA_W_BITS));

      printf("Begin SPI setup\n");

      common_init();

      printf("Setup screen timing\n");

      // Setup timing
      hw_set_bits(&st_pio->inte1, 0x010 << timing_sm);  // TX not full
      // Remove the MicroPython handler if it's set
      current = irq_get_exclusive_handler(pio_get_irq_num(st_pio, 1));
      if(current) irq_remove_handler(pio_get_irq_num(st_pio, 1), current);
      irq_set_exclusive_handler(pio_get_irq_num(st_pio, 1), timing_isr);
      irq_set_enabled(pio_get_irq_num(st_pio, 1), true);

      hw_set_bits(&st_pio->inte0, 0x300); // IRQ 0
      // Remove the MicroPython handler if it's set
      current = irq_get_exclusive_handler(pio_get_irq_num(st_pio, 0));
      if(current) irq_remove_handler(pio_get_irq_num(st_pio, 0), current);
      irq_set_exclusive_handler(pio_get_irq_num(st_pio, 0), end_of_line_isr);
      irq_set_enabled(pio_get_irq_num(st_pio, 0), true);
    }

  void ST7701::common_init() {
    // if a backlight pin is provided then set it up for
    // pwm control
    if(lcd_bl != PIN_UNUSED) {
      #if 0
      pwm_config cfg = pwm_get_default_config();
      pwm_set_wrap(pwm_gpio_to_slice_num(lcd_bl), 65535);
      pwm_init(pwm_gpio_to_slice_num(lcd_bl), &cfg, true);
      gpio_set_function(lcd_bl, GPIO_FUNC_PWM);
      set_backlight(0); // Turn backlight off initially to avoid nasty surprises
      #else
      gpio_init(lcd_bl);
      gpio_set_dir(lcd_bl, GPIO_OUT);
      gpio_put(lcd_bl, 0);
      #endif
    }

    command(reg::SWRESET);

    sleep_ms(150);

    // Commmand 2 BK0 - kinda a page select
    command(reg::CND2BKxSEL, 5, "\x77\x01\x00\x00\x10");

    /*if(width == 480 && height == 480)*/ {
      // TODO: Figure out what's actually display specific
      command(reg::MADCTL, 1, "\x00");  // Normal scan direction and RGB pixels
      command(reg::LNESET, 2, "\x3b\x00");   // (59 + 1) * 8 = 480 lines
      command(reg::PORCTRL, 2, "\x0d\x05");  // 13 VBP, 5 VFP
      command(reg::INVSET, 2, "\x32\x05");
      command(reg::COLCTRL, 1, "\x08");      // LED polarity reversed
      command(reg::PVGAMCTRL, 16, "\x00\x11\x18\x0e\x11\x06\x07\x08\x07\x22\x04\x12\x0f\xaa\x31\x18");
      command(reg::NVGAMCTRL, 16, "\x00\x11\x19\x0e\x12\x07\x08\x08\x08\x22\x04\x11\x11\xa9\x32\x18");
    }

    // Command 2 BK1 - Voltages and power and stuff
    command(reg::CND2BKxSEL, 5, "\x77\x01\x00\x00\x11");
    command(reg::VHRS, 1, "\x60");    // 4.7375v
    command(reg::VCOMS, 1, "\x32");   // 0.725v
    command(reg::VGHSS, 1, "\x07");   // 15v
    command(reg::TESTCMD, 1, "\x80"); // y tho?
    command(reg::VGLS, 1, "\x49");    // -10.17v
    command(reg::PWCTRL1, 1, "\x85"); // Middle/Min/Min bias
    command(reg::PWCTRL2, 1, "\x21"); // 6.6 / -4.6
    command(reg::PDR1, 1, "\x78");    // 1.6uS
    command(reg::PDR2, 1, "\x78");    // 6.4uS

    // Begin Forbidden Knowledge
    // This sequence is probably specific to TL040WVS03CT15-H1263A.
    // It is not documented in the ST7701s datasheet.
    // TODO: 👇 W H A T ! ? 👇
    command(0xE0, 3, "\x00\x1b\x02");
    command(0xE1, 11, "\x08\xa0\x00\x00\x07\xa0\x00\x00\x00\x44\x44");
    command(0xE2, 12, "\x11\x11\x44\x44\xed\xa0\x00\x00\xec\xa0\x00\x00");
    command(0xE3, 4, "\x00\x00\x11\x11");
    command(0xE4, 2, "\x44\x44");
    command(0xE5, 16, "\x0a\xe9\xd8\xa0\x0c\xeb\xd8\xa0\x0e\xed\xd8\xa0\x10\xef\xd8\xa0");
    command(0xE6, 4, "\x00\x00\x11\x11");
    command(0xE7, 2, "\x44\x44");
    command(0xE8, 16, "\x09\xe8\xd8\xa0\x0b\xea\xd8\xa0\x0d\xec\xd8\xa0\x0f\xee\xd8\xa0");
    command(0xEB, 7, "\x02\x00\xe4\xe4\x88\x00\x40");
    command(0xEC, 2, "\x3c\x00");
    command(0xED, 16, "\xab\x89\x76\x54\x02\xff\xff\xff\xff\xff\xff\x20\x45\x67\x98\xba");
    command(0x36, 1, "\x00");
    // End Forbidden Knowledge

    // Command 2 BK3
    command(reg::CND2BKxSEL, 5, "\x77\x01\x00\x00\x13");
    //command(reg::COLMOD, 1, "\x77");  // 24 bits per pixel...
    command(reg::COLMOD, 1, "\x66");    // 18 bits per pixel...
    //command(reg::COLMOD, 1, "\x55");  // 16 bits per pixel...
    
    command(reg::INVON);
    sleep_ms(1);
    command(reg::SLPOUT);
    sleep_ms(120);
    command(reg::DISPON);
    sleep_ms(50);

    // TODO: Support rotation
    // configure_display(rotation);

    if(lcd_bl != PIN_UNUSED) {
      //update(); // Send the new buffer to the display to clear any previous content
      sleep_ms(50); // Wait for the update to apply
      #if 0
      set_backlight(255); // Turn backlight on now surprises have passed
      #else
      gpio_put(lcd_bl, 1);
      #endif
    }
  }

  void ST7701::cleanup() {
    irq_handler_t current;

    irq_set_enabled(pio_get_irq_num(st_pio, 0), false);
    current = irq_get_exclusive_handler(pio_get_irq_num(st_pio, 0));
    if(current) irq_remove_handler(pio_get_irq_num(st_pio, 0), current);
  
    irq_set_enabled(pio_get_irq_num(st_pio, 1), false);
    current = irq_get_exclusive_handler(pio_get_irq_num(st_pio, 1));
    if(current) irq_remove_handler(pio_get_irq_num(st_pio, 1), current);

    next_line_addr = 0;
    if(dma_channel_is_claimed(st_dma)) {
      while (dma_channel_is_busy(st_dma))
        ;
      dma_channel_abort(st_dma);
      dma_channel_unclaim(st_dma);
    }
    if(dma_channel_is_claimed(st_dma2)) {
      dma_channel_unclaim(st_dma2);
    }

    if(pio_sm_is_claimed(st_pio, parallel_sm)) {
      pio_sm_set_enabled(st_pio, parallel_sm, false);
      pio_sm_clear_fifos(st_pio, parallel_sm);
      pio_sm_unclaim(st_pio, parallel_sm);
    }

    if(pio_sm_is_claimed(st_pio, timing_sm)) {
      pio_sm_set_enabled(st_pio, timing_sm, false);
      pio_sm_clear_fifos(st_pio, timing_sm);
      pio_sm_unclaim(st_pio, timing_sm);
    }

    pio_clear_instruction_memory(st_pio);
  }

  void ST7701::configure_display(Rotation rotate) {
    uint8_t madctl = 0;

    if(rotate == ROTATE_90 || rotate == ROTATE_270) {
      std::swap(width, height);
    }

    // 480x480 Square Display
    /*if(width == 480 && height == 480)*/ {
      madctl = 0;
    }

    command(reg::MADCTL, 1, (char *)&madctl);
  }

  void ST7701::write_blocking_dma(const uint8_t *src, size_t len) {
    while (dma_channel_is_busy(st_dma))
      ;
    dma_channel_set_trans_count(st_dma, len, false);
    dma_channel_set_read_addr(st_dma, src, true);
  }

  void ST7701::write_blocking_parallel(const uint8_t *src, size_t len) {
    write_blocking_dma(src, len);
    dma_channel_wait_for_finish_blocking(st_dma);

    // This may cause a race between PIO and the
    // subsequent chipselect deassert for the last pixel
    while(!pio_sm_is_tx_fifo_empty(st_pio, parallel_sm))
      ;
  }

  void ST7701::command(uint8_t command, size_t len, const char *data) {
    static uint16_t _data[20] = {0};
    gpio_put(spi_cs, 0);
    
    // Add leading byte for 9th D/CX bit
    uint16_t _command = (dcx::CMD << 8) | command;
    spi_write16_blocking(spi, &_command, 1);

    if(data) {
      // Add leading bytes for 9th D/CX bits
      // TODO: OOOF - I *think* this is how 9bit SPI is supposed to work!?
      // We'd probably be better off with some dedicated PIO tomfoolery
      for(auto i = 0u; i < len; i++) {
        _data[i] = (dcx::DATA << 8) | data[i];
      }
      spi_write16_blocking(spi, _data, len);
    }

    gpio_put(spi_cs, 1);
  }
  
  void ST7701::update(PicoGraphics *graphics) {
    //uint8_t cmd = reg::RAMWR;

    // TODO: Where's my buffer at? Where's my buffer at?
    // I left it out back in the PSRAM chip, y'all
    // And now I cannot find it even though it's 60k tall

    if(graphics->pen_type == PicoGraphics::PEN_RGB565) { // Display buffer is screen native
      // command(cmd, width * height * sizeof(uint16_t), (const char*)graphics->frame_buffer);
    } else {
      /*graphics->frame_convert(PicoGraphics::PEN_RGB565, [this](void *data, size_t length) {
        if (length > 0) {
          write_blocking_dma((const uint8_t*)data, length);
        }
        else {
          dma_channel_wait_for_finish_blocking(st_dma);
        }
      });*/
    }
  }

  void ST7701::set_backlight(uint8_t brightness) {
    // gamma correct the provided 0-255 brightness value onto a
    // 0-65535 range for the pwm counter
    float gamma = 2.8;
    uint16_t value = (uint16_t)(pow((float)(brightness) / 255.0f, gamma) * 65535.0f + 0.5f);
    pwm_set_gpio_level(lcd_bl, value);
  }

  void ST7701::wait_for_vsync() {
    waiting_for_vsync = true;
    while (waiting_for_vsync) __wfe();
  }
}
