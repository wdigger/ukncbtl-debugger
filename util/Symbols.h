// util/Symbols.h
//
// Address -> symbol name lookup, loaded from a GNU ld "-Map" file -- the
// same linker map this project's own example Makefiles already produce via
// -Wl,-Map=out.map, no toolchain/object-format change required. The flat
// RT-11 SAV image has no room for a symbol table (it's a raw memory image,
// header + bitmap, nothing else), but the *linker* already emits one as
// ordinary text in its map file, so that's where this reads from.

#pragma once

class ElfImage;

// Load symbols from a linker map file, replacing any previously loaded
// table. Returns the number of symbols loaded, or 0 on failure (file not
// found, or no symbol-looking lines in it).
size_t Symbols_LoadFromMapFile(const std::wstring& filename);

// Load symbols from an already-read ELF executable's .symtab, replacing any
// previously loaded table. Returns the number of symbols loaded. Unlike the
// map file this also carries each symbol's size, so an address in the gap
// after one symbol's end is reported as having no symbol rather than as a
// large offset into the one before it.
size_t Symbols_LoadFromElfImage(const ElfImage& elf);

struct DwarfFunction;

// Give the symbols at the start of each of these functions the function's
// own extent and name. Call after Symbols_LoadFromElfImage, with what
// Dwarf_LoadFunctions found. Returns how many symbols it covered.
//
// The pdp11 back end emits no .type/.size, so nothing in .symtab says how
// far a function reaches, and the compiler's own labels inside one (LM_5,
// LVL_3) are ordinary symbols indistinguishable from it. Without extents
// an address in the middle of a function is named after whichever of those
// labels is nearest; with them the function wins.
size_t Symbols_ApplyFunctionExtents(const std::vector<DwarfFunction>& functions);

// True if any symbols are currently loaded.
bool Symbols_IsLoaded();

// Look up the symbol at or immediately below `address`. Returns true and
// fills `name`/`offset` (address - symbol's address; 0 for an exact match)
// if one exists. Returns false if no symbols are loaded, or `address` is
// below the lowest loaded symbol.
bool Symbols_Find(uint16_t address, std::wstring* name, uint16_t* offset);

// Look up a symbol by exact name (case-sensitive, matching the linker's own
// spelling). Returns true and fills `address` on success.
bool Symbols_FindByName(const std::wstring& name, uint16_t* address);

// "name" or "name+NNNNNN" (octal offset) for `address` -- a convenience
// wrapper around Symbols_Find. Empty string if no symbol covers it.
std::wstring Symbols_Format(uint16_t address);

// " <name>" / " <name+NNNNNN>", already bracketed and space-prefixed for
// appending directly after a printed address -- empty string (not " <>")
// if there's no match, so callers can append it unconditionally.
std::wstring Symbols_FormatSuffix(uint16_t address);

// Print the whole loaded symbol table, address-sorted (the "symbols" command).
void Symbols_PrintAll();
