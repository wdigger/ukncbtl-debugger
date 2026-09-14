// util/Dwarf.cpp

#include "stdafx.h"
#include <algorithm>
#include <sstream>
#include "ElfFile.h"
#include "Dwarf.h"

//////////////////////////////////////////////////////////////////////

namespace {

// --- DWARF constants, only the ones this reader acts on --------------

const uint8_t DW_LNS_copy               = 1;
const uint8_t DW_LNS_advance_pc         = 2;
const uint8_t DW_LNS_advance_line       = 3;
const uint8_t DW_LNS_set_file           = 4;
const uint8_t DW_LNS_set_column         = 5;
const uint8_t DW_LNS_negate_stmt        = 6;
const uint8_t DW_LNS_set_basic_block    = 7;
const uint8_t DW_LNS_const_add_pc       = 8;
const uint8_t DW_LNS_fixed_advance_pc   = 9;

const uint8_t DW_LNE_end_sequence   = 1;
const uint8_t DW_LNE_set_address    = 2;

const uint64_t DW_LNCT_path             = 1;
const uint64_t DW_LNCT_directory_index  = 2;

const uint64_t DW_FORM_block2       = 0x03;
const uint64_t DW_FORM_block4       = 0x04;
const uint64_t DW_FORM_data2        = 0x05;
const uint64_t DW_FORM_data4        = 0x06;
const uint64_t DW_FORM_data8        = 0x07;
const uint64_t DW_FORM_string       = 0x08;
const uint64_t DW_FORM_block        = 0x09;
const uint64_t DW_FORM_block1       = 0x0a;
const uint64_t DW_FORM_data1        = 0x0b;
const uint64_t DW_FORM_flag         = 0x0c;
const uint64_t DW_FORM_sdata        = 0x0d;
const uint64_t DW_FORM_strp         = 0x0e;
const uint64_t DW_FORM_udata        = 0x0f;
const uint64_t DW_FORM_data16       = 0x1e;
const uint64_t DW_FORM_line_strp    = 0x1f;

//////////////////////////////////////////////////////////////////////

// A bounds-checked cursor over a section. Every read past the end sets
// `bad` and returns zero, so a truncated or malformed section stops the
// parse instead of walking off the buffer; callers check `bad` once per
// structure rather than once per field.
struct Reader
{
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
    bool bad = false;

    Reader() = default;
    Reader(const uint8_t* d, size_t s) : data(d), size(s) {}

    bool AtEnd() const { return pos >= size; }
    size_t Left() const { return (pos < size) ? size - pos : 0; }

    uint8_t U8()
    {
        if (pos + 1 > size) { bad = true; return 0; }
        return data[pos++];
    }
    uint16_t U16()
    {
        if (pos + 2 > size) { bad = true; pos = size; return 0; }
        uint16_t v = (uint16_t)(data[pos] | (data[pos + 1] << 8));
        pos += 2;
        return v;
    }
    uint32_t U32()
    {
        if (pos + 4 > size) { bad = true; pos = size; return 0; }
        uint32_t v = (uint32_t)data[pos]
            | ((uint32_t)data[pos + 1] << 8)
            | ((uint32_t)data[pos + 2] << 16)
            | ((uint32_t)data[pos + 3] << 24);
        pos += 4;
        return v;
    }
    uint64_t U64()
    {
        uint64_t lo = U32(), hi = U32();
        return lo | (hi << 32);
    }
    uint64_t ULEB()
    {
        uint64_t result = 0;
        unsigned shift = 0;
        for (;;)
        {
            uint8_t b = U8();
            if (bad)
                return 0;
            if (shift < 64)
                result |= (uint64_t)(b & 0x7f) << shift;
            shift += 7;
            if ((b & 0x80) == 0)
                break;
        }
        return result;
    }
    int64_t SLEB()
    {
        int64_t result = 0;
        unsigned shift = 0;
        uint8_t b = 0;
        for (;;)
        {
            b = U8();
            if (bad)
                return 0;
            if (shift < 64)
                result |= (int64_t)(b & 0x7f) << shift;
            shift += 7;
            if ((b & 0x80) == 0)
                break;
        }
        if (shift < 64 && (b & 0x40))
            result |= -((int64_t)1 << shift);
        return result;
    }
    // An address of `length` bytes; the PDP-11's are two bytes wide but
    // DWARF records them in four (see DWARF2_ADDR_SIZE in the gcc port).
    uint64_t Address(unsigned length)
    {
        uint64_t v = 0;
        for (unsigned i = 0; i < length; i++)
        {
            uint8_t b = U8();
            if (i < 8)
                v |= (uint64_t)b << (i * 8);
        }
        return v;
    }
    std::string String()
    {
        std::string s;
        for (;;)
        {
            uint8_t b = U8();
            if (bad || b == 0)
                break;
            s.push_back((char)b);
        }
        return s;
    }
    void Skip(size_t count)
    {
        if (count > Left()) { bad = true; pos = size; return; }
        pos += count;
    }
};

std::string StringAt(const uint8_t* section, size_t sectionSize, uint64_t offset)
{
    if (section == nullptr || offset >= sectionSize)
        return std::string();
    const char* p = (const char*)section + offset;
    return std::string(p, strnlen(p, sectionSize - (size_t)offset));
}

std::wstring Widen(const std::string& s)
{
    return std::wstring(s.begin(), s.end());
}

// The last path component of `path`, for matching a user-typed file name
// against a compiler-recorded one.
std::wstring BaseName(const std::wstring& path)
{
    size_t pos = path.find_last_of(L"/\\");
    return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

bool IsAbsolute(const std::wstring& path)
{
    if (path.empty())
        return false;
    if (path[0] == L'/' || path[0] == L'\\')
        return true;
    return path.size() > 2 && path[1] == L':';  // "C:\..."
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name)
{
    if (dir.empty() || IsAbsolute(name))
        return name;
    if (dir.back() == L'/' || dir.back() == L'\\')
        return dir + name;
    return dir + L'/' + name;
}

//////////////////////////////////////////////////////////////////////

struct SourceFile
{
    std::wstring name;  // As the compiler wrote it, directories and all
    std::wstring path;  // Joined with its directory, absolute where known
};

struct LineRow
{
    uint16_t address;
    uint32_t file;      // Index into g_files
    int32_t  line;
    bool     endSequence;
};

std::vector<SourceFile> g_files;
std::vector<LineRow> g_rows;   // Sorted by address; see the comparator below

// Files are shared between compilation units -- every unit that includes a
// header lists it again -- so they are pooled by full path.
uint32_t InternFile(const std::wstring& name, const std::wstring& path)
{
    for (size_t i = 0; i < g_files.size(); i++)
    {
        // Keyed on the joined path alone: a file reached from two units is
        // often spelled differently in each (a bare name in one, a full
        // path in the other) and is still the same file.
        if (g_files[i].path == path)
            return (uint32_t)i;
    }
    g_files.push_back(SourceFile{ name, path });
    return (uint32_t)(g_files.size() - 1);
}

// One value of one of the forms a line-table header entry may use. Only
// the two content types this reader wants are decoded; the rest just have
// to be skipped over correctly, which is why every form is listed.
bool ReadFormValue(Reader& r, uint64_t form,
                   const uint8_t* strSection, size_t strSize,
                   const uint8_t* lineStrSection, size_t lineStrSize,
                   std::string* str, uint64_t* num)
{
    switch (form)
    {
    case DW_FORM_string:    *str = r.String(); return true;
    case DW_FORM_strp:      *str = StringAt(strSection, strSize, r.U32()); return true;
    case DW_FORM_line_strp: *str = StringAt(lineStrSection, lineStrSize, r.U32()); return true;
    case DW_FORM_udata:     *num = r.ULEB(); return true;
    case DW_FORM_sdata:     *num = (uint64_t)r.SLEB(); return true;
    case DW_FORM_data1:
    case DW_FORM_flag:      *num = r.U8(); return true;
    case DW_FORM_data2:     *num = r.U16(); return true;
    case DW_FORM_data4:     *num = r.U32(); return true;
    case DW_FORM_data8:     *num = r.U64(); return true;
    case DW_FORM_data16:    r.Skip(16); return true;   // An MD5 hash
    case DW_FORM_block1:    r.Skip(r.U8()); return true;
    case DW_FORM_block2:    r.Skip(r.U16()); return true;
    case DW_FORM_block4:    r.Skip(r.U32()); return true;
    case DW_FORM_block:     r.Skip((size_t)r.ULEB()); return true;
    default:
        // Anything else (the .debug_str_offsets-based strx forms above all)
        // cannot be skipped without knowing its size, so the unit is lost.
        return false;
    }
}

// The directory and file lists of one line-table header, in whichever of
// the two layouts the version calls for. Fills `fileMap`, which turns the
// unit's own file numbers into indices into g_files.
bool ReadFileTable(Reader& r, int version,
                   const uint8_t* strSection, size_t strSize,
                   const uint8_t* lineStrSection, size_t lineStrSize,
                   std::vector<uint32_t>* fileMap)
{
    std::vector<std::wstring> dirs;

    if (version >= 5)
    {
        // Both lists are described by a format: a sequence of (content
        // type, form) pairs that each entry then follows.
        uint8_t dirFormatCount = r.U8();
        std::vector<std::pair<uint64_t, uint64_t>> dirFormat;
        for (uint8_t i = 0; i < dirFormatCount; i++)
        {
            uint64_t type = r.ULEB();
            uint64_t form = r.ULEB();
            dirFormat.push_back({ type, form });
        }
        uint64_t dirCount = r.ULEB();
        for (uint64_t i = 0; i < dirCount && !r.bad; i++)
        {
            std::wstring path;
            for (const auto& f : dirFormat)
            {
                std::string str;
                uint64_t num = 0;
                if (!ReadFormValue(r, f.second, strSection, strSize,
                                   lineStrSection, lineStrSize, &str, &num))
                    return false;
                if (f.first == DW_LNCT_path)
                    path = Widen(str);
            }
            dirs.push_back(path);
        }

        uint8_t fileFormatCount = r.U8();
        std::vector<std::pair<uint64_t, uint64_t>> fileFormat;
        for (uint8_t i = 0; i < fileFormatCount; i++)
        {
            uint64_t type = r.ULEB();
            uint64_t form = r.ULEB();
            fileFormat.push_back({ type, form });
        }
        uint64_t fileCount = r.ULEB();
        for (uint64_t i = 0; i < fileCount && !r.bad; i++)
        {
            std::wstring name;
            uint64_t dirIndex = 0;
            for (const auto& f : fileFormat)
            {
                std::string str;
                uint64_t num = 0;
                if (!ReadFormValue(r, f.second, strSection, strSize,
                                   lineStrSection, lineStrSize, &str, &num))
                    return false;
                if (f.first == DW_LNCT_path)
                    name = Widen(str);
                else if (f.first == DW_LNCT_directory_index)
                    dirIndex = num;
            }
            std::wstring dir = (dirIndex < dirs.size()) ? dirs[dirIndex] : std::wstring();
            std::wstring path = JoinPath(dir, name);
            // A relative directory is relative to the compilation
            // directory, which in this layout is directory 0.
            if (!IsAbsolute(path) && !dirs.empty())
                path = JoinPath(dirs[0], path);
            fileMap->push_back(InternFile(name, path));
        }
        // Version 5 numbers files from zero, so the map is already aligned.
    }
    else
    {
        // Versions 2 to 4: two NUL-terminated lists, each ended by an empty
        // entry, and file numbers start at one.
        dirs.push_back(std::wstring());  // Index 0 is the compilation directory
        for (;;)
        {
            std::string dir = r.String();
            if (r.bad || dir.empty())
                break;
            dirs.push_back(Widen(dir));
        }
        fileMap->push_back(0);  // File 0 is unused before version 5
        for (;;)
        {
            std::string name = r.String();
            if (r.bad || name.empty())
                break;
            uint64_t dirIndex = r.ULEB();
            r.ULEB();  // Last modification time
            r.ULEB();  // Length in bytes
            std::wstring dir = (dirIndex < dirs.size()) ? dirs[dirIndex] : std::wstring();
            std::wstring wname = Widen(name);
            fileMap->push_back(InternFile(wname, JoinPath(dir, wname)));
        }
        if (!fileMap->empty())
            fileMap->at(0) = fileMap->size() > 1 ? fileMap->at(1) : 0;
    }

    return !r.bad;
}

// Run one unit's line program, appending its rows.
bool ReadLineProgram(Reader& r, size_t unitEnd,
                     const uint8_t* strSection, size_t strSize,
                     const uint8_t* lineStrSection, size_t lineStrSize)
{
    uint16_t version = r.U16();
    if (version < 2 || version > 5)
        return false;

    unsigned addressSize = 4;
    if (version >= 5)
    {
        addressSize = r.U8();
        r.U8();  // Segment selector size
    }

    uint32_t headerLength = r.U32();
    size_t programStart = r.pos + headerLength;

    uint8_t minInstLength = r.U8();
    if (minInstLength == 0)
        return false;
    if (version >= 4)
        r.U8();  // Maximum operations per instruction (never > 1 here)
    uint8_t defaultIsStmt = r.U8();
    int8_t lineBase = (int8_t)r.U8();
    uint8_t lineRange = r.U8();
    uint8_t opcodeBase = r.U8();
    if (lineRange == 0 || opcodeBase == 0)
        return false;

    std::vector<uint8_t> standardLengths(opcodeBase > 0 ? opcodeBase - 1 : 0);
    for (size_t i = 0; i + 1 < opcodeBase; i++)
        standardLengths[i] = r.U8();

    std::vector<uint32_t> fileMap;
    if (!ReadFileTable(r, version, strSection, strSize, lineStrSection, lineStrSize, &fileMap))
        return false;

    // The header length is authoritative: it says where the program starts
    // whatever this reader made of the tables.
    if (programStart > unitEnd || programStart < r.pos)
        return false;
    r.pos = programStart;

    uint64_t address = 0;
    uint64_t file = 1;
    int64_t line = 1;
    bool endSequence = false;

    auto emit = [&]()
    {
        LineRow row;
        row.address = (uint16_t)address;
        uint32_t globalFile = (file < fileMap.size()) ? fileMap[(size_t)file] : 0;
        row.file = globalFile;
        row.line = (int32_t)line;
        row.endSequence = endSequence;
        if (address <= 0xffff && !g_files.empty())
            g_rows.push_back(row);
    };
    auto reset = [&]()
    {
        address = 0;
        file = 1;
        line = 1;
        endSequence = false;
    };

    while (r.pos < unitEnd && !r.bad)
    {
        uint8_t opcode = r.U8();
        if (opcode >= opcodeBase)
        {
            unsigned adjusted = opcode - opcodeBase;
            address += (uint64_t)minInstLength * (adjusted / lineRange);
            line += lineBase + (int)(adjusted % lineRange);
            emit();
            continue;
        }
        if (opcode == 0)
        {
            // Extended opcode: a length, then the opcode itself.
            uint64_t length = r.ULEB();
            size_t next = r.pos + (size_t)length;
            if (length == 0 || next > unitEnd)
                return false;
            uint8_t sub = r.U8();
            if (sub == DW_LNE_end_sequence)
            {
                endSequence = true;
                emit();
                reset();
            }
            else if (sub == DW_LNE_set_address)
            {
                unsigned width = (length > 1) ? (unsigned)(length - 1) : addressSize;
                address = r.Address(width);
            }
            r.pos = next;  // Whatever it was, the length says where it ends
            continue;
        }

        switch (opcode)
        {
        case DW_LNS_copy:
            emit();
            break;
        case DW_LNS_advance_pc:
            address += (uint64_t)minInstLength * r.ULEB();
            break;
        case DW_LNS_advance_line:
            line += r.SLEB();
            break;
        case DW_LNS_set_file:
            file = r.ULEB();
            break;
        case DW_LNS_set_column:
            r.ULEB();
            break;
        case DW_LNS_negate_stmt:
        case DW_LNS_set_basic_block:
            break;
        case DW_LNS_const_add_pc:
            address += (uint64_t)minInstLength * ((255 - opcodeBase) / lineRange);
            break;
        case DW_LNS_fixed_advance_pc:
            address += r.U16();
            break;
        default:
            // A standard opcode this reader doesn't know: the header said
            // how many arguments it takes, so it can still be stepped over.
            for (uint8_t i = 0; i < standardLengths[opcode - 1]; i++)
                r.ULEB();
            break;
        }
    }

    (void)defaultIsStmt;
    return !r.bad;
}

}  // namespace

//////////////////////////////////////////////////////////////////////

void Dwarf_Unload()
{
    g_files.clear();
    g_rows.clear();
}

bool Dwarf_IsLoaded()
{
    return !g_rows.empty();
}

size_t Dwarf_LoadLines(const ElfImage& elf, std::wstring* error)
{
    Dwarf_Unload();

    size_t lineSize = 0, strSize = 0, lineStrSize = 0;
    const uint8_t* lineSection = elf.Section(".debug_line", &lineSize);
    const uint8_t* strSection = elf.Section(".debug_str", &strSize);
    const uint8_t* lineStrSection = elf.Section(".debug_line_str", &lineStrSize);

    if (lineSection == nullptr)
    {
        *error = L"the file has no .debug_line section -- was it built with -g?";
        return 0;
    }

    size_t unitsRead = 0, unitsFailed = 0;
    Reader outer(lineSection, lineSize);
    while (outer.pos + 4 <= lineSize)
    {
        size_t unitStart = outer.pos;
        uint32_t unitLength = outer.U32();
        if (unitLength == 0xffffffff)
        {
            *error = L"64-bit DWARF is not supported";
            break;
        }
        if (unitLength == 0 || unitLength > lineSize - outer.pos)
            break;
        size_t unitEnd = outer.pos + unitLength;

        Reader unit(lineSection, unitEnd);
        unit.pos = outer.pos;
        if (ReadLineProgram(unit, unitEnd, strSection, strSize, lineStrSection, lineStrSize))
            unitsRead++;
        else
            unitsFailed++;

        outer.pos = unitEnd;
        if (outer.pos <= unitStart)
            break;  // No progress: a malformed section, stop rather than spin
    }

    if (g_rows.empty())
    {
        if (unitsFailed > 0 && error->empty())
            *error = L"the line table is in a form this debugger cannot read";
        return 0;
    }

    // Sorted by address, and at one address an end-of-sequence row sorts
    // before a row that starts the next one -- so a lookup that lands
    // exactly on a boundary gets the code that begins there, not the code
    // that just ended.
    std::stable_sort(g_rows.begin(), g_rows.end(),
        [](const LineRow& a, const LineRow& b)
        {
            if (a.address != b.address)
                return a.address < b.address;
            return (a.endSequence ? 1 : 0) > (b.endSequence ? 1 : 0);
        });

    return g_rows.size();
}

bool Dwarf_LineForAddress(uint16_t address, std::wstring* file, int* line)
{
    if (g_rows.empty())
        return false;

    auto it = std::upper_bound(g_rows.begin(), g_rows.end(), address,
        [](uint16_t addr, const LineRow& row) { return addr < row.address; });
    if (it == g_rows.begin())
        return false;
    --it;
    if (it->endSequence)
        return false;  // Past the end of the code this table covers

    if (it->file >= g_files.size())
        return false;
    *file = BaseName(g_files[it->file].name);
    *line = it->line;
    return true;
}

std::wstring Dwarf_FormatLocation(uint16_t address)
{
    std::wstring file;
    int line = 0;
    if (!Dwarf_LineForAddress(address, &file, &line))
        return std::wstring();

    std::wostringstream oss;
    oss << file << L":" << line;
    return oss.str();
}

std::wstring Dwarf_FormatLocationSuffix(uint16_t address)
{
    std::wstring formatted = Dwarf_FormatLocation(address);
    if (formatted.empty())
        return std::wstring();
    return L" " + formatted;
}

bool Dwarf_AddressForLine(const std::wstring& file, int line,
                          uint16_t* address, int* actualLine)
{
    if (g_rows.empty())
        return false;

    std::wstring wanted = BaseName(file);

    // The lowest address among the rows for the lowest line at or after the
    // one asked for. Taking the lowest address matters with optimization
    // on, where one line's code can be scattered.
    bool found = false;
    int bestLine = 0;
    uint16_t bestAddress = 0;
    for (const LineRow& row : g_rows)
    {
        if (row.endSequence || row.file >= g_files.size())
            continue;
        if (BaseName(g_files[row.file].name) != wanted)
            continue;
        if (row.line < line)
            continue;
        if (!found || row.line < bestLine ||
            (row.line == bestLine && row.address < bestAddress))
        {
            found = true;
            bestLine = row.line;
            bestAddress = row.address;
        }
    }

    if (!found)
        return false;
    *address = bestAddress;
    *actualLine = bestLine;
    return true;
}

bool Dwarf_FullPathForFile(const std::wstring& file, std::wstring* path)
{
    std::wstring wanted = BaseName(file);
    for (const SourceFile& f : g_files)
    {
        if (BaseName(f.name) == wanted)
        {
            *path = f.path;
            return true;
        }
    }
    return false;
}

void Dwarf_ListFiles(std::vector<std::wstring>* files)
{
    for (const SourceFile& f : g_files)
        files->push_back(f.path);
}
