// -*- C++ -*-
#ifndef _vm_opcode_h_
#define _vm_opcode_h_

namespace vm {

enum OpCode {
  OP_INVALID,
  OP_SYM,
  OP_NUM,
  OP_STR,
  OP_ELM_REF,
  OP_LOAD_OBJ,
  OP_FUNCALL,
  OP_BIT_RANGE,
  OP_FUNCALL_DONE,
  OP_ADD,
  OP_SUB,
  OP_ASSIGN,
  OP_LT,
  OP_GT,
  OP_LTE,
  OP_GTE,
  OP_EQ,
  OP_NE,
  OP_AND,
  OP_OR,
  OP_XOR,
  OP_LSHIFT,
  OP_RSHIFT,
  OP_MUL,
  OP_LAND,
  OP_LOR,
  OP_CONCAT,
  OP_PLUS,
  OP_MINUS,
  OP_POST_INC,
  OP_PRE_INC,
  OP_POST_DEC,
  OP_PRE_DEC,
  OP_BIT_INV,
  OP_LOGIC_INV,
  OP_IF,
  OP_GOTO,
  OP_NOP,

  // Read/Write.
  OP_ARRAY_READ,
  OP_ARRAY_WRITE,
  OP_MEMBER_READ,
  OP_MEMBER_WRITE,

  // OPs for top level.
  OP_IMPORT,
  OP_FUNCDECL,
  OP_VARDECL,
  OP_CHANNEL_DECL,
  OP_MAILBOX_DECL,
  OP_THREAD_DECL,

  OP_SET_TYPE_OBJECT,
};

const char *OpCodeName(enum OpCode node);

}  // namespace vm

#endif  // _vm_opcode_h_
