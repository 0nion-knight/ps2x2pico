/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2022 No0ne (https://github.com/No0ne)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "ps2device.pio.h"
#include "hardware/gpio.h"
#include "bsp/board.h"
#include "tusb.h"

#define KBCLK 11
#define KBDAT 12
#define LVPWR 13
#define MSCLK 14
#define MSDAT 15

PIO iokbd = pio0;
PIO ioms = pio1;
uint txkbd;
uint txms;
uint jmpkbd;
uint jmpms;

uint8_t const led2ps2[] = { 0, 4, 1, 5, 2, 6, 3, 7 };
uint8_t const mod2ps2[] = { 0x14, 0x12, 0x11, 0x1f, 0x14, 0x59, 0x11, 0x27 };
uint8_t const hid2ps2[] = {
  0x00, 0x00, 0xfc, 0x00, 0x1c, 0x32, 0x21, 0x23, 0x24, 0x2b, 0x34, 0x33, 0x43, 0x3b, 0x42, 0x4b,
  0x3a, 0x31, 0x44, 0x4d, 0x15, 0x2d, 0x1b, 0x2c, 0x3c, 0x2a, 0x1d, 0x22, 0x35, 0x1a, 0x16, 0x1e,
  0x26, 0x25, 0x2e, 0x36, 0x3d, 0x3e, 0x46, 0x45, 0x5a, 0x76, 0x66, 0x0d, 0x29, 0x4e, 0x55, 0x54,
  0x5b, 0x5d, 0x5d, 0x4c, 0x52, 0x0e, 0x41, 0x49, 0x4a, 0x58, 0x05, 0x06, 0x04, 0x0c, 0x03, 0x0b,
  0x83, 0x0a, 0x01, 0x09, 0x78, 0x07, 0x7c, 0x7e, 0x7e, 0x70, 0x6c, 0x7d, 0x71, 0x69, 0x7a, 0x74,
  0x6b, 0x72, 0x75, 0x77, 0x4a, 0x7c, 0x7b, 0x79, 0x5a, 0x69, 0x72, 0x7a, 0x6b, 0x73, 0x74, 0x6c,
  0x75, 0x7d, 0x70, 0x71, 0x61, 0x2f, 0x37, 0x0f, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40,
  0x48, 0x50, 0x57, 0x5f
};
uint8_t const maparray = sizeof(hid2ps2) / sizeof(uint8_t);

bool kbd_enabled = true;
uint8_t kbd_addr = 0;
uint8_t kbd_inst = 0;

bool blinking = false;
bool repeating = false;
uint32_t repeat_us = 35000;
uint16_t delay_ms = 250;
alarm_id_t repeater;

uint8_t prev_rpt[] = { 0, 0, 0, 0, 0, 0, 0, 0 };
uint8_t prev_kbd = 0;
uint8_t resend_kbd = 0;
uint8_t resend_ms = 0;
uint8_t repeat = 0;
uint8_t leds = 0;

#define MS_TYPE_STANDARD  0x00
#define MS_TYPE_WHEEL_3   0x03
#define MS_TYPE_WHEEL_5   0x04

#define MS_MODE_IDLE      0
#define MS_MODE_STREAMING 1

#define MS_INPUT_CMD      0
#define MS_INPUT_SET_RATE 1

uint8_t ms_type = MS_TYPE_STANDARD;
uint8_t ms_mode = MS_MODE_IDLE;
uint8_t ms_input_mode = MS_INPUT_CMD;
uint8_t ms_rate = 100;
uint32_t ms_magic_seq = 0x00;

int64_t repeat_callback(alarm_id_t id, void *user_data) {
  if(repeat) {
    repeating = true;
    return repeat_us;
  }
  
  repeater = 0;
  return 0;
}

void ps2_send(uint8_t data, bool channel) {
  if(channel) {
    resend_kbd = data;
  } else {
    resend_ms = data;
  }
  
  uint8_t parity = 1;
  for(uint8_t i = 0; i < 8; i++) {
    parity = parity ^ (data >> i & 1);
  }
  
  pio_sm_put(channel ? iokbd : ioms, channel ? txkbd : txms, ((1 << 10) | (parity << 9) | (data << 1)) ^ 0x7ff);
}

void ms_send(uint8_t data) {
  printf("send MS %02x\n", data);
  ps2_send(data, false);
}

void kbd_send(uint8_t data) {
  printf("send KB %02x\n", data);
  ps2_send(data, true);
}

void maybe_send_e0(uint8_t data) {
  if(data == 0x46 ||
     data >= 0x48 && data <= 0x52 ||
     data == 0x54 || data == 0x58 ||
     data == 0x65 || data == 0x66 ||
     data >= 0x81) {
    ps2_send(0xe0, true);
  }
}

void kbd_set_leds(uint8_t data) {
  if(data > 7) data = 0;
  leds = led2ps2[data];
  tuh_hid_set_report(kbd_addr, kbd_inst, 0, HID_REPORT_TYPE_OUTPUT, &leds, sizeof(leds));
}

int64_t blink_callback(alarm_id_t id, void *user_data) {
  if(kbd_addr) {
    if(blinking) {
      kbd_set_leds(7);
      blinking = false;
      return 500000;
    } else {
      kbd_set_leds(0);
    }
  }
  return 0;
}

void process_kbd(uint8_t data) {
  switch(prev_kbd) {
    case 0xed: // CMD: Set LEDs
      prev_kbd = 0;
      kbd_set_leds(data);
    break;
    
    case 0xf3: // CMD: Set typematic rate and delay
      prev_kbd = 0;
      repeat_us = data & 0x1f;
      delay_ms = data & 0x60;
      
      repeat_us = 35000 + repeat_us * 15000;
      
      if(delay_ms == 0x00) delay_ms = 250;
      if(delay_ms == 0x20) delay_ms = 500;
      if(delay_ms == 0x40) delay_ms = 750;
      if(delay_ms == 0x60) delay_ms = 1000;
    break;
    
    default:
      switch(data) {
        case 0xff: // CMD: Reset
          pio_sm_drain_tx_fifo(iokbd, txkbd);
          pio_sm_clear_fifos(iokbd, txkbd);
          kbd_send(0xfa);
          
          kbd_enabled = true;
          blinking = true;
          add_alarm_in_ms(1, blink_callback, NULL, false);
          
          sleep_ms(3);
          kbd_send(0xaa);
          
          return;
        break;
        
        case 0xfe: // CMD: Resend
          kbd_send(resend_kbd);
          return;
        break;
        
        case 0xee: // CMD: Echo
          kbd_send(0xee);
          return;
        break;
        
        case 0xf2: // CMD: Identify keyboard
          kbd_send(0xfa);
          kbd_send(0xab);
          kbd_send(0x83);
          return;
        break;
        
        case 0xf3: // CMD: Set typematic rate and delay
        case 0xed: // CMD: Set LEDs
          prev_kbd = data;
        break;
        
        case 0xf4: // CMD: Enable scanning
          kbd_enabled = true;
        break;
        
        case 0xf5: // CMD: Disable scanning, restore default parameters
        case 0xf6: // CMD: Set default parameters
          kbd_enabled = data == 0xf6;
          repeat_us = 35000;
          delay_ms = 250;
          kbd_set_leds(0);
        break;
      }
    break;
  }
  
  kbd_send(0xfa);
}

void process_ms(uint8_t data) {

  if(ms_input_mode == MS_INPUT_SET_RATE) {
    ms_rate = data;  // TODO... need to actually honor the sample rate!
    ms_input_mode = MS_INPUT_CMD;
    ms_send(0xfa);

    ms_magic_seq = (ms_magic_seq << 8) | data;
    if(ms_type == MS_TYPE_STANDARD && ms_magic_seq == 0xc86450) {
      ms_type = MS_TYPE_WHEEL_3;
    } else if (ms_type == MS_TYPE_WHEEL_3 && ms_magic_seq == 0xc8c850) {
      ms_type = MS_TYPE_WHEEL_5;
    }
    printf("  MS magic seq: %06x type: %d\n", ms_magic_seq, ms_type);
    return;
  }

  if(data != 0xf3) {
    ms_magic_seq = 0x00;
  }

  switch(data) {
    case 0xff: // CMD: Reset
      pio_sm_drain_tx_fifo(ioms, txms);
      pio_sm_clear_fifos(ioms, txms);
      ms_send(0xfa);
      
      ms_type = MS_TYPE_STANDARD;
      ms_mode = MS_MODE_IDLE;
      ms_rate = 100;
      
      sleep_ms(3);
      ms_send(0xaa);
      ms_send(ms_type);
    return;

    case 0xf6: // CMD: Set Defaults
      ms_type = MS_TYPE_STANDARD;
      ms_rate = 100;
    case 0xf5: // CMD: Disable Data Reporting
    case 0xea: // CMD: Set Stream Mode
      ms_mode = MS_MODE_IDLE;
      ms_send(0xfa);
    return;

    case 0xf4: // CMD: Enable Data Reporting
      ms_mode = MS_MODE_STREAMING;
      ms_send(0xfa);
    return;

    case 0xf3: // CMD: Set Sample Rate
      ms_input_mode = MS_INPUT_SET_RATE;
      ms_send(0xfa);
    return;

    case 0xf2: // CMD: Get Device ID
      ms_send(0xfa);
      ms_send(ms_type);
    return;

    case 0xe9: // CMD: Status Request
      ms_send(0xfa);
      ms_send(0x00); // Bit6: Mode, Bit 5: Enable, Bit 4: Scaling, Bits[2,1,0] = Buttons[L,M,R]
      ms_send(0x02); // Resolution
      ms_send(ms_rate); // Sample Rate
    return;

// TODO: Implement (more of) these?
//    case 0xfe: // CMD: Resend
//    case 0xf0: // CMD: Set Remote Mode
//    case 0xee: // CMD: Set Wrap Mode
//    case 0xec: // CMD: Reset Wrap Mode
//    case 0xeb: // CMD: Read Data
//    case 0xe8: // CMD: Set Resolution
//    case 0xe7: // CMD: Set Scaling 2:1
//    case 0xe6: // CMD: Set Scaling 1:1
  }

  ms_send(0xfa);
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len) {
  printf("HID device address = %d, instance = %d is mounted\n", dev_addr, instance);

  switch(tuh_hid_interface_protocol(dev_addr, instance)) {
    case HID_ITF_PROTOCOL_KEYBOARD:
      printf("HID Interface Protocol = Keyboard\n");
      
      kbd_addr = dev_addr;
      kbd_inst = instance;
      
      blinking = true;
      add_alarm_in_ms(1, blink_callback, NULL, false);
      
      tuh_hid_receive_report(dev_addr, instance);
    break;
    
    case HID_ITF_PROTOCOL_MOUSE:
      printf("HID Interface Protocol = Mouse\n");
      //tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_REPORT);
      tuh_hid_receive_report(dev_addr, instance);
    break;
  }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
  printf("HID device address = %d, instance = %d is unmounted\r\n", dev_addr, instance);
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len) {
  
  switch(tuh_hid_interface_protocol(dev_addr, instance)) {
    case HID_ITF_PROTOCOL_KEYBOARD:
      
      if(!kbd_enabled || report[1] != 0) {
        tuh_hid_receive_report(dev_addr, instance);
        return;
      }
      
      board_led_write(1);
      
      if(report[0] != prev_rpt[0]) {
        uint8_t rbits = report[0];
        uint8_t pbits = prev_rpt[0];
        
        for(uint8_t j = 0; j < 8; j++) {
          
          if((rbits & 0x01) != (pbits & 0x01)) {
            if(j > 2 && j != 5) kbd_send(0xe0);
            
            if(rbits & 0x01) {
              kbd_send(mod2ps2[j]);
            } else {
              kbd_send(0xf0);
              kbd_send(mod2ps2[j]);
            }
          }
          
          rbits = rbits >> 1;
          pbits = pbits >> 1;
          
        }
        
        prev_rpt[0] = report[0];
      }
      
      for(uint8_t i = 2; i < 8; i++) {
        if(prev_rpt[i]) {
          bool brk = true;
          
          for(uint8_t j = 2; j < 8; j++) {
            if(prev_rpt[i] == report[j]) {
              brk = false;
              break;
            }
          }
          
          if(brk && report[i] < maparray) {
            if(prev_rpt[i] == 0x48) continue;
            if(prev_rpt[i] == repeat) repeat = 0;
            
            maybe_send_e0(prev_rpt[i]);
            kbd_send(0xf0);
            kbd_send(hid2ps2[prev_rpt[i]]);
          }
        }
        
        if(report[i]) {
          bool make = true;
          
          for(uint8_t j = 2; j < 8; j++) {
            if(report[i] == prev_rpt[j]) {
              make = false;
              break;
            }
          }
          
          if(make && report[i] < maparray) {
            if(report[i] == 0x48) {
              
              if(report[0] & 0x1 || report[0] & 0x10) {
                kbd_send(0xe0); kbd_send(0x7e); kbd_send(0xe0); kbd_send(0xf0); kbd_send(0x7e);
              } else {
                kbd_send(0xe1); kbd_send(0x14); kbd_send(0x77); kbd_send(0xe1);
                kbd_send(0xf0); kbd_send(0x14); kbd_send(0xf0); kbd_send(0x77);
              }
              
              continue;
            }
            
            repeat = report[i];
            if(repeater) cancel_alarm(repeater);
            repeater = add_alarm_in_ms(delay_ms, repeat_callback, NULL, false);
            
            maybe_send_e0(report[i]);
            kbd_send(hid2ps2[report[i]]);
          }
        }
        
        prev_rpt[i] = report[i];
      }
      
      tuh_hid_receive_report(dev_addr, instance);
      board_led_write(0);
    break;
    
    case HID_ITF_PROTOCOL_MOUSE:
      printf("%02x %02x %02x %02x\n", report[0], report[1], report[2], report[3]);
      
      if(ms_mode != MS_MODE_STREAMING) {
        tuh_hid_receive_report(dev_addr, instance);
        return;
      }
      
      board_led_write(1);
      
      uint8_t s = report[0] + 8;
      uint8_t x = report[1] & 0x7f;
      uint8_t y = report[2] & 0x7f;
      uint8_t z = report[3] & 7;
      
      if(report[1] >> 7) {
        s += 0x10;
        x += 0x80;
      }
      
      if(report[2] >> 7) {
        y = 0x80 - y;
      } else if(y) {
        s += 0x20;
        y = 0x100 - y;
      }
      
      ms_send(s);
      ms_send(x);
      ms_send(y);
      
      if (ms_type == MS_TYPE_WHEEL_3 || ms_type == MS_TYPE_WHEEL_5) {
        // TODO: add proper support for buttons 4 & 5
        
        if(report[3] >> 7) {
          z = 0x8 - z;
        } else if(z) {
          z = 0x100 - z;
        }
        
        ms_send(z);
      }
      
      tuh_hid_receive_report(dev_addr, instance);
      board_led_write(0);
    break;
  }
  
}

void irq_callback(uint gpio, uint32_t events) {
  if(gpio == KBCLK && !gpio_get(KBDAT) && !pio_interrupt_get(iokbd, 0)) {
    printf("IRQ KB  ");
    pio_sm_drain_tx_fifo(iokbd, txkbd);
    pio_sm_exec(iokbd, txkbd, pio_encode_jmp(jmpkbd + 2));
  }
  
  if(gpio == MSCLK && !gpio_get(MSDAT) && !pio_interrupt_get(ioms, 0)) {
    printf("IRQ MS  ");
    pio_sm_drain_tx_fifo(ioms, txms);
    pio_sm_exec(ioms, txms, pio_encode_jmp(jmpms + 2));
  }
}

void main() {
  board_init();
  printf("ps2x2pico-0.6\n");
  
  gpio_init(LVPWR);
  gpio_set_dir(LVPWR, GPIO_OUT);
  gpio_put(LVPWR, 1);
  pio_gpio_init(iokbd, KBCLK);
  pio_gpio_init(iokbd, KBDAT);
  pio_gpio_init(ioms, MSCLK);
  pio_gpio_init(ioms, MSDAT);
  
  jmpkbd = pio_add_program(iokbd, &ps2dev_program);
  jmpms = pio_add_program(ioms, &ps2dev_program);
  
  txkbd = pio_claim_unused_sm(iokbd, true);
  pio_sm_config c1 = ps2dev_program_get_default_config(jmpkbd);
  sm_config_set_clkdiv(&c1, 2560);
  sm_config_set_jmp_pin(&c1, KBCLK);
  sm_config_set_set_pins(&c1, KBCLK, 1);
  sm_config_set_sideset_pins(&c1, KBDAT);
  sm_config_set_out_pins(&c1, KBDAT, 1);
  sm_config_set_out_shift(&c1, true, true, 11);
  sm_config_set_in_pins(&c1, KBDAT);
  sm_config_set_in_shift(&c1, true, true, 9);
  pio_sm_init(iokbd, txkbd, jmpkbd, &c1);
  pio_sm_set_enabled(iokbd, txkbd, true);
  
  txms = pio_claim_unused_sm(ioms, true);
  pio_sm_config c2 = ps2dev_program_get_default_config(jmpms);
  sm_config_set_clkdiv(&c2, 2560);
  sm_config_set_jmp_pin(&c2, MSCLK);
  sm_config_set_set_pins(&c2, MSCLK, 1);
  sm_config_set_sideset_pins(&c2, MSDAT);
  sm_config_set_out_pins(&c2, MSDAT, 1);
  sm_config_set_out_shift(&c2, true, true, 11);
  sm_config_set_in_pins(&c2, MSDAT);
  sm_config_set_in_shift(&c2, true, true, 9);
  pio_sm_init(ioms, txms, jmpms, &c2);
  pio_sm_set_enabled(ioms, txms, true);
  
  gpio_set_irq_enabled_with_callback(KBCLK, GPIO_IRQ_EDGE_RISE, true, &irq_callback);
  gpio_set_irq_enabled_with_callback(MSCLK, GPIO_IRQ_EDGE_RISE, true, &irq_callback);
  tusb_init();
  
  while(true) {
    tuh_task();
    
    if(!pio_sm_is_rx_fifo_empty(iokbd, txkbd)) {
      uint32_t fifo = pio_sm_get(iokbd, txkbd);
      printf("fifo %08x ", fifo);
      fifo = fifo >> 23;
      
      uint8_t parity = 1;
      for(uint8_t i = 0; i < 8; i++) {
        parity = parity ^ (fifo >> i & 1);
      }
      
      if(parity != fifo & 0x100) {
        kbd_send(0xfe);
      } else {
        printf("got KB %02x  ", (unsigned char)fifo);
        process_kbd(fifo);
      }
    }
    
    if(pio_sm_get_rx_fifo_level(ioms, txms)) {
      uint32_t fifo = pio_sm_get_blocking(ioms, txms);
      printf("fifo %08x ", fifo);
      fifo = fifo >> 23;
      
      uint8_t parity = 1;
      for(uint8_t i = 0; i < 8; i++) {
        parity = parity ^ (fifo >> i & 1);
      }
      
      if(parity != fifo & 0x100) {
        ms_send(0xfe);
      } else {
        printf("got MS %02x  ", (unsigned char)fifo);
        process_ms(fifo);
      }
    }
    
    if(repeating) {
      repeating = false;
      
      if(repeat) {
        maybe_send_e0(repeat);
        kbd_send(hid2ps2[repeat]);
      }
    }
  }
}
