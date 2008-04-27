#include "assembler.h"
#include "vector.h"

using namespace vm;

#define INDEX1(a, b) ((a) + (BinaryOperationCount * (b)))

#define CAST1(x) reinterpret_cast<UnaryOperationType>(x)

#define INDEX2(a, b, c) \
  ((a) \
   + (BinaryOperationCount * (b)) \
   + (BinaryOperationCount * OperandTypeCount * (c)))

#define CAST2(x) reinterpret_cast<BinaryOperationType>(x)

namespace {

enum {
  rax = 0,
  rcx = 1,
  rdx = 2,
  rbx = 3,
  rsp = 4,
  rbp = 5,
  rsi = 6,
  rdi = 7,
  r8 = 8,
  r9 = 9,
  r10 = 10,
  r11 = 11,
  r12 = 12,
  r13 = 13,
  r14 = 14,
  r15 = 15,
};

int64_t FORCE_ALIGN
divideLong(int64_t a, int64_t b)
{
  return a / b;
}

int64_t FORCE_ALIGN
moduloLong(int64_t a, int64_t b)
{
  return a % b;
}

inline bool
isInt8(intptr_t v)
{
  return v == static_cast<int8_t>(v);
}

inline bool
isInt32(intptr_t v)
{
  return v == static_cast<int32_t>(v);
}

class Task;

class Context {
 public:
  Context(System* s, Allocator* a, Zone* zone):
    s(s), zone(zone), client(0), code(s, a, 1024), tasks(0), result(0)
  { }

  System* s;
  Zone* zone;
  Assembler::Client* client;
  Vector code;
  Task* tasks;
  uint8_t* result;
};

inline void NO_RETURN
abort(Context* c)
{
  abort(c->s);
}

#ifndef NDEBUG
inline void
assert(Context* c, bool v)
{
  assert(c->s, v);
}
#endif // not NDEBUG

inline void
expect(Context* c, bool v)
{
  expect(c->s, v);
}

class CodePromise: public Promise {
 public:
  CodePromise(Context* c, unsigned offset): c(c), offset(offset) { }

  virtual int64_t value() {
    if (resolved()) {
      return reinterpret_cast<intptr_t>(c->result + offset);
    }
    
    abort(c);
  }

  virtual bool resolved() {
    return c->result != 0;
  }

  Context* c;
  unsigned offset;
};

CodePromise*
codePromise(Context* c, unsigned offset)
{
  return new (c->zone->allocate(sizeof(CodePromise))) CodePromise(c, offset);
}

class Task {
 public:
  Task(Task* next): next(next) { }

  virtual ~Task() { }

  virtual void run(Context* c) = 0;

  Task* next;
};

class OffsetTask: public Task {
 public:
  OffsetTask(Task* next, Promise* promise, unsigned instructionOffset,
             unsigned instructionSize):
    Task(next),
    promise(promise),
    instructionOffset(instructionOffset),
    instructionSize(instructionSize)
  { }

  virtual void run(Context* c) {
    uint8_t* instruction = c->result + instructionOffset;
    intptr_t v = reinterpret_cast<uint8_t*>(promise->value())
      - instruction - instructionSize;
    
    expect(c, isInt32(v));
    
    int32_t v4 = v;
    memcpy(instruction + instructionSize - 4, &v4, 4);
  }

  Promise* promise;
  unsigned instructionOffset;
  unsigned instructionSize;
};

void
appendOffsetTask(Context* c, Promise* promise, int instructionOffset,
                 unsigned instructionSize)
{
  c->tasks = new (c->zone->allocate(sizeof(OffsetTask))) OffsetTask
    (c->tasks, promise, instructionOffset, instructionSize);
}

class ImmediateTask: public Task {
 public:
  ImmediateTask(Task* next, Promise* promise, unsigned offset):
    Task(next),
    promise(promise),
    offset(offset)
  { }

  virtual void run(Context* c) {
    intptr_t v = promise->value();
    memcpy(c->result + offset, &v, BytesPerWord);
  }

  Promise* promise;
  unsigned offset;
};

void
appendImmediateTask(Context* c, Promise* promise, unsigned offset)
{
  c->tasks = new (c->zone->allocate(sizeof(ImmediateTask))) ImmediateTask
    (c->tasks, promise, offset);
}

void
encode(Context* c, uint8_t* instruction, unsigned length, int a, int b,
       int32_t displacement, int index, unsigned scale)
{
  c->code.append(instruction, length);

  uint8_t width;
  if (displacement == 0 and b != rbp) {
    width = 0;
  } else if (isInt8(displacement)) {
    width = 0x40;
  } else {
    width = 0x80;
  }

  if (index == -1) {
    c->code.append(width | (a << 3) | b);
    if (b == rsp) {
      c->code.append(0x24);
    }
  } else {
    assert(c, b != rsp);
    c->code.append(width | (a << 3) | 4);
    c->code.append((log(scale) << 6) | (index << 3) | b);
  }

  if (displacement == 0 and b != rbp) {
    // do nothing
  } else if (isInt8(displacement)) {
    c->code.append(displacement);
  } else {
    c->code.append4(displacement);
  }
}

void
rex(Context* c, uint8_t mask, int r)
{
  if (BytesPerWord == 8) {
    c->code.append(mask | ((r & 8) >> 3));
  }
}

void
rex(Context* c)
{
  rex(c, 0x48, rax);
}

void
encode(Context* c, uint8_t instruction, int a, Assembler::Memory* b, bool rex)
{
  if (rex) {
    ::rex(c);
  }

  encode(c, &instruction, 1, a, b->base, b->offset, b->index, b->scale);
}

void
encode2(Context* c, uint16_t instruction, int a, Assembler::Memory* b,
        bool rex)
{
  if (rex) {
    ::rex(c);
  }

  uint8_t i[2] = { instruction >> 8, instruction & 0xff };
  encode(c, i, 2, a, b->base, b->offset, b->index, b->scale);
}

typedef void (*OperationType)(Context*);
OperationType
Operations[OperationCount];

typedef void (*UnaryOperationType)(Context*, unsigned, Assembler::Operand*);
UnaryOperationType
UnaryOperations[UnaryOperationCount * OperandTypeCount];

typedef void (*BinaryOperationType)
(Context*, unsigned, Assembler::Operand*, Assembler::Operand*);
BinaryOperationType
BinaryOperations[BinaryOperationCount * OperandTypeCount * OperandTypeCount];

void
return_(Context* c)
{
  c->code.append(0xc3);
}

void
unconditional(Context* c, unsigned jump, Assembler::Constant* a)
{
  appendOffsetTask(c, a->value, c->code.length(), 5);

  c->code.append(jump);
  c->code.append4(0);
}

void
conditional(Context* c, unsigned condition, Assembler::Constant* a)
{
  appendOffsetTask(c, a->value, c->code.length(), 6);
  
  c->code.append(0x0f);
  c->code.append(condition);
  c->code.append4(0);
}

void
moveCR(Context*, unsigned, Assembler::Constant*, Assembler::Register*);

void
callR(Context*, unsigned, Assembler::Register*);

void
callC(Context* c, unsigned size, Assembler::Constant* a)
{
  assert(c, size == BytesPerWord);

  if (BytesPerWord == 8) {
    Assembler::Register r(r10);
    moveCR(c, size, a, &r);
    callR(c, size, &r);
  } else {
    unconditional(c, 0xe8, a);
  }
}

void
alignedCallC(Context* c, unsigned size, Assembler::Constant* a)
{
  if (BytesPerWord == 8) {
    while ((c->code.length() + 2) % 8) {
      c->code.append(0x90);
    }
  } else {
    while ((c->code.length() + 1) % 4) {
      c->code.append(0x90);
    }
  }
  callC(c, size, a);
}

void
callR(Context* c, unsigned size UNUSED, Assembler::Register* a)
{
  assert(c, size == BytesPerWord);

  if (a->low & 8) rex(c, 0x40, a->low);
  c->code.append(0xff);
  c->code.append(0xd0 | (a->low & 7));
}

void
callM(Context* c, unsigned size UNUSED, Assembler::Memory* a)
{
  assert(c, size == BytesPerWord);

  encode(c, 0xff, 2, a, false);
}

void
jumpR(Context* c, unsigned size UNUSED, Assembler::Register* a)
{
  assert(c, size == BytesPerWord);

  c->code.append(0xff);
  c->code.append(0xe0 | a->low);
}

void
jumpC(Context* c, unsigned size UNUSED, Assembler::Constant* a)
{
  assert(c, size == BytesPerWord);

  unconditional(c, 0xe9, a);
}

void
jumpM(Context* c, unsigned size UNUSED, Assembler::Memory* a)
{
  assert(c, size == BytesPerWord);

  encode(c, 0xff, 4, a, false);
}

void
jumpIfEqualC(Context* c, unsigned size UNUSED, Assembler::Constant* a)
{
  assert(c, size == BytesPerWord);

  conditional(c, 0x84, a);
}

void
jumpIfNotEqualC(Context* c, unsigned size UNUSED, Assembler::Constant* a)
{
  assert(c, size == BytesPerWord);

  conditional(c, 0x85, a);
}

void
jumpIfGreaterC(Context* c, unsigned size UNUSED, Assembler::Constant* a)
{
  assert(c, size == BytesPerWord);

  conditional(c, 0x8f, a);
}

void
jumpIfGreaterOrEqualC(Context* c, unsigned size UNUSED, Assembler::Constant* a)
{
  assert(c, size == BytesPerWord);

  conditional(c, 0x8d, a);
}

void
jumpIfLessC(Context* c, unsigned size UNUSED, Assembler::Constant* a)
{
  assert(c, size == BytesPerWord);

  conditional(c, 0x8c, a);
}

void
jumpIfLessOrEqualC(Context* c, unsigned size UNUSED, Assembler::Constant* a)
{
  assert(c, size == BytesPerWord);

  conditional(c, 0x8e, a);
}

void
pushR(Context*, unsigned, Assembler::Register*);

void
pushC(Context* c, unsigned size, Assembler::Constant* a)
{
  int64_t v = a->value->value();

  if (BytesPerWord == 4 and size == 8) {
    ResolvedPromise high((v >> 32) & 0xFFFFFFFF);
    Assembler::Constant ah(&high);

    ResolvedPromise low(v & 0xFFFFFFFF);
    Assembler::Constant al(&low);

    pushC(c, 4, &ah);
    pushC(c, 4, &al);
  } else {
    if (isInt8(v)) {
      c->code.append(0x6a);
      c->code.append(v);
    } else if (isInt32(v)) {
      c->code.append(0x68);
      c->code.append4(v);
    } else {
      Assembler::Register tmp(c->client->acquireTemporary());
      moveCR(c, size, a, &tmp);
      pushR(c, size, &tmp);
      c->client->releaseTemporary(tmp.low);
    }
  }
}

void
moveAR(Context*, unsigned, Assembler::Address*, Assembler::Register* b);

void
pushA(Context* c, unsigned size, Assembler::Address* a)
{
  assert(c, BytesPerWord == 8 or size == 4); // todo
  
  Assembler::Register tmp(c->client->acquireTemporary());
  moveAR(c, size, a, &tmp);
  pushR(c, size, &tmp);
  c->client->releaseTemporary(tmp.low);
}

void
pushR(Context* c, unsigned size, Assembler::Register* a)
{
  if (BytesPerWord == 4 and size == 8) {
    Assembler::Register ah(a->high);

    pushR(c, 4, &ah);
    pushR(c, 4, a);
  } else {
    c->code.append(0x50 | a->low);      
  }
}

void
pushM(Context* c, unsigned size, Assembler::Memory* a)
{
  if (BytesPerWord == 4 and size == 8) {
    Assembler::Memory ah(a->base, a->offset + 4, a->index, a->scale);

    pushM(c, 4, &ah);
    pushM(c, 4, a);
  } else {
    assert(c, BytesPerWord == 4 or size == 8);

    encode(c, 0xff, 6, a, false);    
  }
}

void
move4To8RR(Context* c, unsigned size, Assembler::Register* a,
           Assembler::Register* b);

void
popR(Context* c, unsigned size, Assembler::Register* a)
{
  if (BytesPerWord == 4 and size == 8) {
    Assembler::Register ah(a->high);

    popR(c, 4, a);
    popR(c, 4, &ah);
  } else {
    c->code.append(0x58 | a->low);
    if (BytesPerWord == 8 and size == 4) {
      move4To8RR(c, 0, a, a);
    }
  }
}

void
popM(Context* c, unsigned size, Assembler::Memory* a)
{
  if (BytesPerWord == 4 and size == 8) {
    Assembler::Memory ah(a->base, a->offset + 4, a->index, a->scale);

    popM(c, 4, a);
    popM(c, 4, &ah);
  } else {
    assert(c, BytesPerWord == 4 or size == 8);

    encode(c, 0x8f, 0, a, false);
  }
}

void
negateR(Context* c, unsigned size, Assembler::Register* a)
{
  assert(c, BytesPerWord == 8 or size == 4); // todo

  rex(c);
  c->code.append(0xf7);
  c->code.append(0xd8 | a->low);
}


void
leaMR(Context* c, unsigned size, Assembler::Memory* b, Assembler::Register* a)
{
  if (BytesPerWord == 8 and size == 4) {
    encode(c, 0x8d, a->low, b, false);
  } else {
    assert(c, BytesPerWord == 8 or size == 4);

    encode(c, 0x8d, a->low, b, true);
  }
}

void
moveCR(Context* c, unsigned size UNUSED, Assembler::Constant* a,
       Assembler::Register* b)
{
  assert(c, BytesPerWord == 8 or size == 4); // todo

  rex(c, 0x48, b->low);
  c->code.append(0xb8 | b->low);
  if (a->value->resolved()) {
    c->code.appendAddress(a->value->value());
  } else {
    appendImmediateTask(c, a->value, c->code.length());
    c->code.appendAddress(static_cast<uintptr_t>(0));
  }
}

void
moveCM(Context* c, unsigned size UNUSED, Assembler::Constant* a,
       Assembler::Memory* b)
{
  assert(c, isInt32(a->value->value())); // todo
  assert(c, BytesPerWord == 8 or size == 4); // todo

  encode(c, 0xc7, 0, b, true);
  c->code.append4(a->value->value());
}

void
moveRR(Context* c, unsigned size, Assembler::Register* a,
       Assembler::Register* b)
{
  if (BytesPerWord == 4 and size == 8) {
    Assembler::Register ah(a->high);
    Assembler::Register bh(b->high);

    moveRR(c, 4, a, b);
    moveRR(c, 4, &ah, &bh);
  } else {
    switch (size) {
    case 1:
      rex(c);
      if (BytesPerWord == 8) {
        c->code.append(0x0f);
      }
      c->code.append(0xbe);
      c->code.append(0xc0 | (b->low << 3) | a->low);
      break;

    case 2:
      rex(c);
      if (BytesPerWord == 8) {
        c->code.append(0x0f);
      }
      c->code.append(0xbf);
      c->code.append(0xc0 | (b->low << 3) | a->low);
      break;

    case 8:
    case 4:
      rex(c);
      c->code.append(0x89);
      c->code.append(0xc0 | (a->low << 3) | b->low);
      break;
    }
  }
}

void
moveRM(Context* c, unsigned size, Assembler::Register* a, Assembler::Memory* b)
{
  if (BytesPerWord == 4 and size == 8) {
    Assembler::Register ah(a->high);
    Assembler::Memory bh(b->base, b->offset + 4, b->index, b->scale);

    moveRM(c, 4, a, b);    
    moveRM(c, 4, &ah, &bh);
  } else if (BytesPerWord == 8 and size == 4) {
    encode(c, 0x89, a->low, b, false);
  } else {
    switch (size) {
    case 1:
      if (BytesPerWord == 8) {
        if (a->low > rbx) {
          encode2(c, 0x4088, a->low, b, false);
        } else {
          encode(c, 0x88, a->low, b, false);
        }
      } else {
        if (a->low > rbx) {
          Assembler::Register ax(c->client->acquireTemporary(rax));
          moveRR(c, BytesPerWord, a, &ax);
          moveRM(c, 1, &ax, b);
          c->client->releaseTemporary(ax.low);
        } else {
          encode(c, 0x88, a->low, b, false);
        }
      }
      break;

    case 2:
      encode2(c, 0x6689, a->low, b, false);
      break;

    case BytesPerWord:
      encode(c, 0x89, a->low, b, true);
      break;

    default: abort(c);
    }
  }
}

void
move4To8RR(Context* c, unsigned size UNUSED, Assembler::Register* a,
           Assembler::Register* b)
{
  if (BytesPerWord == 8) {
    rex(c);
    c->code.append(0x63);
    c->code.append(0xc0 | (b->low << 3) | a->low);
  } else {
    if (a->low == rax and b->low == rax and b->high == rdx) {
      c->code.append(0x99); // cdq
    } else {
      Assembler::Register axdx(c->client->acquireTemporary(rax),
                               c->client->acquireTemporary(rdx));

      moveRR(c, 4, a, &axdx);
      move4To8RR(c, 0, &axdx, &axdx);
      moveRR(c, 8, &axdx, b);

      c->client->releaseTemporary(axdx.low);
      c->client->releaseTemporary(axdx.high);
    }
  }
}

void
moveMR(Context* c, unsigned size, Assembler::Memory* a, Assembler::Register* b)
{
  switch (size) {
  case 1:
    encode2(c, 0x0fbe, b->low, a, true);
    break;

  case 2:
    encode2(c, 0x0fbf, b->low, a, true);
    break;

  case 4:
  case 8:
    if (BytesPerWord == 4 and size == 8) {
      Assembler::Memory ah(a->base, a->offset + 4, a->index, a->scale);
      Assembler::Register bh(b->high);

      moveMR(c, 4, a, b);    
      moveMR(c, 4, &ah, &bh);
    } else if (BytesPerWord == 8 and size == 4) {
      encode(c, 0x63, b->low, a, true);
    } else {
      encode(c, 0x8b, b->low, a, true);
    }
    break;

  default: abort(c);
  }
}

void
moveAR(Context* c, unsigned size, Assembler::Address* a,
       Assembler::Register* b)
{
  assert(c, BytesPerWord == 8 or size == 4); // todo

  Assembler::Constant constant(a->address);
  Assembler::Memory memory(b->low, 0, -1, 0);

  moveCR(c, size, &constant, b);
  moveMR(c, size, &memory, b);
}

void
moveAM(Context* c, unsigned size, Assembler::Address* a,
       Assembler::Memory* b)
{
  assert(c, BytesPerWord == 8 or size == 4); // todo

  Assembler::Register tmp(c->client->acquireTemporary());
  moveAR(c, size, a, &tmp);
  moveRM(c, size, &tmp, b);
  c->client->releaseTemporary(tmp.low);
}

void
moveMM(Context* c, unsigned size, Assembler::Memory* a,
       Assembler::Memory* b)
{
  if (BytesPerWord == 8 or size <= 4) {
    Assembler::Register tmp(c->client->acquireTemporary());
    moveMR(c, size, a, &tmp);
    moveRM(c, size, &tmp, b);
    c->client->releaseTemporary(tmp.low);
  } else {
    Assembler::Register tmp(c->client->acquireTemporary(),
                            c->client->acquireTemporary());
    moveMR(c, size, a, &tmp);
    moveRM(c, size, &tmp, b);    
    c->client->releaseTemporary(tmp.low);
    c->client->releaseTemporary(tmp.high);
  }
}

void
move4To8MR(Context* c, unsigned, Assembler::Memory* a, Assembler::Register* b)
{
  if (BytesPerWord == 8) {
    encode(c, 0x63, b->low, a, true);
  } else {
    Assembler::Register axdx(c->client->acquireTemporary(rax),
                             c->client->acquireTemporary(rdx));

    moveMR(c, 4, a, &axdx);
    move4To8RR(c, 0, &axdx, &axdx);
    moveRR(c, 8, &axdx, b);

    c->client->releaseTemporary(axdx.low);
    c->client->releaseTemporary(axdx.high);
  }
}

void
moveZMR(Context* c, unsigned size, Assembler::Memory* a,
        Assembler::Register* b)
{
  switch (size) {
  case 2:
    encode2(c, 0x0fb7, b->low, a, true);
    break;

  default: abort(c); // todo
  }
}

void
moveZRR(Context* c, unsigned size, Assembler::Register* a,
        Assembler::Register* b)
{
  switch (size) {
  case 2:
    rex(c);
    c->code.append(0x0f);
    c->code.append(0xb7);
    c->code.append(0xc0 | (b->low << 3) | a->low);
    break;

  default: abort(c); // todo
  }
}

void
addCR(Context* c, unsigned size UNUSED, Assembler::Constant* a,
      Assembler::Register* b)
{
  assert(c, BytesPerWord == 8 or size == 4); // todo

  int64_t v = a->value->value();
  if (v) {
    rex(c);
    if (isInt8(v)) {
      c->code.append(0x83);
      c->code.append(0xc0 | b->low);
      c->code.append(v);
    } else if (isInt32(v)) {
      c->code.append(0x81);
      c->code.append(0xc0 | b->low);
      c->code.append4(v);        
    } else {
      abort(c);
    }
  }
}

void
subtractCR(Context* c, unsigned size UNUSED, Assembler::Constant* a,
           Assembler::Register* b)
{
  assert(c, BytesPerWord == 8 or size == 4); // todo

  int64_t v = a->value->value();
  if (v) {
    rex(c);
    if (isInt8(v)) {
      c->code.append(0x83);
      c->code.append(0xe8 | b->low);
      c->code.append(v);
    } else if (isInt32(v)) {
      c->code.append(0x81);
      c->code.append(0xe8 | b->low);
      c->code.append4(v);        
    } else {
      abort(c);
    }
  }
}

void
subtractRR(Context* c, unsigned size UNUSED, Assembler::Register* a,
           Assembler::Register* b)
{
  assert(c, BytesPerWord == 8 or size == 4); // todo

  rex(c);
  c->code.append(0x29);
  c->code.append(0xc0 | (a->low << 3) | b->low);
}

void
addRR(Context* c, unsigned size UNUSED, Assembler::Register* a,
      Assembler::Register* b)
{
  assert(c, BytesPerWord == 8 or size == 4); // todo

  rex(c);
  c->code.append(0x01);
  c->code.append(0xc0 | (a->low << 3) | b->low);
}

void
addRM(Context* c, unsigned size UNUSED, Assembler::Register* a,
      Assembler::Memory* b)
{
  assert(c, BytesPerWord == 8 or size == 4);

  encode(c, 0x01, a->low, b, true);
}

void
multiplyRR(Context* c, unsigned size, Assembler::Register* a,
           Assembler::Register* b)
{
  assert(c, BytesPerWord == 8 or size == 4); // todo

  rex(c);
  c->code.append(0x0f);
  c->code.append(0xaf);
  c->code.append(0xc0 | (b->low << 3) | a->low);
}

void
multiplyCR(Context* c, unsigned size, Assembler::Constant* a,
           Assembler::Register* b)
{
  assert(c, BytesPerWord == 8 or size == 4); // todo

  int64_t v = a->value->value();
  if (v) {
    if (isInt32(v)) {
      rex(c);
      if (isInt8(v)) {
        c->code.append(0x6b);
        c->code.append(0xc0 | (b->low << 3) | b->low);
        c->code.append(v);
      } else {
        c->code.append(0x69);
        c->code.append(0xc0 | (b->low << 3) | b->low);
        c->code.append4(v);        
      }
    } else {
      Assembler::Register tmp(c->client->acquireTemporary());
      moveCR(c, size, a, &tmp);
      multiplyRR(c, size, &tmp, b);
      c->client->releaseTemporary(tmp.low);      
    }
  }
}

void
divideRR(Context* c, unsigned size, Assembler::Register* a,
         Assembler::Register* b)
{
  if (BytesPerWord == 4 and size == 8) {
    abort(c); // todo: sync stack first

    Assembler::Register axdx(c->client->acquireTemporary(rax),
                             c->client->acquireTemporary(rdx));

    pushR(c, size, a);
    pushR(c, size, b);
    
    ResolvedPromise addressPromise(reinterpret_cast<intptr_t>(divideLong));
    Assembler::Constant address(&addressPromise);
    callC(c, BytesPerWord, &address);

    ResolvedPromise offsetPromise(16);
    Assembler::Constant offset(&offsetPromise);
    Assembler::Register stack(rsp);
    addCR(c, BytesPerWord, &offset, &stack);

    moveRR(c, size, &axdx, b);

    c->client->releaseTemporary(axdx.low);
    c->client->releaseTemporary(axdx.high);
  } else {
    Assembler::Register ax(rax);
    Assembler::Register divisor(a->low);

    if (a->low == rdx) {
      divisor.low = c->client->acquireTemporary();
      moveRR(c, BytesPerWord, a, &divisor);      
    } else if (b->low != rdx) {
      c->client->save(rdx);
    }

    if (b->low != rax) {
      c->client->save(rax);
      c->client->acquireTemporary(rax);
      moveRR(c, BytesPerWord, b, &ax);
    }
    
    rex(c);
    c->code.append(0x99);
    rex(c);
    c->code.append(0xf7);
    c->code.append(0xf8 | divisor.low);

    if (b->low != rax) {
      moveRR(c, BytesPerWord, &ax, b);
      c->client->releaseTemporary(rax);
      c->client->restore(rax);
    }

    if (a->low == rdx) {
      moveRR(c, BytesPerWord, &divisor, a);
      c->client->releaseTemporary(divisor.low);
    } else if (b->low != rdx) {
      c->client->restore(rdx);
    }
  }
}

void
divideCR(Context* c, unsigned size, Assembler::Constant* a,
         Assembler::Register* b)
{
  assert(c, BytesPerWord == 8 or size == 4); // todo

  Assembler::Register tmp(c->client->acquireTemporary());
  moveCR(c, size, a, &tmp);
  divideRR(c, size, &tmp, b);
  c->client->releaseTemporary(tmp.low);  
}

void
remainderRR(Context* c, unsigned size, Assembler::Register* a,
            Assembler::Register* b)
{
  if (BytesPerWord == 4 and size == 8) {
    abort(c); // todo: sync stack first

    Assembler::Register axdx(c->client->acquireTemporary(rax),
                             c->client->acquireTemporary(rdx));

    pushR(c, size, a);
    pushR(c, size, b);
    
    ResolvedPromise addressPromise(reinterpret_cast<intptr_t>(moduloLong));
    Assembler::Constant address(&addressPromise);
    callC(c, BytesPerWord, &address);

    ResolvedPromise offsetPromise(16);
    Assembler::Constant offset(&offsetPromise);
    Assembler::Register stack(rsp);
    addCR(c, BytesPerWord, &offset, &stack);

    moveRR(c, size, &axdx, b);

    c->client->releaseTemporary(axdx.low);
    c->client->releaseTemporary(axdx.high);
  } else {
    Assembler::Register ax(rax);
    Assembler::Register dx(rdx);
    Assembler::Register divisor(a->low);

    if (a->low == rdx) {
      divisor.low = c->client->acquireTemporary();
      moveRR(c, BytesPerWord, a, &divisor);      
    } else if (b->low != rdx) {
      c->client->save(rdx);
    }

    if (b->low != rax) {
      c->client->save(rax);
      c->client->acquireTemporary(rax);
      moveRR(c, BytesPerWord, b, &ax);
    }
    
    rex(c);
    c->code.append(0x99);
    rex(c);
    c->code.append(0xf7);
    c->code.append(0xf8 | divisor.low);

    if (b->low != rdx) {
      moveRR(c, BytesPerWord, &dx, b);
      c->client->releaseTemporary(rax);
      c->client->restore(rax);
    }

    if (a->low == rdx) {
      moveRR(c, BytesPerWord, &divisor, a);
      c->client->releaseTemporary(divisor.low);
    } else if (b->low != rdx) {
      c->client->restore(rdx);
    }
  }
}

void
andRR(Context* c, unsigned size UNUSED, Assembler::Register* a,
      Assembler::Register* b)
{
  assert(c, BytesPerWord == 8 or size == 4); // todo

  rex(c);
  c->code.append(0x21);
  c->code.append(0xc0 | (a->low << 3) | b->low);
}

void
andCR(Context* c, unsigned size UNUSED, Assembler::Constant* a,
      Assembler::Register* b)
{
  assert(c, BytesPerWord == 8 or size == 4); // todo

  int64_t v = a->value->value();

  if (isInt32(v)) {
    rex(c);
    if (isInt8(v)) {
      c->code.append(0x83);
      c->code.append(0xe0 | b->low);
      c->code.append(v);
    } else {
      c->code.append(0x81);
      c->code.append(0xe0 | b->low);
      c->code.append(v);
    }
  } else {
    Assembler::Register tmp(c->client->acquireTemporary());
    moveCR(c, size, a, &tmp);
    andRR(c, size, &tmp, b);
    c->client->releaseTemporary(tmp.low);
  }
}

void
andCM(Context* c, unsigned size UNUSED, Assembler::Constant* a,
      Assembler::Memory* b)
{
  assert(c, BytesPerWord == 8 or size == 4);

  int64_t v = a->value->value();

  encode(c, isInt8(a->value->value()) ? 0x83 : 0x81, 5, b, true);
  if (isInt8(v)) {
    c->code.append(v);
  } else if (isInt32(v)) {
    c->code.append4(v);
  } else {
    abort(c);
  }
}

void
xorRR(Context* c, unsigned size UNUSED, Assembler::Register* a,
      Assembler::Register* b)
{
  assert(c, BytesPerWord == 8 or size == 4); // todo

  rex(c);
  c->code.append(0x31);
  c->code.append(0xc0 | (a->low << 3) | b->low);
}

void
xorCR(Context* c, unsigned size UNUSED, Assembler::Constant* a,
      Assembler::Register* b)
{
  assert(c, BytesPerWord == 8 or size == 4); // todo

  int64_t v = a->value->value();
  if (v) {
    if (isInt32(v)) {
      rex(c);
      if (isInt8(v)) {
        c->code.append(0x83);
        c->code.append(0xf0 | b->low);
        c->code.append(v);
      } else {
        c->code.append(0x81);
        c->code.append(0xf0 | b->low);
        c->code.append4(v);        
      }
    } else {
      Assembler::Register tmp(c->client->acquireTemporary());
      moveCR(c, size, a, &tmp);
      xorRR(c, size, &tmp, b);
      c->client->releaseTemporary(tmp.low);
    }
  }
}

void
shiftLeftCR(Context* c, unsigned size UNUSED, Assembler::Constant* a,
            Assembler::Register* b)
{
  assert(c, BytesPerWord == 8 or size == 4);

  int64_t v = a->value->value();

  rex(c);
  if (v == 1) {
    c->code.append(0xd1);
    c->code.append(0xe0 | b->low);
  } else if (isInt8(v)) {
    c->code.append(0xc1);
    c->code.append(0xe0 | b->low);
    c->code.append(v);
  } else {
    abort(c);
  }
}

void
compareCR(Context* c, unsigned size UNUSED, Assembler::Constant* a,
          Assembler::Register* b)
{
  assert(c, BytesPerWord == 8 or size == 4);

  int64_t v = a->value->value();

  if (size == 8) rex(c);
  if (isInt8(v)) {
    c->code.append(0x83);
    c->code.append(0xf8 | b->low);
    c->code.append(v);
  } else if (isInt32(v)) {
    c->code.append(0x81);
    c->code.append(0xf8 | b->low);
    c->code.append4(v);
  } else {
    abort(c);
  }
}

void
compareRR(Context* c, unsigned size UNUSED, Assembler::Register* a,
          Assembler::Register* b)
{
  if (BytesPerWord == 4 and size == 8) {
    Assembler::Register ah(a->high);
    Assembler::Register bh(b->high);

    compareRR(c, 4, &ah, &bh);

    // if the high order bits are equal, we compare the low order
    // bits; otherwise, we jump past that comparison
    c->code.append(0x0f);
    c->code.append(0x85); // jne
    c->code.append4(2);

    compareRR(c, 4, a, b);
  } else {
    if (size == 8) rex(c);
    c->code.append(0x39);
    c->code.append(0xc0 | (a->low << 3) | b->low);
  }
}

void
compareCM(Context* c, unsigned size UNUSED, Assembler::Constant* a,
          Assembler::Memory* b)
{
  assert(c, BytesPerWord == 8 or size == 4);

  encode(c, isInt8(a->value->value()) ? 0x83 : 0x81, 7, b, true);

  if (isInt8(a->value->value())) {
    c->code.append(a->value->value());
  } else if (isInt32(a->value->value())) {
    c->code.append4(a->value->value());
  } else {
    abort(c);
  }
}

void
compareRM(Context* c, unsigned size UNUSED, Assembler::Register* a,
          Assembler::Memory* b)
{
  assert(c, BytesPerWord == 8 or size == 4); // todo

  encode(c, 0x39, a->low, b, true);
}

void
compareMR(Context* c, unsigned size UNUSED, Assembler::Memory* a,
          Assembler::Register* b)
{
  assert(c, BytesPerWord == 8 or size == 4); // todo

  encode(c, 0x3b, b->low, a, true);
}

void
compareMM(Context* c, unsigned size UNUSED, Assembler::Memory* a,
          Assembler::Memory* b)
{
  assert(c, BytesPerWord == 8 or size == 4); // todo

  Assembler::Register tmp(c->client->acquireTemporary());
  moveMR(c, size, a, &tmp);
  compareRM(c, size, &tmp, b);
  c->client->releaseTemporary(tmp.low);
}

void
populateTables()
{
  Operations[Return] = return_;

  const int Constant = ConstantOperand;
  const int Address = AddressOperand;
  const int Register = RegisterOperand;
  const int Memory = MemoryOperand;

  UnaryOperations[INDEX1(Call, Constant)] = CAST1(callC);
  UnaryOperations[INDEX1(Call, Register)] = CAST1(callR);
  UnaryOperations[INDEX1(Call, Memory)] = CAST1(callM);

  UnaryOperations[INDEX1(AlignedCall, Constant)] = CAST1(alignedCallC);

  UnaryOperations[INDEX1(Jump, Register)] = CAST1(jumpR);
  UnaryOperations[INDEX1(Jump, Constant)] = CAST1(jumpC);
  UnaryOperations[INDEX1(Jump, Memory)] = CAST1(jumpM);

  UnaryOperations[INDEX1(JumpIfEqual, Constant)] = CAST1(jumpIfEqualC);
  UnaryOperations[INDEX1(JumpIfNotEqual, Constant)] = CAST1(jumpIfNotEqualC);
  UnaryOperations[INDEX1(JumpIfGreater, Constant)] = CAST1(jumpIfGreaterC);
  UnaryOperations[INDEX1(JumpIfGreaterOrEqual, Constant)]
    = CAST1(jumpIfGreaterOrEqualC);
  UnaryOperations[INDEX1(JumpIfLess, Constant)] = CAST1(jumpIfLessC);
  UnaryOperations[INDEX1(JumpIfLessOrEqual, Constant)]
    = CAST1(jumpIfLessOrEqualC);

  UnaryOperations[INDEX1(Push, Constant)] = CAST1(pushC);
  UnaryOperations[INDEX1(Push, Address)] = CAST1(pushA);
  UnaryOperations[INDEX1(Push, Register)] = CAST1(pushR);
  UnaryOperations[INDEX1(Push, Memory)] = CAST1(pushM);

  UnaryOperations[INDEX1(Pop, Register)] = CAST1(popR);
  UnaryOperations[INDEX1(Pop, Memory)] = CAST1(popM);

  UnaryOperations[INDEX1(Negate, Register)] = CAST1(negateR);

  BinaryOperations[INDEX2(LoadAddress, Memory, Register)] = CAST2(leaMR);

  BinaryOperations[INDEX2(Move, Constant, Register)] = CAST2(moveCR);
  BinaryOperations[INDEX2(Move, Constant, Memory)] = CAST2(moveCM);
  BinaryOperations[INDEX2(Move, Register, Memory)] = CAST2(moveRM);
  BinaryOperations[INDEX2(Move, Register, Register)] = CAST2(moveRR);
  BinaryOperations[INDEX2(Move, Memory, Register)] = CAST2(moveMR);
  BinaryOperations[INDEX2(Move, Address, Register)] = CAST2(moveAR);
  BinaryOperations[INDEX2(Move, Address, Memory)] = CAST2(moveAM);
  BinaryOperations[INDEX2(Move, Memory, Memory)] = CAST2(moveMM);

  BinaryOperations[INDEX2(Move4To8, Register, Register)] = CAST2(move4To8RR);
  BinaryOperations[INDEX2(Move4To8, Memory, Register)] = CAST2(move4To8MR);

  BinaryOperations[INDEX2(MoveZ, Memory, Register)] = CAST2(moveZMR);
  BinaryOperations[INDEX2(MoveZ, Register, Register)] = CAST2(moveZRR);

  BinaryOperations[INDEX2(Add, Constant, Register)] = CAST2(addCR);
  BinaryOperations[INDEX2(Add, Register, Register)] = CAST2(addRR);
  BinaryOperations[INDEX2(Add, Register, Memory)] = CAST2(addRM);

  BinaryOperations[INDEX2(Multiply, Register, Register)] = CAST2(multiplyRR);
  BinaryOperations[INDEX2(Multiply, Constant, Register)] = CAST2(multiplyCR);

  BinaryOperations[INDEX2(Divide, Register, Register)] = CAST2(divideRR);
  BinaryOperations[INDEX2(Divide, Constant, Register)] = CAST2(divideCR);

  BinaryOperations[INDEX2(Remainder, Register, Register)] = CAST2(remainderRR);

  BinaryOperations[INDEX2(And, Register, Register)] = CAST2(andRR);
  BinaryOperations[INDEX2(And, Constant, Register)] = CAST2(andCR);
  BinaryOperations[INDEX2(And, Constant, Memory)] = CAST2(andCM);

  BinaryOperations[INDEX2(Xor, Register, Register)] = CAST2(xorRR);
  BinaryOperations[INDEX2(Xor, Constant, Register)] = CAST2(xorCR);

  BinaryOperations[INDEX2(ShiftLeft, Constant, Register)] = CAST2(shiftLeftCR);

  BinaryOperations[INDEX2(Subtract, Constant, Register)] = CAST2(subtractCR);
  BinaryOperations[INDEX2(Subtract, Register, Register)] = CAST2(subtractRR);

  BinaryOperations[INDEX2(Compare, Constant, Register)] = CAST2(compareCR);
  BinaryOperations[INDEX2(Compare, Register, Register)] = CAST2(compareRR);
  BinaryOperations[INDEX2(Compare, Register, Memory)] = CAST2(compareRM);
  BinaryOperations[INDEX2(Compare, Memory, Register)] = CAST2(compareMR);
  BinaryOperations[INDEX2(Compare, Constant, Memory)] = CAST2(compareCM);
  BinaryOperations[INDEX2(Compare, Memory, Memory)] = CAST2(compareMM);
}

class MyAssembler: public Assembler {
 public:
  MyAssembler(System* s, Allocator* a, Zone* zone): c(s, a, zone) {
    static bool populated = false;
    if (not populated) {
      populated = true;
      populateTables();
    }
  }

  virtual void setClient(Client* client) {
    assert(&c, c.client == 0);
    c.client = client;
  }

  virtual unsigned registerCount() {
    return 8;//BytesPerWord == 4 ? 8 : 16;
  }

  virtual int base() {
    return rbp;
  }

  virtual int stack() {
    return rsp;
  }

  virtual int thread() {
    return rbx;
  }

  virtual int returnLow() {
    return rax;
  }

  virtual int returnHigh() {
    return (BytesPerWord == 4 ? rdx : NoRegister);
  }

  virtual unsigned argumentRegisterCount() {
    return (BytesPerWord == 4 ? 0 : 6);
  }

  virtual int argumentRegister(unsigned index) {
    assert(&c, BytesPerWord == 8);

    switch (index) {
    case 0:
      return rdi;
    case 1:
      return rsi;
    case 2:
      return rdx;
    case 3:
      return rcx;
    case 4:
      return r8;
    case 5:
      return r9;
    default:
      abort(&c);
    }
  }

  virtual void getTargets(UnaryOperation /*op*/, unsigned /*size*/,
                          Register* r)
  {
    // todo
    r->low = NoRegister;
    r->high = NoRegister;
  }

  virtual void getTargets(BinaryOperation /*op*/, unsigned /*size*/,
                          Register* a, Register* b)
  {
    // todo
    a->low = NoRegister;
    a->high = NoRegister;
    b->low = NoRegister;
    b->high = NoRegister;
  }

  virtual void apply(Operation op) {
    Operations[op](&c);
  }

  virtual void apply(UnaryOperation op, unsigned size,
                     OperandType type, Operand* operand)
  {
    UnaryOperations[INDEX1(op, type)](&c, size, operand);
  }

  virtual void apply(BinaryOperation op, unsigned size,
                     OperandType aType, Operand* a,
                     OperandType bType, Operand* b)
  {
    BinaryOperations[INDEX2(op, aType, bType)](&c, size, a, b);
  }

  virtual void writeTo(uint8_t* dst) {
    c.result = dst;
    memcpy(dst, c.code.data, c.code.length());
    
    for (Task* t = c.tasks; t; t = t->next) {
      t->run(&c);
    }
  }

  virtual unsigned length() {
    return c.code.length();
  }

  virtual void updateCall(void* returnAddress, void* newTarget) {
    if (BytesPerWord == 8) {
      uint8_t* instruction = static_cast<uint8_t*>(returnAddress) - 13;
      assert(&c, instruction[0] == 0x49);
      assert(&c, instruction[1] == 0xba);
      assert(&c, instruction[10] == 0x41);
      assert(&c, instruction[11] == 0xff);
      assert(&c, instruction[12] == 0xd2);
      assert(&c, reinterpret_cast<uintptr_t>(instruction + 2) % 8 == 0);

      intptr_t v = reinterpret_cast<intptr_t>(newTarget);
      memcpy(instruction + 2, &v, 8);
    } else {
      uint8_t* instruction = static_cast<uint8_t*>(returnAddress) - 5;
      assert(&c, *instruction == 0xE8);
      assert(&c, reinterpret_cast<uintptr_t>(instruction + 1) % 4 == 0);

      int32_t v = static_cast<uint8_t*>(newTarget)
        - static_cast<uint8_t*>(returnAddress);
      memcpy(instruction + 1, &v, 4);
    }
  }

  virtual void dispose() {
    c.code.dispose();
  }

  Context c;
};

} // namespace

namespace vm {

Assembler*
makeAssembler(System* system, Allocator* allocator, Zone* zone)
{
  return new (zone->allocate(sizeof(MyAssembler)))
    MyAssembler(system, allocator, zone);  
}

} // namespace vm
