// util/ElfFile.h
//
// A read-only ELF32 reader, just large enough to get at an executable's
// symbol table and its DWARF sections.
//
// The RT-11 SAV image a program actually runs from is a raw memory image
// with no room for either, but the linker produces an ELF on the way to it
// (pdp11-uknc-rt11-gcc -Wl,-m,pdp11rt11), and the addresses in that ELF are
// the addresses the program runs at: RT-11 loads a SAV at 001000 absolute,
// which is where the linker script puts .text. So this file can be read
// out-of-band and its addresses used directly, with no relocation.
//
// Only what is needed is implemented: 32-bit, little-endian, no relocation
// processing, no program headers. The whole file is held in memory, which
// for these (a few hundred KB) is not worth avoiding.

#pragma once

//////////////////////////////////////////////////////////////////////

// One entry of .symtab worth keeping: defined, in range, and named.
struct ElfSymbol
{
    uint16_t    address;
    uint16_t    size;       // 0 when the producer gave none
    bool        isFunction;
    std::wstring name;      // As written in the file, leading '_' and all
};

class ElfImage
{
public:
    // Reads and validates the file. On failure returns false and puts a
    // one-line reason in `error`.
    bool Load(const std::wstring& filename, std::wstring* error);

    bool IsLoaded() const { return !m_data.empty(); }

    // Section contents by name, or nullptr if there is no such section.
    // The pointer is into this object and lives as long as it does.
    const uint8_t* Section(const char* name, size_t* size) const;

    const std::vector<ElfSymbol>& Symbols() const { return m_symbols; }

    uint16_t EntryPoint() const { return m_entry; }

    // One past the highest address any loaded section occupies. Addresses
    // above this are not part of the program at all -- they are RT-11, the
    // stack, or the machine's own I/O page -- so nothing in the symbol
    // table describes them.
    uint16_t ImageEnd() const { return m_imageEnd; }

private:
    struct SectionInfo
    {
        std::string name;
        uint32_t    offset;
        uint32_t    size;
        uint32_t    type;
        uint32_t    flags;
        uint32_t    link;
        uint32_t    entsize;
    };

    bool ReadSections(std::wstring* error);
    void ReadSymbols();

    uint8_t  Byte(size_t offset) const { return m_data[offset]; }
    uint16_t Half(size_t offset) const;
    uint32_t Word(size_t offset) const;
    bool     InRange(size_t offset, size_t length) const;

    std::vector<uint8_t> m_data;
    std::vector<SectionInfo> m_sections;
    std::vector<ElfSymbol> m_symbols;
    uint16_t m_entry = 0;
    uint16_t m_imageEnd = 0;
};
