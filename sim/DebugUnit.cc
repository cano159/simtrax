#include "DebugUnit.h"
#include "SimpleRegisterFile.h"
#include "IssueUnit.h"
#include "ThreadState.h"
#include "WriteRequest.h"
#include <float.h>
#include <math.h>
#include <cassert>
#include <string.h>

DebugUnit::DebugUnit(int _latency) :
    FunctionalUnit(_latency)
{
}

// From FunctionalUnit
bool DebugUnit::SupportsOp(Instruction::Opcode op) const
{
  if (op == Instruction::PRINT ||
      op == Instruction::PRINTF ||
      op == Instruction::CLOCK)
    return true;
  else
    return false;
}

bool DebugUnit::AcceptInstruction(Instruction& ins, IssueUnit* issuer, ThreadState* thread)
{
  if (ins.args[0]==0)
    printf("\n");
  else {
    reg_value arg;
    reg_value result;
    int write_reg = ins.args[0];
    long long int write_cycle = issuer->current_cycle + latency;
    Instruction::Opcode failop = Instruction::NOP;
    if(ins.op != Instruction::CLOCK) // clock doesn't read anything
      {
	if(!thread->ReadRegister(ins.args[0], issuer->current_cycle, arg, failop))
	  {
	    printf("PRINT: failed to read register\n");
	  }
      }

    switch(ins.op)
    {

      case Instruction::PRINT:
        if(registers == NULL)
        {
          // Use register number
          printf("<%p> Register %u has value %d, %u, %f\n",
                 thread, ins.args[0], arg.idata, arg.udata, arg.fdata);
        }
        else
        {
          // Use register symbol
          printf("<Thread: %p> Register %u (\"%s\") has value %d, %u, %f\n",
                 thread, ins.args[0], registers->at(ins.args[0])->names[0], arg.idata, arg.udata, arg.fdata);
        }
        break;

      case Instruction::PRINTF:
        PrintFormatString(arg.udata, thread);
        break;

    case Instruction::CLOCK:
      result.udata = issuer->current_cycle;
      break;
        
      default:
        printf("Error: DebugUnit executing unsupported op\n");
        exit(1);
        break;
    }
  
    // Write the value(s)
    if (ins.op == Instruction::CLOCK && !thread->QueueWrite(write_reg, result, write_cycle, ins.op, &ins))
      {
	// pipeline hazzard
	return false;
      }
  }
  return true;
}

// Simple implementation of similar to stdio::printf
void DebugUnit::PrintFormatString(int format_addr, ThreadState* thread)
{
  // format_addr is the local stack address of the format string pointer (char**)

  // load the address of the pointer to the string
  int string_stack_addr = ((FourByte *)((char *)(ls_unit->storage[thread->thread_id]) + format_addr))->uvalue;

  // load the address of the string
  char* str = ((char *)(ls_unit->storage[thread->thread_id]) + string_stack_addr);

  // Now parse the format string:
  // Support %f, %d, %u, %c

  // All subsequent optional arguments are at consecutive word addresses on the stack
  //int next_arg_addr = format_addr + 4;
  // MIPS compiler is placing the first vararg 12-bytes away from the string address
  int next_arg_addr = format_addr + 12;

  std::string parsed;
  char temp[32];

  while(*str != '\0')
  {
    temp[0] = '\0';
    if(*str != '%')
      parsed += *str;
    else
    {
      str++; // get next char after format token (%)
      if(*str == '\0')
        break;
      
      switch(*str)
      {
        case 'f': // %f
          // align to 8-byte boundary
          next_arg_addr = (next_arg_addr % 8) ? next_arg_addr + 4 : next_arg_addr;
          sprintf(temp, "%f",
                  *(((double *)((char *)(ls_unit->storage[thread->thread_id]) + next_arg_addr))));
          
          // floats get promoted to double for variadic functions, add an extra 4
          next_arg_addr += 4;
          break;
          
        case 'd': // %d
          sprintf(temp, "%d",
                  ((FourByte *)((char *)(ls_unit->storage[thread->thread_id]) + next_arg_addr))->ivalue);
          break;
          
        case 'u':
          sprintf(temp, "%u",
                  ((FourByte *)((char *)(ls_unit->storage[thread->thread_id]) + next_arg_addr))->uvalue);
          break;
          
        case 'c': // %c
          sprintf(temp, "%c",
                  (char)(((FourByte *)((char *)(ls_unit->storage[thread->thread_id]) + next_arg_addr))->uvalue));
          break;
      }
      parsed += temp;
      next_arg_addr += 4;
    }
    str++;
  }

  printf("<Thread: %p> %s\n", thread, parsed.c_str());
}

// From HardwareModule
void DebugUnit::ClockRise()
{
  // We do nothing on rise (or read from register file on first cycle, but
  // we can probably claim that this was done already)
}

void DebugUnit::ClockFall()
{
}

void DebugUnit::print()
{
}
