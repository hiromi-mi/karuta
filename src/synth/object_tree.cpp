#include "synth/object_tree.h"

#include "vm/object.h"

#include <list>
#include <set>

using std::list;
using std::set;

namespace synth {

ObjectTree::ObjectTree(vm::Object *root_obj) : root_obj_(root_obj) {
}

ObjectTree::~ObjectTree() {
}

void ObjectTree::Build() {
  list<vm::Object *> q;
  q.push_back(root_obj_);
  set<vm::Object *> seen;
  // BFS traversal.
  while (q.size() > 0) {
    vm::Object *o = *(q.begin());
    q.pop_front();
    if (seen.find(o) != seen.end()) {
      continue;
    }
    seen.insert(o);
    CheckObject(o, seen, &q);
  }
}

std::map<vm::Object *, string> ObjectTree::GetChildObjects(vm::Object *o) {
  return obj_children_map_[o];
}

void ObjectTree::CheckObject(vm::Object *o, std::set<vm::Object *> &seen,
			     std::list<vm::Object *> *q) {
  map<sym_t, vm::Object *> member_objs;
  o->GetAllMemberObjs(&member_objs);
  for (auto it : member_objs) {
    auto &m = obj_children_map_[o];
    if (seen.find(it.second) != seen.end()) {
      continue;
    }
    m[it.second] = sym_str(it.first);
    q->push_back(it.second);
  }
}

}  // namespace synth
