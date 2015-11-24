// -*- C++ -*-
#ifndef _writer_vl_writer_h_
#define _writer_vl_writer_h_

#include "writer.h"

namespace dfg {
class DModule;
}  // namespace dfg

namespace writer {

class VLChannelWriter;
class VLModule;

class VLWriter : public Writer {
public:
  VLWriter(DModule *mod, const string &name, ostream &os);
  void Output();

  static bool WriteModule(DModule *mod, const string &fn);

  static void ICE(const char *msg, const sym_t sym = NULL);

private:
  const string &top_module_name_;

  void OutputSubModulesRec(DModule *mod, const string &path_name,
			   VLChannelWriter *ch,
			   vector<string> *files, ostream &os);
  void OutputTopLevelWrapper(VLModule *module_writer, ostream &os);
};

}  // namespace writer

#endif  // _writer_vl_writer_h_
