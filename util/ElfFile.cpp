// util/ElfFile.cpp

#include "stdafx.h"
#include <algorithm>
#include "ElfFile.h"

//////////////////////////////////////////////////////////////////////

namespace {

// ELF32 header field offsets, and the few constants worth naming.
const size_t EI_NIDENT      = 16;
const size_t EHDR_SIZE      = 52;
const size_t EHDR_TYPE      = 16;
const size_t EHDR_MACHINE   = 18;
const size_t EHDR_ENTRY     = 24;
const size_t EHDR_SHOFF     = 32;
const size_t EHDR_SHENTSIZE = 46;
const size_t EHDR_SHNUM     = 48;
const size_t EHDR_SHSTRNDX  = 50;

const size_t SHDR_NAME    = 0;
const size_t SHDR_TYPE    = 4;
const size_t SHDR_FLAGS   = 8;
const size_t SHDR_ADDR    = 12;
const size_t SHDR_OFFSET  = 16;
const size_t SHDR_SIZE    = 20;
const size_t SHDR_LINK    = 24;
const size_t SHDR_ENTSIZE = 36;

const size_t SYM_SIZE   = 16;
const size_t SYM_NAME   = 0;
const size_t SYM_VALUE  = 4;
const size_t SYM_SSIZE  = 8;
const size_t SYM_INFO   = 12;
const size_t SYM_SHNDX  = 14;

const uint32_t SHT_SYMTAB = 2;
const uint32_t SHT_NOBITS = 8;

const uint32_t SHF_ALLOC = 0x2;

const uint16_t SHN_UNDEF = 0;
const uint16_t SHN_ABS   = 0xfff1;

const uint8_t STT_OBJECT = 1;
const uint8_t STT_FUNC   = 2;
const uint8_t STT_FILE   = 4;

const uint16_t EM_PDP11 = 65;

std::wstring Widen(const std::string& s)
{
    return std::wstring(s.begin(), s.end());
}

}  // namespace

//////////////////////////////////////////////////////////////////////

uint16_t ElfImage::Half(size_t offset) const
{
    return (uint16_t)(m_data[offset] | (m_data[offset + 1] << 8));
}

uint32_t ElfImage::Word(size_t offset) const
{
    return (uint32_t)m_data[offset]
        | ((uint32_t)m_data[offset + 1] << 8)
        | ((uint32_t)m_data[offset + 2] << 16)
        | ((uint32_t)m_data[offset + 3] << 24);
}

bool ElfImage::InRange(size_t offset, size_t length) const
{
    return offset <= m_data.size() && length <= m_data.size() - offset;
}

bool ElfImage::Load(const std::wstring& filename, std::wstring* error)
{
    m_data.clear();
    m_sections.clear();
    m_symbols.clear();
    m_entry = 0;
    m_imageEnd = 0;

    std::ifstream file(WStringToNarrowString(filename), std::ios::binary);
    if (!file.is_open())
    {
        *error = L"cannot open the file";
        return false;
    }

    m_data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (m_data.size() < EHDR_SIZE)
    {
        *error = L"too short to be an ELF file";
        m_data.clear();
        return false;
    }

    if (m_data[0] != 0x7f || m_data[1] != 'E' || m_data[2] != 'L' || m_data[3] != 'F')
    {
        *error = L"not an ELF file";
        m_data.clear();
        return false;
    }
    if (m_data[4] != 1)  // ELFCLASS32
    {
        *error = L"not a 32-bit ELF file";
        m_data.clear();
        return false;
    }
    if (m_data[5] != 1)  // ELFDATA2LSB
    {
        *error = L"not a little-endian ELF file";
        m_data.clear();
        return false;
    }
    if (Half(EHDR_MACHINE) != EM_PDP11)
    {
        *error = L"not a PDP-11 ELF file";
        m_data.clear();
        return false;
    }

    m_entry = (uint16_t)Word(EHDR_ENTRY);

    if (!ReadSections(error))
    {
        m_data.clear();
        return false;
    }
    ReadSymbols();
    return true;
}

bool ElfImage::ReadSections(std::wstring* error)
{
    uint32_t shoff = Word(EHDR_SHOFF);
    uint16_t shentsize = Half(EHDR_SHENTSIZE);
    uint16_t shnum = Half(EHDR_SHNUM);
    uint16_t shstrndx = Half(EHDR_SHSTRNDX);

    if (shoff == 0 || shnum == 0 || shentsize < 40)
    {
        *error = L"the file has no section table";
        return false;
    }
    if (!InRange(shoff, (size_t)shnum * shentsize))
    {
        *error = L"the section table runs past the end of the file";
        return false;
    }
    if (shstrndx >= shnum)
    {
        *error = L"the section name table index is out of range";
        return false;
    }

    // The section name strings, needed before the names can be read.
    size_t strOff = Word(shoff + (size_t)shstrndx * shentsize + SHDR_OFFSET);
    size_t strSize = Word(shoff + (size_t)shstrndx * shentsize + SHDR_SIZE);
    if (!InRange(strOff, strSize))
    {
        *error = L"the section name table runs past the end of the file";
        return false;
    }

    m_sections.reserve(shnum);
    for (uint16_t i = 0; i < shnum; i++)
    {
        size_t base = shoff + (size_t)i * shentsize;
        SectionInfo s;
        s.type = Word(base + SHDR_TYPE);
        s.flags = Word(base + SHDR_FLAGS);
        s.offset = Word(base + SHDR_OFFSET);
        s.size = Word(base + SHDR_SIZE);
        s.link = Word(base + SHDR_LINK);
        s.entsize = Word(base + SHDR_ENTSIZE);

        uint32_t nameOff = Word(base + SHDR_NAME);
        if (nameOff < strSize)
        {
            const char* p = (const char*)m_data.data() + strOff + nameOff;
            size_t maxLen = strSize - nameOff;
            s.name.assign(p, strnlen(p, maxLen));
        }

        // A NOBITS section (.bss) occupies no file space; everything else
        // must actually be there before anyone reads it.
        if (s.type != SHT_NOBITS && !InRange(s.offset, s.size))
        {
            s.offset = 0;
            s.size = 0;
        }
        if ((s.flags & SHF_ALLOC) != 0)
        {
            uint32_t sectionAddr = Word(base + SHDR_ADDR);
            uint32_t end = sectionAddr + s.size;
            if (end <= 0x10000 && end > m_imageEnd)
                m_imageEnd = (uint16_t)end;
        }

        m_sections.push_back(std::move(s));
    }
    return true;
}

void ElfImage::ReadSymbols()
{
    for (const SectionInfo& sec : m_sections)
    {
        if (sec.type != SHT_SYMTAB || sec.entsize < SYM_SIZE)
            continue;
        if (sec.link >= m_sections.size())
            continue;

        const SectionInfo& strtab = m_sections[sec.link];
        size_t count = sec.size / sec.entsize;
        for (size_t i = 0; i < count; i++)
        {
            size_t base = sec.offset + i * sec.entsize;
            uint16_t shndx = Half(base + SYM_SHNDX);
            if (shndx == SHN_UNDEF)
                continue;  // Undefined: nothing to point at

            uint8_t type = (uint8_t)(Byte(base + SYM_INFO) & 0x0f);
            if (type == STT_FILE)
                continue;  // A source file name, not an address

            uint32_t value = Word(base + SYM_VALUE);
            if (value > 0xffff)
                continue;  // Outside the PDP-11's address space
            if (shndx == SHN_ABS)
            {
                if (type != STT_FUNC && type != STT_OBJECT)
                    continue;  // Assembler absolutes, not places in memory
            }
            else
            {
                // Only symbols in a section the loader actually places have
                // a run-time address. The debug sections carry labels of
                // their own (Ldebug_info_0 and friends), all at offset zero
                // in a section that is never loaded, and taking those for
                // addresses would put a symbol name on every low address in
                // the machine.
                if (shndx >= m_sections.size())
                    continue;
                if ((m_sections[shndx].flags & SHF_ALLOC) == 0)
                    continue;
            }

            uint32_t nameOff = Word(base + SYM_NAME);
            if (nameOff == 0 || nameOff >= strtab.size)
                continue;
            const char* p = (const char*)m_data.data() + strtab.offset + nameOff;
            std::string name(p, strnlen(p, strtab.size - nameOff));
            if (name.empty())
                continue;

            // The linker's own map file, which this debugger has always
            // read, prints C names -- so strip the one underscore the
            // target prefixes them with, and `b main` means the same thing
            // whichever file the symbols came from.
            if (name[0] == '_')
                name.erase(0, 1);

            ElfSymbol sym;
            sym.address = (uint16_t)value;
            uint32_t size = Word(base + SYM_SSIZE);
            sym.size = (size > 0xffff) ? 0 : (uint16_t)size;
            sym.isFunction = (type == STT_FUNC);
            sym.name = Widen(name);
            m_symbols.push_back(std::move(sym));
        }
    }

    std::sort(m_symbols.begin(), m_symbols.end(),
        [](const ElfSymbol& a, const ElfSymbol& b) { return a.address < b.address; });
}

const uint8_t* ElfImage::Section(const char* name, size_t* size) const
{
    for (const SectionInfo& s : m_sections)
    {
        if (s.name != name || s.size == 0 || s.type == SHT_NOBITS)
            continue;
        *size = s.size;
        return m_data.data() + s.offset;
    }
    *size = 0;
    return nullptr;
}
