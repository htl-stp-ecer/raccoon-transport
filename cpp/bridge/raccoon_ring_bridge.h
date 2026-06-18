#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int raccoon_ring_bridge_node_create(void** out_node, const char* name);
void raccoon_ring_bridge_node_destroy(void* node);

int raccoon_ring_bridge_publisher_create(void* node, const char* channel, void** out_pub);
int raccoon_ring_bridge_publisher_send(void* pub, const uint8_t* data, size_t len);
/* As publisher_send, but when `deduplicate` is non-zero a byte-identical
 * value-channel payload is dropped (command channels are never dropped).
 * The dedup policy is shared with raccoon::Transport via raccoon::dedup. */
int raccoon_ring_bridge_publisher_send_ex(void* pub, const uint8_t* data,
                                          size_t len, int deduplicate);
void raccoon_ring_bridge_publisher_destroy(void* pub);

int raccoon_ring_bridge_subscriber_create(void* node, const char* channel, void** out_sub);
int raccoon_ring_bridge_subscriber_receive(void* sub, uint8_t* buf, size_t* out_len, size_t max_len);
void raccoon_ring_bridge_subscriber_destroy(void* sub);

#ifdef __cplusplus
}
#endif
