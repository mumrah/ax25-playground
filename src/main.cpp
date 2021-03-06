#include <Wire.h>
#include <Arduino.h>
#include <CircularBuffer.h>
#include <cc1101.h>
#include <ccpacket.h>

#include "ax25.h"
#include "HDLC.h"
#include "KISS.h"

#define CC1101Interrupt 0 // Pin 2
#define CC1101_GDO0 2

CC1101 radio;

byte syncWord[2] = {199, 10};

#define I2C_ADDRESS 0x04

#define DEBUG false

#if DEBUG
#define PRINTS(s)   { Serial.print(F(s)); }
#define PRINT(v)    { Serial.print(v); }
#define PRINTLN(v)  { Serial.println(v); }
#define PRINTLNS(s)  { Serial.println(F(s)); }
#define PRINT2(s,v)  { Serial.print(F(s)); Serial.print(v); }
#define PRINT3(s,v,b) { Serial.print(F(s)); Serial.print(v, b); }
#else
#define PRINTS(s)
#define PRINT(v)
#define PRINTLN(v)
#define PRINTLNS(s)
#define PRINT2(s,v)
#define PRINT3(s,v,b)
#endif

#define EXP_BACKOFF(a,b,x) (a + b * (2^x))

// Get signal strength indicator in dBm.
// See: http://www.ti.com/lit/an/swra114d/swra114d.pdf
int rssi(char raw) {
    uint8_t rssi_dec;
    // TODO: This rssi_offset is dependent on baud and MHz; this is for 38.4kbps and 433 MHz.
    uint8_t rssi_offset = 74;
    rssi_dec = (uint8_t) raw;
    if (rssi_dec >= 128)
        return ((int)( rssi_dec - 256) / 2) - rssi_offset;
    else
        return (rssi_dec / 2) - rssi_offset;
}

// Get link quality indicator.
int lqi(char raw) {
    return 0x3F - raw;
}

// globals
CircularBuffer<uint8_t, 400> i2c_input_buffer;
CircularBuffer<uint8_t, 100> i2c_output_buffer;
CircularBuffer<uint8_t, 8> ack_queue;

KISSCtx kissCtx;
HDLC incomingFrame;
HDLC outgoingFrame;
CCPACKET packet;

#define TX_COUNTDOWN_MS 20
#define HDLC_MODE HDLC_MODE_BEST_EFFORT

unsigned long last_tx = 0;
uint8_t tx_packet_buffer[48];

bool packetWaiting;

void messageReceived() {
    packetWaiting = true;
}

/*
 * i2c master is requesting data from us. Write out anything in the output
 * buffer, otherwise send 0x0E to indicate we have nothing to write
 */
void on_i2c_read_request() {
  if(i2c_output_buffer.isEmpty()) {
    Wire.write(0x0E); // Tell the TNC we've got nothing
  } else {
    PRINTS("Writing data to TNC");

    uint8_t tmp[8];
    uint8_t i;
    for(i=0; i<min(i2c_output_buffer.size(), 8); i++) {
      tmp[i] = i2c_output_buffer.pop();
    }
    Wire.write(tmp, i);
  }
}

/*
 * i2c master is writing to us, read as many bytes from the i2c
 * buffer as we can into our own input buffer
 */
void on_i2c_write_receive(int n) {
  while (Wire.available() && i2c_input_buffer.available() > 0) {
    uint8_t byte = Wire.read();
    bool res = i2c_input_buffer.push(byte);
    if(!res) {
      PRINTLN("!!!! Buffer overrun !!!!");
    }
  }
}

void hdlc_send_data(HDLC * hdlc, CCPACKET * packet) {
  packet->data[0] = hdlc->address & 0xFF;
  packet->data[1] = hdlc->control & 0xFF;
  memcpy(&(packet->data[2]), hdlc->data, hdlc->data_length);
  packet->length = hdlc->data_length + 2;
  //delayMicroseconds(75); // settling time for other receiver
  detachInterrupt(CC1101Interrupt);
  radio.sendData(*packet);
  attachInterrupt(CC1101Interrupt, messageReceived, FALLING);
  hdlc->send_attempts += 1;
  hdlc->time_sent = millis();
  hdlc->do_retry = false;
  // don't set ACK here
}

HDLC supervisory;
void hdlc_send_ack(CCPACKET * packet, uint8_t seq) {
  hdlc_new_ack_frame(&supervisory, seq);
  hdlc_send_data(&supervisory, packet);
}

void read_hdlc(CCPACKET * packet, HDLC * hdlc) {
  hdlc->address = packet->data[0];
  hdlc->control = packet->data[1];
  memcpy(hdlc->data, &(packet->data[2]), packet->length - 2);
  hdlc->data_length = packet->length - 2;
}

/**
 * Here we have recieved a packet from the radio
 */
void on_rf_packet(uint8_t * data, uint8_t len) {
  for(uint8_t i=0; i<incomingFrame.data_length; i++) {
    bool res = i2c_output_buffer.push(incomingFrame.data[i]);
    if(!res) {
      PRINTLN("!!! Output Buffer Overrun !!!");
      break;
    }
  }
}

void debug_state(char * msg) {
  PRINT("=================="); PRINT(msg); PRINTLN("==================");
  PRINT2("packetWaiting: ",packetWaiting); PRINTLN("");

  PRINT2("incomingFrame#ack: ", incomingFrame.ack); PRINTLN("");
  PRINT2("incomingFrame#time_sent: ", incomingFrame.time_sent); PRINTLN("");
  PRINT2("incomingFrame#send_attempts: ", incomingFrame.send_attempts); PRINTLN("");
  PRINT2("incomingFrame#do_retry: ", incomingFrame.do_retry); PRINTLN("");

  PRINT2("outgoingFrame#ack: ", outgoingFrame.ack); PRINTLN("");
  PRINT2("outgoingFrame#time_sent: ", outgoingFrame.time_sent); PRINTLN("");
  PRINT2("outgoingFrame#send_attempts: ", outgoingFrame.send_attempts); PRINTLN("");
  PRINT2("outgoingFrame#do_retry: ", outgoingFrame.do_retry); PRINTLN("");

}

void setup() {
  radio.init();
  radio.setSyncWord(syncWord);
  radio.setCarrierFreq(CFREQ_433);
  radio.disableAddressCheck();
  radio.setTxPowerAmp(PA_LowPower);
  //radio.writeReg(CC1101_MDMCFG2, 0x03);
  //radio.writeReg(CC1101_MDMCFG3, 0x83);
  //radio.writeReg(CC1101_MDMCFG4, 0xF5);
  //radio.writeReg(CC1101_DEVIATN, 0x40);

  attachInterrupt(CC1101Interrupt, messageReceived, FALLING);

  Wire.begin(I2C_ADDRESS);
  Wire.onRequest(on_i2c_read_request);
  Wire.onReceive(on_i2c_write_receive);

  Serial.begin(9600);
  PRINTLN("CC1101_PARTNUM ");
  PRINTLN(radio.readReg(CC1101_PARTNUM, CC1101_STATUS_REGISTER));
  PRINTLN("CC1101_VERSION ");
  PRINTLN(radio.readReg(CC1101_VERSION, CC1101_STATUS_REGISTER));
  PRINTLN("CC1101_MARCSTATE ");
  PRINTLN(radio.readReg(CC1101_MARCSTATE, CC1101_STATUS_REGISTER) & 0x1f);

  PRINTLN("CC1101 radio initialized.");
  PRINTLN("Begin!");
}

void loop() {
  bool newFrame = false;

  // Check incoming packet
  if(packetWaiting) {
    detachInterrupt(CC1101Interrupt);
    packetWaiting = false;
    if (radio.receiveData(&packet) > 0) {
      if (packet.crc_ok && packet.length > 0) {
        read_hdlc(&packet, &incomingFrame);
        newFrame = true;
      }
    }
    attachInterrupt(CC1101Interrupt, messageReceived, FALLING);
  }

  // Handle new HDLC frame
  if(newFrame) {
    switch(hdlc_get_frame_type(&incomingFrame)) {
      case HDLC_I_FRAME: { // Information (data) frame
        // Buffer ACK
        uint8_t seq = hdlc_get_i_frame_send_seq(&incomingFrame);
        ack_queue.push(seq);
        // Run callback
        on_rf_packet(incomingFrame.data, incomingFrame.data_length);
      } break;
      case HDLC_S_FRAME: { // Supervisory frame
        // TODO check seq num for RR and REJ
        //uint8_t recv_seq = hdlc_get_s_frame_recv_seq(&incomingFrame);
        if(hdlc_get_s_frame_type(&incomingFrame) == HDLC_S_TYPE_RR) {
          //uint8_t sent_seq = hdlc_get_i_frame_send_seq(&outgoingFrame);
          outgoingFrame.ack = true;
        }
      } break;
      case HDLC_U_FRAME: {
        if(hdlc_get_s_frame_type(&incomingFrame) == HDLC_U_TYPE_UI) {
          on_rf_packet(incomingFrame.data, incomingFrame.data_length);
        }
      } break;
      default:
        break;
    }
  }

  // Check incoming serial data
  while (Serial.available() && i2c_input_buffer.available() > 0) {
    uint8_t byte = Serial.read();
    bool res = i2c_input_buffer.push(byte);
    if(!res) {
      // Shouldn't happen
      PRINTLN("!!!! Input Buffer Overrun !!!!");
      break;
    }
  }

  // Decide if we need to TX
  unsigned long now = millis();
  if(now - last_tx > TX_COUNTDOWN_MS) {
    if(ack_queue.size() > 0) {
      hdlc_send_ack(&packet, ack_queue.shift());
      last_tx = now;
    } else {
      // Check ACK timeout
      if(outgoingFrame.ack == false) {
        if(now - outgoingFrame.time_sent > EXP_BACKOFF(100, 50, outgoingFrame.send_attempts) ) {
          outgoingFrame.do_retry = true;
        }

        if(outgoingFrame.do_retry == true) {
          if(outgoingFrame.send_attempts > 7) { // too many retries
            PRINTLN("drop");
            outgoingFrame.do_retry = false;
            outgoingFrame.failed = true;
            outgoingFrame.ack = true;
          } else {
            PRINTLN("resend");
            outgoingFrame.do_retry = false;
            outgoingFrame.send_attempts += 1; // fake it for now
            //hdlc_send_data(&outgoingFrame, &packet);
          }
        }
      }

      if(i2c_input_buffer.size() > 0) {
        uint8_t n = min(i2c_input_buffer.size(), 48);
        if(n > 0) {
          for(uint8_t i=0; i<n; i++) {
            tx_packet_buffer[i] = i2c_input_buffer.shift();
          }
          #if HDLC_MODE == HDLC_MODE_BEST_EFFORT
          hdlc_new_ui_frame(&outgoingFrame, tx_packet_buffer, n);
          #elif HDLC_MODE == HDLC_MODE_RELIABLE
          hdlc_new_data_frame(&outgoingFrame, tx_packet_buffer, n);
          #endif
          hdlc_send_data(&outgoingFrame, &packet);
          last_tx = now;
        }
      }
    }
  }

  // Output Serial, if any
  while(i2c_output_buffer.size() > 0) {
    Serial.write(i2c_output_buffer.shift());
  }
}
