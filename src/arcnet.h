/*
 * arcnet.h  --  Public API for the USB22-485 WinUSB user-mode DLL.
 *
 * Hardware  : Contemporary Controls USB22-485 (VID=0x0D0B, PID=0x1002)
 * Protocol  : reference/usb22-protocol-notes.md (UPDATE 1-6)
 * Build     : MSVC + CMake; link arcnet.lib (import lib) — arcnet.dll must be
 *             present at runtime next to the executable.
 *
 * Multiple devices can be open simultaneously: each arc_open() call
 * allocates an independent arc_ctx_t context.
 *
 * Thread safety: all API functions on the SAME context serialize via a
 * per-context CRITICAL_SECTION.  Concurrent calls on DIFFERENT contexts
 * are fully parallel.  Do not call arc_close() while another thread may
 * still be inside any API function on the same context.
 *
 * Logging: per-context, level-filtered, optionally redirected via callback.
 * Default level is ARC_LOG_NONE (completely silent — production friendly).
 */

#ifndef ARCNET_H
#define ARCNET_H

#include <stdint.h>
#include <stdbool.h>

/* -----------------------------------------------------------------------
 * DLL export / import
 *   When building arcnet.dll define ARCNET_BUILD_DLL (set automatically
 *   by CMake via target_compile_definitions on the arcnet target).
 *   Consumers that include this header do NOT define it — they get imports.
 * --------------------------------------------------------------------- */
#ifdef ARCNET_BUILD_DLL
#  define ARCNET_API __declspec(dllexport)
#else
#  define ARCNET_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * Device identification
 * --------------------------------------------------------------------- */
#define ARC_VID                 0x0D0Bu
#define ARC_PID                 0x1002u

/* DeviceInterfaceGUID written by usb22_winusb.inf into the registry.
 * Must match [Dev_AddReg] in driver/usb22_winusb.inf exactly. */
#define ARC_DEVICE_INTERFACE_GUID_STR  "{E6B4B5C0-F74E-4A1D-9B8F-2C3D4E5F6A7B}"

/* -----------------------------------------------------------------------
 * USB endpoint addresses  (confirmed by winusb_probe)
 * --------------------------------------------------------------------- */
#define ARC_EP_CMD_OUT          0x01u   /* Command channel (host -> device)         */
#define ARC_EP_EVT_IN           0x81u   /* Response + async events (device -> host) */
#define ARC_EP_TX_OUT           0x02u   /* ARCNET transmit data (host -> device)    */
#define ARC_EP_RX_IN            0x86u   /* ARCNET receive data  (device -> host)    */

/* -----------------------------------------------------------------------
 * Protocol opcodes  (protocol notes UPDATE 2 + 4)
 * --------------------------------------------------------------------- */
#define ARC_OPCODE_INIT         0x00u   /* 12 B command / 6 B response              */
#define ARC_OPCODE_REGISTER     0x01u   /*  5 B command / 7 B response              */
#define ARC_OPCODE_CMD04        0x04u   /*  2 B command / 4 B response (session start) */
#define ARC_OPCODE_EVENT        0x20u   /* Unsolicited RECON/status event from device  */

/* -----------------------------------------------------------------------
 * Timing constants (milliseconds)
 * --------------------------------------------------------------------- */
#define ARC_EP_EVT_MAXPACKET    64u     /* EP 0x81 MaxPacketSize (measured)         */
#define ARC_READ_TIMEOUT_MS     1000u   /* PIPE_TRANSFER_TIMEOUT on EP 0x81         */
#define ARC_BUDGET_SHORT_MS     1500u   /* Response budget: cmd04 / register        */
#define ARC_BUDGET_INIT_MS      5000u   /* Response budget: init (~2.5 s on device) */
#define ARC_RECEIVE_TIMEOUT_MS  150u    /* EP 0x86 / EP 0x02 timeout per arc_receive() poll  */
#define ARC_TRANSMIT_TIMEOUT_MS  2000u  /* EP 0x02 timeout for real arc_transmit()           */
#define ARC_ACK_POLL_BUDGET_MS    100u  /* How long to poll reg0 for TMA bit after transmit  */
#define ARC_ACK_POLL_INTERVAL_MS   10u  /* Sleep between reg0 reads in ACK poll loop         */

/* -----------------------------------------------------------------------
 * Log levels  (per-context; default ARC_LOG_NONE)
 * --------------------------------------------------------------------- */
typedef enum {
    ARC_LOG_NONE  = 0,   /* completely silent                              */
    ARC_LOG_ERROR = 1,   /* fatal errors only (device gone, IO fail, ...)  */
    ARC_LOG_INFO  = 2,   /* notable events: open, init, reconnect, close   */
    ARC_LOG_DEBUG = 3,   /* full protocol trace (current verbose behaviour)*/
} arc_log_level_t;

/* Log callback — set via arc_set_log_callback().
 *   level : severity of this message
 *   msg   : NUL-terminated, includes "[arcnet] " prefix and trailing '\n'
 *   user  : opaque pointer passed to arc_set_log_callback()              */
typedef void (*arc_log_fn)(arc_log_level_t level, const char *msg, void *user);

/* -----------------------------------------------------------------------
 * Result codes
 * --------------------------------------------------------------------- */
typedef enum {
    ARC_OK           =  0,  /* Success (and ACK received if waitAck=true)  */
    ARC_NO_PACKET    =  1,  /* arc_receive: channel empty, try again       */
    ARC_NOT_ACKED       =  2,  /* arc_transmit: sent but no ACK within budget          */
    ARC_ERR_OPEN        = -1,  /* Device not found or CreateFile failed                */
    ARC_ERR_IO          = -2,  /* USB read/write failure (transient)                   */
    ARC_ERR_TIMEOUT     = -3,  /* Response budget exhausted                            */
    ARC_ERR_PARAM       = -4,  /* Invalid argument                                     */
    ARC_ERR_ECHO        = -5,  /* Response echo mismatch                               */
    ARC_ERR_DEVICE_GONE = -6,  /* Device physically removed; call arc_reopen() + init  */
    ARC_ERR_NET_BUSY    = -7,  /* ARCNET RECON / transmitter not available (transient)  */
} arc_result_t;

/* -----------------------------------------------------------------------
 * Device context — opaque; allocated by arc_open, freed by arc_close.
 * Each context is an independent device session; multiple contexts can
 * exist simultaneously (one per physical USB22-485 adapter).
 * --------------------------------------------------------------------- */
typedef struct arc_ctx_s arc_ctx_t;

/* -----------------------------------------------------------------------
 * API
 * --------------------------------------------------------------------- */

/* Return a short string for a result code. */
ARCNET_API const char *arc_result_str(arc_result_t r);

/* Enumerate CC ARCNET devices.
 *   paths[]    : caller-supplied array; each element holds a device path.
 *   maxDevices : capacity of paths[].
 *   Returns    : number of devices found (0 = none). */
ARCNET_API int arc_list_devices(char paths[][256], int maxDevices);

/* Open a device and allocate a context.
 *   devicePath : WinUSB interface path (from arc_list_devices).
 *                Pass NULL to auto-select the first CC ARCNET device.
 *   verbose    : enable diagnostic output to stdout/stderr.
 *   Returns    : non-NULL context on success, NULL on failure.
 *   The returned context must be released with arc_close(). */
ARCNET_API arc_ctx_t *arc_open(const char *devicePath, bool verbose);

/* Set the log level for an open context.
 *   ARC_LOG_NONE  (0) — silent (default)
 *   ARC_LOG_ERROR (1) — errors only
 *   ARC_LOG_INFO  (2) — open / init / reconnect / close events + errors
 *   ARC_LOG_DEBUG (3) — full protocol trace                              */
ARCNET_API void arc_set_log_level(arc_ctx_t *ctx, arc_log_level_t level);

/* Set a log callback.  When fn is non-NULL all log output goes to fn
 * instead of stderr.  Pass fn=NULL to restore the default (stderr).     */
ARCNET_API void arc_set_log_callback(arc_ctx_t *ctx, arc_log_fn fn, void *user);

/* Backward-compatible verbose toggle:
 *   enable=true  -> ARC_LOG_DEBUG
 *   enable=false -> ARC_LOG_NONE                                         */
ARCNET_API void arc_set_verbose(arc_ctx_t *ctx, bool enable);

/* Initialize the ARCNET node.
 *   Runs the full startup sequence:
 *     (1) cmd04  -- session-start command (UPDATE 4)
 *     (2) handshake -- data-channel probe (UPDATE 5)
 *     (3) COM20022 config command (UPDATE 6; waits up to 5 s for response)
 *
 *   nodeID         : this node's ARCNET address (0x01..0xFE)
 *   timeout        : reconfiguration timeout byte (0x18 recommended)
 *   clockPrescaler : COM20022 clock divisor (0x00 = default speed)
 *   recvBroadcasts : receive broadcast frames (destination = node 0)
 *
 *   Returns ARC_OK on success. */
ARCNET_API arc_result_t arc_init(arc_ctx_t *ctx, uint8_t nodeID, uint8_t timeout,
                                 uint8_t clockPrescaler, bool recvBroadcasts);

/* Read or write a COM20022 chip register.
 *   bWrite=false : *value is filled with the register value on return.
 *   bWrite=true  : *value is written to the register.
 *   Returns ARC_OK on success. */
ARCNET_API arc_result_t arc_register(arc_ctx_t *ctx, bool bWrite,
                                     uint8_t reg, uint8_t *value);

/* Transmit an ARCNET packet.
 *   destNode : destination ARCNET address
 *   data     : payload bytes (must not be NULL)
 *   len      : payload length, 1..252
 *   waitAck  : if true, polls COM20022 status reg 0 bit 1 (TMA) for up to
 *              ARC_ACK_POLL_BUDGET_MS after sending; returns ARC_OK if the
 *              bit is set (packet acknowledged) or ARC_NOT_ACKED if the
 *              budget expires without seeing it.
 *              if false, returns ARC_OK as soon as the USB write succeeds
 *              (higher throughput, no delivery confirmation).
 *   Returns ARC_OK        packet sent (and ACKed when waitAck=true).
 *           ARC_NOT_ACKED packet sent but no ACK within budget.
 *           ARC_ERR_IO    USB write failed. */
ARCNET_API arc_result_t arc_transmit(arc_ctx_t *ctx, uint8_t destNode,
                                     const uint8_t *data, int len, bool waitAck);

/* Poll for a received ARCNET packet (non-blocking within ARC_RECEIVE_TIMEOUT_MS).
 *   ARC_OK        : packet received; *src/*dst/*data/*len are valid.
 *   ARC_NO_PACKET : channel empty; call again later.
 *   ARC_ERR_IO    : hardware failure.
 * data[] must be at least 253 bytes. */
ARCNET_API arc_result_t arc_receive(arc_ctx_t *ctx, uint8_t *src, uint8_t *dst,
                                    uint8_t *data, int *len);

/* Read a raw event/status packet from EP 0x81 (unsolicited device -> host).
 *   timeout_ms : pipe timeout; if 0 the current policy is kept.
 *   buf        : caller buffer (>= ARC_EP_EVT_MAXPACKET bytes recommended).
 *   bufsize    : capacity of buf.
 *   out_len    : bytes received (valid only on ARC_OK).
 *   Returns ARC_OK        packet received.
 *           ARC_NO_PACKET timeout, no data.
 *           ARC_ERR_IO    USB error. */
ARCNET_API arc_result_t arc_read_event(arc_ctx_t *ctx, uint8_t *buf, int bufsize,
                                       uint32_t timeout_ms, int *out_len);

/* Attempt to reopen a context whose device has been physically removed.
 *
 *   Closes any dead handles, clears the device_gone flag, and tries to
 *   re-establish the WinUSB session using the same device path that was
 *   used by arc_open().  If the device is still absent, returns
 *   ARC_ERR_DEVICE_GONE (safe to call again later).
 *
 *   On ARC_OK the handles are valid but the ARCNET node is not yet
 *   initialized — the caller MUST call arc_init() before transmitting
 *   or receiving. */
ARCNET_API arc_result_t arc_reopen(arc_ctx_t *ctx);

/* Close device, release all resources, and free the context.
 * Safe to call with NULL. After this call the pointer is invalid. */
ARCNET_API void arc_close(arc_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ARCNET_H */
