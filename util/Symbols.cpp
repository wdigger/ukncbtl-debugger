// util/Symbols.cpp

#include "stdafx.h"
#include <algorithm>
#include <cwctype>
#include <sstream>
#include "ElfFile.h"
#include "Dwarf.h"
#include "Symbols.h"

//////////////////////////////////////////////////////////////////////

namespace {

struct Symbol
{
    uint16_t address;
    uint16_t size;      // 0 when the source of the symbols gave none
    std::wstring name;
};

std::vector<Symbol> g_symbols;  // Kept sorted by address ascending

// One past the last address the loaded program occupies, when that is
// known (an ELF says so; a linker map does not). Zero means "unknown", and
// then any address at or above the lowest symbol gets a name, which is the
// behaviour this debugger has always had for map files.
uint16_t g_imageEnd = 0;

// Parse one line of a GNU ld map file. A pure symbol-definition line looks
// like (leading whitespace, then "0x" + hex address, then whitespace, then
// the symbol name and nothing else):
//
//                 0x0000040e                main
//
// Section-content lines have extra fields after the name (a size, and/or a
// source file/archive-member path) and don't match, e.g.:
//
//  .text          0x000002f0      0x4f8 spigot.o
//
// nor do "LOAD file.o" lines, blank lines, or the few one-off assignment
// lines like ". = 0x200" (no valid identifier follows the address there).
bool ParseSymbolLine(const std::wstring& line, uint16_t* address, std::wstring* name)
{
    size_t pos = line.find_first_not_of(L" \t");
    if (pos == std::wstring::npos || line.compare(pos, 2, L"0x") != 0)
        return false;

    size_t hexStart = pos + 2;
    size_t hexEnd = hexStart;
    while (hexEnd < line.size() && iswxdigit(line[hexEnd]))
        hexEnd++;
    if (hexEnd == hexStart)
        return false;

    size_t nameStart = line.find_first_not_of(L" \t", hexEnd);
    if (nameStart == std::wstring::npos)
        return false;
    size_t nameEnd = line.find_first_of(L" \t", nameStart);
    std::wstring token = (nameEnd == std::wstring::npos)
        ? line.substr(nameStart)
        : line.substr(nameStart, nameEnd - nameStart);
    if (token.empty())
        return false;

    // A pure symbol-definition line has nothing else after the name.
    if (nameEnd != std::wstring::npos && line.find_first_not_of(L" \t", nameEnd) != std::wstring::npos)
        return false;

    // Must look like a C identifier/assembler symbol, not e.g. a bare "="
    // (from ". = 0x200"-style assignment lines).
    if (!iswalpha(token[0]) && token[0] != L'_' && token[0] != L'.' && token[0] != L'$')
        return false;
    for (wchar_t ch : token)
        if (!iswalnum(ch) && ch != L'_' && ch != L'.' && ch != L'$')
            return false;

    uint32_t value = 0;
    for (size_t i = hexStart; i < hexEnd; i++)
    {
        wchar_t ch = line[i];
        value <<= 4;
        if (ch >= L'0' && ch <= L'9') value |= (uint32_t)(ch - L'0');
        else if (ch >= L'a' && ch <= L'f') value |= (uint32_t)(ch - L'a' + 10);
        else if (ch >= L'A' && ch <= L'F') value |= (uint32_t)(ch - L'A' + 10);
    }
    if (value > 0xffff)
        return false;  // Out of PDP-11 address range -- not a real code/data symbol

    *address = (uint16_t)value;
    *name = token;
    return true;
}

}  // namespace

//////////////////////////////////////////////////////////////////////

size_t Symbols_LoadFromMapFile(const std::wstring& filename)
{
    std::string narrowName = WStringToNarrowString(filename);
    std::wifstream file(narrowName);
    if (!file.is_open())
        return 0;

    std::vector<Symbol> loaded;
    std::wstring line;
    while (std::getline(file, line))
    {
        uint16_t address;
        std::wstring name;
        if (ParseSymbolLine(line, &address, &name))
            loaded.push_back(Symbol{ address, 0, name });
    }

    if (loaded.empty())
        return 0;

    std::sort(loaded.begin(), loaded.end(),
        [](const Symbol& a, const Symbol& b) { return a.address < b.address; });

    g_symbols = std::move(loaded);
    g_imageEnd = 0;  // A map file says nothing about where the image ends
    return g_symbols.size();
}

size_t Symbols_LoadFromElfImage(const ElfImage& elf)
{
    std::vector<Symbol> loaded;
    for (const ElfSymbol& sym : elf.Symbols())
        loaded.push_back(Symbol{ sym.address, sym.size, sym.name });

    if (loaded.empty())
        return 0;

    std::sort(loaded.begin(), loaded.end(),
        [](const Symbol& a, const Symbol& b) { return a.address < b.address; });

    g_symbols = std::move(loaded);
    g_imageEnd = elf.ImageEnd();
    return g_symbols.size();
}

size_t Symbols_ApplyFunctionExtents(const std::vector<DwarfFunction>& functions)
{
    size_t applied = 0;
    for (const DwarfFunction& fn : functions)
    {
        if (fn.high <= fn.low)
            continue;

        // Every symbol at the function's entry address: the function's own
        // symbol is there, and so, often, is a label the compiler put on
        // the same instruction. Prefer the one already spelled the same,
        // and otherwise take the first -- either way the address now has a
        // symbol that says how far the function goes.
        auto it = std::lower_bound(g_symbols.begin(), g_symbols.end(), fn.low,
            [](const Symbol& s, uint16_t addr) { return s.address < addr; });

        Symbol* chosen = nullptr;
        for (auto scan = it; scan != g_symbols.end() && scan->address == fn.low; ++scan)
        {
            if (chosen == nullptr)
                chosen = &*scan;
            if (scan->name == fn.name)
            {
                chosen = &*scan;
                break;
            }
        }

        if (chosen == nullptr)
            continue;  // No symbol there at all; a stripped or partial table
        chosen->name = fn.name;
        chosen->size = (uint16_t)(fn.high - fn.low);
        applied++;
    }
    return applied;
}

bool Symbols_IsLoaded()
{
    return !g_symbols.empty();
}

bool Symbols_Find(uint16_t address, std::wstring* name, uint16_t* offset)
{
    if (g_symbols.empty())
        return false;
    if (g_imageEnd != 0 && address >= g_imageEnd)
        return false;  // Past the program: RT-11, the stack, or the I/O page

    // Largest address <= given address: upper_bound (first entry strictly
    // greater), then step back one.
    auto it = std::upper_bound(g_symbols.begin(), g_symbols.end(), address,
        [](uint16_t addr, const Symbol& s) { return addr < s.address; });
    if (it == g_symbols.begin())
        return false;  // address is below the lowest known symbol
    --it;

    // The nearest symbol below an address is often an assembler-generated
    // label inside a function (a loop head, a jump target) rather than the
    // function itself, and "L_3" says much less than "sum+6". A symbol that
    // was given a size says where it ends, so when one of those covers the
    // address it wins over any unsized label that happens to sit closer.
    if (it->size == 0)
    {
        for (auto back = it; back != g_symbols.begin(); )
        {
            --back;
            if (back->size == 0)
                continue;
            if (address - back->address < back->size)
                it = back;
            break;
        }
    }

    uint16_t offsetInSymbol = (uint16_t)(address - it->address);
    // A sized symbol says where it ends; an address past that belongs to
    // nothing (padding, or a section this table doesn't cover) and naming
    // it "something+2000" would be worse than saying nothing.
    if (it->size != 0 && offsetInSymbol >= it->size)
        return false;

    *name = it->name;
    *offset = offsetInSymbol;
    return true;
}

bool Symbols_FindByName(const std::wstring& name, uint16_t* address)
{
    for (const Symbol& s : g_symbols)
    {
        if (s.name == name)
        {
            *address = s.address;
            return true;
        }
    }
    return false;
}

std::wstring Symbols_Format(uint16_t address)
{
    std::wstring name;
    uint16_t offset;
    if (!Symbols_Find(address, &name, &offset))
        return std::wstring();
    if (offset == 0)
        return name;

    TCHAR bufOffset[7];
    PrintOctalValue(bufOffset, offset);
    std::wostringstream oss;
    oss << name << L"+" << bufOffset;
    return oss.str();
}

std::wstring Symbols_FormatSuffix(uint16_t address)
{
    std::wstring formatted = Symbols_Format(address);
    if (formatted.empty())
        return std::wstring();

    std::wostringstream oss;
    oss << L" <" << formatted << L">";
    return oss.str();
}

void Symbols_PrintAll()
{
    if (g_symbols.empty())
    {
        std::wcout << L" No symbols loaded." << std::endl;
        return;
    }
    for (const Symbol& s : g_symbols)
    {
        TCHAR bufAddr[7];
        PrintOctalValue(bufAddr, s.address);
        std::wcout << L"  " << bufAddr << L"  " << s.name << std::endl;
    }
}
