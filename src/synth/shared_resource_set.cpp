#include "synth/shared_resource_set.h"

#include "base/stl_util.h"
#include "iroha/i_design.h"
#include "synth/object_method_names.h"
#include "synth/object_synth.h"
#include "synth/thread_synth.h"
#include "vm/insn.h"

namespace synth {

SharedResource::SharedResource()
  : owner_thr_(nullptr), owner_res_(nullptr) {
}

SharedResource::~SharedResource() {
}

void SharedResource::AddOwnerResource(IResource *res) {
  owner_res_ = res;
}

void SharedResource::AddAccessorResource(IResource *res) {
  accessor_resources_.insert(res);
}

SharedResourceSet::~SharedResourceSet() {
  STLDeleteSecondElements(&obj_resources_);
  STLDeleteSecondElements(&value_resources_);
}

void SharedResourceSet::DetermineOwnerThreadAll() {
  for (auto it : obj_resources_) {
    DetermineOwnerThread(it.second);
  }
  for (auto it : value_resources_) {
    DetermineOwnerThread(it.second);
  }
}

void SharedResourceSet::ResolveResourceAccessors() {
  for (auto it : obj_resources_) {
    ResolveSharedResourceAccessor(it.second);
  }
  for (auto it : value_resources_) {
    ResolveSharedResourceAccessor(it.second);
  }
}

void SharedResourceSet::DetermineOwnerThread(SharedResource *res) {
  CHECK(res->axi_ctrl_thrs_.size() <= 1);
  if (res->axi_ctrl_thrs_.size() == 1) {
    res->owner_thr_ = *(res->axi_ctrl_thrs_.begin());
  }
  for (ThreadSynth *thr : res->ordered_accessors_) {
    if (res->owner_thr_ == nullptr) {
      // Picks up first one.
      res->owner_thr_ = thr;
    }
  }
}

void SharedResourceSet::ResolveSharedResourceAccessor(SharedResource *sres) {
  for (IResource *res : sres->accessor_resources_) {
    res->SetParentResource(sres->owner_res_);
  }
}

void SharedResourceSet::AddMemberAccessor(ThreadSynth *thr, sym_t name,
					  vm::Insn *insn, bool is_tls) {
  ThreadSynth *tls_thr = nullptr;
  if (is_tls) {
    tls_thr = thr;
  }
  SharedResource *res = GetBySlotName(thr->GetObjectSynth()->GetObject(),
				      tls_thr,
				      name);
  res->ordered_accessors_.push_back(thr);
  res->accessors_.insert(thr);
  if (insn->op_ == vm::OP_MEMBER_READ) {
    res->readers_.insert(thr);
  }
  if (insn->op_ == vm::OP_MEMBER_WRITE) {
    res->writers_.insert(thr);
  }
}

void SharedResourceSet::AddObjectAccessor(ThreadSynth *thr, vm::Object *obj,
					  vm::Insn *insn,
					  const string &synth_name,
					  bool is_tls) {
  ThreadSynth *tls_thr = nullptr;
  if (is_tls) {
    tls_thr = thr;
  }
  SharedResource *res = GetByObj(obj, tls_thr);
  res->ordered_accessors_.push_back(thr);
  res->accessors_.insert(thr);
  if (insn->op_ == vm::OP_ARRAY_READ) {
    res->readers_.insert(thr);
  }
  if (insn->op_ == vm::OP_ARRAY_WRITE) {
    res->writers_.insert(thr);
  }
  if (insn->op_ == vm::OP_FUNCALL) {
    if (synth_name == kAxiLoad || synth_name == kAxiStore) {
      res->axi_ctrl_thrs_.insert(thr);
    }
    if (synth_name == kMailboxGet || synth_name == kMailboxPut) {
    }
  }
}

bool SharedResourceSet::AddExtIOMethodAccessor(ThreadSynth *thr,
					       vm::Method *method) {
  auto it = ext_io_methods_.find(method);
  if (it == ext_io_methods_.end()) {
    ext_io_methods_[method] = thr;
    return true;
  } else {
    if (it->second != thr) {
      // Claimed by other thread.
      return false;
    }
  }
  return true;
}

SharedResource *SharedResourceSet::GetBySlotName(vm::Object *obj,
						 ThreadSynth *thr,
						 sym_t name) {
  auto key = std::make_tuple(obj, thr, name);
  auto it = value_resources_.find(key);
  if (it != value_resources_.end()) {
    return it->second;
  }
  SharedResource *res = new SharedResource();
  value_resources_[key] = res;
  return res;
}

SharedResource *SharedResourceSet::GetByObj(vm::Object *obj,
					    ThreadSynth *thr) {
  auto key = std::make_tuple(obj, thr);
  auto it = obj_resources_.find(key);
  if (it != obj_resources_.end()) {
    return it->second;
  }
  SharedResource *res = new SharedResource();
  obj_resources_[key] = res;
  return res;
}

  bool SharedResourceSet::HasAccessor(vm::Object *obj, ThreadSynth *thr) {
  auto key = std::make_tuple(obj, thr);
  return (obj_resources_.find(key) != obj_resources_.end());
}

bool SharedResourceSet::HasExtIOAccessor(vm::Method *method) {
  auto it = ext_io_methods_.find(method);
  if (it == ext_io_methods_.end()) {
    return false;
  }
  return true;
}

}  // namespace synth
