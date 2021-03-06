// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <endian.h>
#include <magenta/hw/usb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "xhci.h"
#include "xhci-util.h"

//#define TRACE 1
#include "xhci-debug.h"

// list of devices pending result of enable slot command
// list is kept on xhci_t.command_queue
typedef struct {
    enum {
        ENUMERATE_DEVICE,
        DISCONNECT_DEVICE,
        RH_PORT_CONNECTED,
    } command;
    list_node_t node;
    uint32_t hub_address;
    uint32_t port;
    usb_speed_t speed;
} xhci_device_command_t;

static uint32_t xhci_get_route_string(xhci_t* xhci, uint32_t hub_address, uint32_t port) {
    if (hub_address == 0) {
        return 0;
    }

    xhci_slot_t* hub_slot = &xhci->slots[hub_address];
    uint32_t route = XHCI_GET_BITS32(&hub_slot->sc->sc0, SLOT_CTX_ROUTE_STRING_START, SLOT_CTX_ROUTE_STRING_BITS);
    int shift = 0;
    while (shift < 20) {
        if ((route & (0xF << shift)) == 0) {
            // reached end of parent hub's route string
            route |= ((port & 0xF) << shift);
            break;
        }
        shift += 4;
    }
    return route;
}

static mx_status_t xhci_address_device(xhci_t* xhci, uint32_t slot_id, uint32_t hub_address,
                                       uint32_t port, usb_speed_t speed) {
    xprintf("xhci_address_device slot_id: %d port: %d hub_address: %d speed: %d\n",
            slot_id, port, hub_address, speed);

    xhci_slot_t* slot = &xhci->slots[slot_id];
    if (slot->sc)
        return ERR_BAD_STATE;
    slot->hub_address = hub_address;
    slot->port = port;
    slot->rh_port = (hub_address == 0 ? port : xhci->slots[hub_address].rh_port);
    slot->speed = speed;

    // allocate DMA memory for device context
    uint8_t* device_context = (uint8_t*)xhci_memalign(xhci, 64, xhci->context_size * XHCI_NUM_EPS);
    if (!device_context) {
        printf("out of DMA memory!\n");
        return ERR_NO_MEMORY;
    }

    mx_status_t status = xhci_transfer_ring_init(xhci, &slot->transfer_rings[0], TRANSFER_RING_SIZE);
    if (status < 0)
        return status;

    xhci_input_control_context_t* icc = (xhci_input_control_context_t*)&xhci->input_context[0 * xhci->context_size];
    xhci_slot_context_t* sc = (xhci_slot_context_t*)&xhci->input_context[1 * xhci->context_size];
    xhci_endpoint_context_t* ep0c = (xhci_endpoint_context_t*)&xhci->input_context[2 * xhci->context_size];
    memset((void*)icc, 0, xhci->context_size);
    memset((void*)sc, 0, xhci->context_size);
    memset((void*)ep0c, 0, xhci->context_size);

    slot->sc = (xhci_slot_context_t*)device_context;
    device_context += xhci->context_size;
    for (int i = 0; i < XHCI_NUM_EPS; i++) {
        slot->epcs[i] = (xhci_endpoint_context_t*)device_context;
        device_context += xhci->context_size;
    }

    // Enable slot context and ep0 context
    XHCI_WRITE32(&icc->add_context_flags, XHCI_ICC_SLOT_FLAG | XHCI_ICC_EP_FLAG(0));

    // Setup slot context
    uint32_t route_string = xhci_get_route_string(xhci, hub_address, port);
    XHCI_SET_BITS32(&sc->sc0, SLOT_CTX_ROUTE_STRING_START, SLOT_CTX_ROUTE_STRING_BITS, route_string);
    XHCI_SET_BITS32(&sc->sc0, SLOT_CTX_SPEED_START, SLOT_CTX_SPEED_BITS, speed);
    XHCI_SET_BITS32(&sc->sc0, SLOT_CTX_CONTEXT_ENTRIES_START, SLOT_CTX_CONTEXT_ENTRIES_BITS, 1);
    XHCI_SET_BITS32(&sc->sc1, SLOT_CTX_ROOT_HUB_PORT_NUM_START, SLOT_CTX_ROOT_HUB_PORT_NUM_BITS, slot->rh_port);

    uint32_t mtt = 0;
    uint32_t tt_hub_slot_id = 0;
    uint32_t tt_port_number = 0;
    if (hub_address != 0 && (speed == USB_SPEED_LOW || speed == USB_SPEED_FULL)) {
        xhci_slot_t* hub_slot = &xhci->slots[hub_address];
        if (hub_slot->speed == USB_SPEED_HIGH) {
            mtt = XHCI_GET_BITS32(&slot->sc->sc0, SLOT_CTX_MTT_START, SLOT_CTX_MTT_BITS);
            tt_hub_slot_id = hub_address;
            tt_port_number = port;
        }
    }
    XHCI_SET_BITS32(&sc->sc0, SLOT_CTX_MTT_START, SLOT_CTX_MTT_BITS, mtt);
    XHCI_SET_BITS32(&sc->sc2, SLOT_CTX_TT_HUB_SLOT_ID_START, SLOT_CTX_TT_HUB_SLOT_ID_BITS, tt_hub_slot_id);
    XHCI_SET_BITS32(&sc->sc2, SLOT_CTX_TT_PORT_NUM_START, SLOT_CTX_TT_PORT_NUM_BITS, tt_port_number);

    // Setup endpoint context for ep0
    void* tr = (void*)slot->transfer_rings[0].start;
    uint64_t tr_dequeue = xhci_virt_to_phys(xhci, (mx_vaddr_t)tr);

    XHCI_SET_BITS32(&ep0c->epc1, EP_CTX_CERR_START, EP_CTX_CERR_BITS, 3); // ???
    XHCI_SET_BITS32(&ep0c->epc1, EP_CTX_EP_TYPE_START, EP_CTX_EP_TYPE_BITS, EP_CTX_EP_TYPE_CONTROL);
    XHCI_SET_BITS32(&ep0c->epc1, EP_CTX_MAX_PACKET_SIZE_START, EP_CTX_MAX_PACKET_SIZE_BITS, 8);
    XHCI_WRITE32(&ep0c->epc2, ((uint32_t)tr_dequeue & EP_CTX_TR_DEQUEUE_LO_MASK) | EP_CTX_DCS);
    XHCI_WRITE32(&ep0c->tr_dequeue_hi, (uint32_t)(tr_dequeue >> 32));
    XHCI_SET_BITS32(&ep0c->epc4, EP_CTX_AVG_TRB_LENGTH_START, EP_CTX_AVG_TRB_LENGTH_BITS, 8); // ???

    // install our device context for the slot
    XHCI_WRITE64(&xhci->dcbaa[slot_id], xhci_virt_to_phys(xhci, (mx_vaddr_t)slot->sc));
    // then send the address device command

    xhci_sync_command_t command;
    xhci_sync_command_init(&command);
    xhci_post_command(xhci, TRB_CMD_ADDRESS_DEVICE, xhci_virt_to_phys(xhci, (mx_vaddr_t)icc),
                      (slot_id << TRB_SLOT_ID_START), &command.context);
    int cc = xhci_sync_command_wait(&command);
    if (cc == TRB_CC_SUCCESS) {
        return NO_ERROR;
    } else {
        printf("xhci_address_device failed cc: %d\n", cc);
        return ERR_INTERNAL;
    }
}

#define NEXT_DESCRIPTOR(header) ((usb_descriptor_header_t*)((void*)header + header->bLength))

#define BOUNDS_CHECK(i, min, max) (i < min ? min : (i > max ? max : i))
#define LOG2(i) (31 - __builtin_clz(i))

static int compute_interval(usb_endpoint_descriptor_t* ep, usb_speed_t speed) {
    int ep_type = ep->bmAttributes & USB_ENDPOINT_TYPE_MASK;
    int interval = ep->bInterval;

    if (ep_type == USB_ENDPOINT_CONTROL || ep_type == USB_ENDPOINT_BULK) {
        if (speed == USB_SPEED_HIGH) {
            return LOG2(interval);
        } else {
            return 0;
        }
    }

    // now we deal with interrupt and isochronous endpoints
    // first make sure bInterval is in legal range
    if (ep_type == USB_ENDPOINT_INTERRUPT && (speed == USB_SPEED_LOW || speed == USB_SPEED_FULL)) {
        interval = BOUNDS_CHECK(interval, 1, 255);
    } else {
        interval = BOUNDS_CHECK(interval, 1, 16);
    }

    switch (speed) {
    case USB_SPEED_LOW:
        return LOG2(interval) + 3; // + 3 to convert 125us to 1ms
    case USB_SPEED_FULL:
        if (ep_type == USB_ENDPOINT_ISOCHRONOUS) {
            return (interval - 1) + 3;
        } else {
            return LOG2(interval) + 3;
        }
    case USB_SPEED_SUPER:
    case USB_SPEED_HIGH:
        return interval - 1;
    default:
        return 0;
    }
}

static mx_status_t xhci_configure_endpoints(xhci_t* xhci, uint32_t slot_id, usb_speed_t speed,
                                            usb_configuration_descriptor_t* config) {
    xhci_slot_t* slot = &xhci->slots[slot_id];

    xhci_input_control_context_t* icc = (xhci_input_control_context_t*)&xhci->input_context[0 * xhci->context_size];
    xhci_slot_context_t* sc = (xhci_slot_context_t*)&xhci->input_context[1 * xhci->context_size];
    memset((void*)icc, 0, xhci->context_size);
    memset((void*)sc, 0, xhci->context_size);

    // iterate through our descriptors
    usb_descriptor_header_t* header = NEXT_DESCRIPTOR(config);
    usb_descriptor_header_t* end = (usb_descriptor_header_t*)((void*)config + config->wTotalLength);

    bool do_endpoints = false;
    uint32_t add_context_flags = XHCI_ICC_SLOT_FLAG;
    uint32_t max_index = 0;

    while (header < end) {
        if (header->bDescriptorType == USB_DT_INTERFACE) {
            usb_interface_descriptor_t* intf = (usb_interface_descriptor_t*)header;
            // ignore alternate interfaces
            do_endpoints = intf->bAlternateSetting == 0;
        } else if (header->bDescriptorType == USB_DT_ENDPOINT && do_endpoints) {
            usb_endpoint_descriptor_t* ep = (usb_endpoint_descriptor_t*)header;

            uint32_t index = xhci_endpoint_index(ep->bEndpointAddress);
            if (max_index < index)
                max_index = index;
            uint32_t ep_type = ep->bmAttributes & USB_ENDPOINT_TYPE_MASK;
            uint32_t ep_index = ep_type;
            if ((ep->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_ENDPOINT_IN)
                ep_index += 4;

            // See Table 65 in XHCI spec
            int cerr = (ep_type == USB_ENDPOINT_ISOCHRONOUS ? 0 : 3);
            int avg_trb_length = (ep_type == USB_ENDPOINT_INTERRUPT ? 1024 : 3 * 1024);

            xhci_endpoint_context_t* epc = (xhci_endpoint_context_t*)&xhci->input_context[(index + 2) * xhci->context_size];
            memset((void*)epc, 0, xhci->context_size);
            // allocate a transfer ring for the endpoint
            mx_status_t status = xhci_transfer_ring_init(xhci, &slot->transfer_rings[index], TRANSFER_RING_SIZE);
            if (status < 0)
                return status;

            void* tr = (void*)slot->transfer_rings[index].start;
            uint64_t tr_dequeue = xhci_virt_to_phys(xhci, (mx_vaddr_t)tr);

            XHCI_SET_BITS32(&epc->epc0, EP_CTX_INTERVAL_START, EP_CTX_INTERVAL_BITS, compute_interval(ep, speed));
            XHCI_SET_BITS32(&epc->epc1, EP_CTX_CERR_START, EP_CTX_CERR_BITS, cerr);
            XHCI_SET_BITS32(&epc->epc1, EP_CTX_EP_TYPE_START, EP_CTX_EP_TYPE_BITS, ep_index);
            XHCI_SET_BITS32(&epc->epc1, EP_CTX_MAX_PACKET_SIZE_START, EP_CTX_MAX_PACKET_SIZE_BITS, ep->wMaxPacketSize);

            XHCI_WRITE32(&epc->epc2, ((uint32_t)tr_dequeue & EP_CTX_TR_DEQUEUE_LO_MASK) | EP_CTX_DCS);
            XHCI_WRITE32(&epc->tr_dequeue_hi, (uint32_t)(tr_dequeue >> 32));
            XHCI_SET_BITS32(&epc->epc4, EP_CTX_AVG_TRB_LENGTH_START, EP_CTX_AVG_TRB_LENGTH_BITS, avg_trb_length);

            add_context_flags |= XHCI_ICC_EP_FLAG(index);
        }
        header = NEXT_DESCRIPTOR(header);
    }

    XHCI_WRITE32(&icc->add_context_flags, add_context_flags);
    XHCI_WRITE32(&sc->sc0, XHCI_READ32(&slot->sc->sc0));
    XHCI_WRITE32(&sc->sc1, XHCI_READ32(&slot->sc->sc1));
    XHCI_WRITE32(&sc->sc2, XHCI_READ32(&slot->sc->sc2));
    XHCI_SET_BITS32(&sc->sc0, SLOT_CTX_CONTEXT_ENTRIES_START, SLOT_CTX_CONTEXT_ENTRIES_BITS, max_index + 1);

    xhci_sync_command_t command;
    xhci_sync_command_init(&command);
    xhci_post_command(xhci, TRB_CMD_CONFIGURE_EP, xhci_virt_to_phys(xhci, (mx_vaddr_t)icc),
                      (slot_id << TRB_SLOT_ID_START), &command.context);

    xhci_sync_command_wait(&command);

    return NO_ERROR;
}

static void xhci_disable_slot(xhci_t* xhci, uint32_t slot_id) {
    xhci_sync_command_t command;
    xhci_sync_command_init(&command);
    xhci_post_command(xhci, TRB_CMD_DISABLE_SLOT, 0, (slot_id << TRB_SLOT_ID_START), &command.context);
    xhci_sync_command_wait(&command);

    xprintf("cleaning up slot %d\n", slot_id);
    xhci_slot_t* slot = &xhci->slots[slot_id];
    xhci_free(xhci, (void*)slot->sc);
    memset(slot, 0, sizeof(*slot));
}

static mx_status_t xhci_handle_enumerate_device(xhci_t* xhci, uint32_t hub_address, uint32_t port,
                                                usb_speed_t speed) {
    xprintf("xhci_handle_enumerate_device\n");
    mx_status_t result = NO_ERROR;
    uint32_t slot_id = 0;

    xhci_sync_command_t command;
    xhci_sync_command_init(&command);
    xhci_post_command(xhci, TRB_CMD_ENABLE_SLOT, 0, 0, &command.context);
    int cc = xhci_sync_command_wait(&command);
    if (cc == TRB_CC_SUCCESS) {
        slot_id = xhci_sync_command_slot_id(&command);
    } else {
        printf("unable to get a slot\n");
        return ERR_NO_RESOURCES;
    }
    xhci_slot_t* slot = &xhci->slots[slot_id];
    memset(slot, 0, sizeof(*slot));

    result = xhci_address_device(xhci, slot_id, hub_address, port, speed);
    if (result != NO_ERROR) {
        goto disable_slot_exit;
    }
    slot->enabled = true;

    // read first 8 bytes of device descriptor to fetch ep0 max packet size
    result = xhci_get_descriptor(xhci, slot_id, USB_TYPE_STANDARD, USB_DT_DEVICE << 8, 0,
                                 xhci->device_descriptor, 8);
    if (result != 8) {
        printf("xhci_get_descriptor failed\n");
        goto disable_slot_exit;
    }

    int mps = xhci->device_descriptor->bMaxPacketSize0;
    // enforce correct max packet size for ep0
    switch (speed) {
        case USB_SPEED_LOW:
            mps = 8;
            break;
        case USB_SPEED_FULL:
            if (mps != 8 && mps != 16 && mps != 32 && mps != 64) {
                mps = 8;
            }
            break;
        case USB_SPEED_HIGH:
            mps = 64;
            break;
        case USB_SPEED_SUPER:
            // bMaxPacketSize0 is an exponent for superspeed devices
            mps = 1 << mps;
            break;
        default:
            break;
    }

    // update the max packet size in our device context
    xhci_input_control_context_t* icc = (xhci_input_control_context_t*)&xhci->input_context[0 * xhci->context_size];
    xhci_endpoint_context_t* ep0c = (xhci_endpoint_context_t*)&xhci->input_context[2 * xhci->context_size];
    memset((void*)icc, 0, xhci->context_size);
    memset((void*)ep0c, 0, xhci->context_size);

    XHCI_WRITE32(&icc->add_context_flags, XHCI_ICC_EP_FLAG(0));
    XHCI_SET_BITS32(&ep0c->epc1, EP_CTX_MAX_PACKET_SIZE_START, EP_CTX_MAX_PACKET_SIZE_BITS, mps);

    xhci_sync_command_init(&command);
    xhci_post_command(xhci, TRB_CMD_EVAL_CONTEXT, xhci_virt_to_phys(xhci, (mx_vaddr_t)icc),
                      (slot_id << TRB_SLOT_ID_START), &command.context);
    cc = xhci_sync_command_wait(&command);
    if (cc != TRB_CC_SUCCESS) {
        printf("TRB_CMD_EVAL_CONTEXT failed\n");
        result = ERR_INTERNAL;
        goto disable_slot_exit;
    }

    // read full device descriptor
    result = xhci_get_descriptor(xhci, slot_id, USB_TYPE_STANDARD, USB_DT_DEVICE << 8, 0,
                                 xhci->device_descriptor, sizeof(usb_device_descriptor_t));
    if (result != sizeof(usb_device_descriptor_t)) {
        printf("xhci_get_descriptor failed\n");
        goto disable_slot_exit;
    }

    int num_configurations = xhci->device_descriptor->bNumConfigurations;
    usb_configuration_descriptor_t** config_descriptors = calloc(num_configurations,
                                                                 sizeof(usb_configuration_descriptor_t*));
    if (!config_descriptors) {
        result = ERR_NO_MEMORY;
        goto disable_slot_exit;
    }

    for (int i = 0; i < num_configurations; i++) {
        // read configuration descriptor header
        int config_size = sizeof(usb_configuration_descriptor_t);
        result = xhci_get_descriptor(xhci, slot_id, USB_TYPE_STANDARD, USB_DT_CONFIG << 8, i,
                                     xhci->config_descriptor, config_size);
        if (result != config_size) {
            printf("xhci_get_descriptor failed\n");
            goto free_config_descriptors_exit;
        }

        config_size = letoh16(xhci->config_descriptor->wTotalLength);
        void* dma_buffer = xhci_malloc(xhci, config_size);
        if (!dma_buffer) {
            result = ERR_NO_MEMORY;
            goto free_config_descriptors_exit;
        }

        // read full configuration descriptor
        result = xhci_get_descriptor(xhci, slot_id, USB_TYPE_STANDARD, USB_DT_CONFIG << 8, i,
                                     dma_buffer, config_size);
        if (result != config_size) {
            printf("xhci_get_descriptor failed\n");
            xhci_free(xhci, dma_buffer);
            goto free_config_descriptors_exit;
        }
        usb_configuration_descriptor_t* config = malloc(config_size);
        if (!config) {
            result = ERR_NO_MEMORY;
            xhci_free(xhci, dma_buffer);
            goto free_config_descriptors_exit;
        }
        memcpy(config, dma_buffer, config_size);
        config_descriptors[i] = config;
        xhci_free(xhci, dma_buffer);
    }

    // Enable endpoints on the first configuration
    xhci_configure_endpoints(xhci, slot_id, speed, config_descriptors[0]);

    // set configuration
    result = xhci_control_request(xhci, slot_id, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                                  USB_REQ_SET_CONFIGURATION,
                                  config_descriptors[0]->bConfigurationValue, 0, (mx_paddr_t)NULL, 0);
    if (result < 0) {
        printf("set configuration failed\n");
        goto free_config_descriptors_exit;
    }

    usb_device_descriptor_t* device_descriptor = malloc(sizeof(usb_device_descriptor_t));
    if (!device_descriptor) {
        result = ERR_NO_MEMORY;
        goto free_config_descriptors_exit;
    }
    memcpy(device_descriptor, xhci->device_descriptor, sizeof(usb_device_descriptor_t));
    xhci_add_device(xhci, slot_id, hub_address, speed, device_descriptor, config_descriptors);
    return NO_ERROR;

free_config_descriptors_exit:
    for (int i = 0; i < num_configurations; i++) {
        if (config_descriptors[i]) {
            free(config_descriptors[i]);
        } else {
            break;
        }
    }
    free(config_descriptors);
disable_slot_exit:
    xhci_disable_slot(xhci, slot_id);
    printf("xhci_handle_enumerate_device failed %d\n", result);
    return result;
}

static mx_status_t xhci_stop_endpoint(xhci_t* xhci, uint32_t slot_id, int ep_id) {
    xhci_sync_command_t command;
    xhci_sync_command_init(&command);
    uint32_t control = (slot_id << TRB_SLOT_ID_START) | (ep_id << TRB_ENDPOINT_ID_START);
    xhci_post_command(xhci, TRB_CMD_STOP_ENDPOINT, 0, control, &command.context);
    int cc = xhci_sync_command_wait(&command);
    return cc == TRB_CC_SUCCESS ? NO_ERROR : -1;
}

static mx_status_t xhci_handle_disconnect_device(xhci_t* xhci, uint32_t hub_address, uint32_t port) {
    xprintf("xhci_handle_disconnect_device\n");
    xhci_slot_t* slot = NULL;
    uint32_t slot_id;

    for (slot_id = 1; slot_id <= xhci->max_slots; slot_id++) {
        xhci_slot_t* test_slot = &xhci->slots[slot_id];
        if (test_slot->hub_address == hub_address && test_slot->port == port) {
            slot = test_slot;
            break;
        }
    }
    if (!slot) {
        printf("slot not found in xhci_handle_disconnect_device\n");
        return ERR_NOT_FOUND;
    }

    slot->enabled = false;
    xhci_transfer_ring_t* transfer_rings = slot->transfer_rings;

    uint32_t drop_flags = 0;
    for (int i = 0; i < XHCI_NUM_EPS; i++) {
        xhci_transfer_ring_t* transfer_ring = &transfer_rings[i];
        if (transfer_ring->start) {
            xhci_stop_endpoint(xhci, slot_id, i);
            drop_flags |= XHCI_ICC_EP_FLAG(i);

            // complete pending requests
            xhci_transfer_context_t* context;
            while ((context = list_remove_head_type(&transfer_ring->pending_requests,
                                                    xhci_transfer_context_t, node)) != NULL) {
                context->callback(ERR_REMOTE_CLOSED, context->data);
            }
            // and any deferred requests
            xhci_process_deferred_txns(xhci, transfer_ring, true);
        }
    }

    xhci_remove_device(xhci, slot_id);

    xhci_input_control_context_t* icc = (xhci_input_control_context_t*)&xhci->input_context[0 * xhci->context_size];
    memset((void*)icc, 0, xhci->context_size);
    XHCI_WRITE32(&icc->drop_context_flags, drop_flags);

    xhci_sync_command_t command;
    xhci_sync_command_init(&command);
    xhci_post_command(xhci, TRB_CMD_CONFIGURE_EP, xhci_virt_to_phys(xhci, (mx_vaddr_t)icc),
                      (slot_id << TRB_SLOT_ID_START), &command.context);
    int cc = xhci_sync_command_wait(&command);
    if (cc != TRB_CC_SUCCESS) {
        printf("TRB_CMD_EVAL_CONTEXT failed\n");
    }

    xhci_disable_slot(xhci, slot_id);

    return NO_ERROR;
}

static int xhci_device_thread(void* arg) {
    xhci_t* xhci = (xhci_t*)arg;

    xhci->input_context = (uint8_t*)xhci_memalign(xhci, 64, xhci->context_size * (XHCI_NUM_EPS + 2));
    if (!xhci->input_context) {
        printf("out of DMA memory!\n");
        return ERR_NO_MEMORY;
    }
    xhci->device_descriptor = (usb_device_descriptor_t*)xhci_malloc(xhci, sizeof(usb_device_descriptor_t));
    if (!xhci->device_descriptor) {
        printf("out of DMA memory!\n");
        xhci_free(xhci, xhci->input_context);
        return ERR_NO_MEMORY;
    }
    xhci->config_descriptor = (usb_configuration_descriptor_t*)xhci_malloc(xhci, sizeof(usb_configuration_descriptor_t));
    if (!xhci->config_descriptor) {
        printf("out of DMA memory!\n");
        xhci_free(xhci, xhci->input_context);
        xhci_free(xhci, xhci->device_descriptor);
        return ERR_NO_MEMORY;
    }

    while (1) {
        xprintf("xhci_device_thread top of loop\n");
        // wait for a device to enumerate
        completion_wait(&xhci->command_queue_completion, MX_TIME_INFINITE);

        mtx_lock(&xhci->command_queue_mutex);
        list_node_t* node = list_remove_head(&xhci->command_queue);
        xhci_device_command_t* command = (node ? containerof(node, xhci_device_command_t, node) : NULL);
        if (list_is_empty(&xhci->command_queue)) {
            completion_reset(&xhci->command_queue_completion);
        }
        mtx_unlock(&xhci->command_queue_mutex);

        if (!command) {
            printf("ERROR: command_queue_completion was signalled, but no command was found");
            break;
        }

        switch (command->command) {
        case ENUMERATE_DEVICE:
            xhci_handle_enumerate_device(xhci, command->hub_address, command->port, command->speed);
            break;
        case DISCONNECT_DEVICE:
            xhci_handle_disconnect_device(xhci, command->hub_address, command->port);
            break;
        case RH_PORT_CONNECTED:
            xhci_handle_rh_port_connected(xhci, command->port);
            break;
        }
    }

    // free our DMA buffers
    xhci_free(xhci, xhci->input_context);
    xhci_free(xhci, xhci->device_descriptor);
    xhci_free(xhci, xhci->config_descriptor);
    xhci->input_context = NULL;
    xhci->device_descriptor = NULL;
    xhci->config_descriptor = NULL;

    return 0;
}

void xhci_start_device_thread(xhci_t* xhci) {
    thrd_create_with_name(&xhci->device_thread, xhci_device_thread, xhci, "xhci_device_thread");
}

static mx_status_t xhci_queue_command(xhci_t* xhci, int command, uint32_t hub_address,
                                      uint32_t port, usb_speed_t speed) {
    xhci_device_command_t* device_command = calloc(1, sizeof(xhci_device_command_t));
    if (!device_command) {
        printf("out of memory\n");
        return ERR_NO_MEMORY;
    }
    device_command->command = command;
    device_command->hub_address = hub_address;
    device_command->port = port;
    device_command->speed = speed;

    mtx_lock(&xhci->command_queue_mutex);
    list_add_tail(&xhci->command_queue, &device_command->node);
    completion_signal(&xhci->command_queue_completion);
    mtx_unlock(&xhci->command_queue_mutex);

    return NO_ERROR;
}

mx_status_t xhci_enumerate_device(xhci_t* xhci, uint32_t hub_address, uint32_t port,
                                  usb_speed_t speed) {
    return xhci_queue_command(xhci, ENUMERATE_DEVICE, hub_address, port, speed);
}

mx_status_t xhci_device_disconnected(xhci_t* xhci, uint32_t hub_address, uint32_t port) {
    xprintf("xhci_device_disconnected %d %d\n", hub_address, port);
    mtx_lock(&xhci->command_queue_mutex);
    // check pending device list first
    xhci_device_command_t* command;
    list_for_every_entry (&xhci->command_queue, command, xhci_device_command_t, node) {
        if (command->command == ENUMERATE_DEVICE && command->hub_address == hub_address &&
            command->port == port) {
            xprintf("found on pending list\n");
            list_delete(&command->node);
            mtx_unlock(&xhci->command_queue_mutex);
            return NO_ERROR;
        }
    }
    mtx_unlock(&xhci->command_queue_mutex);

    return xhci_queue_command(xhci, DISCONNECT_DEVICE, hub_address, port, USB_SPEED_UNDEFINED);
}

mx_status_t xhci_rh_port_connected(xhci_t* xhci, uint32_t port) {
    return xhci_queue_command(xhci, RH_PORT_CONNECTED, 0, port, USB_SPEED_UNDEFINED);
}

mx_status_t xhci_configure_hub(xhci_t* xhci, int slot_id, usb_speed_t speed, usb_hub_descriptor_t* descriptor) {
    xprintf("xhci_configure_hub slot_id: %d speed: %d\n", slot_id, speed);
    xhci_slot_t* slot = &xhci->slots[slot_id];
    uint8_t* input_context = (uint8_t*)xhci_memalign(xhci, 64, xhci->context_size * 2);
    if (!input_context) {
        printf("out of DMA memory!\n");
        return ERR_NO_MEMORY;
    }

    uint32_t num_ports = descriptor->bNbrPorts;
    uint32_t ttt = 0;
    if (speed == USB_SPEED_HIGH) {
        ttt = (descriptor->wHubCharacteristics >> 5) & 3;
    }

    xhci_input_control_context_t* icc = (xhci_input_control_context_t*)&input_context[0 * xhci->context_size];
    xhci_slot_context_t* sc = (xhci_slot_context_t*)&input_context[1 * xhci->context_size];
    memset((void*)icc, 0, xhci->context_size);
    memset((void*)sc, 0, xhci->context_size);

    XHCI_WRITE32(&icc->add_context_flags, XHCI_ICC_SLOT_FLAG);
    XHCI_WRITE32(&sc->sc0, XHCI_READ32(&slot->sc->sc0) | SLOT_CTX_HUB);
    XHCI_WRITE32(&sc->sc1, XHCI_READ32(&slot->sc->sc1));
    XHCI_WRITE32(&sc->sc2, XHCI_READ32(&slot->sc->sc2));

    XHCI_SET_BITS32(&sc->sc1, SLOT_CTX_ROOT_NUM_PORTS_START, SLOT_CTX_ROOT_NUM_PORTS_BITS, num_ports);
    XHCI_SET_BITS32(&sc->sc2, SLOT_CTX_TTT_START, SLOT_CTX_TTT_BITS, ttt);

    xhci_sync_command_t command;
    xhci_sync_command_init(&command);
    xhci_post_command(xhci, TRB_CMD_EVAL_CONTEXT, xhci_virt_to_phys(xhci, (mx_vaddr_t)icc),
                      (slot_id << TRB_SLOT_ID_START), &command.context);
    int cc = xhci_sync_command_wait(&command);

    xhci_free(xhci, input_context);

    if (cc != TRB_CC_SUCCESS) {
        printf("TRB_CMD_EVAL_CONTEXT failed\n");
        return -1;
    }

    if (speed == USB_SPEED_SUPER) {
        // compute hub depth
        int depth = 0;
        while (slot->hub_address != 0) {
            depth++;
            slot = &xhci->slots[slot->hub_address];
        }

        xprintf("USB_HUB_SET_DEPTH %d\n", depth);
        mx_status_t result = xhci_control_request(xhci, slot_id,
                                      USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_DEVICE,
                                      USB_HUB_SET_DEPTH, depth, 0, (mx_paddr_t)NULL, 0);
        if (result < 0) {
            printf("USB_HUB_SET_DEPTH failed\n");
        }
    }

    return NO_ERROR;
}
