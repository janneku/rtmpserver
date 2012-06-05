#ifndef __rtmp_h
#define __rtmp_h

#define PORT	1935

#define DEFAULT_CHUNK_LEN	128

#define PACKED	__attribute__((packed))

#define HANDSHAKE_PLAINTEXT	0x03

#define RANDOM_LEN		(1536 - 8)

#define MSG_SET_CHUNK		0x01
#define MSG_BYTES_READ		0x03
#define MSG_USER_CONTROL	0x04
#define MSG_RESPONSE		0x05
#define MSG_REQUEST		0x06
#define MSG_AUDIO		0x08
#define MSG_VIDEO		0x09
#define MSG_INVOKE3		0x11	/* AMF3 */
#define MSG_NOTIFY		0x12
#define MSG_OBJECT		0x13
#define MSG_INVOKE		0x14	/* AMF0 */
#define MSG_FLASH_VIDEO		0x16

#define CONTROL_CLEAR_STREAM	0x00
#define CONTROL_CLEAR_BUFFER	0x01
#define CONTROL_STREAM_DRY	0x02
#define CONTROL_BUFFER_TIME	0x03
#define CONTROL_RESET_STREAM	0x04
#define CONTROL_PING		0x06
#define CONTROL_REQUEST_VERIFY	0x1a
#define CONTROL_RESPOND_VERIFY	0x1b
#define CONTROL_BUFFER_EMPTY	0x1f
#define CONTROL_BUFFER_READY	0x20

#define CONTROL_ID		0
#define STREAM_ID		1337

#define CHAN_CONTROL		2
#define CHAN_RESULT		3
#define CHAN_STREAM		4

#define FLV_KEY_FRAME		0x01
#define FLV_INTER_FRAME		0x02

struct Handshake {
	uint8_t flags[8];
	uint8_t random[RANDOM_LEN];
} PACKED;

struct RTMP_Header {
	uint8_t flags;
	char timestamp[3];
	char msg_len[3];
	uint8_t msg_type;
	char endpoint[4]; /* Note, this is little-endian while others are BE */
} PACKED;

#endif
