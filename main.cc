/*
 * RTMPServer
 *
 * Copyright 2011 Janne Kulmala <janne.t.kulmala@iki.fi>
 *
 * Program code is licensed with GNU LGPL 2.1. See COPYING.LGPL file.
 */
#include "amf.h"
#include "utils.h"
#include "rtmp.h"
#include <vector>
#include <stdexcept>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/poll.h>
#include <unistd.h>

#define APP_NAME	"live"

struct RTMP_Message {
	uint8_t type;
	size_t len;
	unsigned long timestamp;
	uint32_t endpoint;
	std::string buf;
};

struct Client {
	int fd;
	bool playing; /* Wants to receive the stream? */
	bool ready; /* Wants to receive and seen a keyframe */
	size_t read_chunk_len;
	size_t write_chunk_len;
	RTMP_Message messages[64];
	std::string buf;
};

namespace {

amf_object_t metadata;
Client *publisher = NULL;
int listen_fd;
std::vector<pollfd> poll_table;
std::vector<Client *> clients;

size_t recv_all(int fd, void *buf, size_t len)
{
	size_t pos = 0;
	while (pos < len) {
		ssize_t bytes = recv(fd, (char *) buf + pos, len - pos, 0);
		if (bytes < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			throw std::runtime_error(
				strf("unable to recv: %s", strerror(errno)));
		}
		if (bytes == 0)
			break;
		pos += bytes;
	}
	return pos;
}

size_t send_all(int fd, const void *buf, size_t len)
{
	size_t pos = 0;
	while (pos < len) {
		ssize_t written = send(fd, (const char *) buf + pos, len - pos, 0);
		if (written < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			throw std::runtime_error(
				strf("unable to send: %s", strerror(errno)));
		}
		if (written == 0)
			break;
		pos += written;
	}
	return pos;
}

void hexdump(const void *buf, size_t len)
{
	const uint8_t *data = (const uint8_t *) buf;
	for (size_t i = 0; i < len; i += 16) {
		for (int j = 0; j < 16; ++j) {
			if (i + j < len)
				debug("%.2x ", data[i + j]);
			else
				debug("   ");
		}
		for (int j = 0; j < 16; ++j) {
			if (i + j < len) {
				putc((data[i + j] >= ' ') ? data[i + j] : '.',
				     stdout);
			} else {
				putc(' ', stdout);
			}
		}
		putc('\n', stdout);
	}
}

void rtmp_send(Client *client, uint8_t type, uint32_t endpoint,
		const std::string &buf, unsigned long timestamp = 0)
{
	int channel = 4;

	RTMP_Header header;
	header.flags = (channel & 0x3f) | (0 << 6);
	header.msg_type = type;
	set_be24(header.timestamp, timestamp);
	set_be24(header.msg_len, buf.size());
	set_le32(header.endpoint, endpoint);

	if (send_all(client->fd, &header, sizeof header) < sizeof header) {
		throw std::runtime_error("Unable to write to a client");
	}

	size_t pos = 0;
	while (pos < buf.size()) {
		if (pos) {
			uint8_t flags = (channel & 0x3f) | (3 << 6);
			send_all(client->fd, &flags, 1);
		}

		size_t chunk = buf.size() - pos;
		if (chunk > client->write_chunk_len)
			chunk = client->write_chunk_len;
		send_all(client->fd, &buf[pos], chunk);
		
		pos += chunk;
	}
}

void handle_connect(Client *client, const RTMP_Message *msg, Decoder *dec)
{
	double txid = amf_load_number(dec);

	amf_object_t params = amf_load_object(dec);
	std::string app = get(params, std::string("app")).as_string();

	if (app != APP_NAME) {
		throw std::runtime_error("Unsupported application: " + app);
	}

	amf_object_t version;
	version.insert(std::make_pair("fmsVer", std::string("FMS/4,5,1,484")));
	version.insert(std::make_pair("capabilities", 255.0));
	version.insert(std::make_pair("mode", 1.0));

	amf_object_t status;
	status.insert(std::make_pair("level", std::string("status")));
	status.insert(std::make_pair("code", std::string("NetConnection.Connect.Success")));
	status.insert(std::make_pair("description", std::string("Connection succeeded.")));
	/* report support for AMF3 */
	status.insert(std::make_pair("objectEncoding", 3.0));

	std::string reply;
	amf_write(reply, std::string("_result"));
	amf_write(reply, txid);
	amf_write(reply, version);
	amf_write(reply, status);
	rtmp_send(client, MSG_INVOKE, CONTROL_ID, reply);
}

void handle_fcpublish(Client *client, const RTMP_Message *msg, Decoder *dec)
{
	if (publisher != NULL) {
		throw std::runtime_error("Already have a publisher");
	}
	publisher = client;
	printf("publisher connected.\n");

	double txid = amf_load_number(dec);

	amf_load(dec); /* NULL */

	std::string path = amf_load_string(dec);
	debug("fcpublish %s\n", path.c_str());

	amf_object_t status;
	status.insert(std::make_pair("code", std::string("NetStream.Publish.Start")));
	status.insert(std::make_pair("description", path));

	std::string invoke;
	amf_write(invoke, std::string("onFCPublish"));
	amf_write(invoke, 0.0);
	amf_write(invoke, status);
	rtmp_send(client, MSG_INVOKE, CONTROL_ID, invoke);

	if (txid > 0) {
		std::string reply;
		amf_write(reply, std::string("_result"));
		amf_write(reply, txid);
		reply += char(AMF_NULL);
		reply += char(AMF_NULL);
		rtmp_send(client, MSG_INVOKE, CONTROL_ID, reply);
	}
}

void handle_createstream(Client *client, const RTMP_Message *msg, Decoder *dec)
{
	double txid = amf_load_number(dec);

	std::string reply;
	amf_write(reply, std::string("_result"));
	amf_write(reply, txid);
	reply += char(AMF_NULL);
	amf_write(reply, double(STREAM_ID));
	rtmp_send(client, MSG_INVOKE, CONTROL_ID, reply);
}

void handle_publish(Client *client, const RTMP_Message *msg, Decoder *dec)
{
	double txid = amf_load_number(dec);

	if (msg->endpoint != STREAM_ID) {
		throw std::runtime_error("stream id mismatch");
	}

	amf_load(dec);

	std::string path = amf_load_string(dec);
	debug("publish %s\n", path.c_str());

	amf_object_t status;
	status.insert(std::make_pair("code", std::string("NetStream.Publish.Start")));
	status.insert(std::make_pair("description", std::string("Stream is now published.")));

	std::string invoke;
	amf_write(invoke, std::string("onStatus"));
	amf_write(invoke, 0.0);
	amf_write(invoke, status);
	rtmp_send(client, MSG_INVOKE, STREAM_ID, invoke);

	if (txid > 0) {
		std::string reply;
		amf_write(reply, std::string("_result"));
		amf_write(reply, txid);
		reply += char(AMF_NULL);
		reply += char(AMF_NULL);
		rtmp_send(client, MSG_INVOKE, CONTROL_ID, reply);
	}
}

void handle_play(Client *client, const RTMP_Message *msg, Decoder *dec)
{
	double txid = amf_load_number(dec);

	if (msg->endpoint != STREAM_ID) {
		throw std::runtime_error("stream id mismatch");
	}

	amf_load(dec); /* NULL */

	std::string path = amf_load_string(dec);
	debug("play %s\n", path.c_str());

	amf_object_t status;
	status.insert(std::make_pair("code", std::string("NetStream.Play.Start")));
	status.insert(std::make_pair("description", std::string("Stream is now playing.")));

	std::string invoke;
	amf_write(invoke, std::string("onStatus"));
	amf_write(invoke, 0.0);
	amf_write(invoke, status);
	rtmp_send(client, MSG_INVOKE, STREAM_ID, invoke);

	if (txid > 0) {
		std::string reply;
		amf_write(reply, std::string("_result"));
		amf_write(reply, txid);
		reply += char(AMF_NULL);
		reply += char(AMF_NULL);
		rtmp_send(client, MSG_INVOKE, CONTROL_ID, reply);
	}

	client->playing = true;

	std::string notify;
	amf_write(notify, std::string("onMetaData"));
	amf_write_ecma(notify, metadata);
	rtmp_send(client, MSG_NOTIFY, STREAM_ID, notify);
}

void handle_setdataframe(Client *client, const RTMP_Message *msg, Decoder *dec)
{
	if (client != publisher) {
		throw std::runtime_error("not a publisher");
	}

	std::string type = amf_load_string(dec);
	if (type != "onMetaData") {
		throw std::runtime_error("can only set metadata");
	}

	metadata = amf_load_ecma(dec);

	std::string notify;
	amf_write(notify, std::string("onMetaData"));
	amf_write_ecma(notify, metadata);

	FOR_EACH(std::vector<Client *>, i, clients) {
		Client *client = *i;
		if (client != NULL && client->ready) {
			rtmp_send(client, MSG_NOTIFY, STREAM_ID, notify);
		}
	}
}

void handle_message(Client *client, const RTMP_Message *msg)
{
	debug("RTMP message %02x, len %zu, timestamp %ld\n", msg->type, msg->len,
		msg->timestamp);

	size_t pos = 0;

	switch (msg->type) {
	case MSG_SET_CHUNK:
		if (pos + 4 > msg->buf.size()) {
			throw std::runtime_error("Not enough data");
		}
		client->read_chunk_len = load_be32(&msg->buf[pos]);
		debug("chunk size set to %zu\n", client->read_chunk_len);
		break;

	case MSG_INVOKE: {
			Decoder dec;
			dec.buf = msg->buf;
			dec.pos = 0;
			std::string method = amf_load_string(&dec);
			debug("invoked %s\n", method.c_str());
			if (msg->endpoint == CONTROL_ID) {
				if (method == "connect") {
					handle_connect(client, msg, &dec);
				} else if (method == "FCPublish") {
					handle_fcpublish(client, msg, &dec);
				} else if (method == "createStream") {
					handle_createstream(client, msg, &dec);
				}
			} else if (msg->endpoint == STREAM_ID) {
				if (method == "publish") {
					handle_publish(client, msg, &dec);
				} else if (method == "play") {
					handle_play(client, msg, &dec);
				}
			}
		}
		break;

	case MSG_NOTIFY: {
			Decoder dec;
			dec.buf = msg->buf;
			dec.pos = 0;
			std::string type = amf_load_string(&dec);
			debug("notify %s\n", type.c_str());
			if (msg->endpoint == STREAM_ID) {
				if (type == "@setDataFrame") {
					handle_setdataframe(client, msg, &dec);
				}
			}
		}
		break;

	case MSG_AUDIO:
		if (client != publisher) {
			throw std::runtime_error("not a publisher");
		}
		FOR_EACH(std::vector<Client *>, i, clients) {
			Client *receiver = *i;
			if (receiver != NULL && receiver->ready) {
				rtmp_send(receiver, msg->type, STREAM_ID, msg->buf,
					msg->timestamp);
			}
		}
		break;

	case MSG_VIDEO: {
		if (client != publisher) {
			throw std::runtime_error("not a publisher");
		}
		uint8_t flags = msg->buf[0];
		FOR_EACH(std::vector<Client *>, i, clients) {
			Client *receiver = *i;
			if (receiver != NULL && receiver->playing) {
				if (flags >> 4 == FLV_KEY_FRAME) {
					receiver->ready = true;
				}
				if (receiver->ready) {
					rtmp_send(receiver, msg->type, STREAM_ID, msg->buf,
						  msg->timestamp);
				}
			}
		}
		}
		break;

	case MSG_FLASH_VIDEO:
		throw std::runtime_error("streaming FLV not supported");
		break;

	default:
		debug("unhandled message: %02x\n", msg->type);
		hexdump(msg->buf.data(), msg->buf.size());
		break;
	}
}

/* TODO: Make this asynchronous */
void do_handshake(Client *client)
{
	uint8_t serversig[SIG_LENGTH];
	uint8_t clientsig[SIG_LENGTH];

	uint8_t c;
	if (recv_all(client->fd, &c, 1) < 1)
		return;
	if (c != HANDSHAKE_PLAINTEXT) {
		throw std::runtime_error("only plaintext handshake supported");
	}

	if (send_all(client->fd, &c, 1) < 1)
		return;

	memset(serversig, 0, sizeof serversig);
	serversig[0] = 0x03;
	for (int i = 8; i < SIG_LENGTH; ++i) {
		serversig[i] = rand();
	}

	if (send_all(client->fd, serversig, SIG_LENGTH) != SIG_LENGTH)
		return;

	/* Echo client's signature back */
	if (recv_all(client->fd, clientsig, SIG_LENGTH) != SIG_LENGTH)
		return;
	if (send_all(client->fd, clientsig, SIG_LENGTH) != SIG_LENGTH)
		return;

	if (recv_all(client->fd, clientsig, SIG_LENGTH) != SIG_LENGTH)
		return;
	if (memcmp(serversig, clientsig, SIG_LENGTH)) {
		throw std::runtime_error("handshake failed");
	}
}

void recv_from_client(Client *client)
{
	std::string chunk(4096, 0);
	ssize_t got = recv(client->fd, &chunk[0], chunk.size(), 0);
	if (got == 0) {
		throw std::runtime_error("EOF from a client");
	} else if (got < 0) {
		if (errno == EAGAIN || errno == EINTR)
			return;
		throw std::runtime_error(strf("recv() failed: %s",
					      strerror(errno)));
	}
	client->buf.append(chunk.begin(), chunk.begin() + got);

	while (!client->buf.empty()) {
		uint8_t flags = client->buf[0];

		static const size_t HEADER_LENGTH[] = {12, 8, 4, 1};
		size_t header_len = HEADER_LENGTH[flags >> 6];

		if (client->buf.size() < header_len) {
			/* need more data */
			break;
		}

		RTMP_Header header;
		memcpy(&header, client->buf.data(), header_len);

		RTMP_Message *msg = &client->messages[flags & 0x3f];

		if (header_len >= 8) {
			msg->len = load_be24(header.msg_len);
			if (msg->len < msg->buf.size()) {
				throw std::runtime_error("invalid msg length");
			}
			msg->type = header.msg_type;
		}
		if (header_len >= 12) {
			msg->endpoint = load_le32(header.endpoint);
		}

		if (msg->len == 0) {
			throw std::runtime_error("message without a header");
		}
		size_t chunklen = msg->len - msg->buf.size();
		if (chunklen > client->read_chunk_len)
			chunklen = client->read_chunk_len;

		if (client->buf.size() < header_len + chunklen) {
			/* need more data */
			break;
		}

		if (header_len >= 4) {
			unsigned long ts = load_be24(header.timestamp);
			if (ts == 0xffffff) {
				throw std::runtime_error("ext timestamp not supported");
			}
			if (header_len < 12) {
				ts += msg->timestamp;
			}
			msg->timestamp = ts;
		}

		msg->buf.append(client->buf.begin() + header_len,
				client->buf.begin() + (header_len + chunklen));
		client->buf.erase(client->buf.begin(),
				  client->buf.begin() + (header_len + chunklen));

		if (msg->buf.size() == msg->len) {
			handle_message(client, msg);
			msg->buf.clear();
		}
	}
}

Client *new_client()
{
	sockaddr_in sin;
	socklen_t addrlen = sizeof sin;
	int fd = accept(listen_fd, (sockaddr *) &sin, &addrlen);
	if (fd < 0) {
		printf("Unable to accept a client: %s\n", strerror(errno));
		return NULL;
	}

	Client *client = new Client;
	client->playing = false;
	client->ready = false;
	client->fd = fd;
	client->read_chunk_len = DEFAULT_CHUNK_LEN;
	client->write_chunk_len = DEFAULT_CHUNK_LEN;
	for (int i = 0; i < 64; ++i) {
		client->messages[i].timestamp = 0;
		client->messages[i].len = 0;
	}

	do_handshake(client);

	pollfd entry;
	entry.events = POLLIN;
	entry.revents = 0;
	entry.fd = fd;
	poll_table.push_back(entry);
	clients.push_back(client);

	return client;
}

void close_client(Client *client, size_t i)
{
	clients.erase(clients.begin() + i);
	poll_table.erase(poll_table.begin() + i);
	close(client->fd);

	if (client == publisher) {
		printf("publisher disconnected.\n");
		publisher = NULL;
	}

	delete client;
}

void do_poll()
{
	if (poll(&poll_table[0], poll_table.size(), -1) < 0) {
		if (errno == EAGAIN || errno == EINTR)
			return;
		throw std::runtime_error(strf("poll() failed: %s",
						strerror(errno)));
	}

	for (size_t i = 0; i < poll_table.size(); ++i) {
		if (poll_table[i].revents & POLLIN) {
			Client *client = clients[i];
			if (client == NULL) {
				new_client();
			} else try {
				recv_from_client(client);
			} catch (const std::runtime_error &e) {
				printf("client error: %s\n", e.what());
				close_client(client, i);
				--i;
			}
		}
	}
}

}

int main()
try {
	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0)
		return 1;

	sockaddr_in sin;
	sin.sin_family = AF_INET;
	sin.sin_port = htons(PORT);
	sin.sin_addr.s_addr = INADDR_ANY;
	if (bind(listen_fd, (sockaddr *) &sin, sizeof sin) < 0) {
		throw std::runtime_error(strf("Unable to listen: %s",
					 strerror(errno)));
		return 1;
	}

	listen(listen_fd, 10);

	pollfd entry;
	entry.events = POLLIN;
	entry.revents = 0;
	entry.fd = listen_fd;
	poll_table.push_back(entry);
	clients.push_back(NULL);

	for (;;) {
		do_poll();
	}
	return 0;
} catch (const std::runtime_error &e) {
	fprintf(stderr, "ERROR: %s\n", e.what());
	return 1;
}
