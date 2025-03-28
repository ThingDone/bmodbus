//
// Created by billm on 3/18/2025.
//
#include <stdio.h>
#include <stdint.h>
#include "bmodbus.h"
#include "bmodbus_internals.h"
/* Headers of requests:
 * 1 read coils = 2 byte starting address, 2 byte quantity of coils
 * 2 read discrete inputs = 2 byte starting address, 2 byte quantity of inputs
 * 3 read holding registers = 2 byte starting address, 2 byte quantity of registers
 * 4 read input registers = 2 byte starting address, 2 byte quantity of registers
 * 5 write single coil = 2 byte address, 2 byte value
 * 6 write single register = 2 byte address, 2 byte value
 * 15 write multiple coils = 2 byte starting address, 2 byte quantity of coils, 1 byte byte count, N bytes of data  (header is only 4 bytes)
 * 16 write multiple registers = 2 byte starting address, 2 byte quantity of registers, 1 byte byte count, N bytes of data (header is only 4 bytes)
 */

#ifndef MODBUS_HTONS
#define MODBUS_HTONS(x) ((((x) & 0xFF) << 8) | (((x) & 0xFF00) >> 8))
#endif
#ifndef MODBUS_MEMMOVE
#define MODBUS_MEMMOVE(DST, SRC, N) {for(int16_t memmove_i=(N)-1; memmove_i>-1; i--) ((uint8_t*)(DST))[memmove_i] = ((uint8_t*)(SRC))[memmove_i];}
#endif

static void bmodbus_init(modbus_client_t *bmodbus, uint32_t interframe_delay, uint8_t client_address){
    printf("bmodbus_init\n");
    bmodbus->state = CLIENT_STATE_IDLE;
    bmodbus->interframe_delay = interframe_delay;
    bmodbus->client_address = client_address;
    bmodbus->crc.half = 0xFFFF;
}

static void bmodbus_deinit(modbus_client_t *bmodbus){
    printf("bmodbus_deinit\n");
    bmodbus->state = CLIENT_NO_INIT;
}

static void process_request(modbus_client_t *bmodbus){

}

static uint16_t crc_update(uint16_t crc, uint8_t byte) {
    uint16_t poly = 0xA001;
    crc = (uint16_t)(crc ^ byte);
    for (uint8_t i = 0; i < 8; i++) {
        if (crc & 1) {
            crc = (crc >> 1) ^ poly;
        } else {
            crc = (crc >> 1);
        }
    }
    return crc;
}

static void bmodbus_client_next_byte(modbus_client_t *bmodbus, uint32_t microseconds, uint8_t byte){
    //If the time delta is greater than the interframe delay, we should reset the state machine, and then process from scratch
    if((microseconds - bmodbus->last_microseconds) > bmodbus->interframe_delay){
        bmodbus->state = CLIENT_STATE_IDLE;
        bmodbus->byte_count = 0;
        bmodbus->crc.half = 0xFFFF;
    }
    bmodbus->last_microseconds = microseconds;
    switch(bmodbus->state){
        case CLIENT_STATE_IDLE:
            if(byte == bmodbus->client_address){
                bmodbus->state = CLIENT_STATE_FUNCTION_CODE;
            }else{
                //FIXME we should add optional tracking of errors for debug purposes
                //FIXME future work, we can add broadcast here
                bmodbus->state = CLIENT_STATE_WAITING_FOR_NEXT_MESSAGE;
            }
            break;
        case CLIENT_STATE_FUNCTION_CODE:
            bmodbus->function = byte;
            bmodbus->state = CLIENT_STATE_HEADER;
            break;
        case CLIENT_STATE_HEADER:
            bmodbus->header.byte[bmodbus->byte_count-2] = byte;
            if(bmodbus->byte_count == 5){
                //Endianness conversion
                bmodbus->header.word[0] = MODBUS_HTONS(bmodbus->header.word[0]);
                bmodbus->header.word[1] = MODBUS_HTONS(bmodbus->header.word[1]);
                if((bmodbus->function == 15) || (bmodbus->function == 16)){ //These are the only functions that have a byte count
                    bmodbus->state = CLIENT_STATE_HEADER_CHECK;
                }else{
                    bmodbus->state = CLIENT_STATE_FOOTER;
                }
            }
            break;
        case CLIENT_STATE_HEADER_CHECK:
            if(byte == 2*(bmodbus->header.word[1])) { //It contains a byte count and we compare it with 2x the number of 16bit registers
                if(byte > BMB_MAXIMUM_MESSAGE_SIZE/2){
                    //FIXME we should add optional tracking of errors for debug purposes
                    bmodbus->state = CLIENT_STATE_WAITING_FOR_NEXT_MESSAGE;
                }else {
                    bmodbus->index = 0;
                    bmodbus->state = CLIENT_STATE_DATA;
                }
            }else{
                //FIXME we should add optional tracking of errors for debug purposes
                bmodbus->state = CLIENT_STATE_WAITING_FOR_NEXT_MESSAGE;
            }
            break;
        case CLIENT_STATE_DATA:
            ((uint8_t*)bmodbus->payload.request.data)[bmodbus->index] = byte;
            if(bmodbus->index & 1){ //Endianness conversion every completed word
                bmodbus->payload.request.data[bmodbus->index/2] = MODBUS_HTONS(bmodbus->payload.request.data[bmodbus->index/2]);
            }
            bmodbus->index++;
            if(bmodbus->index == 2 * bmodbus->header.word[1]){
                //Here we can process the request
                bmodbus->state = CLIENT_STATE_FOOTER;
                bmodbus->index = 0;
            }
            break;
        case CLIENT_STATE_FOOTER:
            //Here we verify the CRC
            if(bmodbus->crc.byte[0] == byte){
                bmodbus->state = CLIENT_STATE_FOOTER2;
            }else{
                //FIXME bad CRC
                bmodbus->state = CLIENT_STATE_WAITING_FOR_NEXT_MESSAGE;
            }
            break;
        case CLIENT_STATE_FOOTER2:
            if(bmodbus->crc.byte[1] == byte){
                //GOOD CRC!!!
                //FIXME --  either process the request OR just wait for a polling entry (pending request)
                bmodbus->state = CLIENT_STATE_PROCESSING_REQUEST;
                process_request(bmodbus);
            }else{
                //FIXME bad CRC
                bmodbus->state = CLIENT_STATE_WAITING_FOR_NEXT_MESSAGE;
            }
            break;
    }
    bmodbus->byte_count++;
    if((bmodbus->state == CLIENT_STATE_FUNCTION_CODE) || (bmodbus->state == CLIENT_STATE_HEADER) || (bmodbus->state == CLIENT_STATE_HEADER_CHECK) || (bmodbus->state == CLIENT_STATE_DATA) || (bmodbus->state == CLIENT_STATE_FOOTER)){
        bmodbus->crc.half = crc_update(bmodbus->crc.half, byte);
    }
}

static void bmodbus_client_loop(modbus_client_t *bmodbus, uint32_t microsecond){
    printf("bmodbus_client_loop\n");
}

static modbus_client_t modbus1;
void modbus1_init(uint8_t client_address){
    bmodbus_init(&modbus1, INTERFRAME_DELAY_MICROSECONDS(BMB1_BAUDRATE), client_address);
}
void modbus1_next_byte(uint32_t microseconds, uint8_t byte){
    bmodbus_client_next_byte(&modbus1, microseconds, byte);
}
void modbus1_single_loop(uint32_t microseconds){
    bmodbus_client_loop(&modbus1, microseconds);
}

#ifdef UNIT_TESTING
modbus_client_t * modbus1_testing = &modbus1;
#endif
