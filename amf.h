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

void amf_write(std::string &out, const std::string &s);
void amf_write(std::string &out, double n);
void amf_write(std::string &out, bool b);
void amf_write_key(std::string &out, const std::string &s);
void amf_write(std::string &out, const amf_object_t &object);
void amf_write_ecma(std::string &out, const amf_object_t &object);
void amf_write(std::string &out, const AMFValue &value);

std::string amf_load_string(const std::string &data, size_t &pos);
double amf_load_number(const std::string &data, size_t &pos);
bool amf_load_boolean(const std::string &data, size_t &pos);
std::string amf_load_key(const std::string &data, size_t &pos);
amf_object_t amf_load_object(const std::string &data, size_t &pos);
amf_object_t amf_load_ecma(const std::string &data, size_t &pos);
AMFValue amf_load(const std::string &data, size_t &pos);

#endif
