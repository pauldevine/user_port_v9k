#ifndef _CRC8_H_
#define _CRC8_H_

#include <stdint.h>
#include <stdbool.h>

#include "protocols.h"

#ifdef __GNUC__
    // Code specific to GNU ARM compiler (GCC)
    void cdprintf (char *msg, ...);
#endif

void generateCrc8Table(void);
uint8_t crc8(const uint8_t *data, size_t len);
void create_command_crc8(Payload *payload);
void create_data_crc8(Payload *payload);
void create_payload_crc8(Payload *payload);
bool is_valid_command_crc8(const Payload *payload);
bool is_valid_data_crc8(const Payload *payload);

#endif /* _CRC8_H_ */
