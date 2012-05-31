#include "amf.h"
#include "utils.h"
#include <stdexcept>
#include <string.h>
#include <arpa/inet.h>

namespace {

uint8_t get_byte(const std::string &data, size_t &pos)
{
	if (pos >= data.size()) {
		throw std::runtime_error("Not enough data");
	}
	return uint8_t(data[pos++]);
}

uint8_t peek(const std::string &data, size_t &pos)
{
	if (pos >= data.size()) {
		throw std::runtime_error("Not enough data");
	}
	return uint8_t(data[pos]);
}

}

AMFValue::AMFValue(AMFType type) :
	m_type(type)
{
	switch (m_type) {
	case AMF_OBJECT:
	case AMF_ECMA_ARRAY:
		m_value.object = new amf_object_t;
		break;
	case AMF_NUMBER:
	case AMF_NULL:
	case AMF_UNDEFINED:
		break;
	default:
		assert(0);
	}
}

AMFValue::AMFValue(const std::string &s) :
	m_type(AMF_STRING)
{
	m_value.string = new std::string(s);
}

AMFValue::AMFValue(double n) :
	m_type(AMF_NUMBER)
{
	m_value.number = n;
}

AMFValue::AMFValue(bool b) :
	m_type(AMF_BOOLEAN)
{
	m_value.boolean = b;
}

AMFValue::AMFValue(const amf_object_t &object) :
	m_type(AMF_OBJECT)
{
	m_value.object = new amf_object_t(object);
}

AMFValue::AMFValue(const AMFValue &from) :
	m_type(AMF_NULL)
{
	*this = from;
}

AMFValue::~AMFValue()
{
	destroy();
}

void AMFValue::destroy()
{
	switch (m_type) {
	case AMF_STRING:
		delete m_value.string;
		break;
	case AMF_OBJECT:
	case AMF_ECMA_ARRAY:
		delete m_value.object;
		break;
	default:
		break;
	}
}

void AMFValue::operator = (const AMFValue &from)
{
	destroy();
	m_type = from.m_type;
	switch (m_type) {
	case AMF_STRING:
		m_value.string = new std::string(*from.m_value.string);
		break;
	case AMF_OBJECT:
	case AMF_ECMA_ARRAY:
		m_value.object = new amf_object_t(*from.m_value.object);
		break;
	case AMF_NUMBER:
		m_value.number = from.m_value.number;
		break;
	case AMF_BOOLEAN:
		m_value.boolean = from.m_value.boolean;
		break;
	default:
		break;
	}
}

void amf_write(std::string &out, const std::string &s)
{
	out += char(AMF_STRING);
	uint16_t str_len = htons(s.size());
	out.append((char *) &str_len, 2);
	out += s;
}

void amf_write(std::string &out, double n)
{
	out += char(AMF_NUMBER);
	uint64_t encoded = 0;
#if defined(__i386__) || defined(__x86_64__)
	/* Flash uses same floating point format as x86 */
	memcpy(&encoded, &n, 8);
#endif
	uint32_t val = htonl(encoded >> 32);
	out.append((char *) &val, 4);
	val = htonl(encoded);
	out.append((char *) &val, 4);
}

void amf_write(std::string &out, bool b)
{
	out += char(AMF_BOOLEAN);
	out += char(b);
}

void amf_write_key(std::string &out, const std::string &s)
{
	uint16_t str_len = htons(s.size());
	out.append((char *) &str_len, 2);
	out += s;
}

void amf_write(std::string &out, const amf_object_t &object)
{
	out += char(AMF_OBJECT);
	for (amf_object_t::const_iterator i = object.begin();
					  i != object.end(); ++i) {
		amf_write_key(out, i->first);
		amf_write(out, i->second);
	}
	amf_write_key(out, "");
	out += char(AMF_OBJECT_END);
}

void amf_write_ecma(std::string &out, const amf_object_t &object)
{
	out += char(AMF_ECMA_ARRAY);
	uint32_t zero = 0;
	out.append((char *) &zero, 4);
	for (amf_object_t::const_iterator i = object.begin();
					  i != object.end(); ++i) {
		amf_write_key(out, i->first);
		amf_write(out, i->second);
	}
	amf_write_key(out, "");
	out += char(AMF_OBJECT_END);
}

void amf_write(std::string &out, const AMFValue &value)
{
	switch (value.type()) {
	case AMF_STRING:
		amf_write(out, value.as_string());
		break;
	case AMF_NUMBER:
		amf_write(out, value.as_number());
		break;
	case AMF_BOOLEAN:
		amf_write(out, value.as_boolean());
		break;
	case AMF_OBJECT:
		amf_write(out, value.as_object());
		break;
	case AMF_ECMA_ARRAY:
		amf_write_ecma(out, value.as_object());
		break;
	default:
		out += char(value.type());
		break;
	}
}

std::string amf_load_string(const std::string &data, size_t &pos)
{
	if (get_byte(data, pos) != AMF_STRING) {
		throw std::runtime_error("Expected a string");
	}
	if (pos + 2 > data.size()) {
		throw std::runtime_error("Not enough data");
	}
	size_t str_len = load_be16(&data[pos]);
	pos += 2;
	if (pos + str_len > data.size()) {
		throw std::runtime_error("Not enough data");
	}
	std::string s(data, pos, str_len);
	pos += str_len;
	return s;
}

double amf_load_number(const std::string &data, size_t &pos)
{
	if (get_byte(data, pos) != AMF_NUMBER) {
		throw std::runtime_error("Expected a string");
	}
	if (pos + 8 > data.size()) {
		throw std::runtime_error("Not enough data");
	}
	uint64_t val = ((uint64_t) load_be32(&data[pos]) << 32) |
			load_be32(&data[pos + 4]);
	double n = 0;
#if defined(__i386__) || defined(__x86_64__)
	/* Flash uses same floating point format as x86 */
	memcpy(&n, &val, 8);
#endif
	pos += 8;
	return n;
}

bool amf_load_boolean(const std::string &data, size_t &pos)
{
	if (get_byte(data, pos) != AMF_BOOLEAN) {
		throw std::runtime_error("Expected a boolean");
	}
	return get_byte(data, pos) != 0;
}

std::string amf_load_key(const std::string &data, size_t &pos)
{
	if (pos + 2 > data.size()) {
		throw std::runtime_error("Not enough data");
	}
	size_t str_len = load_be16(&data[pos]);
	pos += 2;
	if (pos + str_len > data.size()) {
		throw std::runtime_error("Not enough data");
	}
	std::string s(data, pos, str_len);
	pos += str_len;
	return s;
}

amf_object_t amf_load_object(const std::string &data, size_t &pos)
{
	amf_object_t object;
	if (get_byte(data, pos) != AMF_OBJECT) {
		throw std::runtime_error("Expected an object");
	}
	while (1) {
		std::string key = amf_load_key(data, pos);
		if (key.empty())
			break;
		AMFValue value = amf_load(data, pos);
		object.insert(std::make_pair(key, value));
	}
	if (get_byte(data, pos) != AMF_OBJECT_END) {
		throw std::runtime_error("expected object end");
	}
	return object;
}

amf_object_t amf_load_ecma(const std::string &data, size_t &pos)
{
	/* ECMA array is the same as object, with 4 extra zero bytes */
	amf_object_t object;
	if (get_byte(data, pos) != AMF_ECMA_ARRAY) {
		throw std::runtime_error("Expected an ECMA array");
	}
	if (pos + 4 > data.size()) {
		throw std::runtime_error("Not enough data");
	}
	pos += 4;
	while (1) {
		std::string key = amf_load_key(data, pos);
		if (key.empty())
			break;
		AMFValue value = amf_load(data, pos);
		object.insert(std::make_pair(key, value));
	}
	if (get_byte(data, pos) != AMF_OBJECT_END) {
		throw std::runtime_error("expected object end");
	}
	return object;
}

AMFValue amf_load(const std::string &data, size_t &pos)
{
	uint8_t type = peek(data, pos);
	switch (type) {
	case AMF_STRING:
		return AMFValue(amf_load_string(data, pos));
	case AMF_NUMBER:
		return AMFValue(amf_load_number(data, pos));
	case AMF_BOOLEAN:
		return AMFValue(amf_load_boolean(data, pos));
	case AMF_OBJECT:
		return AMFValue(amf_load_object(data, pos));
	case AMF_ECMA_ARRAY:
		return AMFValue(amf_load_ecma(data, pos));
	default:
		pos++;
		return AMFValue(AMFType(type));
	}
}
