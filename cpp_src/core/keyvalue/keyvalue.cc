#include "keyvalue.h"
#include <cstring>
#include "core/payload/payloadiface.h"
#include "tools/errors.h"

namespace reindexer {

KeyValue::KeyValue(const KeyRef &other) : KeyRef(other) {
	if (type == KeyValueComposite) h_value_composite = static_cast<PayloadValue>(other);
	if (type == KeyValueString) h_value_string = static_cast<key_string>(other);
	relink();
}

KeyValue::KeyValue(const KeyValue &other) : KeyRef(other) {
	h_value_composite = other.h_value_composite;
	h_value_string = other.h_value_string;
	h_composite_values = other.h_composite_values;
	relink();
}

KeyValue &KeyValue::operator=(const KeyValue &other) {
	if (&other != this) {
		KeyRef::operator=(other);
		h_value_composite = other.h_value_composite;
		h_value_string = other.h_value_string;
		h_composite_values = other.h_composite_values;
		relink();
	}
	return *this;
}

bool KeyValue::operator==(const KeyValue &other) const {
	if (type == KeyValueComposite) {
		return (h_composite_values == other.h_composite_values);
	}
	return KeyRef::operator==(other);
}

bool KeyValue::operator!=(const KeyValue &other) const { return !operator==(other); }

int KeyValue::convert(KeyValueType _type) {
	if (_type == type) return 0;
	switch (_type) {
		case KeyValueInt:
			value_int = As<int>();
			break;
		case KeyValueInt64:
			value_int64 = As<int64_t>();
			break;
		case KeyValueDouble:
			value_double = As<double>();
			break;
		case KeyValueString: {
			auto s = As<string>();
			h_value_string = make_key_string(s.data(), s.length());
		} break;
		default:
			assertf(0, "Can't convert KeyValue from type '%s' to to type '%s'", TypeName(type), TypeName(_type));
	}
	type = _type;
	relink();
	return 0;
}

void KeyValue::convertToComposite(const PayloadType &payloadType, const FieldsSet &fields) {
	assert(type == KeyValueComposite);
	if (!h_value_composite.IsFree()) {
		return;
	}

	if (h_composite_values.size() != fields.size()) {
		throw Error(errLogic, "Invalid count of arguments for composite index, expected %d, got %d", int(fields.size()),
					int(h_composite_values.size()));
	}
	h_value_composite.AllocOrClone(payloadType.TotalSize());

	Payload pv(payloadType, h_value_composite);

	auto field = fields.begin();
	for (const KeyValue &compositeValue : h_composite_values) {
		if (*field != IndexValueType::SetByJsonPath) {
			pv.Set(*field, {compositeValue});
		} else {
			// TODO: will have to implement SetByJsonPath in PayloadIFace
			// or this "mixed" composite queries (by ordinary indexes + indexes
			// from cjson) won't work properly.
			throw Error(errConflict, "SetByJsonPath is not implemented yet");
		}
		field++;
	}
	h_composite_values.clear();
}

const std::vector<KeyValue> &KeyValue::getCompositeValues() const { return h_composite_values; }

}  // namespace reindexer
