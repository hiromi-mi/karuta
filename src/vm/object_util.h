// -*- C++ -*-
#ifndef _vm_object_util_h_
#define _vm_object_util_h_

#include "vm/common.h"

namespace vm {

class ObjectUtil {
public:
  static int GetAddressWidth(Object *obj);
  static void SetAddressWidth(Object *obj, int width);
  static string GetStringMember(Object *obj, const string &key);
  static void SetStringMember(Object *obj, const string &key,
			      const string &str);
};

}  // namespace vm

#endif  // _vm_object_util_h_
