#pragma once

#include <library/cpp/digest/crc32c/crc32c.h>

#include <util/generic/deque.h>
#include <util/generic/hash.h>
#include <util/generic/strbuf.h>
#include <util/generic/string.h>
#include <util/generic/vector.h>
#include <util/stream/mem.h>
#include <util/system/file.h>
#include <util/system/filemap.h>
#include <util/system/yassert.h>

#include <cstddef>
#include <cstring>
#include <optional>

namespace NCloud {

////////////////////////////////////////////////////////////////////////////////

template <typename H>
class TPersistentDynamicTable
{
public:
    struct THeader;
    struct TRecordDescriptor;

private:
    enum class EDataRetrievalMode
    {
        NoValidation = 0,
        WithValidation,
    };

    TString FileName;
    ui64 MaxRecords = 0;
    ui64 NextFreeRecordIndex = 0;
    ui64 GapSpaceSize = 0;
    ui64 InitialDataAreaSize = 0;
    ui64 DataAreaOffset = 0;
    ui64 DataAreaSize = 0;
    ui64 NextDataOffset = 0;
    ui64 HeadDataIndex = InvalidIndex;
    ui64 TailDataIndex = InvalidIndex;

    std::unique_ptr<TFileMap> FileMap;
    TDeque<ui64> FreeRecordIndexes;

    THeader* HeaderPtr = nullptr;
    TRecordDescriptor* DescriptorsPtr = nullptr;
    char* DataPtr = nullptr;
    ui8 GapSpacePercentageCompactionThreshold = 30;

public:
    static constexpr ui64 InvalidIndex = -1;
    static constexpr ui32 Version = 1;

    struct THeader
    {
        ui32 Version = 0;
        ui64 HeaderSize = 0;
        ui64 RecordDescriptorSize = 0;
        ui64 DataAreaOffset = 0;
        ui64 DataAreaSize = 0;
        ui64 NextDataOffset = 0;
        ui64 NextFreeRecordIndex = 0;
        ui64 MaxRecords = 0;

        // indexes used during compaction
        ui64 CompactedRecordSrcIndex = InvalidIndex;
        ui64 CompactedRecordDstIndex = InvalidIndex;

        H Data;
    };

    enum class ERecordState
    {
        Free = 0,
        Allocated,
        Stored,
    };

    struct TRecordDescriptor
    {
        ui64 DataOffset = 0;
        ui64 DataSize = 0;
        ui32 CRC32 = 0;
        ui64 PrevDataIndex = InvalidIndex;
        ui64 NextDataIndex = InvalidIndex;
        ERecordState State = ERecordState::Free;
    };

    class TIterator
    {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = TStringBuf;
        using difference_type = std::ptrdiff_t;
        using pointer = TStringBuf*;
        using reference = TStringBuf&;

        TIterator(TPersistentDynamicTable& table, ui64 index)
            : Table(table)
            , Index(index)
        {
            SkipEmptyRecords();
        }

        bool operator==(const TIterator& other) const
        {
            return Index == other.Index;
        }

        bool operator!=(const TIterator& other) const
        {
            return !(*this == other);
        }

        TIterator& operator++()
        {
            ++Index;
            SkipEmptyRecords();
            return *this;
        }

        TIterator operator++(int)
        {
            auto tmp = *this;
            ++*this;
            return tmp;
        }

        TStringBuf operator*()
        {
            return Table.GetRecordWithValidation(Index);
        }

        TStringBuf operator->()
        {
            return Table.GetRecordWithValidation(Index);
        }

        ui64 GetIndex() const
        {
            return Index;
        }

        ui64 GetDataSize() const
        {
            if (Index >= Table.MaxRecords) {
                return 0;
            }
            return Table.DescriptorsPtr[Index].DataSize;
        }

    private:
        void SkipEmptyRecords()
        {
            while (Index < Table.MaxRecords &&
                   Table.DescriptorsPtr[Index].State != ERecordState::Stored)
            {
                ++Index;
            }
        }

    private:
        TPersistentDynamicTable& Table;
        ui64 Index;
    };

    TIterator begin()
    {
        return TIterator(*this, 0);
    }

    TIterator end()
    {
        return TIterator(*this, MaxRecords);
    }

public:
    TPersistentDynamicTable(
        const TString& fileName,
        ui64 maxRecords,
        ui64 initialDataAreaSize = 1024 * 1024,
        ui8 gapSpacePercentageCompactionThreshold = 30)
        : FileName(fileName)
        , MaxRecords(maxRecords)
        , InitialDataAreaSize(initialDataAreaSize)
        , DataAreaSize(initialDataAreaSize)
        , GapSpacePercentageCompactionThreshold(
              gapSpacePercentageCompactionThreshold)
    {
        Init();
    }

    H* HeaderData()
    {
        return &HeaderPtr->Data;
    }

    TStringBuf GetRecord(ui64 index)
    {
        return GetRecord(index, EDataRetrievalMode::NoValidation);
    }

    TStringBuf GetRecordWithValidation(ui64 index)
    {
        return GetRecord(index, EDataRetrievalMode::WithValidation);
    }

    ui64 AllocRecord(ui64 dataSize)
    {
        ui64 index = InvalidIndex;
        if (dataSize == 0) {
            return InvalidIndex;
        }
        if (!FreeRecordIndexes.empty()) {
            index = FreeRecordIndexes.front();
            FreeRecordIndexes.pop_front();
        } else if (NextFreeRecordIndex < MaxRecords) {
            index = NextFreeRecordIndex++;
        }

        if (index == InvalidIndex) {
            return InvalidIndex;
        }

        if (NextDataOffset + dataSize > DataAreaSize) {
            TryCompactDataArea(dataSize);
            if (NextDataOffset + dataSize > DataAreaSize) {
                ExpandDataArea(dataSize);
            }
        }

        DescriptorsPtr[index].DataOffset = NextDataOffset;
        DescriptorsPtr[index].DataSize = dataSize;
        DescriptorsPtr[index].State = ERecordState::Allocated;
        AddToDataList(index);

        NextDataOffset += dataSize;

        HeaderPtr->NextFreeRecordIndex = NextFreeRecordIndex;
        HeaderPtr->NextDataOffset = NextDataOffset;

        return index;
    }

    bool CommitRecord(ui64 index)
    {
        if (index >= MaxRecords ||
            DescriptorsPtr[index].State != ERecordState::Allocated)
        {
            return false;
        }

        DescriptorsPtr[index].State = ERecordState::Stored;

        return true;
    }

    bool DeleteRecord(ui64 index)
    {
        if (index >= MaxRecords ||
            DescriptorsPtr[index].State != ERecordState::Stored)
        {
            return false;
        }

        RemoveFromDataList(index);

        DescriptorsPtr[index].State = ERecordState::Free;
        GapSpaceSize += DescriptorsPtr[index].DataSize;

        if (index + 1 == NextFreeRecordIndex) {
            NextFreeRecordIndex--;
            HeaderPtr->NextFreeRecordIndex = NextFreeRecordIndex;
        } else {
            FreeRecordIndexes.push_back(index);
        }
        return true;
    }

    ui64 CountRecords()
    {
        return NextFreeRecordIndex - FreeRecordIndexes.size();
    }

    void Clear()
    {
        NextFreeRecordIndex = 0;
        NextDataOffset = 0;
        GapSpaceSize = 0;
        HeadDataIndex = InvalidIndex;
        TailDataIndex = InvalidIndex;
        FreeRecordIndexes.clear();

        FileMap->ResizeAndRemap(0, 0);
        FileMap.reset();
        Init();
    }

    TMemoryOutput GetRecordData(ui64 index)
    {
        if (index >= MaxRecords ||
            DescriptorsPtr[index].State != ERecordState::Allocated)
        {
            return TMemoryOutput(nullptr, 0);
        }

        return TMemoryOutput(
            DataPtr + DescriptorsPtr[index].DataOffset,
            DescriptorsPtr[index].DataSize);
    }

    bool WriteRecordData(ui64 index, const void* data, ui64 size)
    {
        if (index >= MaxRecords || size > DescriptorsPtr[index].DataSize ||
            size == 0 || data == nullptr ||
            DescriptorsPtr[index].State == ERecordState::Free)
        {
            return false;
        }

        char* recordData = DataPtr + DescriptorsPtr[index].DataOffset;
        std::memcpy(recordData, data, size);
        if (size < DescriptorsPtr[index].DataSize) {
#ifdef NDEBUG
#else
            std::memset(
                recordData + size,
                0,
                DescriptorsPtr[index].DataSize - size);
#endif
            GapSpaceSize += DescriptorsPtr[index].DataSize - size;
            DescriptorsPtr[index].DataSize = size;
        }

        DescriptorsPtr[index].CRC32 =
            Crc32c(recordData, DescriptorsPtr[index].DataSize);

        return true;
    }

private:
    void AddToDataList(ui64 index)
    {
        DescriptorsPtr[index].PrevDataIndex = InvalidIndex;
        DescriptorsPtr[index].NextDataIndex = InvalidIndex;

        if (HeadDataIndex == InvalidIndex) {
            HeadDataIndex = index;
            TailDataIndex = index;
        } else {
            DescriptorsPtr[TailDataIndex].NextDataIndex = index;
            DescriptorsPtr[index].PrevDataIndex = TailDataIndex;
            TailDataIndex = index;
        }
    }

    void RemoveFromDataList(ui64 index)
    {
        ui64 prevIndex = DescriptorsPtr[index].PrevDataIndex;
        ui64 nextIndex = DescriptorsPtr[index].NextDataIndex;

        if (prevIndex != InvalidIndex) {
            DescriptorsPtr[prevIndex].NextDataIndex = nextIndex;
        } else {
            HeadDataIndex = nextIndex;
        }

        if (nextIndex != InvalidIndex) {
            DescriptorsPtr[nextIndex].PrevDataIndex = prevIndex;
        } else {
            TailDataIndex = prevIndex;
        }

        DescriptorsPtr[index].PrevDataIndex = InvalidIndex;
        DescriptorsPtr[index].NextDataIndex = InvalidIndex;
    }

    void Init()
    {
        TFile file(FileName, OpenAlways | WrOnly);
        if (file.GetLength() == 0) {
            file.Resize(sizeof(THeader));
        }
        file.Close();

        FileMap = std::make_unique<TFileMap>(FileName, TMemoryMapCommon::oRdWr);
        FileMap->Map(0, sizeof(THeader));

        auto* header = reinterpret_cast<THeader*>(FileMap->Ptr());
        if (header->MaxRecords == 0) {
            header->Version = Version;
            header->HeaderSize = sizeof(THeader);
            header->RecordDescriptorSize = sizeof(TRecordDescriptor);
            header->MaxRecords = MaxRecords;
            header->NextFreeRecordIndex = 0;
            header->DataAreaOffset =
                sizeof(THeader) + MaxRecords * sizeof(TRecordDescriptor);
            header->DataAreaSize = DataAreaSize;
            header->NextDataOffset = 0;
            header->CompactedRecordSrcIndex = InvalidIndex;
            header->CompactedRecordDstIndex = InvalidIndex;
        }

        Y_ABORT_UNLESS(
            header->Version == Version,
            "Invalid header version %d",
            header->Version);
        Y_ABORT_UNLESS(
            header->HeaderSize == sizeof(THeader),
            "Invalid header size %lu != %lu",
            header->HeaderSize,
            sizeof(THeader));
        Y_ABORT_UNLESS(
            header->RecordDescriptorSize == sizeof(TRecordDescriptor),
            "Invalid record descriptor size %lu != %lu",
            header->RecordDescriptorSize,
            sizeof(TRecordDescriptor));

        MaxRecords = header->MaxRecords;
        NextFreeRecordIndex = header->NextFreeRecordIndex;
        DataAreaOffset = header->DataAreaOffset;
        DataAreaSize = header->DataAreaSize;
        NextDataOffset = header->NextDataOffset;

        ResizeAndRemap();

        CompactRecords();
        CompactDataArea(EDataRetrievalMode::WithValidation);
    }

    ui64 CalcFileSize(ui64 dataAreaSize)
    {
        return sizeof(THeader) + MaxRecords * sizeof(TRecordDescriptor) +
               dataAreaSize;
    }

    void CompactRecords()
    {
        ui64 writeRecordIndex = 0;
        ui64 readRecordIndex = 0;

        FinishCompactRecord();

        while (readRecordIndex < NextFreeRecordIndex) {
            if (DescriptorsPtr[readRecordIndex].State != ERecordState::Stored) {
                if (DescriptorsPtr[readRecordIndex].State ==
                    ERecordState::Allocated)
                {
                    RemoveFromDataList(readRecordIndex);
                }
                DescriptorsPtr[readRecordIndex++].State = ERecordState::Free;
                continue;
            }

            if (writeRecordIndex != readRecordIndex) {
                PrepareCompactRecord(readRecordIndex, writeRecordIndex);
                FinishCompactRecord();
            }
            if (DescriptorsPtr[writeRecordIndex].PrevDataIndex == InvalidIndex)
            {
                HeadDataIndex = writeRecordIndex;
            }
            if (DescriptorsPtr[writeRecordIndex].NextDataIndex == InvalidIndex)
            {
                TailDataIndex = writeRecordIndex;
            }

            ++writeRecordIndex;
            ++readRecordIndex;
        }

        NextFreeRecordIndex = writeRecordIndex;
        HeaderPtr->NextFreeRecordIndex = NextFreeRecordIndex;
    }

    void PrepareCompactRecord(ui64 srcIndex, ui64 dstIndex)
    {
        // set compacted indexes atomically (64bit memory assignment)
        // if we restart during the copy we can retry the copy until we succeed
        // and set at least one of the indexes to InvalidIndex value
        HeaderPtr->CompactedRecordSrcIndex = srcIndex;
        HeaderPtr->CompactedRecordDstIndex = dstIndex;
    }

    void FinishCompactRecord()
    {
        if (HeaderPtr->CompactedRecordSrcIndex == InvalidIndex ||
            HeaderPtr->CompactedRecordDstIndex == InvalidIndex)
        {
            // Prepare phase didn't finish and none of the records were moved
            HeaderPtr->CompactedRecordSrcIndex = InvalidIndex;
            HeaderPtr->CompactedRecordDstIndex = InvalidIndex;
            return;
        }

        Y_ABORT_UNLESS(
            HeaderPtr->CompactedRecordSrcIndex < MaxRecords,
            "Invalid CompactedRecordSrcIndex %lu",
            HeaderPtr->CompactedRecordSrcIndex);
        Y_ABORT_UNLESS(
            HeaderPtr->CompactedRecordDstIndex < MaxRecords,
            "Invalid CompactedRecordDstIndex %lu",
            HeaderPtr->CompactedRecordDstIndex);

        std::memcpy(
            &DescriptorsPtr[HeaderPtr->CompactedRecordDstIndex],
            &DescriptorsPtr[HeaderPtr->CompactedRecordSrcIndex],
            sizeof(TRecordDescriptor));

        DescriptorsPtr[HeaderPtr->CompactedRecordSrcIndex].State =
            ERecordState::Free;
        DescriptorsPtr[HeaderPtr->CompactedRecordDstIndex].State =
            ERecordState::Stored;

        ui64 prev =
            DescriptorsPtr[HeaderPtr->CompactedRecordDstIndex].PrevDataIndex;
        ui64 next =
            DescriptorsPtr[HeaderPtr->CompactedRecordDstIndex].NextDataIndex;

        if (prev != InvalidIndex) {
            DescriptorsPtr[prev].NextDataIndex =
                HeaderPtr->CompactedRecordDstIndex;
        }
        if (next != InvalidIndex) {
            DescriptorsPtr[next].PrevDataIndex =
                HeaderPtr->CompactedRecordDstIndex;
        }

        HeaderPtr->CompactedRecordSrcIndex = InvalidIndex;
        HeaderPtr->CompactedRecordDstIndex = InvalidIndex;
    }

    void TryCompactDataArea(ui64 requiredSize)
    {
        uint64_t threshold = (InitialDataAreaSize / 100) *
                                 GapSpacePercentageCompactionThreshold +
                             ((InitialDataAreaSize % 100) *
                              GapSpacePercentageCompactionThreshold) /
                                 100;
        if (GapSpaceSize > threshold && GapSpaceSize >= requiredSize) {
            CompactDataArea(EDataRetrievalMode::NoValidation);
        }
    }

    void CompactDataArea(EDataRetrievalMode mode)
    {
        ui64 newOffset = 0;
        ui64 currentIndex = HeadDataIndex;
        while (currentIndex != InvalidIndex) {
            Y_ABORT_UNLESS(
                currentIndex < NextFreeRecordIndex,
                "Invalid data index %lu in linked list",
                currentIndex);
            Y_ABORT_UNLESS(
                DescriptorsPtr[currentIndex].State != ERecordState::Free,
                "Non-stored record %lu found in data linked list",
                currentIndex);

            ui64 oldOffset = DescriptorsPtr[currentIndex].DataOffset;
            ui64 dataSize = DescriptorsPtr[currentIndex].DataSize;
            ui64 nextIndex = DescriptorsPtr[currentIndex].NextDataIndex;

            if (mode == EDataRetrievalMode::WithValidation) {
                ui64 calculatedCRC = Crc32c(DataPtr + oldOffset, dataSize);
                if (calculatedCRC != DescriptorsPtr[currentIndex].CRC32) {
                    RemoveFromDataList(currentIndex);
                    DescriptorsPtr[currentIndex].State = ERecordState::Free;
                    FreeRecordIndexes.push_back(currentIndex);
                    currentIndex = nextIndex;
                    continue;
                }
            }

            if (oldOffset != newOffset) {
                std::memmove(
                    DataPtr + newOffset,
                    DataPtr + oldOffset,
                    dataSize);
                DescriptorsPtr[currentIndex].DataOffset = newOffset;
            }
            newOffset += dataSize;
            currentIndex = nextIndex;
        }

        NextDataOffset = newOffset;
        GapSpaceSize = 0;
        HeaderPtr->NextDataOffset = NextDataOffset;
    }

    void ExpandDataArea(ui64 requiredSize)
    {
        ui64 newDataAreaSize = DataAreaSize;
        while (NextDataOffset + requiredSize > newDataAreaSize) {
            newDataAreaSize *= 2;
        }

        DataAreaSize = newDataAreaSize;
        HeaderPtr->DataAreaSize = DataAreaSize;

        ResizeAndRemap();
    }

    void ResizeAndRemap()
    {
        FileMap->ResizeAndRemap(0, CalcFileSize(DataAreaSize));
        HeaderPtr = reinterpret_cast<THeader*>(FileMap->Ptr());
        DescriptorsPtr = reinterpret_cast<TRecordDescriptor*>(
            reinterpret_cast<char*>(HeaderPtr) + sizeof(THeader));
        DataPtr = reinterpret_cast<char*>(HeaderPtr) + DataAreaOffset;
    }

    TStringBuf GetRecord(ui64 index, EDataRetrievalMode mode)
    {
        if (index >= MaxRecords ||
            DescriptorsPtr[index].State != ERecordState::Stored)
        {
            return TStringBuf();
        }

        TStringBuf buf(
            DataPtr + DescriptorsPtr[index].DataOffset,
            DescriptorsPtr[index].DataSize);

        if (mode == EDataRetrievalMode::WithValidation) {
            ui32 calculatedCRC = Crc32c(buf.data(), buf.size());
            if (calculatedCRC != DescriptorsPtr[index].CRC32) {
                return TStringBuf();
            }
        }

        return buf;
    }
};

}   // namespace NCloud
