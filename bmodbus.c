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
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define MODBUS_HTONS(x) ((((x) & 0xFF) << 8) | (((x) & 0xFF00) >> 8))
#else
#define MODBUS_HTONS(x) (x)
#endif
#endif

#ifndef MODBUS_MEMMOVE
static void modbus_memmove(void * dst, void * src, uint16_t count) {
    if(dst < src) { //If dest is before source, we can copy forward
        for(uint16_t i = 0; i<count; i++) {
            ((uint8_t*)dst)[i] = ((uint8_t*)src)[i];
        }
        return;
    }else {
        for (int16_t i = (count) - 1; i >= 0; i--) {
            ((uint8_t *) dst)[i] = ((uint8_t *) src)[i];
        }
    }
}
#define MODBUS_MEMMOVE modbus_memmove
#endif
#define MODBUS_UNUSED(x) (void)(x)

static void bmodbus_init(modbus_client_t *bmodbus, uint32_t interframe_delay, uint8_t client_address){
    bmodbus->state = CLIENT_STATE_IDLE;
    bmodbus->interframe_delay = interframe_delay;
    bmodbus->client_address = client_address;
    bmodbus->crc.half = 0xFFFF;
}

static void bmodbus_deinit(modbus_client_t *bmodbus){
    printf("bmodbus_deinit\n");
    bmodbus->state = CLIENT_NO_INIT;
}

static inline uint8_t process_request(modbus_client_t *bmodbus){
    //Returns true iff the request was processed successfully via events
    MODBUS_UNUSED(bmodbus);
#ifdef MODBUS1_CALLBACK
    if(bmodbus == &modbus1){
        return MODBUS1_CALLBACK(bmodbus);
    }
#endif //MODBUS1_CALLBACK
    return 0;
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
                if(bmodbus->function == 16) { //These are the only functions that have a byte count
                    bmodbus->state = CLIENT_STATE_HEADER_CHECK;
                    bmodbus->byte_size = 2 * bmodbus->header.word[1]; //Bytes for number of registers
                    //FIXME -- ensure byte_size cannot be more than 250
                }else if(bmodbus->function == 15){
                    bmodbus->state = CLIENT_STATE_HEADER_CHECK;
                    bmodbus->byte_size = (bmodbus->header.word[1] + 7) / 8;//Bytes for target number of bits
                    //FIXME -- ensure byte_size cannot be more than 250
                }else{
                    bmodbus->state = CLIENT_STATE_FOOTER;
                }
            }
            break;
        case CLIENT_STATE_HEADER_CHECK:
            if(byte == bmodbus->byte_size) { //It contains a byte count and we compare it with 2x the number of 16bit registers
                bmodbus->index = 0;
                bmodbus->state = CLIENT_STATE_DATA;
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
            if(bmodbus->index == bmodbus->byte_size){
                //Here we can process the request
                bmodbus->state = CLIENT_STATE_FOOTER;
                bmodbus->index = 0;
            }
            break;
        case CLIENT_STATE_FOOTER:
            bmodbus->crc.half = MODBUS_HTONS(bmodbus->crc.half);
            //Here we verify the CRC
            if(bmodbus->crc.byte[1] == byte){
                bmodbus->state = CLIENT_STATE_FOOTER2;
            }else{
                //FIXME bad CRC
                bmodbus->state = CLIENT_STATE_WAITING_FOR_NEXT_MESSAGE;
            }
            break;
        case CLIENT_STATE_FOOTER2:
            if(bmodbus->crc.byte[0] == byte){
                //FIXME --  either process the request OR just wait for a polling entry (pending request)
                bmodbus->payload.request.function = bmodbus->function;
                bmodbus->payload.request.address = bmodbus->header.word[0];
                //Here we prep the request struct for higher layers
                switch (bmodbus->function) {
                    case 6:
                        bmodbus->payload.request.size = 1;
                        bmodbus->payload.request.data[0] = bmodbus->header.word[1];
                        break;
                    case 15:
                    case 16:
                    case 3:
                    case 4:
                    case 2:
                    case 1:
                        bmodbus->payload.request.size = bmodbus->header.word[1];
                        break;
                    default:
                        break;
                }
                bmodbus->payload.request.result = 0;
                bmodbus->state = CLIENT_STATE_PROCESSING_REQUEST;
                if(process_request(bmodbus)){
                    //event processing was complete
                }
            }else{
                //FIXME bad CRC
                bmodbus->state = CLIENT_STATE_WAITING_FOR_NEXT_MESSAGE;
            }
            break;
        default: //Ignore bytes in all other states
            break;
    }
    bmodbus->byte_count++;
    if((bmodbus->state == CLIENT_STATE_FUNCTION_CODE) || (bmodbus->state == CLIENT_STATE_HEADER) || (bmodbus->state == CLIENT_STATE_HEADER_CHECK) || (bmodbus->state == CLIENT_STATE_DATA) || (bmodbus->state == CLIENT_STATE_FOOTER)){
        bmodbus->crc.half = crc_update(bmodbus->crc.half, byte);
    }
}

static void bmodbus_client_loop(modbus_client_t *bmodbus, uint32_t microsecond){
    //FIXME -- currently not implemented
    MODBUS_UNUSED(bmodbus);
    MODBUS_UNUSED(microsecond);
    printf("bmodbus_client_loop\n");
}

static void bmodbus_encode_client_response(modbus_client_t *bmodbus){
    uint16_t temp1, temp2;
    int i;
    //This takes the request and encodes it into the response (assuming processing is completed)
    switch (bmodbus->function){
        case 6:
        case 16:
            //If failed return no response
            if(bmodbus->payload.request.result){
                bmodbus->payload.response.size = 0;
                break;
            }
            //Store values from the request for the response
            temp1 = bmodbus->payload.request.size;
            temp2 = bmodbus->payload.request.address;
            bmodbus->payload.response.size = 6;
            bmodbus->payload.response.data[2] = (temp2 & 0xFF00) >> 8;
            bmodbus->payload.response.data[3] = temp2 & 0xFF;
            bmodbus->payload.response.data[4] = (temp1 & 0xFF00) >> 8;
            bmodbus->payload.response.data[5] = temp1 & 0xFF;
            break;
        case 3:
        case 4:
            //If failed return no response
            if(bmodbus->payload.request.result){
                bmodbus->payload.response.size = 0;
                break;
            }
            //Store values from the request for the response
            temp1 = bmodbus->payload.request.size;
            //Endian flip the results
            for(i=0;i<temp1;i++){
                bmodbus->payload.request.data[i] = MODBUS_HTONS(bmodbus->payload.request.data[i]);
            }
            //Move the payload data to the response at the offset
            MODBUS_MEMMOVE(bmodbus->payload.response.data+3, bmodbus->payload.request.data, 2*temp1);
            //FIXME above move could overflow buffer on a weird read request
            bmodbus->payload.response.size = 3 + 2*temp1;
            bmodbus->payload.response.data[2] = 2*temp1;
            break;
        case 1:
        case 2:
            //If failed return no response
            if(bmodbus->payload.request.result){
                bmodbus->payload.response.size = 0;
                break;
            }
            //Store values from the request for the response
            temp1 = (bmodbus->payload.request.size + 7) / 8; //Number of bytes from number of bits
            //Move the payload data (bytes) to the response at the offset
            MODBUS_MEMMOVE(bmodbus->payload.response.data+3, bmodbus->payload.request.data, temp1);
            //FIXME above move could overflow buffer on a weird read request
            bmodbus->payload.response.size = 3 + temp1;
            bmodbus->payload.response.data[2] = temp1;
            break;
        case 15:
            //If failed return no response
            if(bmodbus->payload.request.result){
                bmodbus->payload.response.size = 0;
                break;
            }
            //Store values from the request for the response
            temp1 = bmodbus->payload.request.size;
            temp2 = bmodbus->payload.request.address;
            bmodbus->payload.response.size = 6;
            bmodbus->payload.response.data[2] = (temp2 & 0xFF00) >> 8;
            bmodbus->payload.response.data[3] = temp2 & 0xFF;
            bmodbus->payload.response.data[4] = (temp1 & 0xFF00) >> 8;
            bmodbus->payload.response.data[5] = temp1 & 0xFF;
            break;
    }
    if(bmodbus->payload.response.size) {
        uint16_t response_crc = 0xFFFF;
        //All responses start the same...
        bmodbus->payload.response.data[0] = bmodbus->client_address;
        bmodbus->payload.response.data[1] = bmodbus->function;
        //Calculate the CRC
        for (i = 0; i < bmodbus->payload.response.size; i++) {
            response_crc = crc_update(response_crc, bmodbus->payload.response.data[i]);
        }
        bmodbus->payload.response.data[bmodbus->payload.response.size] = response_crc & 0xFF;
        bmodbus->payload.response.data[bmodbus->payload.response.size + 1] = (response_crc & 0xFF00) >> 8;
        bmodbus->payload.response.size += 2;
    }
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
modbus_request_t * modbus1_get_request(void){
    if(modbus1.state == CLIENT_STATE_PROCESSING_REQUEST){
        return &(modbus1.payload.request);
    }
    return NULL;
}
modbus_uart_response_t * modbus1_get_response(void){
    if(modbus1.state == CLIENT_STATE_PROCESSING_REQUEST){
        //Here we process the request data structure into the UART response
        bmodbus_encode_client_response(&modbus1);
        modbus1.state = CLIENT_STATE_SENDING_RESPONSE;
        return &(modbus1.payload.response);
    }else if(modbus1.state == CLIENT_STATE_SENDING_RESPONSE){
        return &(modbus1.payload.response);
    }
    return NULL;
}

#ifdef UNIT_TESTING
modbus_client_t * modbus1_testing = &modbus1;
#endif
