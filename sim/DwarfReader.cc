#include <stdio.h>
#include <stdint.h>
#include <algorithm>
#include <queue>
#include "DwarfReader.h"
#include "Assembler.h"


// Helpers
unsigned char readByte(const char* jump_table, int& addr);
uint16_t readHalf(const char* jump_table, int& addr);
uint32_t readWord(const char* jump_table, int& addr);
unsigned int readVarSize(const char* jump_table, int& addr, int size);
unsigned int readVarType(const char* jump_table, int& addr, int type);

DwarfReader::DwarfReader()
{
  rootRuntime = NULL;
}


// Using debug symbols, create a tree representation of the source info with program counter ranges
bool DwarfReader::BuildSourceTree(const std::vector<symbol*> labels, std::vector<symbol*> elf_vars,
			       const std::vector<symbol*> data_table, const char* jump_table, 
			       int jtable_size, int debug_start, int abbrev_start)
{
  int unit_ptr = debug_start;

  // Find the addresses of each DWARF abbreviation unit
  FindAbbrevCodes(jump_table, abbrev_start, jtable_size);
  
  // Read each compilation unit until done
  while(unit_ptr < abbrev_start)
    {
      // Read fixed header data
      int unit_length;
      int start_unit = unit_ptr;
      ReadUnitHeader(unit_length, jump_table, unit_ptr);

      // Read the rest of the unit data
      rootSource.children.push_back(ReadCompilationUnit(labels, elf_vars, data_table, 
							jump_table, jtable_size, unit_ptr, 
							debug_start, abbrev_start,
							start_unit, 0));
    }

  // Combine all units and their children in to one list
  for(size_t i = 0; i < rootSource.children.size(); i++)
    AddUnitList(&(rootSource.children[i]));

  // Update names of reference units (DW_AT_specification, DW_AT_abstract_origin)
  // N^2 algorithm, but who cares, N is small
  // Meanwhile, find "main"
  for(size_t i = 0; i < unit_list.size(); i++)
    {
      if(unit_list[i]->name.compare(std::string("main")) == 0)
	{
	  if(unit_list[i]->ranges.size() > 0)
	    {
	      rootRuntime = new RuntimeNode();
	      rootRuntime->source_node = (unit_list[i]);
	    }
	}
	
      // Check if the unit has a reference
      if(unit_list[i]->pointsTo >= 0)
	{
	  for(size_t j = 0; j < unit_list.size(); j++)
	    if(unit_list[j]->addr == unit_list[i]->pointsTo)
	      {
		unit_list[i]->name = unit_list[j]->name;
	      }
	}
    }

  if(rootRuntime == NULL)
    {
      printf("ERROR: (--profile): Found no \"main\" routine. Did you compile your TRaX project with -g?\n");
      exit(1);
    }
  
  // For debugging. May be useful
  // Writes a graphviz file representing the source tree
  WriteDot("sourcetree.dot");

  return true;
 
 }
 
 
// Read one debugging information entry and its children
// Note that these are entries and not what DWARF technically calls "compilation units"
// DWARF compilation units are considered a debugging entry here
// jump_table is the program's raw data segment
// data_table is the "symbol" representation of the data segment (gives info about each entry)
CompilationUnit DwarfReader::ReadCompilationUnit(const std::vector<symbol*> labels, std::vector<symbol*> elf_vars, 
					      const std::vector<symbol*> data_table, const char* jump_table, 
					      int jtable_size, int& current_addr, 
					      int debug_start, int abbrev_start, int top_unit_start,
					      int top_unit_pc)
{
  
  if(current_addr < debug_start || current_addr >= abbrev_start)
    {
      printf("ERROR (--profile): Invalid profile data. Did you compile your TRaX project with -g?\n");
      exit(1);
    }

  CompilationUnit retval;

  retval.addr = current_addr;
  retval.top_level_addr = top_unit_start;

  // The unit's abbreviation code is always the first byte
  int abbrev_code = readByte(jump_table, current_addr);
  retval.abbrev = abbrev_code;

  // End of children marker
  if(abbrev_code == 0)
    return retval;
    
  // Find this unit's address in the vector of symbol objects created by the assembler.
  // This gives us more info about the jump_table (like the size of each entry)
  int dtable_ptr = -1;
  for(size_t i=0; i < data_table.size(); i++)
    if(current_addr == data_table[i]->address)
      {
	dtable_ptr = i;
	break;
      }

  // Make sure we found it
  if(dtable_ptr < 0)
    {
      printf("ERROR (--profile): Invalid profile data (did you compile your TRaX project with -g?)\n");
      exit(1);
    }

  // Find the DWARF abbreviation table entry
  int abbrev_addr = -1;
  for(size_t i=0; i < abbrevCodes.size(); i++)
    if(abbrevCodes[i].code == abbrev_code)
      {
	abbrev_addr = abbrevCodes[i].addr;
	break;
      }

  // Make sure we found it
  if(abbrev_addr < 0)
    {
      printf("ERROR (--profile): Invalid profile data (did you compile your TRaX project with -g?)\n");
      exit(1);
    }

  // Consume the abbrev code itself
  readByte(jump_table, abbrev_addr);
  // Tag is always next
  retval.tag = readByte(jump_table, abbrev_addr);
  // Children is next
  bool has_children = readByte(jump_table, abbrev_addr);

  // Read attribute/type pairs one at a time
  while(true)
    {
      // Read attribute code
      unsigned int attribute = readByte(jump_table, abbrev_addr);
      if(attribute == DW_AT_MIPS_linkage_name || attribute == DW_AT_APPLE_optimized)
	{
	  // These attributes are larger than others
	  // Throw away the extra byte associated with these
	  readByte(jump_table, abbrev_addr); 
	}
      // Read type code
      unsigned int type = readByte(jump_table, abbrev_addr);
      if(attribute == 0 && type == 0) // EOM marker
	break;

      // Read and parse the attribute data
      if(!ReadAttribute(retval, attribute, type, debug_start, jump_table, current_addr, data_table, dtable_ptr, top_unit_pc))
	exit(1);

    }
  
  // If this is the top-level unit corresponding to an object file (a compilation unit)
  // Its children may use its base address as an offset
  if(retval.tag == DW_TAG_compile_unit)
    {
      if(retval.ranges.size() == 1)
	top_unit_pc = retval.ranges[0].first;
      else
	top_unit_pc = 0;
    }

  // Recursively read this unit's children
  if(has_children)
    {
      while(true)
	{
	  CompilationUnit child = ReadCompilationUnit(labels, elf_vars, data_table, 
						      jump_table, jtable_size, current_addr, 
						      debug_start, abbrev_start, top_unit_start,
						      top_unit_pc);
	  // Propogate a class unit's name down to its children
	  if(retval.tag == DW_TAG_class_type && retval.name.size() > 0)
	    if(child.name.size() > 0)
	      child.name = retval.name + "::" + child.name;
	  
	  // End of children
	  if(child.abbrev == 0)
	    break;

	  retval.children.push_back(child);
	  
	}
    }

  return retval;  
}


// Reads and parses a DWARF attribute and saves any relevant data that our profiler cares about
bool DwarfReader::ReadAttribute(CompilationUnit& retval, unsigned int attribute, unsigned int type, 
			     int debug_start, const char* jump_table, int& current_addr, 
			     const std::vector<symbol*> data_table, int& dtable_ptr,
			     int top_unit_pc)
{
  int value;
  int rangePtr = -1;
  int low_pc = -1;
  int high_pc = -1;
  bool unused_attribute = false;
  int tmpAddr;
  switch(attribute)
    {
    case DW_AT_name:
      value = readWord(jump_table, current_addr);
      retval.name = std::string((const char*)&(jump_table[value]));
      break;
    case DW_AT_APPLE_optimized:
      // These appear in the abbrev table but have no corresponding item in the debug entry.
      return true;
      break;
    case DW_AT_low_pc: // low program coutner (creates a pair)
      retval.ranges.push_back(std::pair<int, int>(readWord(jump_table, current_addr), -1));
      break;
    case DW_AT_high_pc: // high PC (requires low to be read first)
      if((retval.ranges.size() < 1) || 
	 (retval.ranges[retval.ranges.size()-1].first == -1) ||
	 (retval.ranges[retval.ranges.size()-1].second != -1))
	{
	  printf("ERROR (--profile): Invalid or inverted PC ranges\n");
	  exit(1);
	}
      retval.ranges[retval.ranges.size()-1].second = readWord(jump_table, current_addr);

      // If the type of high_pc is not a raw address, then it's an offset from low_pc
      if(type != DW_FORM_addr)
	retval.ranges[retval.ranges.size()-1].second += retval.ranges[retval.ranges.size()-1].first;
      break;

    case DW_AT_ranges: // a non-contiguous range of PCs
      rangePtr = readWord(jump_table, current_addr);
      for(size_t i = 0; i < data_table.size(); i++) // find the symbol for the start of the ranges
	if(data_table[i]->address == rangePtr)
	  {
	    rangePtr = i; // repurpose as an index in to the symbol vector now.
	    break;
	  }
      if(rangePtr == -1)
	{
	  printf("ERROR (--profile): Can't find debug ranges symbol\n");
	  exit(1);
	}
      while(true)// load all range pairs
	{
	  if((unsigned)rangePtr > (data_table.size()-2))
	    {
	      printf("ERROR (--profile): invalid debug ranges (DW_AT_rages)\n");
	      exit(1);
	    }
	  // Read low pc
	  tmpAddr = data_table[rangePtr + 0]->address;
	  low_pc = readVarSize(jump_table, tmpAddr, data_table[rangePtr + 0]->size);
	  // Read high pc
	  tmpAddr = data_table[rangePtr + 1]->address;
	  high_pc = readVarSize(jump_table, tmpAddr, data_table[rangePtr + 1]->size);
	  // EOM
	  if(low_pc == 0 && high_pc == 0)
	    break;
	  low_pc += top_unit_pc; // ranges are offsets from the containing compilation unit
	  high_pc += top_unit_pc;
	  retval.ranges.push_back(std::pair<int, int>(low_pc, high_pc));
	  rangePtr+=2;
	}
      break;
    case DW_AT_specification: // These two attributes indicate the unit references another for information
    case DW_AT_abstract_origin:
      if(type == DW_FORM_ref_addr)
	retval.pointsTo = readVarType(jump_table, current_addr, type) + debug_start;
      else
	retval.pointsTo = readVarType(jump_table, current_addr, type) + retval.top_level_addr;
      break;
    default:
      // Something our profiler doesn't care about
      unused_attribute = true;
      break;
    }

  // Advance/backtrack current_addr past this attribute if it contains any extra data
  // or back if it contained no data
  switch(type)
    {
    case DW_FORM_exprloc: // Variable-sized attributes
    case DW_FORM_block:
    case DW_FORM_block1:
    case DW_FORM_block2:
    case DW_FORM_block4:
      value = readVarSize(jump_table, current_addr, data_table[dtable_ptr]->size);
      current_addr += value;
      dtable_ptr += value;
      break;
    case DW_FORM_flag_present:
      // Flags in the abbrev table have no corresponding entry in the debug unit, don't advance current_addr
      dtable_ptr--; // undo the increment below
      break;
    default:
      if(unused_attribute) // Move past the unused data
	current_addr += data_table[dtable_ptr]->size;
      break;
    }

  dtable_ptr++;

  return true;

}

// Reads the header information for a DWARF compilation unit
void DwarfReader::ReadUnitHeader(int& unit_length, const char* jump_table, int& current_addr)
{
  unit_length = readWord(jump_table, current_addr);

  uint16_t DWARF_version = readHalf(jump_table, current_addr);
  if(DWARF_version != 4 && DWARF_version != 2) // We need DWARF version 4
    {
      printf("ERROR (--profile): Debug data compiled with wrong DWARF version number (%u). Can't build profiler.\n", DWARF_version);
      exit(1);
    }

  readWord(jump_table, current_addr); // throw away abbrev offset

  if(readByte(jump_table, current_addr) != 4) // We need 32-bit addresses
    {
      printf("ERROR (--profile): Debug data contains code compiled for non-TRaX architecture (wrong address size). Can't build profiler.\n");
      exit(1);
    }
}

// Preprocess step to find the locations of each entry in the DWARF abbreviations table
void DwarfReader::FindAbbrevCodes(const char* jump_table, int abbrev_start, int jtable_size)
{
  int addr = abbrev_start;
  AbbreviationCode ac;
  while(addr < jtable_size - 2) // this loop reads 2 at a time, must stop early to be safe
    {
      ac.addr = addr;
      ac.code = readByte(jump_table, addr);

      if(ac.code == 0) // EOM marker
	return;
      
      abbrevCodes.push_back(ac);
      
      // consume the tag and has_children (so they don't count as zeros for EOM detection)
      readByte(jump_table, addr);
      readByte(jump_table, addr);

      bool read_zero = false;
      while(true)
	{
	  if(addr >= jtable_size)
	    {
	      printf("ERROR (--profile): Invalid profile data (did you compile your TRaX project with -g?)\n");
	      exit(1);
	    }
	  if(readByte(jump_table, addr) == 0)
	    {
	      if(read_zero)
		break;
	      read_zero = true;
	    }
	  else
	    read_zero = false;
	}
    }
}


// Recursively combine a compilation unit and its children in to one list (the unit_list)
void DwarfReader::AddUnitList(CompilationUnit* cu)
{
  unit_list.push_back(cu);
  for(size_t i=0; i < cu->children.size(); i++)
    AddUnitList((&cu->children[i]));
}


// Finds a function call (when the instruction's program counter is not within the previous unit)
CompilationUnit* CompilationUnit::FindFunctionCall(Instruction* ins)
{
  if(ContainsPC(ins->pc_address))
    {
      if(tag != DW_TAG_compile_unit)
	return this;
      else
	{
	  for(size_t i = 0; i < children.size(); i++)
	    {
	      CompilationUnit* cu = children[i].FindFunctionCall(ins);
	      if(cu != NULL)
		return cu;
	    }
	}
    }
  return NULL;
}


// Writes a graphviz file representing the debug source tree
void DwarfReader::WriteDot(const char* filename)
{
  
  FILE *output = fopen(filename, "w");
  if(output == NULL)
    {
      printf("Error (--profile), could not open DOT file for writing: %s\n", filename);
      exit(1);
    }

   fprintf(output, "graph SourceTree {\n");

   for(size_t i = 0; i < rootSource.children.size(); i++)
     {
       WriteDotRecursive(output, rootSource.children[i]);
     }

   fprintf(output, "}\n");
   fclose(output);
}

// Recursive helper for the above
void DwarfReader::WriteDotRecursive(FILE* output, CompilationUnit node)
{
 
  static int nodeID = -1;
  int myID = ++nodeID;
  fprintf(output, "Node%d [label=\"%s\\nt=%d\\nr=", myID, node.name.c_str(), node.tag);
  for(size_t i=0; i < node.ranges.size(); i++)
    fprintf(output, "%d-%d, ", node.ranges[i].first, node.ranges[i].second);
  fprintf(output, "\\na=%d\\nr=%d\\nbrev=%d\"];\n", node.addr, node.pointsTo, node.abbrev);
  
  for(size_t i=0; i < node.children.size(); i++)
    {
      volatile int childID = nodeID + 1;
      WriteDotRecursive(output, node.children[i]);
      fprintf(output, "Node%d -- Node%d;\n", myID, childID);
    }
}


// ----- Helpers -----
unsigned char readByte(const char* jump_table, int& addr)
{
  char retval = jump_table[addr];
  addr++;
  return retval; 
}

uint16_t readHalf(const char* jump_table, int& addr)
{
  int16_t retval = *((int16_t*)(jump_table + addr));
  addr+=2;
  return retval;
}

uint32_t readWord(const char* jump_table, int& addr)
{
  int32_t retval = *((int32_t*)(jump_table + addr));
  addr += 4;
  return retval;
}

unsigned int readVarSize(const char* jump_table, int& addr, int size)
{
  switch(size)
    {
    case 1:
      return readByte(jump_table, addr);
      break;
    case 2:
      return readHalf(jump_table, addr);
      break;
    case 4:
      return readWord(jump_table, addr);
      break;
    default:
      printf("ERROR (--profile): Debug data with invalid size: %d (should be 1, 2, or 4)\n", size);
      exit(-1);
      break;
    }
}

unsigned int readVarType(const char* jump_table, int& addr, int type)
{
  switch(type)
    {
      // 4 bytes
    case DW_FORM_addr:
    case DW_FORM_data4:
    case DW_FORM_strp:
    case DW_FORM_ref_addr:
    case DW_FORM_ref4:
      return readWord(jump_table, addr);
      break;

      // 2 bytes
    case DW_FORM_data2:
    case DW_FORM_ref2:
      return readHalf(jump_table, addr);
      break;      

      // 1 byte
    case DW_FORM_data1:
    case DW_FORM_ref1:
      return readByte(jump_table, addr);
      break;

    default:
      printf("ERROR (--profile): Debug data contains unhandled DWARF format specifier: %x\n", type);
      exit(1);
      break;
    }
}

