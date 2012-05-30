#ifndef __utils_h
#define __utils_h

#include <map>
#include <string>
#include <stdio.h>
#include <stdint.h>

#define FOR_EACH(type, i, where)	\
	for (typename type::iterator i = (where).begin(); i != (where).end(); ++i)

#define FOR_EACH_CONST(type, i, where)	\
	for (typename type::const_iterator i = (where).begin(); \
		i != (where).end(); ++i)

#define debug(fmt...)	\
	fprintf(stderr, fmt)

template<class Key, class Value>
Value get(const std::map<Key, Value> &map, const Key &k,
	  const Value &def = Value())
{
	typename std::map<Key, Value>::const_iterator i = map.find(k);
	if (i == map.end())
		return def;
	return i->second;
}

uint32_t load_be32(const void *p);
uint16_t load_be16(const void *p);
uint32_t load_be24(const void *p);
uint32_t load_le32(const void *p);
void set_be24(void *p, uint32_t val);
void set_le32(void *p, uint32_t val);

const std::string strf(const char *fmt, ...);

#endif
