/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "adc.h"
#include "board.h"
#include "charge_manager.h"
#include "chipset.h"
#include "common.h"
#include "console.h"
#include "crc.h"
#include "ec_commands.h"
#include "gpio.h"
#include "hooks.h"
#include "host_command.h"
#include "registers.h"
#include "system.h"
#include "task.h"
#include "timer.h"
#include "util.h"
#include "usb_pd.h"
#include "usb_pd_config.h"

#ifdef CONFIG_COMMON_RUNTIME
#define CPRINTF(format, args...) cprintf(CC_USBPD, format, ## args)
#define CPRINTS(format, args...) cprints(CC_USBPD, format, ## args)

/*
 * Debug log level - higher number == more log
 *   Level 0: Log state transitions
 *   Level 1: Level 0, plus packet info
 *   Level 2: Level 1, plus ping packet and packet dump on error
 *
 * Note that higher log level causes timing changes and thus may affect
 * performance.
 */
static int debug_level;
#else
#define CPRINTF(format, args...)
const int debug_level;
#endif

/* Encode 5 bits using Biphase Mark Coding */
#define BMC(x)   ((x &  1 ? 0x001 : 0x3FF) \
		^ (x &  2 ? 0x004 : 0x3FC) \
		^ (x &  4 ? 0x010 : 0x3F0) \
		^ (x &  8 ? 0x040 : 0x3C0) \
		^ (x & 16 ? 0x100 : 0x300))

/* 4b/5b + Bimark Phase encoding */
static const uint16_t bmc4b5b[] = {
/* 0 = 0000 */ BMC(0x1E) /* 11110 */,
/* 1 = 0001 */ BMC(0x09) /* 01001 */,
/* 2 = 0010 */ BMC(0x14) /* 10100 */,
/* 3 = 0011 */ BMC(0x15) /* 10101 */,
/* 4 = 0100 */ BMC(0x0A) /* 01010 */,
/* 5 = 0101 */ BMC(0x0B) /* 01011 */,
/* 6 = 0110 */ BMC(0x0E) /* 01110 */,
/* 7 = 0111 */ BMC(0x0F) /* 01111 */,
/* 8 = 1000 */ BMC(0x12) /* 10010 */,
/* 9 = 1001 */ BMC(0x13) /* 10011 */,
/* A = 1010 */ BMC(0x16) /* 10110 */,
/* B = 1011 */ BMC(0x17) /* 10111 */,
/* C = 1100 */ BMC(0x1A) /* 11010 */,
/* D = 1101 */ BMC(0x1B) /* 11011 */,
/* E = 1110 */ BMC(0x1C) /* 11100 */,
/* F = 1111 */ BMC(0x1D) /* 11101 */,
/* Sync-1      K-code       11000 Startsynch #1 */
/* Sync-2      K-code       10001 Startsynch #2 */
/* RST-1       K-code       00111 Hard Reset #1 */
/* RST-2       K-code       11001 Hard Reset #2 */
/* EOP         K-code       01101 EOP End Of Packet */
/* Reserved    Error        00000 */
/* Reserved    Error        00001 */
/* Reserved    Error        00010 */
/* Reserved    Error        00011 */
/* Reserved    Error        00100 */
/* Reserved    Error        00101 */
/* Reserved    Error        00110 */
/* Reserved    Error        01000 */
/* Reserved    Error        01100 */
/* Reserved    Error        10000 */
/* Reserved    Error        11111 */
};

static const uint8_t dec4b5b[] = {
/* Error    */ 0x10 /* 00000 */,
/* Error    */ 0x10 /* 00001 */,
/* Error    */ 0x10 /* 00010 */,
/* Error    */ 0x10 /* 00011 */,
/* Error    */ 0x10 /* 00100 */,
/* Error    */ 0x10 /* 00101 */,
/* Error    */ 0x10 /* 00110 */,
/* RST-1    */ 0x13 /* 00111 K-code: Hard Reset #1 */,
/* Error    */ 0x10 /* 01000 */,
/* 1 = 0001 */ 0x01 /* 01001 */,
/* 4 = 0100 */ 0x04 /* 01010 */,
/* 5 = 0101 */ 0x05 /* 01011 */,
/* Error    */ 0x10 /* 01100 */,
/* EOP      */ 0x15 /* 01101 K-code: EOP End Of Packet */,
/* 6 = 0110 */ 0x06 /* 01110 */,
/* 7 = 0111 */ 0x07 /* 01111 */,
/* Error    */ 0x10 /* 10000 */,
/* Sync-2   */ 0x12 /* 10001 K-code: Startsynch #2 */,
/* 8 = 1000 */ 0x08 /* 10010 */,
/* 9 = 1001 */ 0x09 /* 10011 */,
/* 2 = 0010 */ 0x02 /* 10100 */,
/* 3 = 0011 */ 0x03 /* 10101 */,
/* A = 1010 */ 0x0A /* 10110 */,
/* B = 1011 */ 0x0B /* 10111 */,
/* Sync-1   */ 0x11 /* 11000 K-code: Startsynch #1 */,
/* RST-2    */ 0x14 /* 11001 K-code: Hard Reset #2 */,
/* C = 1100 */ 0x0C /* 11010 */,
/* D = 1101 */ 0x0D /* 11011 */,
/* E = 1110 */ 0x0E /* 11100 */,
/* F = 1111 */ 0x0F /* 11101 */,
/* 0 = 0000 */ 0x00 /* 11110 */,
/* Error    */ 0x10 /* 11111 */,
};

/* Start of Packet sequence : three Sync-1 K-codes, then one Sync-2 K-code */
#define PD_SOP (PD_SYNC1 | (PD_SYNC1<<5) | (PD_SYNC1<<10) | (PD_SYNC2<<15))
#define PD_SOP_PRIME	(PD_SYNC1 | (PD_SYNC1<<5) | \
			(PD_SYNC3<<10) | (PD_SYNC3<<15))
#define PD_SOP_PRIME_PRIME	(PD_SYNC1 | (PD_SYNC3<<5) | \
				(PD_SYNC1<<10) | (PD_SYNC3<<15))

/* Hard Reset sequence : three RST-1 K-codes, then one RST-2 K-code */
#define PD_HARD_RESET (PD_RST1 | (PD_RST1 << 5) |\
		      (PD_RST1 << 10) | (PD_RST2 << 15))

/* Polarity is based 'DFP Perspective' (see table USB Type-C Cable and Connector
 * Specification)
 *
 * CC1    CC2    STATE             POSITION
 * ----------------------------------------
 * open   open   NC                N/A
 * Rd     open   UFP attached      1
 * open   Rd     UFP attached      2
 * open   Ra     pwr cable no UFP  1
 * Ra     open   pwr cable no UFP  2
 * Rd     Ra     pwr cable & UFP   1
 * Ra     Rd     pwr cable & UFP   2
 * Rd     Rd     dbg accessory     N/A
 * Ra     Ra     audio accessory   N/A
 *
 * Note, V(Rd) > V(Ra)
 * TODO(crosbug.com/p/31197): Need to identify necessary polarity switching for
 *                            debug dongle.
 *
 */
#ifndef PD_SRC_RD_THRESHOLD
#define PD_SRC_RD_THRESHOLD  200 /* mV */
#endif
#define CC_RA(cc)  (cc < PD_SRC_RD_THRESHOLD)
#define CC_RD(cc) ((cc > PD_SRC_RD_THRESHOLD) && (cc < PD_SRC_VNC))
#define GET_POLARITY(cc1, cc2) (CC_RD(cc2) || CC_RA(cc1))
#define IS_CABLE(cc1, cc2)     (CC_RD(cc1) || CC_RD(cc2))

/*
 * Type C power source charge current limits are identified by their cc
 * voltage (set by selecting the proper Rd resistor). Any voltage below
 * TYPE_C_SRC_500_THRESHOLD will not be identified as a type C charger.
 */
#define TYPE_C_SRC_500_THRESHOLD	PD_SRC_RD_THRESHOLD
#define TYPE_C_SRC_1500_THRESHOLD	660  /* mV */
#define TYPE_C_SRC_3000_THRESHOLD	1230 /* mV */

/* Type C supply voltage (mV) */
#define TYPE_C_VOLTAGE	5000 /* mV */

/* PD counter definitions */
#define PD_MESSAGE_ID_COUNT 7
#define PD_RETRY_COUNT 2
#define PD_HARD_RESET_COUNT 2
#define PD_CAPS_COUNT 50

/* Timers */
#define PD_T_SEND_SOURCE_CAP  (100*MSEC) /* between 100ms and 200ms */
#define PD_T_SINK_WAIT_CAP    (240*MSEC) /* between 210ms and 250ms */
#define PD_T_SINK_TRANSITION   (35*MSEC) /* between 20ms and 35ms */
#define PD_T_SOURCE_ACTIVITY   (45*MSEC) /* between 40ms and 50ms */
#define PD_T_SENDER_RESPONSE   (30*MSEC) /* between 24ms and 30ms */
#define PD_T_PS_TRANSITION    (500*MSEC) /* between 450ms and 550ms */
#define PD_T_PS_SOURCE_ON     (480*MSEC) /* between 390ms and 480ms */
#define PD_T_PS_SOURCE_OFF    (920*MSEC) /* between 750ms and 920ms */
#define PD_T_DRP_HOLD         (120*MSEC) /* between 100ms and 150ms */
#define PD_T_DRP_LOCK         (120*MSEC) /* between 100ms and 150ms */
/* DRP_SNK + DRP_SRC must be between 50ms and 100ms with 30%-70% duty cycle */
#define PD_T_DRP_SNK           (40*MSEC) /* toggle time for sink DRP */
#define PD_T_DRP_SRC           (30*MSEC) /* toggle time for source DRP */
#define PD_T_DEBOUNCE          (15*MSEC) /* between 10ms and 20ms */
#define PD_T_SINK_ADJ          (55*MSEC) /* between PD_T_DEBOUNCE and 60ms */
#define PD_T_SRC_RECOVER      (760*MSEC) /* between 660ms and 1000ms */
#define PD_T_SRC_RECOVER_MAX (1000*MSEC) /* 1000ms */
#define PD_T_SRC_TURN_ON      (275*MSEC) /* 275ms */
#define PD_T_SAFE_0V          (650*MSEC) /* 650ms */

/* from USB Type-C Specification Table 5-1 */
#define PD_T_AME (1*SECOND) /* timeout from UFP attach to Alt Mode Entry */

/* Port role at startup */
#ifdef CONFIG_USB_PD_DUAL_ROLE
#define PD_ROLE_DEFAULT PD_ROLE_SINK
#else
#define PD_ROLE_DEFAULT PD_ROLE_SOURCE
#endif

enum vdm_states {
	VDM_STATE_ERR_BUSY = -3,
	VDM_STATE_ERR_SEND = -2,
	VDM_STATE_ERR_TMOUT = -1,
	VDM_STATE_DONE = 0,
	/* Anything >0 represents an active state */
	VDM_STATE_READY = 1,
	VDM_STATE_BUSY = 2,
};

enum pd_request_types {
	PD_REQUEST_MIN,
	PD_REQUEST_MAX,
};

#ifdef CONFIG_USB_PD_DUAL_ROLE
/* Port dual-role state */
enum pd_dual_role_states drp_state = PD_DRP_TOGGLE_OFF;

/* Last received source cap */
static uint32_t pd_src_caps[PD_PORT_COUNT][PDO_MAX_OBJECTS];
static int pd_src_cap_cnt[PD_PORT_COUNT];
#endif

#define PD_FLAGS_PING_ENABLED      (1 << 0) /* SRC_READY pings enabled */
#define PD_FLAGS_PARTNER_DR_POWER  (1 << 1) /* port partner is dualrole power */
#define PD_FLAGS_PARTNER_DR_DATA   (1 << 2) /* port partner is dualrole data */
#define PD_FLAGS_DATA_SWAPPED      (1 << 3) /* data swap complete */
#define PD_FLAGS_SNK_CAP_RECVD     (1 << 4) /* sink capabilities received */
#define PD_FLAGS_GET_SNK_CAP_SENT  (1 << 5) /* get sink cap sent */
#define PD_FLAGS_NEW_CONTRACT      (1 << 6) /* new power contract established */
#define PD_FLAGS_EXPLICIT_CONTRACT (1 << 7) /* explicit pwr contract in place */
/* Flags to clear on a disconnect */
#define PD_FLAGS_RESET_ON_DISCONNECT_MASK (PD_FLAGS_PARTNER_DR_POWER | \
					   PD_FLAGS_PARTNER_DR_DATA | \
					   PD_FLAGS_DATA_SWAPPED | \
					   PD_FLAGS_SNK_CAP_RECVD | \
					   PD_FLAGS_GET_SNK_CAP_SENT | \
					   PD_FLAGS_NEW_CONTRACT | \
					   PD_FLAGS_EXPLICIT_CONTRACT)

static struct pd_protocol {
	/* current port power role (SOURCE or SINK) */
	uint8_t power_role;
	/* current port data role (DFP or UFP) */
	uint8_t data_role;
	/* port flags, see PD_FLAGS_* */
	uint8_t flags;
	/* 3-bit rolling message ID counter */
	uint8_t msg_id;
	/* Port polarity : 0 => CC1 is CC line, 1 => CC2 is CC line */
	uint8_t polarity;
	/* PD state for port */
	enum pd_states task_state;
	/* PD state when we run state handler the last time */
	enum pd_states last_state;
	/* The state to go to after timeout */
	enum pd_states timeout_state;
	/* Timeout for the current state. Set to 0 for no timeout. */
	uint64_t timeout;
	/* Time for source recovery after hard reset */
	uint64_t src_recover;

	/* last requested voltage PDO index */
	int requested_idx;
#ifdef CONFIG_USB_PD_DUAL_ROLE
	/* Current limit / voltage based on the last request message */
	uint32_t curr_limit;
	uint32_t supply_voltage;
	/* Signal charging update that affects the port */
	int new_power_request;
#endif

	/* PD state for Vendor Defined Messages */
	enum vdm_states vdm_state;
	/* next Vendor Defined Message to send */
	uint32_t vdo_data[VDO_MAX_SIZE];
	uint8_t vdo_count;

	/* Attached ChromeOS device id & RW hash */
	uint16_t dev_id;
	uint32_t dev_rw_hash[PD_RW_HASH_SIZE/4];
} pd[PD_PORT_COUNT];

/*
 * PD communication enabled flag. When false, PD state machine still
 * detects source/sink connection and disconnection, and will still
 * provide VBUS, but never sends any PD communication.
 */
static uint8_t pd_comm_enabled = CONFIG_USB_PD_COMM_ENABLED;

struct mutex pd_crc_lock;

/*
 * 4 entry rw_hash table of type-C devices that AP has firmware updates for.
 */
#ifdef CONFIG_COMMON_RUNTIME
#define RW_HASH_ENTRIES 4
static struct ec_params_usb_pd_rw_hash_entry rw_hash_table[RW_HASH_ENTRIES];
#endif

static inline void set_state_timeout(int port,
				     uint64_t timeout,
				     enum pd_states timeout_state)
{
	pd[port].timeout = timeout;
	pd[port].timeout_state = timeout_state;
}

/* Return flag for pd state is connected */
int pd_is_connected(int port)
{
	if (pd[port].task_state == PD_STATE_DISABLED)
		return 0;

#ifdef CONFIG_USB_PD_DUAL_ROLE
	/* Check if sink is connected */
	if (pd[port].power_role == PD_ROLE_SINK)
		return pd[port].task_state != PD_STATE_SNK_DISCONNECTED;
#endif
	/* Must be a source */
	return pd[port].task_state != PD_STATE_SRC_DISCONNECTED;
}

static inline void set_state(int port, enum pd_states next_state)
{
	enum pd_states last_state = pd[port].task_state;
#ifdef CONFIG_LOW_POWER_IDLE
	int i;
#endif

	set_state_timeout(port, 0, 0);
	pd[port].task_state = next_state;

	if (last_state == next_state)
		return;
#ifdef CONFIG_USB_PD_DUAL_ROLE
	/* Ignore dual-role toggling between sink and source */
	if ((last_state == PD_STATE_SNK_DISCONNECTED &&
	     next_state == PD_STATE_SRC_DISCONNECTED) ||
	    (last_state == PD_STATE_SRC_DISCONNECTED &&
	     next_state == PD_STATE_SNK_DISCONNECTED))
		return;
#endif

#ifdef CONFIG_USB_PD_DUAL_ROLE
	if (next_state == PD_STATE_SRC_DISCONNECTED ||
	    next_state == PD_STATE_SNK_DISCONNECTED) {
#else
	if (next_state == PD_STATE_SRC_DISCONNECTED) {
#endif
		pd[port].dev_id = 0;
		pd[port].flags &= ~PD_FLAGS_RESET_ON_DISCONNECT_MASK;
		pd_exit_mode(port, NULL);
#ifdef CONFIG_USBC_SS_MUX
		board_set_usb_mux(port, TYPEC_MUX_NONE,
				  pd[port].polarity);
#endif
#ifdef CONFIG_USBC_VCONN
		pd_set_vconn(port, pd[port].polarity, 0);
#endif
	}

#ifdef CONFIG_LOW_POWER_IDLE
	/* If any PD port is connected, then disable deep sleep */
	for (i = 0; i < PD_PORT_COUNT; i++) {
		if (pd_is_connected(i))
			break;
	}
	if (i == PD_PORT_COUNT)
		enable_sleep(SLEEP_MASK_USB_PD);
	else
		disable_sleep(SLEEP_MASK_USB_PD);
#endif

	CPRINTF("C%d st%d\n", port, next_state);
}

/* increment message ID counter */
static void inc_id(int port)
{
	pd[port].msg_id = (pd[port].msg_id + 1) & PD_MESSAGE_ID_COUNT;
}

static inline int encode_short(int port, int off, uint16_t val16)
{
	off = pd_write_sym(port, off, bmc4b5b[(val16 >> 0) & 0xF]);
	off = pd_write_sym(port, off, bmc4b5b[(val16 >> 4) & 0xF]);
	off = pd_write_sym(port, off, bmc4b5b[(val16 >> 8) & 0xF]);
	return pd_write_sym(port, off, bmc4b5b[(val16 >> 12) & 0xF]);
}

int encode_word(int port, int off, uint32_t val32)
{
	off = encode_short(port, off, (val32 >> 0) & 0xFFFF);
	return encode_short(port, off, (val32 >> 16) & 0xFFFF);
}

/* prepare a 4b/5b-encoded PD message to send */
int prepare_message(int port, uint16_t header, uint8_t cnt,
		   const uint32_t *data)
{
	int off, i;
	/* 64-bit preamble */
	off = pd_write_preamble(port);
	/* Start Of Packet: 3x Sync-1 + 1x Sync-2 */
	off = pd_write_sym(port, off, BMC(PD_SYNC1));
	off = pd_write_sym(port, off, BMC(PD_SYNC1));
	off = pd_write_sym(port, off, BMC(PD_SYNC1));
	off = pd_write_sym(port, off, BMC(PD_SYNC2));
	/* header */
	off = encode_short(port, off, header);

#ifdef CONFIG_COMMON_RUNTIME
	mutex_lock(&pd_crc_lock);
#endif

	crc32_init();
	crc32_hash16(header);
	/* data payload */
	for (i = 0; i < cnt; i++) {
		off = encode_word(port, off, data[i]);
		crc32_hash32(data[i]);
	}
	/* CRC */
	off = encode_word(port, off, crc32_result());

#ifdef CONFIG_COMMON_RUNTIME
	mutex_unlock(&pd_crc_lock);
#endif

	/* End Of Packet */
	off = pd_write_sym(port, off, BMC(PD_EOP));
	/* Ensure that we have a final edge */
	return pd_write_last_edge(port, off);
}

static int analyze_rx(int port, uint32_t *payload);
static void analyze_rx_bist(int port);

void send_hard_reset(int port)
{
	int off;

	/* If PD communication is disabled, return */
	if (!pd_comm_enabled)
		return;

	if (debug_level >= 1)
		CPRINTF("Sending hard reset\n");

	/* 64-bit preamble */
	off = pd_write_preamble(port);
	/* Hard-Reset: 3x RST-1 + 1x RST-2 */
	off = pd_write_sym(port, off, BMC(PD_RST1));
	off = pd_write_sym(port, off, BMC(PD_RST1));
	off = pd_write_sym(port, off, BMC(PD_RST1));
	off = pd_write_sym(port, off, BMC(PD_RST2));
	/* Ensure that we have a final edge */
	off = pd_write_last_edge(port, off);
	/* Transmit the packet */
	pd_start_tx(port, pd[port].polarity, off);
	pd_tx_done(port, pd[port].polarity);
}

static int send_validate_message(int port, uint16_t header,
				 uint8_t cnt, const uint32_t *data)
{
	int r;
	static uint32_t payload[7];

	/* If PD communication is disabled, return error */
	if (!pd_comm_enabled)
		return -2;

	/* retry 3 times if we are not getting a valid answer */
	for (r = 0; r <= PD_RETRY_COUNT; r++) {
		int bit_len, head;
		/* write the encoded packet in the transmission buffer */
		bit_len = prepare_message(port, header, cnt, data);
		/* Transmit the packet */
		pd_start_tx(port, pd[port].polarity, bit_len);
		pd_tx_done(port, pd[port].polarity);
		/*
		 * If we failed the first try, enable interrupt and yield
		 * to other tasks, so that we don't starve them.
		 */
		if (r) {
			pd_rx_enable_monitoring(port);
			/* Message receive timeout is 2.7ms */
			if (task_wait_event(USB_PD_RX_TMOUT_US) ==
			    TASK_EVENT_TIMER)
				continue;
			/*
			 * Make sure we woke up due to rx recd, otherwise
			 * we need to manually start
			 */
			if (!pd_rx_started(port)) {
				pd_rx_disable_monitoring(port);
				pd_rx_start(port);
			}
		} else {
			/* starting waiting for GoodCrc */
			pd_rx_start(port);
		}
		/* read the incoming packet if any */
		head = analyze_rx(port, payload);
		pd_rx_complete(port);
		if (head > 0) { /* we got a good packet, analyze it */
			int type = PD_HEADER_TYPE(head);
			int nb = PD_HEADER_CNT(head);
			uint8_t id = PD_HEADER_ID(head);
			if (type == PD_CTRL_GOOD_CRC && nb == 0 &&
			   id == pd[port].msg_id) {
				/* got the GoodCRC we were expecting */
				inc_id(port);
				/* do not catch last edges as a new packet */
				udelay(20);
				return bit_len;
			} else {
				/*
				 * we have received a good packet
				 * but not the expected GoodCRC,
				 * the other side is trying to contact us,
				 * bail out immediatly so we can get the retry.
				 */
				return -4;
				/* CPRINTF("ERR ACK/%d %04x\n", id, head); */
			}
		}
	}
	/* we failed all the re-transmissions */
	/* TODO: try HardReset */
	if (debug_level >= 1)
		CPRINTF("TX NO ACK %04x/%d\n", header, cnt);
	return -1;
}

static int send_control(int port, int type)
{
	int bit_len;
	uint16_t header = PD_HEADER(type, pd[port].power_role,
			pd[port].data_role, pd[port].msg_id, 0);

	bit_len = send_validate_message(port, header, 0, NULL);

	if (debug_level >= 1)
		CPRINTF("CTRL[%d]>%d\n", type, bit_len);

	return bit_len;
}

static void send_goodcrc(int port, int id)
{
	uint16_t header = PD_HEADER(PD_CTRL_GOOD_CRC, pd[port].power_role,
			pd[port].data_role, id, 0);
	int bit_len = prepare_message(port, header, 0, NULL);

	/* If PD communication is disabled, return */
	if (!pd_comm_enabled)
		return;

	pd_start_tx(port, pd[port].polarity, bit_len);
	pd_tx_done(port, pd[port].polarity);
}

static int send_source_cap(int port)
{
	int bit_len;
#ifdef CONFIG_USB_PD_DYNAMIC_SRC_CAP
	const uint32_t *src_pdo;
	const int src_pdo_cnt = pd_get_source_pdo(&src_pdo);
#else
	const uint32_t *src_pdo = pd_src_pdo;
	const int src_pdo_cnt = pd_src_pdo_cnt;
#endif
	uint16_t header = PD_HEADER(PD_DATA_SOURCE_CAP, pd[port].power_role,
			pd[port].data_role, pd[port].msg_id, src_pdo_cnt);

	bit_len = send_validate_message(port, header, src_pdo_cnt, src_pdo);
	if (debug_level >= 1)
		CPRINTF("srcCAP>%d\n", bit_len);

	return bit_len;
}

#ifdef CONFIG_USB_PD_DUAL_ROLE
static void send_sink_cap(int port)
{
	int bit_len;
	uint16_t header = PD_HEADER(PD_DATA_SINK_CAP, pd[port].power_role,
			pd[port].data_role, pd[port].msg_id, pd_snk_pdo_cnt);

	bit_len = send_validate_message(port, header, pd_snk_pdo_cnt,
					pd_snk_pdo);
	if (debug_level >= 1)
		CPRINTF("snkCAP>%d\n", bit_len);
}

static int send_request(int port, uint32_t rdo)
{
	int bit_len;
	uint16_t header = PD_HEADER(PD_DATA_REQUEST, pd[port].power_role,
			pd[port].data_role, pd[port].msg_id, 1);

	bit_len = send_validate_message(port, header, 1, &rdo);
	if (debug_level >= 1)
		CPRINTF("REQ%d>\n", bit_len);

	return bit_len;
}
#endif /* CONFIG_USB_PD_DUAL_ROLE */

static int send_bist_cmd(int port)
{
	/* currently only support sending bist carrier 2 */
	uint32_t bdo = BDO(BDO_MODE_CARRIER2, 0);
	int bit_len;
	uint16_t header = PD_HEADER(PD_DATA_BIST, pd[port].power_role,
			pd[port].data_role, pd[port].msg_id, 1);

	bit_len = send_validate_message(port, header, 1, &bdo);
	CPRINTF("BIST>%d\n", bit_len);

	return bit_len;
}

static void bist_mode_2_tx(int port)
{
	int bit;

	/* If PD communication is not allowed, return */
	if (!pd_comm_enabled)
		return;

	CPRINTF("BIST carrier 2 - sending on port %d\n", port);

	/*
	 * build context buffer with 5 bytes, where the data is
	 * alternating 1's and 0's.
	 */
	bit = pd_write_sym(port, 0,   BMC(0x15));
	bit = pd_write_sym(port, bit, BMC(0x0a));
	bit = pd_write_sym(port, bit, BMC(0x15));
	bit = pd_write_sym(port, bit, BMC(0x0a));

	/* start a circular DMA transfer (will never end) */
	pd_tx_set_circular_mode(port);
	pd_start_tx(port, pd[port].polarity, bit);

	/* do not let pd task state machine run anymore */
	while (1)
		task_wait_event(-1);
}

static void bist_mode_2_rx(int port)
{
	/* monitor for incoming packet */
	pd_rx_enable_monitoring(port);

	/* loop until we start receiving data */
	while (1) {
		task_wait_event(500*MSEC);
		/* incoming packet ? */
		if (pd_rx_started(port))
			break;
	}

	/*
	 * once we start receiving bist data, do not
	 * let state machine run again. stay here, and
	 * analyze a chunk of data every 250ms.
	 */
	while (1) {
		analyze_rx_bist(port);
		pd_rx_complete(port);
		msleep(250);
		pd_rx_enable_monitoring(port);
	}
}

static void handle_vdm_request(int port, int cnt, uint32_t *payload)
{
	int rlen = 0;
	uint32_t *rdata;

	if (pd[port].vdm_state == VDM_STATE_BUSY)
		pd[port].vdm_state = VDM_STATE_DONE;

	rlen = pd_vdm(port, cnt, payload, &rdata);
	if (rlen > 0) {
		uint16_t header = PD_HEADER(PD_DATA_VENDOR_DEF,
					    pd[port].power_role,
					    pd[port].data_role,
					    pd[port].msg_id,
					    rlen);
		send_validate_message(port, header, rlen, rdata);
		return;
	}
	if (debug_level >= 1)
		CPRINTF("Unhandled VDM VID %04x CMD %04x\n",
			PD_VDO_VID(payload[0]), payload[0] & 0xFFFF);
}

static void execute_hard_reset(int port)
{
	if (pd[port].last_state == PD_STATE_HARD_RESET_SEND)
		CPRINTF("HARD RESET (SENT)!\n");
	else
		CPRINTF("HARD RESET (RECV)!\n");

	pd[port].msg_id = 0;
	pd_exit_mode(port, NULL);

#ifdef CONFIG_USB_PD_DUAL_ROLE
	if (pd[port].power_role == PD_ROLE_SINK) {
		/* Clear the input current limit */
		pd_set_input_current_limit(port, 0, 0);
#ifdef CONFIG_CHARGE_MANAGER
		typec_set_input_current_limit(port, 0, 0);
		charge_manager_set_ceil(port, CHARGE_CEIL_NONE);
#endif /* CONFIG_CHARGE_MANAGER */

		set_state(port, PD_STATE_SNK_HARD_RESET_RECOVER);
		return;
	}
#endif /* CONFIG_USB_PD_DUAL_ROLE */

	/* We are a source, cut power */
	pd_power_supply_reset(port);
	pd[port].src_recover = get_time().val + PD_T_SRC_RECOVER;
	set_state(port, PD_STATE_SRC_HARD_RESET_RECOVER);
}

static void execute_soft_reset(int port)
{
	pd[port].msg_id = 0;
#ifdef CONFIG_USB_PD_DUAL_ROLE
	set_state(port, pd[port].power_role == PD_ROLE_SINK ?
			PD_STATE_SNK_DISCOVERY : PD_STATE_SRC_DISCOVERY);
#else
	set_state(port, PD_STATE_SRC_DISCOVERY);
#endif
	CPRINTF("Soft Reset\n");
}

void pd_soft_reset(void)
{
	int i;

	for (i = 0; i < PD_PORT_COUNT; ++i)
		if (pd_is_connected(i)) {
			set_state(i, PD_STATE_SOFT_RESET);
			task_wake(PORT_TO_TASK_ID(i));
		}
}

#ifdef CONFIG_USB_PD_DUAL_ROLE
static void pd_store_src_cap(int port, int cnt, uint32_t *src_caps)
{
	int i;

	pd_src_cap_cnt[port] = cnt;
	for (i = 0; i < cnt; i++)
		pd_src_caps[port][i] = *src_caps++;
}

static void pd_send_request_msg(int port, enum pd_request_types request)
{
	uint32_t rdo, curr_limit, supply_voltage;
	int res;

	/* we were waiting for them, let's process them */
	res = pd_choose_voltage(pd_src_cap_cnt[port], pd_src_caps[port], &rdo,
				&curr_limit, &supply_voltage);
	if (res != EC_SUCCESS)
		/*
		 * If fail to choose voltage, do nothing, let source re-send
		 * source cap
		 */
		return;

#ifdef CONFIG_CHARGE_MANAGER
	/* Set max. limit, but apply 500mA ceiling */
	charge_manager_set_ceil(port, PD_MIN_MA);
	pd_set_input_current_limit(port, curr_limit, supply_voltage);
	/* Negotiate for Vsafe5V, if requested */
	if (request == PD_REQUEST_MIN)
		pd_choose_voltage_min(pd_src_cap_cnt[port], pd_src_caps[port],
				      &rdo, &curr_limit, &supply_voltage);
#endif
	pd[port].curr_limit = curr_limit;
	pd[port].supply_voltage = supply_voltage;
	res = send_request(port, rdo);
	if (res >= 0)
		set_state(port, PD_STATE_SNK_REQUESTED);
	/* If fail send request, do nothing, let source re-send source cap */
}
#endif

static void pd_update_pdo_flags(int port, uint32_t pdo)
{
	/* can only parse PDO flags if type is fixed */
	if ((pdo & PDO_TYPE_MASK) == PDO_TYPE_FIXED) {
		if (pdo & PDO_FIXED_DUAL_ROLE)
			pd[port].flags |= PD_FLAGS_PARTNER_DR_POWER;
		else
			pd[port].flags &= ~PD_FLAGS_PARTNER_DR_POWER;

		if (pdo & PDO_FIXED_DATA_SWAP)
			pd[port].flags |= PD_FLAGS_PARTNER_DR_DATA;
		else
			pd[port].flags &= ~PD_FLAGS_PARTNER_DR_DATA;
	}
}

static void handle_data_request(int port, uint16_t head,
		uint32_t *payload)
{
	int type = PD_HEADER_TYPE(head);
	int cnt = PD_HEADER_CNT(head);

	switch (type) {
#ifdef CONFIG_USB_PD_DUAL_ROLE
	case PD_DATA_SOURCE_CAP:
		if ((pd[port].task_state == PD_STATE_SNK_DISCOVERY)
			|| (pd[port].task_state == PD_STATE_SNK_TRANSITION)
			|| (pd[port].task_state == PD_STATE_SNK_READY)) {
			pd_store_src_cap(port, cnt, payload);
			/* src cap 0 should be fixed PDO */
			pd_update_pdo_flags(port, payload[0]);
			pd_send_request_msg(port, PD_REQUEST_MIN);
		}
		break;
#endif /* CONFIG_USB_PD_DUAL_ROLE */
	case PD_DATA_REQUEST:
		if ((pd[port].power_role == PD_ROLE_SOURCE) && (cnt == 1))
			if (!pd_check_requested_voltage(payload[0])) {
				if (send_control(port, PD_CTRL_ACCEPT) < 0)
					/*
					 * if we fail to send accept, do
					 * nothing and let sink timeout and
					 * send hard reset
					 */
					return;

				/* explicit contract is now in place */
				pd[port].flags |= PD_FLAGS_EXPLICIT_CONTRACT;
				pd[port].requested_idx = payload[0] >> 28;
				set_state(port, PD_STATE_SRC_ACCEPTED);
				return;
			}
		/* the message was incorrect or cannot be satisfied */
		send_control(port, PD_CTRL_REJECT);
		break;
	case PD_DATA_BIST:
		/* currently only support sending bist carrier mode 2 */
		if ((payload[0] >> 28) == 5)
			/* bist data object mode is 2 */
			bist_mode_2_tx(port);

		break;
	case PD_DATA_SINK_CAP:
		pd[port].flags |= PD_FLAGS_SNK_CAP_RECVD;
		/* snk cap 0 should be fixed PDO */
		pd_update_pdo_flags(port, payload[0]);
		break;
	case PD_DATA_VENDOR_DEF:
		handle_vdm_request(port, cnt, payload);
		break;
	default:
		CPRINTF("Unhandled data message type %d\n", type);
	}
}

#ifdef CONFIG_USB_PD_DUAL_ROLE
void pd_request_power_swap(int port)
{
	if (pd[port].task_state == PD_STATE_SRC_READY)
		set_state(port, PD_STATE_SRC_SWAP_INIT);
	else if (pd[port].task_state == PD_STATE_SNK_READY)
		set_state(port, PD_STATE_SNK_SWAP_INIT);
	task_wake(PORT_TO_TASK_ID(port));
}
#endif

void pd_request_data_swap(int port)
{
	if (pd[port].task_state == PD_STATE_SRC_READY)
		set_state(port, PD_STATE_SRC_DR_SWAP);
#ifdef CONFIG_USB_PD_DUAL_ROLE
	else if (pd[port].task_state == PD_STATE_SNK_READY)
		set_state(port, PD_STATE_SNK_DR_SWAP);
#endif
	task_wake(PORT_TO_TASK_ID(port));
}

static void pd_dr_swap(int port)
{
	pd[port].data_role = !pd[port].data_role;
	pd_execute_data_swap(port, pd[port].data_role);
	pd[port].flags |= PD_FLAGS_DATA_SWAPPED;
}

static void handle_ctrl_request(int port, uint16_t head,
		uint32_t *payload)
{
	int type = PD_HEADER_TYPE(head);
	int res;

	switch (type) {
	case PD_CTRL_GOOD_CRC:
		/* should not get it */
		break;
	case PD_CTRL_PING:
		/* Nothing else to do */
		break;
	case PD_CTRL_GET_SOURCE_CAP:
		res = send_source_cap(port);
		if ((res >= 0) &&
		    (pd[port].task_state == PD_STATE_SRC_DISCOVERY))
			set_state(port, PD_STATE_SRC_NEGOCIATE);
		break;
#ifdef CONFIG_USB_PD_DUAL_ROLE
	case PD_CTRL_GET_SINK_CAP:
		send_sink_cap(port);
		break;
	case PD_CTRL_GOTO_MIN:
		break;
	case PD_CTRL_PS_RDY:
		if (pd[port].task_state == PD_STATE_SNK_SWAP_SRC_DISABLE) {
			set_state(port, PD_STATE_SNK_SWAP_STANDBY);
		} else if (pd[port].task_state == PD_STATE_SRC_SWAP_STANDBY) {
			/* reset message ID and swap roles */
			pd[port].msg_id = 0;
			pd[port].power_role = PD_ROLE_SINK;
			set_state(port, PD_STATE_SNK_DISCOVERY);
		} else if (pd[port].task_state == PD_STATE_SNK_DISCOVERY) {
			/* Don't know what power source is ready. Reset. */
			set_state(port, PD_STATE_HARD_RESET_SEND);
		} else if (pd[port].power_role == PD_ROLE_SINK) {
			set_state(port, PD_STATE_SNK_READY);
#ifdef CONFIG_CHARGE_MANAGER
			/* Set ceiling based on what's negotiated */
			charge_manager_set_ceil(port, pd[port].curr_limit);
#else
			pd_set_input_current_limit(port, pd[port].curr_limit,
						   pd[port].supply_voltage);
#endif
		}
		break;
#endif
	case PD_CTRL_REJECT:
	case PD_CTRL_WAIT:
		if (pd[port].task_state == PD_STATE_SRC_DR_SWAP)
			set_state(port, PD_STATE_SRC_READY);
#ifdef CONFIG_USB_PD_DUAL_ROLE
		else if (pd[port].task_state == PD_STATE_SNK_DR_SWAP)
			set_state(port, PD_STATE_SNK_READY);
		else if (pd[port].task_state == PD_STATE_SRC_SWAP_INIT)
			set_state(port, PD_STATE_SRC_READY);
		else if (pd[port].task_state == PD_STATE_SNK_SWAP_INIT)
			set_state(port, PD_STATE_SNK_READY);
		else if (pd[port].task_state == PD_STATE_SNK_REQUESTED)
			/* no explicit contract */
			set_state(port, PD_STATE_SNK_READY);
#endif
		break;
	case PD_CTRL_ACCEPT:
		if (pd[port].task_state == PD_STATE_SOFT_RESET) {
#ifdef CONFIG_USB_PD_DUAL_ROLE
			set_state(port, pd[port].power_role == PD_ROLE_SINK ?
					PD_STATE_SNK_DISCOVERY :
					PD_STATE_SRC_DISCOVERY);
#else
			set_state(port, PD_STATE_SRC_DISCOVERY);
#endif
		} else if (pd[port].task_state == PD_STATE_SRC_DR_SWAP) {
			/* switch data role */
			pd_dr_swap(port);
			set_state(port, PD_STATE_SRC_READY);
		}
#ifdef CONFIG_USB_PD_DUAL_ROLE
		else if (pd[port].task_state == PD_STATE_SNK_DR_SWAP) {
			/* switch data role */
			pd_dr_swap(port);
			set_state(port, PD_STATE_SNK_READY);
		} else if (pd[port].task_state == PD_STATE_SRC_SWAP_INIT) {
			/* explicit contract goes away for power swap */
			pd[port].flags &= ~PD_FLAGS_EXPLICIT_CONTRACT;
			set_state(port, PD_STATE_SRC_SWAP_SNK_DISABLE);
		} else if (pd[port].task_state == PD_STATE_SNK_SWAP_INIT) {
			/* explicit contract goes away for power swap */
			pd[port].flags &= ~PD_FLAGS_EXPLICIT_CONTRACT;
			set_state(port, PD_STATE_SNK_SWAP_SNK_DISABLE);
		} else if (pd[port].task_state == PD_STATE_SNK_REQUESTED) {
			/* explicit contract is now in place */
			pd[port].flags |= PD_FLAGS_EXPLICIT_CONTRACT;
			set_state(port, PD_STATE_SNK_TRANSITION);
		}
#endif
		break;
	case PD_CTRL_SOFT_RESET:
		execute_soft_reset(port);
		/* We are done, acknowledge with an Accept packet */
		send_control(port, PD_CTRL_ACCEPT);
		break;
	case PD_CTRL_PR_SWAP:
#ifdef CONFIG_USB_PD_DUAL_ROLE
		if (pd_check_power_swap(port)) {
			send_control(port, PD_CTRL_ACCEPT);
			if (pd[port].power_role == PD_ROLE_SINK)
				set_state(port, PD_STATE_SNK_SWAP_SNK_DISABLE);
			else
				set_state(port, PD_STATE_SRC_SWAP_SNK_DISABLE);
		} else {
			send_control(port, PD_CTRL_REJECT);
		}
#else
		send_control(port, PD_CTRL_REJECT);
#endif
		break;
	case PD_CTRL_DR_SWAP:
		if (pd_check_data_swap(port, pd[port].data_role)) {
			/* Accept switch and perform data swap */
			if (send_control(port, PD_CTRL_ACCEPT) >= 0)
				pd_dr_swap(port);
		} else {
			send_control(port, PD_CTRL_REJECT);
		}
		break;
	case PD_CTRL_VCONN_SWAP:
		send_control(port, PD_CTRL_REJECT);
		break;
	default:
		CPRINTF("Unhandled ctrl message type %d\n", type);
	}
}

static void handle_request(int port, uint16_t head,
		uint32_t *payload)
{
	int cnt = PD_HEADER_CNT(head);
	int p;

	if (PD_HEADER_TYPE(head) != PD_CTRL_GOOD_CRC || cnt)
		send_goodcrc(port, PD_HEADER_ID(head));

	/* dump received packet content (only dump ping at debug level 2) */
	if ((debug_level == 1 && PD_HEADER_TYPE(head) != PD_CTRL_PING) ||
	    debug_level >= 2) {
		CPRINTF("RECV %04x/%d ", head, cnt);
		for (p = 0; p < cnt; p++)
			CPRINTF("[%d]%08x ", p, payload[p]);
		CPRINTF("\n");
	}

	/*
	 * If we are in disconnected state, we shouldn't get a request. Do
	 * a hard reset if we get one.
	 */
	if (!pd_is_connected(port))
		set_state(port, PD_STATE_HARD_RESET_SEND);

	if (cnt)
		handle_data_request(port, head, payload);
	else
		handle_ctrl_request(port, head, payload);
}

static inline int decode_short(int port, int off, uint16_t *val16)
{
	uint32_t w;
	int end;

	end = pd_dequeue_bits(port, off, 20, &w);

#if 0 /* DEBUG */
	CPRINTS("%d-%d: %05x %x:%x:%x:%x\n",
		off, end, w,
		dec4b5b[(w >> 15) & 0x1f], dec4b5b[(w >> 10) & 0x1f],
		dec4b5b[(w >>  5) & 0x1f], dec4b5b[(w >>  0) & 0x1f]);
#endif
	*val16 = dec4b5b[w & 0x1f] |
		(dec4b5b[(w >>  5) & 0x1f] << 4) |
		(dec4b5b[(w >> 10) & 0x1f] << 8) |
		(dec4b5b[(w >> 15) & 0x1f] << 12);
	return end;
}

static inline int decode_word(int port, int off, uint32_t *val32)
{
	off = decode_short(port, off, (uint16_t *)val32);
	return decode_short(port, off, ((uint16_t *)val32 + 1));
}

static int count_set_bits(int n)
{
	int count = 0;
	while (n) {
		n &= (n - 1);
		count++;
	}
	return count;
}

static void analyze_rx_bist(int port)
{
	int i = 0, bit = -1;
	uint32_t w, match;
	int invalid_bits = 0;
	static int total_invalid_bits;

	/* dequeue bits until we see a full byte of alternating 1's and 0's */
	while (i < 10 && (bit < 0 || (w != 0xaa && w != 0x55)))
		bit = pd_dequeue_bits(port, i++, 8, &w);

	/* if we didn't find any bytes that match criteria, display error */
	if (i == 10) {
		CPRINTF("Could not find any bytes of alternating bits\n");
		return;
	}

	/*
	 * now we know what matching byte we are looking for, dequeue a bunch
	 * more data and count how many bits differ from expectations.
	 */
	match = w;
	bit = i - 1;
	for (i = 0; i < 40; i++) {
		bit = pd_dequeue_bits(port, bit, 8, &w);
		if (i % 20 == 0)
			CPRINTF("\n");
		CPRINTF("%02x ", w);
		invalid_bits += count_set_bits(w ^ match);
	}

	total_invalid_bits += invalid_bits;
	CPRINTF("- incorrect bits: %d / %d\n", invalid_bits,
			total_invalid_bits);
}

static int analyze_rx(int port, uint32_t *payload)
{
	int bit;
	char *msg = "---";
	uint32_t val = 0;
	uint16_t header;
	uint32_t pcrc, ccrc;
	int p, cnt;
	/* uint32_t eop; */

	pd_init_dequeue(port);

	/* Detect preamble */
	bit = pd_find_preamble(port);
	if (bit < 0) {
		msg = "Preamble";
		goto packet_err;
	}

	/* Find the Start Of Packet sequence */
	while (bit > 0) {
		bit = pd_dequeue_bits(port, bit, 20, &val);
		if (val == PD_SOP) {
			break;
		} else if (val == PD_SOP_PRIME) {
			CPRINTF("SOP'\n");
			return -5;
		} else if (val == PD_SOP_PRIME_PRIME) {
			CPRINTF("SOP''\n");
			return -5;
		}
	}
	if (bit < 0) {
		msg = "SOP";
		goto packet_err;
	}

	/* read header */
	bit = decode_short(port, bit, &header);

#ifdef CONFIG_COMMON_RUNTIME
	mutex_lock(&pd_crc_lock);
#endif

	crc32_init();
	crc32_hash16(header);
	cnt = PD_HEADER_CNT(header);

	/* read payload data */
	for (p = 0; p < cnt && bit > 0; p++) {
		bit = decode_word(port, bit, payload+p);
		crc32_hash32(payload[p]);
	}
	ccrc = crc32_result();

#ifdef CONFIG_COMMON_RUNTIME
	mutex_unlock(&pd_crc_lock);
#endif

	if (bit < 0) {
		msg = "len";
		goto packet_err;
	}

	/* check transmitted CRC */
	bit = decode_word(port, bit, &pcrc);
	if (bit < 0 || pcrc != ccrc) {
		msg = "CRC";
		if (pcrc != ccrc)
			bit = PD_ERR_CRC;
		if (debug_level >= 1)
			/* DEBUG */CPRINTF("CRC %08x <> %08x\n", pcrc, ccrc);
		goto packet_err;
	}

	/* check End Of Packet */
	/* SKIP EOP for now
	bit = pd_dequeue_bits(port, bit, 5, &eop);
	if (bit < 0 || eop != PD_EOP) {
		msg = "EOP";
		goto packet_err;
	}
	*/

	return header;
packet_err:
	if (debug_level >= 2)
		pd_dump_packet(port, msg);
	else
		CPRINTF("RX ERR (%d)\n", bit);
	return bit;
}

void pd_send_vdm(int port, uint32_t vid, int cmd, const uint32_t *data,
		 int count)
{
	int i;

	if (count > VDO_MAX_SIZE - 1) {
		CPRINTF("VDM over max size\n");
		return;
	}

	pd[port].vdo_data[0] = VDO(vid, ((vid & USB_SID_PD) == USB_SID_PD) ?
				   1 : 0, cmd);

	pd[port].vdo_count = count + 1;
	for (i = 1; i < count + 1; i++)
		pd[port].vdo_data[i] = data[i-1];

	/* Set ready, pd task will actually send */
	pd[port].vdm_state = VDM_STATE_READY;
	task_wake(PORT_TO_TASK_ID(port));
}

static void pd_vdm_send_state_machine(int port)
{
	int res;
	uint16_t header;
	static uint64_t vdm_timeout;

	switch (pd[port].vdm_state) {
	case VDM_STATE_READY:
		/* Only transmit VDM if connected */
		if (!pd_is_connected(port)) {
			pd[port].vdm_state = VDM_STATE_ERR_BUSY;
			break;
		}

		/* Prepare and send VDM */
		header = PD_HEADER(PD_DATA_VENDOR_DEF, pd[port].power_role,
				   pd[port].data_role, pd[port].msg_id,
				   (int)pd[port].vdo_count);
		res = send_validate_message(port, header,
				    pd[port].vdo_count,
				    pd[port].vdo_data);
		if (res < 0) {
			pd[port].vdm_state = VDM_STATE_ERR_SEND;
		} else {
			pd[port].vdm_state = VDM_STATE_BUSY;
			vdm_timeout = get_time().val + 500*MSEC;
		}
		break;
	case VDM_STATE_BUSY:
		/* Wait for VDM response or timeout */
		if (get_time().val > vdm_timeout)
			pd[port].vdm_state = VDM_STATE_ERR_TMOUT;
		break;
	default:
		break;
	}
}

static inline void pd_dev_dump_info(uint16_t dev_id, uint8_t *hash)
{
	int j;
	ccprintf("Device:0x%04x Hash:", dev_id);
	for (j = 0; j < PD_RW_HASH_SIZE; j += 4) {
		ccprintf(" 0x%02x%02x%02x%02x", hash[j + 3], hash[j + 2],
			 hash[j + 1], hash[j]);
	}
	ccprintf("\n");
}

void pd_dev_store_rw_hash(int port, uint16_t dev_id, uint32_t *rw_hash)
{
	pd[port].dev_id = dev_id;
	memcpy(pd[port].dev_rw_hash, rw_hash, PD_RW_HASH_SIZE);
	if (debug_level >= 1)
		pd_dev_dump_info(dev_id, (uint8_t *)rw_hash);
}

#ifdef CONFIG_USB_PD_DUAL_ROLE
void pd_set_dual_role(enum pd_dual_role_states state)
{
	int i;
	drp_state = state;

	for (i = 0; i < PD_PORT_COUNT; i++) {
		/*
		 * Change to sink if port is currently a source AND (new DRP
		 * state is force sink OR new DRP state is toggle off and we
		 * are in the source disconnected state).
		 */
		if (pd[i].power_role == PD_ROLE_SOURCE &&
		    (drp_state == PD_DRP_FORCE_SINK ||
		     (drp_state == PD_DRP_TOGGLE_OFF
		      && pd[i].task_state == PD_STATE_SRC_DISCONNECTED))) {
			pd[i].power_role = PD_ROLE_SINK;
			set_state(i, PD_STATE_SNK_DISCONNECTED);
			pd_set_host_mode(i, 0);
			task_wake(PORT_TO_TASK_ID(i));
		}

		/*
		 * Change to source if port is currently a sink and the
		 * new DRP state is force source.
		 */
		if (pd[i].power_role == PD_ROLE_SINK &&
		    drp_state == PD_DRP_FORCE_SOURCE) {
			pd[i].power_role = PD_ROLE_SOURCE;
			set_state(i, PD_STATE_SRC_DISCONNECTED);
			pd_set_host_mode(i, 1);
			task_wake(PORT_TO_TASK_ID(i));
		}
	}
}

int pd_get_role(int port)
{
	return pd[port].power_role;
}

static int pd_is_power_swapping(int port)
{
	/* return true if in the act of swapping power roles */
	return  pd[port].task_state == PD_STATE_SNK_SWAP_SNK_DISABLE ||
		pd[port].task_state == PD_STATE_SNK_SWAP_SRC_DISABLE ||
		pd[port].task_state == PD_STATE_SNK_SWAP_STANDBY ||
		pd[port].task_state == PD_STATE_SNK_SWAP_COMPLETE ||
		pd[port].task_state == PD_STATE_SRC_SWAP_SNK_DISABLE ||
		pd[port].task_state == PD_STATE_SRC_SWAP_SRC_DISABLE ||
		pd[port].task_state == PD_STATE_SRC_SWAP_STANDBY;
}

#endif /* CONFIG_USB_PD_DUAL_ROLE */

int pd_get_polarity(int port)
{
	return pd[port].polarity;
}

int pd_get_partner_dualrole_capable(int port)
{
	/* return dualrole status of port partner */
	return pd[port].flags & PD_FLAGS_PARTNER_DR_POWER;
}

int pd_get_partner_data_swap_capable(int port)
{
	/* return data swap capable status of port partner */
	return pd[port].flags & PD_FLAGS_PARTNER_DR_DATA;
}

void pd_comm_enable(int enable)
{
	pd_comm_enabled = enable;

#ifdef CONFIG_USB_PD_DUAL_ROLE
	/*
	 * If communications are enabled, start hard reset timer for
	 * any port in PD_SNK_DISCOVERY.
	 */
	if (enable) {
		int i;
		for (i = 0; i < PD_PORT_COUNT; i++) {
			if (pd[i].task_state == PD_STATE_SNK_DISCOVERY)
				set_state_timeout(i,
						  get_time().val +
						  PD_T_SINK_WAIT_CAP,
						  PD_STATE_HARD_RESET_SEND);
		}
	}
#endif
}

void pd_ping_enable(int port, int enable)
{
	if (enable)
		pd[port].flags |= PD_FLAGS_PING_ENABLED;
	else
		pd[port].flags &= ~PD_FLAGS_PING_ENABLED;
}

#ifdef CONFIG_CHARGE_MANAGER
/**
 * Returns type C current limit (mA) based upon cc_voltage (mV).
 */
static inline int get_typec_current_limit(int cc_voltage)
{
	int charge;

	/* Detect type C charger current limit based upon vbus voltage. */
	if (cc_voltage > TYPE_C_SRC_3000_THRESHOLD)
		charge = 3000;
	else if (cc_voltage > TYPE_C_SRC_1500_THRESHOLD)
		charge = 1500;
	else if (cc_voltage > PD_SNK_VA)
		charge = 500;
	else
		charge = 0;

	return charge;
}

/**
 * Signal power request to indicate a charger update that affects the port.
 */
void pd_set_new_power_request(int port)
{
	pd[port].new_power_request = 1;
	task_wake(PORT_TO_TASK_ID(port));
}
#endif /* CONFIG_CHARGE_MANAGER */

void pd_task(void)
{
	int head;
	int port = TASK_ID_TO_PORT(task_get_current());
	uint32_t payload[7];
	int timeout = 10*MSEC;
	int cc1_volt, cc2_volt;
	int res, incoming_packet;
#ifdef CONFIG_USB_PD_DUAL_ROLE
	uint64_t next_role_swap = PD_T_DRP_SNK;
	int hard_reset_count = 0;
	int snk_hard_reset_vbus_off = 0;
#ifdef CONFIG_CHARGE_MANAGER
	static int initialized[PD_PORT_COUNT];
	int typec_curr = 0, typec_curr_change = 0;
#endif /* CONFIG_CHARGE_MANAGER */
#endif /* CONFIG_USB_PD_DUAL_ROLE */
	enum pd_states this_state;
	timestamp_t now;
	int caps_count = 0, src_connected = 0;

	/* Initialize TX pins and put them in Hi-Z */
	pd_tx_init();

	/* Initialize PD protocol state variables for each port. */
	pd[port].power_role = PD_ROLE_DEFAULT;
	pd[port].data_role = PD_ROLE_DEFAULT;
	pd[port].vdm_state = VDM_STATE_DONE;
	pd[port].flags = 0;
	set_state(port, PD_DEFAULT_STATE);

	/* Ensure the power supply is in the default state */
	pd_power_supply_reset(port);

	/* Initialize physical layer */
	pd_hw_init(port);

	while (1) {
		/* process VDM messages last */
		pd_vdm_send_state_machine(port);

		/* monitor for incoming packet if in a connected state */
		if (pd_is_connected(port) && pd_comm_enabled)
			pd_rx_enable_monitoring(port);
		else
			pd_rx_disable_monitoring(port);

		/* Verify board specific health status : current, voltages... */
		res = pd_board_checks();
		if (res != EC_SUCCESS) {
			/* cut the power */
			execute_hard_reset(port);
			/* notify the other side of the issue */
			send_hard_reset(port);
		}
		/* wait for next event/packet or timeout expiration */
		task_wait_event(timeout);
		/* incoming packet ? */
		if (pd_rx_started(port) && pd_comm_enabled) {
			incoming_packet = 1;
			head = analyze_rx(port, payload);
			pd_rx_complete(port);
			if (head > 0)
				handle_request(port,  head, payload);
			else if (head == PD_ERR_HARD_RESET)
				execute_hard_reset(port);
		} else {
			incoming_packet = 0;
		}
		/* if nothing to do, verify the state of the world in 500ms */
		this_state = pd[port].task_state;
		timeout = 500*MSEC;
		switch (this_state) {
		case PD_STATE_DISABLED:
			/* Nothing to do */
			break;
		case PD_STATE_SRC_DISCONNECTED:
			timeout = 10*MSEC;

			/* Vnc monitoring */
			cc1_volt = pd_adc_read(port, 0);
			cc2_volt = pd_adc_read(port, 1);
			if ((cc1_volt < PD_SRC_VNC) ||
			    (cc2_volt < PD_SRC_VNC)) {
				pd[port].polarity =
					GET_POLARITY(cc1_volt, cc2_volt);
				pd_select_polarity(port, pd[port].polarity);
				/* initial data role for source is DFP */
				pd[port].data_role = PD_ROLE_DFP;
				/* Set to USB SS initially */
#ifdef CONFIG_USBC_SS_MUX
				board_set_usb_mux(port, TYPEC_MUX_USB,
						  pd[port].polarity);
#endif
				/* Enable VBUS */
				if (pd_set_power_supply_ready(port)) {
#ifdef CONFIG_USBC_SS_MUX
					board_set_usb_mux(port, TYPEC_MUX_NONE,
							  pd[port].polarity);
#endif
					break;
				}

#ifdef CONFIG_USBC_VCONN
				pd_set_vconn(port, pd[port].polarity, 1);
#endif

				set_state(port, PD_STATE_SRC_STARTUP);
			}
#ifdef CONFIG_USB_PD_DUAL_ROLE
			/* Swap roles if time expired or VBUS is present */
			else if (drp_state != PD_DRP_FORCE_SOURCE &&
				 (get_time().val >= next_role_swap ||
				  pd_snk_is_vbus_provided(port))) {
				pd[port].power_role = PD_ROLE_SINK;
				set_state(port, PD_STATE_SNK_DISCONNECTED);
				pd_set_host_mode(port, 0);
				next_role_swap = get_time().val + PD_T_DRP_SNK;

				/* Swap states quickly */
				timeout = 2*MSEC;
			}
#endif
			break;
		case PD_STATE_SRC_HARD_RESET_RECOVER:
			/* Do not continue until hard reset recovery time */
			if (get_time().val < pd[port].src_recover) {
				timeout = 50*MSEC;
				break;
			}

			/* Enable VBUS */
			timeout = 10*MSEC;
			if (pd_set_power_supply_ready(port)) {
				set_state(port, PD_STATE_SRC_DISCONNECTED);
				break;
			}
			set_state(port, PD_STATE_SRC_STARTUP);
			break;
		case PD_STATE_SRC_STARTUP:
			/* Wait for power source to enable */
			if (pd[port].last_state != pd[port].task_state) {
				/*
				 * fake set data role swapped flag so we send
				 * discover identity when we enter SRC_READY
				 */
				pd[port].flags |= PD_FLAGS_DATA_SWAPPED;
				/* reset various counters */
				caps_count = 0;
				src_connected = 0;
				pd[port].msg_id = 0;
				set_state_timeout(
					port,
					get_time().val +
					PD_POWER_SUPPLY_TRANSITION_DELAY,
					PD_STATE_SRC_DISCOVERY);
			}
			break;
		case PD_STATE_SRC_DISCOVERY:
#ifdef CONFIG_USB_PD_DUAL_ROLE
			/* Keep VBUS up for the hold period */
			if (pd[port].last_state != pd[port].task_state)
				next_role_swap = get_time().val + PD_T_DRP_HOLD;
#endif
			/*
			 * While we were enabling VBUS, other side could have
			 * toggled roles, so now wait until other side settles
			 * on UFP, before sending source cap.
			 */
			if (!src_connected) {
				cc1_volt = pd_adc_read(port, pd[port].polarity);
				if (cc1_volt < PD_SRC_VNC) {
					src_connected = 1;
				} else {
					timeout = 10*MSEC;
					break;
				}
			}

			/* Send source cap some minimum number of times */
			if (caps_count < PD_CAPS_COUNT) {
				/* Query capabilites of the other side */
				res = send_source_cap(port);
				/* packet was acked => PD capable device) */
				if (res >= 0) {
					set_state(port,
						  PD_STATE_SRC_NEGOCIATE);
					caps_count = 0;
				} else { /* failed, retry later */
					timeout = PD_T_SEND_SOURCE_CAP;
					caps_count++;
				}
			}
			break;
		case PD_STATE_SRC_NEGOCIATE:
			/* wait for a "Request" message */
			if (pd[port].last_state != pd[port].task_state)
				set_state_timeout(port,
						  get_time().val +
						  PD_T_SENDER_RESPONSE,
						  PD_STATE_HARD_RESET_SEND);
			break;
		case PD_STATE_SRC_ACCEPTED:
			/* Accept sent, wait for enabling the new voltage */
			if (pd[port].last_state != pd[port].task_state)
				set_state_timeout(
					port,
					get_time().val +
					PD_T_SINK_TRANSITION,
					PD_STATE_SRC_POWERED);
			break;
		case PD_STATE_SRC_POWERED:
			/* Switch to the new requested voltage */
			if (pd[port].last_state != pd[port].task_state) {
				pd_transition_voltage(pd[port].requested_idx);
				set_state_timeout(
					port,
					get_time().val +
					PD_POWER_SUPPLY_TRANSITION_DELAY,
					PD_STATE_SRC_TRANSITION);
			}
			break;
		case PD_STATE_SRC_TRANSITION:
			/* the voltage output is good, notify the source */
			res = send_control(port, PD_CTRL_PS_RDY);
			if (res >= 0) {
				timeout = 10*MSEC;
				pd[port].flags |= PD_FLAGS_NEW_CONTRACT;
				/* it'a time to ping regularly the sink */
				set_state(port, PD_STATE_SRC_READY);
			} else {
				/* The sink did not ack, cut the power... */
				pd_power_supply_reset(port);
				set_state(port, PD_STATE_SRC_DISCONNECTED);
			}
			break;
		case PD_STATE_SRC_READY:
			timeout = PD_T_SOURCE_ACTIVITY;

			if (pd[port].last_state != pd[port].task_state)
				pd[port].flags |= PD_FLAGS_GET_SNK_CAP_SENT;

			/*
			 * Don't send any PD traffic if we woke up due to
			 * incoming packet to avoid collisions
			 */
			if (incoming_packet)
				break;

			/* Send get sink cap if haven't received it yet */
			if ((pd[port].flags & PD_FLAGS_GET_SNK_CAP_SENT) &&
			    !(pd[port].flags & PD_FLAGS_SNK_CAP_RECVD)) {
				/* Get sink cap to know if dual-role device */
				send_control(port, PD_CTRL_GET_SINK_CAP);
				pd[port].flags &= ~PD_FLAGS_GET_SNK_CAP_SENT;
				break;
			}

			/* Check our role policy, which may trigger a swap */
			if (pd[port].flags & PD_FLAGS_NEW_CONTRACT) {
				pd_new_contract(
					port, PD_ROLE_SOURCE,
					pd[port].data_role,
					pd[port].flags &
						PD_FLAGS_PARTNER_DR_POWER,
					pd[port].flags &
						PD_FLAGS_PARTNER_DR_DATA);
				pd[port].flags &= ~PD_FLAGS_NEW_CONTRACT;
				break;
			}

			/* Send discovery SVDMs last */
			if (pd[port].data_role == PD_ROLE_DFP &&
			    (pd[port].flags & PD_FLAGS_DATA_SWAPPED)) {
#ifndef CONFIG_USB_PD_SIMPLE_DFP
				pd_send_vdm(port, USB_SID_PD,
					    CMD_DISCOVER_IDENT, NULL, 0);
#endif
				pd[port].flags &= ~PD_FLAGS_DATA_SWAPPED;
				break;
			}

			if (!(pd[port].flags & PD_FLAGS_PING_ENABLED))
				break;

			/* Verify that the sink is alive */
			res = send_control(port, PD_CTRL_PING);
			if (res >= 0)
				break;

			/* Ping dropped. Try soft reset. */
			set_state(port, PD_STATE_SOFT_RESET);
			timeout = 10 * MSEC;
			break;
		case PD_STATE_SRC_DR_SWAP:
			if (pd[port].last_state != pd[port].task_state) {
				res = send_control(port, PD_CTRL_DR_SWAP);
				if (res < 0)
					set_state(port,
						  PD_STATE_HARD_RESET_SEND);
				/* Wait for accept or reject */
				set_state_timeout(port,
						  get_time().val +
						  PD_T_SENDER_RESPONSE,
						  PD_STATE_SRC_READY);
			}
			break;
#ifdef CONFIG_USB_PD_DUAL_ROLE
		case PD_STATE_SRC_SWAP_INIT:
			if (pd[port].last_state != pd[port].task_state) {
				res = send_control(port, PD_CTRL_PR_SWAP);
				if (res < 0)
					set_state(port,
						  PD_STATE_HARD_RESET_SEND);
				/* Wait for accept or reject */
				set_state_timeout(port,
						  get_time().val +
						  PD_T_SENDER_RESPONSE,
						  PD_STATE_SRC_READY);
			}
			break;
		case PD_STATE_SRC_SWAP_SNK_DISABLE:
			/* Give time for sink to stop drawing current */
			if (pd[port].last_state != pd[port].task_state)
				set_state_timeout(port,
						  get_time().val +
						  PD_T_SINK_TRANSITION,
						  PD_STATE_SRC_SWAP_SRC_DISABLE);
			break;
		case PD_STATE_SRC_SWAP_SRC_DISABLE:
			/* Turn power off */
			if (pd[port].last_state != pd[port].task_state) {
				pd_power_supply_reset(port);
				set_state_timeout(port,
						  get_time().val +
						  PD_POWER_SUPPLY_TRANSITION_DELAY,
						  PD_STATE_SRC_SWAP_STANDBY);
			}
			break;
		case PD_STATE_SRC_SWAP_STANDBY:
			/* Send PS_RDY to let sink know our power is off */
			if (pd[port].last_state != pd[port].task_state) {
				/* Send PS_RDY */
				res = send_control(port, PD_CTRL_PS_RDY);
				if (res < 0)
					set_state(port,
						  PD_STATE_HARD_RESET_SEND);
				/* Switch to Rd */
				pd_set_host_mode(port, 0);
				/* Wait for PS_RDY from sink */
				set_state_timeout(port,
						  get_time().val +
						  PD_T_PS_SOURCE_ON,
						  PD_STATE_HARD_RESET_EXECUTE);
			}
			break;
		case PD_STATE_SUSPENDED:
			pd_rx_disable_monitoring(port);
			pd_hw_release(port);
			pd_power_supply_reset(port);

			/* Wait for resume */
			while (pd[port].task_state == PD_STATE_SUSPENDED)
				task_wait_event(-1);

			pd_hw_init(port);
			break;
		case PD_STATE_SNK_DISCONNECTED:
			timeout = 10*MSEC;

			/* Source connection monitoring */
			if (pd_snk_is_vbus_provided(port)) {
				cc1_volt = pd_adc_read(port, 0);
				cc2_volt = pd_adc_read(port, 1);
				if ((cc1_volt >= PD_SNK_VA) ||
				    (cc2_volt >= PD_SNK_VA)) {
					pd[port].polarity =
						GET_POLARITY(cc1_volt,
							     cc2_volt);
					pd_select_polarity(port,
							   pd[port].polarity);
					/* reset message ID  on connection */
					pd[port].msg_id = 0;
					/* initial data role for sink is UFP */
					pd[port].data_role = PD_ROLE_UFP;
#ifdef CONFIG_CHARGE_MANAGER
					initialized[port] = 1;
					typec_curr = get_typec_current_limit(
						pd[port].polarity ? cc2_volt :
								    cc1_volt);
					typec_set_input_current_limit(
					  port, typec_curr, TYPE_C_VOLTAGE);
#endif
					set_state(port, PD_STATE_SNK_DISCOVERY);
					timeout = 10*MSEC;
					hook_call_deferred(
						pd_usb_billboard_deferred,
						PD_T_AME);
					break;
				}
			}

#ifdef CONFIG_CHARGE_MANAGER
			/*
			 * Set charge limit to zero if we have yet to set
			 * a limit for this port.
			 */
			if (!initialized[port]) {
				initialized[port] = 1;
				typec_set_input_current_limit(port, 0, 0);
				pd_set_input_current_limit(port, 0, 0);
			}
#endif
			/*
			 * If no source detected, reset hard reset counter and
			 * check for role swap
			 */
			hard_reset_count = 0;
			if (drp_state == PD_DRP_TOGGLE_ON &&
				   get_time().val >= next_role_swap) {
				/* Swap roles to source */
				pd[port].power_role = PD_ROLE_SOURCE;
				set_state(port, PD_STATE_SRC_DISCONNECTED);
				pd_set_host_mode(port, 1);
				next_role_swap = get_time().val + PD_T_DRP_SRC;

				/* Swap states quickly */
				timeout = 2*MSEC;
			}
			break;
		case PD_STATE_SNK_HARD_RESET_RECOVER:
			/* Wait for VBUS to go low and then high*/
			if (pd[port].last_state != pd[port].task_state) {
				snk_hard_reset_vbus_off = 0;
				set_state_timeout(port,
						  get_time().val +
						  PD_T_SAFE_0V,
						  PD_STATE_HARD_RESET_SEND);
			}

			if (!pd_snk_is_vbus_provided(port) &&
			    !snk_hard_reset_vbus_off) {
				/* VBUS has gone low, reset timeout */
				snk_hard_reset_vbus_off = 1;
				set_state_timeout(port,
						  get_time().val +
						  PD_T_SRC_RECOVER_MAX +
						  PD_T_SRC_TURN_ON,
						  PD_STATE_SNK_DISCONNECTED);

			}
			if (pd_snk_is_vbus_provided(port) &&
			    snk_hard_reset_vbus_off) {
				/* VBUS went high again */
				set_state(port, PD_STATE_SNK_DISCOVERY);
				timeout = 10*MSEC;
			}

			/*
			 * Don't need to set timeout because VBUS changing
			 * will trigger an interrupt and wake us up.
			 */
			break;
		case PD_STATE_SNK_DISCOVERY:
			/*
			 * Wait for source cap expired only if we are enabled
			 * and haven't passed the hard reset counter
			 */
			if ((pd[port].last_state != pd[port].task_state)
			    && hard_reset_count < PD_HARD_RESET_COUNT
			    && pd_comm_enabled) {
				/*
				 * fake set data role swapped flag so we send
				 * discover identity when we enter SRC_READY
				 */
				pd[port].flags |= PD_FLAGS_DATA_SWAPPED;
				pd[port].flags |= PD_FLAGS_NEW_CONTRACT;
				set_state_timeout(port,
						  get_time().val +
						  PD_T_SINK_WAIT_CAP,
						  PD_STATE_HARD_RESET_SEND);
			}

#ifdef CONFIG_CHARGE_MANAGER
			timeout = PD_T_SINK_ADJ - PD_T_DEBOUNCE;

			/* Check if CC pull-up has changed */
			cc1_volt = pd_adc_read(port, pd[port].polarity);
			if (typec_curr != get_typec_current_limit(cc1_volt)) {
				/* debounce signal by requiring two reads */
				if (typec_curr_change) {
					/* set new input current limit */
					typec_curr = get_typec_current_limit(
							cc1_volt);
					typec_set_input_current_limit(
					  port, typec_curr, TYPE_C_VOLTAGE);
				} else {
					/* delay for debounce */
					timeout = PD_T_DEBOUNCE;
				}
				typec_curr_change = !typec_curr_change;
			} else {
				typec_curr_change = 0;
			}
#endif
			break;
		case PD_STATE_SNK_REQUESTED:
			/* Wait for ACCEPT or REJECT */
			if (pd[port].last_state != pd[port].task_state) {
				hard_reset_count = 0;
				set_state_timeout(port,
						  get_time().val +
						  PD_T_SENDER_RESPONSE,
						  PD_STATE_HARD_RESET_SEND);
			}
			break;
		case PD_STATE_SNK_TRANSITION:
			/* Wait for PS_RDY */
			if (pd[port].last_state != pd[port].task_state)
				set_state_timeout(port,
						  get_time().val +
						  PD_T_PS_TRANSITION,
						  PD_STATE_HARD_RESET_SEND);
			break;
		case PD_STATE_SNK_READY:
			timeout = 20*MSEC;

			/*
			 * Don't send any PD traffic if we woke up due to
			 * incoming packet to avoid collisions
			 */
			if (incoming_packet)
				break;

			/* Check for new power to request */
			if (pd[port].new_power_request) {
				pd[port].new_power_request = 0;
#ifdef CONFIG_CHARGE_MANAGER
				if (charge_manager_get_active_charge_port()
				    != port)
					pd_send_request_msg(port,
							    PD_REQUEST_MIN);
				else if (charge_manager_get_active_charge_port()
					 == port)
#endif
					pd_send_request_msg(port,
							    PD_REQUEST_MAX);
				break;
			}

			/*
			 * Give time for source to check role policy first by
			 * not running this the first time through state.
			 */
			if ((pd[port].flags & PD_FLAGS_NEW_CONTRACT) &&
			    pd[port].last_state == pd[port].task_state) {
				pd_new_contract(
					port, PD_ROLE_SINK,
					pd[port].data_role,
					pd[port].flags &
						PD_FLAGS_PARTNER_DR_POWER,
					pd[port].flags &
						PD_FLAGS_PARTNER_DR_DATA);
				pd[port].flags &= ~PD_FLAGS_NEW_CONTRACT;
				break;
			}

			/* If DFP, send discovery SVDMs */
			if (pd[port].data_role == PD_ROLE_DFP &&
			     (pd[port].flags & PD_FLAGS_DATA_SWAPPED)) {
				pd_send_vdm(port, USB_SID_PD,
					    CMD_DISCOVER_IDENT, NULL, 0);
				pd[port].flags &= ~PD_FLAGS_DATA_SWAPPED;
				break;
			}

			/* Sent all messages, don't need to wake very often */
			timeout = 200*MSEC;
			break;
		case PD_STATE_SNK_DR_SWAP:
			if (pd[port].last_state != pd[port].task_state) {
				res = send_control(port, PD_CTRL_DR_SWAP);
				if (res < 0)
					set_state(port,
						  PD_STATE_HARD_RESET_SEND);
				/* Wait for accept or reject */
				set_state_timeout(port,
						  get_time().val +
						  PD_T_SENDER_RESPONSE,
						  PD_STATE_SRC_READY);
			}
			break;
		case PD_STATE_SNK_SWAP_INIT:
			if (pd[port].last_state != pd[port].task_state) {
				res = send_control(port, PD_CTRL_PR_SWAP);
				if (res < 0)
					set_state(port,
						  PD_STATE_HARD_RESET_SEND);
				/* Wait for accept or reject */
				set_state_timeout(port,
						  get_time().val +
						  PD_T_SENDER_RESPONSE,
						  PD_STATE_SNK_READY);
			}
			break;
		case PD_STATE_SNK_SWAP_SNK_DISABLE:
			/* Stop drawing power */
			pd_set_input_current_limit(port, 0, 0);
#ifdef CONFIG_CHARGE_MANAGER
			typec_set_input_current_limit(port, 0, 0);
			charge_manager_set_ceil(port, CHARGE_CEIL_NONE);
#endif
			set_state(port, PD_STATE_SNK_SWAP_SRC_DISABLE);
			timeout = 10*MSEC;
			break;
		case PD_STATE_SNK_SWAP_SRC_DISABLE:
			/* Wait for PS_RDY */
			if (pd[port].last_state != pd[port].task_state)
				set_state_timeout(port,
						  get_time().val +
						  PD_T_PS_SOURCE_OFF,
						  PD_STATE_HARD_RESET_SEND);
			break;
		case PD_STATE_SNK_SWAP_STANDBY:
			if (pd[port].last_state != pd[port].task_state) {
				/* Switch to Rp and enable power supply */
				pd_set_host_mode(port, 1);
				if (pd_set_power_supply_ready(port)) {
					/* Restore Rd */
					pd_set_host_mode(port, 0);
					set_state(port,
						  PD_STATE_HARD_RESET_SEND);
					break;
				}
				/* Wait for power supply to turn on */
				set_state_timeout(
					port,
					get_time().val +
					PD_POWER_SUPPLY_TRANSITION_DELAY,
					PD_STATE_SNK_SWAP_COMPLETE);
			}
			break;
		case PD_STATE_SNK_SWAP_COMPLETE:
			/* Send PS_RDY and change to source role */
			res = send_control(port, PD_CTRL_PS_RDY);
			if (res < 0) {
				/* Restore Rd */
				pd_set_host_mode(port, 0);
				set_state(port, PD_STATE_HARD_RESET_SEND);
			}

			caps_count = 0;
			pd[port].msg_id = 0;
			pd[port].power_role = PD_ROLE_SOURCE;
			set_state(port, PD_STATE_SRC_DISCOVERY);
			timeout = 10*MSEC;
			break;
#endif /* CONFIG_USB_PD_DUAL_ROLE */
		case PD_STATE_SOFT_RESET:
			if (pd[port].last_state != pd[port].task_state) {
				execute_soft_reset(port);
				res = send_control(port, PD_CTRL_SOFT_RESET);

				/* if soft reset failed, try hard reset. */
				if (res < 0) {
					set_state(port,
						  PD_STATE_HARD_RESET_SEND);
					break;
				}

				set_state_timeout(
					port,
					get_time().val + PD_T_SENDER_RESPONSE,
					PD_STATE_HARD_RESET_SEND);
			}
			break;
		case PD_STATE_HARD_RESET_SEND:
#ifdef CONFIG_USB_PD_DUAL_ROLE
			if (pd[port].last_state == PD_STATE_SNK_DISCOVERY)
				hard_reset_count++;
#endif
			if (pd[port].last_state != pd[port].task_state) {
				send_hard_reset(port);
				/*
				 * If we are source, delay before cutting power
				 * to allow sink time to get hard reset.
				 */
				if (pd[port].power_role == PD_ROLE_SOURCE) {
					set_state_timeout(port,
					  get_time().val + PD_T_SINK_TRANSITION,
					  PD_STATE_HARD_RESET_EXECUTE);
				} else {
					set_state(port,
						  PD_STATE_HARD_RESET_EXECUTE);
					timeout = 10*MSEC;
				}
			}
			break;
		case PD_STATE_HARD_RESET_EXECUTE:
#ifdef CONFIG_USB_PD_DUAL_ROLE
			/*
			 * If hard reset while in the last stages of power
			 * swap, then we need to restore our CC resistor.
			 */
			if (pd[port].last_state == PD_STATE_SRC_SWAP_STANDBY)
				pd_set_host_mode(port, 1);
			else if (pd[port].last_state ==
					PD_STATE_SNK_SWAP_STANDBY)
				pd_set_host_mode(port, 0);
#endif

			/* reset our own state machine */
			execute_hard_reset(port);
			timeout = 10*MSEC;
			break;
		case PD_STATE_BIST:
			send_bist_cmd(port);
			bist_mode_2_rx(port);
			break;
		default:
			break;
		}

		pd[port].last_state = this_state;

		/*
		 * Check for state timeout, and if not check if need to adjust
		 * timeout value to wake up on the next state timeout.
		 */
		now = get_time();
		if (pd[port].timeout) {
			if (now.val >= pd[port].timeout) {
				set_state(port, pd[port].timeout_state);
				/* On a state timeout, run next state soon */
				timeout = timeout < 10*MSEC ? timeout : 10*MSEC;
			} else if (pd[port].timeout - now.val < timeout) {
				timeout = pd[port].timeout - now.val;
			}
		}

		/* Check for disconnection */
#ifdef CONFIG_USB_PD_DUAL_ROLE
		if (!pd_is_connected(port) || pd_is_power_swapping(port))
			continue;
#endif
		if (pd[port].power_role == PD_ROLE_SOURCE &&
		    pd[port].task_state != PD_STATE_SRC_STARTUP) {
			/* Source: detect disconnect by monitoring CC */
			cc1_volt = pd_adc_read(port, pd[port].polarity);
#ifdef CONFIG_USB_PD_DUAL_ROLE
			if (cc1_volt > PD_SRC_VNC &&
			    get_time().val >= next_role_swap) {
				/* Stay a source port for lock period */
				next_role_swap = get_time().val + PD_T_DRP_LOCK;
#else
			if (cc1_volt > PD_SRC_VNC) {
#endif
				pd_power_supply_reset(port);
				set_state(port, PD_STATE_SRC_DISCONNECTED);
				/* Debouncing */
				timeout = 50*MSEC;
			}
		}
#ifdef CONFIG_USB_PD_DUAL_ROLE
		/*
		 * Sink disconnect if VBUS is low and we are not recovering
		 * a hard reset.
		 */
		if (pd[port].power_role == PD_ROLE_SINK &&
		    !pd_snk_is_vbus_provided(port) &&
		    pd[port].task_state != PD_STATE_SNK_HARD_RESET_RECOVER &&
		    pd[port].task_state != PD_STATE_HARD_RESET_EXECUTE) {
			/* Sink: detect disconnect by monitoring VBUS */
			set_state(port, PD_STATE_SNK_DISCONNECTED);
			/* Clear the input current limit */
			pd_set_input_current_limit(port, 0, 0);
#ifdef CONFIG_CHARGE_MANAGER
			typec_set_input_current_limit(port, 0, 0);
			charge_manager_set_ceil(port, CHARGE_CEIL_NONE);
#endif
			/* set timeout small to reconnect fast */
			timeout = 5*MSEC;
		}
#endif /* CONFIG_USB_PD_DUAL_ROLE */
	}
}

void pd_rx_event(int port)
{
	task_set_event(PORT_TO_TASK_ID(port), PD_EVENT_RX, 0);
}

#ifdef CONFIG_USB_PD_DUAL_ROLE
static void dual_role_on(void)
{
	pd_set_dual_role(PD_DRP_TOGGLE_ON);
	CPRINTS("chipset -> S0, enable dual-role toggling");
}
DECLARE_HOOK(HOOK_CHIPSET_RESUME, dual_role_on, HOOK_PRIO_DEFAULT);

static void dual_role_off(void)
{
	pd_set_dual_role(PD_DRP_TOGGLE_OFF);
	CPRINTS("chipset -> S3, disable dual-role toggling");
}
DECLARE_HOOK(HOOK_CHIPSET_SUSPEND, dual_role_off, HOOK_PRIO_DEFAULT);
DECLARE_HOOK(HOOK_CHIPSET_STARTUP, dual_role_off, HOOK_PRIO_DEFAULT);

static void dual_role_force_sink(void)
{
	pd_set_dual_role(PD_DRP_FORCE_SINK);
	CPRINTS("chipset -> S5, force dual-role port to sink");
}
DECLARE_HOOK(HOOK_CHIPSET_SHUTDOWN, dual_role_force_sink, HOOK_PRIO_DEFAULT);

#ifdef HAS_TASK_CHIPSET
static void dual_role_init(void)
{
	if (chipset_in_state(CHIPSET_STATE_ANY_OFF))
		dual_role_force_sink();
	else if (chipset_in_state(CHIPSET_STATE_SUSPEND))
		dual_role_off();
	else /* CHIPSET_STATE_ON */
		dual_role_on();
}
DECLARE_HOOK(HOOK_INIT, dual_role_init, HOOK_PRIO_DEFAULT);
#endif /* HAS_TASK_CHIPSET */
#endif /* CONFIG_USB_PD_DUAL_ROLE */

#ifdef CONFIG_COMMON_RUNTIME
void pd_set_suspend(int port, int enable)
{
	set_state(port, enable ? PD_STATE_SUSPENDED : PD_DEFAULT_STATE);

	task_wake(PORT_TO_TASK_ID(port));
}

#ifdef CONFIG_CMD_PD
static int hex8tou32(char *str, uint32_t *val)
{
	char *ptr = str;
	uint32_t tmp = 0;

	while (*ptr) {
		char c = *ptr++;
		if (c >= '0' && c <= '9')
			tmp = (tmp << 4) + (c - '0');
		else if (c >= 'A' && c <= 'F')
			tmp = (tmp << 4) + (c - 'A' + 10);
		else if (c >= 'a' && c <= 'f')
			tmp = (tmp << 4) + (c - 'a' + 10);
		else
			return EC_ERROR_INVAL;
	}
	if (ptr != str + 8)
		return EC_ERROR_INVAL;
	*val = tmp;
	return EC_SUCCESS;
}

static int remote_flashing(int argc, char **argv)
{
	int port, cnt, cmd;
	uint32_t data[VDO_MAX_SIZE-1];
	char *e;
	static int flash_offset[PD_PORT_COUNT];

	if (argc < 4 || argc > (VDO_MAX_SIZE + 4 - 1))
		return EC_ERROR_PARAM_COUNT;

	port = strtoi(argv[1], &e, 10);
	if (*e || port >= PD_PORT_COUNT)
		return EC_ERROR_PARAM2;

	cnt = 0;
	if (!strcasecmp(argv[3], "erase")) {
		cmd = VDO_CMD_FLASH_ERASE;
		flash_offset[port] = 0;
		ccprintf("ERASE ...");
	} else if (!strcasecmp(argv[3], "reboot")) {
		cmd = VDO_CMD_REBOOT;
		ccprintf("REBOOT ...");
	} else if (!strcasecmp(argv[3], "signature")) {
		cmd = VDO_CMD_ERASE_SIG;
		ccprintf("ERASE SIG ...");
	} else if (!strcasecmp(argv[3], "info")) {
		cmd = VDO_CMD_READ_INFO;
		ccprintf("INFO...");
	} else if (!strcasecmp(argv[3], "version")) {
		cmd = VDO_CMD_VERSION;
		ccprintf("VERSION...");
	} else {
		int i;
		argc -= 3;
		for (i = 0; i < argc; i++)
			if (hex8tou32(argv[i+3], data + i))
				return EC_ERROR_INVAL;
		cmd = VDO_CMD_FLASH_WRITE;
		cnt = argc;
		ccprintf("WRITE %d @%04x ...", argc * 4,
			 flash_offset[port]);
		flash_offset[port] += argc * 4;
	}

	pd_send_vdm(port, USB_VID_GOOGLE, cmd, data, cnt);

	/* Wait until VDM is done */
	while (pd[port].vdm_state > 0)
		task_wait_event(100*MSEC);

	ccprintf("DONE %d\n", pd[port].vdm_state);
	return EC_SUCCESS;
}
#endif

#if defined(CONFIG_USB_PD_ALT_MODE) && !defined(CONFIG_USB_PD_ALT_MODE_DFP)
void pd_send_hpd(int port, enum hpd_event hpd)
{
	uint32_t data[1];
	int opos = pd_alt_mode(port);
	if (!opos)
		return;

	data[0] = VDO_DP_STATUS((hpd == hpd_irq),  /* IRQ_HPD */
				(hpd == hpd_high), /* HPD_HI|LOW */
				0,		      /* request exit DP */
				0,		      /* request exit USB */
				0,		      /* MF pref */
				gpio_get_level(GPIO_PD_SBU_ENABLE),
				0,		      /* power low */
				0x2);
	pd_send_vdm(port, USB_SID_DISPLAYPORT,
		    VDO_OPOS(opos) | CMD_ATTENTION, data, 1);
	/* Wait until VDM is done. */
	while (pd[0].vdm_state > 0)
		task_wait_event(USB_PD_RX_TMOUT_US * (PD_RETRY_COUNT + 1));
}
#endif

void pd_request_source_voltage(int port, int mv)
{
	pd_set_max_voltage(mv);

	if (pd[port].task_state == PD_STATE_SNK_READY) {
		/* Set flag to send new power request in pd_task */
		pd[port].new_power_request = 1;
	} else {
		pd[port].power_role = PD_ROLE_SINK;
		pd_set_host_mode(port, 0);
		set_state(port, PD_STATE_SNK_DISCONNECTED);
	}

	task_wake(PORT_TO_TASK_ID(port));
}

static int command_pd(int argc, char **argv)
{
	int port;
	char *e;

	if (argc < 2)
		return EC_ERROR_PARAM_COUNT;

#ifdef CONFIG_CMD_PD
	/* command: pd <subcmd> <args> */
	if (!strcasecmp(argv[1], "dualrole")) {
		if (argc < 3) {
			ccprintf("dual-role toggling: ");
			switch (drp_state) {
			case PD_DRP_TOGGLE_ON:
				ccprintf("on\n");
				break;
			case PD_DRP_TOGGLE_OFF:
				ccprintf("off\n");
				break;
			case PD_DRP_FORCE_SINK:
				ccprintf("force sink\n");
				break;
			case PD_DRP_FORCE_SOURCE:
				ccprintf("force source\n");
				break;
			}
		} else {
			if (!strcasecmp(argv[2], "on"))
				pd_set_dual_role(PD_DRP_TOGGLE_ON);
			else if (!strcasecmp(argv[2], "off"))
				pd_set_dual_role(PD_DRP_TOGGLE_OFF);
			else if (!strcasecmp(argv[2], "sink"))
				pd_set_dual_role(PD_DRP_FORCE_SINK);
			else if (!strcasecmp(argv[2], "source"))
				pd_set_dual_role(PD_DRP_FORCE_SOURCE);
			else
				return EC_ERROR_PARAM3;
		}
		return EC_SUCCESS;
	} else
#endif
	if (!strcasecmp(argv[1], "dump")) {
		int level;

		if (argc < 3)
			ccprintf("dump level: %d\n", debug_level);
		else {
			level = strtoi(argv[2], &e, 10);
			if (*e)
				return EC_ERROR_PARAM2;
			debug_level = level;
		}
		return EC_SUCCESS;
	}
#ifdef CONFIG_CMD_PD
	else if (!strcasecmp(argv[1], "enable")) {
		int enable;

		if (argc < 3)
			return EC_ERROR_PARAM_COUNT;

		enable = strtoi(argv[2], &e, 10);
		if (*e)
			return EC_ERROR_PARAM3;
		pd_comm_enable(enable);
		ccprintf("Ports %s\n", enable ? "enabled" : "disabled");
		return EC_SUCCESS;
	} else if (!strncasecmp(argv[1], "rwhashtable", 3)) {
		int i;
		struct ec_params_usb_pd_rw_hash_entry *p;
		for (i = 0; i < RW_HASH_ENTRIES; i++) {
			p = &rw_hash_table[i];
			pd_dev_dump_info(p->dev_id, p->dev_rw_hash);
		}
		return EC_SUCCESS;
	}

#endif
	/* command: pd <port> <subcmd> [args] */
	port = strtoi(argv[1], &e, 10);
	if (argc < 3)
		return EC_ERROR_PARAM_COUNT;
	if (*e || port >= PD_PORT_COUNT)
		return EC_ERROR_PARAM2;
#ifdef CONFIG_CMD_PD

	if (!strcasecmp(argv[2], "tx")) {
		set_state(port, PD_STATE_SNK_DISCOVERY);
		task_wake(PORT_TO_TASK_ID(port));
	} else if (!strcasecmp(argv[2], "bist")) {
		set_state(port, PD_STATE_BIST);
		task_wake(PORT_TO_TASK_ID(port));
	} else if (!strcasecmp(argv[2], "charger")) {
		pd[port].power_role = PD_ROLE_SOURCE;
		pd_set_host_mode(port, 1);
		set_state(port, PD_STATE_SRC_DISCONNECTED);
		task_wake(PORT_TO_TASK_ID(port));
	} else if (!strncasecmp(argv[2], "dev", 3)) {
		int max_volt = -1;
		if (argc >= 4)
			max_volt = strtoi(argv[3], &e, 10) * 1000;

		pd_request_source_voltage(port, max_volt);
	} else if (!strcasecmp(argv[2], "clock")) {
		int freq;

		if (argc < 4)
			return EC_ERROR_PARAM2;

		freq = strtoi(argv[3], &e, 10);
		if (*e)
			return EC_ERROR_PARAM2;
		pd_set_clock(port, freq);
		ccprintf("set TX frequency to %d Hz\n", freq);
	} else if (!strncasecmp(argv[2], "hard", 4)) {
		set_state(port, PD_STATE_HARD_RESET_SEND);
		task_wake(PORT_TO_TASK_ID(port));
	} else if (!strncasecmp(argv[2], "hash", 4)) {
		int i;
		for (i = 0; i < PD_RW_HASH_SIZE / 4; i++)
			ccprintf("%08x ", pd[port].dev_rw_hash[i]);
		ccprintf("\n");
	} else if (!strncasecmp(argv[2], "soft", 4)) {
		set_state(port, PD_STATE_SOFT_RESET);
		task_wake(PORT_TO_TASK_ID(port));
	} else if (!strncasecmp(argv[2], "swap", 4)) {
		if (argc < 4)
			return EC_ERROR_PARAM_COUNT;

		if (!strncasecmp(argv[3], "power", 5))
			pd_request_power_swap(port);
		else if (!strncasecmp(argv[3], "data", 4))
			pd_request_data_swap(port);
		else
			return EC_ERROR_PARAM3;
	} else if (!strncasecmp(argv[2], "ping", 4)) {
		int enable;

		if (argc > 3) {
			enable = strtoi(argv[3], &e, 10);
			if (*e)
				return EC_ERROR_PARAM3;
			pd_ping_enable(port, enable);
		}

		ccprintf("Pings %s\n",
			 (pd[port].flags & PD_FLAGS_PING_ENABLED) ?
			 "on" : "off");
	} else if (!strncasecmp(argv[2], "vdm", 3)) {
		if (argc < 4)
			return EC_ERROR_PARAM_COUNT;

		if (!strncasecmp(argv[3], "ping", 4)) {
			uint32_t enable;
			if (argc < 5)
				return EC_ERROR_PARAM_COUNT;
			enable = strtoi(argv[4], &e, 10);
			if (*e)
				return EC_ERROR_PARAM4;
			pd_send_vdm(port, USB_VID_GOOGLE, VDO_CMD_PING_ENABLE,
				    &enable, 1);
		} else if (!strncasecmp(argv[3], "curr", 4)) {
			pd_send_vdm(port, USB_VID_GOOGLE, VDO_CMD_CURRENT,
				    NULL, 0);
		} else {
			return EC_ERROR_PARAM_COUNT;
		}
	} else if (!strncasecmp(argv[2], "flash", 4)) {
		return remote_flashing(argc, argv);
	} else
#endif
	if (!strncasecmp(argv[2], "state", 5)) {
		const char * const state_names[] = {
			"DISABLED", "SUSPENDED",
#ifdef CONFIG_USB_PD_DUAL_ROLE
			"SNK_DISCONNECTED", "SNK_HARD_RESET_RECOVER",
			"SNK_DISCOVERY", "SNK_REQUESTED",
			"SNK_TRANSITION", "SNK_READY", "SNK_DR_SWAP",
			"SNK_SWAP_INIT", "SNK_SWAP_SNK_DISABLE",
			"SNK_SWAP_SRC_DISABLE", "SNK_SWAP_STANDBY",
			"SNK_SWAP_COMPLETE",
#endif /* CONFIG_USB_PD_DUAL_ROLE */
			"SRC_DISCONNECTED", "SRC_HARD_RESET_RECOVER",
			"SRC_STARTUP", "SRC_DISCOVERY",
			"SRC_NEGOCIATE", "SRC_ACCEPTED", "SRC_POWERED",
			"SRC_TRANSITION", "SRC_READY", "SRC_DR_SWAP",
#ifdef CONFIG_USB_PD_DUAL_ROLE
			"SRC_SWAP_INIT", "SRC_SWAP_SNK_DISABLE",
			"SRC_SWAP_SRC_DISABLE", "SRC_SWAP_STANDBY",
#endif /* CONFIG_USB_PD_DUAL_ROLE */
			"SOFT_RESET", "HARD_RESET_SEND", "HARD_RESET_EXECUTE",
			"BIST",
		};
		BUILD_ASSERT(ARRAY_SIZE(state_names) == PD_STATE_COUNT);
		ccprintf("Port C%d, %s - Role: %s-%s Polarity: CC%d "
			 "Contract: %s, Partner: %s%s, State: %s\n",
			port, pd_comm_enabled ? "Ena" : "Dis",
			pd[port].power_role == PD_ROLE_SOURCE ? "SRC" : "SNK",
			pd[port].data_role == PD_ROLE_DFP ? "DFP" : "UFP",
			pd[port].polarity + 1,
			pd[port].flags & PD_FLAGS_EXPLICIT_CONTRACT ?
				"Yes" : "No",
			(pd[port].flags & PD_FLAGS_PARTNER_DR_POWER) ?
				"PR_SWAP," : "",
			(pd[port].flags & PD_FLAGS_PARTNER_DR_DATA) ?
				"DR_SWAP" : "",
			state_names[pd[port].task_state]);
	} else {
		return EC_ERROR_PARAM1;
	}

	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(pd, command_pd,
			"dualrole|dump|enable [0|1]|rwhashtable|\n\t<port> "
			"[tx|bist|charger|clock|dev"
			"|soft|hash|hard|ping|state|swap [power|data]|"
			"vdm [ping | curr]]",
			"USB PD",
			NULL);

#ifdef CONFIG_USBC_SS_MUX
static int command_typec(int argc, char **argv)
{
	const char * const mux_name[] = {"none", "usb", "dp", "dock"};
	char *e;
	int port;
	enum typec_mux mux = TYPEC_MUX_NONE;
	int i;

	if (argc < 2)
		return EC_ERROR_PARAM_COUNT;

	port = strtoi(argv[1], &e, 10);
	if (*e || port >= PD_PORT_COUNT)
		return EC_ERROR_PARAM1;

	if (argc < 3) {
		const char *dp_str, *usb_str;
		ccprintf("Port C%d: CC1 %d mV  CC2 %d mV (polarity:CC%d)\n",
			port, pd_adc_read(port, 0), pd_adc_read(port, 1),
			pd_get_polarity(port) + 1);
		if (board_get_usb_mux(port, &dp_str, &usb_str))
			ccprintf("Superspeed %s%s%s\n",
				 dp_str ? dp_str : "",
				 dp_str && usb_str ? "+" : "",
				 usb_str ? usb_str : "");
		else
			ccprintf("No Superspeed connection\n");

		return EC_SUCCESS;
	}

	for (i = 0; i < ARRAY_SIZE(mux_name); i++)
		if (!strcasecmp(argv[2], mux_name[i]))
			mux = i;
	board_set_usb_mux(port, mux, pd_get_polarity(port));
	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(typec, command_typec,
			"<port> [none|usb|dp|dock]",
			"Control type-C connector muxing",
			NULL);
#endif /* CONFIG_USBC_SS_MUX */

static int hc_pd_ports(struct host_cmd_handler_args *args)
{
	struct ec_response_usb_pd_ports *r = args->response;
	r->num_ports = PD_PORT_COUNT;

	args->response_size = sizeof(*r);
	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_USB_PD_PORTS,
		     hc_pd_ports,
		     EC_VER_MASK(0));

static const enum pd_dual_role_states dual_role_map[USB_PD_CTRL_ROLE_COUNT] = {
	[USB_PD_CTRL_ROLE_TOGGLE_ON]    = PD_DRP_TOGGLE_ON,
	[USB_PD_CTRL_ROLE_TOGGLE_OFF]   = PD_DRP_TOGGLE_OFF,
	[USB_PD_CTRL_ROLE_FORCE_SINK]   = PD_DRP_FORCE_SINK,
	[USB_PD_CTRL_ROLE_FORCE_SOURCE] = PD_DRP_FORCE_SOURCE,
};

#ifdef CONFIG_USBC_SS_MUX
static const enum typec_mux typec_mux_map[USB_PD_CTRL_MUX_COUNT] = {
	[USB_PD_CTRL_MUX_NONE] = TYPEC_MUX_NONE,
	[USB_PD_CTRL_MUX_USB]  = TYPEC_MUX_USB,
	[USB_PD_CTRL_MUX_AUTO] = TYPEC_MUX_DP,
	[USB_PD_CTRL_MUX_DP]   = TYPEC_MUX_DP,
	[USB_PD_CTRL_MUX_DOCK] = TYPEC_MUX_DOCK,
};
#endif

static int hc_usb_pd_control(struct host_cmd_handler_args *args)
{
	const struct ec_params_usb_pd_control *p = args->params;
	struct ec_response_usb_pd_control *r = args->response;

	if (p->role >= USB_PD_CTRL_ROLE_COUNT ||
	    p->mux >= USB_PD_CTRL_MUX_COUNT)
		return EC_RES_INVALID_PARAM;

	if (p->role != USB_PD_CTRL_ROLE_NO_CHANGE)
		pd_set_dual_role(dual_role_map[p->role]);

#ifdef CONFIG_USBC_SS_MUX
	if (p->mux != USB_PD_CTRL_MUX_NO_CHANGE)
		board_set_usb_mux(p->port, typec_mux_map[p->mux],
				  pd_get_polarity(p->port));
#endif /* CONFIG_USBC_SS_MUX */
	r->enabled = pd_comm_enabled;
	r->role = pd[p->port].power_role;
	r->polarity = pd[p->port].polarity;
	r->state = pd[p->port].task_state;
	args->response_size = sizeof(*r);
	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_USB_PD_CONTROL,
		     hc_usb_pd_control,
		     EC_VER_MASK(0));

static int hc_remote_flash(struct host_cmd_handler_args *args)
{
	const struct ec_params_usb_pd_fw_update *p = args->params;
	int port = p->port;
	const uint32_t *data = &(p->size) + 1;
	int i, size;

	if (p->size + sizeof(*p) > args->params_size)
		return EC_RES_INVALID_PARAM;

	switch (p->cmd) {
	case USB_PD_FW_REBOOT:
		pd_send_vdm(port, USB_VID_GOOGLE, VDO_CMD_REBOOT, NULL, 0);

		/* Delay to give time for device to reboot */
		usleep(900 * MSEC);
		return EC_RES_SUCCESS;

	case USB_PD_FW_FLASH_ERASE:
		pd_send_vdm(port, USB_VID_GOOGLE, VDO_CMD_FLASH_ERASE, NULL, 0);

		/* Wait until VDM is done */
		while (pd[port].vdm_state > 0)
			task_wait_event(100*MSEC);
		break;

	case USB_PD_FW_ERASE_SIG:
		pd_send_vdm(port, USB_VID_GOOGLE, VDO_CMD_ERASE_SIG, NULL, 0);

		/* Wait until VDM is done */
		while (pd[port].vdm_state > 0)
			task_wait_event(100*MSEC);
		break;

	case USB_PD_FW_FLASH_WRITE:
		/* Data size must be a multiple of 4 */
		if (!p->size || p->size % 4)
			return EC_RES_INVALID_PARAM;

		size = p->size / 4;
		for (i = 0; i < size; i += VDO_MAX_SIZE - 1) {
			pd_send_vdm(port, USB_VID_GOOGLE, VDO_CMD_FLASH_WRITE,
				    data + i, MIN(size - i, VDO_MAX_SIZE - 1));

			/* Wait until VDM is done */
			while (pd[port].vdm_state > 0)
				task_wait_event(10*MSEC);
		}
		break;

	default:
		return EC_RES_INVALID_PARAM;
		break;
	}

	if (pd[port].vdm_state < 0)
		return EC_RES_ERROR;
	else
		return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_USB_PD_FW_UPDATE,
		     hc_remote_flash,
		     EC_VER_MASK(0));

static int hc_remote_rw_hash_entry(struct host_cmd_handler_args *args)
{
	int i, idx = 0, found = 0;
	const struct ec_params_usb_pd_rw_hash_entry *p = args->params;
	static int rw_hash_next_idx;

	if (!p->dev_id)
		return EC_RES_INVALID_PARAM;

	for (i = 0; i < RW_HASH_ENTRIES; i++) {
		if (p->dev_id == rw_hash_table[i].dev_id) {
			idx = i;
			found = 1;
			break;
		}
	}
	if (!found) {
		idx = rw_hash_next_idx;
		rw_hash_next_idx = rw_hash_next_idx + 1;
		if (rw_hash_next_idx == RW_HASH_ENTRIES)
			rw_hash_next_idx = 0;
	}
	memcpy(&rw_hash_table[idx], p, sizeof(*p));

	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_USB_PD_RW_HASH_ENTRY,
		     hc_remote_rw_hash_entry,
		     EC_VER_MASK(0));

static int hc_remote_pd_dev_info(struct host_cmd_handler_args *args)
{
	const uint8_t *port = args->params;
	struct ec_params_usb_pd_rw_hash_entry *r = args->response;

	if (*port >= PD_PORT_COUNT)
		return EC_RES_INVALID_PARAM;

	r->dev_id = pd[*port].dev_id;

	if (r->dev_id) {
		memcpy(r->dev_rw_hash, pd[*port].dev_rw_hash,
		       PD_RW_HASH_SIZE);
	}

	args->response_size = sizeof(*r);
	return EC_RES_SUCCESS;
}

DECLARE_HOST_COMMAND(EC_CMD_USB_PD_DEV_INFO,
		     hc_remote_pd_dev_info,
		     EC_VER_MASK(0));

#endif /* CONFIG_COMMON_RUNTIME */
