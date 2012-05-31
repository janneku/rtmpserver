#ifndef __amf_h
#define __amf_h

#include <string>
#include <map>
#include <assert.h>

enum AMFType {
	AMF_NUMBER,
	AMF_INTEGER,
	AMF_BOOLEAN,
	AMF_STRING,
	AMF_OBJECT,
	AMF_NULL,
	AMF_UNDEFINED,
	AMF_ECMA_ARRAY,
};

enum {
	AMF0_NUMBER,
	AMF0_BOOLEAN,
	AMF0_STRING,
	AMF0_OBJECT,
	AMF0_MOVIECLIP,
	AMF0_NULL,
	AMF0_UNDEFINED,
	AMF0_REFERENCE,
	AMF0_ECMA_ARRAY,
	AMF0_OBJECT_END,
	AMF0_STRICT_ARRAY,
	AMF0_DATE,
	AMF0_LONG_STRING,
	AMF0_UNSUPPORTED,
	AMF0_RECORD_SET,
	AMF0_XML_OBJECT,
	AMF0_TYPED_OBJECT,
	AMF0_SWITCH_AMF3,
};

enum {
	AMF3_UNDEFINED,
	AMF3_NULL,
	AMF3_FALSE,
	AMF3_TRUE,
	AMF3_INTEGER,
	AMF3_NUMBER,
	AMF3_STRING,
	AMF3_LEGACY_XML,
	AMF3_DATE,
	AMF3_ARRAY,
	AMF3_OBJECT,
	AMF3_XML,
	AMF3_BYTE_ARRAY,
};

struct Decoder {
	std::string buf;
	size_t pos;
	int version;
};

struct Encoder {
	std::string buf;
};

class AMFValue;

typedef std::map<std::string, AMFValue> amf_object_t;

class AMFValue {
public:
	AMFValue(AMFType type = AMF_NULL);
	AMFValue(const std::string &s);
	AMFValue(double n);
	AMFValue(int i);
	AMFValue(bool b);
	AMFValue(const amf_object_t &object);
	AMFValue(const AMFValue &from);
	~AMFValue();

	AMFType type() const { return m_type; }

	std::string as_string() const
	{
		assert(m_type == AMF_STRING);
		return *m_value.string;
	}
	double as_number() const
	{
		assert(m_type == AMF_NUMBER);
		return m_value.number;
	}
	double as_integer() const
	{
		assert(m_type == AMF_INTEGER);
		return m_value.integer;
	}
	bool as_boolean() const
	{
		assert(m_type == AMF_BOOLEAN);
		return m_value.boolean;
	}
	amf_object_t as_object() const
	{
		assert(m_type == AMF_OBJECT);
		return *m_value.object;
	}

	AMFValue get(const std::string &s) const
	{
		assert(m_type == AMF_OBJECT);
		amf_object_t::const_iterator i = m_value.object->find(s);
		if (i == m_value.object->end())
			return AMFValue();
		return i->second;
	}

	void set(const std::string &s, const AMFValue &val)
	{
		assert(m_type == AMF_OBJECT);
		m_value.object->insert(std::make_pair(s, val));
	}

	void operator = (const AMFValue &from);

private:
	AMFType m_type;
	union {
		std::string *string;
		double number;
		int integer;
		bool boolean;
		amf_object_t *object;
	} m_value;

	void destroy();
};

void amf_write(Encoder *enc, const std::string &s);
void amf_write(Encoder *enc, double n);
void amf_write(Encoder *enc, bool b);
void amf_write_key(Encoder *enc, const std::string &s);
void amf_write(Encoder *enc, const amf_object_t &object);
void amf_write_ecma(Encoder *enc, const amf_object_t &object);
void amf_write_null(Encoder *enc);
void amf_write(Encoder *enc, const AMFValue &value);

std::string amf_load_string(Decoder *dec);
double amf_load_number(Decoder *dec);
bool amf_load_boolean(Decoder *dec);
std::string amf_load_key(Decoder *dec);
amf_object_t amf_load_object(Decoder *dec);
amf_object_t amf_load_ecma(Decoder *dec);
AMFValue amf_load(Decoder *dec);

#endif
