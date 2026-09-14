// util/Dwarf.h
//
// Source-line information, read from the .debug_line section of an ELF
// executable produced by pdp11-uknc-rt11-gcc -g. See ElfFile.h for why the
// addresses in that file can be used directly.
//
// What this gives is the two directions of the line table: the source line
// an address belongs to (for annotating the PC, a breakpoint, a trace) and
// the address a source line starts at (for setting a breakpoint by
// file:line). The table is the compiler's own, so "the address a line
// starts at" means the first machine instruction the compiler attributed to
// that line -- with optimization on, that need not be in source order, and
// a line may have no code of its own at all.

#pragma once

//////////////////////////////////////////////////////////////////////

class ElfImage;

// One function, as .debug_info describes it: the name the programmer
// wrote, and the address range the compiler gave it. The ELF symbol table
// does not carry this -- the pdp11 back end emits no .type/.size
// directives, so every symbol in it is an untyped, sizeless label and an
// address inside a function is as likely to be named after a compiler's
// own local label (LM_5) as after the function.
struct DwarfFunction
{
    std::wstring name;
    uint16_t low;
    uint16_t high;  // One past the last byte
};

// Read the functions out of .debug_info. Empty if the file has none.
void Dwarf_LoadFunctions(const ElfImage& elf, std::vector<DwarfFunction>* functions);

// Read the line table out of an already-loaded ELF image, replacing
// whatever was loaded before. Returns the number of line-table rows, or 0
// if the file carries no line information (not an error -- a program built
// without -g simply has none), with the reason in `error`.
size_t Dwarf_LoadLines(const ElfImage& elf, std::wstring* error);

// Forget everything loaded, so that a failed or symbol-only load doesn't
// leave the previous program's lines behind.
void Dwarf_Unload();

bool Dwarf_IsLoaded();

// The source line `address` belongs to: the last line-table row at or
// before it, unless that row ends a sequence (the address is past the end
// of the code the table covers). `file` is the name as the compiler wrote
// it, without directories.
bool Dwarf_LineForAddress(uint16_t address, std::wstring* file, int* line);

// "file.c:42" for `address`, empty if it has no line.
std::wstring Dwarf_FormatLocation(uint16_t address);

// " file.c:42", space-prefixed for appending after an address, or empty.
std::wstring Dwarf_FormatLocationSuffix(uint16_t address);

// The address code for `file`:`line` starts at. `file` matches on the file
// name's last component, case-sensitively, so "vfprintf.c" is enough. When
// the line itself generated no code, the next line that did is used and
// `actualLine` says which -- callers should report it when it differs.
bool Dwarf_AddressForLine(const std::wstring& file, int line,
                          uint16_t* address, int* actualLine);

// Where the source of `file` lives, as the compiler recorded it: the
// directory from the line table's own directory list joined to the name.
// Returns false if no loaded file matches.
bool Dwarf_FullPathForFile(const std::wstring& file, std::wstring* path);

// Every source file the line table names, in the order first seen.
void Dwarf_ListFiles(std::vector<std::wstring>* files);
