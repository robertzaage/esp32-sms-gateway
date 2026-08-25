#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "usb_modem_descriptor.h"

#define CFG_DESC(total_len, interfaces) \
    9, 2, (uint8_t)((total_len) & 0xff), (uint8_t)(((total_len) >> 8) & 0xff), interfaces, 1, 0, 0x80, 1
#define IF_DESC(num, alt, eps, cls, sub, proto) \
    9, 4, num, alt, eps, cls, sub, proto, 0
#define EP_DESC(addr, attr, mps) \
    7, 5, addr, attr, (uint8_t)((mps) & 0xff), (uint8_t)(((mps) >> 8) & 0xff), 0

int main(void)
{
    /* Synthetic equivalent of the Huawei 12d1:1506 layout observed on Linux. */
    static const uint8_t descriptor[] = {
        CFG_DESC(0x0096, 5),

        IF_DESC(0, 0, 2, 0xff, 2, 0x12),
        EP_DESC(0x82, 2, 64),
        EP_DESC(0x02, 2, 64),

        IF_DESC(1, 0, 3, 0xff, 2, 0x01),
        EP_DESC(0x84, 3, 10),
        EP_DESC(0x83, 2, 64),
        EP_DESC(0x03, 2, 64),

        /* NCM control alt 0 has no bulk pair. */
        IF_DESC(2, 0, 1, 0xff, 2, 0x16),
        EP_DESC(0x86, 3, 16),

        /* NCM data alt 1 has bulk endpoints but must never be an AT candidate. */
        IF_DESC(2, 1, 3, 0xff, 2, 0x16),
        EP_DESC(0x86, 3, 16),
        EP_DESC(0x85, 2, 64),
        EP_DESC(0x04, 2, 64),

        IF_DESC(3, 0, 2, 8, 6, 0x50),
        EP_DESC(0x87, 2, 64),
        EP_DESC(0x05, 2, 64),
    };

    modem_usb_candidate_t candidates[4] = {0};
    const size_t count = modem_usb_find_serial_candidates(
        descriptor, sizeof(descriptor), candidates, 4);

    assert(count == 2);

    /* Protocol 0x01 + interrupt endpoint makes interface 1 the preferred AT port. */
    assert(candidates[0].interface_number == 1);
    assert(candidates[0].interface_protocol == 0x01);
    assert(candidates[0].bulk_in_ep == 0x83);
    assert(candidates[0].bulk_out_ep == 0x03);
    assert(candidates[0].interrupt_in_ep == 0x84);

    assert(candidates[1].interface_number == 0);
    assert(candidates[1].interface_protocol == 0x12);
    assert(candidates[1].bulk_in_ep == 0x82);
    assert(candidates[1].bulk_out_ep == 0x02);

    return 0;
}
