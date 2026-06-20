#ifndef COLL_UTIL_H
#define COLL_UTIL_H
#include <cstdint>
#include <string>
#include "ns3/enum.h"
namespace ns3 {
static const uint16_t COLLECTIVES_PROTOCOL = 0x9500;
class DataType {
	public:
		enum Type : uint8_t {
			INT32,
			FLOAT64,
			FLOAT32,
			INT16
		};

		static inline uint32_t GetSizeBytes(Type t){
			switch (t) {
				case FLOAT32: return 4;
				case FLOAT64: return 8;
				case INT32:   return 4;
				case INT16:   return 2;
				default:      return 0;
			}
		}

		static inline uint32_t GetSizeBytes(uint32_t t) {
			return GetSizeBytes(static_cast<Type>(t));
		}

		static inline const char* ToString(Type t){
			switch (t) {
				case FLOAT32: return "FLOAT32";
				case FLOAT64: return "FLOAT64";
				case INT32:   return "INT32";
				case INT16:   return "INT16";
				default:      return "UNKNOWN";
			}
		}
};

enum RedOp {
	ADD
};

struct DataBuffer {
	size_t len;
	void* dataBuffer = nullptr;
};

} // namespace ns3
#endif
