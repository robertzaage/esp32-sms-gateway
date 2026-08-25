#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t interface_number;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t bulk_in_ep;
    uint8_t bulk_out_ep;
    uint8_t interrupt_in_ep;
    uint8_t score;
} modem_usb_candidate_t;

/**
 * Parse a raw active USB configuration descriptor and return ranked
 * vendor-specific serial candidates. Only alternate setting zero is eligible.
 */
size_t modem_usb_find_serial_candidates(const uint8_t *descriptor,
                                        size_t descriptor_len,
                                        modem_usb_candidate_t *candidates,
                                        size_t max_candidates);

#ifdef __cplusplus
}
#endif
