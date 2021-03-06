#include "tools/json2kv.h"
#include <cmath>
namespace reindexer {

KeyRef jsonValue2KeyRef(JsonValue &v, KeyValueType t, const char *fieldName) {
	switch (v.getTag()) {
		case JSON_NUMBER:
			switch (t) {
				case KeyValueUndefined: {
					double value = v.toNumber(), intpart;
					if (std::modf(value, &intpart) == 0.0) {
						return KeyRef(static_cast<int64_t>(v.toNumber()));
					}
					return KeyRef(v.toNumber());
				}
				case KeyValueDouble:
					return KeyRef(v.toNumber());
				case KeyValueInt:
					return KeyRef(static_cast<int>(v.toNumber()));
				case KeyValueInt64:
					return KeyRef(static_cast<int64_t>(v.toNumber()));
				default:
					throw Error(errLogic, "Error parsing json field '%s' - got number, expected %s", fieldName, KeyValue::TypeName(t));
			}
		case JSON_STRING:
			return KeyRef(p_string(v.toString()));
		case JSON_FALSE:
			return KeyRef(static_cast<int>(0));
		case JSON_TRUE:
			return KeyRef(static_cast<int>(1));
		case JSON_NULL:
			switch (t) {
				case KeyValueDouble:
					return KeyRef(static_cast<double>(0));
				case KeyValueInt:
					return KeyRef(static_cast<int>(0));
				case KeyValueInt64:
					return KeyRef(static_cast<int64_t>(0));
				case KeyValueString:
					return KeyRef(p_string(static_cast<const char *>(nullptr)));
				default:
					throw Error(errLogic, "Error parsing json field '%s' - got null, expected %s", fieldName, KeyValue::TypeName(t));
			}
		case JSON_OBJECT:
			throw Error(errLogic, "Error parsing json field '%s' - got object, expected %s", fieldName, KeyValue::TypeName(t));
		case JSON_ARRAY:
			throw Error(errLogic, "Error parsing json field '%s' - got array, expected %s", fieldName, KeyValue::TypeName(t));
		default:
			abort();
	}
	return KeyRef();
}

KeyValue jsonValue2KeyValue(JsonValue &values) {
	KeyValues kvs;
	for (auto elem : values) {
		if (elem->value.getTag() != JSON_NULL) {
			KeyValue kv(jsonValue2KeyRef(elem->value, KeyValueUndefined));
			kvs.push_back(kv);
		}
	}
	return KeyValue(kvs);
}

}  // namespace reindexer
