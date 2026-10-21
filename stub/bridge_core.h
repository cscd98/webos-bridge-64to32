#pragma once
/*
 * bridge_core.h — internal API for the aarch64 stub side.
 */
#include "gles_bridge_protocol.h"
#include <stdint.h>
#include <string.h>

void bridge_begin(void);
uint32_t bridge_data_write(const void *src, size_t size);
void bridge_data_read(void *dst, uint32_t offset, size_t size);
uint8_t *bridge_data_ptr(uint32_t offset);
void bridge_send_void(void);
uint64_t bridge_send_call(void);
uint64_t bridge_send_swap(void);
BridgeCtrl *bridge_ctrl(void);
uint8_t *bridge_result_buf(void);
uint32_t bridge_result_len(void);
uint64_t bridge_result_val(void);

/* arg is evaluated and discarded so existing `BridgeCtrl *C2` locals
 * don't become unused-variable warnings */
#define GL_RESULT(C) ((void)(C), bridge_result_buf())

#define GL_RESULT_LEN() bridge_result_len()
#define GL_RESULT_VAL() bridge_result_val()

/*
 * Convenience macros used in gles2_stub.c
 *
 * Usage pattern for a void call with only scalar args:
 *
 *   BRIDGE_BEGIN();
 *   BridgeCtrl *C = bridge_ctrl();
 *   C->opcode = OP_glFoo;
 *   ArgWriter W = aw_init(C->args, BRIDGE_ARGS_SIZE);
 *   aw_u32(&W, bar);
 *   aw_f32(&W, baz);
 *   C->args_len  = W.pos;
 *   C->data_offset = 0; C->data_size = 0;
 *   C->data2_offset = 0; C->data2_size = 0;
 *   BRIDGE_SEND_VOID();
 *
 * For a call with a return value:
 *   ...
 *   GLuint ret = (GLuint)BRIDGE_SEND_CALL();
 */
#define BRIDGE_BEGIN() bridge_begin()
#define BRIDGE_SEND_VOID() bridge_send_void()
#define BRIDGE_SEND_CALL() bridge_send_call()
#define BRIDGE_CTRL() bridge_ctrl()

// ================= WL BRIDGE =================
void wl_bridge_init(void);
void wl_bridge_begin(void);
void wl_bridge_end(void);

uint64_t bridge_send_call_wl(void);
void bridge_data_read_wl(void *dst, uint32_t offset, size_t size);
uint32_t bridge_data_write_wl(const void *src, size_t size);
void bridge_send_void_wl(void);

BridgeCtrl *bridge_ctrl_wl(void);

#define BRIDGE_BEGIN_WL() wl_bridge_begin()
#define BRIDGE_SEND_VOID_WL() bridge_send_void_wl()
#define BRIDGE_SEND_CALL_WL() bridge_send_call_wl()
#define BRIDGE_CTRL_WL() bridge_ctrl_wl()

extern __thread unsigned int g_stub_current_ctx;

#ifdef DEBUG_OPCODES
void bridge_dump_backpressure_stats(void);
#endif
