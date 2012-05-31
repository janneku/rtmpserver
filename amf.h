#ifndef __amf_h
#define __amf_h

#include <string>
#include <map>
#include <assert.h>

enum AMFType {
	AMF_NUMBER,
	AMF_BOOLEAN,
	AMF_STRING,
	AMF_OBJECT,
	AMF_MOVIECLIP,
	AMF_NULL,
	AMF_UNDEFINED,
	AMF_REFERENCE,
	AMF_ECMA_ARRAY,
	AMF_OBJECT_END,
	AMF_STRICT_ARRAY,
	AMF_DATE,
	AMF_LONG_STRING,
	AMF_UNSUPPORTED,
	AMF_RECORD_SET,
	AMF_XML_OBJECT,
	AMF_TYPED_OBJECT,
};

struct Decoder {
	std::string buf;
	size_t pos;
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
