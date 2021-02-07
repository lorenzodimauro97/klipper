// Generic handling of serial over CAN support
//
// Copyright (C) 2019 Eug Krashtan <eug.krashtan@gmail.com>
// Copyright (C) 2020 Pontus Borg <glpontus@gmail.com>
// Copyright (C) 2021  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <string.h> // memcpy
#include "canbus.h" // canbus_set_uuid
#include "command.h" // DECL_CONSTANT
#include "sched.h" // sched_wake_task

static uint32_t canbus_assigned_id;
static uint8_t canbus_uuid[CANBUS_UUID_LEN];


/****************************************************************
 * Data transmission over CAN
 ****************************************************************/

static struct task_wake canbus_tx_wake;
static uint8_t transmit_buf[96], transmit_pos, transmit_max;

void
canbus_notify_tx(void)
{
    sched_wake_task(&canbus_tx_wake);
}

void
canbus_tx_task(void)
{
    if (!sched_check_wake(&canbus_tx_wake))
        return;
    uint32_t id = canbus_assigned_id;
    if (!id) {
        transmit_pos = transmit_max = 0;
        return;
    }
    uint32_t tpos = transmit_pos, tmax = transmit_max;
    for (;;) {
        int avail = tmax - tpos, now = avail > 8 ? 8 : avail;
        if (avail <= 0)
            break;
        int ret = canbus_send(id + 1, now, &transmit_buf[tpos]);
        if (ret <= 0)
            break;
        tpos += now;
    }
    transmit_pos = tpos;
}
DECL_TASK(canbus_tx_task);

// Encode and transmit a "response" message
void
console_sendf(const struct command_encoder *ce, va_list args)
{
    // Verify space for message
    uint32_t tpos = transmit_pos, tmax = transmit_max;
    if (tpos >= tmax)
        transmit_pos = transmit_max = tpos = tmax = 0;
    uint32_t max_size = ce->max_size;
    if (tmax + max_size > sizeof(transmit_buf)) {
        if (tmax + max_size - tpos > sizeof(transmit_buf))
            // Not enough space for message
            return;
        // Move buffer
        tmax -= tpos;
        memmove(&transmit_buf[0], &transmit_buf[tpos], tmax);
        transmit_pos = tpos = 0;
        transmit_max = tmax;
    }

    // Generate message
    uint32_t msglen = command_encode_and_frame(&transmit_buf[tmax], ce, args);

    // Start message transmit
    transmit_max = tmax + msglen;
    canbus_notify_tx();
}


/****************************************************************
 * CAN "admin" command handling
 ****************************************************************/

// Available commands and responses
#define CANBUS_CMD_QUERY_UNASSIGNED 0
#define CANBUS_CMD_QUERY 1
#define CANBUS_CMD_SET_CANID 2
#define CANBUS_CMD_REBOOT 3
#define CANBUS_RESP_NEED_CANID 32
#define CANBUS_RESP_HAVE_CANID 33

// Helper to verify a UUID in a command matches this chip's UUID
static int
can_check_uuid(uint32_t id, uint32_t len, uint8_t *data)
{
    return len >= 7 && memcmp(&data[1], canbus_uuid, sizeof(canbus_uuid)) == 0;
}

// Helpers to encode/decode a CAN id to a 1-byte identifier
static int
can_encode_id(void)
{
    if (!canbus_assigned_id)
        return 0;
    return (canbus_assigned_id - 0x100) >> 1;
}
static uint32_t
can_decode_id(int encoded_id)
{
    return (encoded_id << 1) + 0x100;
}

// Helper to send this chip's UUID
static void
can_send_uuid(void)
{
    uint8_t data[8];
    if (canbus_assigned_id)
        data[0] = CANBUS_RESP_HAVE_CANID;
    else
        data[0] = CANBUS_RESP_NEED_CANID;
    memcpy(&data[1], canbus_uuid, sizeof(canbus_uuid));
    data[7] = can_encode_id();
    // Send with retry
    for (;;) {
        int ret = canbus_send(CANBUS_ID_ADMIN_RESP, 8, data);
        if (ret >= 0)
            return;
    }
}

static void
can_process_query_unassigned(uint32_t id, uint32_t len, uint8_t *data)
{
    if (!canbus_assigned_id)
        can_send_uuid();
}

static void
can_process_query(uint32_t id, uint32_t len, uint8_t *data)
{
    if (can_check_uuid(id, len, data))
        can_send_uuid();
}

static void
can_process_set_canid(uint32_t id, uint32_t len, uint8_t *data)
{
    if (len < 8)
        return;
    uint32_t newid = can_decode_id(data[7]);
    if (can_check_uuid(id, len, data)) {
        if (newid != canbus_assigned_id) {
            canbus_assigned_id = newid;
            canbus_set_filter(canbus_assigned_id);
        }
        can_send_uuid();
    } else if (newid == canbus_assigned_id) {
        canbus_assigned_id = 0;
        canbus_set_filter(canbus_assigned_id);
        can_send_uuid();
        shutdown("Another CAN node assigned this ID");
    }
}

static void
can_process_reboot(uint32_t id, uint32_t len, uint8_t *data)
{
    if (can_check_uuid(id, len, data))
        canbus_reboot();
}

// Handle an "admin" command
static void
can_process(uint32_t id, uint32_t len, uint8_t *data)
{
    if (!len)
        return;
    switch (data[0]) {
    case CANBUS_CMD_QUERY_UNASSIGNED:
        can_process_query_unassigned(id, len, data);
        break;
    case CANBUS_CMD_QUERY:
        can_process_query(id, len, data);
        break;
    case CANBUS_CMD_SET_CANID:
        can_process_set_canid(id, len, data);
        break;
    case CANBUS_CMD_REBOOT:
        can_process_reboot(id, len, data);
        break;
    }
}


/****************************************************************
 * CAN packet reading
 ****************************************************************/

static struct task_wake canbus_rx_wake;

void
canbus_notify_rx(void)
{
    sched_wake_task(&canbus_rx_wake);
}

static uint8_t receive_buf[192], receive_pos;
DECL_CONSTANT("RECEIVE_WINDOW", ARRAY_SIZE(receive_buf));

static void
can_process_data(uint32_t id, uint32_t len, uint8_t *data)
{
    int rpos = receive_pos;
    if (len > sizeof(receive_buf) - rpos)
        len = sizeof(receive_buf) - rpos;
    memcpy(&receive_buf[rpos], data, len);
    receive_pos = rpos + len;
}

void
canbus_rx_task(void)
{
    if (!sched_check_wake(&canbus_rx_wake))
        return;

    // Read any pending CAN packets
    for (;;) {
        uint8_t data[8];
        uint32_t id;
        int ret = canbus_read(&id, data);
        if (ret < 0)
            break;
        if (id && id == canbus_assigned_id)
            can_process_data(id, ret, data);
        else if (id == CANBUS_ID_ADMIN)
            can_process(id, ret, data);
    }

    // Check for a complete message block and process it
    uint_fast8_t rpos = receive_pos, pop_count;
    int ret = command_find_and_dispatch(receive_buf, rpos, &pop_count);
    if (ret) {
        // Move buffer
        int needcopy = rpos - pop_count;
        if (needcopy) {
            memmove(receive_buf, &receive_buf[pop_count], needcopy);
            canbus_notify_rx();
        }
        rpos = needcopy;
    }
    receive_pos = rpos;
}
DECL_TASK(canbus_rx_task);


/****************************************************************
 * Setup and shutdown
 ****************************************************************/

void
canbus_set_uuid(void *uuid)
{
    memcpy(canbus_uuid, uuid, sizeof(canbus_uuid));
    canbus_notify_rx();

    // Send initial message
    can_send_uuid();
}

void
canbus_shutdown(void)
{
    canbus_notify_tx();
    canbus_notify_rx();
}
DECL_SHUTDOWN(canbus_shutdown);
