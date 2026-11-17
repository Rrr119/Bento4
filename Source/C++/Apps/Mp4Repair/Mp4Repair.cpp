/*****************************************************************
|
|    AP4 - MP4 File Repair
|
|    Copyright 2002-2026 Axiomatic Systems, LLC
|
|
|    This file is part of Bento4/AP4 (MP4 Atom Processing Library).
|
|    Unless you have obtained Bento4 under a difference license,
|    this version of Bento4 is Bento4|GPL.
|    Bento4|GPL is free software; you can redistribute it and/or modify
|    it under the terms of the GNU General Public License as published by
|    the Free Software Foundation; either version 2, or (at your option)
|    any later version.
|
|    Bento4|GPL is distributed in the hope that it will be useful,
|    but WITHOUT ANY WARRANTY; without even the implied warranty of
|    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
|    GNU General Public License for more details.
|
|    You should have received a copy of the GNU General Public License
|    along with Bento4|GPL; see the file COPYING.  If not, write to the
|    Free Software Foundation, 59 Temple Place - Suite 330, Boston, MA
|    02111-1307, USA.
|
 ****************************************************************/

/*----------------------------------------------------------------------
|   includes
+---------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "Ap4.h"
#include "Ap4AdtsParser.h"
#include "Ap4AvcParser.h"
#include "Ap4Results.h"

/*----------------------------------------------------------------------
|   constants
+---------------------------------------------------------------------*/
#define BANNER "MP4 File Repair - Version 1.0\n"\
               "(Bento4 Version " AP4_VERSION_STRING ")\n"\
               "(c) 2002-2026 Axiomatic Systems, LLC"

#define AP4_ATOM_TYPE_SKIP_ AP4_ATOM_TYPE('s','k','i','p')

/*----------------------------------------------------------------------
|   types
+---------------------------------------------------------------------*/
struct AtomRef {
    AtomRef() :
        m_Type(0),
        m_Offset(0),
        m_Payload(0),
        m_End(0),
        m_DeclaredSize(0),
        m_SizeIsValid(false) {}

    AP4_UI32     m_Type;
    AP4_UI64     m_Offset;
    AP4_UI64     m_Payload;
    AP4_UI64     m_End;
    AP4_UI64     m_DeclaredSize;
    bool         m_SizeIsValid;
};

struct SampleRecord {
    AP4_UI64 m_Offset;
    AP4_UI32 m_Size;
    AP4_UI64 m_Dts;
    AP4_UI32 m_Duration;
    AP4_UI32 m_CtsDelta;
    bool     m_IsSync;
};

struct TrackRecovery {
    TrackRecovery() :
        m_HandlerType(0),
        m_MediaTimeScale(0),
        m_MediaDuration(0),
        m_Width(0),
        m_Height(0),
        m_SampleDescription(NULL),
        m_SampleDescriptionIsOwned(true),
        m_SampleDataStream(NULL),
        m_DefaultDuration(0),
        m_DescriptionIndex(0) {}

    ~TrackRecovery() {
        if (m_SampleDescriptionIsOwned) delete m_SampleDescription;
        if (m_SampleDataStream) m_SampleDataStream->Release();
    }

    AP4_UI32                  m_HandlerType;
    AP4_UI32                  m_MediaTimeScale;
    AP4_UI64                  m_MediaDuration;
    AP4_UI32                  m_Width;
    AP4_UI32                  m_Height;
    AP4_SampleDescription*    m_SampleDescription;
    bool                      m_SampleDescriptionIsOwned;
    AP4_ByteStream*           m_SampleDataStream;
    AP4_UI32                  m_DefaultDuration;
    AP4_Ordinal               m_DescriptionIndex;
    std::vector<SampleRecord> m_Samples;
};

struct TopLevelBox {
    AP4_UI32 m_Type;
    AP4_UI64 m_Offset;
    AP4_UI64 m_Payload;
    AP4_UI64 m_End;
};

struct RepairOptions {
    RepairOptions() :
        m_Verbose(false) {}

    bool m_Verbose;
};

/*----------------------------------------------------------------------
|   PrintUsageAndExit
---------------------------------------------------------------------*/
static void
PrintUsageAndExit()
{
    fprintf(stderr,
            BANNER
            "\n\nusage: mp4repair [options] <input> <output>\n"
            "options are:\n"
            "  --verbose       print details about recovered atoms and samples\n"
            "\n"
            "The input is never modified. The output has the moov atom before the\n"
            "media data and can be played by clients that require a valid index.\n");
    exit(1);
}

/*----------------------------------------------------------------------
|   ReadStream
---------------------------------------------------------------------*/
static AP4_Result
ReadStream(AP4_ByteStream& stream, AP4_Position offset, void* data, AP4_Size size)
{
    AP4_Result result = stream.Seek(offset);
    if (AP4_FAILED(result)) return result;
    return stream.Read(data, size);
}

/*----------------------------------------------------------------------
|   ReadUint32
---------------------------------------------------------------------*/
static AP4_UI32
ReadUint32(const AP4_UI08* data)
{
    return ((AP4_UI32)data[0] << 24) |
           ((AP4_UI32)data[1] << 16) |
           ((AP4_UI32)data[2] <<  8) |
           ((AP4_UI32)data[3]);
}

/*----------------------------------------------------------------------
|   ReadUint64
---------------------------------------------------------------------*/
static AP4_UI64
ReadUint64(const AP4_UI08* data)
{
    return ((AP4_UI64)ReadUint32(data) << 32) | ReadUint32(data+4);
}

/*----------------------------------------------------------------------
|   TypeName
---------------------------------------------------------------------*/
static const char*
TypeName(AP4_UI32 type, char* buffer)
{
    buffer[0] = (char)(type >> 24);
    buffer[1] = (char)(type >> 16);
    buffer[2] = (char)(type >>  8);
    buffer[3] = (char)(type);
    buffer[4] = '\0';
    return buffer;
}

/*----------------------------------------------------------------------
|   IsKnownTopLevelType
---------------------------------------------------------------------*/
static bool
IsKnownTopLevelType(AP4_UI32 type)
{
    switch (type) {
        case AP4_ATOM_TYPE_FTYP:
        case AP4_ATOM_TYPE_MOOV:
        case AP4_ATOM_TYPE_MDAT:
        case AP4_ATOM_TYPE_MOOF:
        case AP4_ATOM_TYPE_MFRA:
        case AP4_ATOM_TYPE_FREE:
        case AP4_ATOM_TYPE_SKIP_:
        case AP4_ATOM_TYPE_WIDE:
        case AP4_ATOM_TYPE_PDIN:
            return true;
    }
    return false;
}

/*----------------------------------------------------------------------
|   IsPlausibleTopLevelBoxStart
---------------------------------------------------------------------*/
static bool
IsPlausibleTopLevelBoxStart(AP4_ByteStream& stream,
                            AP4_UI64        offset,
                            AP4_LargeSize   file_size)
{
    if (offset+8 > file_size) return false;
    AP4_UI08 header[16];
    if (AP4_FAILED(ReadStream(stream, offset, header, 8))) return false;

    AP4_UI32 size_32 = ReadUint32(header);
    AP4_UI32 type    = ReadUint32(header+4);
    AP4_UI64 size    = size_32;
    AP4_UI32 header_size = 8;
    if (size_32 == 1) {
        if (offset+16 > file_size) return false;
        if (AP4_FAILED(ReadStream(stream, offset+8, header+8, 8))) return false;
        size = ReadUint64(header+8);
        header_size = 16;
    } else if (size_32 == 0) {
        size = file_size-offset;
    }
    return IsKnownTopLevelType(type) &&
           size >= header_size &&
           offset+size <= file_size;
}

/*----------------------------------------------------------------------
|   FindMarker
---------------------------------------------------------------------*/
static AP4_Result
FindMarker(AP4_ByteStream& stream,
           AP4_UI32        type,
           AP4_Position    search_start,
           AP4_Position    search_end,
           AP4_Position&   found)
{
    found = 0;
    if (search_start >= search_end || search_end-search_start < 4) {
        return AP4_ERROR_EOS;
    }

    AP4_UI08 marker[4] = {
        (AP4_UI08)(type >> 24),
        (AP4_UI08)(type >> 16),
        (AP4_UI08)(type >>  8),
        (AP4_UI08)(type)
    };

    const AP4_Size buffer_size = 64*1024;
    AP4_UI08* buffer = new AP4_UI08[buffer_size+3];
    AP4_Position position = search_start;
    AP4_Size carry = 0;
    AP4_Result result = AP4_SUCCESS;

    while (position < search_end && AP4_SUCCEEDED(result)) {
        AP4_Size to_read = buffer_size;
        if (position+to_read > search_end) to_read = (AP4_Size)(search_end-position);
        AP4_Result read_result = ReadStream(stream, position, buffer+carry, to_read);
        if (AP4_FAILED(read_result)) {
            result = read_result;
            break;
        }

        AP4_Size available = carry+to_read;
        for (AP4_Size i=0; i+4<=available; i++) {
            if (buffer[i  ] == marker[0] &&
                buffer[i+1] == marker[1] &&
                buffer[i+2] == marker[2] &&
                buffer[i+3] == marker[3]) {
                found = position+i-carry;
                delete[] buffer;
                return AP4_SUCCESS;
            }
        }

        carry = available >= 3 ? 3 : available;
        if (available >= carry) {
            memmove(buffer, buffer+available-carry, carry);
        }
        position += to_read;
    }

    delete[] buffer;
    return AP4_ERROR_EOS;
}

/*----------------------------------------------------------------------
|   ReadTopLevelBoxes
---------------------------------------------------------------------*/
static AP4_Result
ReadTopLevelBoxes(AP4_ByteStream& stream,
                  AP4_LargeSize   file_size,
                  std::vector<TopLevelBox>& boxes)
{
    AP4_Position position = 0;
    while (position+8 <= file_size) {
        AP4_UI08 header[16];
        AP4_Result result = ReadStream(stream, position, header, 8);
        if (AP4_FAILED(result)) break;

        AP4_UI32 size_32 = ReadUint32(header);
        AP4_UI32 type    = ReadUint32(header+4);
        AP4_UI64 size    = size_32;
        AP4_UI32 header_size = 8;
        bool known = IsKnownTopLevelType(type);

        if (size_32 == 1) {
            if (position+16 > file_size) break;
            result = ReadStream(stream, position+8, header+8, 8);
            if (AP4_FAILED(result)) break;
            size = ReadUint64(header+8);
            header_size = 16;
        } else if (size_32 == 0) {
            size = file_size-position;
        }

        bool valid = known &&
                     size >= header_size &&
                     position+size <= file_size;
        if (known && !valid && type == AP4_ATOM_TYPE_MOOV) {
            TopLevelBox box;
            box.m_Type    = type;
            box.m_Offset  = position;
            box.m_Payload = position+header_size;
            box.m_End     = file_size;

            static const AP4_UI32 boundary_types[] = {
                AP4_ATOM_TYPE_MDAT,
                AP4_ATOM_TYPE_MOOF,
                AP4_ATOM_TYPE_MFRA
            };
            for (unsigned int i=0;
                 i<sizeof(boundary_types)/sizeof(boundary_types[0]);
                 i++) {
                AP4_Position marker;
                if (AP4_SUCCEEDED(FindMarker(stream,
                                             boundary_types[i],
                                             box.m_Payload+8,
                                             file_size,
                                             marker)) &&
                    marker >= 4 &&
                    IsPlausibleTopLevelBoxStart(stream, marker-4, file_size) &&
                    marker-4 < box.m_End) {
                    box.m_End = marker-4;
                }
            }
            boxes.push_back(box);
            position = box.m_End;
            continue;
        }
        if (valid) {
            TopLevelBox box;
            box.m_Type    = type;
            box.m_Offset  = position;
            box.m_Payload = position+header_size;
            box.m_End     = position+size;
            boxes.push_back(box);

            // A damaged moov size can either cut off the track table or
            // encompass the following media data. Use the next top-level
            // media atom as the authoritative boundary.
            if (type == AP4_ATOM_TYPE_MOOV) {
                static const AP4_UI32 boundary_types[] = {
                    AP4_ATOM_TYPE_MDAT,
                    AP4_ATOM_TYPE_MOOF,
                    AP4_ATOM_TYPE_MFRA
                };
                AP4_UI64 boundary = file_size;
                bool boundary_found = false;
                for (unsigned int i=0;
                     i<sizeof(boundary_types)/sizeof(boundary_types[0]);
                     i++) {
                    AP4_Position marker;
                    if (AP4_SUCCEEDED(FindMarker(stream,
                                                 boundary_types[i],
                                                 box.m_Payload+8,
                                                 file_size,
                                                 marker)) &&
                        marker >= 4 &&
                        marker-4 > box.m_Payload+8 &&
                        IsPlausibleTopLevelBoxStart(stream, marker-4, file_size) &&
                        marker-4 < boundary) {
                        boundary = marker-4;
                        boundary_found = true;
                    }
                }
                if (!boundary_found) boundary = file_size;
                if (boundary != box.m_End) {
                    box.m_End = boundary;
                    boxes.back() = box;
                    position = box.m_End;
                    continue;
                }
            }
            position = box.m_End;
            continue;
        }

        // Resynchronize on the next plausible top-level atom.
        static const AP4_UI32 resync_types[] = {
            AP4_ATOM_TYPE_MOOV,
            AP4_ATOM_TYPE_MDAT,
            AP4_ATOM_TYPE_FTYP,
            AP4_ATOM_TYPE_MOOF,
            AP4_ATOM_TYPE_MFRA,
            AP4_ATOM_TYPE_FREE,
            AP4_ATOM_TYPE_SKIP_,
            AP4_ATOM_TYPE_WIDE
        };
        AP4_Position next_position = 0;
        bool found_next = false;
        for (unsigned int i=0; i<sizeof(resync_types)/sizeof(resync_types[0]); i++) {
            AP4_Position marker;
            if (AP4_SUCCEEDED(FindMarker(stream,
                                         resync_types[i],
                                         position+1,
                                         file_size,
                                         marker))) {
                if (marker >= 4) {
                    AP4_Position candidate = marker-4;
                    if (!found_next || candidate < next_position) {
                        next_position = candidate;
                        found_next = true;
                    }
                }
            }
        }
        if (!found_next || next_position <= position) break;
        position = next_position;
    }

    return boxes.empty() ? AP4_ERROR_INVALID_FORMAT : AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   FindBox
---------------------------------------------------------------------*/
class AtomScanner
{
public:
    AtomScanner(AP4_ByteStream& stream,
                AP4_UI64        region_start,
                AP4_UI64        region_end,
                const AP4_UI08* data,
                AP4_Size        data_size) :
        m_Stream(stream),
        m_RegionStart(region_start),
        m_RegionEnd(region_end),
        m_Data(data),
        m_DataSize(data_size) {}

    AP4_Result Find(AP4_UI32 type, AP4_UI64 search_start, AtomRef& atom);

private:
    AP4_Result MakeAtom(AP4_UI64 marker_position,
                        AP4_UI64 region_end,
                        AtomRef& atom);

    AP4_ByteStream& m_Stream;
    AP4_UI64        m_RegionStart;
    AP4_UI64        m_RegionEnd;
    const AP4_UI08* m_Data;
    AP4_Size        m_DataSize;
};

/*----------------------------------------------------------------------
|   AtomScanner::MakeAtom
---------------------------------------------------------------------*/
AP4_Result
AtomScanner::MakeAtom(AP4_UI64 marker_position, AP4_UI64 region_end, AtomRef& atom)
{
    if (marker_position < m_RegionStart+4 || marker_position+4 > region_end) {
        return AP4_ERROR_INVALID_FORMAT;
    }

    AP4_UI64 offset = marker_position-4;
    AP4_Size index = (AP4_Size)(offset-m_RegionStart);
    if (index+8 > m_DataSize) return AP4_ERROR_INVALID_FORMAT;

    AP4_UI32 size_32 = ReadUint32(m_Data+index);
    AP4_UI32 type    = ReadUint32(m_Data+index+4);
    AP4_UI64 size    = size_32;
    AP4_UI32 header_size = 8;

    if (size_32 == 1) {
        if (index+16 > m_DataSize) return AP4_ERROR_INVALID_FORMAT;
        size = ReadUint64(m_Data+index+8);
        header_size = 16;
    } else if (size_32 == 0) {
        size = region_end-offset;
    }

    atom.m_Type         = type;
    atom.m_Offset       = offset;
    atom.m_Payload      = offset+header_size;
    atom.m_DeclaredSize = size;
    atom.m_SizeIsValid  = size >= header_size && offset+size <= region_end &&
                          offset+size > marker_position;
    atom.m_End          = atom.m_SizeIsValid ? offset+size : region_end;

    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AtomScanner::Find
---------------------------------------------------------------------*/
AP4_Result
AtomScanner::Find(AP4_UI32 type, AP4_UI64 search_start, AtomRef& atom)
{
    if (search_start < m_RegionStart) search_start = m_RegionStart;
    if (search_start+8 > m_RegionEnd) return AP4_ERROR_EOS;

    AP4_Size index = (AP4_Size)(search_start-m_RegionStart);
    AP4_UI08 marker[4] = {
        (AP4_UI08)(type >> 24),
        (AP4_UI08)(type >> 16),
        (AP4_UI08)(type >>  8),
        (AP4_UI08)(type)
    };

    for (; index+4 <= m_DataSize; index++) {
        if (m_Data[index  ] != marker[0] ||
            m_Data[index+1] != marker[1] ||
            m_Data[index+2] != marker[2] ||
            m_Data[index+3] != marker[3]) {
            continue;
        }

        AP4_UI64 marker_position = m_RegionStart+index;
        AtomRef candidate;
        if (AP4_FAILED(MakeAtom(marker_position, m_RegionEnd, candidate))) {
            continue;
        }
        if (candidate.m_Type != type) continue;
        if (candidate.m_Offset+8 > m_RegionEnd) continue;

        atom = candidate;
        return AP4_SUCCESS;
    }

    return AP4_ERROR_EOS;
}

/*----------------------------------------------------------------------
|   BufferHas
---------------------------------------------------------------------*/
static bool
BufferHas(const AP4_UI08* data, AP4_Size size, AP4_UI64 offset, AP4_Size count)
{
    return offset <= size && count <= size-(AP4_Size)offset;
}

/*----------------------------------------------------------------------
|   ParseSampleSizes
---------------------------------------------------------------------*/
static bool
ParseSampleSizes(const AtomRef&           stsz,
                 const AP4_UI08*          data,
                 AP4_Size                 data_size,
                 AP4_UI64                 region_start,
                 AP4_UI64                 track_end,
                 std::vector<AP4_UI32>&   sizes)
{
    if (!BufferHas(data, data_size, stsz.m_Payload-region_start, 12)) return false;
    AP4_Size index = (AP4_Size)(stsz.m_Payload-region_start);
    AP4_UI32 sample_size  = ReadUint32(data+index+4);
    AP4_UI32 sample_count = ReadUint32(data+index+8);
    if (sample_count > 10000000) return false;

    AP4_UI64 entries_offset = stsz.m_Payload+12;
    if (sample_size != 0) {
        sizes.assign(sample_count, sample_size);
        return sample_count != 0;
    }

    AP4_UI64 available = stsz.m_End > entries_offset ? stsz.m_End-entries_offset : 0;
    AP4_UI64 required = (AP4_UI64)sample_count*4;
    if (sample_count <= 10000000 &&
        entries_offset+required <= track_end &&
        entries_offset+required <= region_start+data_size) {
        available = required;
    }
    AP4_UI32 max_entries = (AP4_UI32)(available/4);
    if (sample_count > max_entries) sample_count = max_entries;
    if (sample_count == 0) return false;

    sizes.resize(sample_count);
    for (AP4_UI32 i=0; i<sample_count; i++) {
        AP4_UI64 offset = entries_offset-region_start+i*4;
        if (!BufferHas(data, data_size, offset, 4)) {
            sizes.resize(i);
            break;
        }
        sizes[i] = ReadUint32(data+(AP4_Size)offset);
    }
    return !sizes.empty();
}

/*----------------------------------------------------------------------
|   ParseStz2Sizes
---------------------------------------------------------------------*/
static bool
ParseStz2Sizes(const AtomRef&           stz2,
               const AP4_UI08*          data,
               AP4_Size                 data_size,
               AP4_UI64                 region_start,
               std::vector<AP4_UI32>&   sizes)
{
    if (!BufferHas(data, data_size, stz2.m_Payload-region_start, 12)) return false;
    AP4_Size index = (AP4_Size)(stz2.m_Payload-region_start);
    AP4_UI08 field_size = data[index+7];
    AP4_UI32 sample_count = ReadUint32(data+index+8);
    if (field_size != 4 && field_size != 8 && field_size != 16) return false;
    if (sample_count > 10000000) return false;

    AP4_UI64 entries_offset = stz2.m_Payload+12;
    AP4_UI64 available = stz2.m_End > entries_offset ? stz2.m_End-entries_offset : 0;
    AP4_UI64 required = ((AP4_UI64)sample_count*field_size+7)/8;
    if (required > available) return false;

    sizes.resize(sample_count);
    for (AP4_UI32 i=0; i<sample_count; i++) {
        if (field_size == 4) {
            AP4_UI64 offset = (AP4_UI64)i/2;
            if (!BufferHas(data, data_size, entries_offset-region_start+offset, 1)) {
                return false;
            }
            AP4_UI08 value = data[(AP4_Size)(entries_offset-region_start+offset)];
            sizes[i] = (i & 1) ? (value & 0xF) : (value >> 4);
        } else if (field_size == 8) {
            if (!BufferHas(data, data_size, entries_offset-region_start+i, 1)) {
                return false;
            }
            sizes[i] = data[(AP4_Size)(entries_offset-region_start+i)];
        } else {
            if (!BufferHas(data, data_size, entries_offset-region_start+i*2, 2)) {
                return false;
            }
            AP4_Size offset = (AP4_Size)(entries_offset-region_start+i*2);
            sizes[i] = ((AP4_UI32)data[offset] << 8) | data[offset+1];
        }
    }
    return !sizes.empty();
}

/*----------------------------------------------------------------------
|   ParseStsc
---------------------------------------------------------------------*/
struct StscEntry {
    AP4_UI32 m_FirstChunk;
    AP4_UI32 m_SamplesPerChunk;
    AP4_UI32 m_DescriptionIndex;
};

static bool
ParseStsc(const AtomRef&           stsc,
          const AP4_UI08*          data,
          AP4_Size                 data_size,
          AP4_UI64                 region_start,
          AP4_UI64                 track_end,
          std::vector<StscEntry>&  entries)
{
    if (!BufferHas(data, data_size, stsc.m_Payload-region_start, 8)) return false;
    AP4_Size index = (AP4_Size)(stsc.m_Payload-region_start);
    AP4_UI32 entry_count = ReadUint32(data+index+4);
    if (entry_count > 1000000) return false;

    AP4_UI64 entries_offset = stsc.m_Payload+8;
    AP4_UI64 available = stsc.m_End > entries_offset ? stsc.m_End-entries_offset : 0;
    AP4_UI64 required = (AP4_UI64)entry_count*12;
    if (entries_offset+required <= track_end &&
        entries_offset+required <= region_start+data_size) {
        available = required;
    }
    if (required > available) {
        entry_count = (AP4_UI32)(available/12);
    }

    entries.resize(entry_count);
    for (AP4_UI32 i=0; i<entry_count; i++) {
        AP4_Size offset = (AP4_Size)(entries_offset-region_start+i*12);
        if (!BufferHas(data, data_size, offset, 12)) return false;
        entries[i].m_FirstChunk        = ReadUint32(data+offset);
        entries[i].m_SamplesPerChunk   = ReadUint32(data+offset+4);
        entries[i].m_DescriptionIndex  = ReadUint32(data+offset+8);
    }
    return !entries.empty();
}

/*----------------------------------------------------------------------
|   ParseChunkOffsets
---------------------------------------------------------------------*/
static bool
ParseChunkOffsets(const AtomRef&              atom,
                  bool                       is_64,
                  const AP4_UI08*            data,
                  AP4_Size                   data_size,
                  AP4_UI64                   region_start,
                  AP4_UI64                   track_end,
                  std::vector<AP4_UI64>&     offsets)
{
    if (!BufferHas(data, data_size, atom.m_Payload-region_start, 8)) return false;
    AP4_Size index = (AP4_Size)(atom.m_Payload-region_start);
    AP4_UI32 entry_count = ReadUint32(data+index+4);
    if (entry_count > 10000000) return false;
    AP4_UI64 entry_size = is_64 ? 8 : 4;
    AP4_UI64 entries_offset = atom.m_Payload+8;
    AP4_UI64 available = atom.m_End > entries_offset ? atom.m_End-entries_offset : 0;
    AP4_UI64 required = (AP4_UI64)entry_count*entry_size;
    if (entries_offset+required <= track_end &&
        entries_offset+required <= region_start+data_size) {
        available = required;
    }
    if (required > available) {
        entry_count = (AP4_UI32)(available/entry_size);
    }
    if (entry_count == 0) return false;

    offsets.resize(entry_count);
    for (AP4_UI32 i=0; i<entry_count; i++) {
        AP4_Size offset = (AP4_Size)(entries_offset-region_start+i*entry_size);
        if (!BufferHas(data, data_size, offset, (AP4_Size)entry_size)) return false;
        offsets[i] = is_64 ? ReadUint64(data+offset) : ReadUint32(data+offset);
    }
    return true;
}

/*----------------------------------------------------------------------
|   ParseStts
---------------------------------------------------------------------*/
struct TimeEntry {
    AP4_UI32 m_Count;
    AP4_UI32 m_Duration;
};

static bool
ParseStts(const AtomRef&          stts,
          const AP4_UI08*         data,
          AP4_Size                data_size,
          AP4_UI64                region_start,
          AP4_UI64                track_end,
          std::vector<TimeEntry>& entries)
{
    if (!BufferHas(data, data_size, stts.m_Payload-region_start, 8)) return false;
    AP4_Size index = (AP4_Size)(stts.m_Payload-region_start);
    AP4_UI32 entry_count = ReadUint32(data+index+4);
    if (entry_count > 1000000) return false;
    AP4_UI64 entries_offset = stts.m_Payload+8;
    AP4_UI64 available = stts.m_End > entries_offset ? stts.m_End-entries_offset : 0;
    AP4_UI64 required = (AP4_UI64)entry_count*8;
    if (entries_offset+required <= track_end &&
        entries_offset+required <= region_start+data_size) {
        available = required;
    }
    if (required > available) {
        entry_count = (AP4_UI32)(available/8);
    }
    entries.resize(entry_count);
    for (AP4_UI32 i=0; i<entry_count; i++) {
        AP4_Size offset = (AP4_Size)(entries_offset-region_start+i*8);
        if (!BufferHas(data, data_size, offset, 8)) return false;
        entries[i].m_Count    = ReadUint32(data+offset);
        entries[i].m_Duration = ReadUint32(data+offset+4);
    }
    return !entries.empty();
}

/*----------------------------------------------------------------------
|   ParseCtts
---------------------------------------------------------------------*/
static bool
ParseCtts(const AtomRef&          ctts,
          const AP4_UI08*         data,
          AP4_Size                data_size,
          AP4_UI64                region_start,
          AP4_UI64                track_end,
          std::vector<TimeEntry>& entries)
{
    if (!BufferHas(data, data_size, ctts.m_Payload-region_start, 8)) return false;
    AP4_Size index = (AP4_Size)(ctts.m_Payload-region_start);
    AP4_UI32 entry_count = ReadUint32(data+index+4);
    if (entry_count > 1000000) return false;
    AP4_UI64 entries_offset = ctts.m_Payload+8;
    AP4_UI64 available = ctts.m_End > entries_offset ? ctts.m_End-entries_offset : 0;
    AP4_UI64 required = (AP4_UI64)entry_count*8;
    if (entries_offset+required <= track_end &&
        entries_offset+required <= region_start+data_size) {
        available = required;
    }
    if (required > available) {
        entry_count = (AP4_UI32)(available/8);
    }
    entries.resize(entry_count);
    for (AP4_UI32 i=0; i<entry_count; i++) {
        AP4_Size offset = (AP4_Size)(entries_offset-region_start+i*8);
        if (!BufferHas(data, data_size, offset, 8)) return false;
        entries[i].m_Count    = ReadUint32(data+offset);
        entries[i].m_Duration = ReadUint32(data+offset+4);
    }
    return true;
}

/*----------------------------------------------------------------------
|   ParseStss
---------------------------------------------------------------------*/
static bool
ParseStss(const AtomRef&          stss,
          const AP4_UI08*         data,
          AP4_Size                data_size,
          AP4_UI64                region_start,
          AP4_UI64                track_end,
          std::vector<AP4_UI32>&  entries)
{
    if (!BufferHas(data, data_size, stss.m_Payload-region_start, 8)) return false;
    AP4_Size index = (AP4_Size)(stss.m_Payload-region_start);
    AP4_UI32 entry_count = ReadUint32(data+index+4);
    if (entry_count > 10000000) return false;
    AP4_UI64 entries_offset = stss.m_Payload+8;
    AP4_UI64 available = stss.m_End > entries_offset ? stss.m_End-entries_offset : 0;
    AP4_UI64 required = (AP4_UI64)entry_count*4;
    if (entries_offset+required <= track_end &&
        entries_offset+required <= region_start+data_size) {
        available = required;
    }
    if (required > available) {
        entry_count = (AP4_UI32)(available/4);
    }
    entries.resize(entry_count);
    for (AP4_UI32 i=0; i<entry_count; i++) {
        AP4_Size offset = (AP4_Size)(entries_offset-region_start+i*4);
        if (!BufferHas(data, data_size, offset, 4)) return false;
        entries[i] = ReadUint32(data+offset);
    }
    return true;
}

/*----------------------------------------------------------------------
|   ParseMdhd
---------------------------------------------------------------------*/
static void
ParseMdhd(const AtomRef&  mdhd,
          const AP4_UI08* data,
          AP4_Size        data_size,
          AP4_UI64        region_start,
          AP4_UI32&       time_scale,
          AP4_UI64&       duration)
{
    if (!BufferHas(data, data_size, mdhd.m_Payload-region_start, 24)) return;
    AP4_Size index = (AP4_Size)(mdhd.m_Payload-region_start);
    AP4_UI08 version = data[index] >> 24;
    if (version == 0) {
        if (!BufferHas(data, data_size, index, 24)) return;
        time_scale = ReadUint32(data+index+12);
        duration   = ReadUint32(data+index+16);
    } else if (version == 1) {
        if (!BufferHas(data, data_size, index, 36)) return;
        time_scale = ReadUint32(data+index+20);
        duration   = ReadUint64(data+index+24);
    }
}

/*----------------------------------------------------------------------
|   ParseHdlr
---------------------------------------------------------------------*/
static AP4_UI32
ParseHdlr(const AtomRef&  hdlr,
          const AP4_UI08* data,
          AP4_Size        data_size,
          AP4_UI64        region_start)
{
    if (!BufferHas(data, data_size, hdlr.m_Payload-region_start, 16)) return 0;
    AP4_Size index = (AP4_Size)(hdlr.m_Payload-region_start);
    return ReadUint32(data+index+8);
}

/*----------------------------------------------------------------------
|   GetTimedValue
---------------------------------------------------------------------*/
static bool
GetTimedValue(const std::vector<TimeEntry>& entries,
              AP4_UI32                      sample_index,
              AP4_UI32&                     value)
{
    AP4_UI64 sample = sample_index;
    AP4_UI64 cursor = 0;
    for (unsigned int i=0; i<entries.size(); i++) {
        AP4_UI64 next = cursor+entries[i].m_Count;
        if (sample < next) {
            value = entries[i].m_Duration;
            return true;
        }
        cursor = next;
    }
    return false;
}

/*----------------------------------------------------------------------
|   IsSyncSample
---------------------------------------------------------------------*/
static bool
IsSyncSample(const std::vector<AP4_UI32>& sync_samples, AP4_UI32 sample)
{
    if (sync_samples.empty()) return true;
    return std::binary_search(sync_samples.begin(), sync_samples.end(), sample);
}

/*----------------------------------------------------------------------
|   ParseSampleTable
---------------------------------------------------------------------*/
static bool
ParseSampleTable(AtomScanner&            scanner,
                 AP4_UI64                track_start,
                 AP4_UI64                track_end,
                 const AP4_UI08*        data,
                 AP4_Size                data_size,
                 AP4_UI64                region_start,
                 AP4_LargeSize           file_size,
                 TrackRecovery&          track)
{
    AtomRef stsz;
    AtomRef stz2;
    AtomRef stsc;
    AtomRef stco;
    AtomRef co64;
    AtomRef stts;
    AtomRef ctts;
    AtomRef mdhd;
    AtomRef stss;

    bool has_stsz = AP4_SUCCEEDED(scanner.Find(AP4_ATOM_TYPE_STSZ, track_start, stsz));
    bool has_stz2 = AP4_SUCCEEDED(scanner.Find(AP4_ATOM_TYPE_STZ2, track_start, stz2));
    bool has_stsc = AP4_SUCCEEDED(scanner.Find(AP4_ATOM_TYPE_STSC, track_start, stsc));
    bool has_stco = AP4_SUCCEEDED(scanner.Find(AP4_ATOM_TYPE_STCO, track_start, stco));
    bool has_co64 = AP4_SUCCEEDED(scanner.Find(AP4_ATOM_TYPE_CO64, track_start, co64));
    bool has_stts = AP4_SUCCEEDED(scanner.Find(AP4_ATOM_TYPE_STTS, track_start, stts));
    bool has_ctts = AP4_SUCCEEDED(scanner.Find(AP4_ATOM_TYPE_CTTS, track_start, ctts));
    bool has_mdhd = AP4_SUCCEEDED(scanner.Find(AP4_ATOM_TYPE_MDHD, track_start, mdhd));
    bool has_stss = AP4_SUCCEEDED(scanner.Find(AP4_ATOM_TYPE_STSS, track_start, stss));
    (void)scanner;

    if ((!has_stsz && !has_stz2) || !has_stsc || (!has_stco && !has_co64)) {
        return false;
    }

    std::vector<AP4_UI32> sizes;
    if (has_stsz) {
        ParseSampleSizes(stsz, data, data_size, region_start, track_end, sizes);
    }
    if (sizes.empty() && has_stz2) {
        ParseStz2Sizes(stz2, data, data_size, region_start, sizes);
    }
    if (sizes.empty()) return false;

    std::vector<StscEntry> stsc_entries;
    if (!ParseStsc(stsc, data, data_size, region_start, track_end, stsc_entries)) return false;

    std::vector<AP4_UI64> chunk_offsets;
    if (has_stco) {
        ParseChunkOffsets(stco, false, data, data_size, region_start, track_end, chunk_offsets);
    }
    if (chunk_offsets.empty() && has_co64) {
        ParseChunkOffsets(co64, true, data, data_size, region_start, track_end, chunk_offsets);
    }
    if (chunk_offsets.empty()) return false;

    std::vector<TimeEntry> stts_entries;
    if (has_stts) ParseStts(stts, data, data_size, region_start, track_end, stts_entries);
    std::vector<TimeEntry> ctts_entries;
    if (has_ctts) ParseCtts(ctts, data, data_size, region_start, track_end, ctts_entries);
    std::vector<AP4_UI32> sync_samples;
    if (has_stss) ParseStss(stss, data, data_size, region_start, track_end, sync_samples);
    std::sort(sync_samples.begin(), sync_samples.end());

    if (has_mdhd) {
        ParseMdhd(mdhd, data, data_size, region_start,
                  track.m_MediaTimeScale, track.m_MediaDuration);
    }

    AP4_UI32 sample_index = 0;
    AP4_UI64 dts = 0;
    for (unsigned int chunk_index=0;
         chunk_index<chunk_offsets.size() && sample_index<sizes.size();
         chunk_index++) {
        AP4_UI32 chunk_number = chunk_index+1;
        unsigned int stsc_index = 0;
        for (unsigned int i=0; i<stsc_entries.size(); i++) {
            if (stsc_entries[i].m_FirstChunk <= chunk_number) {
                stsc_index = i;
            } else {
                break;
            }
        }
        AP4_UI32 samples_per_chunk = stsc_entries[stsc_index].m_SamplesPerChunk;
        AP4_UI32 description_index = stsc_entries[stsc_index].m_DescriptionIndex;
        if (samples_per_chunk == 0) return false;

        AP4_UI64 offset = chunk_offsets[chunk_index];
        for (AP4_UI32 i=0; i<samples_per_chunk && sample_index<sizes.size(); i++) {
            AP4_UI32 size = sizes[sample_index];
            if (size == 0 || offset+size > file_size) {
                sample_index++;
                offset += size;
                continue;
            }

            SampleRecord sample;
            sample.m_Offset   = offset;
            sample.m_Size     = size;
            sample.m_Dts      = dts;
            sample.m_Duration = 0;
            sample.m_CtsDelta = 0;
            sample.m_IsSync   = IsSyncSample(sync_samples, sample_index+1);

            if (!stts_entries.empty()) {
                GetTimedValue(stts_entries, sample_index, sample.m_Duration);
            }
            if (!ctts_entries.empty()) {
                GetTimedValue(ctts_entries, sample_index, sample.m_CtsDelta);
            }
            if (sample.m_Duration == 0) {
                sample.m_Duration = track.m_DefaultDuration;
            }
            if (sample.m_Duration == 0) sample.m_Duration = 1;

            track.m_Samples.push_back(sample);
            dts += sample.m_Duration;
            offset += size;
            sample_index++;
            if (track.m_DescriptionIndex == 0 && description_index > 0) {
                track.m_DescriptionIndex = description_index-1;
            }
        }
    }

    if (track.m_MediaTimeScale == 0) {
        if (track.m_HandlerType == AP4_HANDLER_TYPE_SOUN) {
            track.m_MediaTimeScale = 48000;
        } else {
            track.m_MediaTimeScale = 30000;
        }
    }
    if (track.m_MediaDuration == 0) {
        track.m_MediaDuration = dts;
    }

    return !track.m_Samples.empty();
}

/*----------------------------------------------------------------------
|   FindSampleEntry
---------------------------------------------------------------------*/
static bool
FindSampleEntry(AtomScanner& scanner,
                AP4_UI64     track_start,
                AP4_UI64     track_end,
                AtomRef&     entry)
{
    AtomRef stsd;
    if (AP4_FAILED(scanner.Find(AP4_ATOM_TYPE_STSD, track_start, stsd))) {
        return false;
    }

    static const AP4_UI32 entry_types[] = {
        AP4_ATOM_TYPE_AVC1,
        AP4_ATOM_TYPE_AVC2,
        AP4_ATOM_TYPE_AVC3,
        AP4_ATOM_TYPE_AVC4,
        AP4_ATOM_TYPE_HVC1,
        AP4_ATOM_TYPE_HEV1,
        AP4_ATOM_TYPE_MP4A,
        AP4_ATOM_TYPE_MP4V,
        AP4_ATOM_TYPE_MP4S,
        AP4_ATOM_TYPE_ALAC,
        AP4_ATOM_TYPE_AC_3,
        AP4_ATOM_TYPE_EC_3,
        AP4_ATOM_TYPE_AC_4,
        AP4_ATOM_TYPE_AV01,
        AP4_ATOM_TYPE_VP08,
        AP4_ATOM_TYPE_VP09,
        AP4_ATOM_TYPE_OPUS,
        AP4_ATOM_TYPE_ENCA,
        AP4_ATOM_TYPE_ENCV
    };

    AP4_UI64 search_start = stsd.m_Payload+8;
    if (search_start >= track_end) search_start = stsd.m_Payload;
    for (unsigned int i=0; i<sizeof(entry_types)/sizeof(entry_types[0]); i++) {
        AtomRef candidate;
        if (AP4_SUCCEEDED(scanner.Find(entry_types[i], search_start, candidate))) {
            if (candidate.m_Offset < track_end) {
                entry = candidate;
                return true;
            }
        }
    }
    return false;
}

/*----------------------------------------------------------------------
|   CreateDescriptionFromSampleEntry
---------------------------------------------------------------------*/
static AP4_SampleDescription*
CreateDescriptionFromSampleEntry(AP4_ByteStream& stream,
                                 const AtomRef&  entry,
                                 AP4_UI64        track_end,
                                 bool            verbose)
{
    AP4_UI64 entry_size = entry.m_End-entry.m_Offset;
    if (!entry.m_SizeIsValid) {
        AP4_Position next_marker;
        if (AP4_SUCCEEDED(FindMarker(stream,
                                     AP4_ATOM_TYPE_STTS,
                                     entry.m_Offset+16,
                                     track_end,
                                     next_marker)) &&
            next_marker >= entry.m_Offset+16) {
            entry_size = next_marker-4-entry.m_Offset;
        }
    }
    if (entry_size < 16 || entry_size > 0xFFFFFFFF) return NULL;

    AP4_SampleDescription* description = NULL;
    bool is_avc = entry.m_Type == AP4_ATOM_TYPE_AVC1 ||
                  entry.m_Type == AP4_ATOM_TYPE_AVC2 ||
                  entry.m_Type == AP4_ATOM_TYPE_AVC3 ||
                  entry.m_Type == AP4_ATOM_TYPE_AVC4;

    if (!is_avc) {
        AP4_AtomFactory factory;
        factory.PushContext(AP4_ATOM_TYPE_STSD);
        AP4_Atom* atom = NULL;
        AP4_Result result = stream.Seek(entry.m_Offset+8);
        if (AP4_SUCCEEDED(result)) {
            result = factory.CreateAtomFromStream(stream,
                                                  entry.m_Type,
                                                  (AP4_UI32)entry_size,
                                                  entry_size,
                                                  atom);
        }
        factory.PopContext();

        if (AP4_SUCCEEDED(result) && atom) {
            AP4_SampleEntry* sample_entry = AP4_DYNAMIC_CAST(AP4_SampleEntry, atom);
            if (sample_entry) description = sample_entry->ToSampleDescription();
        }
        delete atom;
        if (description) return description;
    }

    // A broken sample entry size may have prevented the normal factory path.
    // Salvage the codec configuration directly for AVC and AAC.
    if (is_avc) {
        AP4_Position avcc_marker;
        if (AP4_SUCCEEDED(FindMarker(stream,
                                     AP4_ATOM_TYPE_AVCC,
                                     entry.m_Offset+16,
                                     track_end,
                                     avcc_marker))) {
            AP4_Position avcc_offset = avcc_marker-4;
            AP4_UI08 header[8];
            if (AP4_SUCCEEDED(ReadStream(stream, avcc_offset, header, 8))) {
                AP4_UI32 avcc_size = ReadUint32(header);
                if (avcc_size >= 8 && avcc_size <= track_end-avcc_offset) {
                    stream.Seek(avcc_offset+8);
                    AP4_AvccAtom* avcc = AP4_AvccAtom::Create(avcc_size, stream);
                    if (avcc) {
                        AP4_UI08 visual[4];
                        AP4_UI16 width = 0;
                        AP4_UI16 height = 0;
                        if (AP4_SUCCEEDED(ReadStream(stream, entry.m_Offset+32, visual, 4))) {
                            width  = ((AP4_UI16)visual[0] << 8) | visual[1];
                            height = ((AP4_UI16)visual[2] << 8) | visual[3];
                        }
                        description = new AP4_AvcSampleDescription(entry.m_Type,
                                                                   width,
                                                                   height,
                                                                   24,
                                                                   "AVC Coding",
                                                                   avcc);
                        if (verbose) {
                            fprintf(stderr,
                                    "mp4repair: recovered AVC configuration directly (%dx%d)\n",
                                    width, height);
                        }
                    }
                }
            }
        }
    } else if (entry.m_Type == AP4_ATOM_TYPE_MP4A ||
               entry.m_Type == AP4_ATOM_TYPE_ENCA) {
        AP4_Position esds_marker;
        if (AP4_SUCCEEDED(FindMarker(stream,
                                     AP4_ATOM_TYPE_ESDS,
                                     entry.m_Offset+16,
                                     track_end,
                                     esds_marker))) {
            AP4_Position esds_offset = esds_marker-4;
            AP4_UI08 header[8];
            AP4_UI08 audio[16];
            AP4_UI32 sample_rate = 0;
            AP4_UI16 channel_count = 0;
            AP4_UI16 sample_size = 16;
            if (AP4_SUCCEEDED(ReadStream(stream, entry.m_Offset+24, audio, 12))) {
                channel_count = ((AP4_UI16)audio[0] << 8) | audio[1];
                sample_size   = ((AP4_UI16)audio[2] << 8) | audio[3];
                sample_rate   = ReadUint32(audio+8) >> 16;
            }
            if (AP4_SUCCEEDED(ReadStream(stream, esds_offset, header, 8))) {
                AP4_UI32 esds_size = ReadUint32(header);
                if (esds_size >= 12 && esds_size <= track_end-esds_offset) {
                    stream.Seek(esds_offset+8);
                    AP4_EsdsAtom* esds = AP4_EsdsAtom::Create(esds_size, stream);
                    if (esds) {
                        description = new AP4_MpegAudioSampleDescription(sample_rate,
                                                                         sample_size,
                                                                         channel_count,
                                                                         esds);
                        delete esds;
                    }
                }
            }
        }
    }

    return description;
}

/*----------------------------------------------------------------------
|   FindTrakEnd
---------------------------------------------------------------------*/
static AP4_UI64
FindTrakEnd(AP4_ByteStream& stream, AP4_UI64 start, AP4_UI64 region_end)
{
    AP4_Position next_trak;
    AP4_Position next_udta;
    AP4_Position next_meta;
    bool found = false;
    AP4_UI64 end = region_end;
    if (AP4_SUCCEEDED(FindMarker(stream, AP4_ATOM_TYPE_TRAK,
                                 start+8, region_end, next_trak)) &&
        next_trak >= 4) {
        end = next_trak-4;
        found = true;
    }
    if (AP4_SUCCEEDED(FindMarker(stream, AP4_ATOM_TYPE_UDTA,
                                 start+8, region_end, next_udta)) &&
        next_udta >= 4 &&
        (!found || next_udta-4 < end)) {
        end = next_udta-4;
    }
    if (AP4_SUCCEEDED(FindMarker(stream, AP4_ATOM_TYPE_META,
                                 start+8, region_end, next_meta)) &&
        next_meta >= 4 &&
        next_meta-4 > start+8 &&
        next_meta-4 < end) {
        end = next_meta-4;
    }
    return end;
}

/*----------------------------------------------------------------------
|   InferMoovRegion
---------------------------------------------------------------------*/
static bool
InferMoovRegion(AP4_ByteStream& stream,
                AP4_LargeSize   file_size,
                TopLevelBox&    moov)
{
    static const AP4_UI32 anchors[] = {
        AP4_ATOM_TYPE_MVHD,
        AP4_ATOM_TYPE_TRAK
    };
    for (unsigned int i=0; i<sizeof(anchors)/sizeof(anchors[0]); i++) {
        AP4_Position marker;
        if (AP4_FAILED(FindMarker(stream, anchors[i], 0, file_size, marker))) {
            continue;
        }
        if (marker < 12) continue;

        moov.m_Type    = AP4_ATOM_TYPE_MOOV;
        moov.m_Offset  = marker-4-8;
        moov.m_Payload = moov.m_Offset+8;
        moov.m_End     = file_size;

        static const AP4_UI32 boundary_types[] = {
            AP4_ATOM_TYPE_MDAT,
            AP4_ATOM_TYPE_MOOF,
            AP4_ATOM_TYPE_MFRA
        };
        for (unsigned int j=0;
             j<sizeof(boundary_types)/sizeof(boundary_types[0]);
             j++) {
            AP4_Position boundary_marker;
            if (AP4_SUCCEEDED(FindMarker(stream,
                                         boundary_types[j],
                                         moov.m_Payload+8,
                                         file_size,
                                         boundary_marker)) &&
                boundary_marker >= 4 &&
                IsPlausibleTopLevelBoxStart(stream, boundary_marker-4, file_size) &&
                boundary_marker-4 < moov.m_End) {
                moov.m_End = boundary_marker-4;
            }
        }
        return moov.m_End > moov.m_Payload;
    }
    return false;
}

/*----------------------------------------------------------------------
|   RecoverTracks
---------------------------------------------------------------------*/
static AP4_Result
RecoverTracks(AP4_ByteStream&  stream,
              AP4_LargeSize    file_size,
              const TopLevelBox& moov,
              const RepairOptions& options,
              std::vector<TrackRecovery*>& tracks)
{
    AP4_UI64 moov_size = moov.m_End-moov.m_Offset;
    if (moov_size < 8 || moov_size > 256*1024*1024) {
        return AP4_ERROR_INVALID_FORMAT;
    }

    AP4_DataBuffer moov_data;
    moov_data.SetDataSize((AP4_Size)moov_size);
    AP4_Result result = ReadStream(stream, moov.m_Offset,
                                   moov_data.UseData(), (AP4_Size)moov_size);
    if (AP4_FAILED(result)) return result;

    AtomScanner scanner(stream, moov.m_Offset, moov.m_End,
                        moov_data.GetData(), moov_data.GetDataSize());

    if (options.m_Verbose) {
        fprintf(stderr,
                "mp4repair: scanning moov at %llu..%llu\n",
                (unsigned long long)moov.m_Offset,
                (unsigned long long)moov.m_End);
    }

    AP4_Position trak_marker = 0;
    AP4_Position search = moov.m_Payload;
    while (AP4_SUCCEEDED(FindMarker(stream, AP4_ATOM_TYPE_TRAK,
                                    search, moov.m_End, trak_marker))) {
        if (options.m_Verbose) {
            fprintf(stderr, "mp4repair: found trak marker at %llu\n",
                    (unsigned long long)trak_marker);
        }
        if (trak_marker < 4) break;
        AP4_UI64 track_start = trak_marker-4;
        AP4_UI64 track_end = FindTrakEnd(stream, track_start, moov.m_End);
        if (options.m_Verbose) {
            fprintf(stderr, "mp4repair: trak range %llu..%llu\n",
                    (unsigned long long)track_start,
                    (unsigned long long)track_end);
        }
        if (track_end <= track_start+8) {
            search = trak_marker+4;
            continue;
        }

        TrackRecovery* track = new TrackRecovery;
        AtomRef hdlr;
        if (AP4_SUCCEEDED(scanner.Find(AP4_ATOM_TYPE_HDLR, track_start, hdlr))) {
            track->m_HandlerType = ParseHdlr(hdlr, moov_data.GetData(),
                                             moov_data.GetDataSize(), moov.m_Offset);
        }

        AtomRef entry;
        if (FindSampleEntry(scanner, track_start, track_end, entry)) {
            track->m_SampleDescription =
                CreateDescriptionFromSampleEntry(stream, entry, track_end, options.m_Verbose);
            if (track->m_SampleDescription) {
                AP4_VideoSampleDescription* video =
                    AP4_DYNAMIC_CAST(AP4_VideoSampleDescription, track->m_SampleDescription);
                if (video) {
                    track->m_Width  = ((AP4_UI32)video->GetWidth()) << 16;
                    track->m_Height = ((AP4_UI32)video->GetHeight()) << 16;
                }
                if (track->m_HandlerType == 0) {
                    AP4_AudioSampleDescription* audio =
                        AP4_DYNAMIC_CAST(AP4_AudioSampleDescription, track->m_SampleDescription);
                    if (audio) {
                        track->m_HandlerType = AP4_HANDLER_TYPE_SOUN;
                        if (track->m_MediaTimeScale == 0) {
                            track->m_MediaTimeScale = audio->GetSampleRate();
                        }
                    } else {
                        track->m_HandlerType = AP4_HANDLER_TYPE_VIDE;
                    }
                }
            }
        }

        if (track->m_HandlerType == AP4_HANDLER_TYPE_SOUN) {
            track->m_DefaultDuration = track->m_MediaTimeScale ?
                (1024*1024/track->m_MediaTimeScale) : 1024;
            if (track->m_MediaTimeScale == 0) track->m_MediaTimeScale = 48000;
            track->m_DefaultDuration = 1024;
        } else {
            track->m_DefaultDuration = 1001;
            if (track->m_MediaTimeScale == 0) track->m_MediaTimeScale = 30000;
        }

        bool samples_recovered =
            ParseSampleTable(scanner, track_start, track_end,
                             moov_data.GetData(), moov_data.GetDataSize(),
                             moov.m_Offset, file_size, *track);

        if (options.m_Verbose) {
            char name[5];
            fprintf(stderr,
                    "mp4repair: track handler=%s description=%s samples=%u\n",
                    TypeName(track->m_HandlerType, name),
                    track->m_SampleDescription ? "yes" : "no",
                    (unsigned int)track->m_Samples.size());
        }

        if (samples_recovered && track->m_SampleDescription) {
            track->m_SampleDataStream = &stream;
            stream.AddReference();
            tracks.push_back(track);
        } else {
            delete track;
        }

        search = trak_marker+4;
    }

    return tracks.empty() ? AP4_ERROR_INVALID_FORMAT : AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   ReadWholeBox
---------------------------------------------------------------------*/
static bool
ReadWholeRegion(AP4_ByteStream& stream,
                AP4_UI64        offset,
                AP4_UI64        end,
                AP4_DataBuffer& data)
{
    if (end <= offset || end-offset > 512*1024*1024) return false;
    data.SetDataSize((AP4_Size)(end-offset));
    return AP4_SUCCEEDED(ReadStream(stream, offset,
                                    data.UseData(), data.GetDataSize()));
}

/*----------------------------------------------------------------------
|   ScanAnnexB
---------------------------------------------------------------------*/
static void
ScanAnnexB(const AP4_UI08*  data,
           AP4_Size         data_size,
           std::vector<AP4_UI64>& nal_offsets,
           std::vector<AP4_UI32>& nal_sizes)
{
    nal_offsets.clear();
    nal_sizes.clear();
    AP4_Size i = 0;
    while (i+3 < data_size) {
        AP4_Size start_code_size = 0;
        if (i+4 <= data_size &&
            data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1) {
            start_code_size = 4;
        } else if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 1) {
            start_code_size = 3;
        }
        if (start_code_size == 0) {
            i++;
            continue;
        }
        AP4_Size nal_start = i+start_code_size;
        AP4_Size j = nal_start;
        while (j+3 < data_size) {
            if ((data[j] == 0 && data[j+1] == 0 && data[j+2] == 1) ||
                (j+4 <= data_size &&
                 data[j] == 0 && data[j+1] == 0 && data[j+2] == 0 && data[j+3] == 1)) {
                break;
            }
            j++;
        }
        AP4_Size nal_end = (j+3 < data_size) ? j : data_size;
        while (nal_end > nal_start && data[nal_end-1] == 0) nal_end--;
        if (nal_end > nal_start) {
            nal_offsets.push_back(nal_start);
            nal_sizes.push_back(nal_end-nal_start);
        }
        i = (j+3 < data_size) ? j : data_size;
    }
}

/*----------------------------------------------------------------------
|   RecoverAnnexBAvcTrack
---------------------------------------------------------------------*/
static TrackRecovery*
RecoverAnnexBAvcTrack(AP4_ByteStream& stream,
                      const std::vector<TopLevelBox>& mdats,
                      bool verbose)
{
    AP4_DataBuffer media;
    for (unsigned int i=0; i<mdats.size(); i++) {
        AP4_DataBuffer part;
        if (!ReadWholeRegion(stream, mdats[i].m_Payload, mdats[i].m_End, part)) {
            continue;
        }
        AP4_Size old_size = media.GetDataSize();
        media.SetDataSize(old_size+part.GetDataSize());
        memcpy(media.UseData()+old_size, part.GetData(), part.GetDataSize());
    }
    if (media.GetDataSize() == 0) return NULL;

    std::vector<AP4_UI64> nal_offsets;
    std::vector<AP4_UI32> nal_sizes;
    ScanAnnexB(media.GetData(), media.GetDataSize(), nal_offsets, nal_sizes);
    if (nal_offsets.empty()) return NULL;

    // A length-prefixed (AVCC) mdat can contain accidental 00 00 01 byte
    // sequences inside compressed data. Require the elementary stream to
    // start like Annex B before trusting any of the discovered NAL units.
    AP4_UI64 first_nal_payload = nal_offsets[0];
    if (first_nal_payload > 32) return NULL;

    AP4_AvcFrameParser parser;
    bool have_sps = false;
    unsigned int width = 0;
    unsigned int height = 0;
    AP4_UI08 profile_idc = 0;
    AP4_UI08 level_idc = 0;
    AP4_UI08 profile_compatibility = 0;
    AP4_UI08 chroma_format_idc = 0;
    AP4_UI08 bit_depth_luma_minus8 = 0;
    AP4_UI08 bit_depth_chroma_minus8 = 0;
    std::vector<AP4_UI64> sps_offsets;
    std::vector<AP4_UI32> sps_sizes;
    std::vector<AP4_UI64> pps_offsets;
    std::vector<AP4_UI32> pps_sizes;
    for (unsigned int i=0; i<nal_offsets.size(); i++) {
        const AP4_UI08* nal = media.GetData()+nal_offsets[i];
        AP4_UI08 nal_type = nal[0] & 0x1F;
        if (nal_type == 7) {
            AP4_AvcSequenceParameterSet candidate;
            if (AP4_SUCCEEDED(parser.ParseSPS(nal, nal_sizes[i], candidate))) {
                have_sps = true;
                candidate.GetInfo(width, height);
                profile_idc = candidate.profile_idc;
                level_idc = candidate.level_idc;
                profile_compatibility =
                    candidate.constraint_set0_flag<<7 |
                    candidate.constraint_set1_flag<<6 |
                    candidate.constraint_set2_flag<<5 |
                    candidate.constraint_set3_flag<<4;
                chroma_format_idc = candidate.chroma_format_idc;
                bit_depth_luma_minus8 = candidate.bit_depth_luma_minus8;
                bit_depth_chroma_minus8 = candidate.bit_depth_chroma_minus8;
                sps_offsets.push_back(nal_offsets[i]);
                sps_sizes.push_back(nal_sizes[i]);
            }
        } else if (nal_type == 8) {
            pps_offsets.push_back(nal_offsets[i]);
            pps_sizes.push_back(nal_sizes[i]);
        }
    }
    if (!have_sps || pps_offsets.empty()) return NULL;

    // Convert Annex B access units to length-prefixed samples in one memory
    // stream. The sample table can then reference contiguous synthesized data.
    AP4_MemoryByteStream* sample_stream = new AP4_MemoryByteStream();
    std::vector<SampleRecord> samples;
    bool in_picture = false;
    bool current_is_idr = false;
    AP4_UI64 sample_offset = 0;
    AP4_UI32 sample_size = 0;
    bool have_picture = false;

    for (unsigned int i=0; i<nal_offsets.size(); i++) {
        AP4_UI08 nal_type = media.GetData()[nal_offsets[i]] & 0x1F;
        bool is_vcl = (nal_type == 1 || nal_type == 2 || nal_type == 5);
        bool starts_picture = false;
        if (nal_type == 9) {
            starts_picture = in_picture || have_picture;
        } else if (is_vcl) {
            starts_picture = in_picture;
        }

        if (starts_picture && have_picture) {
            SampleRecord sample;
            sample.m_Offset   = sample_offset;
            sample.m_Size     = sample_size;
            sample.m_Dts      = samples.size()*1001;
            sample.m_Duration = 1001;
            sample.m_CtsDelta = 0;
            sample.m_IsSync   = current_is_idr;
            samples.push_back(sample);
            sample_offset += sample_size;
            sample_size = 0;
            have_picture = false;
        }

        if (nal_type == 9) {
            in_picture = false;
            current_is_idr = false;
        }

        AP4_UI32 prefix = nal_sizes[i];
        sample_stream->WriteUI32(prefix);
        sample_stream->Write(media.GetData()+nal_offsets[i], nal_sizes[i]);
        sample_size += 4+nal_sizes[i];
        if (is_vcl) {
            in_picture = true;
            have_picture = true;
            if (nal_type == 5) current_is_idr = true;
        }
    }
    if (have_picture && sample_size) {
        SampleRecord sample;
        sample.m_Offset   = sample_offset;
        sample.m_Size     = sample_size;
        sample.m_Dts      = samples.size()*1001;
        sample.m_Duration = 1001;
        sample.m_CtsDelta = 0;
        sample.m_IsSync   = current_is_idr;
        samples.push_back(sample);
    }
    if (samples.empty()) {
        sample_stream->Release();
        return NULL;
    }

    AP4_Array<AP4_DataBuffer> sps_array;
    AP4_Array<AP4_DataBuffer> pps_array;
    for (unsigned int i=0; i<sps_offsets.size(); i++) {
        AP4_DataBuffer parameter_set(media.GetData()+sps_offsets[i], sps_sizes[i]);
        sps_array.Append(parameter_set);
    }
    for (unsigned int i=0; i<pps_offsets.size(); i++) {
        AP4_DataBuffer parameter_set(media.GetData()+pps_offsets[i], pps_sizes[i]);
        pps_array.Append(parameter_set);
    }

    TrackRecovery* track = new TrackRecovery;
    track->m_HandlerType = AP4_HANDLER_TYPE_VIDE;
    track->m_MediaTimeScale = 30000;
    track->m_MediaDuration = (AP4_UI64)samples.size()*1001;
    track->m_Width = width << 16;
    track->m_Height = height << 16;
    track->m_DefaultDuration = 1001;
    track->m_SampleDescription = new AP4_AvcSampleDescription(
        AP4_SAMPLE_FORMAT_AVC1,
        width,
        height,
        24,
        "AVC Coding",
        profile_idc,
        level_idc,
        profile_compatibility,
        4,
        chroma_format_idc,
        bit_depth_luma_minus8,
        bit_depth_chroma_minus8,
        sps_array,
        pps_array);
    track->m_Samples.swap(samples);
    track->m_SampleDataStream = sample_stream;

    if (verbose) {
        fprintf(stderr,
                "mp4repair: rebuilt Annex B AVC track (%ux%u, %u samples)\n",
                width, height, (unsigned int)track->m_Samples.size());
    }
    return track;
}

/*----------------------------------------------------------------------
|   RecoverAdtsTrack
---------------------------------------------------------------------*/
static TrackRecovery*
RecoverAdtsTrack(AP4_ByteStream& stream,
                 const std::vector<TopLevelBox>& mdats,
                 bool verbose)
{
    AP4_DataBuffer media;
    for (unsigned int i=0; i<mdats.size(); i++) {
        AP4_DataBuffer part;
        if (!ReadWholeRegion(stream, mdats[i].m_Payload, mdats[i].m_End, part)) {
            continue;
        }
        AP4_Size old_size = media.GetDataSize();
        media.SetDataSize(old_size+part.GetDataSize());
        memcpy(media.UseData()+old_size, part.GetData(), part.GetDataSize());
    }

    AP4_Size cursor = 0;
    AP4_UI32 sample_rate = 0;
    AP4_UI08 channel_config = 0;
    AP4_UI08 audio_object_type = 0;
    AP4_UI08 sampling_frequency_index = 0;
    AP4_MemoryByteStream* sample_stream = new AP4_MemoryByteStream();
    std::vector<SampleRecord> samples;
    AP4_UI64 output_offset = 0;
    AP4_UI64 first_frame_offset = 0;
    AP4_UI64 frame_bytes = 0;

    while (cursor+7 <= media.GetDataSize()) {
        const AP4_UI08* header = media.GetData()+cursor;
        if (header[0] != 0xFF || (header[1] & 0xF6) != 0xF0) {
            cursor++;
            continue;
        }
        AP4_AdtsHeader adts(header);
        if (AP4_FAILED(adts.Check()) ||
            adts.m_FrameLength < 7 ||
            cursor+adts.m_FrameLength > media.GetDataSize()) {
            cursor++;
            continue;
        }

        if (sample_rate == 0) {
            sample_rate = AP4_AdtsSamplingFrequencyTable[adts.m_SamplingFrequencyIndex];
            channel_config = (AP4_UI08)adts.m_ChannelConfiguration;
            audio_object_type = (AP4_UI08)(adts.m_ProfileObjectType+1);
            sampling_frequency_index = (AP4_UI08)adts.m_SamplingFrequencyIndex;
            first_frame_offset = cursor;
        }

        AP4_Size payload_size = adts.m_FrameLength-7;
        sample_stream->Write(header+7, payload_size);

        SampleRecord sample;
        sample.m_Offset   = output_offset;
        sample.m_Size     = payload_size;
        sample.m_Dts      = (AP4_UI64)samples.size()*1024;
        sample.m_Duration = 1024;
        sample.m_CtsDelta = 0;
        sample.m_IsSync   = true;
        samples.push_back(sample);
        output_offset += payload_size;
        frame_bytes += adts.m_FrameLength;
        cursor += adts.m_FrameLength;
    }

    if (samples.size() < 2 ||
        sample_rate == 0 ||
        first_frame_offset > 4096 ||
        frame_bytes*2 < media.GetDataSize()) {
        sample_stream->Release();
        return NULL;
    }

    AP4_DataBuffer dsi;
    AP4_UI08 dsi_bytes[2];
    dsi_bytes[0] = (AP4_UI08)((audio_object_type << 3) |
                              ((sampling_frequency_index & 0x0E) >> 1));
    dsi_bytes[1] = (AP4_UI08)(((sampling_frequency_index & 1) << 7) |
                              ((channel_config & 0x0F) << 3));
    dsi.SetData(dsi_bytes, 2);

    TrackRecovery* track = new TrackRecovery;
    track->m_HandlerType = AP4_HANDLER_TYPE_SOUN;
    track->m_MediaTimeScale = sample_rate;
    track->m_MediaDuration = (AP4_UI64)samples.size()*1024;
    track->m_DefaultDuration = 1024;
    track->m_SampleDescription = new AP4_MpegAudioSampleDescription(
        AP4_OTI_MPEG4_AUDIO,
        sample_rate,
        16,
        channel_config,
        &dsi,
        6144,
        128000,
        128000);
    track->m_Samples.swap(samples);
    track->m_SampleDataStream = sample_stream;

    if (verbose) {
        fprintf(stderr,
                "mp4repair: rebuilt ADTS AAC track (%u Hz, %u samples)\n",
                sample_rate, (unsigned int)track->m_Samples.size());
    }
    return track;
}

/*----------------------------------------------------------------------
|   BuildOutput
---------------------------------------------------------------------*/
static AP4_Result
BuildOutput(AP4_ByteStream& input,
            AP4_LargeSize   file_size,
            const std::vector<TopLevelBox>& top_level,
            const std::vector<TrackRecovery*>& tracks,
            const RepairOptions& options,
            AP4_ByteStream& output)
{
    AP4_Movie* movie = new AP4_Movie(1000, 0);
    AP4_File*  file  = new AP4_File(movie);

    AP4_UI32 major_brand = AP4_FILE_BRAND_ISOM;
    AP4_UI32 minor_version = 0;
    std::vector<AP4_UI32> compatible_brands;
    for (unsigned int i=0; i<top_level.size(); i++) {
        if (top_level[i].m_Type != AP4_ATOM_TYPE_FTYP) continue;
        AP4_UI08 header[16];
        if (AP4_FAILED(ReadStream(input, top_level[i].m_Payload, header, 16))) break;
        major_brand = ReadUint32(header);
        minor_version = ReadUint32(header+4);
        AP4_UI64 brands_offset = top_level[i].m_Payload+8;
        AP4_UI64 brands_end = top_level[i].m_End;
        for (AP4_UI64 offset=brands_offset;
             offset+4<=brands_end;
             offset+=4) {
            AP4_UI08 brand[4];
            if (AP4_FAILED(ReadStream(input, offset, brand, 4))) break;
            compatible_brands.push_back(ReadUint32(brand));
        }
        break;
    }
    file->SetFileType(major_brand, minor_version,
                      compatible_brands.empty() ? NULL : &compatible_brands[0],
                      (AP4_Cardinal)compatible_brands.size());

    AP4_UI32 track_id = 1;
    for (unsigned int i=0; i<tracks.size(); i++) {
        TrackRecovery* recovery = tracks[i];
        if (recovery->m_SampleDescription == NULL || recovery->m_Samples.empty()) {
            continue;
        }

        AP4_SyntheticSampleTable* table = new AP4_SyntheticSampleTable();
        table->AddSampleDescription(recovery->m_SampleDescription, false);

        AP4_UI32 default_duration = recovery->m_DefaultDuration ?
                                    recovery->m_DefaultDuration : 1;
        for (unsigned int j=0; j<recovery->m_Samples.size(); j++) {
            SampleRecord& sample = recovery->m_Samples[j];
            AP4_UI32 duration = sample.m_Duration ? sample.m_Duration : default_duration;
            AP4_Result result = table->AddSample(*recovery->m_SampleDataStream,
                                                 sample.m_Offset,
                                                 sample.m_Size,
                                                 duration,
                                                 0,
                                                 sample.m_Dts,
                                                 sample.m_CtsDelta,
                                                 sample.m_IsSync);
            if (AP4_FAILED(result)) {
                // A damaged stts can produce a non-contiguous DTS. Rebuild
                // the timing from the durations recovered so far.
                AP4_UI64 dts = 0;
                for (unsigned int k=0; k<j; k++) {
                    dts += recovery->m_Samples[k].m_Duration ?
                           recovery->m_Samples[k].m_Duration : default_duration;
                }
                sample.m_Dts = dts;
                result = table->AddSample(*recovery->m_SampleDataStream,
                                          sample.m_Offset,
                                          sample.m_Size,
                                          duration,
                                          0,
                                          sample.m_Dts,
                                          sample.m_CtsDelta,
                                          sample.m_IsSync);
                if (AP4_FAILED(result)) {
                    delete table;
                    delete file;
                    return result;
                }
            }
        }

        bool audio = recovery->m_HandlerType == AP4_HANDLER_TYPE_SOUN;
        AP4_Track::Type type = audio ? AP4_Track::TYPE_AUDIO : AP4_Track::TYPE_VIDEO;
        AP4_UI64 media_duration = 0;
        for (unsigned int j=0; j<recovery->m_Samples.size(); j++) {
            media_duration += recovery->m_Samples[j].m_Duration ?
                              recovery->m_Samples[j].m_Duration : default_duration;
        }
        AP4_UI32 media_timescale = recovery->m_MediaTimeScale ?
                                   recovery->m_MediaTimeScale : 1;
        AP4_UI64 movie_duration = AP4_ConvertTime(media_duration,
                                                  media_timescale,
                                                  1000);
        AP4_Track* track = new AP4_Track(type,
                                         table,
                                         track_id++,
                                         1000,
                                         movie_duration,
                                         media_timescale,
                                         media_duration,
                                         "und",
                                         recovery->m_Width,
                                         recovery->m_Height);
        movie->AddTrack(track);

        if (options.m_Verbose) {
            fprintf(stderr,
                    "mp4repair: wrote track %u with %u samples, duration=%llu ms\n",
                    (unsigned int)track->GetId(),
                    (unsigned int)recovery->m_Samples.size(),
                    (unsigned long long)movie_duration);
        }
    }

    if (movie->GetTracks().ItemCount() == 0) {
        delete file;
        return AP4_ERROR_INVALID_FORMAT;
    }

    AP4_Result result = AP4_FileWriter::Write(*file, output);
    delete file;
    return result;
}

/*----------------------------------------------------------------------
|   main
---------------------------------------------------------------------*/
int
main(int argc, char** argv)
{
    RepairOptions options;
    const char* input_filename = NULL;
    const char* output_filename = NULL;

    for (int i=1; i<argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) {
            options.m_Verbose = true;
        } else if (argv[i][0] == '-') {
            PrintUsageAndExit();
        } else if (input_filename == NULL) {
            input_filename = argv[i];
        } else if (output_filename == NULL) {
            output_filename = argv[i];
        } else {
            PrintUsageAndExit();
        }
    }
    if (input_filename == NULL || output_filename == NULL) PrintUsageAndExit();
    if (strcmp(input_filename, output_filename) == 0) {
        fprintf(stderr, "ERROR: input and output must be different files\n");
        return 1;
    }

    AP4_ByteStream* input = NULL;
    AP4_Result result = AP4_FileByteStream::Create(input_filename,
                                                   AP4_FileByteStream::STREAM_MODE_READ,
                                                   input);
    if (AP4_FAILED(result)) {
        fprintf(stderr, "ERROR: cannot open input file (%s)\n", input_filename);
        return 1;
    }

    AP4_LargeSize file_size = 0;
    result = input->GetSize(file_size);
    if (AP4_FAILED(result) || file_size < 8) {
        fprintf(stderr, "ERROR: cannot determine input size\n");
        input->Release();
        return 1;
    }

    std::vector<TopLevelBox> top_level;
    std::vector<TopLevelBox> mdats;
    TopLevelBox moov;
    bool have_moov = false;
    bool has_fragments = false;
    memset(&moov, 0, sizeof(moov));

    result = ReadTopLevelBoxes(*input, file_size, top_level);
    if (AP4_FAILED(result)) {
        fprintf(stderr, "ERROR: no MP4 atoms could be recovered\n");
        input->Release();
        return 1;
    }

    if (options.m_Verbose) {
        for (unsigned int i=0; i<top_level.size(); i++) {
            char name[5];
            fprintf(stderr,
                    "mp4repair: top-level %s at %llu..%llu\n",
                    TypeName(top_level[i].m_Type, name),
                    (unsigned long long)top_level[i].m_Offset,
                    (unsigned long long)top_level[i].m_End);
        }
    }

    for (unsigned int i=0; i<top_level.size(); i++) {
        if (top_level[i].m_Type == AP4_ATOM_TYPE_MOOV) {
            if (!have_moov || top_level[i].m_End > moov.m_End) {
                moov = top_level[i];
                have_moov = true;
            }
        } else if (top_level[i].m_Type == AP4_ATOM_TYPE_MDAT) {
            mdats.push_back(top_level[i]);
        } else if (top_level[i].m_Type == AP4_ATOM_TYPE_MOOF) {
            has_fragments = true;
        }
    }

    std::vector<TrackRecovery*> tracks;
    if (!have_moov && InferMoovRegion(*input, file_size, moov)) {
        have_moov = true;
    }
    if (have_moov) {
        result = RecoverTracks(*input, file_size, moov, options, tracks);
    }
    if (tracks.empty()) {
        TopLevelBox inferred_moov;
        if (InferMoovRegion(*input, file_size, inferred_moov) &&
            (!have_moov ||
             inferred_moov.m_Offset != moov.m_Offset ||
             inferred_moov.m_End != moov.m_End)) {
            moov = inferred_moov;
            result = RecoverTracks(*input, file_size, moov, options, tracks);
        }
    }

    if (tracks.empty() && !has_fragments) {
        TrackRecovery* track = RecoverAnnexBAvcTrack(*input, mdats, options.m_Verbose);
        if (track) tracks.push_back(track);
    }
    if (tracks.empty() && !has_fragments) {
        TrackRecovery* track = RecoverAdtsTrack(*input, mdats, options.m_Verbose);
        if (track) tracks.push_back(track);
    }

    if (tracks.empty()) {
        fprintf(stderr,
                "ERROR: unable to recover a sample description and sample table\n");
        input->Release();
        return 1;
    }

    AP4_ByteStream* output = NULL;
    result = AP4_FileByteStream::Create(output_filename,
                                        AP4_FileByteStream::STREAM_MODE_WRITE,
                                        output);
    if (AP4_FAILED(result)) {
        fprintf(stderr, "ERROR: cannot open output file (%s)\n", output_filename);
        for (unsigned int i=0; i<tracks.size(); i++) delete tracks[i];
        input->Release();
        return 1;
    }

    result = BuildOutput(*input, file_size, top_level, tracks, options, *output);
    output->Release();
    for (unsigned int i=0; i<tracks.size(); i++) delete tracks[i];
    input->Release();

    if (AP4_FAILED(result)) {
        remove(output_filename);
        fprintf(stderr, "ERROR: failed to write repaired file (%d)\n", result);
        return 1;
    }

    if (options.m_Verbose) {
        fprintf(stderr, "mp4repair: repaired file written to %s\n", output_filename);
    }
    return 0;
}
