
#include "marshaller.h"

uint8_t marshaller(const char *operation, const char *topics, const char *message, char *payload, size_t size) {

    snprintf(payload, size, 
    "OP:'%s'\nTHING_ID:'%s'\nTOPICS:['%s']\n\n%s",
    operation, THING_ID, topics, message);

    return 0;
    
}

void unmarshaller(const char *operation, const char *topics, char *buffer, const size_t len) {
    
} 
