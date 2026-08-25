#include "usb_modem_descriptor.h"

#include <stdbool.h>
#include <string.h>

#define USB_DESC_TYPE_INTERFACE 0x04
#define USB_DESC_TYPE_ENDPOINT 0x05
#define USB_EP_XFER_TYPE_MASK 0x03
#define USB_EP_XFER_BULK 0x02
#define USB_EP_XFER_INTERRUPT 0x03
#define USB_EP_DIR_IN 0x80
#define USB_VENDOR_SPECIFIC_CLASS 0xFF

static uint8_t candidate_score(const modem_usb_candidate_t *candidate)
{
    uint8_t score = 10;

    /* Huawei's V.250/AT interface normally reports protocol 0x01. */
    if (candidate->interface_protocol == 0x01) {
        score += 100;
    } else if (candidate->interface_protocol == 0x12) {
        /* Known secondary Huawei serial/PC-UI style interface. */
        score += 50;
    }

    if (candidate->interrupt_in_ep != 0) {
        score += 10;
    }
    return score;
}

static void sort_candidates(modem_usb_candidate_t *candidates, size_t count)
{
    for (size_t i = 1; i < count; ++i) {
        modem_usb_candidate_t current = candidates[i];
        size_t j = i;
        while (j > 0 && candidates[j - 1].score < current.score) {
            candidates[j] = candidates[j - 1];
            --j;
        }
        candidates[j] = current;
    }
}

static void add_candidate(modem_usb_candidate_t *candidates,
                          size_t max_candidates,
                          size_t *count,
                          const modem_usb_candidate_t *candidate)
{
    if (candidate->interface_class != USB_VENDOR_SPECIFIC_CLASS ||
        candidate->bulk_in_ep == 0 || candidate->bulk_out_ep == 0 ||
        *count >= max_candidates) {
        return;
    }

    modem_usb_candidate_t copy = *candidate;
    copy.score = candidate_score(&copy);
    candidates[(*count)++] = copy;
}

size_t modem_usb_find_serial_candidates(const uint8_t *descriptor,
                                        size_t descriptor_len,
                                        modem_usb_candidate_t *candidates,
                                        size_t max_candidates)
{
    if (descriptor == NULL || candidates == NULL || max_candidates == 0) {
        return 0;
    }

    const uint8_t *cursor = descriptor;
    const uint8_t *end = descriptor + descriptor_len;
    modem_usb_candidate_t current = {0};
    bool collecting = false;
    size_t count = 0;

    while (cursor + 2 <= end) {
        const uint8_t length = cursor[0];
        const uint8_t type = cursor[1];
        if (length < 2 || cursor + length > end) {
            break;
        }

        if (type == USB_DESC_TYPE_INTERFACE && length >= 9) {
            if (collecting) {
                add_candidate(candidates, max_candidates, &count, &current);
            }

            memset(&current, 0, sizeof(current));
            current.interface_number = cursor[2];
            const uint8_t alternate_setting = cursor[3];
            current.interface_class = cursor[5];
            current.interface_subclass = cursor[6];
            current.interface_protocol = cursor[7];

            /* Excludes e.g. Huawei NCM data alternate setting 1. */
            collecting = alternate_setting == 0 &&
                         current.interface_class == USB_VENDOR_SPECIFIC_CLASS;
        } else if (type == USB_DESC_TYPE_ENDPOINT && length >= 7 && collecting) {
            const uint8_t address = cursor[2];
            const uint8_t transfer_type = cursor[3] & USB_EP_XFER_TYPE_MASK;
            if (transfer_type == USB_EP_XFER_BULK) {
                if ((address & USB_EP_DIR_IN) != 0) {
                    current.bulk_in_ep = address;
                } else {
                    current.bulk_out_ep = address;
                }
            } else if (transfer_type == USB_EP_XFER_INTERRUPT &&
                       (address & USB_EP_DIR_IN) != 0) {
                current.interrupt_in_ep = address;
            }
        }

        cursor += length;
    }

    if (collecting) {
        add_candidate(candidates, max_candidates, &count, &current);
    }

    sort_candidates(candidates, count);
    return count;
}
