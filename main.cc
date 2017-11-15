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
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>

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
	RTMP_Message messages[64];
	std::string buf;
	std::string send_queue;
	size_t chunk_len;
	uint32_t written_seq;
	uint32_t read_seq;
};

namespace {

amf_object_t metadata;
Client *publisher = NULL;
int listen_fd;
std::vector<pollfd> poll_table;
std::vector<Client *> clients;

int set_nonblock(int fd, bool enabled)
{
	int flags = fcntl(fd, F_GETFL) & ~O_NONBLOCK;
	if (enabled) {
		flags |= O_NONBLOCK;
	}
	return fcntl(fd, F_SETFL, flags);
}

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

bool is_safe(uint8_t b)
{
	return b >= ' ' && b < 128;
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
				putc(is_safe(data[i + j]) ? data[i + j] : '.',
				     stdout);
			} else {
				putc(' ', stdout);
			}
		}
		putc('\n', stdout);
	}
}

void try_to_send(Client *client)
{
	size_t len = client->send_queue.size();
	if (len > 4096)
		len = 4096;

	ssize_t written = send(client->fd, client->send_queue.data(), len, 0);
	if (written < 0) {
		if (errno == EAGAIN || errno == EINTR)
			return;
		throw std::runtime_error(strf("unable to write to a client: %s",
						strerror(errno)));
	}

	client->send_queue.erase(0, written);
}

void rtmp_send(Client *client, uint8_t type, uint32_t endpoint,
		const std::string &buf, unsigned long timestamp = 0,
		int channel_num = CHAN_CONTROL)
{
	if (endpoint == STREAM_ID) {
		/*
		 * For some unknown reason, stream-related msgs must be sent
		 * on a specific channel.
		 */
		channel_num = CHAN_STREAM;
	}

	RTMP_Header header;
	header.flags = (channel_num & 0x3f) | (0 << 6);
	header.msg_type = type;
	set_be24(header.timestamp, timestamp);
	set_be24(header.msg_len, buf.size());
	set_le32(header.endpoint, endpoint);

	client->send_queue.append((char *) &header, sizeof header);
	client->written_seq += sizeof header;

	size_t pos = 0;
	while (pos < buf.size()) {
		if (pos) {
			uint8_t flags = (channel_num & 0x3f) | (3 << 6);
			client->send_queue += char(flags);

			client->written_seq += 1;
		}

		size_t chunk = buf.size() - pos;
		if (chunk > client->chunk_len)
			chunk = client->chunk_len;
		client->send_queue.append(buf, pos, chunk);

		client->written_seq += chunk;
		pos += chunk;
	}

	try_to_send(client);
}

void send_reply(Client *client, double txid, const AMFValue &reply = AMFValue(),
		const AMFValue &status = AMFValue())
{
	if (txid <= 0.0)
		return;
	Encoder invoke;
	amf_write(&invoke, std::string("_result"));
	amf_write(&invoke, txid);
	amf_write(&invoke, reply);
	amf_write(&invoke, status);
	rtmp_send(client, MSG_INVOKE, CONTROL_ID, invoke.buf, 0, CHAN_RESULT);
}

void handle_connect(Client *client, double txid, Decoder *dec)
{
	amf_object_t params = amf_load_object(dec);
	std::string app = get(params, std::string("app")).as_string();
	std::string ver = "(unknown)";
	AMFValue flashver = get(params, std::string("flashVer"));
	if (flashver.type() == AMF_STRING) {
		ver = flashver.as_string();
	}

	if (app != APP_NAME) {
		throw std::runtime_error("Unsupported application: " + app);
	}

	printf("connect: %s (version %s)\n", app.c_str(), ver.c_str());

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

	send_reply(client, txid, version, status);

/*
	uint32_t chunk_len = htonl(1024);
	std::string set_chunk((char *) &chunk_len, 4);
	rtmp_send(client, MSG_SET_CHUNK, CONTROL_ID, set_chunk, 0,
		  MEDIA_CHANNEL);

	client->chunk_len = 1024;
*/
}

void handle_fcpublish(Client *client, double txid, Decoder *dec)
{
	if (publisher != NULL) {
		throw std::runtime_error("Already have a publisher");
	}
	publisher = client;
	printf("publisher connected.\n");

	amf_load(dec); /* NULL */

	std::string path = amf_load_string(dec);
	debug("fcpublish %s\n", path.c_str());

	amf_object_t status;
	status.insert(std::make_pair("code", std::string("NetStream.Publish.Start")));
	status.insert(std::make_pair("description", path));

	Encoder invoke;
	amf_write(&invoke, std::string("onFCPublish"));
	amf_write(&invoke, 0.0);
	amf_write_null(&invoke);
	amf_write(&invoke, status);
	rtmp_send(client, MSG_INVOKE, CONTROL_ID, invoke.buf);

	send_reply(client, txid);
}

void handle_createstream(Client *client, double txid, Decoder *dec)
{
	send_reply(client, txid, AMFValue(), double(STREAM_ID));
}

void handle_publish(Client *client, double txid, Decoder *dec)
{
	amf_load(dec); /* NULL */

	std::string path = amf_load_string(dec);
	debug("publish %s\n", path.c_str());

	amf_object_t status;
	status.insert(std::make_pair("level", std::string("status")));
	status.insert(std::make_pair("code", std::string("NetStream.Publish.Start")));
	status.insert(std::make_pair("description", std::string("Stream is now published.")));
	status.insert(std::make_pair("details", path));

	Encoder invoke;
	amf_write(&invoke, std::string("onStatus"));
	amf_write(&invoke, 0.0);
	amf_write_null(&invoke);
	amf_write(&invoke, status);
	rtmp_send(client, MSG_INVOKE, STREAM_ID, invoke.buf);

	send_reply(client, txid);
}

void start_playback(Client *client)
{
	amf_object_t status;
	status.insert(std::make_pair("level", std::string("status")));
	status.insert(std::make_pair("code", std::string("NetStream.Play.Reset")));
	status.insert(std::make_pair("description", std::string("Resetting and playing stream.")));

	Encoder invoke;
	amf_write(&invoke, std::string("onStatus"));
	amf_write(&invoke, 0.0);
	amf_write_null(&invoke);
	amf_write(&invoke, status);
	rtmp_send(client, MSG_INVOKE, STREAM_ID, invoke.buf);

	status.clear();
	status.insert(std::make_pair("level", std::string("status")));
	status.insert(std::make_pair("code", std::string("NetStream.Play.Start")));
	status.insert(std::make_pair("description", std::string("Started playing.")));

	invoke.buf.clear();
	amf_write(&invoke, std::string("onStatus"));
	amf_write(&invoke, 0.0);
	amf_write_null(&invoke);
	amf_write(&invoke, status);
	rtmp_send(client, MSG_INVOKE, STREAM_ID, invoke.buf);

	invoke.buf.clear();
	amf_write(&invoke, std::string("|RtmpSampleAccess"));
	amf_write(&invoke, true);
	amf_write(&invoke, true);
	rtmp_send(client, MSG_NOTIFY, STREAM_ID, invoke.buf);

	client->playing = true;
	client->ready = false;

	if (publisher != NULL) {
		Encoder notify;
		amf_write(&notify, std::string("onMetaData"));
		amf_write_ecma(&notify, metadata);
		rtmp_send(client, MSG_NOTIFY, STREAM_ID, notify.buf);
	}
}

void handle_play(Client *client, double txid, Decoder *dec)
{
	amf_load(dec); /* NULL */

	std::string path = amf_load_string(dec);

	debug("play %s\n", path.c_str());

	start_playback(client);

	send_reply(client, txid);
}

void handle_play2(Client *client, double txid, Decoder *dec)
{
	amf_load(dec); /* NULL */

	amf_object_t params = amf_load_object(dec);
	std::string path = get(params, std::string("streamName")).as_string();

	debug("play %s\n", path.c_str());

	start_playback(client);

	send_reply(client, txid);
}

void handle_pause(Client *client, double txid, Decoder *dec)
{
	amf_load(dec); /* NULL */

	bool paused = amf_load_boolean(dec);

	if (paused) {
		debug("pausing\n");

		amf_object_t status;
		status.insert(std::make_pair("level", std::string("status")));
		status.insert(std::make_pair("code", std::string("NetStream.Pause.Notify")));
		status.insert(std::make_pair("description", std::string("Pausing.")));

		Encoder invoke;
		amf_write(&invoke, std::string("onStatus"));
		amf_write(&invoke, 0.0);
		amf_write_null(&invoke);
		amf_write(&invoke, status);
		rtmp_send(client, MSG_INVOKE, STREAM_ID, invoke.buf);
		client->playing = false;
	} else {
		start_playback(client);
	}

	send_reply(client, txid);
}

void handle_setdataframe(Client *client, Decoder *dec)
{
	if (client != publisher) {
		throw std::runtime_error("not a publisher");
	}

	std::string type = amf_load_string(dec);
	if (type != "onMetaData") {
		throw std::runtime_error("can only set metadata");
	}

	metadata = amf_load_ecma(dec);

	Encoder notify;
	amf_write(&notify, std::string("onMetaData"));
	amf_write_ecma(&notify, metadata);

	FOR_EACH(std::vector<Client *>, i, clients) {
		Client *client = *i;
		if (client != NULL && client->playing) {
			rtmp_send(client, MSG_NOTIFY, STREAM_ID, notify.buf);
		}
	}
}

void handle_invoke(Client *client, const RTMP_Message *msg, Decoder *dec)
{
	std::string method = amf_load_string(dec);
	double txid = amf_load_number(dec);

	debug("invoked %s\n", method.c_str());

	if (msg->endpoint == CONTROL_ID) {
		if (method == "connect") {
			handle_connect(client, txid, dec);
		} else if (method == "FCPublish") {
			handle_fcpublish(client, txid, dec);
		} else if (method == "createStream") {
			handle_createstream(client, txid, dec);
		}

	} else if (msg->endpoint == STREAM_ID) {
		if (method == "publish") {
			handle_publish(client, txid, dec);
		} else if (method == "play") {
			handle_play(client, txid, dec);
		} else if (method == "play2") {
			handle_play2(client, txid, dec);
		} else if (method == "pause") {
			handle_pause(client, txid, dec);
		}
	}
}

void handle_message(Client *client, RTMP_Message *msg)
{
	/*
	debug("RTMP message %02x, len %zu, timestamp %ld\n", msg->type, msg->len,
		msg->timestamp);
	*/

	size_t pos = 0;

	switch (msg->type) {
	case MSG_BYTES_READ:
		if (pos + 4 > msg->buf.size()) {
			throw std::runtime_error("Not enough data");
		}
		client->read_seq = load_be32(&msg->buf[pos]);
		debug("%d in queue\n",
			int(client->written_seq - client->read_seq));
		break;

	case MSG_SET_CHUNK:
		if (pos + 4 > msg->buf.size()) {
			throw std::runtime_error("Not enough data");
		}
		client->chunk_len = load_be32(&msg->buf[pos]);
		debug("chunk size set to %zu\n", client->chunk_len);
		break;

	case MSG_INVOKE: {
			Decoder dec;
			dec.version = 0;
			dec.buf = msg->buf;
			dec.pos = 0;
			handle_invoke(client, msg, &dec);
		}
		break;

	case MSG_INVOKE3: {
			Decoder dec;
			dec.version = 0;
			dec.buf = msg->buf;
			dec.pos = 1;
			handle_invoke(client, msg, &dec);
		}
		break;

	case MSG_NOTIFY: {
			Decoder dec;
			dec.version = 0;
			dec.buf = msg->buf;
			dec.pos = 0;
			std::string type = amf_load_string(&dec);
			debug("notify %s\n", type.c_str());
			if (msg->endpoint == STREAM_ID) {
				if (type == "@setDataFrame") {
					handle_setdataframe(client, &dec);
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
				rtmp_send(receiver, MSG_AUDIO, STREAM_ID,
					  msg->buf, msg->timestamp);
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
				if (flags >> 4 == FLV_KEY_FRAME &&
				    !receiver->ready) {
					std::string control;
					uint16_t type = htons(CONTROL_CLEAR_STREAM);
					control.append((char *) &type, 2);
					uint32_t stream = htonl(STREAM_ID);
					control.append((char *) &stream, 4);
					rtmp_send(receiver, MSG_USER_CONTROL, CONTROL_ID, control);
					receiver->ready = true;
				}
				if (receiver->ready) {
					rtmp_send(receiver, MSG_VIDEO,
						  STREAM_ID, msg->buf,
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
	Handshake serversig;
	Handshake clientsig;

	uint8_t c;
	if (recv_all(client->fd, &c, 1) < 1)
		return;
	if (c != HANDSHAKE_PLAINTEXT) {
		throw std::runtime_error("only plaintext handshake supported");
	}

	if (send_all(client->fd, &c, 1) < 1)
		return;

	memset(&serversig, 0, sizeof serversig);
	serversig.flags[0] = 0x03;
	for (int i = 0; i < RANDOM_LEN; ++i) {
		serversig.random[i] = rand();
	}

	if (send_all(client->fd, &serversig, sizeof serversig) < sizeof serversig)
		return;

	/* Echo client's signature back */
	if (recv_all(client->fd, &clientsig, sizeof serversig) < sizeof serversig)
		return;
	if (send_all(client->fd, &clientsig, sizeof serversig) < sizeof serversig)
		return;

	if (recv_all(client->fd, &clientsig, sizeof serversig) < sizeof serversig)
		return;
	if (memcmp(serversig.random, clientsig.random, RANDOM_LEN) != 0) {
		throw std::runtime_error("invalid handshake");
	}

	client->read_seq = 1 + sizeof serversig * 2;
	client->written_seq = 1 + sizeof serversig * 2;
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
		throw std::runtime_error(strf("unable to read from a client: %s",
					      strerror(errno)));
	}
	client->buf.append(chunk, 0, got);

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
		size_t chunk = msg->len - msg->buf.size();
		if (chunk > client->chunk_len)
			chunk = client->chunk_len;

		if (client->buf.size() < header_len + chunk) {
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

		msg->buf.append(client->buf, header_len, chunk);
		client->buf.erase(0, header_len + chunk);

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
	client->written_seq = 0;
	client->read_seq = 0;
	client->chunk_len = DEFAULT_CHUNK_LEN;
	for (int i = 0; i < 64; ++i) {
		client->messages[i].timestamp = 0;
		client->messages[i].len = 0;
	}

	try {
		do_handshake(client);
	} catch (const std::runtime_error &e) {
		printf("handshake failed: %s\n", e.what());
		close(fd);
		delete client;
		return NULL;
	}

	set_nonblock(fd, true);

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
		FOR_EACH(std::vector<Client *>, i, clients) {
			Client *client = *i;
			if (client != NULL) {
				client->ready = false;
			}
		}
	}

	delete client;
}

void do_poll()
{
	for (size_t i = 0; i < poll_table.size(); ++i) {
		Client *client = clients[i];
		if (client != NULL) {
			if (!client->send_queue.empty()) {
				debug("waiting for pollout\n");
				poll_table[i].events = POLLIN | POLLOUT;
			} else {
				poll_table[i].events = POLLIN;
			}
		}
	}

	if (poll(&poll_table[0], poll_table.size(), -1) < 0) {
		if (errno == EAGAIN || errno == EINTR)
			return;
		throw std::runtime_error(strf("poll() failed: %s",
						strerror(errno)));
	}

	for (size_t i = 0; i < poll_table.size(); ++i) {
		Client *client = clients[i];
		if (poll_table[i].revents & POLLOUT) {
			try {
				try_to_send(client);
			} catch (const std::runtime_error &e) {
				printf("client error: %s\n", e.what());
                                if (client) {
                                    close_client(client, i);
                                }
				--i;
				continue;
			}
		}
		if (poll_table[i].revents & POLLIN) {
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
